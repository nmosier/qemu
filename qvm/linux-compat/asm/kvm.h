/*
 * QVM emulates KVM for an x86 guest regardless of what the host CPU is, so
 * <asm/kvm.h> always resolves to the x86 flavour here.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */
#ifndef QVM_COMPAT_ASM_KVM_H
#define QVM_COMPAT_ASM_KVM_H

#include <asm-x86/kvm.h>

#endif /* QVM_COMPAT_ASM_KVM_H */
