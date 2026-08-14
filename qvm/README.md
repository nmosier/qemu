# QVM

QVM implements the KVM API in userspace, on top of QEMU's system-mode TCG.  An
application links `libqvm` and drives it with the same request numbers and
structures it would send to `/dev/kvm`, on any host QEMU's x86_64 system
emulation runs on -- no host kernel support, and no requirement that the host
CPU be an x86.

```c
#include <linux/kvm.h>
#include <qvm/qvm.h>

int sys = qvm_open("/dev/kvm", O_RDWR);
int vm  = qvm_ioctl(sys, KVM_CREATE_VM, 0);
...
```

`qvm_open`, `qvm_ioctl`, `qvm_mmap`, `qvm_munmap` and `qvm_close` stand in for
the corresponding syscalls.  `kvm-hello-world/` is a worked example.

## How it fits together

QVM starts a whole QEMU instance the first time the client issues
`KVM_CREATE_VM`, using a board (`qvm-machine.c`) that builds nothing at all:
guest RAM arrives later as `KVM_SET_USER_MEMORY_REGION`, vCPUs as
`KVM_CREATE_VCPU`, and device emulation stays on the client's side of
`KVM_EXIT_IO`.  QEMU's event loop moves to a thread of QVM's own so the client
keeps its main thread.

The one structural departure from stock QEMU is who runs the vCPU.  QVM
overrides the accelerator's `create_vcpu_thread` so that no vCPU thread is
spawned; `qvm_ioctl(vcpu, KVM_RUN)` instead calls `cpu_exec()` on the client's
own thread, exactly as `ioctl(vcpu_fd, KVM_RUN)` does.  Everything that has to
be reported back is then already on the stack of the thread that will return
it, and there is no cross-thread handshake to get wrong.  In exchange,
`qvm_vcpu_run()` has to do the housekeeping the accelerator's thread loop would
otherwise do -- acknowledging kicks, draining queued cross-CPU work -- and to
absorb the `cpu_exec()` returns that are QEMU's business rather than the
client's.

### Exits

`KVM_EXIT_HLT` falls out of `cpu_exec()` naturally.  `KVM_EXIT_IO` is harder:
the trap has to stop the vCPU at an instruction boundary, but it is discovered
deep inside a `MemoryRegion` callback with an RCU read section (and possibly
the BQL) still held, where unwinding would strand both.

So the callback in `qvm-vm.c` only *records* the exit, and the unwind happens
once the access has returned to the x86 port I/O helper, via the
`qvm_io_exit_hook` in `include/system/qvm-hooks.h`.  That hook rewinds the
guest to the start of the faulting instruction, which gives the client KVM's
restart semantics: the instruction re-executes on the next `KVM_RUN`, and the
trap handler completes it from the shared page instead of trapping again.

## Building

QVM is built as part of QEMU; configure a tree with the x86_64 system target
and build `libqvm`:

```
./configure --target-list=x86_64-softmmu
ninja -C build libqvm.dylib     # libqvm.so on ELF hosts
```

`include/qvm/qvm.h` is the public header.  Clients that include
`<linux/kvm.h>` on a non-Linux host also want `-isystem qvm/linux-compat
-isystem linux-headers`, which supply the KVM UAPI headers and the few kernel
definitions (`__u64`, Linux's `_IOC` encoding) they rest on.

## Environment

| Variable  | Default    | Meaning                              |
|-----------|------------|--------------------------------------|
| `QVM_CPU` | `qemu64`   | x86 CPU model to emulate             |
| `QVM_SMP` | `1`        | vCPUs the client may create          |
| `QVM_LOG` | unset      | QEMU `-d` categories, e.g. `int,mmu` |

## Current limits

- One VM per process, since QEMU's machine and address spaces are process-wide
  singletons.  A second `KVM_CREATE_VM` fails with `EBUSY`.
- Guest is x86; the host can be anything QEMU targets.
- Implemented requests: `KVM_GET_API_VERSION`, `KVM_CHECK_EXTENSION`,
  `KVM_CREATE_VM`, `KVM_GET_VCPU_MMAP_SIZE`, `KVM_SET_USER_MEMORY_REGION`,
  `KVM_SET_TSS_ADDR`, `KVM_SET_IDENTITY_MAP_ADDR`, `KVM_CREATE_VCPU`,
  `KVM_RUN`, `KVM_GET_REGS`, `KVM_SET_REGS`, `KVM_GET_SREGS`, `KVM_SET_SREGS`.
  Anything else returns `ENOTTY`.
- Exits reported: `KVM_EXIT_HLT`, `KVM_EXIT_IO`, `KVM_EXIT_DEBUG`,
  `KVM_EXIT_SHUTDOWN`, `KVM_EXIT_INTERNAL_ERROR`.  `KVM_EXIT_MMIO` is not
  implemented: unassigned guest memory accesses still get QEMU's default
  handling rather than being handed to the client.
- No MSR, FPU/XSAVE, CPUID, interrupt-injection or dirty-log requests, and no
  in-kernel irqchip.
- Descriptors are not host file descriptors and cannot be closed, polled or
  inherited as such; `KVM_CREATE_VM`/`KVM_CREATE_VCPU` results are only valid
  as arguments to the `qvm_*` functions.
