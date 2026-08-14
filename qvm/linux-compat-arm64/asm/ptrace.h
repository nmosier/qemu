/*
 * The two structures <asm/kvm.h> takes from the arm64 kernel's <asm/ptrace.h>.
 *
 * QEMU's linux-headers/ does not ship arm64's ptrace.h, and the rest of that
 * header is a debugging interface QVM has no use for.  Only the register
 * layouts struct kvm_regs is built from are needed, and they are ABI.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */
#ifndef QVM_COMPAT_ASM_PTRACE_H
#define QVM_COMPAT_ASM_PTRACE_H

#include <linux/types.h>

struct user_pt_regs {
    __u64 regs[31];
    __u64 sp;
    __u64 pc;
    __u64 pstate;
};

struct user_fpsimd_state {
    __uint128_t vregs[32];
    __u32 fpsr;
    __u32 fpcr;
    __u32 __reserved[2];
};

#endif /* QVM_COMPAT_ASM_PTRACE_H */
