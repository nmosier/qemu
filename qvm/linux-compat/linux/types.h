/*
 * Minimal <linux/types.h> for hosts that are not Linux.
 *
 * QEMU normally only exposes linux-headers/ when building on Linux, where the
 * kernel's own <linux/types.h> is available.  QVM needs the KVM UAPI headers on
 * any host, so provide just enough of the kernel's fixed-width type vocabulary
 * for linux/kvm.h and asm-x86/kvm.h to compile.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */
#ifndef QVM_COMPAT_LINUX_TYPES_H
#define QVM_COMPAT_LINUX_TYPES_H

#include <stdint.h>

typedef uint8_t  __u8;
typedef int8_t   __s8;
typedef uint16_t __u16;
typedef int16_t  __s16;
typedef uint32_t __u32;
typedef int32_t  __s32;
typedef uint64_t __u64;
typedef int64_t  __s64;

typedef __u16 __le16;
typedef __u16 __be16;
typedef __u32 __le32;
typedef __u32 __be32;
typedef __u64 __le64;
typedef __u64 __be64;

/* The kernel forces 8-byte alignment on these even in 32-bit UAPI structs. */
typedef __u64 __attribute__((aligned(8))) __aligned_u64;
typedef __s64 __attribute__((aligned(8))) __aligned_s64;
typedef __le64 __attribute__((aligned(8))) __aligned_le64;
typedef __be64 __attribute__((aligned(8))) __aligned_be64;

typedef long           __kernel_long_t;
typedef unsigned long  __kernel_ulong_t;
typedef __kernel_ulong_t __kernel_size_t;
typedef __kernel_long_t  __kernel_ssize_t;

#ifndef __bitwise
#define __bitwise
#endif
#ifndef __force
#define __force
#endif
#ifndef __user
#define __user
#endif

#ifndef __BITS_PER_LONG
#define __BITS_PER_LONG (__SIZEOF_LONG__ * 8)
#endif
#ifndef __BITS_PER_LONG_LONG
#define __BITS_PER_LONG_LONG 64
#endif

#endif /* QVM_COMPAT_LINUX_TYPES_H */
