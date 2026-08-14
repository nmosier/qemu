/*
 * Hooks used by QVM, QEMU's userspace implementation of the KVM API.
 *
 * QVM (see qvm/) presents a QEMU system emulator to its client as if it were
 * /dev/kvm.  A KVM_RUN has to stop the vCPU at a precise instruction boundary
 * when the guest touches something the client is responsible for emulating,
 * which is not something the normal device path ever needs to do.  Rather than
 * unwind from inside a MemoryRegion callback -- where an RCU read section and
 * possibly the BQL are still held -- the callback only records the pending
 * exit, and the unwind happens here, once the access has fully completed.
 *
 * The hook is NULL, and these checks are dead code, unless libqvm is driving
 * this QEMU instance.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef SYSTEM_QVM_HOOKS_H
#define SYSTEM_QVM_HOOKS_H

#include "qemu/typedefs.h"

/**
 * qvm_io_exit_hook: leave cpu_exec() if QVM has an exit pending for @cs.
 * @retaddr: host return address inside the translated block, used to restore
 *           the guest state to the start of the faulting instruction.
 *
 * Does not return when an exit is pending.
 */
extern void (*qvm_io_exit_hook)(CPUState *cs, uintptr_t retaddr);

static inline void qvm_io_exit_check(CPUState *cs, uintptr_t retaddr)
{
    if (unlikely(qvm_io_exit_hook)) {
        qvm_io_exit_hook(cs, retaddr);
    }
}

#endif /* SYSTEM_QVM_HOOKS_H */
