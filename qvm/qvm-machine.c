/*
 * The "qvm" machine.
 *
 * Deliberately empty: everything a board would normally wire up arrives later
 * over the KVM API instead.  Guest RAM comes from KVM_SET_USER_MEMORY_REGION,
 * vCPUs from KVM_CREATE_VCPU, and device emulation stays on the client side of
 * KVM_EXIT_IO / KVM_EXIT_MMIO.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/core/boards.h"

#include "qvm-arch.h"
#include "qvm-internal.h"

static void qvm_machine_init(MachineState *machine)
{
    /* Nothing to build; see the file comment. */
}

/*
 * Registering the machine itself is left to the architecture backend.  Which
 * machines a target will even consider is target-specific: ARM filters the
 * list by an interface that a plain DEFINE_MACHINE does not carry, so a
 * generic definition here would be invisible to qemu-system-aarch64 while
 * working perfectly well on x86.
 */
void qvm_machine_class_init(MachineClass *mc)
{
    mc->desc = "QVM (KVM API emulated on TCG)";
    mc->init = qvm_machine_init;
    mc->max_cpus = QVM_MAX_VCPUS;

    /* The client owns the machine's memory map, so QEMU allocates none. */
    mc->default_ram_size = 0;
    mc->default_ram_id = NULL;

    mc->no_serial = 1;
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;

    qvm_arch_machine_class_init(mc);
}
