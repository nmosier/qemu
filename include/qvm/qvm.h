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

#ifdef __cplusplus
}
#endif

#endif /* QVM_QVM_H */
