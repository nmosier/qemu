/*
 * The parts of the KVM API that differ per guest architecture.
 *
 * QVM's core -- descriptors, memory slots, the run loop, exits, signals -- is
 * the same whatever the guest is, because that much of KVM's API is.  What is
 * not shared is the vCPU's state and how interrupts reach it, which is nearly
 * all of what a client does between runs.  Each guest architecture implements
 * this interface once (qvm-x86.c, qvm-arm.c) and the core never has to know
 * which one it is talking to.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef QVM_ARCH_H
#define QVM_ARCH_H

#include "qvm-internal.h"

/**
 * qvm_arch_name: the guest architecture, for diagnostics.
 */
const char *qvm_arch_name(void);

/**
 * qvm_arch_default_cpu: QEMU CPU model to emulate when QVM_CPU is unset.
 *
 * Should be the most capable model this QEMU can execute: a client describes
 * the CPU it wants the guest to see itself, and anything it advertises that
 * the emulated CPU cannot execute becomes an undefined-instruction trap.
 */
const char *qvm_arch_default_cpu(void);

/**
 * qvm_arch_machine_class_init: finish the qvm machine for this architecture.
 */
void qvm_arch_machine_class_init(MachineClass *mc);

/**
 * qvm_arch_has_pio: whether the guest architecture has a port I/O space.
 *
 * Only x86 does; elsewhere everything is memory mapped and QVM registers no
 * port trap at all.
 */
bool qvm_arch_has_pio(void);

/**
 * qvm_arch_vcpu_realize: create the underlying QEMU CPU for @vcpu.
 *
 * Architectures differ in what has to be set before realize (an APIC id on
 * x86) and taken back out after it (the APIC itself, which belongs to the
 * client).  Returns 0, or -1 with errno set.
 */
int qvm_arch_vcpu_realize(QvmVM *vm, int id, CPUState **csp);

/**
 * qvm_arch_vcpu_reset_state: initialise the QVM-side state of a new vCPU.
 */
void qvm_arch_vcpu_init_state(QvmVcpu *vcpu);

/**
 * qvm_arch_install_hooks: point QEMU's QVM hooks at this architecture's
 * implementations.  See "system/qvm-hooks.h".
 */
void qvm_arch_install_hooks(void);

/**
 * qvm_arch_prepare_run: take in whatever the client changed through kvm_run
 * and line QEMU's pending-interrupt state up with it, before entering a run.
 */
void qvm_arch_prepare_run(QvmVcpu *vcpu);

/**
 * qvm_arch_update_run_state: refresh the fields of kvm_run that describe
 * whether the guest could take an interrupt.
 */
void qvm_arch_update_run_state(QvmVcpu *vcpu);

/**
 * qvm_arch_sys_ioctl: architecture-specific requests on the /dev/kvm handle.
 * qvm_arch_vm_ioctl: ... on a VM handle.
 * qvm_arch_vcpu_ioctl: ... on a vCPU handle, which is most of the state the
 *                      client exchanges.
 *
 * Each returns -1/ENOTTY for a request it does not implement, so the core can
 * try its own generic handling first and fall through to here.
 */
int qvm_arch_sys_ioctl(unsigned long request, uintptr_t arg);
int qvm_arch_vm_ioctl(QvmVM *vm, unsigned long request, uintptr_t arg);
int qvm_arch_vcpu_ioctl(QvmVcpu *vcpu, unsigned long request, uintptr_t arg);

/**
 * qvm_arch_check_extension: architecture-specific KVM_CHECK_EXTENSION values.
 *
 * Returns -1 to let the core answer for capabilities that are not
 * architecture specific.
 */
int qvm_arch_check_extension(unsigned long cap);

#endif /* QVM_ARCH_H */
