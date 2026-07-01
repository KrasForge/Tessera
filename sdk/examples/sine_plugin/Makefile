# sdk/examples/sine_plugin/Makefile - build the example plugin with only the
# SDK and an aarch64 bare-metal toolchain (Issue #38).
#
#   make                                    # -> sine_plugin.elf
#   make CROSS_COMPILE=aarch64-linux-gnu-   # use a different toolchain triple
#
# No dependencies beyond the SDK in ../.. and the cross compiler.

PLUGIN = sine_plugin
SDK    = ../..

CROSS_COMPILE ?= aarch64-none-elf-
CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)ld

CFLAGS   = -mcpu=cortex-a72 -ffreestanding -fPIC -O2 -std=c11 -Wall -Wextra -I$(SDK)
LDSCRIPT = $(SDK)/tessera.ld

.PHONY: all clean
all: $(PLUGIN).elf

$(SDK)/libtessera.a:
	$(MAKE) -C $(SDK) CROSS_COMPILE=$(CROSS_COMPILE)

$(PLUGIN).o: $(PLUGIN).c
	$(CC) $(CFLAGS) -c $< -o $@

$(PLUGIN).elf: $(PLUGIN).o $(SDK)/libtessera.a
	$(LD) -T $(LDSCRIPT) -o $@ $(PLUGIN).o $(SDK)/libtessera.a

clean:
	rm -f $(PLUGIN).o $(PLUGIN).elf
