# sdk/Makefile - build the Tessera plugin SDK static library (Issue #38).
#
# Produces libtessera.a from the SDK sources with a stock aarch64 bare-metal
# toolchain.  Override CROSS_COMPILE for your toolchain triple, e.g.:
#
#     make CROSS_COMPILE=aarch64-linux-gnu-
#
# The default targets a bare-metal cross toolchain (aarch64-none-elf-).

CROSS_COMPILE ?= aarch64-none-elf-
CC  = $(CROSS_COMPILE)gcc
AR  = $(CROSS_COMPILE)ar

# Freestanding, position-independent, no libc - the plugin ABI build flags.
CFLAGS = -mcpu=cortex-a72 -ffreestanding -fPIC -O2 -std=c11 -Wall -Wextra -I.

LIB     = libtessera.a
SOURCES = lib/tessera_math.c lib/tessera_param.c
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean
all: $(LIB)

$(LIB): $(OBJECTS)
	$(AR) rcs $@ $(OBJECTS)

lib/%.o: lib/%.c tessera.h plugin_abi.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(LIB)
