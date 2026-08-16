# Tests for the KVM API surface gem5's KvmCPU uses.
#
# There is one test per guest architecture, and each links against the libqvm
# built for that guest: the ABI a client compiles against comes from the
# qvm/linux-compat-<arch> directory matching the QEMU build it links to.

QEMU_SRC ?= ../..
X86_BUILD ?= $(QEMU_SRC)/build
ARM_BUILD ?= $(QEMU_SRC)/build-arm

# Variable-length KVM structs are the API's own idiom, and gem5 builds them
# the same way.
CFLAGS = -Wall -Wextra -O2 -g -Wno-gnu-variable-sized-type-not-at-end
CPPFLAGS = -I$(QEMU_SRC)/include -isystem $(QEMU_SRC)/linux-headers
LDLIBS = -lqvm -lpthread

x86_CPPFLAGS = -isystem $(QEMU_SRC)/qvm/linux-compat-x86 \
	       -isystem $(QEMU_SRC)/qvm/linux-compat
arm_CPPFLAGS = -isystem $(QEMU_SRC)/qvm/linux-compat-arm64 \
	       -isystem $(QEMU_SRC)/qvm/linux-compat

x86_LDFLAGS = -L$(X86_BUILD) -Wl,-rpath,$(abspath $(X86_BUILD))
arm_LDFLAGS = -L$(ARM_BUILD) -Wl,-rpath,$(abspath $(ARM_BUILD))

# The guest images are guest-architecture ELF whatever the host is.
GUEST_LD ?= ld.lld
x86_GUEST_CC ?= $(CC) --target=i386-unknown-none-elf
arm_GUEST_CC ?= $(CC) --target=aarch64-unknown-none-elf

.PHONY: all
all: gem5-api-test arm-smoke-test icount-test plugin-halt-test halt-plugin.dylib

.PHONY: run
run: run-x86 run-arm

.PHONY: run-x86
run-x86: gem5-api-test icount-test plugin-halt-test halt-plugin.dylib
	./gem5-api-test
	./icount-test
	./plugin-halt-test ./halt-plugin.dylib

.PHONY: run-arm
run-arm: arm-smoke-test
	./arm-smoke-test

# ---------------------------------------------------------------- x86

gem5-api-test: gem5-api-test.o payload.o
	$(CC) $(x86_LDFLAGS) $^ -o $@ $(LDLIBS)

gem5-api-test.o: gem5-api-test.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(x86_CPPFLAGS) -c -o $@ $<

icount-test: icount-test.o icount-payload.o
	$(CC) $(x86_LDFLAGS) $^ -o $@ $(LDLIBS)

icount-test.o: icount-test.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(x86_CPPFLAGS) -c -o $@ $<

icount-payload.o: icount-payload.S icount-guest.bin
	$(CC) -c -o $@ $<

icount-guest.o: icount-guest.S
	$(x86_GUEST_CC) -c -o $@ $<

icount-guest.bin: icount-guest.o guest.ld
	$(GUEST_LD) -T guest.ld $< -o $@

plugin-halt-test: plugin-halt-test.o icount-payload.o
	$(CC) $(x86_LDFLAGS) $^ -o $@ $(LDLIBS)

plugin-halt-test.o: plugin-halt-test.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(x86_CPPFLAGS) -c -o $@ $<

# A plugin resolves the qemu_plugin_* API from whoever loaded it, which for a
# QVM client is libqvm itself -- hence the deferred lookup.
halt-plugin.dylib: halt-plugin.c
	$(CC) -shared -fPIC -o $@ $< -I$(QEMU_SRC)/include/plugins \
	      $(shell pkg-config --cflags glib-2.0) \
	      $(shell pkg-config --libs glib-2.0) \
	      -Wl,-undefined,dynamic_lookup

payload.o: payload.S guest.bin
	$(CC) -c -o $@ $<

guest.o: guest.S
	$(x86_GUEST_CC) -c -o $@ $<

guest.bin: guest.o guest.ld
	$(GUEST_LD) -T guest.ld $< -o $@

# ---------------------------------------------------------------- arm

arm-smoke-test: arm-smoke-test.o arm-payload.o
	$(CC) $(arm_LDFLAGS) $^ -o $@ $(LDLIBS)

arm-smoke-test.o: arm-smoke-test.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(arm_CPPFLAGS) -c -o $@ $<

arm-payload.o: arm-payload.S arm-guest.bin
	$(CC) -c -o $@ $<

arm-guest.o: arm-guest.S
	$(arm_GUEST_CC) -c -o $@ $<

arm-guest.bin: arm-guest.o guest.ld
	$(GUEST_LD) -T guest.ld $< -o $@

.PHONY: clean
clean:
	$(RM) gem5-api-test gem5-api-test.o payload.o guest.o guest.bin \
	      arm-smoke-test arm-smoke-test.o arm-payload.o arm-guest.o \
	      arm-guest.bin icount-test icount-test.o icount-payload.o \
	      icount-guest.o icount-guest.bin plugin-halt-test \
	      plugin-halt-test.o halt-plugin.dylib
