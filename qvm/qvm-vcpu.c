/*
 * QVM vCPUs: creation, the KVM_RUN loop, signals and interrupt injection.
 *
 * A QVM vCPU is an ordinary QEMU CPU that QEMU never schedules itself.
 * qvm_vcpu_run() drives cpu_exec() directly on whichever client thread issued
 * KVM_RUN, which is both what KVM does and what makes the exit path simple:
 * everything that has to be reported to the client is already on the stack of
 * the thread that will return it.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qemu/memalign.h"
#include "qemu/rcu.h"
#include "cpu.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/cputlb.h"
#include "exec/translation-block.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "system/cpus.h"
#include "tcg/startup.h"

#include "qvm-arch.h"
#include "qvm-internal.h"

#include <pthread.h>

/* Indexed by CPUState::cpu_index, for looking up the vCPU behind current_cpu. */
static QvmVcpu *qvm_vcpus[QVM_MAX_VCPUS];

/*
 * Whether this thread has been introduced to QEMU's RCU and TCG subsystems.
 * The thread that called qemu_init() is already an RCU reader; every other
 * client thread that runs a vCPU has to register itself.
 */
static __thread bool qvm_thread_rcu_registered;
static __thread bool qvm_thread_tcg_registered;

/* Set from a signal handler when a kick was delivered inside a KVM_RUN. */
static __thread volatile sig_atomic_t qvm_kick_received;

void qvm_thread_mark_rcu_registered(void)
{
    qvm_thread_rcu_registered = true;
}

QvmVcpu *qvm_vcpu_of(CPUState *cs)
{
    if (!cs || cs->cpu_index < 0 || cs->cpu_index >= QVM_MAX_VCPUS) {
        return NULL;
    }
    return qvm_vcpus[cs->cpu_index];
}

QvmVcpu *qvm_vcpu_current(void)
{
    return qvm_vcpu_of(current_cpu);
}

void qvm_vcpu_request_exit(QvmVcpu *vcpu)
{
    vcpu->exit_pending = true;
}

void qvm_vcpu_io_completed(QvmVcpu *vcpu)
{
    vcpu->completing_io = false;

    /*
     * The guest instruction the client was told about has now been
     * re-executed.  A kick that arrived while it was pending was held off
     * until exactly this point, the way KVM only looks for signals once it
     * has finished delivering pending I/O.
     */
    if (qvm_kick_received) {
        cpu_exit(vcpu->cs);
    }
}

/*
 * Installed as qvm_io_exit_hook: called from the x86 port I/O helpers and from
 * cputlb's failed-transaction path, once the access is complete and no
 * memory-subsystem locks are held.  Rewinding to the start of the instruction
 * gives the client KVM's restart semantics: the instruction is re-executed on
 * the next KVM_RUN, and the trap handlers complete it from the shared page
 * instead of trapping again.
 */
void qvm_io_exit(CPUState *cs, uintptr_t retaddr)
{
    QvmVcpu *vcpu = qvm_vcpu_of(cs);

    if (!vcpu || !vcpu->exit_pending) {
        return;
    }

    vcpu->exit_pending = false;
    cs->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit_restore(cs, retaddr);
}

/* ------------------------------------------------------------------ */
/* Kick signals                                                        */
/* ------------------------------------------------------------------ */

/*
 * A KVM client bounds a guest run by arranging for a signal to be delivered
 * only while the vCPU is inside KVM_RUN: it blocks the signal in its own
 * thread and hands KVM a mask with the signal unblocked (KVM_SET_SIGNAL_MASK).
 * gem5 drives its whole timing model this way.
 *
 * A signal on its own does not stop TCG, so QVM interposes its own handler on
 * exactly those signals and turns delivery into a cpu_exit().  Whatever the
 * client installed still runs afterwards.
 */
static QemuMutex qvm_signal_lock;
static struct sigaction qvm_saved_sa[NSIG];
static bool qvm_kick_installed[NSIG];

static void __attribute__((constructor)) qvm_signal_init(void)
{
    qemu_mutex_init(&qvm_signal_lock);
}

static void qvm_kick_handler(int sig, siginfo_t *info, void *uc)
{
    CPUState *cs = current_cpu;
    /*
     * Read without the lock: entries are published before the signal can first
     * be delivered, and never change afterwards.
     */
    const struct sigaction *chain = &qvm_saved_sa[sig];

    qvm_kick_received = 1;
    if (cs) {
        /*
         * Open-coded cpu_exit(): it broadcasts a condition variable, which is
         * not async-signal-safe.  These two stores are all a running vCPU
         * needs to see.
         */
        qatomic_set(&cs->exit_request, true);
        qatomic_set(&cs->neg.icount_decr.u16.high, -1);
    }

    if (chain->sa_flags & SA_SIGINFO) {
        if (chain->sa_sigaction) {
            chain->sa_sigaction(sig, info, uc);
        }
    } else if (chain->sa_handler != SIG_DFL && chain->sa_handler != SIG_IGN) {
        chain->sa_handler(sig);
    }
}

static void qvm_install_kick_handler(int sig)
{
    struct sigaction sa;

    QEMU_LOCK_GUARD(&qvm_signal_lock);
    if (qvm_kick_installed[sig]) {
        return;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = qvm_kick_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);

    if (sigaction(sig, &sa, &qvm_saved_sa[sig]) == 0) {
        qvm_kick_installed[sig] = true;
    }
}

/*
 * Work out which signals the client means as kicks: the ones it keeps blocked
 * in its own thread but asks KVM to unblock while the guest runs.
 */
static void qvm_arm_kick_signals(QvmVcpu *vcpu)
{
    sigset_t blocked;
    int sig;

    if (vcpu->kick_set_valid) {
        return;
    }

    if (pthread_sigmask(SIG_SETMASK, NULL, &blocked) != 0) {
        return;
    }

    sigemptyset(&vcpu->kick_set);
    for (sig = 1; sig < NSIG; sig++) {
        if (sigismember(&blocked, sig) == 1 &&
            sigismember(&vcpu->sigmask, sig) == 0) {
            sigaddset(&vcpu->kick_set, sig);
            qvm_install_kick_handler(sig);
        }
    }
    vcpu->kick_set_valid = true;
}

static int qvm_set_signal_mask(QvmVcpu *vcpu,
                               const struct kvm_signal_mask *mask)
{
    unsigned i;

    vcpu->kick_set_valid = false;

    if (!mask) {
        vcpu->has_sigmask = false;
        return 0;
    }

    /*
     * The payload is the kernel's sigset_t: a little-endian bitmap in which
     * bit 0 is signal 1.  Decoding it bit by bit rather than casting keeps
     * this correct on hosts whose own sigset_t is laid out differently.
     */
    sigemptyset(&vcpu->sigmask);
    for (i = 0; i < mask->len * 8u; i++) {
        int sig = i + 1;

        if (sig < NSIG && (mask->sigset[i / 8] & (1u << (i % 8)))) {
            sigaddset(&vcpu->sigmask, sig);
        }
    }
    vcpu->has_sigmask = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* vCPU lifecycle                                                      */
/* ------------------------------------------------------------------ */

/*
 * Adopt @vcpu onto the calling thread.  KVM lets any thread issue KVM_RUN, so
 * this runs on every vCPU ioctl rather than once: QEMU decides whether work
 * such as a TLB flush can be done synchronously by comparing CPUState::thread
 * against the caller, and getting that wrong would queue work onto a vCPU
 * thread that does not exist.
 */
static void qvm_vcpu_bind(QvmVcpu *vcpu)
{
    CPUState *cs = vcpu->cs;

    if (!qvm_thread_rcu_registered) {
        rcu_register_thread();
        qvm_thread_rcu_registered = true;
    }
    if (!qvm_thread_tcg_registered) {
        tcg_register_thread();
        qvm_thread_tcg_registered = true;
    }

    if (!qemu_cpu_is_self(cs)) {
        qemu_thread_get_self(cs->thread);
        cs->thread_id = qemu_get_thread_id();
    }

    current_cpu = cs;
    cs->neg.can_do_io = true;
}

int qvm_vcpu_create(QvmVM *vm, int id, QvmVcpu **vcpup)
{
    MachineState *ms;
    QvmVcpu *vcpu;
    CPUState *cs;
    Object *obj;
    Error *err = NULL;

    if (id < 0 || id >= QVM_MAX_VCPUS) {
        return qvm_err(EINVAL);
    }
    if (vm->vcpus[id]) {
        return qvm_err(EEXIST);
    }

    if (qvm_arch_vcpu_realize(vm, id, &cs) < 0) {
        return -1;
    }

    if (cs->cpu_index < 0 || cs->cpu_index >= QVM_MAX_VCPUS) {
        return qvm_err(EINVAL);
    }

    vcpu = g_new0(QvmVcpu, 1);
    vcpu->vm = vm;
    vcpu->id = id;
    vcpu->cs = cs;
    vcpu->run = qemu_memalign(QVM_KVM_PAGE_SIZE, QVM_VCPU_MMAP_SIZE);
    memset(vcpu->run, 0, QVM_VCPU_MMAP_SIZE);

    vm->vcpus[id] = vcpu;
    qvm_vcpus[cs->cpu_index] = vcpu;
    qvm_arch_vcpu_init_state(vcpu);

    /*
     * The vCPU is runnable from the moment it is created, as KVM's is.  QEMU
     * parks freshly created CPUs instead, expecting an accelerator thread to
     * release them.
     */
    bql_lock();
    cs->stop = false;
    cs->stopped = false;
    cs->halted = 0;
    /*
     * CF_PARALLEL is not conditional here the way it is for QEMU's own
     * accelerator, which sets it from the machine's CPU count.  A QVM client
     * creates vCPUs when it likes and runs each on a thread of its own, so
     * from the first one onwards they really can execute at the same time --
     * and without this, TCG compiles the guest's atomic instructions into
     * non-atomic sequences.  A single vCPU never notices; two deadlock on the
     * first lock they contend.
     */
    tcg_cflags_set(cs, (cs->cluster_index << CF_CLUSTER_SHIFT) | CF_PARALLEL);
    bql_unlock();

    qvm_vcpu_bind(vcpu);

    *vcpup = vcpu;
    return 0;
}

void qvm_vcpu_destroy(QvmVcpu *vcpu)
{
    /*
     * Removing a realized CPU from a running QEMU is a hotunplug operation
     * that the qvm machine does not support, so the vCPU outlives its
     * descriptor.  Only the shared page is unpublished.
     */
}

/* ------------------------------------------------------------------ */
/* KVM_RUN                                                             */
/* ------------------------------------------------------------------ */

static int qvm_vcpu_run(QvmVcpu *vcpu)
{
    CPUState *cs = vcpu->cs;
    struct kvm_run *run = vcpu->run;
    sigset_t saved_mask;
    bool mask_swapped = false;
    bool completion_done = false;
    int ret;

    qvm_arch_prepare_run(vcpu);

    run->exit_reason = KVM_EXIT_UNKNOWN;

    if (vcpu->has_sigmask) {
        qvm_arm_kick_signals(vcpu);
        pthread_sigmask(SIG_SETMASK, &vcpu->sigmask, &saved_mask);
        mask_swapped = true;
    }

    /*
     * cpu_exec() returns for reasons that are QEMU's business rather than the
     * client's -- an inter-CPU kick, a translation buffer flush, an atomic
     * section needing serialisation.  KVM_RUN only returns when there is
     * something to report, so absorb those here and re-enter the guest.
     */
    for (;;) {
        bool completing = vcpu->completing_io && !completion_done;

        if (qatomic_read(&qvm_shutdown)) {
            run->exit_reason = KVM_EXIT_SHUTDOWN;
            break;
        }

        /*
         * A kick ends the run -- but not before any I/O the client has already
         * been told about has been delivered to the guest, which is what its
         * zero-length entries rely on.
         */
        if (qvm_kick_received && !completing) {
            qvm_kick_received = 0;
            run->exit_reason = KVM_EXIT_INTR;
            ret = qvm_err(EINTR);
            goto out;
        }

        /*
         * Stand in for the accelerator's vCPU thread loop, which QVM does not
         * have: acknowledge any pending kick (cpu_exec() only tests the flag,
         * it never clears it) and drain cross-CPU work such as TLB flushes.
         * Unlike qemu_process_cpu_events(), never block waiting for the vCPU
         * to become runnable -- a halted vCPU is reported to the client as
         * KVM_EXIT_HLT and it decides what happens next.
         */
        bql_lock();
        qatomic_set(&cs->exit_request, false);
        qemu_process_cpu_events_common(cs);
        bql_unlock();

        vcpu->exit_pending = false;

        if (completing) {
            /*
             * Translate the restarted instruction on its own so that finishing
             * it cannot run away into the rest of its original block.
             */
            cs->cflags_next_tb = cs->tcg_cflags | CF_NOIRQ | 1;
            completion_done = true;
        }

        cpu_exec_start(cs);
        ret = cpu_exec(cs);
        cpu_exec_end(cs);

        if (ret == EXCP_HLT || ret == EXCP_HALTED) {
            run->exit_reason = KVM_EXIT_HLT;
            break;
        }
        if (ret == EXCP_DEBUG) {
            run->exit_reason = KVM_EXIT_DEBUG;
            break;
        }
        if (ret == EXCP_ATOMIC) {
            cpu_exec_step_atomic(cs);
            continue;
        }
        if (ret == EXCP_INTERRUPT || ret == EXCP_YIELD) {
            /* A trap handler filled run in before unwinding us. */
            if (run->exit_reason != KVM_EXIT_UNKNOWN) {
                break;
            }
            continue;
        }

        run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
        run->internal.suberror = KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON;
        run->internal.ndata = 1;
        run->internal.data[0] = ret;
        break;
    }

    ret = 0;

out:
    if (mask_swapped) {
        pthread_sigmask(SIG_SETMASK, &saved_mask, NULL);
    }

    /*
     * A reported I/O or MMIO access leaves the guest rip on the instruction
     * that caused it; the next KVM_RUN re-executes that instruction and must
     * complete the access from the shared page rather than trap again.
     */
    vcpu->completing_io = (run->exit_reason == KVM_EXIT_IO ||
                           run->exit_reason == KVM_EXIT_MMIO);

    qvm_arch_update_run_state(vcpu);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

int qvm_vcpu_ioctl(QvmVcpu *vcpu, unsigned long request, uintptr_t arg)
{
    qvm_vcpu_bind(vcpu);

    switch (request) {
    case KVM_RUN:
        return qvm_vcpu_run(vcpu);

    case KVM_SET_SIGNAL_MASK:
        /* A null argument means "do not touch the mask while running". */
        return qvm_set_signal_mask(vcpu, (const struct kvm_signal_mask *)arg);

    default:
        return qvm_arch_vcpu_ioctl(vcpu, request, arg);
    }
}
