/*
 * QVM's aarch64 guest backend.
 *
 * Where x86 exchanges vCPU state through a handful of big structures, ARM's
 * KVM API does it one register at a time: every value a client reads or writes
 * is addressed by a KVM_REG_ARM64 identifier through KVM_GET_ONE_REG /
 * KVM_SET_ONE_REG.  Most of this file is therefore the translation from those
 * identifiers to fields of CPUARMState -- directly for the core registers, and
 * through QEMU's own coprocessor-register table for the system registers,
 * which is the same table the guest's MRS/MSR instructions go through.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "cpu.h"
#include "cpregs.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/cputlb.h"
#include "hw/core/boards.h"
#include "hw/core/qdev.h"
#include "system/memory.h"
#include "system/qvm-hooks.h"

#include "qvm-arch.h"
#include "qvm-internal.h"

struct QvmVcpuArch
{
    /* Set once the client has run KVM_ARM_VCPU_INIT on this vCPU. */
    bool initialised;
    struct kvm_vcpu_init init;

    /*
     * Interrupt lines the client has asserted with KVM_IRQ_LINE.  QVM has no
     * in-kernel interrupt controller, so these are the CPU's own IRQ and FIQ
     * inputs, driven directly.
     */
    bool irq_level;
    bool fiq_level;
};

/* ------------------------------------------------------------------ */
/* Core registers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Core register ids address a __u32-indexed offset into struct kvm_regs.
 * Rather than reproduce that layout, ask the header for each offset and pair
 * it with the CPUARMState field holding the same thing.
 */
#define CORE_OFFSET(field) \
    (offsetof(struct kvm_regs, field) / sizeof(uint32_t))

static bool qvm_arm_core_reg(CPUARMState *env, uint64_t id, uint64_t *val,
                             bool write)
{
    uint64_t off = id & ~(KVM_REG_ARCH_MASK | KVM_REG_SIZE_MASK |
                          KVM_REG_ARM_COPROC_MASK);
    uint64_t *slot = NULL;
    unsigned i;

    /* X0..X30, then SP_EL0, which KVM calls regs.sp. */
    for (i = 0; i < 31; i++) {
        if (off == CORE_OFFSET(regs.regs[0]) +
                   i * (sizeof(uint64_t) / sizeof(uint32_t))) {
            slot = &env->xregs[i];
            break;
        }
    }

    if (!slot) {
        if (off == CORE_OFFSET(regs.sp)) {
            slot = &env->xregs[31];
        } else if (off == CORE_OFFSET(regs.pc)) {
            slot = &env->pc;
        } else if (off == CORE_OFFSET(sp_el1)) {
            slot = &env->sp_el[1];
        } else if (off == CORE_OFFSET(elr_el1)) {
            slot = &env->elr_el[1];
        } else if (off == CORE_OFFSET(regs.pstate)) {
            /* Held decomposed across several fields, not as one word. */
            if (write) {
                pstate_write(env, *val);
            } else {
                *val = pstate_read(env);
            }
            return true;
        }
    }

    if (!slot) {
        /*
         * SPSR_EL1 and the AArch32 banked copies.  KVM's spsr[] is indexed by
         * its own KVM_SPSR_* order, which is the order QEMU banks them in.
         */
        for (i = 0; i < KVM_NR_SPSR; i++) {
            if (off == CORE_OFFSET(spsr[0]) +
                       i * (sizeof(uint64_t) / sizeof(uint32_t))) {
                slot = &env->banked_spsr[i + 1];
                break;
            }
        }
    }

    if (!slot) {
        return false;
    }

    if (write) {
        *slot = *val;
    } else {
        *val = *slot;
    }
    return true;
}

/* The SIMD registers, which are 128 bits wide and live in the SVE array. */
static bool qvm_arm_fp_reg(CPUARMState *env, uint64_t id, void *data,
                           bool write)
{
    uint64_t off = id & ~(KVM_REG_ARCH_MASK | KVM_REG_SIZE_MASK |
                          KVM_REG_ARM_COPROC_MASK);
    unsigned i;

    for (i = 0; i < 32; i++) {
        if (off == CORE_OFFSET(fp_regs.vregs[0]) +
                   i * (sizeof(uint64_t) * 2 / sizeof(uint32_t))) {
            uint64_t *q = &env->vfp.zregs[i].d[0];

            if (write) {
                memcpy(q, data, 16);
            } else {
                memcpy(data, q, 16);
            }
            return true;
        }
    }

    if (off == CORE_OFFSET(fp_regs.fpsr)) {
        if (write) {
            vfp_set_fpsr(env, *(uint32_t *)data);
        } else {
            *(uint32_t *)data = vfp_get_fpsr(env);
        }
        return true;
    }
    if (off == CORE_OFFSET(fp_regs.fpcr)) {
        if (write) {
            vfp_set_fpcr(env, *(uint32_t *)data);
        } else {
            *(uint32_t *)data = vfp_get_fpcr(env);
        }
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* System registers                                                   */
/* ------------------------------------------------------------------ */

/*
 * A KVM_REG_ARM64_SYSREG id carries the same op0/op1/crn/crm/op2 encoding the
 * guest's MRS and MSR instructions use, so it can be looked up in the very
 * table those instructions go through.
 */
static const ARMCPRegInfo *qvm_arm_sysreg(ARMCPU *cpu, uint64_t id)
{
    uint32_t op0, op1, crn, crm, op2, key;

    op0 = (id & KVM_REG_ARM64_SYSREG_OP0_MASK) >>
          KVM_REG_ARM64_SYSREG_OP0_SHIFT;
    op1 = (id & KVM_REG_ARM64_SYSREG_OP1_MASK) >>
          KVM_REG_ARM64_SYSREG_OP1_SHIFT;
    crn = (id & KVM_REG_ARM64_SYSREG_CRN_MASK) >>
          KVM_REG_ARM64_SYSREG_CRN_SHIFT;
    crm = (id & KVM_REG_ARM64_SYSREG_CRM_MASK) >>
          KVM_REG_ARM64_SYSREG_CRM_SHIFT;
    op2 = (id & KVM_REG_ARM64_SYSREG_OP2_MASK) >>
          KVM_REG_ARM64_SYSREG_OP2_SHIFT;

    key = ENCODE_AA64_CP_REG(op0, op1, crn, crm, op2);
    return get_arm_cp_reginfo(cpu->cp_regs, key);
}

/* ------------------------------------------------------------------ */
/* ONE_REG                                                            */
/* ------------------------------------------------------------------ */

static unsigned qvm_arm_reg_bytes(uint64_t id)
{
    switch (id & KVM_REG_SIZE_MASK) {
    case KVM_REG_SIZE_U8:   return 1;
    case KVM_REG_SIZE_U16:  return 2;
    case KVM_REG_SIZE_U32:  return 4;
    case KVM_REG_SIZE_U64:  return 8;
    case KVM_REG_SIZE_U128: return 16;
    default:                return 0;
    }
}

static int qvm_arm_one_reg(QvmVcpu *vcpu, const struct kvm_one_reg *reg,
                           bool write)
{
    ARMCPU *cpu = ARM_CPU(vcpu->cs);
    CPUARMState *env = &cpu->env;
    unsigned size = qvm_arm_reg_bytes(reg->id);
    const ARMCPRegInfo *ri;
    uint64_t val = 0;

    if (!size || !reg->addr) {
        return qvm_err(EINVAL);
    }
    if ((reg->id & KVM_REG_ARCH_MASK) != KVM_REG_ARM64) {
        return qvm_err(EINVAL);
    }

    switch (reg->id & KVM_REG_ARM_COPROC_MASK) {
    case KVM_REG_ARM_CORE:
        if (size == 16) {
            uint8_t buf[16];

            if (write) {
                memcpy(buf, (void *)(uintptr_t)reg->addr, 16);
            }
            if (!qvm_arm_fp_reg(env, reg->id, buf, write)) {
                return qvm_err(EINVAL);
            }
            if (!write) {
                memcpy((void *)(uintptr_t)reg->addr, buf, 16);
            }
            return 0;
        }

        if (qvm_arm_fp_reg(env, reg->id, &val, write)) {
            /* fpsr/fpcr, which are 32 bits wide. */
            if (!write) {
                memcpy((void *)(uintptr_t)reg->addr, &val, size);
            }
            return 0;
        }

        if (write) {
            memcpy(&val, (void *)(uintptr_t)reg->addr, size);
        }
        if (!qvm_arm_core_reg(env, reg->id, &val, write)) {
            return qvm_err(EINVAL);
        }
        if (!write) {
            memcpy((void *)(uintptr_t)reg->addr, &val, size);
        }
        return 0;

    case KVM_REG_ARM64_SYSREG:
        ri = qvm_arm_sysreg(cpu, reg->id);
        if (!ri) {
            return qvm_err(EINVAL);
        }
        if (write) {
            memcpy(&val, (void *)(uintptr_t)reg->addr, size);
            bql_lock();
            raw_write(env, ri, val);
            bql_unlock();
        } else {
            bql_lock();
            val = read_raw_cp_reg(env, ri);
            bql_unlock();
            memcpy((void *)(uintptr_t)reg->addr, &val, size);
        }
        return 0;

    default:
        return qvm_err(EINVAL);
    }
}

/*
 * KVM_GET_REG_LIST tells a client which registers it may ask for.  QVM
 * advertises the core set: everything a client needs to describe an AArch64
 * context, without the long tail of implementation-defined system registers
 * whose values QEMU derives rather than stores.
 */
static int qvm_arm_get_reg_list(QvmVcpu *vcpu, struct kvm_reg_list *list)
{
    uint64_t ids[64];
    uint64_t n = 0;
    unsigned i;

    for (i = 0; i < 31; i++) {
        ids[n++] = KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
                   (CORE_OFFSET(regs.regs[0]) + i * 2);
    }
    ids[n++] = KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
               CORE_OFFSET(regs.sp);
    ids[n++] = KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
               CORE_OFFSET(regs.pc);
    ids[n++] = KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
               CORE_OFFSET(regs.pstate);
    ids[n++] = KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
               CORE_OFFSET(sp_el1);
    ids[n++] = KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
               CORE_OFFSET(elr_el1);
    for (i = 0; i < KVM_NR_SPSR; i++) {
        ids[n++] = KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
                   (CORE_OFFSET(spsr[0]) + i * 2);
    }

    if (list->n < n) {
        list->n = n;
        return qvm_err(E2BIG);
    }
    memcpy(list->reg, ids, n * sizeof(ids[0]));
    list->n = n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Architecture interface                                             */
/* ------------------------------------------------------------------ */

const char *
qvm_arch_name(void)
{
    return "aarch64";
}

const char *
qvm_arch_default_cpu(void)
{
    return "max";
}

void
qvm_arch_machine_class_init(MachineClass *mc)
{
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("max");
}

bool
qvm_arch_has_pio(void)
{
    /* ARM has no port I/O space; everything the guest touches is memory. */
    return false;
}

void
qvm_arch_install_hooks(void)
{
    /*
     * The x86 hooks have no counterpart here: interrupts arrive on the CPU's
     * IRQ and FIQ inputs rather than as a vector fetched at delivery time, and
     * there is no CPUID for a client to override.
     */
}

int
qvm_arch_vcpu_realize(QvmVM *vm, int id, CPUState **csp)
{
    MachineState *ms;
    Object *obj;
    Error *err = NULL;

    bql_lock();
    ms = MACHINE(qdev_get_machine());
    obj = object_new(ms->cpu_type);

    object_property_set_int(obj, "mp-affinity", id, &error_abort);

    /*
     * The client resets and starts vCPUs itself through the KVM API, so QEMU
     * must not arrange any of that: no PSCI, and no secondary CPU left parked
     * waiting for a start call that will never come.
     */
    if (object_property_find(obj, "start-powered-off")) {
        object_property_set_bool(obj, "start-powered-off", false,
                                 &error_abort);
    }
    if (object_property_find(obj, "psci-conduit")) {
        object_property_set_int(obj, "psci-conduit", 0, &error_abort);
    }

    if (!qdev_realize(DEVICE(obj), NULL, &err)) {
        error_report_err(err);
        object_unref(obj);
        bql_unlock();
        return qvm_err(EINVAL);
    }
    bql_unlock();

    *csp = CPU(obj);
    return 0;
}

void
qvm_arch_vcpu_init_state(QvmVcpu *vcpu)
{
    vcpu->arch = g_new0(QvmVcpuArch, 1);
}

void
qvm_arch_prepare_run(QvmVcpu *vcpu)
{
}

void
qvm_arch_update_run_state(QvmVcpu *vcpu)
{
    CPUARMState *env = &ARM_CPU(vcpu->cs)->env;

    /*
     * kvm_run's interrupt fields are x86's; on ARM a client drives the CPU's
     * interrupt inputs instead and reads the masks out of PSTATE.  Report
     * something honest for if_flag rather than leaving it stale.
     */
    vcpu->run->if_flag = !(pstate_read(env) & PSTATE_I);
    vcpu->run->ready_for_interrupt_injection = vcpu->run->if_flag;
}

int
qvm_arch_check_extension(unsigned long cap)
{
    switch (cap) {
    case KVM_CAP_ONE_REG:
    case KVM_CAP_ARM_PSCI:
        return 1;
    default:
        return -1;
    }
}

int
qvm_arch_sys_ioctl(unsigned long request, uintptr_t arg)
{
    return qvm_err(ENOTTY);
}

int
qvm_arch_vm_ioctl(QvmVM *vm, unsigned long request, uintptr_t arg)
{
    switch (request) {
    case KVM_ARM_PREFERRED_TARGET: {
        struct kvm_vcpu_init init;

        if (!arg) {
            return qvm_err(EFAULT);
        }
        memset(&init, 0, sizeof(init));
        /*
         * The generic target: QVM emulates whatever CPU model QEMU was asked
         * for, and does not pretend to be one of KVM's fixed implementations.
         */
        init.target = KVM_ARM_TARGET_GENERIC_V8;
        memcpy((void *)arg, &init, sizeof(init));
        return 0;
    }

    case KVM_IRQ_LINE: {
        struct kvm_irq_level level;
        QvmVcpu *vcpu;
        unsigned type, vcpu_idx, num;

        if (!arg) {
            return qvm_err(EFAULT);
        }
        memcpy(&level, (void *)arg, sizeof(level));

        type = (level.irq & KVM_ARM_IRQ_TYPE_MASK) >> KVM_ARM_IRQ_TYPE_SHIFT;
        vcpu_idx = (level.irq & KVM_ARM_IRQ_VCPU_MASK) >>
                   KVM_ARM_IRQ_VCPU_SHIFT;
        num = (level.irq & KVM_ARM_IRQ_NUM_MASK) >> KVM_ARM_IRQ_NUM_SHIFT;

        if (type != KVM_ARM_IRQ_TYPE_CPU || vcpu_idx >= QVM_MAX_VCPUS) {
            /*
             * SPIs and PPIs are the interrupt controller's business, and QVM
             * has none: the client is expected to own it and drive the CPU
             * lines directly.
             */
            return qvm_err(EINVAL);
        }

        vcpu = vm->vcpus[vcpu_idx];
        if (!vcpu) {
            return qvm_err(EINVAL);
        }

        bql_lock();
        if (num == KVM_ARM_IRQ_CPU_IRQ) {
            vcpu->arch->irq_level = level.level;
            if (level.level) {
                cpu_interrupt(vcpu->cs, CPU_INTERRUPT_HARD);
            } else {
                cpu_reset_interrupt(vcpu->cs, CPU_INTERRUPT_HARD);
            }
        } else if (num == KVM_ARM_IRQ_CPU_FIQ) {
            vcpu->arch->fiq_level = level.level;
            if (level.level) {
                cpu_interrupt(vcpu->cs, CPU_INTERRUPT_FIQ);
            } else {
                cpu_reset_interrupt(vcpu->cs, CPU_INTERRUPT_FIQ);
            }
        } else {
            bql_unlock();
            return qvm_err(EINVAL);
        }
        bql_unlock();
        return 0;
    }

    default:
        return qvm_err(ENOTTY);
    }
}

int
qvm_arch_vcpu_ioctl(QvmVcpu *vcpu, unsigned long request, uintptr_t arg)
{
    switch (request) {
    case KVM_ARM_VCPU_INIT: {
        struct kvm_vcpu_init init;

        if (!arg) {
            return qvm_err(EFAULT);
        }
        memcpy(&init, (void *)arg, sizeof(init));

        if (init.target != KVM_ARM_TARGET_GENERIC_V8 &&
            init.target != (uint32_t)-1) {
            return qvm_err(EINVAL);
        }

        vcpu->arch->init = init;
        vcpu->arch->initialised = true;

        bql_lock();
        cpu_reset(vcpu->cs);
        bql_unlock();
        return 0;
    }

    case KVM_GET_ONE_REG:
    case KVM_SET_ONE_REG: {
        struct kvm_one_reg reg;

        if (!arg) {
            return qvm_err(EFAULT);
        }
        memcpy(&reg, (void *)arg, sizeof(reg));
        return qvm_arm_one_reg(vcpu, &reg, request == KVM_SET_ONE_REG);
    }

    case KVM_GET_REG_LIST:
        if (!arg) {
            return qvm_err(EFAULT);
        }
        return qvm_arm_get_reg_list(vcpu, (struct kvm_reg_list *)arg);

    default:
        return qvm_err(ENOTTY);
    }
}
