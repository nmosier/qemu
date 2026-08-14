/*
 * Linux's ioctl request encoding, for hosts that are not Linux.
 *
 * The KVM UAPI headers build their request numbers out of _IO/_IOR/_IOW/_IOWR.
 * BSD-derived hosts (macOS) define those macros with a different, incompatible
 * layout in <sys/ioctl.h>, so override them: a QVM request number must mean the
 * same thing everywhere, since it is an ABI shared with the guest-facing
 * headers rather than something the host kernel ever sees.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */
#ifndef QVM_COMPAT_LINUX_IOCTL_H
#define QVM_COMPAT_LINUX_IOCTL_H

#undef _IOC
#undef _IO
#undef _IOR
#undef _IOW
#undef _IOWR

#define _IOC_NRBITS     8
#define _IOC_TYPEBITS   8
#define _IOC_SIZEBITS   14
#define _IOC_DIRBITS    2

#define _IOC_NRMASK     ((1 << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK   ((1 << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK   ((1 << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK    ((1 << _IOC_DIRBITS) - 1)

#define _IOC_NRSHIFT    0
#define _IOC_TYPESHIFT  (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT  (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT   (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE       0U
#define _IOC_WRITE      1U
#define _IOC_READ       2U

#define _IOC(dir, type, nr, size) \
    (((dir)  << _IOC_DIRSHIFT) |  \
     ((type) << _IOC_TYPESHIFT) | \
     ((nr)   << _IOC_NRSHIFT) |   \
     ((size) << _IOC_SIZESHIFT))

#define _IOC_TYPECHECK(t) (sizeof(t))

#define _IO(type, nr)           _IOC(_IOC_NONE,  (type), (nr), 0)
#define _IOR(type, nr, size)    _IOC(_IOC_READ,  (type), (nr), _IOC_TYPECHECK(size))
#define _IOW(type, nr, size)    _IOC(_IOC_WRITE, (type), (nr), _IOC_TYPECHECK(size))
#define _IOWR(type, nr, size)   _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), \
                                     _IOC_TYPECHECK(size))

#define _IOC_DIR(nr)    (((nr) >> _IOC_DIRSHIFT) & _IOC_DIRMASK)
#define _IOC_TYPE(nr)   (((nr) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(nr)     (((nr) >> _IOC_NRSHIFT) & _IOC_NRMASK)
#define _IOC_SIZE(nr)   (((nr) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)

#endif /* QVM_COMPAT_LINUX_IOCTL_H */
