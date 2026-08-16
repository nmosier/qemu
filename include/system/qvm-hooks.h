/*
 * Hooks used by QVM, QEMU's userspace implementation of the KVM API.
 *
 * QVM (see qvm/) presents a QEMU system emulator to its client as if it were
 * /dev/kvm.  Most of that is done from outside QEMU's core, but a few things a
 * KVM client may do have no equivalent in a normal QEMU machine and need a
 * choke point inside it.  Each hook here is NULL, and the checks around it dead
 * code, unless libqvm is driving this QEMU instance.
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
 * A KVM_RUN has to stop the vCPU at a precise instruction boundary when the
 * guest touches something the client is responsible for emulating.  Unwinding
 * from inside the MemoryRegion callback that noticed is not safe -- an RCU read
 * section and possibly the BQL are still held -- so the callback only records
 * the pending exit and the unwind happens here, once the access has completed.
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

/**
 * qvm_plugin_halt_hook: a TCG plugin asked for @cs to stop executing.
 *
 * Plugins observe the guest; this is the one thing they can ask it to do.
 * QVM records the request so that the KVM_RUN in progress returns to the
 * client instead of resuming, which is what makes the stop visible outside
 * the emulator at all.
 */
extern void (*qvm_plugin_halt_hook)(CPUState *cs);

/**
 * qvm_pic_interrupt_hook: source the vector for a pending hardware interrupt.
 *
 * QVM's client owns the interrupt controller, so vectors arrive over
 * KVM_INTERRUPT rather than from an emulated PIC or APIC.  When set, this
 * replaces cpu_get_pic_interrupt() entirely.  It may not return: QVM uses the
 * same call site to notice that the guest has become able to accept an
 * interrupt, which is what the client asked for with
 * kvm_run::request_interrupt_window.
 */
extern int (*qvm_pic_interrupt_hook)(CPUState *cs);

/**
 * qvm_cpuid_hook: answer a guest CPUID from the client's table.
 *
 * A KVM client describes the CPU it wants the guest to see with
 * KVM_SET_CPUID2, which need not be the CPU model QEMU was configured with.
 * Returns true when the leaf was answered from that table.
 */
extern bool (*qvm_cpuid_hook)(CPUState *cs, uint32_t function, uint32_t index,
                              uint32_t *eax, uint32_t *ebx,
                              uint32_t *ecx, uint32_t *edx);

#endif /* SYSTEM_QVM_HOOKS_H */
