# Building Tessera for ARM (AArch64)

Tessera targets the **Raspberry Pi CM4 / Pi 4** (BCM2711, quad-core
Cortex-A72, ARMv8-A). This page covers the cross-compilation toolchain and the
bare-metal build that produces a `kernel8.img` the Pi firmware can load.

> Status: this is the **Issue #1** build skeleton. The image currently boots to
> a minimal entry stub (`boot/start.S` -> `kmain`) that parks the core. The
> EL2->EL1 transition, BSS clear, UART console, and everything above it land in
> issues #2 onward.

## 1. Toolchain

You do **not** need a separate AArch64 GCC. Tessera builds with **LLVM/clang**,
which is a native cross-compiler, plus `ld.lld` and `llvm-objcopy`.

### Option A - LLVM/clang (default)

Ubuntu / Debian:

```sh
sudo apt-get update
sudo apt-get install -y clang lld llvm binutils
# or, equivalently:
make arm-install-deps
```

`binutils` is only needed for `readelf` (used by the build to print the ELF
header); clang/lld/llvm-objcopy do the actual compiling, linking, and image
flattening.

### Option B - GNU cross toolchain

If you prefer GCC, install a bare-metal or Linux AArch64 toolchain and pass its
prefix via `CROSS_COMPILE`:

```sh
# Bare-metal (recommended), from the Arm GNU Toolchain downloads:
make arm CROSS_COMPILE=aarch64-none-elf-

# Or the Debian/Ubuntu Linux toolchain (works for freestanding too):
sudo apt-get install -y gcc-aarch64-linux-gnu
make arm CROSS_COMPILE=aarch64-linux-gnu-
```

## 2. Build

```sh
make arm
```

Output (under `build/arm/`):

| File           | Description                                            |
| -------------- | ------------------------------------------------------ |
| `kernel8.elf`  | Linked kernel with symbols (for debugging / `gdb`).    |
| `kernel8.img`  | Raw binary the Pi firmware loads at `0x80000`.          |

Clean just the ARM artifacts with `make arm-clean` (or `make clean` to remove
the whole `build/` tree).

## 3. Verify

The `make arm` target prints the ELF header on success. You can confirm it by
hand:

```sh
readelf -h build/arm/kernel8.elf | grep -E 'Class|Machine|Entry'
# Class:   ELF64
# Machine: AArch64
# Entry point address: 0x80000
```

Disassemble the entry point (the first instruction of the raw image must be
`_start`):

```sh
llvm-objdump -d build/arm/kernel8.elf | head -20
```

## 4. Compiler / linker flags

Set in the `Makefile` ARM section:

- `-mcpu=cortex-a72` - tune for the CM4 / Pi 4 core.
- `-ffreestanding -nostdlib` semantics - no hosted C runtime or libc.
- `-mgeneral-regs-only` - kernel C code must not implicitly use FP/SIMD
  registers (NEON save/restore is added later, in issue #15).
- `-fno-stack-protector -fno-pic -fno-pie` - no stack canaries, fixed link
  address (no position-independent code).
- Link script: [`arch/arm64/kernel.ld`](../arch/arm64/kernel.ld), which fixes
  the load address at `0x80000` and exports `__bss_start`, `__bss_end`, and
  `__stack_top` for the boot code.

## 5. Running

- **QEMU:** a `qemu-system-aarch64 -machine raspi4b` boot-smoke target is added
  in **issue #5** (CI). `qemu-system-aarch64` is not required to build.
- **Hardware:** copy `build/arm/kernel8.img` **and** [`boot/config.txt`](../boot/config.txt)
  onto the FAT boot partition of a Pi 4 / CM4 SD/eMMC card. The provided
  `config.txt` sets `arm_64bit=1`, loads the kernel at `0x80000`, and enables the
  PL011 UART0 console (GPIO 14/15, 115200 8N1). Attach a 3.3 V USB-serial adapter
  to those pins and you should see the boot banner. The console driver is
  `drivers/uart_pl011.c`; the VideoCore clock/board-revision handshake used at
  boot is the property mailbox, `drivers/mailbox.c` (issue #105).

### VideoCore mailbox

The ARM cores configure the UART and I2S clocks - and read the board revision -
by posting a **property message** to the VideoCore firmware over the mailbox
(`drivers/mailbox.c`). A message is a tag list (`get board revision`, `set the PCM
clock to 48000*256`, ...); the message *format* is pure serialisation and is
host-tested with `make test-arm-mailbox`, while `mbox_call` is the one-register
hardware doorbell used on the board.

### raspi4b under QEMU

QEMU >= 9.0 emulates the BCM2711 as `-machine raspi4b`. When available it is used
for a boot-smoke in CI (issue #107); to run it locally:

```
qemu-system-aarch64 -machine raspi4b -nographic -serial stdio \
    -no-reboot -kernel build/arm/kernel8.img
```

Older QEMU builds do not provide the `raspi4b` machine; the CI step skips itself
with a notice in that case.

## Layout

```
arch/arm64/
  kernel.ld     Linker script (load address, sections, stack/bss symbols)
  main.c        kmain() - minimal C entry (skeleton)
boot/
  start.S       _start - AArch64 entry stub (parks secondaries, sets SP)
```
