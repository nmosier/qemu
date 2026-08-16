/*
 * End-to-end test of QVM's AArch64 backend.
 *
 * This drives the same sequence an ARM KVM client uses to get a vCPU running:
 * create the VM, ask what target to initialise for, initialise, describe the
 * guest's memory, set the entry state, and run.  The guest then exits three
 * times for MMIO and once for a halt, which covers both directions of the
 * MMIO path -- including the read, where QVM has to restart the faulting
 * instruction and complete it from the shared page.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <linux/kvm.h>

#include <qvm/qvm.h>

/* The guest image, from arm-payload.S. */
extern const unsigned char arm_guest[], arm_guest_end[];

#define GUEST_PHYS  0x40000000UL
#define GUEST_SIZE  0x10000UL
#define MMIO_ADDR   0x09000000UL

/* EL1h, AArch64, with DAIF masked -- the state a client hands a fresh vCPU. */
#define PSTATE_EL1H 0x3c5

#define CORE_REG(field)                                                     \
    (KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |                  \
     (offsetof(struct kvm_regs, field) / sizeof(uint32_t)))

static unsigned checks, failures;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

static void checkf(bool ok, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void checkf(bool ok, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    check(ok, buf);
}

static int set_one_reg(int vcpu, uint64_t id, uint64_t val)
{
    struct kvm_one_reg reg = { .id = id, .addr = (uint64_t)(uintptr_t)&val };

    return qvm_ioctl(vcpu, KVM_SET_ONE_REG, &reg);
}

static int get_one_reg(int vcpu, uint64_t id, uint64_t *val)
{
    struct kvm_one_reg reg = { .id = id, .addr = (uint64_t)(uintptr_t)val };

    *val = 0;
    return qvm_ioctl(vcpu, KVM_GET_ONE_REG, &reg);
}

int main(void)
{
    int sys, vm, vcpu, ret;
    struct kvm_run *run;
    struct kvm_vcpu_init init;
    struct kvm_userspace_memory_region region;
    struct kvm_reg_list probe;
    struct kvm_reg_list *list;
    void *mem;
    size_t guest_len = arm_guest_end - arm_guest;
    uint64_t val, mmap_size;
    unsigned i;
    bool found_pc = false;

    printf("QVM AArch64 smoke test\n\n");

    printf("VM and vCPU setup\n");

    sys = qvm_open("/dev/kvm", 0);
    check(sys >= 0, "qvm_open(\"/dev/kvm\")");
    if (sys < 0) {
        return 1;
    }

    ret = qvm_ioctl(sys, KVM_GET_API_VERSION);
    checkf(ret == 12, "KVM_GET_API_VERSION == 12 (got %d)", ret);

    ret = qvm_ioctl(sys, KVM_CHECK_EXTENSION, KVM_CAP_ONE_REG);
    checkf(ret == 1, "KVM_CHECK_EXTENSION(KVM_CAP_ONE_REG) == 1 (got %d)", ret);

    vm = qvm_ioctl(sys, KVM_CREATE_VM, 0);
    check(vm >= 0, "KVM_CREATE_VM");
    if (vm < 0) {
        return 1;
    }

    memset(&init, 0, sizeof(init));
    ret = qvm_ioctl(vm, KVM_ARM_PREFERRED_TARGET, &init);
    check(ret == 0, "KVM_ARM_PREFERRED_TARGET");
    checkf(init.target == KVM_ARM_TARGET_GENERIC_V8,
           "preferred target is GENERIC_V8 (got %u)", init.target);

    vcpu = qvm_ioctl(vm, KVM_CREATE_VCPU, 0);
    check(vcpu >= 0, "KVM_CREATE_VCPU");
    if (vcpu < 0) {
        return 1;
    }

    ret = qvm_ioctl(sys, KVM_GET_VCPU_MMAP_SIZE);
    mmap_size = ret;
    check(ret > 0, "KVM_GET_VCPU_MMAP_SIZE");

    run = qvm_mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   vcpu, 0);
    check(run != MAP_FAILED, "mmap the kvm_run page");
    if (run == MAP_FAILED) {
        return 1;
    }

    ret = qvm_ioctl(vcpu, KVM_ARM_VCPU_INIT, &init);
    check(ret == 0, "KVM_ARM_VCPU_INIT");

    printf("\nRegister discovery\n");

    /*
     * A client sizes the list first, which the API reports by failing.  gem5
     * builds its whole register map from what comes back here.
     */
    probe.n = 0;
    ret = qvm_ioctl(vcpu, KVM_GET_REG_LIST, &probe);
    checkf(ret == -1 && errno == E2BIG,
           "KVM_GET_REG_LIST with n = 0 fails with E2BIG");
    checkf(probe.n > 0, "KVM_GET_REG_LIST reports a size (%llu)",
           (unsigned long long)probe.n);

    list = calloc(1, sizeof(*list) + probe.n * sizeof(uint64_t));
    list->n = probe.n;
    ret = qvm_ioctl(vcpu, KVM_GET_REG_LIST, list);
    check(ret == 0, "KVM_GET_REG_LIST fills the list");

    for (i = 0; i < list->n; i++) {
        if (list->reg[i] == CORE_REG(regs.pc)) {
            found_pc = true;
        }
    }
    check(found_pc, "the register list contains the PC");

    /* Every register it advertises has to be readable, or the map is a lie. */
    for (i = 0; i < list->n; i++) {
        if (get_one_reg(vcpu, list->reg[i], &val) != 0) {
            checkf(false, "KVM_GET_ONE_REG on advertised id %#llx",
                   (unsigned long long)list->reg[i]);
            break;
        }
    }
    if (i == list->n) {
        checkf(true, "all %llu advertised registers are readable",
               (unsigned long long)list->n);
    }

    printf("\nMemory and entry state\n");

    mem = qvm_mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(mem != MAP_FAILED, "allocate guest memory");
    if (mem == MAP_FAILED) {
        return 1;
    }
    memcpy(mem, arm_guest, guest_len);

    memset(&region, 0, sizeof(region));
    region.slot = 0;
    region.guest_phys_addr = GUEST_PHYS;
    region.memory_size = GUEST_SIZE;
    region.userspace_addr = (uint64_t)(uintptr_t)mem;
    ret = qvm_ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region);
    check(ret == 0, "KVM_SET_USER_MEMORY_REGION");

    check(set_one_reg(vcpu, CORE_REG(regs.pstate), PSTATE_EL1H) == 0,
          "set PSTATE to EL1h");
    check(set_one_reg(vcpu, CORE_REG(regs.pc), GUEST_PHYS) == 0,
          "set PC to the guest entry point");

    check(get_one_reg(vcpu, CORE_REG(regs.pc), &val) == 0 &&
          val == GUEST_PHYS, "PC reads back as it was written");

    printf("\nRunning\n");

    ret = qvm_ioctl(vcpu, KVM_RUN, 0);
    checkf(ret == 0, "KVM_RUN (first store)");
    checkf(run->exit_reason == KVM_EXIT_MMIO,
           "exit is KVM_EXIT_MMIO (got %u)", run->exit_reason);
    checkf(run->mmio.phys_addr == MMIO_ADDR,
           "MMIO address is %#lx (got %#llx)", MMIO_ADDR,
           (unsigned long long)run->mmio.phys_addr);
    check(run->mmio.is_write == 1, "MMIO exit is a write");
    checkf(run->mmio.len == 8, "MMIO access is 8 bytes (got %u)",
           run->mmio.len);
    memcpy(&val, run->mmio.data, 8);
    checkf(val == 42, "the guest stored 42 (got %llu)",
           (unsigned long long)val);

    ret = qvm_ioctl(vcpu, KVM_RUN, 0);
    checkf(ret == 0, "KVM_RUN (load)");
    checkf(run->exit_reason == KVM_EXIT_MMIO,
           "exit is KVM_EXIT_MMIO (got %u)", run->exit_reason);
    check(run->mmio.is_write == 0, "MMIO exit is a read");

    /* Answer the read; QVM must restart the load and complete it from here. */
    val = 100;
    memcpy(run->mmio.data, &val, 8);

    ret = qvm_ioctl(vcpu, KVM_RUN, 0);
    checkf(ret == 0, "KVM_RUN (second store)");
    checkf(run->exit_reason == KVM_EXIT_MMIO,
           "exit is KVM_EXIT_MMIO (got %u)", run->exit_reason);
    check(run->mmio.is_write == 1, "MMIO exit is a write");
    memcpy(&val, run->mmio.data, 8);
    checkf(val == 101, "the guest stored what it read, plus one (got %llu)",
           (unsigned long long)val);

    ret = qvm_ioctl(vcpu, KVM_RUN, 0);
    checkf(ret == 0, "KVM_RUN (to the halt)");
    checkf(run->exit_reason == KVM_EXIT_HLT,
           "exit is KVM_EXIT_HLT (got %u)", run->exit_reason);

    printf("\nFinal register state\n");

    check(get_one_reg(vcpu, CORE_REG(regs.regs[2]), &val) == 0 && val == 100,
          "x2 holds the value supplied for the load");
    check(get_one_reg(vcpu, CORE_REG(regs.regs[3]), &val) == 0 && val == 101,
          "x3 holds the incremented value");
    check(get_one_reg(vcpu, CORE_REG(regs.regs[4]), &val) == 0 &&
          val == 0xabc, "x4 holds the witness written before the halt");

    printf("\n%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
