/*
 * QVM - a userspace implementation of the KVM API backed by QEMU's system-mode
 * TCG, usable on any host architecture.
 *
 * An application that would normally drive /dev/kvm through open(), ioctl() and
 * mmap() can instead link against libqvm and call qvm_open(), qvm_ioctl() and
 * qvm_mmap().  The request numbers and structures are exactly Linux's, taken
 * from <linux/kvm.h>; no host kernel support is required.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef QVM_QVM_H
#define QVM_QVM_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * QVM descriptors live in their own namespace: they are small non-negative
 * integers, but they are not host file descriptors and must only be passed to
 * the qvm_* functions.
 */

/**
 * qvm_open: open a QVM descriptor for the emulated /dev/kvm.
 * @path: must be "/dev/kvm".
 * @flags: accepted for symmetry with open(2); ignored.
 *
 * Returns a QVM descriptor, or -1 with errno set.
 */
int qvm_open(const char *path, int flags);

/**
 * qvm_close: release a QVM descriptor.
 *
 * Returns 0, or -1 with errno set.
 */
int qvm_close(int fd);

/**
 * qvm_ioctl: issue a KVM request against a QVM descriptor.
 * @fd: a descriptor from qvm_open() or from KVM_CREATE_VM/KVM_CREATE_VCPU.
 * @request: a KVM_* request number from <linux/kvm.h>.
 *
 * Returns the request-specific result (usually 0), or -1 with errno set.
 */
int qvm_ioctl(int fd, unsigned long request, ...);

/**
 * qvm_mmap: map a QVM descriptor's shared region.
 *
 * Mapping a vCPU descriptor at offset 0 yields its struct kvm_run, exactly as
 * mmap()ing a KVM vCPU fd would.  Anonymous mappings (@fd < 0) are forwarded to
 * the host mmap(), so callers can use one allocator for guest RAM too.
 *
 * Returns the mapping, or MAP_FAILED with errno set.
 */
void *qvm_mmap(void *addr, size_t length, int prot, int flags,
               int fd, off_t offset);

/**
 * qvm_munmap: undo a qvm_mmap().
 *
 * Returns 0, or -1 with errno set.
 */
int qvm_munmap(void *addr, size_t length);

/**
 * qvm_vcpu_insns: guest instructions this vCPU has retired.
 * @fd: a vCPU descriptor from KVM_CREATE_VCPU.
 * @insns: filled in with the running total.
 *
 * QVM translates the guest, so unlike KVM it can say exactly how much work a
 * vCPU has done without any host counter to read.  The count covers only
 * instructions the guest actually completed: one abandoned part way through a
 * translated block -- by a trap, or by an access that became a KVM_EXIT_MMIO
 * -- is not counted until it is re-executed and finishes.
 *
 * Returns 0, or -1 with errno set.
 */
int qvm_vcpu_insns(int fd, unsigned long long *insns);

/**
 * qvm_vcpu_set_insn_budget: run the guest for a bounded number of instructions.
 * @fd: a vCPU descriptor from KVM_CREATE_VCPU.
 * @insns: how many more instructions to allow, or 0 to remove the bound.
 *
 * The budget spans KVM_RUN calls and is consumed as the guest executes.  When
 * it reaches zero the run ends with KVM_EXIT_INTR, as a signal would have
 * ended it; the guest stops on an instruction boundary, having executed
 * exactly the number asked for.  Setting a new budget resumes.
 *
 * This has no equivalent in KVM, where the closest a client can come is
 * arranging for a signal to arrive at roughly the right time.
 *
 * Returns 0, or -1 with errno set.
 */
int qvm_vcpu_set_insn_budget(int fd, unsigned long long insns);

/*
 * Reasons a run ended that KVM has no reason code for.  All of them report
 * KVM_EXIT_INTR to keep the API the one being emulated; this says which.
 */
#define QVM_HALT_NONE         0  /* ended for a reason KVM_RUN already gives */
#define QVM_HALT_INSN_BUDGET  1  /* qvm_vcpu_set_insn_budget() was exhausted */
#define QVM_HALT_PLUGIN       2  /* a TCG plugin called qemu_plugin_halt_vcpu */

/**
 * qvm_vcpu_halt_reason: why the last KVM_RUN on @fd ended.
 * @reason: filled in with one of the QVM_HALT_* values above.
 *
 * Only meaningful when that run reported KVM_EXIT_INTR; anything else leaves
 * it QVM_HALT_NONE, since the exit already says why.
 *
 * Returns 0, or -1 with errno set.
 */
int qvm_vcpu_halt_reason(int fd, unsigned *reason);

/**
 * qvm_load_plugin: load a QEMU TCG plugin into the emulator running the guest.
 * @path: the plugin shared object, as QEMU's -plugin file= would take it.
 * @args: plugin arguments in QEMU's "name=value,name=value" form, or NULL.
 *
 * QVM runs its guest under TCG, so QEMU's plugin interface -- instruction and
 * block callbacks, memory tracing, and so on -- is available to a client that
 * asks for it.  This has no equivalent in KVM, where the guest runs on real
 * hardware and there is nothing to instrument; it is the one thing QVM offers
 * that the API it emulates cannot.
 *
 * May be called before or after the guest starts.  Plugins loaded once the
 * guest is already running see everything from that point on: code translated
 * earlier is discarded so that it is instrumented on its next execution.
 *
 * Returns 0, or -1 with errno set.
 */
int qvm_load_plugin(const char *path, const char *args);

#ifdef __cplusplus
}
#endif

#endif /* QVM_QVM_H */
