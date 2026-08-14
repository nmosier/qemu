/*
 * QVM internals.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef QVM_INTERNAL_H
#define QVM_INTERNAL_H

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "hw/core/cpu.h"
#include "system/memory.h"

#include <signal.h>
#include <linux/kvm.h>

/*
 * <sys/ioctl.h> on BSD-derived hosts defines _IO()/_IOR()/... with a layout
 * that is not Linux's.  qvm/linux-compat/linux/ioctl.h overrides it, but only
 * if it is the last one included; catch the other order at build time rather
 * than by silently computing the wrong request numbers.
 */
QEMU_BUILD_BUG_MSG(KVM_GET_API_VERSION != 0xae00,
                   "KVM ioctl numbers do not use Linux's _IOC encoding");

#define QVM_MAX_VCPUS       8
#define QVM_MAX_MEMSLOTS    32

/*
 * The vCPU shared page layout is KVM's: struct kvm_run in the first page, the
 * port I/O data buffer in the second.  These are ABI, not host properties, so
 * they stay 4KiB even where the host page size is larger.
 */
#define QVM_KVM_PAGE_SIZE       4096
#define QVM_VCPU_MMAP_SIZE      (2 * QVM_KVM_PAGE_SIZE)
#define QVM_PIO_DATA_OFFSET     QVM_KVM_PAGE_SIZE

typedef struct QvmVM QvmVM;

/*
 * Whatever the guest architecture needs to remember about a vCPU beyond the
 * common state below -- interrupt-controller registers the client owns, the
 * CPU description it installed, and so on.  Defined in qvm-x86.c / qvm-arm.c.
 */
typedef struct QvmVcpuArch QvmVcpuArch;

typedef struct QvmMemSlot {
    bool used;
    uint32_t flags;
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    uint64_t userspace_addr;
    MemoryRegion *mr;
} QvmMemSlot;

typedef struct QvmVcpu {
    QvmVM *vm;
    int id;
    CPUState *cs;

    /* The shared page pair handed out by qvm_mmap(). */
    struct kvm_run *run;

    /*
     * Set by a trap handler that has filled in @run and needs the vCPU to
     * leave cpu_exec() at the current instruction boundary.
     */
    bool exit_pending;

    /*
     * The instruction reported by the previous exit is about to be re-executed
     * (see qvm_vcpu_run()).  Its first guest-visible access must be completed
     * from @run rather than trapped again.
     */
    bool completing_io;

    /*
     * Signals the client wants unblocked while the guest runs
     * (KVM_SET_SIGNAL_MASK).  Delivery of one of them ends the KVM_RUN with
     * KVM_EXIT_INTR, which is how a client bounds a run.
     */
    bool has_sigmask;
    sigset_t sigmask;

    /* Cached once per mask: signals blocked outside the run but not inside. */
    bool kick_set_valid;
    sigset_t kick_set;

    /* See QvmVcpuArch; allocated by qvm_arch_vcpu_init_state(). */
    QvmVcpuArch *arch;
} QvmVcpu;

struct QvmVM {
    QemuMutex lock;
    QvmMemSlot slots[QVM_MAX_MEMSLOTS];
    QvmVcpu *vcpus[QVM_MAX_VCPUS];

    /* Identity-mapping base for KVM_SET_TSS_ADDR/KVM_SET_IDENTITY_MAP_ADDR. */
    uint64_t tss_addr;
    uint64_t identity_map_addr;
};

/* qvm-vm.c */
void qvm_qemu_ensure_started(void);
int qvm_vm_create(QvmVM **vmp);
int qvm_vm_ioctl(QvmVM *vm, unsigned long request, uintptr_t arg);
void qvm_vm_destroy(QvmVM *vm);

/* Set once QEMU's event loop has exited; reported as KVM_EXIT_SHUTDOWN. */
extern bool qvm_shutdown;

/* qvm-vcpu.c */
int qvm_vcpu_create(QvmVM *vm, int id, QvmVcpu **vcpup);
int qvm_vcpu_ioctl(QvmVcpu *vcpu, unsigned long request, uintptr_t arg);
void qvm_vcpu_destroy(QvmVcpu *vcpu);
QvmVcpu *qvm_vcpu_current(void);
QvmVcpu *qvm_vcpu_of(CPUState *cs);

/* Installed as the io-exit hook in "system/qvm-hooks.h". */
void qvm_io_exit(CPUState *cs, uintptr_t retaddr);

/* Record that the thread calling qemu_init() is already an RCU reader. */
void qvm_thread_mark_rcu_registered(void);

/* Marks @vcpu as needing to leave cpu_exec() once the access completes. */
void qvm_vcpu_request_exit(QvmVcpu *vcpu);

/* Called by a trap handler that has just finished a previously reported
 * access from the shared page. */
void qvm_vcpu_io_completed(QvmVcpu *vcpu);

/* Pointer to the port I/O data buffer inside the vCPU's shared pages. */
static inline void *qvm_run_io_data(struct kvm_run *run)
{
    return (uint8_t *)run + QVM_PIO_DATA_OFFSET;
}

/* qvm-fd.c */
int qvm_fd_alloc_vm(QvmVM *vm);
int qvm_fd_alloc_vcpu(QvmVcpu *vcpu);
int qvm_check_extension(unsigned long cap);

/*
 * Report a failure the way an ioctl() would.  KVM returns negative errno
 * values from its handlers; the syscall layer turns them into -1/errno.
 */
static inline int qvm_err(int err)
{
    errno = err;
    return -1;
}

#endif /* QVM_INTERNAL_H */
