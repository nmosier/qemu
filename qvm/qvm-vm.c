/*
 * QVM virtual machine: QEMU bring-up, guest memory slots and VM-level ioctls.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu/lockable.h"
#include "qemu/thread.h"
#include "accel/accel-cpu-ops.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "system/address-spaces.h"
#include "system/cpus.h"
#include "system/memory.h"
#include "system/qvm-hooks.h"
#include "system/replay.h"
#include "system/runstate.h"
#include "system/system.h"
#include "qom/object.h"

#include "qvm-internal.h"

/*
 * system/main.c, which normally defines this, is the one object deliberately
 * left out of libqvm: the client owns main().  Nothing in the library installs
 * a main-thread hook, so QEMU's event loop is free to run on a thread of our
 * choosing.
 */
int (*qemu_main)(void);

/*
 * QEMU's machine, address spaces and accelerator are process-wide singletons,
 * so a process gets one QVM machine.  KVM_CREATE_VM beyond the first fails
 * with EBUSY rather than quietly aliasing the existing one.
 */
static QvmVM *qvm_the_vm;
static bool qvm_qemu_running;
static QemuThread qvm_main_loop_thread;

bool qvm_shutdown;

static MemoryRegion qvm_pio_mr;

/*
 * Port I/O.  Every port the client has not claimed with a device lands here
 * and becomes a KVM_EXIT_IO.
 *
 * These callbacks run inside an RCU read section, so they only record the exit;
 * the actual unwind out of cpu_exec() happens in qvm_io_exit_hook() once the
 * access has unwound back to the x86 I/O helper.
 */
static uint64_t qvm_pio_read(void *opaque, hwaddr addr, unsigned size)
{
    QvmVcpu *vcpu = qvm_vcpu_current();
    struct kvm_run *run;
    uint64_t val = 0;

    if (!vcpu) {
        /* Not a guest access (monitor, migration, ...): read as open bus. */
        return (uint64_t)-1;
    }
    run = vcpu->run;

    if (vcpu->completing_io) {
        /*
         * The client has already been told about this access and has left the
         * result in the shared page; the guest instruction is being
         * re-executed to consume it.
         */
        vcpu->completing_io = false;
        memcpy(&val, qvm_run_io_data(run), MIN(size, sizeof(val)));
        return val;
    }

    run->exit_reason = KVM_EXIT_IO;
    run->io.direction = KVM_EXIT_IO_IN;
    run->io.size = size;
    run->io.port = addr;
    run->io.count = 1;
    run->io.data_offset = QVM_PIO_DATA_OFFSET;
    memset(qvm_run_io_data(run), 0, size);
    qvm_vcpu_request_exit(vcpu);
    return 0;
}

static void qvm_pio_write(void *opaque, hwaddr addr, uint64_t data,
                          unsigned size)
{
    QvmVcpu *vcpu = qvm_vcpu_current();
    struct kvm_run *run;

    if (!vcpu) {
        return;
    }
    run = vcpu->run;

    if (vcpu->completing_io) {
        /* Already reported; re-executing the instruction must not repeat it. */
        vcpu->completing_io = false;
        return;
    }

    run->exit_reason = KVM_EXIT_IO;
    run->io.direction = KVM_EXIT_IO_OUT;
    run->io.size = size;
    run->io.port = addr;
    run->io.count = 1;
    run->io.data_offset = QVM_PIO_DATA_OFFSET;
    memcpy(qvm_run_io_data(run), &data, MIN(size, sizeof(data)));
    qvm_vcpu_request_exit(vcpu);
}

static const MemoryRegionOps qvm_pio_ops = {
    .read = qvm_pio_read,
    .write = qvm_pio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

/*
 * QVM runs each vCPU on whichever client thread calls KVM_RUN, the way KVM
 * does, so the accelerator must not spawn threads of its own.
 */
static void qvm_create_vcpu_thread(CPUState *cpu)
{
    cpu_thread_signal_created(cpu);
}

static void qvm_kick_vcpu_thread(CPUState *cpu)
{
    /*
     * cpus_kick_thread() would pthread_kill() the client's thread, which has
     * no SIG_IPI handler of its own.  Setting exit_request is enough: the vCPU
     * checks it between translation blocks.
     */
    qatomic_set(&cpu->exit_request, true);
}

static void *qvm_main_loop(void *opaque)
{
    rcu_register_thread();
    replay_mutex_lock();
    bql_lock();

    qemu_main_loop();

    qatomic_set(&qvm_shutdown, true);
    bql_unlock();
    replay_mutex_unlock();
    rcu_unregister_thread();
    return NULL;
}

static unsigned qvm_env_uint(const char *name, unsigned def)
{
    const char *val = getenv(name);
    unsigned long n;
    char *end;

    if (!val || !*val) {
        return def;
    }
    n = strtoul(val, &end, 0);
    if (*end || n == 0) {
        return def;
    }
    return n;
}

/*
 * The client never sees a QEMU command line, so the few knobs worth exposing
 * are read from the environment:
 *
 *   QVM_CPU  x86 CPU model to emulate               (default "qemu64")
 *   QVM_SMP  number of vCPUs the client may create  (default 1)
 *   QVM_LOG  QEMU -d log categories, e.g. "int,mmu" (default off)
 */
static void qvm_qemu_start(void)
{
    AccelOpsClass *ops;
    unsigned nr_cpus = MIN(qvm_env_uint("QVM_SMP", 1), QVM_MAX_VCPUS);
    g_autofree char *smp = g_strdup_printf("%u", nr_cpus);
    const char *cpu_model = getenv("QVM_CPU") ?: "qemu64";
    const char *log = getenv("QVM_LOG");
    g_autoptr(GPtrArray) argv = g_ptr_array_new();

    g_ptr_array_add(argv, (char *)"qvm");
    g_ptr_array_add(argv, (char *)"-machine");
    g_ptr_array_add(argv, (char *)"qvm");
    g_ptr_array_add(argv, (char *)"-accel");
    g_ptr_array_add(argv, (char *)"tcg");
    g_ptr_array_add(argv, (char *)"-cpu");
    g_ptr_array_add(argv, (char *)cpu_model);
    g_ptr_array_add(argv, (char *)"-smp");
    g_ptr_array_add(argv, smp);
    g_ptr_array_add(argv, (char *)"-display");
    g_ptr_array_add(argv, (char *)"none");
    g_ptr_array_add(argv, (char *)"-nodefaults");
    g_ptr_array_add(argv, (char *)"-no-user-config");
    if (log && *log) {
        g_ptr_array_add(argv, (char *)"-d");
        g_ptr_array_add(argv, (char *)log);
    }

    qemu_init(argv->len, (char **)argv->pdata);

    /*
     * qemu_init() returns holding the BQL and the replay lock, expecting its
     * caller to hand them to whichever thread runs the event loop.
     */
    ops = (AccelOpsClass *)cpus_get_accel();
    ops->create_vcpu_thread = qvm_create_vcpu_thread;
    ops->kick_vcpu_thread = qvm_kick_vcpu_thread;

    memory_region_init_io(&qvm_pio_mr, NULL, &qvm_pio_ops, NULL,
                          "qvm-pio", 0x10000);
    memory_region_add_subregion_overlap(get_system_io(), 0, &qvm_pio_mr, -1);

    qvm_io_exit_hook = qvm_io_exit;

    /* qemu_init() registered this thread with RCU on our behalf. */
    qvm_thread_mark_rcu_registered();

    vm_start();

    bql_unlock();
    replay_mutex_unlock();

    qemu_thread_create(&qvm_main_loop_thread, "qvm-main-loop",
                       qvm_main_loop, NULL, QEMU_THREAD_DETACHED);
    qvm_qemu_running = true;
}

int qvm_vm_create(QvmVM **vmp)
{
    QvmVM *vm;

    if (qvm_the_vm) {
        return qvm_err(EBUSY);
    }

    if (!qvm_qemu_running) {
        qvm_qemu_start();
    }

    vm = g_new0(QvmVM, 1);
    qemu_mutex_init(&vm->lock);
    vm->identity_map_addr = 0xfffbc000;
    vm->tss_addr = 0xfffbd000;

    qvm_the_vm = vm;
    *vmp = vm;
    return 0;
}

void qvm_vm_destroy(QvmVM *vm)
{
    /*
     * QEMU's machine cannot be torn down and rebuilt within a process, so a
     * closed VM stays closed; leave the state in place rather than pretend.
     */
}

static int qvm_set_memory_region(QvmVM *vm,
                                 const struct kvm_userspace_memory_region *m)
{
    QvmMemSlot *slot;
    MemoryRegion *mr;
    g_autofree char *name = NULL;

    if (m->slot >= QVM_MAX_MEMSLOTS) {
        return qvm_err(EINVAL);
    }
    if ((m->memory_size | m->guest_phys_addr | m->userspace_addr) &
        (QVM_KVM_PAGE_SIZE - 1)) {
        return qvm_err(EINVAL);
    }
    if (m->flags & ~(uint32_t)KVM_MEM_READONLY) {
        return qvm_err(EINVAL);
    }

    QEMU_LOCK_GUARD(&vm->lock);
    slot = &vm->slots[m->slot];

    bql_lock();
    if (slot->used) {
        memory_region_del_subregion(get_system_memory(), slot->mr);
        object_unref(OBJECT(slot->mr));
        g_free(slot->mr);
        slot->mr = NULL;
        slot->used = false;
    }

    if (m->memory_size) {
        name = g_strdup_printf("qvm-slot%u", m->slot);
        mr = g_new0(MemoryRegion, 1);
        memory_region_init_ram_ptr(mr, NULL, name, m->memory_size,
                                   (void *)(uintptr_t)m->userspace_addr);
        if (m->flags & KVM_MEM_READONLY) {
            memory_region_set_readonly(mr, true);
        }
        memory_region_add_subregion(get_system_memory(), m->guest_phys_addr,
                                    mr);
        slot->used = true;
        slot->mr = mr;
        slot->flags = m->flags;
        slot->guest_phys_addr = m->guest_phys_addr;
        slot->memory_size = m->memory_size;
        slot->userspace_addr = m->userspace_addr;
    }
    bql_unlock();

    return 0;
}

int qvm_vm_ioctl(QvmVM *vm, unsigned long request, uintptr_t arg)
{
    switch (request) {
    case KVM_SET_USER_MEMORY_REGION: {
        struct kvm_userspace_memory_region m;

        if (!arg) {
            return qvm_err(EFAULT);
        }
        memcpy(&m, (void *)arg, sizeof(m));
        return qvm_set_memory_region(vm, &m);
    }

    case KVM_CREATE_VCPU: {
        QvmVcpu *vcpu;
        int ret = qvm_vcpu_create(vm, arg, &vcpu);

        if (ret < 0) {
            return ret;
        }
        return qvm_fd_alloc_vcpu(vcpu);
    }

    case KVM_SET_TSS_ADDR:
        /*
         * A VMX implementation detail: the guest never sees these pages under
         * TCG, but accept the request so unmodified clients keep working.
         */
        vm->tss_addr = arg;
        return 0;

    case KVM_SET_IDENTITY_MAP_ADDR:
        if (!arg) {
            return qvm_err(EFAULT);
        }
        vm->identity_map_addr = *(uint64_t *)arg;
        return 0;

    case KVM_CHECK_EXTENSION:
        return qvm_check_extension(arg);

    default:
        return qvm_err(ENOTTY);
    }
}
