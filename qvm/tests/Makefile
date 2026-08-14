# Conformance test for the KVM API surface gem5's KvmCPU uses.

QEMU_SRC ?= ../..
QEMU_BUILD ?= $(QEMU_SRC)/build

# Variable-length KVM structs are the API's own idiom, and gem5 builds them
# the same way.
CFLAGS = -Wall -Wextra -O2 -g -Wno-gnu-variable-sized-type-not-at-end
CPPFLAGS = -I$(QEMU_SRC)/include \
	   -isystem $(QEMU_SRC)/qvm/linux-compat \
	   -isystem $(QEMU_SRC)/linux-headers
LDFLAGS = -L$(QEMU_BUILD) -Wl,-rpath,$(abspath $(QEMU_BUILD))
LDLIBS = -lqvm -lpthread

# The guest image is x86 ELF whatever the host is.
GUEST_CC ?= $(CC) --target=i386-unknown-none-elf
GUEST_LD ?= ld.lld

.PHONY: run
run: gem5-api-test
	./gem5-api-test

gem5-api-test: gem5-api-test.o payload.o
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

payload.o: payload.S guest.bin
	$(CC) -c -o $@ $<

guest.o: guest.S
	$(GUEST_CC) -c -o $@ $<

guest.bin: guest.o guest.ld
	$(GUEST_LD) -T guest.ld $< -o $@

.PHONY: clean
clean:
	$(RM) gem5-api-test gem5-api-test.o payload.o guest.o guest.bin
