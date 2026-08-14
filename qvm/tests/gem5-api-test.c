/*
 * A conformance test for the parts of the KVM API that gem5's KvmCPU uses.
 *
 * gem5 drives KVM through three thin ioctl wrappers (Kvm::ioctl, KvmVM::ioctl
 * and BaseKvmCPU::ioctl), so what it needs from an implementation is a
 * specific set of requests issued in a specific order, with a specific set of
 * capabilities advertised.  This reproduces that order against libqvm:
 * capability probe, CPUID/MSR discovery, memory slots, vCPU setup and state
 * sync, the signal-driven run loop, IO and MMIO exits, and interrupt delivery.
 *
 * Where gem5's own code is unusual, this follows it rather than the KVM
 * documentation -- notably the growing-buffer loops in Kvm::getSupportedCPUID
 * and Kvm::getSupportedMSRs, and the zero-length "half entry" in
 * BaseKvmCPU::kvmRun(0), which relies on KVM finishing a pending IO before it
 * looks at pending signals.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include <linux/kvm.h>

#include <qvm/qvm.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

/* gem5 uses SIGRTMIN; hosts without POSIX realtime signals get SIGUSR1. */
#ifdef SIGRTMIN
#define KICK_SIGNAL     SIGRTMIN
#else
#define KICK_SIGNAL     SIGUSR1
#endif

#define MEM_SIZE        0x200000
#define GDT_ADDR        0x3000
#define IDT_ADDR        0x2000
#define MMIO_ADDR       0xd0000000
#define IRQ_VECTOR      0x20

/* Where guest.S parks its interrupt handler; the two must agree. */
#define ISR_OFFSET      0x100

#define RESULT_IN       0x1000
#define RESULT_MMIO     0x1004
#define RESULT_CPUID_A  0x1008
#define RESULT_CPUID_B  0x100c
#define IRQ_COUNT       0x1010
#define RESULT_DONE     0x1014

#define IN_VALUE        0x5a
#define MMIO_VALUE      0xc0ffee00u
#define CPUID_MAX_LEAF  0x5
#define CPUID_VENDOR_B  0x356d6567u     /* "gem5" */

extern const unsigned char guest[], guest_end[];

static int failures;
static int checks;

static void ok(bool cond, const char *fmt, ...)
{
    va_list ap;

    checks++;
    if (!cond) {
        failures++;
    }
    printf("%s ", cond ? "  ok  " : "FAILED");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void fail(const char *what)
{
    fprintf(stderr, "fatal: %s: %s\n", what, strerror(errno));
    exit(1);
}

/* ------------------------------------------------------------------ */
/* Descriptor tables                                                   */
/* ------------------------------------------------------------------ */

static void set_gdt_entry(uint8_t *mem, int slot, uint8_t type)
{
    uint8_t *e = mem + GDT_ADDR + slot * 8;

    /* base 0, limit 0xfffff, 4KiB granularity, 32-bit. */
    e[0] = 0xff; e[1] = 0xff;           /* limit 15:0 */
    e[2] = e[3] = e[4] = 0;             /* base 23:0 */
    e[5] = type;                        /* P|S|DPL0|type */
    e[6] = 0xcf;                        /* G|D|limit 19:16 */
    e[7] = 0;                           /* base 31:24 */
}

static void set_idt_entry(uint8_t *mem, int vector, uint32_t handler)
{
    uint8_t *e = mem + IDT_ADDR + vector * 8;

    e[0] = handler & 0xff;
    e[1] = (handler >> 8) & 0xff;
    e[2] = 0x08;                        /* code segment selector */
    e[3] = 0x00;
    e[4] = 0;
    e[5] = 0x8e;                        /* present, DPL0, 32-bit int gate */
    e[6] = (handler >> 16) & 0xff;
    e[7] = (handler >> 24) & 0xff;
}

/* ------------------------------------------------------------------ */
/* Kicks                                                               */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t kicks_delivered;
static pthread_t vcpu_thread;

static void on_kick(int sig, siginfo_t *si, void *data)
{
    (void)sig; (void)si; (void)data;
    kicks_delivered++;
}

/*
 * BaseKvmCPU::setupSignalHandler(): install a handler, tell KVM to run with
 * the kick signal unblocked, and keep it blocked everywhere else so it can
 * only ever be delivered inside KVM_RUN.
 */
static void setup_signal_handler(int vcpu_fd)
{
    struct sigaction sa;
    sigset_t sigset;
    struct {
        struct kvm_signal_mask hdr;
        uint8_t bits[8];
    } mask;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_kick;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    if (sigaction(KICK_SIGNAL, &sa, NULL) == -1) {
        fail("sigaction");
    }

    if (pthread_sigmask(SIG_BLOCK, NULL, &sigset) == -1) {
        fail("pthread_sigmask");
    }
    sigdelset(&sigset, KICK_SIGNAL);

    /*
     * gem5 memcpy()s the host's sigset_t and declares it 8 bytes long.  Build
     * the same 8-byte kernel bitmap from scratch instead, so the test says the
     * same thing on hosts whose sigset_t is not laid out that way.
     */
    memset(&mask, 0, sizeof(mask));
    mask.hdr.len = 8;
    for (int sig = 1; sig < 64; sig++) {
        if (sigismember(&sigset, sig) == 1) {
            mask.bits[(sig - 1) / 8] |= 1u << ((sig - 1) % 8);
        }
    }
    if (qvm_ioctl(vcpu_fd, KVM_SET_SIGNAL_MASK, &mask.hdr) == -1) {
        fail("KVM_SET_SIGNAL_MASK");
    }

    sigaddset(&sigset, KICK_SIGNAL);
    if (pthread_sigmask(SIG_SETMASK, &sigset, NULL) == -1) {
        fail("pthread_sigmask");
    }
}

static void kick(void)
{
    pthread_kill(vcpu_thread, KICK_SIGNAL);
}

struct delayed_kick {
    unsigned usec;
};

static void *delayed_kick_thread(void *opaque)
{
    struct delayed_kick *dk = opaque;
    struct timespec ts = {
        .tv_sec = dk->usec / 1000000,
        .tv_nsec = (dk->usec % 1000000) * 1000,
    };

    nanosleep(&ts, NULL);
    kick();
    return NULL;
}

/*
 * Stand-in for gem5's PosixKvmTimer: bound a run by arranging for the kick to
 * arrive while the vCPU is inside KVM_RUN.
 */
static pthread_t arm_kick_timer(struct delayed_kick *dk, unsigned usec)
{
    pthread_t t;

    dk->usec = usec;
    pthread_create(&t, NULL, delayed_kick_thread, dk);
    return t;
}

/* ------------------------------------------------------------------ */
/* Discovery, the way gem5 does it                                     */
/* ------------------------------------------------------------------ */

static int check_extension(int sys_fd, int cap)
{
    int ret = qvm_ioctl(sys_fd, KVM_CHECK_EXTENSION, cap);

    if (ret == -1) {
        fail("KVM_CHECK_EXTENSION");
    }
    return ret;
}

/* Kvm::getSupportedCPUID(): grow the buffer by one entry until it fits. */
static struct kvm_cpuid2 *get_supported_cpuid(int sys_fd, int *rounds)
{
    struct kvm_cpuid2 *cpuid = NULL;
    int n = 1;

    *rounds = 0;
    for (;;) {
        free(cpuid);
        cpuid = calloc(1, sizeof(*cpuid) + n * sizeof(struct kvm_cpuid_entry2));
        cpuid->nent = n;
        (*rounds)++;

        if (qvm_ioctl(sys_fd, KVM_GET_SUPPORTED_CPUID, cpuid) != -1) {
            return cpuid;
        }
        if (errno != E2BIG) {
            fail("KVM_GET_SUPPORTED_CPUID");
        }
        if (++n > 512) {
            fprintf(stderr, "fatal: CPUID list never fit\n");
            exit(1);
        }
    }
}

/* Kvm::getSupportedMSRs(): same pattern. */
static struct kvm_msr_list *get_supported_msrs(int sys_fd)
{
    struct kvm_msr_list *msrs = NULL;
    int n = 0;

    for (;;) {
        free(msrs);
        msrs = calloc(1, sizeof(*msrs) + n * sizeof(uint32_t));
        msrs->nmsrs = n;

        if (qvm_ioctl(sys_fd, KVM_GET_MSR_INDEX_LIST, msrs) != -1) {
            return msrs;
        }
        if (errno != E2BIG) {
            fail("KVM_GET_MSR_INDEX_LIST");
        }
        if (++n > 512) {
            fprintf(stderr, "fatal: MSR list never fit\n");
            exit(1);
        }
    }
}

/* ------------------------------------------------------------------ */
/* vCPU state                                                          */
/* ------------------------------------------------------------------ */

static void setup_protected_mode(struct kvm_sregs *sregs)
{
    struct kvm_segment code = {
        .base = 0, .limit = 0xffffffff, .selector = 1 << 3,
        .present = 1, .type = 11, .dpl = 0, .db = 1, .s = 1, .l = 0, .g = 1,
    };
    struct kvm_segment data = code;

    data.type = 3;
    data.selector = 2 << 3;

    sregs->cr0 |= 1;                    /* CR0.PE */
    sregs->cs = code;
    sregs->ds = sregs->es = sregs->fs = sregs->gs = sregs->ss = data;

    sregs->gdt.base = GDT_ADDR;
    sregs->gdt.limit = 3 * 8 - 1;
    sregs->idt.base = IDT_ADDR;
    sregs->idt.limit = 256 * 8 - 1;
}

/* X86KvmCPU::updateCPUID() pushes gem5's own CPU model, not the host's. */
static void set_cpuid(int vcpu_fd)
{
    struct kvm_cpuid2 *cpuid;

    cpuid = calloc(1, sizeof(*cpuid) + 2 * sizeof(struct kvm_cpuid_entry2));
    cpuid->nent = 2;
    cpuid->entries[0].function = 0;
    cpuid->entries[0].eax = CPUID_MAX_LEAF;
    cpuid->entries[0].ebx = CPUID_VENDOR_B;
    cpuid->entries[1].function = 1;
    cpuid->entries[1].edx = (1 << 0);   /* FPU */

    if (qvm_ioctl(vcpu_fd, KVM_SET_CPUID2, cpuid) == -1) {
        fail("KVM_SET_CPUID2");
    }
    free(cpuid);
}

static uint64_t get_msr(int vcpu_fd, uint32_t index)
{
    struct {
        struct kvm_msrs hdr;
        struct kvm_msr_entry entry;
    } m;

    memset(&m, 0, sizeof(m));
    m.hdr.nmsrs = 1;
    m.entry.index = index;
    if (qvm_ioctl(vcpu_fd, KVM_GET_MSRS, &m.hdr) == -1) {
        fail("KVM_GET_MSRS");
    }
    return m.entry.data;
}

static int set_msr(int vcpu_fd, uint32_t index, uint64_t value)
{
    struct {
        struct kvm_msrs hdr;
        struct kvm_msr_entry entry;
    } m;

    memset(&m, 0, sizeof(m));
    m.hdr.nmsrs = 1;
    m.entry.index = index;
    m.entry.data = value;
    return qvm_ioctl(vcpu_fd, KVM_SET_MSRS, &m.hdr);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    int sys_fd, vm_fd, vcpu_fd;
    int api_version, mmap_size;
    struct kvm_userspace_memory_region memreg;
    struct kvm_run *run;
    struct kvm_sregs sregs;
    struct kvm_regs regs;
    struct kvm_cpuid2 *supported_cpuid;
    struct kvm_msr_list *supported_msrs;
    struct delayed_kick dk;
    pthread_t timer;
    uint8_t *mem;
    int rounds;
    bool have_tsc = false;

    vcpu_thread = pthread_self();

    puts("== Kvm::Kvm() ==");
    sys_fd = qvm_open("/dev/kvm", O_RDWR);
    ok(sys_fd >= 0, "open /dev/kvm");

    api_version = qvm_ioctl(sys_fd, KVM_GET_API_VERSION, 0);
    ok(api_version == KVM_API_VERSION, "KVM_GET_API_VERSION == %d (got %d)",
       KVM_API_VERSION, api_version);

    mmap_size = qvm_ioctl(sys_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    ok(mmap_size >= (int)sizeof(struct kvm_run),
       "KVM_GET_VCPU_MMAP_SIZE = %d", mmap_size);

    puts("\n== capability probe (Kvm::cap*) ==");
    /* X86KvmCPU::init() panics without these two. */
    ok(check_extension(sys_fd, KVM_CAP_SET_TSS_ADDR) != 0,
       "KVM_CAP_SET_TSS_ADDR (required)");
    ok(check_extension(sys_fd, KVM_CAP_EXT_CPUID) != 0,
       "KVM_CAP_EXT_CPUID (required)");
    ok(check_extension(sys_fd, KVM_CAP_USER_MEMORY) != 0,
       "KVM_CAP_USER_MEMORY (required)");
    /* These only produce warnings, but gem5 loses function without them. */
    ok(check_extension(sys_fd, KVM_CAP_USER_NMI) != 0, "KVM_CAP_USER_NMI");
    ok(check_extension(sys_fd, KVM_CAP_VCPU_EVENTS) != 0,
       "KVM_CAP_VCPU_EVENTS");
    ok(check_extension(sys_fd, KVM_CAP_DEBUGREGS) != 0, "KVM_CAP_DEBUGREGS");
    ok(check_extension(sys_fd, KVM_CAP_XSAVE) != 0, "KVM_CAP_XSAVE");
    ok(check_extension(sys_fd, KVM_CAP_XCRS) != 0, "KVM_CAP_XCRS");
    ok(check_extension(sys_fd, KVM_CAP_NR_MEMSLOTS) > 0,
       "KVM_CAP_NR_MEMSLOTS = %d", check_extension(sys_fd,
                                                  KVM_CAP_NR_MEMSLOTS));
    /* Absent on purpose: the client owns the interrupt controller. */
    ok(check_extension(sys_fd, KVM_CAP_IRQCHIP) == 0,
       "KVM_CAP_IRQCHIP absent (userspace irqchip)");
    ok(check_extension(sys_fd, 0x7fffffff) == 0, "unknown capability reads 0");

    puts("\n== Kvm::getSupportedMSRs() ==");
    supported_msrs = get_supported_msrs(sys_fd);
    ok(supported_msrs->nmsrs > 0, "KVM_GET_MSR_INDEX_LIST: %u MSRs",
       supported_msrs->nmsrs);
    for (uint32_t i = 0; i < supported_msrs->nmsrs; i++) {
        if (supported_msrs->indices[i] == 0x10) {
            have_tsc = true;
        }
    }
    ok(have_tsc, "MSR_TSC advertised (X86KvmCPU::getHostCycles)");

    puts("\n== KvmVM ==");
    vm_fd = qvm_ioctl(sys_fd, KVM_CREATE_VM, 0);
    ok(vm_fd >= 0, "KVM_CREATE_VM");
    ok(qvm_ioctl(vm_fd, KVM_SET_TSS_ADDR, 0xfffbd000) == 0,
       "KVM_SET_TSS_ADDR");

    mem = qvm_mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) {
        fail("mmap guest memory");
    }

    memset(&memreg, 0, sizeof(memreg));
    memreg.slot = 0;
    memreg.guest_phys_addr = 0;
    memreg.memory_size = MEM_SIZE;
    memreg.userspace_addr = (uintptr_t)mem;
    ok(qvm_ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &memreg) == 0,
       "KVM_SET_USER_MEMORY_REGION");

    puts("\n== BaseKvmCPU::startup() ==");
    vcpu_fd = qvm_ioctl(vm_fd, KVM_CREATE_VCPU, 0);
    ok(vcpu_fd >= 0, "KVM_CREATE_VCPU");

    run = qvm_mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   vcpu_fd, 0);
    ok(run != MAP_FAILED, "mmap kvm_run");

    /*
     * Kvm::getSupportedCPUID() may run at any time under KVM; QVM needs a
     * realized vCPU first, so it is exercised here.
     */
    supported_cpuid = get_supported_cpuid(sys_fd, &rounds);
    ok(supported_cpuid->nent > 0, "KVM_GET_SUPPORTED_CPUID: %u entries after "
       "%d E2BIG rounds", supported_cpuid->nent, rounds - 1);

    /*
     * Kvm::getSupportedCPUID() would normally run at any time; QVM needs a
     * realized vCPU first, so it is exercised here.
     */
    supported_cpuid = get_supported_cpuid(sys_fd, &rounds);
    ok(supported_cpuid->nent > 0, "KVM_GET_SUPPORTED_CPUID: %u entries after "
       "%d E2BIG rounds", supported_cpuid->nent, rounds - 1);

    setup_signal_handler(vcpu_fd);
    ok(true, "KVM_SET_SIGNAL_MASK");
    ok(qvm_ioctl(vcpu_fd, KVM_SET_SIGNAL_MASK, NULL) == 0,
       "KVM_SET_SIGNAL_MASK(NULL) clears the mask");
    setup_signal_handler(vcpu_fd);

    set_cpuid(vcpu_fd);
    ok(true, "KVM_SET_CPUID2");

    puts("\n== X86KvmCPU::updateKvmState() / updateThreadContext() ==");
    {
        struct kvm_fpu fpu, fpu2;
        struct kvm_xsave xsave;
        struct kvm_xcrs xcrs;
        struct kvm_debugregs dregs, dregs2;
        struct kvm_vcpu_events events;

        ok(qvm_ioctl(vcpu_fd, KVM_GET_FPU, &fpu) == 0, "KVM_GET_FPU");
        fpu.fcw = 0x037f;
        fpu.mxcsr = 0x1f80;
        fpu.xmm[3][0] = 0xa5;
        ok(qvm_ioctl(vcpu_fd, KVM_SET_FPU, &fpu) == 0, "KVM_SET_FPU");
        ok(qvm_ioctl(vcpu_fd, KVM_GET_FPU, &fpu2) == 0, "KVM_GET_FPU again");
        ok(fpu2.fcw == 0x037f && fpu2.mxcsr == 0x1f80 && fpu2.xmm[3][0] == 0xa5,
           "FPU state round-trips");

        ok(qvm_ioctl(vcpu_fd, KVM_GET_XSAVE, &xsave) == 0, "KVM_GET_XSAVE");
        ok(qvm_ioctl(vcpu_fd, KVM_SET_XSAVE, &xsave) == 0, "KVM_SET_XSAVE");

        ok(qvm_ioctl(vcpu_fd, KVM_GET_XCRS, &xcrs) == 0, "KVM_GET_XCRS");
        ok(xcrs.nr_xcrs >= 1 && xcrs.xcrs[0].xcr == 0, "XCR0 reported");
        ok(qvm_ioctl(vcpu_fd, KVM_SET_XCRS, &xcrs) == 0, "KVM_SET_XCRS");

        ok(qvm_ioctl(vcpu_fd, KVM_GET_DEBUGREGS, &dregs) == 0,
           "KVM_GET_DEBUGREGS");
        dregs.db[1] = 0x1234;
        ok(qvm_ioctl(vcpu_fd, KVM_SET_DEBUGREGS, &dregs) == 0,
           "KVM_SET_DEBUGREGS");
        ok(qvm_ioctl(vcpu_fd, KVM_GET_DEBUGREGS, &dregs2) == 0 &&
           dregs2.db[1] == 0x1234, "debug registers round-trip");

        ok(qvm_ioctl(vcpu_fd, KVM_GET_VCPU_EVENTS, &events) == 0,
           "KVM_GET_VCPU_EVENTS");
        ok(!events.exception.injected && !events.interrupt.injected &&
           !events.nmi.injected && !events.nmi.pending,
           "X86KvmCPU::archIsDrained() sees no pending events");
    }

    puts("\n== MSRs ==");
    {
        uint64_t tsc0, tsc1;

        ok(set_msr(vcpu_fd, 0xc0000081 /* MSR_STAR */, 0x1122334455667788ull)
           == 1, "KVM_SET_MSRS handles MSR_STAR");
        ok(get_msr(vcpu_fd, 0xc0000081) == 0x1122334455667788ull,
           "MSR_STAR round-trips");
        ok(set_msr(vcpu_fd, 0x174 /* SYSENTER_CS */, 0x8) == 1,
           "KVM_SET_MSRS handles MSR_IA32_SYSENTER_CS");
        ok(get_msr(vcpu_fd, 0x174) == 0x8, "MSR_IA32_SYSENTER_CS round-trips");

        tsc0 = get_msr(vcpu_fd, 0x10);
        tsc1 = get_msr(vcpu_fd, 0x10);
        ok(tsc1 >= tsc0, "MSR_TSC readable and monotonic (%llu -> %llu)",
           (unsigned long long)tsc0, (unsigned long long)tsc1);
    }

    puts("\n== guest setup ==");
    memcpy(mem, guest, guest_end - guest);
    set_gdt_entry(mem, 0, 0x00);
    set_gdt_entry(mem, 1, 0x9a);        /* code */
    set_gdt_entry(mem, 2, 0x92);        /* data */
    for (int v = 0; v < 256; v++) {
        set_idt_entry(mem, v, ISR_OFFSET);
    }

    if (qvm_ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) == -1) {
        fail("KVM_GET_SREGS");
    }
    setup_protected_mode(&sregs);
    if (qvm_ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) == -1) {
        fail("KVM_SET_SREGS");
    }

    memset(&regs, 0, sizeof(regs));
    regs.rflags = 2;
    regs.rip = 0;
    if (qvm_ioctl(vcpu_fd, KVM_SET_REGS, &regs) == -1) {
        fail("KVM_SET_REGS");
    }

    {
        struct kvm_sregs back;

        ok(qvm_ioctl(vcpu_fd, KVM_GET_SREGS, &back) == 0, "KVM_GET_SREGS");
        ok(back.cs.selector == (1 << 3) && back.cs.db == 1 &&
           back.gdt.base == GDT_ADDR && back.idt.base == IDT_ADDR,
           "segment and descriptor table state round-trips");
    }

    puts("\n== run loop ==");

    /* 1: OUT. */
    ok(qvm_ioctl(vcpu_fd, KVM_RUN, 0) == 0, "KVM_RUN");
    ok(run->exit_reason == KVM_EXIT_IO, "KVM_EXIT_IO (got %u)",
       run->exit_reason);
    ok(run->io.direction == KVM_EXIT_IO_OUT && run->io.port == 0xe9 &&
       run->io.size == 1 && run->io.count == 1,
       "out to port 0xe9, size 1, count 1");
    ok(*((uint8_t *)run + run->io.data_offset) == 0x2a,
       "io.data_offset carries 0x2a");

    /* 2: IN. */
    ok(qvm_ioctl(vcpu_fd, KVM_RUN, 0) == 0, "KVM_RUN");
    ok(run->exit_reason == KVM_EXIT_IO &&
       run->io.direction == KVM_EXIT_IO_IN && run->io.port == 0x10,
       "KVM_EXIT_IO in from port 0x10");
    *((uint8_t *)run + run->io.data_offset) = IN_VALUE;

    /*
     * BaseKvmCPU::kvmRun(0): raise the kick first, then enter.  The pending
     * IO must still be delivered to the guest before the signal is noticed,
     * otherwise the value above is lost.
     */
    kick();
    ok(qvm_ioctl(vcpu_fd, KVM_RUN, 0) == -1 && errno == EINTR,
       "zero-length entry returns EINTR");
    ok(run->exit_reason == KVM_EXIT_INTR, "KVM_EXIT_INTR (got %u)",
       run->exit_reason);
    /*
     * The entry finishes the "in" instruction and stops there, so look at the
     * register it loaded rather than at the store that follows it.
     */
    if (qvm_ioctl(vcpu_fd, KVM_GET_REGS, &regs) == -1) {
        fail("KVM_GET_REGS");
    }
    ok((regs.rax & 0xff) == IN_VALUE,
       "pending IO was delivered before the signal was taken (rax=0x%llx)",
       (unsigned long long)regs.rax);

    /* 3: MMIO write. */
    ok(qvm_ioctl(vcpu_fd, KVM_RUN, 0) == 0, "KVM_RUN");
    ok(run->exit_reason == KVM_EXIT_MMIO, "KVM_EXIT_MMIO (got %u)",
       run->exit_reason);
    ok(run->mmio.is_write && run->mmio.phys_addr == MMIO_ADDR &&
       run->mmio.len == 4,
       "mmio write to 0x%llx len 4", (unsigned long long)run->mmio.phys_addr);
    ok(*(uint32_t *)run->mmio.data == 0xdeadbeefu,
       "mmio.data carries 0xdeadbeef");

    /* 4: MMIO read. */
    ok(qvm_ioctl(vcpu_fd, KVM_RUN, 0) == 0, "KVM_RUN");
    ok(run->exit_reason == KVM_EXIT_MMIO && !run->mmio.is_write &&
       run->mmio.phys_addr == MMIO_ADDR, "KVM_EXIT_MMIO read");
    *(uint32_t *)run->mmio.data = MMIO_VALUE;

    /* 5: the guest spins; a timer kick has to be able to end the run. */
    timer = arm_kick_timer(&dk, 50000);
    ok(qvm_ioctl(vcpu_fd, KVM_RUN, 0) == -1 && errno == EINTR,
       "timer kick ends a running guest");
    ok(run->exit_reason == KVM_EXIT_INTR, "KVM_EXIT_INTR from the timer");
    pthread_join(timer, NULL);

    ok(*(uint32_t *)(mem + RESULT_MMIO) == MMIO_VALUE,
       "mmio read result reached the guest");
    ok(*(uint32_t *)(mem + RESULT_CPUID_A) == CPUID_MAX_LEAF &&
       *(uint32_t *)(mem + RESULT_CPUID_B) == CPUID_VENDOR_B,
       "guest CPUID came from KVM_SET_CPUID2");

    /* 6: the guest is interruptible, so it should be asking for nothing. */
    ok(run->ready_for_interrupt_injection && run->if_flag,
       "kvm_run reports the guest ready for an interrupt");

    /* X86KvmCPU::kvmRun() sets this when it cannot deliver yet. */
    run->request_interrupt_window = 1;
    ok(qvm_ioctl(vcpu_fd, KVM_RUN, 0) == 0, "KVM_RUN with interrupt window");
    ok(run->exit_reason == KVM_EXIT_IRQ_WINDOW_OPEN,
       "KVM_EXIT_IRQ_WINDOW_OPEN (got %u)", run->exit_reason);
    run->request_interrupt_window = 0;

    /* 7: deliver the interrupt that releases the guest's loop. */
    {
        struct kvm_interrupt intr = { .irq = IRQ_VECTOR };

        ok(qvm_ioctl(vcpu_fd, KVM_INTERRUPT, &intr) == 0, "KVM_INTERRUPT");
    }

    timer = arm_kick_timer(&dk, 500000);
    ok(qvm_ioctl(vcpu_fd, KVM_RUN, 0) == 0, "KVM_RUN after injection");
    ok(run->exit_reason == KVM_EXIT_HLT, "KVM_EXIT_HLT (got %u)",
       run->exit_reason);
    pthread_join(timer, NULL);

    ok(*(uint32_t *)(mem + IRQ_COUNT) >= 1,
       "the guest's interrupt handler ran (%u times)",
       *(uint32_t *)(mem + IRQ_COUNT));
    ok(*(uint32_t *)(mem + RESULT_DONE) == 0x600d,
       "the guest reached the end");

    puts("\n== final state ==");
    ok(qvm_ioctl(vcpu_fd, KVM_GET_REGS, &regs) == 0, "KVM_GET_REGS");
    ok(qvm_ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) == 0, "KVM_GET_SREGS");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
