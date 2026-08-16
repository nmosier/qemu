/*
 * A TCG plugin stopping the guest.
 *
 * Plugins can already watch everything the guest does; this checks the one
 * thing they can make it do.  The guest here never stops on its own -- it is
 * an endless loop -- so if the run returns at all, the plugin is what ended
 * it.
 *
 * The check that matters is where it stopped.  The request is honoured at the
 * end of the translated block the guest is in, so the count lands at or just
 * past the number the plugin asked for, and never further than one block's
 * worth beyond it.
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

/* CF_COUNT_MASK: the most guest instructions QEMU puts in one block. */
#define MAX_INSNS_PER_TB 512

#define HALT_AFTER 5000

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

int main(int argc, char **argv)
{
    const char *plugin = argc > 1 ? argv[1] : "./halt-plugin.dylib";
    char args[64];
    int sys, vm, vcpu, ret;
    struct kvm_run *run;
    struct kvm_sregs sregs;
    struct kvm_regs regs;
    struct kvm_userspace_memory_region region;
    unsigned long long insns;
    unsigned reason;
    void *mem;

    printf("QVM plugin-initiated halt\n\n");

    snprintf(args, sizeof(args), "insns=%u", HALT_AFTER);
    checkf(qvm_load_plugin(plugin, args) == 0,
           "load %s with %s", plugin, args);

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

    printf("\nRunning a guest that never stops by itself\n");

    ret = qvm_ioctl(vcpu, KVM_RUN, 0);
    checkf(ret == 0, "KVM_RUN returns");
    checkf(run->exit_reason == KVM_EXIT_INTR,
           "the run ends with KVM_EXIT_INTR (got %u)", run->exit_reason);

    checkf(qvm_vcpu_halt_reason(vcpu, &reason) == 0 &&
           reason == QVM_HALT_PLUGIN,
           "the halt is attributed to the plugin (got %u)", reason);

    checkf(qvm_vcpu_insns(vcpu, &insns) == 0, "read the instruction count");
    checkf(insns >= HALT_AFTER,
           "stopped at or after the %u the plugin asked for (%llu)",
           HALT_AFTER, insns);
    checkf(insns < HALT_AFTER + MAX_INSNS_PER_TB,
           "and within one translated block of it (%llu, limit %u)",
           insns, HALT_AFTER + MAX_INSNS_PER_TB);

    /*
     * The client is in charge of what happens next, so it can resume.  The
     * plugin halts only once, and the guest still never stops by itself, so
     * bound this run instead -- which also shows the two endings apart.
     */
    printf("\nThe client decides what happens next\n");

    checkf(qvm_vcpu_set_insn_budget(vcpu, 1000) == 0, "bound the next run");

    ret = qvm_ioctl(vcpu, KVM_RUN, 0);
    checkf(ret == 0, "KVM_RUN resumes after a plugin halt");
    checkf(qvm_vcpu_halt_reason(vcpu, &reason) == 0 &&
           reason == QVM_HALT_INSN_BUDGET,
           "the second run reports the budget, not a stale plugin halt "
           "(got %u)", reason);

    {
        unsigned long long after = 0;

        qvm_vcpu_insns(vcpu, &after);
        checkf(after == insns + 1000,
               "and ran exactly the 1000 it was given (%llu -> %llu)",
               insns, after);
    }

    printf("\n%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
