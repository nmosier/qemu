/*
 * QVM: translation between KVM's vCPU state structures and QEMU's CPUX86State.
 *
 * This is the bulk of what a KVM client touches between runs.  Where QEMU's
 * own KVM accelerator converts QEMU state into KVM's structures, QVM does the
 * same conversions in the opposite direction: here QEMU is the hypervisor and
 * the client is the one describing the guest.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "cpu.h"
#include "tcg/helper-tcg.h"
#include "hw/core/boards.h"
#include "hw/core/qdev.h"
#include "qapi/error.h"
#include "exec/cputlb.h"
#include "qemu/bswap.h"
#include "system/memory.h"

#include "qvm-internal.h"

/*
 * eflags bits KVM_SET_REGS is allowed to load.  The arithmetic flags are held
 * lazily in cc_src/cc_dst and DF is kept in env->df, so both are excluded here
 * and restored by cpu_load_eflags() instead.
 */
#define QVM_EFLAGS_MASK (TF_MASK | AC_MASK | ID_MASK | NT_MASK | \
                         IF_MASK | IOPL_MASK | VM_MASK | RF_MASK)

/* Bound on the CPUID table a client may install; KVM's own limit is similar. */
#define QVM_MAX_CPUID_ENTRIES 256

/* ------------------------------------------------------------------ */
/* Segments and general purpose registers                             */
/* ------------------------------------------------------------------ */

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

static void qvm_get_regs(QvmVcpu *vcpu, struct kvm_regs *regs)
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

static void qvm_set_regs(QvmVcpu *vcpu, const struct kvm_regs *regs)
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
    /*
     * An access reported by the last exit is completed by re-executing the
     * instruction that made it, so a client writing rip back unchanged -- as
     * one does when it services an exit by updating the guest's registers --
     * must not disturb that.  Only an rip that actually moves invalidates the
     * pending completion, because then a different instruction runs next.
     *
     * KVM has the same property for a different reason: it finishes the
     * access itself, from state it keeps in the vCPU, which KVM_SET_REGS does
     * not touch.
     */
    if (regs->rip != env->eip) {
        vcpu->completing_io = false;
    }

    env->eip = regs->rip;
    cpu_load_eflags(env, regs->rflags, QVM_EFLAGS_MASK);
    x86_update_hflags(env);
}

static void qvm_get_sregs(QvmVcpu *vcpu, struct kvm_sregs *sregs)
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
    sregs->cr8 = vcpu->cr8;
    sregs->efer = env->efer;
    sregs->apic_base = vcpu->apic_base;

    /*
     * interrupt_bitmap reports interrupts KVM has accepted but not yet
     * delivered.  QVM hands an injected vector straight to the guest on the
     * next entry, so there is never one queued here.
     */
}

static void qvm_set_sregs(QvmVcpu *vcpu, const struct kvm_sregs *sregs)
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

    vcpu->apic_base = sregs->apic_base;
    vcpu->cr8 = sregs->cr8;

    x86_update_hflags(env);
    tlb_flush(cs);
}

/* ------------------------------------------------------------------ */
/* Floating point and extended state                                  */
/* ------------------------------------------------------------------ */

static void qvm_get_fpu(QvmVcpu *vcpu, struct kvm_fpu *fpu)
{
    CPUX86State *env = cpu_env(vcpu->cs);
    int i;

    memset(fpu, 0, sizeof(*fpu));

    fpu->fsw = env->fpus & ~(7 << 11);
    fpu->fsw |= (env->fpstt & 7) << 11;
    fpu->fcw = env->fpuc;
    fpu->last_opcode = env->fpop;
    fpu->last_ip = env->fpip;
    fpu->last_dp = env->fpdp;
    for (i = 0; i < 8; ++i) {
        fpu->ftwx |= (!env->fptags[i]) << i;
    }
    memcpy(fpu->fpr, env->fpregs, sizeof(fpu->fpr));
    for (i = 0; i < CPU_NB_REGS; i++) {
        stq_p(&fpu->xmm[i][0], env->xmm_regs[i].ZMM_Q(0));
        stq_p(&fpu->xmm[i][8], env->xmm_regs[i].ZMM_Q(1));
    }
    fpu->mxcsr = env->mxcsr;
}

static void qvm_set_fpu(QvmVcpu *vcpu, const struct kvm_fpu *fpu)
{
    CPUX86State *env = cpu_env(vcpu->cs);
    int i;

    env->fpstt = (fpu->fsw >> 11) & 7;
    env->fpus = fpu->fsw;
    env->fpuc = fpu->fcw;
    env->fpop = fpu->last_opcode;
    env->fpip = fpu->last_ip;
    env->fpdp = fpu->last_dp;
    for (i = 0; i < 8; ++i) {
        env->fptags[i] = !((fpu->ftwx >> i) & 1);
    }
    memcpy(env->fpregs, fpu->fpr, sizeof(env->fpregs));
    for (i = 0; i < CPU_NB_REGS; i++) {
        env->xmm_regs[i].ZMM_Q(0) = ldq_p(&fpu->xmm[i][0]);
        env->xmm_regs[i].ZMM_Q(1) = ldq_p(&fpu->xmm[i][8]);
    }
    env->mxcsr = fpu->mxcsr;

    update_fp_status(env);
    update_mxcsr_status(env);
}

/*
 * KVM's struct kvm_xsave is the raw XSAVE area, which is exactly what QEMU's
 * xsave helpers produce and consume.
 */
static int qvm_get_xsave(QvmVcpu *vcpu, struct kvm_xsave *xsave)
{
    memset(xsave, 0, sizeof(*xsave));
    x86_cpu_xsave_all_areas(env_archcpu(cpu_env(vcpu->cs)),
                            xsave->region, sizeof(xsave->region));
    return 0;
}

static int qvm_set_xsave(QvmVcpu *vcpu, const struct kvm_xsave *xsave)
{
    x86_cpu_xrstor_all_areas(env_archcpu(cpu_env(vcpu->cs)),
                             xsave->region, sizeof(xsave->region));
    return 0;
}

static void qvm_get_xcrs(QvmVcpu *vcpu, struct kvm_xcrs *xcrs)
{
    CPUX86State *env = cpu_env(vcpu->cs);

    memset(xcrs, 0, sizeof(*xcrs));
    xcrs->nr_xcrs = 1;
    xcrs->xcrs[0].xcr = 0;
    xcrs->xcrs[0].value = env->xcr0;
}

static void qvm_set_xcrs(QvmVcpu *vcpu, const struct kvm_xcrs *xcrs)
{
    CPUX86State *env = cpu_env(vcpu->cs);
    uint32_t i;

    for (i = 0; i < xcrs->nr_xcrs && i < KVM_MAX_XCRS; i++) {
        if (xcrs->xcrs[i].xcr == 0) {
            env->xcr0 = xcrs->xcrs[i].value;
            break;
        }
    }
    cpu_sync_avx_hflag(env);
}

static void qvm_get_debugregs(QvmVcpu *vcpu, struct kvm_debugregs *dregs)
{
    CPUX86State *env = cpu_env(vcpu->cs);
    int i;

    memset(dregs, 0, sizeof(*dregs));
    for (i = 0; i < 4; i++) {
        dregs->db[i] = env->dr[i];
    }
    dregs->dr6 = env->dr[6];
    dregs->dr7 = env->dr[7];
}

static void qvm_set_debugregs(QvmVcpu *vcpu, const struct kvm_debugregs *dregs)
{
    CPUX86State *env = cpu_env(vcpu->cs);
    int i;

    for (i = 0; i < 4; i++) {
        env->dr[i] = dregs->db[i];
    }
    env->dr[6] = dregs->dr6;
    /*
     * Route dr7 through the update helper so the breakpoints it describes are
     * actually installed in the CPU rather than only recorded.
     */
    env->dr[7] = dregs->dr7 & ~(DR7_GLOBAL_BP_MASK | DR7_LOCAL_BP_MASK);
    cpu_x86_update_dr7(env, dregs->dr7);
}

/* ------------------------------------------------------------------ */
/* Pending events                                                     */
/* ------------------------------------------------------------------ */

static void qvm_get_vcpu_events(QvmVcpu *vcpu, struct kvm_vcpu_events *events)
{
    CPUX86State *env = cpu_env(vcpu->cs);

    memset(events, 0, sizeof(*events));

    events->exception.injected = env->exception_injected;
    events->exception.nr = env->exception_nr;
    events->exception.has_error_code = env->has_error_code;
    events->exception.error_code = env->error_code;

    events->interrupt.injected = vcpu->irq_injected;
    events->interrupt.nr = vcpu->irq_vector;
    events->interrupt.shadow = (env->hflags & HF_INHIBIT_IRQ_MASK) != 0;

    events->nmi.pending = vcpu->nmi_pending;
    events->nmi.masked = (env->hflags2 & HF2_NMI_MASK) != 0;

    events->sipi_vector = env->sipi_vector;
}

static void qvm_set_vcpu_events(QvmVcpu *vcpu,
                                const struct kvm_vcpu_events *events)
{
    CPUX86State *env = cpu_env(vcpu->cs);

    env->exception_injected = events->exception.injected;
    env->exception_nr = events->exception.nr;
    env->has_error_code = events->exception.has_error_code;
    env->error_code = events->exception.error_code;

    if (events->interrupt.injected) {
        vcpu->irq_pending = true;
        vcpu->irq_vector = events->interrupt.nr;
        cpu_interrupt(vcpu->cs, CPU_INTERRUPT_HARD);
    }

    vcpu->nmi_pending = events->nmi.pending;
    if (events->nmi.pending) {
        cpu_interrupt(vcpu->cs, CPU_INTERRUPT_NMI);
    }
    if (events->nmi.masked) {
        env->hflags2 |= HF2_NMI_MASK;
    } else {
        env->hflags2 &= ~HF2_NMI_MASK;
    }

    env->sipi_vector = events->sipi_vector;
}

/* ------------------------------------------------------------------ */
/* Model specific registers                                           */
/* ------------------------------------------------------------------ */

/*
 * The MSRs QVM can translate to and from CPUX86State.  A client discovers this
 * set with KVM_GET_MSR_INDEX_LIST and, like gem5, intersects it with whatever
 * its own CPU model knows about, so advertising only what is really backed
 * here keeps that intersection honest.
 */
static const uint32_t qvm_msr_list[] = {
    MSR_IA32_TSC,
    MSR_IA32_APICBASE,
    MSR_MTRRcap,
    MSR_IA32_SYSENTER_CS,
    MSR_IA32_SYSENTER_ESP,
    MSR_IA32_SYSENTER_EIP,
    MSR_MCG_CAP,
    MSR_MCG_STATUS,
    MSR_MCG_CTL,
    MSR_MTRRphysBase(0), MSR_MTRRphysMask(0),
    MSR_MTRRphysBase(1), MSR_MTRRphysMask(1),
    MSR_MTRRphysBase(2), MSR_MTRRphysMask(2),
    MSR_MTRRphysBase(3), MSR_MTRRphysMask(3),
    MSR_MTRRphysBase(4), MSR_MTRRphysMask(4),
    MSR_MTRRphysBase(5), MSR_MTRRphysMask(5),
    MSR_MTRRphysBase(6), MSR_MTRRphysMask(6),
    MSR_MTRRphysBase(7), MSR_MTRRphysMask(7),
    MSR_MTRRfix64K_00000,
    MSR_MTRRfix16K_80000, MSR_MTRRfix16K_A0000,
    MSR_MTRRfix4K_C0000, MSR_MTRRfix4K_C8000,
    MSR_MTRRfix4K_D0000, MSR_MTRRfix4K_D8000,
    MSR_MTRRfix4K_E0000, MSR_MTRRfix4K_E8000,
    MSR_MTRRfix4K_F0000, MSR_MTRRfix4K_F8000,
    MSR_PAT,
    MSR_MTRRdefType,
    MSR_EFER,
    MSR_STAR,
    MSR_LSTAR,
    MSR_CSTAR,
    MSR_FMASK,
    MSR_FSBASE,
    MSR_GSBASE,
    MSR_KERNELGSBASE,
    MSR_TSC_AUX,
};

static uint64_t *qvm_msr_slot(CPUX86State *env, uint32_t index)
{
    switch (index) {
    case MSR_PAT:               return &env->pat;
    case MSR_MTRRdefType:       return &env->mtrr_deftype;
    case MSR_MCG_STATUS:        return &env->mcg_status;
    case MSR_MCG_CTL:           return &env->mcg_ctl;
    case MSR_MCG_CAP:           return &env->mcg_cap;
    case MSR_STAR:              return &env->star;
    case MSR_LSTAR:             return &env->lstar;
    case MSR_CSTAR:             return &env->cstar;
    case MSR_FMASK:             return &env->fmask;
    case MSR_KERNELGSBASE:      return &env->kernelgsbase;
    case MSR_TSC_AUX:           return &env->tsc_aux;
    case MSR_MTRRfix64K_00000:  return &env->mtrr_fixed[0];
    case MSR_MTRRfix16K_80000:
    case MSR_MTRRfix16K_A0000:
        return &env->mtrr_fixed[index - MSR_MTRRfix16K_80000 + 1];
    case MSR_MTRRfix4K_C0000 ... MSR_MTRRfix4K_F8000:
        return &env->mtrr_fixed[index - MSR_MTRRfix4K_C0000 + 3];
    default:
        break;
    }

    if (index >= MSR_MTRRphysBase(0) && index <= MSR_MTRRphysMask(7)) {
        MTRRVar *var = &env->mtrr_var[MSR_MTRRphysIndex(index)];
        return (index & 1) ? &var->mask : &var->base;
    }
    return NULL;
}

static bool qvm_msr_read(QvmVcpu *vcpu, uint32_t index, uint64_t *val)
{
    CPUX86State *env = cpu_env(vcpu->cs);
    uint64_t *slot;

    switch (index) {
    case MSR_IA32_TSC:
        *val = cpu_get_tsc(env) + env->tsc_offset;
        return true;
    case MSR_IA32_APICBASE:
        *val = vcpu->apic_base;
        return true;
    case MSR_MTRRcap:
        *val = MSR_MTRRcap_VCNT | MSR_MTRRcap_FIXRANGE_SUPPORT |
               MSR_MTRRcap_WC_SUPPORTED;
        return true;
    case MSR_EFER:
        *val = env->efer;
        return true;
    case MSR_IA32_SYSENTER_CS:
        *val = env->sysenter_cs;
        return true;
    case MSR_IA32_SYSENTER_ESP:
        *val = env->sysenter_esp;
        return true;
    case MSR_IA32_SYSENTER_EIP:
        *val = env->sysenter_eip;
        return true;
    case MSR_FSBASE:
        *val = env->segs[R_FS].base;
        return true;
    case MSR_GSBASE:
        *val = env->segs[R_GS].base;
        return true;
    default:
        break;
    }

    slot = qvm_msr_slot(env, index);
    if (slot) {
        *val = *slot;
        return true;
    }
    return false;
}

static bool qvm_msr_write(QvmVcpu *vcpu, uint32_t index, uint64_t val)
{
    CPUX86State *env = cpu_env(vcpu->cs);
    uint64_t *slot;

    switch (index) {
    case MSR_IA32_TSC:
        env->tsc_offset = val - cpu_get_tsc(env);
        return true;
    case MSR_IA32_APICBASE:
        vcpu->apic_base = val;
        return true;
    case MSR_MTRRcap:
        /* Read-only; accept and ignore, as the hardware does. */
        return true;
    case MSR_EFER:
        cpu_load_efer(env, val);
        return true;
    case MSR_IA32_SYSENTER_CS:
        env->sysenter_cs = val;
        return true;
    case MSR_IA32_SYSENTER_ESP:
        env->sysenter_esp = val;
        return true;
    case MSR_IA32_SYSENTER_EIP:
        env->sysenter_eip = val;
        return true;
    case MSR_FSBASE:
        env->segs[R_FS].base = val;
        return true;
    case MSR_GSBASE:
        env->segs[R_GS].base = val;
        return true;
    default:
        break;
    }

    slot = qvm_msr_slot(env, index);
    if (slot) {
        *slot = val;
        return true;
    }
    return false;
}

int qvm_x86_get_msr_index_list(struct kvm_msr_list *list)
{
    uint32_t n = ARRAY_SIZE(qvm_msr_list);

    if (list->nmsrs < n) {
        list->nmsrs = n;
        return qvm_err(E2BIG);
    }
    memcpy(list->indices, qvm_msr_list, sizeof(qvm_msr_list));
    list->nmsrs = n;
    return 0;
}

/*
 * KVM_GET_MSRS/KVM_SET_MSRS return the number of entries handled rather than
 * failing on the first one they do not recognise, so a client can probe.
 */
static int qvm_get_msrs(QvmVcpu *vcpu, struct kvm_msrs *msrs)
{
    uint32_t i;

    for (i = 0; i < msrs->nmsrs; i++) {
        if (!qvm_msr_read(vcpu, msrs->entries[i].index,
                          &msrs->entries[i].data)) {
            break;
        }
    }
    return i;
}

static int qvm_set_msrs(QvmVcpu *vcpu, const struct kvm_msrs *msrs)
{
    uint32_t i;

    for (i = 0; i < msrs->nmsrs; i++) {
        if (!qvm_msr_write(vcpu, msrs->entries[i].index,
                           msrs->entries[i].data)) {
            break;
        }
    }
    return i;
}

/* ------------------------------------------------------------------ */
/* CPUID                                                              */
/* ------------------------------------------------------------------ */

static int qvm_set_cpuid2(QvmVcpu *vcpu, const struct kvm_cpuid2 *cpuid)
{
    struct kvm_cpuid_entry2 *table;

    if (cpuid->nent > QVM_MAX_CPUID_ENTRIES) {
        return qvm_err(E2BIG);
    }

    table = g_new0(struct kvm_cpuid_entry2, cpuid->nent ?: 1);
    memcpy(table, cpuid->entries, cpuid->nent * sizeof(*table));

    /*
     * Publish the table only once it is fully built: the guest may be reading
     * CPUID through qvm_cpuid() on another thread.
     */
    bql_lock();
    g_free(vcpu->cpuid);
    vcpu->cpuid = table;
    vcpu->cpuid_nent = cpuid->nent;
    bql_unlock();

    /* Translated code may have inlined CPUID results for this vCPU. */
    tlb_flush(vcpu->cs);
    return 0;
}

static int qvm_get_cpuid2(QvmVcpu *vcpu, struct kvm_cpuid2 *cpuid)
{
    if (cpuid->nent < vcpu->cpuid_nent) {
        cpuid->nent = vcpu->cpuid_nent;
        return qvm_err(E2BIG);
    }
    memcpy(cpuid->entries, vcpu->cpuid,
           vcpu->cpuid_nent * sizeof(*cpuid->entries));
    cpuid->nent = vcpu->cpuid_nent;
    return 0;
}

bool qvm_cpuid(CPUState *cs, uint32_t function, uint32_t index,
               uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    QvmVcpu *vcpu = qvm_vcpu_of(cs);
    const struct kvm_cpuid_entry2 *entry;
    uint32_t i;

    if (!vcpu || !vcpu->cpuid) {
        return false;
    }

    for (i = 0; i < vcpu->cpuid_nent; i++) {
        entry = &vcpu->cpuid[i];
        if (entry->function != function) {
            continue;
        }
        if ((entry->flags & KVM_CPUID_FLAG_SIGNIFCANT_INDEX) &&
            entry->index != index) {
            continue;
        }
        *eax = entry->eax;
        *ebx = entry->ebx;
        *ecx = entry->ecx;
        *edx = entry->edx;
        return true;
    }

    /*
     * The client's table is authoritative: a leaf it did not describe reads as
     * zero rather than falling back to the CPU model QEMU was configured with,
     * which the guest was never told about.
     */
    *eax = *ebx = *ecx = *edx = 0;
    return true;
}

/*
 * KVM_GET_SUPPORTED_CPUID describes what the hypervisor can emulate, which
 * here means what QEMU's configured CPU model reports.
 *
 * Unlike KVM, QVM cannot answer this before a vCPU exists: a CPU model's
 * feature words and cache descriptions are only filled in when one is
 * realized, and there is no host CPU to fall back on.  Clients that ask early
 * get EAGAIN.  gem5 never calls this -- X86KvmCPU::updateCPUID() builds its
 * table from gem5's own ISA model -- so the ordering costs it nothing.
 */
int qvm_x86_get_supported_cpuid(struct kvm_cpuid2 *cpuid)
{
    /* The basic and extended leaf ranges; not a range to iterate over. */
    static const uint32_t bases[] = { 0x00000000u, 0x80000000u };
    struct kvm_cpuid_entry2 entries[QVM_MAX_CPUID_ENTRIES];
    CPUX86State *env;
    uint32_t nent = 0;
    uint32_t function, limit;
    size_t i;

    if (!first_cpu) {
        return qvm_err(EAGAIN);
    }
    env = cpu_env(first_cpu);

    for (i = 0; i < ARRAY_SIZE(bases); i++) {
        uint32_t base = bases[i];
        uint32_t unused;

        cpu_x86_cpuid(env, base, 0, &limit, &unused, &unused, &unused);
        if (limit < base || limit - base > 0x100) {
            continue;
        }
        for (function = base;
             function <= limit && nent < ARRAY_SIZE(entries);
             function++) {
            memset(&entries[nent], 0, sizeof(entries[nent]));
            entries[nent].function = function;
            cpu_x86_cpuid(env, function, 0,
                          &entries[nent].eax, &entries[nent].ebx,
                          &entries[nent].ecx, &entries[nent].edx);
            nent++;
        }
    }

    if (cpuid->nent < nent) {
        cpuid->nent = nent;
        return qvm_err(E2BIG);
    }
    memcpy(cpuid->entries, entries, nent * sizeof(entries[0]));
    cpuid->nent = nent;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */

/*
 * Copy through a local struct rather than working on the client's buffer:
 * these run concurrently with nothing, but a partially updated CPUX86State is
 * much harder to reason about than a partially updated caller buffer.
 */
#define QVM_IOCTL_GET(type, fn)                             \
    do {                                                    \
        type _v;                                            \
        if (!arg) {                                         \
            return qvm_err(EFAULT);                         \
        }                                                   \
        fn(vcpu, &_v);                                      \
        memcpy((void *)arg, &_v, sizeof(_v));               \
        return 0;                                           \
    } while (0)

#define QVM_IOCTL_SET(type, fn)                             \
    do {                                                    \
        type _v;                                            \
        if (!arg) {                                         \
            return qvm_err(EFAULT);                         \
        }                                                   \
        memcpy(&_v, (void *)arg, sizeof(_v));               \
        fn(vcpu, &_v);                                      \
        return 0;                                           \
    } while (0)

int qvm_x86_ioctl(QvmVcpu *vcpu, unsigned long request, uintptr_t arg)
{
    switch (request) {
    case KVM_GET_REGS:
        QVM_IOCTL_GET(struct kvm_regs, qvm_get_regs);
    case KVM_SET_REGS:
        QVM_IOCTL_SET(struct kvm_regs, qvm_set_regs);
    case KVM_GET_SREGS:
        QVM_IOCTL_GET(struct kvm_sregs, qvm_get_sregs);
    case KVM_SET_SREGS:
        QVM_IOCTL_SET(struct kvm_sregs, qvm_set_sregs);
    case KVM_GET_FPU:
        QVM_IOCTL_GET(struct kvm_fpu, qvm_get_fpu);
    case KVM_SET_FPU:
        QVM_IOCTL_SET(struct kvm_fpu, qvm_set_fpu);
    case KVM_GET_XCRS:
        QVM_IOCTL_GET(struct kvm_xcrs, qvm_get_xcrs);
    case KVM_SET_XCRS:
        QVM_IOCTL_SET(struct kvm_xcrs, qvm_set_xcrs);
    case KVM_GET_DEBUGREGS:
        QVM_IOCTL_GET(struct kvm_debugregs, qvm_get_debugregs);
    case KVM_SET_DEBUGREGS:
        QVM_IOCTL_SET(struct kvm_debugregs, qvm_set_debugregs);
    case KVM_GET_VCPU_EVENTS:
        QVM_IOCTL_GET(struct kvm_vcpu_events, qvm_get_vcpu_events);
    case KVM_SET_VCPU_EVENTS:
        QVM_IOCTL_SET(struct kvm_vcpu_events, qvm_set_vcpu_events);

    case KVM_GET_XSAVE:
        if (!arg) {
            return qvm_err(EFAULT);
        }
        return qvm_get_xsave(vcpu, (struct kvm_xsave *)arg);
    case KVM_SET_XSAVE:
        if (!arg) {
            return qvm_err(EFAULT);
        }
        return qvm_set_xsave(vcpu, (const struct kvm_xsave *)arg);

    case KVM_GET_MSRS:
        if (!arg) {
            return qvm_err(EFAULT);
        }
        return qvm_get_msrs(vcpu, (struct kvm_msrs *)arg);
    case KVM_SET_MSRS:
        if (!arg) {
            return qvm_err(EFAULT);
        }
        return qvm_set_msrs(vcpu, (const struct kvm_msrs *)arg);

    case KVM_SET_CPUID2:
        if (!arg) {
            return qvm_err(EFAULT);
        }
        return qvm_set_cpuid2(vcpu, (const struct kvm_cpuid2 *)arg);
    case KVM_GET_CPUID2:
        if (!arg) {
            return qvm_err(EFAULT);
        }
        return qvm_get_cpuid2(vcpu, (struct kvm_cpuid2 *)arg);

    default:
        return qvm_err(ENOTTY);
    }
}
