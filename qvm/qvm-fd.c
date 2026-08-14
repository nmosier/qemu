/*
 * QVM descriptor table and public entry points.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/lockable.h"
#include "qvm/qvm.h"
#include "qvm-internal.h"

#include <stdarg.h>

/*
 * QVM descriptors are deliberately not host file descriptors: handing a real
 * fd out would suggest that read(2)/close(2) work on it.  They start well
 * above any plausible fd so that passing one to a real syscall by mistake
 * fails immediately instead of hitting an unrelated open file.
 */
#define QVM_FD_BASE     4096
#define QVM_MAX_FDS     64

typedef enum QvmFdType {
    QVM_FD_UNUSED = 0,
    QVM_FD_SYS,
    QVM_FD_VM,
    QVM_FD_VCPU,
} QvmFdType;

typedef struct QvmFd {
    QvmFdType type;
    union {
        QvmVM *vm;
        QvmVcpu *vcpu;
    };
} QvmFd;

static QemuMutex qvm_fd_lock;
static QvmFd qvm_fds[QVM_MAX_FDS];

static void __attribute__((constructor)) qvm_fd_init(void)
{
    qemu_mutex_init(&qvm_fd_lock);
}

static int qvm_fd_alloc(QvmFdType type, void *obj)
{
    int i;

    QEMU_LOCK_GUARD(&qvm_fd_lock);
    for (i = 0; i < QVM_MAX_FDS; i++) {
        if (qvm_fds[i].type == QVM_FD_UNUSED) {
            qvm_fds[i].type = type;
            qvm_fds[i].vm = obj;
            return QVM_FD_BASE + i;
        }
    }
    return qvm_err(EMFILE);
}

static QvmFd *qvm_fd_lookup(int fd)
{
    int i = fd - QVM_FD_BASE;

    if (i < 0 || i >= QVM_MAX_FDS || qvm_fds[i].type == QVM_FD_UNUSED) {
        return NULL;
    }
    return &qvm_fds[i];
}

int qvm_fd_alloc_vm(QvmVM *vm)
{
    return qvm_fd_alloc(QVM_FD_VM, vm);
}

int qvm_fd_alloc_vcpu(QvmVcpu *vcpu)
{
    return qvm_fd_alloc(QVM_FD_VCPU, vcpu);
}

int qvm_open(const char *path, int flags)
{
    if (!path || strcmp(path, "/dev/kvm") != 0) {
        return qvm_err(ENOENT);
    }
    return qvm_fd_alloc(QVM_FD_SYS, NULL);
}

int qvm_close(int fd)
{
    QvmFd *f;

    QEMU_LOCK_GUARD(&qvm_fd_lock);
    f = qvm_fd_lookup(fd);
    if (!f) {
        return qvm_err(EBADF);
    }

    switch (f->type) {
    case QVM_FD_VCPU:
        qvm_vcpu_destroy(f->vcpu);
        break;
    case QVM_FD_VM:
        qvm_vm_destroy(f->vm);
        break;
    default:
        break;
    }

    f->type = QVM_FD_UNUSED;
    f->vm = NULL;
    return 0;
}

/*
 * KVM_CHECK_EXTENSION is answered identically on the /dev/kvm and VM
 * descriptors, as it is in KVM.
 */
int qvm_check_extension(unsigned long cap)
{
    switch (cap) {
    case KVM_CAP_USER_MEMORY:
    case KVM_CAP_SET_TSS_ADDR:
    case KVM_CAP_SET_IDENTITY_MAP_ADDR:
        return 1;
    case KVM_CAP_NR_VCPUS:
    case KVM_CAP_MAX_VCPUS:
        return QVM_MAX_VCPUS;
    case KVM_CAP_NR_MEMSLOTS:
        return QVM_MAX_MEMSLOTS;
    default:
        return 0;
    }
}

/* Requests handled by the /dev/kvm descriptor itself. */
static int qvm_sys_ioctl(unsigned long request, uintptr_t arg)
{
    switch (request) {
    case KVM_GET_API_VERSION:
        return KVM_API_VERSION;

    case KVM_GET_VCPU_MMAP_SIZE:
        return QVM_VCPU_MMAP_SIZE;

    case KVM_CREATE_VM: {
        QvmVM *vm;
        int ret = qvm_vm_create(&vm);

        if (ret < 0) {
            return ret;
        }
        return qvm_fd_alloc_vm(vm);
    }

    case KVM_CHECK_EXTENSION:
        return qvm_check_extension(arg);

    default:
        return qvm_err(ENOTTY);
    }
}

int qvm_ioctl(int fd, unsigned long request, ...)
{
    QvmFd *f;
    QvmFdType type;
    void *obj;
    uintptr_t arg;
    va_list ap;

    /*
     * KVM's request numbers encode whether the argument is a pointer to a
     * struct or a bare scalar.  That distinction matters here in a way it does
     * not for ioctl(2) on Linux/x86: on hosts that pass variadic arguments on
     * the stack, reading an int-sized argument as a pointer picks up garbage.
     */
    va_start(ap, request);
    if (_IOC_DIR(request) == _IOC_NONE) {
        arg = va_arg(ap, unsigned int);
    } else {
        arg = (uintptr_t)va_arg(ap, void *);
    }
    va_end(ap);

    qemu_mutex_lock(&qvm_fd_lock);
    f = qvm_fd_lookup(fd);
    if (!f) {
        qemu_mutex_unlock(&qvm_fd_lock);
        return qvm_err(EBADF);
    }
    type = f->type;
    obj = f->vm;
    qemu_mutex_unlock(&qvm_fd_lock);

    switch (type) {
    case QVM_FD_SYS:
        return qvm_sys_ioctl(request, arg);
    case QVM_FD_VM:
        return qvm_vm_ioctl(obj, request, arg);
    case QVM_FD_VCPU:
        return qvm_vcpu_ioctl(obj, request, arg);
    default:
        return qvm_err(EBADF);
    }
}

void *qvm_mmap(void *addr, size_t length, int prot, int flags,
               int fd, off_t offset)
{
    QvmFd *f;
    QvmVcpu *vcpu;

    /*
     * Anonymous mappings have nothing to do with QVM; pass them straight
     * through so a client can allocate guest RAM with the same call it uses
     * for the vCPU's shared page.
     */
    if (fd < 0) {
        return mmap(addr, length, prot, flags, fd, offset);
    }

    QEMU_LOCK_GUARD(&qvm_fd_lock);
    f = qvm_fd_lookup(fd);
    if (!f) {
        errno = EBADF;
        return MAP_FAILED;
    }
    if (f->type != QVM_FD_VCPU) {
        errno = ENODEV;
        return MAP_FAILED;
    }

    vcpu = f->vcpu;
    if (offset != 0 || length > QVM_VCPU_MMAP_SIZE) {
        errno = EINVAL;
        return MAP_FAILED;
    }

    /*
     * The shared page already lives in this process, so there is nothing to
     * map: hand back the buffer itself.  It is not reference counted, and
     * qvm_munmap() on it is a no-op.
     */
    return vcpu->run;
}

int qvm_munmap(void *addr, size_t length)
{
    int i;

    QEMU_LOCK_GUARD(&qvm_fd_lock);
    for (i = 0; i < QVM_MAX_FDS; i++) {
        if (qvm_fds[i].type == QVM_FD_VCPU &&
            qvm_fds[i].vcpu->run == addr) {
            return 0;
        }
    }
    return munmap(addr, length);
}
