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

`KVM_EXIT_HLT` falls out of `cpu_exec()` naturally.  `KVM_EXIT_IO` and
`KVM_EXIT_MMIO` are harder: the trap has to stop the vCPU at an instruction
boundary, but it is discovered deep inside a `MemoryRegion` callback with an
RCU read section (and possibly the BQL) still held, where unwinding would
strand both.

So the callbacks in `qvm-vm.c` only *record* the exit, and the unwind happens
once the access has completed, via the `qvm_io_exit_hook` in
`include/system/qvm-hooks.h`.  Port I/O reaches it from the x86 I/O helpers;
MMIO reaches it from cputlb's failed-transaction path, which QVM's catch-all
region enters by returning `MEMTX_ERROR`.  Either way the hook rewinds the
guest to the start of the faulting instruction, giving the client KVM's restart
semantics: the instruction re-executes on the next `KVM_RUN`, and the trap
handler completes it from the shared page instead of trapping again.

### Interrupts and signals

QVM has no in-kernel interrupt controller: `KVM_CAP_IRQCHIP` reads 0, the vCPU
is built without a local APIC, and the guest's APIC page comes out as a
`KVM_EXIT_MMIO`.  The client delivers vectors with `KVM_INTERRUPT`, and asks to
be told when the guest can accept one with `kvm_run::request_interrupt_window`
(answered by `KVM_EXIT_IRQ_WINDOW_OPEN`).

`KVM_SET_SIGNAL_MASK` works the way clients rely on: the mask is installed
around the guest run, and delivery of a signal it unblocks ends `KVM_RUN` with
`KVM_EXIT_INTR` and `EINTR`.  A signal raised while an I/O access is still
outstanding is held off until that access has been delivered to the guest, so
the zero-length entry clients use to complete an I/O and return immediately
behaves as it does under KVM.

## Guest architectures

QVM is built as part of QEMU, once per guest architecture.  The KVM ABI a
client compiles against *is* the guest's, so a single libqvm cannot speak for
two of them: give each its own build directory.

```
./configure --target-list=x86_64-softmmu   && ninja -C build libqvm.so
./configure --target-list=aarch64-softmmu  && ninja -C build-arm libqvm.so
```

| guest | backend | state |
|-------|---------|-------|
| x86_64 | `qvm-x86.c` | runs Linux under gem5, SE and FS, 1 and 2 cores |
| aarch64 | `qvm-arm.c` | registers and interrupt lines only; no VGIC yet |

`qvm/qvm-arch.h` is the line between them.  QVM's core -- descriptors, memory
slots, the run loop, exits, signals -- is genuinely architecture independent,
because that much of KVM's API is; what is not is the vCPU's state and how
interrupts reach it.

`include/qvm/qvm.h` is the public header.  Clients also need the KVM UAPI
headers, and on a non-Linux host the few kernel definitions they rest on:

```
-isystem <qemu>/qvm/linux-compat-x86     # or linux-compat-arm64
-isystem <qemu>/qvm/linux-compat
-isystem <qemu>/linux-headers
```

Put the architecture directory first; it decides which guest ABI
`<asm/kvm.h>` describes.

## Environment

| Variable  | Default    | Meaning                              |
|-----------|------------|--------------------------------------|
| `QVM_CPU` | `max`      | CPU model to emulate                 |
| `QVM_LOG` | unset      | QEMU `-d` categories, e.g. `int,mmu` |

`QVM_CPU` matters more than it looks.  A client describes the CPU it wants the
guest to see itself (on x86, with `KVM_SET_CPUID2`), and anything it
advertises that the emulated CPU cannot execute becomes an
undefined-instruction trap in the guest -- exactly as it would have to be
backed by the host CPU under KVM.  Hence the most capable model by default.

## Instrumenting the guest

`qvm_load_plugin()` loads a QEMU TCG plugin into the emulator running the
guest.  This is the one thing QVM offers that the API it emulates cannot: under
KVM the guest executes on real hardware and there is nothing to instrument,
while under QVM every guest instruction is translated first.

```c
qvm_load_plugin("/path/to/libbbv.so", "outfile=bbv,interval=100000");
```

Arguments are QEMU's `-plugin` syntax, so what works on a QEMU command line
works here.  The call is accepted before or after the guest starts; a plugin
loaded once it is running discards already-translated code so that it is
instrumented on its next execution.

QEMU must be configured with `--enable-plugins`.  Note that this also changes
how libqvm is linked: emulators restrict their exported symbols to the plugin
API, which libqvm cannot do -- it has to export its own entry points too -- so
that restriction is dropped for it.

## Testing

- `kvm-hello-world/` — the classic four-mode smoke test (real, protected,
  32-bit paging, long mode).
- `qvm/tests/` — a conformance test that reproduces gem5's KvmCPU call sequence
  request for request.  See `qvm/gem5/` for running gem5 itself against QVM.
- `gem5/configs/example/qvm-plugin-bbv.py` — runs a workload under gem5's
  `X86KvmCPU` with `contrib/plugins/bbv.c` attached, collecting a basic block
  vector from a guest a KVM CPU could not otherwise see inside.

## Current limits

- One VM per process, since QEMU's machine and address spaces are process-wide
  singletons.  A second `KVM_CREATE_VM` fails with `EBUSY`.
- Guest is x86; the host can be anything QEMU targets.
- Implemented: `KVM_GET_API_VERSION`, `KVM_CHECK_EXTENSION`, `KVM_CREATE_VM`,
  `KVM_GET_VCPU_MMAP_SIZE`, `KVM_GET_SUPPORTED_CPUID`,
  `KVM_GET_MSR_INDEX_LIST`, `KVM_SET_USER_MEMORY_REGION`, `KVM_SET_TSS_ADDR`,
  `KVM_SET_IDENTITY_MAP_ADDR`, `KVM_REGISTER_COALESCED_MMIO` (accepted, no
  ring), `KVM_CREATE_VCPU`, `KVM_RUN`, `KVM_SET_SIGNAL_MASK`, `KVM_INTERRUPT`,
  `KVM_NMI`, and `KVM_GET`/`KVM_SET` for `REGS`, `SREGS`, `FPU`, `XSAVE`,
  `XCRS`, `DEBUGREGS`, `VCPU_EVENTS`, `MSRS` and `CPUID2`.  Anything else
  returns `ENOTTY`.
- Exits reported: `KVM_EXIT_IO`, `KVM_EXIT_MMIO`, `KVM_EXIT_HLT`,
  `KVM_EXIT_INTR`, `KVM_EXIT_IRQ_WINDOW_OPEN`, `KVM_EXIT_DEBUG`,
  `KVM_EXIT_SHUTDOWN`, `KVM_EXIT_INTERNAL_ERROR`.
- `KVM_GET_SUPPORTED_CPUID` needs a vCPU to exist first and returns `EAGAIN`
  before that: QVM's "host CPU" is the emulated model, whose feature words are
  only filled in when a CPU is realized.
- The MSRs QVM can translate are the ones `KVM_GET_MSR_INDEX_LIST` reports;
  `KVM_GET_MSRS`/`KVM_SET_MSRS` stop at the first index outside that set and
  return how many they handled, as KVM does.
- No dirty-log, no `KVM_SET_GUEST_DEBUG`, no nested virtualisation, no
  `KVM_CREATE_IRQCHIP`/`KVM_IRQ_LINE`, no ioeventfd/irqfd.
- `qvm_load_plugin()` needs a QEMU configured with `--enable-plugins`; without
  it the call compiles but does nothing.
- Descriptors are not host file descriptors and cannot be closed, polled or
  inherited as such; `KVM_CREATE_VM`/`KVM_CREATE_VCPU` results are only valid
  as arguments to the `qvm_*` functions.
- Request numbers are truncated to 32 bits on entry, as the kernel's syscall
  layer does, so clients holding them in an `int` work.  For requests encoded
  with `_IO()` the argument is read as a 32-bit scalar; KVM has no such request
  that carries a wider value.
