/*
 * QVM vCPUs: creation, register access and KVM_RUN.
 *
 * A QVM vCPU is an ordinary QEMU X86CPU that QEMU never schedules itself.
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
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/memalign.h"
#include "qemu/rcu.h"
#include "cpu.h"
#include "tcg/helper-tcg.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/cputlb.h"
#include "exec/translation-block.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev.h"
#include "system/cpus.h"
#include "tcg/startup.h"

#include "qvm-internal.h"

/*
 * eflags bits KVM_SET_REGS is allowed to load.  The arithmetic flags are held
 * lazily in cc_src/cc_dst and DF is kept in env->df, so both are excluded here
 * and restored by cpu_load_eflags() instead.
 */
#define QVM_EFLAGS_MASK (TF_MASK | AC_MASK | ID_MASK | NT_MASK | \
                         IF_MASK | IOPL_MASK | VM_MASK | RF_MASK)

/* Indexed by CPUState::cpu_index, for looking up the vCPU behind current_cpu. */
static QvmVcpu *qvm_vcpus[QVM_MAX_VCPUS];

/*
 * Whether this thread has been introduced to QEMU's RCU and TCG subsystems.
 * The thread that called qemu_init() is already an RCU reader; every other
 * client thread that runs a vCPU has to register itself.
 */
static __thread bool qvm_thread_rcu_registered;
static __thread bool qvm_thread_tcg_registered;

void qvm_thread_mark_rcu_registered(void)
{
    qvm_thread_rcu_registered = true;
}

QvmVcpu *qvm_vcpu_current(void)
{
    CPUState *cs = current_cpu;

    if (!cs || cs->cpu_index < 0 || cs->cpu_index >= QVM_MAX_VCPUS) {
        return NULL;
    }
    return qvm_vcpus[cs->cpu_index];
}

void qvm_vcpu_request_exit(QvmVcpu *vcpu)
{
    vcpu->exit_pending = true;
}

/*
 * Installed as qvm_io_exit_hook: called from the x86 port I/O helpers once the
 * access is complete and no memory-subsystem locks are held.  Rewinding to the
 * start of the instruction means the client sees the same restart semantics as
 * KVM: the instruction is re-executed on the next KVM_RUN, and the trap
 * handlers complete it from the shared page instead of trapping again.
 */
void qvm_io_exit(CPUState *cs, uintptr_t retaddr)
{
    QvmVcpu *vcpu;

    if (cs->cpu_index < 0 || cs->cpu_index >= QVM_MAX_VCPUS) {
        return;
    }
    vcpu = qvm_vcpus[cs->cpu_index];
    if (!vcpu || !vcpu->exit_pending) {
        return;
    }

    vcpu->exit_pending = false;
    cs->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit_restore(cs, retaddr);
}

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

    /*
     * cpu_create() is not enough for x86: the APIC id is normally assigned by
     * the board's pre-plug handler, and realize refuses to run without one.
     * QVM has no board to do that, so number the vCPUs itself.
     */
    bql_lock();
    ms = MACHINE(qdev_get_machine());
    obj = object_new(ms->cpu_type);
    object_property_set_int(obj, "apic-id", id, &error_abort);
    if (!qdev_realize(DEVICE(obj), NULL, &err)) {
        error_report_err(err);
        object_unref(obj);
        bql_unlock();
        return qvm_err(EINVAL);
    }
    cs = CPU(obj);
    bql_unlock();

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

    /*
     * The vCPU is runnable from the moment it is created, as KVM's is.  QEMU
     * parks freshly created CPUs instead, expecting an accelerator thread to
     * release them.
     */
    bql_lock();
    cs->stop = false;
    cs->stopped = false;
    cs->halted = 0;
    tcg_cflags_set(cs, cs->cluster_index << CF_CLUSTER_SHIFT);
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

static void seg_to_kvm(struct kvm_segment *out, const SegmentCache *in)
{
    unsigned flags = in->flags;

    out->selector = in->selector;
    out->base = in->base;
    out->limit = in->limit;
    out->type = (flags >> DESC_TYPE_SHIFT) & 15;
    out->present = (flags & DESC_P_MASK) != 0;
    out->dpl = (flags >> DESC_DPL_SHIFT) & 3;
    out->db = (flags >> DESC_B_SHIFT) & 1;
    out->s = (flags & DESC_S_MASK) != 0;
    out->l = (flags >> DESC_L_SHIFT) & 1;
    out->g = (flags & DESC_G_MASK) != 0;
    out->avl = (flags & DESC_AVL_MASK) != 0;
    out->unusable = !out->present;
    out->padding = 0;
}

static void seg_from_kvm(SegmentCache *out, const struct kvm_segment *in)
{
    out->selector = in->selector;
    out->base = in->base;
    out->limit = in->limit;
    out->flags = (in->type << DESC_TYPE_SHIFT) |
                 ((in->present && !in->unusable) * DESC_P_MASK) |
                 (in->dpl << DESC_DPL_SHIFT) |
                 (in->db << DESC_B_SHIFT) |
                 (in->s * DESC_S_MASK) |
                 (in->l << DESC_L_SHIFT) |
                 (in->g * DESC_G_MASK) |
                 (in->avl * DESC_AVL_MASK);
}

static void qvm_vcpu_get_regs(QvmVcpu *vcpu, struct kvm_regs *regs)
{
    CPUX86State *env = cpu_env(vcpu->cs);

    regs->rax = env->regs[R_EAX];
    regs->rbx = env->regs[R_EBX];
    regs->rcx = env->regs[R_ECX];
    regs->rdx = env->regs[R_EDX];
    regs->rsi = env->regs[R_ESI];
    regs->rdi = env->regs[R_EDI];
    regs->rsp = env->regs[R_ESP];
    regs->rbp = env->regs[R_EBP];
#ifdef TARGET_X86_64
    regs->r8  = env->regs[8];
    regs->r9  = env->regs[9];
    regs->r10 = env->regs[10];
    regs->r11 = env->regs[11];
    regs->r12 = env->regs[12];
    regs->r13 = env->regs[13];
    regs->r14 = env->regs[14];
    regs->r15 = env->regs[15];
#endif
    regs->rip = env->eip;
    regs->rflags = cpu_compute_eflags(env);
}

static void qvm_vcpu_set_regs(QvmVcpu *vcpu, const struct kvm_regs *regs)
{
    CPUX86State *env = cpu_env(vcpu->cs);

    env->regs[R_EAX] = regs->rax;
    env->regs[R_EBX] = regs->rbx;
    env->regs[R_ECX] = regs->rcx;
    env->regs[R_EDX] = regs->rdx;
    env->regs[R_ESI] = regs->rsi;
    env->regs[R_EDI] = regs->rdi;
    env->regs[R_ESP] = regs->rsp;
    env->regs[R_EBP] = regs->rbp;
#ifdef TARGET_X86_64
    env->regs[8]  = regs->r8;
    env->regs[9]  = regs->r9;
    env->regs[10] = regs->r10;
    env->regs[11] = regs->r11;
    env->regs[12] = regs->r12;
    env->regs[13] = regs->r13;
    env->regs[14] = regs->r14;
    env->regs[15] = regs->r15;
#endif
    env->eip = regs->rip;
    cpu_load_eflags(env, regs->rflags, QVM_EFLAGS_MASK);
    x86_update_hflags(env);

    /*
     * Moving rip invalidates any I/O access the previous exit left half
     * finished: the instruction that was going to be re-executed may not be
     * the one that runs next.
     */
    vcpu->completing_io = false;
}

static void qvm_vcpu_get_sregs(QvmVcpu *vcpu, struct kvm_sregs *sregs)
{
    CPUX86State *env = cpu_env(vcpu->cs);

    memset(sregs, 0, sizeof(*sregs));

    seg_to_kvm(&sregs->cs, &env->segs[R_CS]);
    seg_to_kvm(&sregs->ds, &env->segs[R_DS]);
    seg_to_kvm(&sregs->es, &env->segs[R_ES]);
    seg_to_kvm(&sregs->fs, &env->segs[R_FS]);
    seg_to_kvm(&sregs->gs, &env->segs[R_GS]);
    seg_to_kvm(&sregs->ss, &env->segs[R_SS]);
    seg_to_kvm(&sregs->tr, &env->tr);
    seg_to_kvm(&sregs->ldt, &env->ldt);

    sregs->gdt.base = env->gdt.base;
    sregs->gdt.limit = env->gdt.limit;
    sregs->idt.base = env->idt.base;
    sregs->idt.limit = env->idt.limit;

    sregs->cr0 = env->cr[0];
    sregs->cr2 = env->cr[2];
    sregs->cr3 = env->cr[3];
    sregs->cr4 = env->cr[4];
    sregs->cr8 = 0;
    sregs->efer = env->efer;
    sregs->apic_base = 0;
}

static void qvm_vcpu_set_sregs(QvmVcpu *vcpu, const struct kvm_sregs *sregs)
{
    CPUState *cs = vcpu->cs;
    CPUX86State *env = cpu_env(cs);

    seg_from_kvm(&env->segs[R_CS], &sregs->cs);
    seg_from_kvm(&env->segs[R_DS], &sregs->ds);
    seg_from_kvm(&env->segs[R_ES], &sregs->es);
    seg_from_kvm(&env->segs[R_FS], &sregs->fs);
    seg_from_kvm(&env->segs[R_GS], &sregs->gs);
    seg_from_kvm(&env->segs[R_SS], &sregs->ss);
    seg_from_kvm(&env->tr, &sregs->tr);
    seg_from_kvm(&env->ldt, &sregs->ldt);

    env->gdt.base = sregs->gdt.base;
    env->gdt.limit = sregs->gdt.limit;
    env->idt.base = sregs->idt.base;
    env->idt.limit = sregs->idt.limit;

    env->cr[2] = sregs->cr2;

    /*
     * Take EFER as given rather than deriving LMA from a CR0.PG transition:
     * the client is describing a complete state, not stepping the CPU through
     * one, and KVM_SET_SREGS carries LMA explicitly.
     */
    env->efer = sregs->efer;
    env->cr[0] = sregs->cr0 | CR0_ET_MASK;
    cpu_x86_update_cr4(env, sregs->cr4);
    env->cr[3] = sregs->cr3;

    x86_update_hflags(env);
    tlb_flush(cs);
}

static int qvm_vcpu_run(QvmVcpu *vcpu)
{
    CPUState *cs = vcpu->cs;
    struct kvm_run *run = vcpu->run;
    int ret;

    run->exit_reason = KVM_EXIT_UNKNOWN;
    run->ready_for_interrupt_injection = 1;

    /*
     * cpu_exec() returns for reasons that are QEMU's business rather than the
     * client's -- an inter-CPU kick, a translation buffer flush, an atomic
     * section needing serialisation.  KVM_RUN only returns when there is
     * something to report, so absorb those here and re-enter the guest.
     */
    for (;;) {
        if (qatomic_read(&qvm_shutdown)) {
            run->exit_reason = KVM_EXIT_SHUTDOWN;
            break;
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

    /*
     * A reported I/O access leaves the guest rip on the instruction that
     * caused it; the next KVM_RUN re-executes that instruction and must
     * complete the access from the shared page rather than trap again.
     */
    vcpu->completing_io = (run->exit_reason == KVM_EXIT_IO);

    return 0;
}

int qvm_vcpu_ioctl(QvmVcpu *vcpu, unsigned long request, uintptr_t arg)
{
    qvm_vcpu_bind(vcpu);

    switch (request) {
    case KVM_RUN:
        return qvm_vcpu_run(vcpu);

    case KVM_GET_REGS: {
        struct kvm_regs regs;

        if (!arg) {
            return qvm_err(EFAULT);
        }
        qvm_vcpu_get_regs(vcpu, &regs);
        memcpy((void *)arg, &regs, sizeof(regs));
        return 0;
    }

    case KVM_SET_REGS: {
        struct kvm_regs regs;

        if (!arg) {
            return qvm_err(EFAULT);
        }
        memcpy(&regs, (void *)arg, sizeof(regs));
        qvm_vcpu_set_regs(vcpu, &regs);
        return 0;
    }

    case KVM_GET_SREGS: {
        struct kvm_sregs sregs;

        if (!arg) {
            return qvm_err(EFAULT);
        }
        qvm_vcpu_get_sregs(vcpu, &sregs);
        memcpy((void *)arg, &sregs, sizeof(sregs));
        return 0;
    }

    case KVM_SET_SREGS: {
        struct kvm_sregs sregs;

        if (!arg) {
            return qvm_err(EFAULT);
        }
        memcpy(&sregs, (void *)arg, sizeof(sregs));
        qvm_vcpu_set_sregs(vcpu, &sregs);
        return 0;
    }

    default:
        return qvm_err(ENOTTY);
    }
}
