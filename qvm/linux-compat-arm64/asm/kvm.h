/*
 * The guest ABI QVM presents when it was built for an aarch64 guest.
 *
 * A client picks this up by putting the matching qvm/linux-compat-<arch>
 * directory on its include path, ahead of qvm/linux-compat, so that the
 * structures and request numbers it uses describe the guest it asked for --
 * which need have nothing to do with the machine it is running on.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */
#ifndef QVM_COMPAT_ASM_KVM_H
#define QVM_COMPAT_ASM_KVM_H

#include <asm-arm64/kvm.h>

#endif /* QVM_COMPAT_ASM_KVM_H */
