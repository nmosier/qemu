/*
 * The PSCI function numbers <asm/kvm.h> refers to.  QEMU's linux-headers/ does
 * not ship linux/psci.h; only the few constants used by the KVM ABI are here.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */
#ifndef QVM_COMPAT_LINUX_PSCI_H
#define QVM_COMPAT_LINUX_PSCI_H

#define PSCI_0_2_FN_BASE            0x84000000
#define PSCI_0_2_FN(n)              (PSCI_0_2_FN_BASE + (n))
#define PSCI_0_2_64BIT              0x40000000
#define PSCI_0_2_FN64_BASE          (PSCI_0_2_FN_BASE + PSCI_0_2_64BIT)
#define PSCI_0_2_FN64(n)            (PSCI_0_2_FN64_BASE + (n))

#define PSCI_0_2_FN_PSCI_VERSION    PSCI_0_2_FN(0)
#define PSCI_0_2_FN_CPU_SUSPEND     PSCI_0_2_FN(1)
#define PSCI_0_2_FN_CPU_OFF         PSCI_0_2_FN(2)
#define PSCI_0_2_FN_CPU_ON          PSCI_0_2_FN(3)
#define PSCI_0_2_FN_AFFINITY_INFO   PSCI_0_2_FN(4)
#define PSCI_0_2_FN_MIGRATE_INFO_TYPE PSCI_0_2_FN(6)
#define PSCI_0_2_FN_SYSTEM_OFF      PSCI_0_2_FN(8)
#define PSCI_0_2_FN_SYSTEM_RESET    PSCI_0_2_FN(9)

#define PSCI_0_2_FN64_CPU_SUSPEND   PSCI_0_2_FN64(1)
#define PSCI_0_2_FN64_CPU_ON        PSCI_0_2_FN64(3)
#define PSCI_0_2_FN64_AFFINITY_INFO PSCI_0_2_FN64(4)

#define PSCI_1_0_FN_PSCI_FEATURES   PSCI_0_2_FN(10)
#define PSCI_1_0_FN_SYSTEM_SUSPEND  PSCI_0_2_FN(14)
#define PSCI_1_0_FN64_SYSTEM_SUSPEND PSCI_0_2_FN64(14)

#endif /* QVM_COMPAT_LINUX_PSCI_H */
