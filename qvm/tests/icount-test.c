/*
 * Instruction counting and budgeting.
 *
 * QVM translates its guest, so it can say exactly how many instructions a
 * vCPU has run and stop it after a given number -- neither of which KVM can
 * do, and neither of which costs the round-robin scheduling that QEMU's own
 * icount mode would impose.
 *
 * The guest here is three instructions of straight-line code followed by a
 * two-instruction loop, so the expected count and the expected register value
 * can both be worked out by hand.  Budgets are chosen to land in the middle
 * of the loop, which is the case that forces a block to be re-translated to
 * exactly the length left.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include <linux/kvm.h>

#include <qvm/qvm.h>

extern const unsigned char icount_guest[], icount_guest_end[];

#define GUEST_PHYS  0x0UL
#define GUEST_SIZE  0x10000UL

static unsigned checks, failures;

static void checkf(bool ok, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void checkf(bool ok, const char *fmt, ...)
{
    va_list ap;

    checks++;
    printf(ok ? "  ok    " : "  FAIL  ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (!ok) {
        failures++;
    }
}

int main(void)
{
    int sys, vm, vcpu, ret;
    struct kvm_run *run;
    struct kvm_sregs sregs;
    struct kvm_regs regs;
    struct kvm_userspace_memory_region region;
    unsigned long long insns;
    void *mem;

    printf("QVM instruction counting\n\n");

    sys = qvm_open("/dev/kvm", 0);
    vm = qvm_ioctl(sys, KVM_CREATE_VM, 0);
    vcpu = qvm_ioctl(vm, KVM_CREATE_VCPU, 0);
    if (sys < 0 || vm < 0 || vcpu < 0) {
        printf("  FAIL  could not create the VM\n");
        return 1;
    }

    ret = qvm_ioctl(sys, KVM_GET_VCPU_MMAP_SIZE);
    run = qvm_mmap(NULL, ret, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);

    mem = qvm_mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (run == MAP_FAILED || mem == MAP_FAILED) {
        printf("  FAIL  could not map the VM\n");
        return 1;
    }
    memcpy(mem, icount_guest, icount_guest_end - icount_guest);

    memset(&region, 0, sizeof(region));
    region.slot = 0;
    region.guest_phys_addr = GUEST_PHYS;
    region.memory_size = GUEST_SIZE;
    region.userspace_addr = (uint64_t)(uintptr_t)mem;
    qvm_ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region);

    /* Real mode at physical zero: no descriptor tables involved. */
    qvm_ioctl(vcpu, KVM_GET_SREGS, &sregs);
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    sregs.cs.limit = 0xffff;
    sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = sregs.cs;
    qvm_ioctl(vcpu, KVM_SET_SREGS, &sregs);

    memset(&regs, 0, sizeof(regs));
    regs.rip = 0;
    regs.rflags = 2;
    qvm_ioctl(vcpu, KVM_SET_REGS, &regs);

    checkf(qvm_vcpu_insns(vcpu, &insns) == 0 && insns == 0,
           "a fresh vCPU has retired no instructions");

    /*
     * 1000 is deliberately not a whole number of loop iterations past the
     * three-instruction preamble: 3 + 498*2 = 999, so the last instruction
     * has to come from a block translated to length one.
     */
    printf("\nA budget that ends mid-loop\n");

    checkf(qvm_vcpu_set_insn_budget(vcpu, 1000) == 0, "set a budget of 1000");

    ret = qvm_ioctl(vcpu, KVM_RUN, 0);
    checkf(ret == 0, "KVM_RUN");
    checkf(run->exit_reason == KVM_EXIT_INTR,
           "the run ends with KVM_EXIT_INTR (got %u)", run->exit_reason);

    checkf(qvm_vcpu_insns(vcpu, &insns) == 0 && insns == 1000,
           "exactly 1000 instructions retired (got %llu)", insns);

    qvm_ioctl(vcpu, KVM_GET_REGS, &regs);
    checkf((regs.rax & 0xffff) == 500,
           "the guest incremented ax 500 times (got %llu)",
           (unsigned long long)(regs.rax & 0xffff));

    /* The guest must resume exactly where the budget stopped it. */
    printf("\nResuming for a further 7\n");

    checkf(qvm_vcpu_set_insn_budget(vcpu, 7) == 0, "set a budget of 7");

    ret = qvm_ioctl(vcpu, KVM_RUN, 0);
    checkf(ret == 0 && run->exit_reason == KVM_EXIT_INTR,
           "the run ends with KVM_EXIT_INTR");

    checkf(qvm_vcpu_insns(vcpu, &insns) == 0 && insns == 1007,
           "the count continues to 1007 (got %llu)", insns);

    qvm_ioctl(vcpu, KVM_GET_REGS, &regs);
    checkf((regs.rax & 0xffff) == 503,
           "ax advanced by exactly 3 more (got %llu)",
           (unsigned long long)(regs.rax & 0xffff));

    printf("\n%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
