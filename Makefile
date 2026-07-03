# IKOS Kernel Makefile
# Builds the bootloader and kernel with comprehensive system components

# Assembler and tools
ASM = nasm
CC = gcc
LD = ld
QEMU = qemu-system-x86_64

# Compiler flags
CFLAGS = -m64 -ffreestanding -mcmodel=large -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -Wall -Wextra -std=c99 -O2 -g -Iinclude/
ASMFLAGS = -f elf64 -g -Iinclude/
LDFLAGS = -T kernel/kernel.ld -nostdlib

# Directories
BOOT_DIR = boot
KERNEL_DIR = kernel
TESTS_DIR = tests
BUILD_DIR = build
INCLUDE_DIR = include

# Kernel source files
KERNEL_C_SOURCES = $(wildcard $(KERNEL_DIR)/*.c)
KERNEL_ASM_SOURCES = $(wildcard $(KERNEL_DIR)/*.asm)
KERNEL_OBJECTS = $(patsubst $(KERNEL_DIR)/%.c,$(BUILD_DIR)/%.o,$(KERNEL_C_SOURCES)) \
                 $(patsubst $(KERNEL_DIR)/%.asm,$(BUILD_DIR)/%.o,$(KERNEL_ASM_SOURCES))

# Test source files
TEST_C_SOURCES = $(wildcard $(TESTS_DIR)/*.c)
TEST_OBJECTS = $(patsubst $(TESTS_DIR)/%.c,$(BUILD_DIR)/test_%.o,$(TEST_C_SOURCES))

# VMM specific files
VMM_SOURCES = $(KERNEL_DIR)/vmm.c $(KERNEL_DIR)/vmm_cow.c $(KERNEL_DIR)/vmm_regions.c \
              $(KERNEL_DIR)/vmm_interrupts.c $(KERNEL_DIR)/vmm_asm.asm
VMM_OBJECTS = $(BUILD_DIR)/vmm.o $(BUILD_DIR)/vmm_cow.o $(BUILD_DIR)/vmm_regions.o \
              $(BUILD_DIR)/vmm_interrupts.o $(BUILD_DIR)/vmm_asm.o

# Interrupt handling specific files
INTERRUPT_SOURCES = $(KERNEL_DIR)/idt.c $(KERNEL_DIR)/interrupt_handlers.c \
                    $(KERNEL_DIR)/interrupt_stubs.asm
INTERRUPT_OBJECTS = $(BUILD_DIR)/idt.o $(BUILD_DIR)/interrupt_handlers.o \
                    $(BUILD_DIR)/interrupt_stubs.o

# User-space process execution specific files
USERSPACE_SOURCES = $(KERNEL_DIR)/process.c $(KERNEL_DIR)/elf_loader.c \
                    $(KERNEL_DIR)/syscalls.c $(KERNEL_DIR)/user_mode.asm
USERSPACE_OBJECTS = $(BUILD_DIR)/process.o $(BUILD_DIR)/elf_loader.o \
                    $(BUILD_DIR)/syscalls.o $(BUILD_DIR)/user_mode.o

# Process Manager specific files
PROCESS_MANAGER_SOURCES = $(KERNEL_DIR)/process_manager.c $(KERNEL_DIR)/pm_syscalls.c $(KERNEL_DIR)/string_utils.c
PROCESS_MANAGER_OBJECTS = $(BUILD_DIR)/process_manager.o $(BUILD_DIR)/pm_syscalls.o $(BUILD_DIR)/string_utils.o

# Virtual File System specific files
VFS_SOURCES = $(KERNEL_DIR)/vfs.c $(KERNEL_DIR)/ramfs.c
VFS_OBJECTS = $(BUILD_DIR)/vfs.o $(BUILD_DIR)/ramfs.o

# FAT Filesystem specific files
FAT_SOURCES = $(KERNEL_DIR)/fat.c $(KERNEL_DIR)/ramdisk.c
FAT_OBJECTS = $(BUILD_DIR)/fat.o $(BUILD_DIR)/ramdisk.o

# Keyboard Driver specific files
KEYBOARD_SOURCES = $(KERNEL_DIR)/keyboard.c $(KERNEL_DIR)/keyboard_syscalls.c \
                   $(KERNEL_DIR)/keyboard_user_api.c
KEYBOARD_OBJECTS = $(BUILD_DIR)/keyboard.o $(BUILD_DIR)/keyboard_syscalls.o \
                   $(BUILD_DIR)/keyboard_user_api.o

# Advanced Memory Management specific files - Issue #27
MEMORY_ADVANCED_SOURCES = $(KERNEL_DIR)/buddy_allocator.c $(KERNEL_DIR)/slab_allocator.c \
                          $(KERNEL_DIR)/demand_paging.c $(KERNEL_DIR)/memory_compression.c \
                          $(KERNEL_DIR)/numa_allocator.c $(KERNEL_DIR)/advanced_memory_manager.c
MEMORY_ADVANCED_OBJECTS = $(BUILD_DIR)/buddy_allocator.o $(BUILD_DIR)/slab_allocator.o \
                          $(BUILD_DIR)/demand_paging.o $(BUILD_DIR)/memory_compression.o \
                          $(BUILD_DIR)/numa_allocator.o $(BUILD_DIR)/advanced_memory_manager.o

# Authentication & Authorization System specific files - Issue #31
AUTH_SOURCES = $(KERNEL_DIR)/auth_core.c $(KERNEL_DIR)/auth_authorization.c \
               $(KERNEL_DIR)/auth_mfa.c
AUTH_OBJECTS = $(BUILD_DIR)/auth_core.o $(BUILD_DIR)/auth_authorization.o \
               $(BUILD_DIR)/auth_mfa.o
AUTH_LIBS = -lssl -lcrypto -lpthread

# User Input Handling System specific files - Issue #38
INPUT_SOURCES = $(KERNEL_DIR)/input_manager.c $(KERNEL_DIR)/input_events.c \
                $(KERNEL_DIR)/input_keyboard.c $(KERNEL_DIR)/input_mouse.c \
                $(KERNEL_DIR)/input_api.c
INPUT_OBJECTS = $(BUILD_DIR)/input_manager.o $(BUILD_DIR)/input_events.o \
                $(BUILD_DIR)/input_keyboard.o $(BUILD_DIR)/input_mouse.o \
                $(BUILD_DIR)/input_api.o

# Network stack source files (including TCP/IP)
NETWORK_SOURCES = $(KERNEL_DIR)/net/network_core.c $(KERNEL_DIR)/net/ethernet.c $(KERNEL_DIR)/net/udp.c $(KERNEL_DIR)/net/tcp.c
NETWORK_OBJECTS = $(BUILD_DIR)/network_core.o $(BUILD_DIR)/ethernet.o $(BUILD_DIR)/udp.o $(BUILD_DIR)/tcp.o

# TCP/IP source files 
TCPIP_SOURCES = $(KERNEL_DIR)/net/udp.c $(KERNEL_DIR)/net/tcp.c
TCPIP_OBJECTS = $(BUILD_DIR)/udp.o $(BUILD_DIR)/tcp.o

# Files
BOOT_ASM = $(BOOT_DIR)/boot.asm
BOOT_ENHANCED_ASM = $(BOOT_DIR)/boot_enhanced.asm
BOOT_COMPACT_ASM = $(BOOT_DIR)/boot_compact.asm
BOOT_PROTECTED_ASM = $(BOOT_DIR)/boot_protected.asm
BOOT_PROTECTED_COMPACT_ASM = $(BOOT_DIR)/boot_protected_compact.asm
BOOT_ELF_LOADER_ASM = $(BOOT_DIR)/boot_elf_loader.asm
BOOT_ELF_COMPACT_ASM = $(BOOT_DIR)/boot_elf_compact.asm
BOOT_LONGMODE_ASM = $(BOOT_DIR)/boot_longmode.asm
BOOT_BIN = $(BUILD_DIR)/boot.bin
BOOT_ENHANCED_BIN = $(BUILD_DIR)/boot_enhanced.bin
BOOT_COMPACT_BIN = $(BUILD_DIR)/boot_compact.bin
BOOT_PROTECTED_BIN = $(BUILD_DIR)/boot_protected.bin
BOOT_PROTECTED_COMPACT_BIN = $(BUILD_DIR)/boot_protected_compact.bin
BOOT_ELF_LOADER_BIN = $(BUILD_DIR)/boot_elf_loader.bin
BOOT_ELF_COMPACT_BIN = $(BUILD_DIR)/boot_elf_compact.bin
BOOT_LONGMODE_BIN = $(BUILD_DIR)/boot_longmode.bin
DISK_IMG = $(BUILD_DIR)/ikos.img
DISK_ENHANCED_IMG = $(BUILD_DIR)/ikos_enhanced.img
DISK_COMPACT_IMG = $(BUILD_DIR)/ikos_compact.img
DISK_PROTECTED_IMG = $(BUILD_DIR)/ikos_protected.img
DISK_PROTECTED_COMPACT_IMG = $(BUILD_DIR)/ikos_protected_compact.img
DISK_ELF_LOADER_IMG = $(BUILD_DIR)/ikos_elf_loader.img
DISK_ELF_COMPACT_IMG = $(BUILD_DIR)/ikos_elf_compact.img
DISK_LONGMODE_IMG = $(BUILD_DIR)/ikos_longmode.img

# Default target - build kernel with VMM, interrupt handling, user-space support, process manager, VFS, FAT filesystem, keyboard driver, advanced memory management, authentication system, network stack, and TCP/IP protocols
all: $(BUILD_DIR)/kernel.elf $(BUILD_DIR)/vmm_test $(BUILD_DIR)/interrupt_test $(BUILD_DIR)/userspace_test $(BUILD_DIR)/process_manager_test $(BUILD_DIR)/vfs_test $(BUILD_DIR)/fat_test $(BUILD_DIR)/keyboard_test $(BUILD_DIR)/advanced_memory_test $(BUILD_DIR)/auth_test $(BUILD_DIR)/network_test $(BUILD_DIR)/tcpip_test $(DISK_LONGMODE_IMG)

# Kernel ELF binary
KERNEL_ELF = $(BUILD_DIR)/kernel.elf

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# =============================================================================
# KERNEL BUILD RULES
# =============================================================================

# Build kernel ELF
$(BUILD_DIR)/kernel.elf: $(KERNEL_OBJECTS) | $(BUILD_DIR)
	$(LD) $(LDFLAGS) $(KERNEL_OBJECTS) -o $@

# Compile C source files
$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile assembly files
$(BUILD_DIR)/%.o: $(KERNEL_DIR)/%.asm | $(BUILD_DIR)
	$(ASM) $(ASMFLAGS) $< -o $@

# Compile network stack files
$(BUILD_DIR)/network_core.o: $(KERNEL_DIR)/net/network_core.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build UDP protocol object
$(BUILD_DIR)/udp.o: $(KERNEL_DIR)/net/udp.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build TCP protocol object
$(BUILD_DIR)/tcp.o: $(KERNEL_DIR)/net/tcp.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ethernet.o: $(KERNEL_DIR)/net/ethernet.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile test files
$(BUILD_DIR)/test_%.o: $(TESTS_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Build VMM test executable
$(BUILD_DIR)/vmm_test: $(VMM_OBJECTS) $(BUILD_DIR)/test_vmm.o | $(BUILD_DIR)
	$(CC) -o $@ $^ -nostdlib -lgcc

# Build interrupt handling test executable
$(BUILD_DIR)/interrupt_test: $(INTERRUPT_OBJECTS) $(BUILD_DIR)/test_interrupts.o | $(BUILD_DIR)
	$(CC) -o $@ $^ -nostdlib -lgcc

# Build user-space process execution test executable
$(BUILD_DIR)/userspace_test: $(USERSPACE_OBJECTS) $(BUILD_DIR)/test_user_space.o | $(BUILD_DIR)
	$(CC) -o $@ $^ -nostdlib -lgcc -no-pie

# Build process manager test executable
$(BUILD_DIR)/process_manager_test: $(PROCESS_MANAGER_OBJECTS) $(BUILD_DIR)/test_stubs.o $(TESTS_DIR)/test_process_manager.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(PROCESS_MANAGER_OBJECTS) $(BUILD_DIR)/test_stubs.o $(TESTS_DIR)/test_process_manager.c -nostdlib -lgcc -no-pie

# Build VFS test executable
$(BUILD_DIR)/vfs_test: $(VFS_OBJECTS) $(BUILD_DIR)/test_vfs.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(VFS_OBJECTS) $(BUILD_DIR)/test_vfs.o -nostdlib -lgcc -no-pie

# Build FAT filesystem test executable
$(BUILD_DIR)/fat_test: $(FAT_OBJECTS) $(VFS_OBJECTS) $(BUILD_DIR)/test_fat.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(FAT_OBJECTS) $(VFS_OBJECTS) $(BUILD_DIR)/test_fat.o -nostdlib -lgcc -no-pie

# Build keyboard driver test executable
$(BUILD_DIR)/keyboard_test: $(KEYBOARD_OBJECTS) $(BUILD_DIR)/test_test_keyboard.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(KEYBOARD_OBJECTS) $(BUILD_DIR)/test_test_keyboard.o -nostdlib -lgcc -no-pie

# Build input system test executable
$(BUILD_DIR)/input_test: $(INPUT_OBJECTS) $(BUILD_DIR)/test_input.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(INPUT_OBJECTS) $(BUILD_DIR)/test_input.o -nostdlib -lgcc -no-pie

# Build advanced memory management test executable
$(BUILD_DIR)/advanced_memory_test: $(MEMORY_ADVANCED_OBJECTS) $(BUILD_DIR)/test_advanced_memory.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(MEMORY_ADVANCED_OBJECTS) $(BUILD_DIR)/test_advanced_memory.o -nostdlib -lgcc -no-pie

# Build authentication & authorization system test executable
$(BUILD_DIR)/auth_test: $(AUTH_OBJECTS) $(BUILD_DIR)/auth_test.o | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $(AUTH_OBJECTS) $(KERNEL_DIR)/auth_test.c $(AUTH_LIBS) -no-pie

# Build network stack test executable
# Network test (user-space standalone)
$(BUILD_DIR)/network_test: tests/network_test_standalone.c | $(BUILD_DIR)
	gcc -o $@ tests/network_test_standalone.c

$(BUILD_DIR)/network_test.o: tests/network_test_standalone.c | $(BUILD_DIR)
	gcc -c -o $@ tests/network_test_standalone.c

# TCP/IP test (user-space standalone)
$(BUILD_DIR)/tcpip_test: tests/tcpip_test.c | $(BUILD_DIR)
	gcc -Iinclude -Iinclude/net -o $@ tests/tcpip_test.c

$(BUILD_DIR)/tcpip_test.o: tests/tcpip_test.c | $(BUILD_DIR)
	gcc -Iinclude -Iinclude/net -c -o $@ tests/tcpip_test.c

# =============================================================================
# VMM SPECIFIC TARGETS
# =============================================================================

# Build VMM only
vmm: $(VMM_OBJECTS)
	@echo "VMM components built successfully"

# Run VMM tests
test-vmm: $(BUILD_DIR)/vmm_test
	$(BUILD_DIR)/vmm_test

# VMM smoke test
vmm-smoke: $(BUILD_DIR)/vmm_test
	$(BUILD_DIR)/vmm_test smoke

# =============================================================================
# INTERRUPT HANDLING TARGETS
# =============================================================================

# Build interrupt handling only
interrupts: $(INTERRUPT_OBJECTS)
	@echo "Interrupt handling components built successfully"

# Run interrupt tests
test-interrupts: $(BUILD_DIR)/interrupt_test
	$(BUILD_DIR)/interrupt_test

# Interrupt smoke test
interrupt-smoke: $(BUILD_DIR)/interrupt_test
	$(BUILD_DIR)/interrupt_test smoke

# =============================================================================
# USER-SPACE PROCESS EXECUTION TARGETS
# =============================================================================

# Build user-space process execution only
userspace: $(USERSPACE_OBJECTS)
	@echo "User-space process execution components built successfully"

# Run user-space tests
test-userspace: $(BUILD_DIR)/userspace_test
	$(BUILD_DIR)/userspace_test

# User-space smoke test
userspace-smoke: $(BUILD_DIR)/userspace_test
	$(BUILD_DIR)/userspace_test smoke

# =============================================================================
# PROCESS MANAGER TARGETS
# =============================================================================

# Build process manager only
process-manager: $(PROCESS_MANAGER_OBJECTS) $(USERSPACE_OBJECTS)
	@echo "Process manager components built successfully"

# Run process manager tests
test-process-manager: $(BUILD_DIR)/process_manager_test
	$(BUILD_DIR)/process_manager_test

# Process manager smoke test
process-manager-smoke: $(BUILD_DIR)/process_manager_test
	$(BUILD_DIR)/process_manager_test smoke

# =============================================================================
# VFS TARGETS
# =============================================================================

# Build VFS only
vfs: $(VFS_OBJECTS)
	@echo "VFS components built successfully"

# Run VFS tests
test-vfs: $(BUILD_DIR)/vfs_test
	$(BUILD_DIR)/vfs_test

# VFS smoke test
vfs-smoke: $(BUILD_DIR)/vfs_test
	$(BUILD_DIR)/vfs_test smoke

# =============================================================================
# FAT FILESYSTEM TARGETS
# =============================================================================

# Build FAT filesystem only
fat: $(FAT_OBJECTS) $(VFS_OBJECTS)
	@echo "FAT filesystem components built successfully"

# Run FAT filesystem tests
test-fat: $(BUILD_DIR)/fat_test
	$(BUILD_DIR)/fat_test

# FAT filesystem smoke test
fat-smoke: $(BUILD_DIR)/fat_test
	$(BUILD_DIR)/fat_test smoke

# =============================================================================
# KEYBOARD DRIVER TARGETS
# =============================================================================

# Build keyboard driver only
keyboard: $(KEYBOARD_OBJECTS)
	@echo "Keyboard driver components built successfully"

# Run keyboard tests
test-keyboard: $(BUILD_DIR)/keyboard_test
	$(BUILD_DIR)/keyboard_test

# Keyboard smoke test
keyboard-smoke: $(BUILD_DIR)/keyboard_test
	$(BUILD_DIR)/keyboard_test smoke

# Keyboard hardware test (requires actual keyboard)
keyboard-hardware: $(BUILD_DIR)/keyboard_test
	$(BUILD_DIR)/keyboard_test hardware

# Run input system tests
test-input: $(BUILD_DIR)/input_test
	$(BUILD_DIR)/input_test

# Input system smoke test
input-smoke: $(BUILD_DIR)/input_test
	$(BUILD_DIR)/input_test smoke

# =============================================================================
# ADVANCED MEMORY MANAGEMENT TARGETS - Issue #27
# =============================================================================

# Build advanced memory management only
memory-advanced: $(MEMORY_ADVANCED_OBJECTS)
	@echo "Advanced memory management components built successfully"

# Run advanced memory management tests
test-memory-advanced: $(BUILD_DIR)/advanced_memory_test
	$(BUILD_DIR)/advanced_memory_test

# Advanced memory management smoke test
memory-advanced-smoke: $(BUILD_DIR)/advanced_memory_test
	$(BUILD_DIR)/advanced_memory_test smoke

# Advanced memory management stress test
memory-advanced-stress: $(BUILD_DIR)/advanced_memory_test
	$(BUILD_DIR)/advanced_memory_test stress

# Test individual memory management components
test-buddy: $(BUILD_DIR)/advanced_memory_test
	$(BUILD_DIR)/advanced_memory_test buddy

test-slab: $(BUILD_DIR)/advanced_memory_test
	$(BUILD_DIR)/advanced_memory_test slab

test-numa: $(BUILD_DIR)/advanced_memory_test
	$(BUILD_DIR)/advanced_memory_test numa

test-compression: $(BUILD_DIR)/advanced_memory_test
	$(BUILD_DIR)/advanced_memory_test compression

test-paging: $(BUILD_DIR)/advanced_memory_test
	$(BUILD_DIR)/advanced_memory_test paging

# =============================================================================
# AUTHENTICATION & AUTHORIZATION SYSTEM TARGETS - Issue #31
# =============================================================================

# Build authentication & authorization system only
auth: $(AUTH_OBJECTS)
	@echo "Authentication & authorization system components built successfully"

# Run authentication & authorization tests
test-auth: $(BUILD_DIR)/auth_test
	$(BUILD_DIR)/auth_test

# Authentication system smoke test
auth-smoke: $(BUILD_DIR)/auth_test
	$(BUILD_DIR)/auth_test smoke

# Authentication system security test
auth-security: $(BUILD_DIR)/auth_test
	$(BUILD_DIR)/auth_test security

# Test individual authentication components
test-user-management: $(BUILD_DIR)/auth_test
	$(BUILD_DIR)/auth_test user

test-sessions: $(BUILD_DIR)/auth_test
	$(BUILD_DIR)/auth_test sessions

test-roles: $(BUILD_DIR)/auth_test
	$(BUILD_DIR)/auth_test roles

test-permissions: $(BUILD_DIR)/auth_test
	$(BUILD_DIR)/auth_test permissions

test-mfa: $(BUILD_DIR)/auth_test
	$(BUILD_DIR)/auth_test mfa

test-acl: $(BUILD_DIR)/auth_test
	$(BUILD_DIR)/auth_test acl

# =============================================================================
# NETWORK STACK TARGETS - Issue #35
# =============================================================================

# Build network stack only
network: $(NETWORK_OBJECTS)
	@echo "Network stack components built successfully"

# Run network stack tests
test-network: $(BUILD_DIR)/network_test
	$(BUILD_DIR)/network_test

# Network stack smoke test
network-smoke: $(BUILD_DIR)/network_test
	$(BUILD_DIR)/network_test smoke

# Test network buffer management
test-network-buffers: $(BUILD_DIR)/network_test
	$(BUILD_DIR)/network_test buffers

# Test network device operations
test-network-devices: $(BUILD_DIR)/network_test
	$(BUILD_DIR)/network_test devices

# Test Ethernet layer
test-ethernet: $(BUILD_DIR)/network_test
	$(BUILD_DIR)/network_test ethernet

# Test IP layer
test-ip: $(BUILD_DIR)/network_test
	$(BUILD_DIR)/network_test ip

# Test socket API
test-sockets: $(BUILD_DIR)/network_test
	$(BUILD_DIR)/network_test sockets

# =============================================================================
# TCP/IP PROTOCOL TESTS  
# =============================================================================

# Test TCP/IP protocols
test-tcpip: $(BUILD_DIR)/tcpip_test
	$(BUILD_DIR)/tcpip_test

# TCP/IP smoke test
tcpip-smoke: $(BUILD_DIR)/tcpip_test
	$(BUILD_DIR)/tcpip_test smoke

# Test UDP protocol
test-udp: $(BUILD_DIR)/tcpip_test
	$(BUILD_DIR)/tcpip_test udp

# Test TCP protocol  
test-tcp: $(BUILD_DIR)/tcpip_test
	$(BUILD_DIR)/tcpip_test tcp

# Test socket API
test-socket-api: $(BUILD_DIR)/tcpip_test
	$(BUILD_DIR)/tcpip_test socket

# Test protocol performance
test-protocol-performance: $(BUILD_DIR)/tcpip_test
	$(BUILD_DIR)/tcpip_test performance

# Test error handling
test-protocol-errors: $(BUILD_DIR)/tcpip_test
	$(BUILD_DIR)/tcpip_test errors

# Test protocol integration
test-protocol-integration: $(BUILD_DIR)/tcpip_test
	$(BUILD_DIR)/tcpip_test integration

# =============================================================================
# BOOTLOADER BUILD RULES
# =============================================================================

# Assemble bootloader
$(BOOT_BIN): $(BOOT_ASM) | $(BUILD_DIR)
	$(ASM) -f bin $(BOOT_ASM) -o $(BOOT_BIN)

# Assemble enhanced bootloader
$(BOOT_ENHANCED_BIN): $(BOOT_ENHANCED_ASM) | $(BUILD_DIR)
	$(ASM) -f bin $(BOOT_ENHANCED_ASM) -o $(BOOT_ENHANCED_BIN) -I include/

# Create disk image with basic bootloader
$(DISK_IMG): $(BOOT_BIN)
	dd if=/dev/zero of=$(DISK_IMG) bs=512 count=2880
	dd if=$(BOOT_BIN) of=$(DISK_IMG) bs=512 count=1 conv=notrunc

# Assemble compact bootloader
$(BOOT_COMPACT_BIN): $(BOOT_COMPACT_ASM) | $(BUILD_DIR)
	$(ASM) -f bin $(BOOT_COMPACT_ASM) -o $(BOOT_COMPACT_BIN) -I include/

# Create disk image with compact bootloader
$(DISK_COMPACT_IMG): $(BOOT_COMPACT_BIN)
	dd if=/dev/zero of=$(DISK_COMPACT_IMG) bs=512 count=2880
	dd if=$(BOOT_COMPACT_BIN) of=$(DISK_COMPACT_IMG) bs=512 count=1 conv=notrunc

# Assemble protected mode bootloader
$(BOOT_PROTECTED_BIN): $(BOOT_PROTECTED_ASM) | $(BUILD_DIR)
	$(ASM) -f bin $(BOOT_PROTECTED_ASM) -o $(BOOT_PROTECTED_BIN) -I include/

# Create disk image with protected mode bootloader
$(DISK_PROTECTED_IMG): $(BOOT_PROTECTED_BIN)
	dd if=/dev/zero of=$(DISK_PROTECTED_IMG) bs=512 count=2880
	dd if=$(BOOT_PROTECTED_BIN) of=$(DISK_PROTECTED_IMG) bs=512 count=1 conv=notrunc

# Assemble protected compact mode bootloader
$(BOOT_PROTECTED_COMPACT_BIN): $(BOOT_PROTECTED_COMPACT_ASM) | $(BUILD_DIR)
	$(ASM) -f bin $(BOOT_PROTECTED_COMPACT_ASM) -o $(BOOT_PROTECTED_COMPACT_BIN) -I include/

# Create disk image with protected compact mode bootloader
$(DISK_PROTECTED_COMPACT_IMG): $(BOOT_PROTECTED_COMPACT_BIN)
	dd if=/dev/zero of=$(DISK_PROTECTED_COMPACT_IMG) bs=512 count=2880
	dd if=$(BOOT_PROTECTED_COMPACT_BIN) of=$(DISK_PROTECTED_COMPACT_IMG) bs=512 count=1 conv=notrunc

# Assemble ELF kernel loader
$(BOOT_ELF_LOADER_BIN): $(BOOT_ELF_LOADER_ASM) | $(BUILD_DIR)
	$(ASM) -f bin $(BOOT_ELF_LOADER_ASM) -o $(BOOT_ELF_LOADER_BIN) -I include/

# Create disk image with ELF kernel loader
$(DISK_ELF_LOADER_IMG): $(BOOT_ELF_LOADER_BIN)
	dd if=/dev/zero of=$(DISK_ELF_LOADER_IMG) bs=512 count=2880
	dd if=$(BOOT_ELF_LOADER_BIN) of=$(DISK_ELF_LOADER_IMG) bs=512 count=1 conv=notrunc

# Assemble ELF compact kernel loader
$(BOOT_ELF_COMPACT_BIN): $(BOOT_ELF_COMPACT_ASM) | $(BUILD_DIR)
	$(ASM) -f bin $(BOOT_ELF_COMPACT_ASM) -o $(BOOT_ELF_COMPACT_BIN) -I include/

# Create disk image with ELF compact kernel loader
$(DISK_ELF_COMPACT_IMG): $(BOOT_ELF_COMPACT_BIN)
	dd if=/dev/zero of=$(DISK_ELF_COMPACT_IMG) bs=512 count=2880
	dd if=$(BOOT_ELF_COMPACT_BIN) of=$(DISK_ELF_COMPACT_IMG) bs=512 count=1 conv=notrunc

# Assemble long mode bootloader
$(BOOT_LONGMODE_BIN): $(BOOT_LONGMODE_ASM) | $(BUILD_DIR)
	$(ASM) -f bin $(BOOT_LONGMODE_ASM) -o $(BOOT_LONGMODE_BIN) -I include/

# Create disk image with long mode bootloader
$(DISK_LONGMODE_IMG): $(BOOT_LONGMODE_BIN)
	dd if=/dev/zero of=$(DISK_LONGMODE_IMG) bs=512 count=2880
	dd if=$(BOOT_LONGMODE_BIN) of=$(DISK_LONGMODE_IMG) bs=512 count=1 conv=notrunc

# Debug-enabled long mode bootloader
$(BUILD_DIR)/boot_longmode_debug.bin: boot/boot_longmode_debug.asm | $(BUILD_DIR)
	$(ASM) -f bin boot/boot_longmode_debug.asm -o $(BUILD_DIR)/boot_longmode_debug.bin -I include/

$(BUILD_DIR)/ikos_longmode_debug.img: $(BUILD_DIR)/boot_longmode_debug.bin
	dd if=/dev/zero of=$(BUILD_DIR)/ikos_longmode_debug.img bs=512 count=2880
	dd if=$(BUILD_DIR)/boot_longmode_debug.bin of=$(BUILD_DIR)/ikos_longmode_debug.img bs=512 count=1 conv=notrunc

debug-build: $(BUILD_DIR)/ikos_longmode_debug.img

# Test basic bootloader in QEMU
test: $(DISK_IMG)
	$(QEMU) -drive format=raw,file=$(DISK_IMG) -no-reboot -no-shutdown -nographic

# Test enhanced bootloader in QEMU
test-enhanced: $(DISK_ENHANCED_IMG)
	$(QEMU) -drive format=raw,file=$(DISK_ENHANCED_IMG) -no-reboot -no-shutdown

# Test with debugging
debug: $(DISK_IMG)
	$(QEMU) -drive format=raw,file=$(DISK_IMG) -no-reboot -no-shutdown -s -S

# Test debug-enabled bootloader with serial output
test-debug: $(BUILD_DIR)/ikos_longmode_debug.img
	$(QEMU) -fda $(BUILD_DIR)/ikos_longmode_debug.img -boot a -nographic -chardev stdio,id=char0 -serial chardev:char0

# Debug session with GDB support
debug-gdb: $(BUILD_DIR)/ikos_longmode_debug.img
	$(QEMU) -fda $(BUILD_DIR)/ikos_longmode_debug.img -boot a -S -s -nographic -serial file:debug_serial.log

# Test compact bootloader in QEMU
test-compact: $(DISK_COMPACT_IMG)
	$(QEMU) -drive format=raw,file=$(DISK_COMPACT_IMG) -no-reboot -no-shutdown -nographic

# Debug compact bootloader
debug-compact: $(DISK_COMPACT_IMG)
	$(QEMU) -drive format=raw,file=$(DISK_COMPACT_IMG) -no-reboot -no-shutdown -s -S

# Test protected mode bootloader in QEMU
test-protected: $(DISK_PROTECTED_IMG)
	$(QEMU) -drive format=raw,file=$(DISK_PROTECTED_IMG) -no-reboot -no-shutdown -nographic

# Test compact protected mode bootloader in QEMU
test-protected-compact: $(DISK_PROTECTED_COMPACT_IMG)
	$(QEMU) -drive format=raw,file=$(DISK_PROTECTED_COMPACT_IMG) -no-reboot -no-shutdown -nographic

# Debug protected mode bootloader
debug-protected: $(DISK_PROTECTED_IMG)
	$(QEMU) -drive format=raw,file=$(DISK_PROTECTED_IMG) -no-reboot -no-shutdown -s -S

# Debug compact protected mode bootloader
debug-protected-compact: $(DISK_PROTECTED_COMPACT_IMG)
	$(QEMU) -drive format=raw,file=$(DISK_PROTECTED_COMPACT_IMG) -no-reboot -no-shutdown -s -S

# Test ELF kernel loader in QEMU
test-elf-loader: $(DISK_ELF_LOADER_IMG)
	$(QEMU) -drive format=raw,file=$(DISK_ELF_LOADER_IMG) -no-reboot -no-shutdown -nographic

# Test compact ELF kernel loader in QEMU
test-elf-compact: $(DISK_ELF_COMPACT_IMG)
	$(QEMU) -drive format=raw,file=$(DISK_ELF_COMPACT_IMG) -no-reboot -no-shutdown -nographic

# Debug ELF kernel loader
debug-elf-loader: $(DISK_ELF_LOADER_IMG)
	$(QEMU) -drive format=raw,file=$(DISK_ELF_LOADER_IMG) -no-reboot -no-shutdown -s -S

# Debug compact ELF kernel loader
debug-elf-compact: $(DISK_ELF_COMPACT_IMG)
	$(QEMU) -drive format=raw,file=$(DISK_ELF_COMPACT_IMG) -no-reboot -no-shutdown -s -S

# Test long mode bootloader in QEMU
test-longmode: $(DISK_LONGMODE_IMG)
	$(QEMU) -drive format=raw,file=$(DISK_LONGMODE_IMG) -no-reboot -no-shutdown -nographic

# Debug long mode bootloader
debug-longmode: $(DISK_LONGMODE_IMG)
	$(QEMU) -drive format=raw,file=$(DISK_LONGMODE_IMG) -no-reboot -no-shutdown -s -S

# Clean build files
clean:
	rm -rf $(BUILD_DIR)

# Install required tools (for Ubuntu/Debian)
install-deps:
	sudo apt-get update
	sudo apt-get install -y nasm qemu-system-x86 ovmf

# =============================================================================
# COMPREHENSIVE TESTING SUITE (Issue #8)
# =============================================================================

# Build debug-enabled bootloaders for testing
debug-builds: $(BUILD_DIR)
	$(ASM) -f bin $(BOOT_DIR)/boot_longmode_debug.asm -o $(BUILD_DIR)/ikos_longmode_debug.img -DDEBUG_ENABLED

# Run comprehensive QEMU testing suite
test-qemu: $(DISK_LONGMODE_IMG) debug-builds
	@echo "=== Running QEMU Testing Suite ==="
	./test_qemu.sh

# Set up real hardware testing environment
setup-real-hardware: $(DISK_LONGMODE_IMG)
	@echo "=== Setting up Real Hardware Testing ==="
	./test_real_hardware.sh

# Create BIOS/UEFI compatibility testing environment
setup-compat-tests:
	@echo "=== Creating BIOS/UEFI Compatibility Tests ==="
	./create_bios_uefi_compat.sh

# Run automated compatibility tests
test-compat: $(DISK_LONGMODE_IMG) debug-builds
	@echo "=== Running Automated Compatibility Tests ==="
	@if [ -d "bios_uefi_compat" ]; then \
		cd bios_uefi_compat && ./automated_compat_test.sh; \
	else \
		echo "Please run 'make setup-compat-tests' first"; \
		exit 1; \
	fi

# Create USB bootable device (requires sudo)
create-usb: $(DISK_LONGMODE_IMG)
	@echo "=== Creating USB Bootable Device ==="
	@echo "This will run the real hardware testing script"
	sudo ./test_real_hardware.sh

# Run comprehensive test suite (QEMU + Compatibility)
test-all: test-qemu test-compat
	@echo "=== All Tests Complete ==="
	@echo "Check test results in:"
	@echo "- qemu_test_results/"
	@echo "- bios_uefi_compat/test_results/"

# Debug with serial output enabled
debug-serial: $(DISK_LONGMODE_IMG)
	$(QEMU) -drive format=raw,file=$(DISK_LONGMODE_IMG) -no-reboot -no-shutdown -s -S -serial stdio

# Debug with GDB integration
debug-gdb: $(DISK_LONGMODE_IMG)
	@echo "Starting QEMU with GDB server on port 1234"
	@echo "Connect with: gdb -ex 'target remote localhost:1234'"
	$(QEMU) -drive format=raw,file=$(DISK_LONGMODE_IMG) -no-reboot -no-shutdown -s -S -nographic

# Performance testing
test-performance: $(DISK_LONGMODE_IMG)
	@echo "=== Performance Testing ==="
	@echo "Testing boot time and memory usage..."
	time $(QEMU) -drive format=raw,file=$(DISK_LONGMODE_IMG) -no-reboot -no-shutdown -nographic & \
	sleep 5 && kill $$!

# Hardware compatibility check
check-hardware:
	@echo "=== Hardware Compatibility Check ==="
	@if [ -f "real_hardware_test/diagnose_hardware.sh" ]; then \
		./real_hardware_test/diagnose_hardware.sh; \
	else \
		echo "Please run 'make setup-real-hardware' first"; \
	fi

# Clean all testing artifacts
clean-tests:
	rm -rf qemu_test_results/
	rm -rf bios_uefi_compat/test_results/
	rm -rf real_hardware_test/

# Clean everything including test setups
clean-all: clean clean-tests
	rm -rf bios_uefi_compat/
	rm -rf real_hardware_test/

# Help target for testing commands
help-testing:
	@echo "IKOS Bootloader Testing Commands:"
	@echo "=================================="
	@echo ""
	@echo "Setup Commands:"
	@echo "  make setup-compat-tests    - Create BIOS/UEFI compatibility tests"
	@echo "  make setup-real-hardware   - Set up real hardware testing tools"
	@echo ""
	@echo "Testing Commands:"
	@echo "  make test-qemu            - Run comprehensive QEMU tests"
	@echo "  make test-compat          - Run BIOS/UEFI compatibility tests"
	@echo "  make test-all             - Run all automated tests"
	@echo "  make test-performance     - Run performance benchmarks"
	@echo ""
	@echo "Hardware Testing:"
	@echo "  make create-usb           - Create bootable USB (requires sudo)"
	@echo "  make check-hardware       - Check hardware compatibility"
	@echo ""
	@echo "Debug Commands:"
	@echo "  make debug-serial         - Debug with serial output"
	@echo "  make debug-gdb            - Debug with GDB integration"
	@echo ""
	@echo "Cleanup Commands:"
	@echo "  make clean-tests          - Clean test results only"
	@echo "  make clean-all            - Clean everything including test setups"
	@echo ""

.PHONY: all test test-enhanced test-compact debug debug-enhanced debug-compact test-protected debug-protected test-protected-compact debug-protected-compact test-elf-loader test-elf-compact debug-elf-loader debug-elf-compact test-longmode debug-longmode clean install-deps debug-builds test-qemu setup-real-hardware setup-compat-tests test-compat create-usb test-all debug-serial debug-gdb test-performance check-hardware clean-tests clean-all help-testing

# QEMU and Real Hardware Test Suite
qemu-test:
	bash qemu_test.sh

real-hardware-test:
	@echo "See real_hardware_test.md for instructions on USB and PXE boot testing."

bios-uefi-test:
	@echo "See real_hardware_test.md for BIOS/UEFI compatibility steps."

# =============================================================================
# AArch64 / ARM bare-metal target - Tessera ARM port (Issue #1)
# -----------------------------------------------------------------------------
# Build:                 make arm
# Clean:                 make arm-clean
# Use a GNU toolchain:   make arm CROSS_COMPILE=aarch64-none-elf-
#                        make arm CROSS_COMPILE=aarch64-linux-gnu-
#
# By default this uses LLVM/clang, which is a native cross-compiler: no
# separate aarch64 GCC is required. See docs/build-arm.md for details.
# =============================================================================

ARCH_ARM_DIR  = arch/arm64
DRIVERS_DIR   = drivers
AUDIO_DIR     = audio
ARM_BUILD_DIR = $(BUILD_DIR)/arm
ARM_CPU       = cortex-a72

# --- Toolchain selection --------------------------------------------------
# If CROSS_COMPILE is set, use that GNU toolchain ($(CROSS_COMPILE)gcc, etc).
# Otherwise fall back to clang/lld/llvm-objcopy with an explicit target.
ifeq ($(strip $(CROSS_COMPILE)),)
  ARM_CC            = clang
  ARM_TARGET_FLAGS  = --target=aarch64-none-elf
  ARM_LD            = ld.lld
  ARM_OBJCOPY       = llvm-objcopy
else
  ARM_CC            = $(CROSS_COMPILE)gcc
  ARM_TARGET_FLAGS  =
  ARM_LD            = $(CROSS_COMPILE)ld
  ARM_OBJCOPY       = $(CROSS_COMPILE)objcopy
endif

ARM_CFLAGS  = $(ARM_TARGET_FLAGS) -mcpu=$(ARM_CPU) -ffreestanding \
              -mgeneral-regs-only -fno-stack-protector -fno-pic -fno-pie \
              -fno-builtin -mno-outline-atomics \
              -Wall -Wextra -std=c11 -O2 -g -Iinclude/ -I$(ARCH_ARM_DIR)
ARM_ASFLAGS = $(ARM_TARGET_FLAGS) -mcpu=$(ARM_CPU) -ffreestanding -g -Iinclude/
ARM_LDSCRIPT = $(ARCH_ARM_DIR)/kernel.ld
ARM_LDFLAGS  = -T $(ARM_LDSCRIPT)

# Sources: C + asm under arch/arm64 and drivers/, plus boot/start.S.
#
# Issue #6 — x86 pruning: the ARM build deliberately excludes all files
# under kernel/ (IDT, GDT, PCI, AC97, VMM asm stubs, interrupt_stubs.asm,
# context_switch.asm, user_mode.asm) which contain x86-specific inline
# assembly and x86 I/O-port intrinsics.  These files remain untouched for
# the legacy x86 target (make with no arguments).
ARM_C_SOURCES   = $(wildcard $(ARCH_ARM_DIR)/*.c) $(wildcard $(DRIVERS_DIR)/*.c) \
                  $(wildcard $(AUDIO_DIR)/*.c)
ARM_ASM_SOURCES = $(wildcard $(ARCH_ARM_DIR)/*.S) $(BOOT_DIR)/start.S
ARM_OBJECTS = $(patsubst %.c,$(ARM_BUILD_DIR)/%.o,$(notdir $(ARM_C_SOURCES))) \
              $(patsubst %.S,$(ARM_BUILD_DIR)/%.o,$(notdir $(ARM_ASM_SOURCES)))

ARM_KERNEL_ELF = $(ARM_BUILD_DIR)/kernel8.elf
ARM_KERNEL_IMG = $(ARM_BUILD_DIR)/kernel8.img

arm: $(ARM_KERNEL_IMG)
	@echo "==> ARM build complete:"
	@echo "    ELF: $(ARM_KERNEL_ELF)"
	@echo "    IMG: $(ARM_KERNEL_IMG)"
	@readelf -h $(ARM_KERNEL_ELF) | grep -E 'Class|Machine|Entry'

$(ARM_BUILD_DIR):
	mkdir -p $(ARM_BUILD_DIR)

# C sources (arch/arm64/*.c)
$(ARM_BUILD_DIR)/%.o: $(ARCH_ARM_DIR)/%.c | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

# Arch assembly (arch/arm64/*.S)
$(ARM_BUILD_DIR)/%.o: $(ARCH_ARM_DIR)/%.S | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -c $< -o $@

# Shared ARM drivers (drivers/*.c)
$(ARM_BUILD_DIR)/%.o: $(DRIVERS_DIR)/%.c | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

# Audio subsystem (audio/*.c)
$(ARM_BUILD_DIR)/%.o: $(AUDIO_DIR)/%.c | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

# Boot entry stub (boot/start.S)
$(ARM_BUILD_DIR)/start.o: $(BOOT_DIR)/start.S | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -c $< -o $@

# Link the kernel ELF (no C runtime, no standard libraries).
$(ARM_KERNEL_ELF): $(ARM_OBJECTS) $(ARM_LDSCRIPT)
	$(ARM_LD) $(ARM_LDFLAGS) -o $@ $(ARM_OBJECTS)

# Flatten to a raw image the Pi firmware can load.
$(ARM_KERNEL_IMG): $(ARM_KERNEL_ELF)
	$(ARM_OBJCOPY) -O binary $< $@

# Host unit tests for the M1 memory subsystem (issues #7, #8/#9, #10).
# Compiles the real arch/arm64 allocator + page-table sources with
# -DHOSTTEST and runs them under AddressSanitizer on the build host — no
# QEMU or cross-toolchain required, so this is the portable CI gate.
ARM_M1_TEST_SRCS = tests/arm64/m1_host_test.c \
                   $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                   $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/kheap.c
ARM_M1_TEST_BIN  = $(ARM_BUILD_DIR)/m1_host_test

test-arm-m1: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -DHOSTTEST -I$(ARCH_ARM_DIR) \
	      $(ARM_M1_TEST_SRCS) -o $(ARM_M1_TEST_BIN)
	$(ARM_M1_TEST_BIN)

# Host unit tests for M2 process isolation (issue #11).
ARM_M2_TEST_SRCS = tests/arm64/m2_process_test.c \
                   $(ARCH_ARM_DIR)/process.c $(ARCH_ARM_DIR)/pmm.c \
                   $(ARCH_ARM_DIR)/mmu.c $(ARCH_ARM_DIR)/vmem.c
ARM_M2_TEST_BIN  = $(ARM_BUILD_DIR)/m2_process_test

test-arm-m2: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -DHOSTTEST -I$(ARCH_ARM_DIR) \
	      $(ARM_M2_TEST_SRCS) -o $(ARM_M2_TEST_BIN)
	$(ARM_M2_TEST_BIN)

# Host unit tests for the exception ESR decoder (issue #12).
ARM_EXC_TEST_SRCS = tests/arm64/exception_test.c $(ARCH_ARM_DIR)/exceptions.c
ARM_EXC_TEST_BIN  = $(ARM_BUILD_DIR)/exception_test

test-arm-exc: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -DHOSTTEST -I$(ARCH_ARM_DIR) \
	      $(ARM_EXC_TEST_SRCS) -o $(ARM_EXC_TEST_BIN)
	$(ARM_EXC_TEST_BIN)

# Runtime exception test on the QEMU 'virt' board (available in every QEMU,
# unlike raspi4b).  Boots the REAL vectors.S + exceptions.c, traps a
# recoverable undefined instruction, and greps the serial log.
VIRT_DIR     = tests/arm64/virt
VIRT_ELF     = $(ARM_BUILD_DIR)/virt_exc.elf

test-arm-exc-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/start_virt.o
	$(ARM_CC) $(ARM_CFLAGS)  -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/uart_virt.o
	$(ARM_CC) $(ARM_CFLAGS)  -c $(VIRT_DIR)/test_main.c -o $(ARM_BUILD_DIR)/test_main.o
	$(ARM_CC) $(ARM_CFLAGS)  -c $(ARCH_ARM_DIR)/exceptions.c -o $(ARM_BUILD_DIR)/exceptions_virt.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S -o $(ARM_BUILD_DIR)/vectors_virt.o
	$(ARM_LD) -T $(VIRT_DIR)/virt.ld -o $(VIRT_ELF) \
	    $(ARM_BUILD_DIR)/start_virt.o $(ARM_BUILD_DIR)/test_main.o \
	    $(ARM_BUILD_DIR)/uart_virt.o $(ARM_BUILD_DIR)/exceptions_virt.o \
	    $(ARM_BUILD_DIR)/vectors_virt.o
	rm -f $(ARM_BUILD_DIR)/virt_exc.log
	-timeout 20 qemu-system-aarch64 -machine virt -cpu cortex-a72 -display none \
	    -serial file:$(ARM_BUILD_DIR)/virt_exc.log -net none \
	    -kernel $(VIRT_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_exc.log
	@grep -q "caught+recovered" $(ARM_BUILD_DIR)/virt_exc.log \
	  && echo "QEMU virt exception test PASSED" \
	  || { echo "QEMU virt exception test FAILED"; exit 1; }

# Runtime EL0 + SVC test on QEMU 'virt' (issue #13).  Boots the REAL entry.S
# + syscall dispatch + vectors with the MMU off, runs a user program that
# sys_writes a greeting and sys_exits, and a second that traps on a
# privileged instruction.
VIRT_USER_ELF = $(ARM_BUILD_DIR)/virt_user.elf

test-arm-user-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/start_virt.o
	$(ARM_CC) $(ARM_CFLAGS)  -c $(VIRT_DIR)/uart_virt.c     -o $(ARM_BUILD_DIR)/uart_virt.o
	$(ARM_CC) $(ARM_CFLAGS)  -c $(VIRT_DIR)/user_main.c     -o $(ARM_BUILD_DIR)/user_main.o
	$(ARM_CC) $(ARM_CFLAGS)  -c $(ARCH_ARM_DIR)/exceptions.c -o $(ARM_BUILD_DIR)/exceptions_virt.o
	$(ARM_CC) $(ARM_CFLAGS)  -c $(ARCH_ARM_DIR)/syscalls.c   -o $(ARM_BUILD_DIR)/syscalls_virt.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S    -o $(ARM_BUILD_DIR)/vectors_virt.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S      -o $(ARM_BUILD_DIR)/entry_virt.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/user_demo.S  -o $(ARM_BUILD_DIR)/user_demo_virt.o
	$(ARM_LD) -T $(VIRT_DIR)/virt.ld -o $(VIRT_USER_ELF) \
	    $(ARM_BUILD_DIR)/start_virt.o $(ARM_BUILD_DIR)/user_main.o \
	    $(ARM_BUILD_DIR)/uart_virt.o $(ARM_BUILD_DIR)/exceptions_virt.o \
	    $(ARM_BUILD_DIR)/syscalls_virt.o $(ARM_BUILD_DIR)/vectors_virt.o \
	    $(ARM_BUILD_DIR)/entry_virt.o $(ARM_BUILD_DIR)/user_demo_virt.o
	rm -f $(ARM_BUILD_DIR)/virt_user.log
	-timeout 20 qemu-system-aarch64 -machine virt -cpu cortex-a72 -display none \
	    -serial file:$(ARM_BUILD_DIR)/virt_user.log -net none \
	    -kernel $(VIRT_USER_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_user.log
	@grep -q "Hello from EL0" $(ARM_BUILD_DIR)/virt_user.log \
	  && grep -q "VIRT-USER: PASS" $(ARM_BUILD_DIR)/virt_user.log \
	  && echo "QEMU virt EL0/SVC test PASSED" \
	  || { echo "QEMU virt EL0/SVC test FAILED"; exit 1; }

# Full-stack fault-containment test on QEMU 'virt' (issue #14).  Builds the
# REAL pmm/mmu/vmem/process/exception/syscall stack with the virt memory map
# and the MMU ENABLED, so the hardware enforces the kernel/user split: a
# process that writes to kernel memory is trapped and killed, the kernel data
# stays intact, and a second process runs normally.
VIRT_MMU_FLAGS = -DPHYS_RAM_START=0x40000000UL -DPHYS_RAM_END=0x48000000UL \
                 -DMMIO_BASE=0x08000000UL -DMMIO_END=0x10000000UL
VIRT_FAULT_ELF = $(ARM_BUILD_DIR)/virt_fault.elf
VIRT_FAULT_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                  $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
                  $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
                  $(ARCH_ARM_DIR)/string.c

test-arm-fault-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/f_start.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c  -o $(ARM_BUILD_DIR)/f_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/fault_main.c -o $(ARM_BUILD_DIR)/f_main.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S   -o $(ARM_BUILD_DIR)/f_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S     -o $(ARM_BUILD_DIR)/f_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/user_demo.S -o $(ARM_BUILD_DIR)/f_userdemo.o
	for s in $(VIRT_FAULT_SRCS); do \
	  o=$(ARM_BUILD_DIR)/f_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_FAULT_ELF) \
	    $(ARM_BUILD_DIR)/f_start.o $(ARM_BUILD_DIR)/f_main.o \
	    $(ARM_BUILD_DIR)/f_uart.o $(ARM_BUILD_DIR)/f_vectors.o \
	    $(ARM_BUILD_DIR)/f_entry.o $(ARM_BUILD_DIR)/f_userdemo.o \
	    $(ARM_BUILD_DIR)/f_pmm.o $(ARM_BUILD_DIR)/f_mmu.o \
	    $(ARM_BUILD_DIR)/f_vmem.o $(ARM_BUILD_DIR)/f_process.o \
	    $(ARM_BUILD_DIR)/f_exceptions.o $(ARM_BUILD_DIR)/f_syscalls.o \
	    $(ARM_BUILD_DIR)/f_string.o
	rm -f $(ARM_BUILD_DIR)/virt_fault.log
	-timeout 20 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_fault.log -net none \
	    -kernel $(VIRT_FAULT_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_fault.log
	@grep -q "Hello from EL0" $(ARM_BUILD_DIR)/virt_fault.log \
	  && grep -q "VIRT-FAULT: PASS" $(ARM_BUILD_DIR)/virt_fault.log \
	  && echo "QEMU virt fault-containment test PASSED" \
	  || { echo "QEMU virt fault-containment test FAILED"; exit 1; }

# Full-stack context-switch test on QEMU 'virt' (issue #15): round-robin EL0
# tasks with per-process isolation and lazy FPU, MMU enabled.
VIRT_SCHED_ELF  = $(ARM_BUILD_DIR)/virt_sched.elf
VIRT_SCHED_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                  $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
                  $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
                  $(ARCH_ARM_DIR)/sched.c $(ARCH_ARM_DIR)/runqueue.c \
                  $(ARCH_ARM_DIR)/string.c

test-arm-sched-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/s_start.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c  -o $(ARM_BUILD_DIR)/s_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/sched_main.c -o $(ARM_BUILD_DIR)/s_main.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S        -o $(ARM_BUILD_DIR)/s_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S          -o $(ARM_BUILD_DIR)/s_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/context_switch.S -o $(ARM_BUILD_DIR)/s_ctx.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/user_demo.S      -o $(ARM_BUILD_DIR)/s_userdemo.o
	for s in $(VIRT_SCHED_SRCS); do \
	  o=$(ARM_BUILD_DIR)/s_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_SCHED_ELF) \
	    $(ARM_BUILD_DIR)/s_start.o $(ARM_BUILD_DIR)/s_main.o \
	    $(ARM_BUILD_DIR)/s_uart.o $(ARM_BUILD_DIR)/s_vectors.o \
	    $(ARM_BUILD_DIR)/s_entry.o $(ARM_BUILD_DIR)/s_ctx.o \
	    $(ARM_BUILD_DIR)/s_userdemo.o \
	    $(ARM_BUILD_DIR)/s_pmm.o $(ARM_BUILD_DIR)/s_mmu.o \
	    $(ARM_BUILD_DIR)/s_vmem.o $(ARM_BUILD_DIR)/s_process.o \
	    $(ARM_BUILD_DIR)/s_exceptions.o $(ARM_BUILD_DIR)/s_syscalls.o \
	    $(ARM_BUILD_DIR)/s_sched.o $(ARM_BUILD_DIR)/s_runqueue.o \
	    $(ARM_BUILD_DIR)/s_string.o
	rm -f $(ARM_BUILD_DIR)/virt_sched.log
	-timeout 20 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_sched.log -net none \
	    -kernel $(VIRT_SCHED_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_sched.log
	@grep -q "VIRT-SCHED: PASS" $(ARM_BUILD_DIR)/virt_sched.log \
	  && echo "QEMU virt context-switch test PASSED" \
	  || { echo "QEMU virt context-switch test FAILED"; exit 1; }

# Host unit tests for the I2S clock + waveform math (issue #16).
ARM_I2S_TEST_SRCS = tests/arm64/i2s_test.c drivers/i2s.c
ARM_I2S_TEST_BIN  = $(ARM_BUILD_DIR)/i2s_test

test-arm-i2s: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -DHOSTTEST -Iinclude \
	      $(ARM_I2S_TEST_SRCS) -o $(ARM_I2S_TEST_BIN)
	$(ARM_I2S_TEST_BIN)

# I2S API smoke test on QEMU 'virt' (issue #16): the real driver against
# scratch-mapped peripheral registers, MMU enabled, no faults.
VIRT_I2S_ELF  = $(ARM_BUILD_DIR)/virt_i2s.elf
VIRT_I2S_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/exceptions.c \
                $(ARCH_ARM_DIR)/string.c drivers/i2s.c drivers/gpio.c \
                drivers/dma.c

test-arm-i2s-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/i_start.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/i_uart.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/i2s_main.c  -o $(ARM_BUILD_DIR)/i_main.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S -o $(ARM_BUILD_DIR)/i_vectors.o
	for s in $(VIRT_I2S_SRCS); do \
	  o=$(ARM_BUILD_DIR)/i_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_I2S_ELF) \
	    $(ARM_BUILD_DIR)/i_start.o $(ARM_BUILD_DIR)/i_main.o \
	    $(ARM_BUILD_DIR)/i_uart.o $(ARM_BUILD_DIR)/i_vectors.o \
	    $(ARM_BUILD_DIR)/i_pmm.o $(ARM_BUILD_DIR)/i_mmu.o \
	    $(ARM_BUILD_DIR)/i_vmem.o $(ARM_BUILD_DIR)/i_exceptions.o \
	    $(ARM_BUILD_DIR)/i_string.o $(ARM_BUILD_DIR)/i_i2s.o \
	    $(ARM_BUILD_DIR)/i_gpio.o $(ARM_BUILD_DIR)/i_dma.o
	rm -f $(ARM_BUILD_DIR)/virt_i2s.log
	-timeout 20 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_i2s.log -net none \
	    -kernel $(VIRT_I2S_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_i2s.log
	@grep -q "I2S-SMOKE: PASS" $(ARM_BUILD_DIR)/virt_i2s.log \
	  && echo "QEMU virt I2S smoke test PASSED" \
	  || { echo "QEMU virt I2S smoke test FAILED"; exit 1; }

# Host unit tests for the I2S capture ring (issue #83): bit-exact FIFO capture,
# wrap-around, drop-the-oldest overrun, silence-on-underrun, and the RX DMA
# control-block encoding.  Pure C, ASan/UBSan.
ARM_I2SRX_TEST_SRCS = tests/arm64/i2s_capture_test.c drivers/i2s_capture.c drivers/dma.c
ARM_I2SRX_TEST_BIN  = $(ARM_BUILD_DIR)/i2s_capture_test

test-arm-i2s-rx: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -DHOSTTEST -Iinclude \
	      $(ARM_I2SRX_TEST_SRCS) -o $(ARM_I2SRX_TEST_BIN)
	$(ARM_I2SRX_TEST_BIN)

# I2S capture on QEMU 'virt' (issue #83): the real RX driver + DMA setup
# against scratch-mapped registers (no faults), plus a modelled capture source
# driving the ring - bit-exact across wrap-around, drop-oldest overrun, and
# silence on underrun.  MMU on.
VIRT_I2SRX_ELF  = $(ARM_BUILD_DIR)/virt_i2s_rx.elf
VIRT_I2SRX_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                  $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/exceptions.c \
                  $(ARCH_ARM_DIR)/string.c drivers/i2s.c drivers/gpio.c \
                  drivers/dma.c drivers/i2s_capture.c

test-arm-i2s-rx-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/ir_start.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c   -o $(ARM_BUILD_DIR)/ir_uart.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -Iinclude -c $(VIRT_DIR)/i2s_rx_main.c -o $(ARM_BUILD_DIR)/ir_main.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S -o $(ARM_BUILD_DIR)/ir_vectors.o
	for s in $(VIRT_I2SRX_SRCS); do \
	  o=$(ARM_BUILD_DIR)/ir_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_I2SRX_ELF) \
	    $(ARM_BUILD_DIR)/ir_start.o $(ARM_BUILD_DIR)/ir_main.o \
	    $(ARM_BUILD_DIR)/ir_uart.o $(ARM_BUILD_DIR)/ir_vectors.o \
	    $(ARM_BUILD_DIR)/ir_pmm.o $(ARM_BUILD_DIR)/ir_mmu.o \
	    $(ARM_BUILD_DIR)/ir_vmem.o $(ARM_BUILD_DIR)/ir_exceptions.o \
	    $(ARM_BUILD_DIR)/ir_string.o $(ARM_BUILD_DIR)/ir_i2s.o \
	    $(ARM_BUILD_DIR)/ir_gpio.o $(ARM_BUILD_DIR)/ir_dma.o \
	    $(ARM_BUILD_DIR)/ir_i2s_capture.o
	rm -f $(ARM_BUILD_DIR)/virt_i2s_rx.log
	-timeout 20 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_i2s_rx.log -net none \
	    -kernel $(VIRT_I2SRX_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_i2s_rx.log
	@grep -q "I2S-RX: PASS" $(ARM_BUILD_DIR)/virt_i2s_rx.log \
	  && echo "QEMU virt I2S capture test PASSED" \
	  || { echo "QEMU virt I2S capture test FAILED"; exit 1; }

# Host unit tests for DMA audio streaming (issue #17).
ARM_AUDIO_TEST_SRCS = tests/arm64/audio_test.c drivers/dma.c drivers/audio.c
ARM_AUDIO_TEST_BIN  = $(ARM_BUILD_DIR)/audio_test

test-arm-audio: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -DHOSTTEST -Iinclude \
	      $(ARM_AUDIO_TEST_SRCS) -o $(ARM_AUDIO_TEST_BIN)
	$(ARM_AUDIO_TEST_BIN)

# DMA-audio API smoke test on QEMU 'virt' (issue #17): full streaming path
# against scratch-mapped peripheral registers, MMU enabled, no faults.
VIRT_AUDIO_ELF  = $(ARM_BUILD_DIR)/virt_audio.elf
VIRT_AUDIO_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                  $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/exceptions.c \
                  $(ARCH_ARM_DIR)/string.c drivers/dma.c drivers/audio.c \
                  drivers/i2s.c drivers/gpio.c

test-arm-audio-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/au_start.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c  -o $(ARM_BUILD_DIR)/au_uart.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/audio_main.c -o $(ARM_BUILD_DIR)/au_main.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S -o $(ARM_BUILD_DIR)/au_vectors.o
	for s in $(VIRT_AUDIO_SRCS); do \
	  o=$(ARM_BUILD_DIR)/au_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_AUDIO_ELF) \
	    $(ARM_BUILD_DIR)/au_start.o $(ARM_BUILD_DIR)/au_main.o \
	    $(ARM_BUILD_DIR)/au_uart.o $(ARM_BUILD_DIR)/au_vectors.o \
	    $(ARM_BUILD_DIR)/au_pmm.o $(ARM_BUILD_DIR)/au_mmu.o \
	    $(ARM_BUILD_DIR)/au_vmem.o $(ARM_BUILD_DIR)/au_exceptions.o \
	    $(ARM_BUILD_DIR)/au_string.o $(ARM_BUILD_DIR)/au_dma.o \
	    $(ARM_BUILD_DIR)/au_audio.o $(ARM_BUILD_DIR)/au_i2s.o \
	    $(ARM_BUILD_DIR)/au_gpio.o
	rm -f $(ARM_BUILD_DIR)/virt_audio.log
	-timeout 20 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_audio.log -net none \
	    -kernel $(VIRT_AUDIO_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_audio.log
	@grep -q "AUDIO-DMA: PASS" $(ARM_BUILD_DIR)/virt_audio.log \
	  && echo "QEMU virt DMA-audio smoke test PASSED" \
	  || { echo "QEMU virt DMA-audio smoke test FAILED"; exit 1; }

# Host unit tests for the sine generator (issue #18).
ARM_SINE_TEST_SRCS = tests/arm64/sine_test.c $(AUDIO_DIR)/sine_gen.c
ARM_SINE_TEST_BIN  = $(ARM_BUILD_DIR)/sine_test

test-arm-sine: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -DHOSTTEST -Iinclude \
	      $(ARM_SINE_TEST_SRCS) -o $(ARM_SINE_TEST_BIN)
	$(ARM_SINE_TEST_BIN)

# Sine-generator API smoke on QEMU 'virt' (issue #18).
VIRT_SINE_ELF  = $(ARM_BUILD_DIR)/virt_sine.elf
VIRT_SINE_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                 $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/exceptions.c \
                 $(ARCH_ARM_DIR)/string.c drivers/dma.c drivers/audio.c \
                 drivers/i2s.c drivers/gpio.c $(AUDIO_DIR)/sine_gen.c

test-arm-sine-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/sg_start.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/sg_uart.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/sine_main.c -o $(ARM_BUILD_DIR)/sg_main.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S -o $(ARM_BUILD_DIR)/sg_vectors.o
	for s in $(VIRT_SINE_SRCS); do \
	  o=$(ARM_BUILD_DIR)/sg_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_SINE_ELF) \
	    $(ARM_BUILD_DIR)/sg_start.o $(ARM_BUILD_DIR)/sg_main.o \
	    $(ARM_BUILD_DIR)/sg_uart.o $(ARM_BUILD_DIR)/sg_vectors.o \
	    $(ARM_BUILD_DIR)/sg_pmm.o $(ARM_BUILD_DIR)/sg_mmu.o \
	    $(ARM_BUILD_DIR)/sg_vmem.o $(ARM_BUILD_DIR)/sg_exceptions.o \
	    $(ARM_BUILD_DIR)/sg_string.o $(ARM_BUILD_DIR)/sg_dma.o \
	    $(ARM_BUILD_DIR)/sg_audio.o $(ARM_BUILD_DIR)/sg_i2s.o \
	    $(ARM_BUILD_DIR)/sg_gpio.o $(ARM_BUILD_DIR)/sg_sine_gen.o
	rm -f $(ARM_BUILD_DIR)/virt_sine.log
	-timeout 20 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_sine.log -net none \
	    -kernel $(VIRT_SINE_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_sine.log
	@grep -q "SINE-GEN: PASS" $(ARM_BUILD_DIR)/virt_sine.log \
	  && echo "QEMU virt sine-generator smoke test PASSED" \
	  || { echo "QEMU virt sine-generator smoke test FAILED"; exit 1; }

# GIC + generic-timer test on QEMU 'virt' (issue #19): real timer interrupts
# at 1 kHz through the emulated GICv2.  Uses the virt GIC base addresses.
VIRT_GIC_FLAGS = -DGIC_DIST_BASE=0x08000000UL -DGIC_CPU_BASE=0x08010000UL
VIRT_TIMER_ELF = $(ARM_BUILD_DIR)/virt_timer.elf

test-arm-timer-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/tm_start.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/uart_virt.c  -o $(ARM_BUILD_DIR)/tm_uart.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/timer_main.c -o $(ARM_BUILD_DIR)/tm_main.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S    -o $(ARM_BUILD_DIR)/tm_vectors.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_GIC_FLAGS) -c $(ARCH_ARM_DIR)/exceptions.c -o $(ARM_BUILD_DIR)/tm_exceptions.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_GIC_FLAGS) -c $(ARCH_ARM_DIR)/irq.c   -o $(ARM_BUILD_DIR)/tm_irq.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_GIC_FLAGS) -c $(ARCH_ARM_DIR)/timer.c -o $(ARM_BUILD_DIR)/tm_timer.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_GIC_FLAGS) -c drivers/gic.c           -o $(ARM_BUILD_DIR)/tm_gic.o
	$(ARM_LD) -T $(VIRT_DIR)/virt.ld -o $(VIRT_TIMER_ELF) \
	    $(ARM_BUILD_DIR)/tm_start.o $(ARM_BUILD_DIR)/tm_main.o \
	    $(ARM_BUILD_DIR)/tm_uart.o $(ARM_BUILD_DIR)/tm_vectors.o \
	    $(ARM_BUILD_DIR)/tm_exceptions.o $(ARM_BUILD_DIR)/tm_irq.o \
	    $(ARM_BUILD_DIR)/tm_timer.o $(ARM_BUILD_DIR)/tm_gic.o
	rm -f $(ARM_BUILD_DIR)/virt_timer.log
	-timeout 25 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_timer.log -net none \
	    -kernel $(VIRT_TIMER_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_timer.log
	@grep -q "TIMER: PASS" $(ARM_BUILD_DIR)/virt_timer.log \
	  && echo "QEMU virt GIC+timer test PASSED" \
	  || { echo "QEMU virt GIC+timer test FAILED"; exit 1; }

# Host unit tests for the RT-scheduler policy (issue #20): priority ordering,
# round-robin fairness, idle-only-when-empty, and priority inheritance.  The
# run queue is pure C, so it runs under ASan/UBSan on the host.
ARM_RTSCHED_TEST_SRCS = tests/arm64/rtsched_test.c $(ARCH_ARM_DIR)/runqueue.c
ARM_RTSCHED_TEST_BIN  = $(ARM_BUILD_DIR)/rtsched_test

test-arm-rtsched: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -DHOSTTEST -I$(ARCH_ARM_DIR) \
	      $(ARM_RTSCHED_TEST_SRCS) -o $(ARM_RTSCHED_TEST_BIN)
	$(ARM_RTSCHED_TEST_BIN)

# Host unit tests for the audio-core building blocks (issue #21): the lock-free
# SPSC ring (incl. a real 2-thread cross-core stress), the overrun watchdog,
# and the DMA refill/underrun path.  All pure C, run under ASan/UBSan.
ARM_SMP_TEST_SRCS = tests/arm64/smp_test.c $(ARCH_ARM_DIR)/spsc_ring.c \
                    $(ARCH_ARM_DIR)/audio_core.c
ARM_SMP_TEST_BIN  = $(ARM_BUILD_DIR)/smp_test

test-arm-smp: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -DHOSTTEST -I$(ARCH_ARM_DIR) \
	      $(ARM_SMP_TEST_SRCS) -o $(ARM_SMP_TEST_BIN) -lpthread
	$(ARM_SMP_TEST_BIN)

# Host unit tests for the audio-latency statistics (issue #22): min/max/mean/
# stddev over the callback window, integer sqrt, and the cycles->us conversion.
ARM_LAT_TEST_SRCS = tests/arm64/latency_test.c $(ARCH_ARM_DIR)/latency.c
ARM_LAT_TEST_BIN  = $(ARM_BUILD_DIR)/latency_test

test-arm-latency: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -DHOSTTEST -I$(ARCH_ARM_DIR) \
	      $(ARM_LAT_TEST_SRCS) -o $(ARM_LAT_TEST_BIN)
	$(ARM_LAT_TEST_BIN)

# Host unit tests for the ELF64 reader (issue #24).
ARM_ELF_TEST_SRCS = tests/arm64/elf_test.c $(ARCH_ARM_DIR)/elf64.c
ARM_ELF_TEST_BIN  = $(ARM_BUILD_DIR)/elf_test

test-arm-elf: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -I$(ARCH_ARM_DIR) \
	      $(ARM_ELF_TEST_SRCS) -o $(ARM_ELF_TEST_BIN)
	$(ARM_ELF_TEST_BIN)

# Host unit tests for the shared audio ring buffer (issue #25).
ARM_RINGBUF_TEST_SRCS = tests/arm64/ringbuf_test.c $(ARCH_ARM_DIR)/audio_ringbuf.c
ARM_RINGBUF_TEST_BIN  = $(ARM_BUILD_DIR)/ringbuf_test

test-arm-ringbuf: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -I$(ARCH_ARM_DIR) \
	      $(ARM_RINGBUF_TEST_SRCS) -o $(ARM_RINGBUF_TEST_BIN) -lpthread
	$(ARM_RINGBUF_TEST_BIN)

# Host unit tests for the resilient audio host (issue #26).
ARM_HOST_TEST_SRCS = tests/arm64/plugin_host_test.c $(ARCH_ARM_DIR)/plugin_host.c \
                     $(ARCH_ARM_DIR)/audio_ringbuf.c
ARM_HOST_TEST_BIN  = $(ARM_BUILD_DIR)/plugin_host_test

test-arm-plugin-host: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -I$(ARCH_ARM_DIR) \
	      $(ARM_HOST_TEST_SRCS) -o $(ARM_HOST_TEST_BIN)
	$(ARM_HOST_TEST_BIN)

# Host unit tests for the audio graph model (issue #27).
ARM_GRAPH_TEST_SRCS = tests/arm64/graph_test.c $(ARCH_ARM_DIR)/audio_graph.c
ARM_GRAPH_TEST_BIN  = $(ARM_BUILD_DIR)/graph_test

test-arm-graph: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -I$(ARCH_ARM_DIR) \
	      $(ARM_GRAPH_TEST_SRCS) -o $(ARM_GRAPH_TEST_BIN)
	$(ARM_GRAPH_TEST_BIN)

# Host unit tests for the graph control plane (issue #28).
ARM_GCTL_TEST_SRCS = tests/arm64/graph_control_test.c $(ARCH_ARM_DIR)/graph_control.c \
                     $(ARCH_ARM_DIR)/audio_graph.c
ARM_GCTL_TEST_BIN  = $(ARM_BUILD_DIR)/graph_control_test

test-arm-graph-control: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -I$(ARCH_ARM_DIR) \
	      $(ARM_GCTL_TEST_SRCS) -o $(ARM_GCTL_TEST_BIN)
	$(ARM_GCTL_TEST_BIN)

# Host unit tests for the per-core audio workers (issue #74): the kick/step
# cadence protocol, skip-and-attribute on a late worker, automatic recovery,
# and the drained invariant blocks + overruns == kicks, including a real
# two-thread kicker/worker stress.  Pure C, run under ASan/UBSan.
ARM_WORKER_TEST_SRCS = tests/arm64/worker_test.c $(ARCH_ARM_DIR)/audio_worker.c
ARM_WORKER_TEST_BIN  = $(ARM_BUILD_DIR)/worker_test

test-arm-worker: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -DHOSTTEST -I$(ARCH_ARM_DIR) \
	      $(ARM_WORKER_TEST_SRCS) -o $(ARM_WORKER_TEST_BIN) -lpthread
	$(ARM_WORKER_TEST_BIN)

# Host unit tests for the topology-aware graph partitioner (issue #75): the
# balance-first planner, the seqlocked stage/apply handoff, the edge
# reset/prime rules, and the graph_control on-change hook.  Pure C, run under
# ASan/UBSan.
ARM_GSCHED_TEST_SRCS = tests/arm64/graph_sched_test.c $(ARCH_ARM_DIR)/graph_sched.c \
                       $(ARCH_ARM_DIR)/audio_graph.c $(ARCH_ARM_DIR)/graph_control.c \
                       $(ARCH_ARM_DIR)/audio_worker.c
ARM_GSCHED_TEST_BIN  = $(ARM_BUILD_DIR)/graph_sched_test

test-arm-gsched: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -DHOSTTEST -I$(ARCH_ARM_DIR) \
	      $(ARM_GSCHED_TEST_SRCS) -o $(ARM_GSCHED_TEST_BIN)
	$(ARM_GSCHED_TEST_BIN)

# Host unit tests for per-plugin time accounting (issue #77): the worker-side
# clock measurement, per-node min/max/mean vs hand-computed values, the
# seqlocked stats board (incl. a two-thread publisher/reader stress), and the
# plugin_time line rendering.  Pure C, run under ASan/UBSan.
ARM_PTIME_TEST_SRCS = tests/arm64/plugin_time_test.c $(ARCH_ARM_DIR)/plugin_time.c \
                      $(ARCH_ARM_DIR)/audio_worker.c $(ARCH_ARM_DIR)/latency.c
ARM_PTIME_TEST_BIN  = $(ARM_BUILD_DIR)/plugin_time_test

test-arm-ptime: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -DHOSTTEST -I$(ARCH_ARM_DIR) \
	      $(ARM_PTIME_TEST_SRCS) -o $(ARM_PTIME_TEST_BIN) -lpthread
	$(ARM_PTIME_TEST_BIN)

# Host unit tests for the CPU-budget policy (issue #78): the mute/kill
# escalation truth table, forgiveness on a clean block, the fair-share
# default, and the control-plane budget registry.  Pure C, ASan/UBSan.
ARM_BUDGET_TEST_SRCS = tests/arm64/budget_test.c $(ARCH_ARM_DIR)/budget.c \
                       $(ARCH_ARM_DIR)/graph_control.c $(ARCH_ARM_DIR)/audio_graph.c
ARM_BUDGET_TEST_BIN  = $(ARM_BUILD_DIR)/budget_test

test-arm-budget: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -DHOSTTEST -I$(ARCH_ARM_DIR) \
	      $(ARM_BUDGET_TEST_SRCS) -o $(ARM_BUDGET_TEST_BIN)
	$(ARM_BUDGET_TEST_BIN)

# Host unit tests for the serial shell core (issue #80): the tokeniser, the
# command dispatcher and built-in help, the line editor (echo, backspace,
# CR/LF/CRLF), over-long-line containment, and a 2M-byte fuzz sweep.  Pure C,
# run under ASan/UBSan.
ARM_SHELL_TEST_SRCS = tests/arm64/shell_test.c $(ARCH_ARM_DIR)/shell.c
ARM_SHELL_TEST_BIN  = $(ARM_BUILD_DIR)/shell_test

test-arm-shell: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -DHOSTTEST -I$(ARCH_ARM_DIR) \
	      $(ARM_SHELL_TEST_SRCS) -o $(ARM_SHELL_TEST_BIN)
	$(ARM_SHELL_TEST_BIN)

# Host unit tests for the shell graph commands (issue #81): argument parsing,
# the load/unload/wire/unwire/set-param verbs forwarding through a mock
# backend, the one-line error paths, and ls/stats rendering.  Pure C,
# ASan/UBSan.
ARM_SHGRAPH_TEST_SRCS = tests/arm64/shell_graph_test.c \
                        $(ARCH_ARM_DIR)/shell.c $(ARCH_ARM_DIR)/shell_graph.c \
                        $(ARCH_ARM_DIR)/patch.c
ARM_SHGRAPH_TEST_BIN  = $(ARM_BUILD_DIR)/shell_graph_test

test-arm-shell-graph: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -DHOSTTEST -I$(ARCH_ARM_DIR) \
	      $(ARM_SHGRAPH_TEST_SRCS) -o $(ARM_SHGRAPH_TEST_BIN)
	$(ARM_SHGRAPH_TEST_BIN)

# ---- M5: plugin ABI (issue #23) -------------------------------------------
# Plugins use floating point for DSP, so they are built WITH FP (no
# -mgeneral-regs-only) and against only the self-contained plugin ABI header.
PLUGIN_CFLAGS = -mcpu=$(ARM_CPU) -ffreestanding -fPIC -O2 -std=c11 -Wall -Wextra
EXAMPLE_PLUGIN_SRC = plugins/example_gain/gain.c
PLUGIN_SYMS = plugin_abi_version plugin_init plugin_process_block \
              plugin_set_param plugin_destroy

# Host semantic test: drive the reference plugin through the ABI.
test-arm-plugin-abi: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -Iinclude tests/arm64/plugin_abi_test.c $(EXAMPLE_PLUGIN_SRC) \
	      -o $(ARM_BUILD_DIR)/plugin_abi_test -lm
	$(ARM_BUILD_DIR)/plugin_abi_test

# Acceptance: the reference plugin builds with the cross toolchain against ONLY
# plugin_abi.h (copied alone, proving no kernel headers are needed) and produces
# a valid AArch64 ELF exporting all five ABI symbols.
test-arm-plugin-elf: | $(ARM_BUILD_DIR)
	rm -rf $(ARM_BUILD_DIR)/plugin_inc
	mkdir -p $(ARM_BUILD_DIR)/plugin_inc
	cp include/plugin_abi.h $(ARM_BUILD_DIR)/plugin_inc/
	$(ARM_CC) $(PLUGIN_CFLAGS) -I$(ARM_BUILD_DIR)/plugin_inc \
	    -c $(EXAMPLE_PLUGIN_SRC) -o $(ARM_BUILD_DIR)/gain.o
	@echo "--- ELF header ---"
	@$(CROSS_COMPILE)readelf -h $(ARM_BUILD_DIR)/gain.o
	@$(CROSS_COMPILE)readelf -h $(ARM_BUILD_DIR)/gain.o | grep -q 'AArch64' \
	  && echo "machine: AArch64 OK" \
	  || { echo "FAILED: not an AArch64 ELF"; exit 1; }
	@for s in $(PLUGIN_SYMS); do \
	  $(CROSS_COMPILE)readelf -sW $(ARM_BUILD_DIR)/gain.o | grep -q " $$s$$" \
	    && echo "symbol: $$s OK" \
	    || { echo "FAILED: missing ABI symbol $$s"; exit 1; }; \
	done
	@echo "plugin ABI ELF test PASSED"

# ---- M9: freeze + verify the plugin ABI v1 (issue #37) --------------------
# Verify every in-tree reference plugin against the frozen ABI spec
# (docs/plugin-abi.md): a little-endian AArch64 ELF that exports all five ABI
# symbols and has NO undefined (imported) named symbols (self-contained).  The
# deliberately-broken fixtures (badabi/badimport/sbsvc/sbmem/evil) are excluded.
ABI_REF_PLUGINS = $(ARM_BUILD_DIR)/plugin_good.elf $(ARM_BUILD_DIR)/plugin_sine.elf \
                  $(ARM_BUILD_DIR)/plugin_echo.elf $(ARM_BUILD_DIR)/plugin_effect.elf \
                  $(ARM_BUILD_DIR)/plugin_effect_filter.elf $(ARM_BUILD_DIR)/plugin_pass.elf \
                  $(ARM_BUILD_DIR)/plugin_producer.elf $(ARM_BUILD_DIR)/plugin_controller.elf

verify-plugin-abi: $(ABI_REF_PLUGINS)
	@echo "=== Verifying reference plugins against Plugin ABI v1 (issue #37) ==="
	@fail=0; for elf in $(ABI_REF_PLUGINS); do \
	  echo "--- $$elf"; \
	  $(CROSS_COMPILE)readelf -h $$elf | grep -q 'AArch64' \
	    || { echo "  FAIL: not an AArch64 ELF"; fail=1; continue; }; \
	  $(CROSS_COMPILE)readelf -h $$elf | grep -qE 'EXEC|DYN' \
	    || { echo "  FAIL: not ET_EXEC/ET_DYN"; fail=1; }; \
	  for s in $(PLUGIN_SYMS); do \
	    $(CROSS_COMPILE)readelf -sW $$elf | grep -q " $$s$$" \
	      || { echo "  FAIL: missing ABI symbol $$s"; fail=1; }; \
	  done; \
	  u=$$($(CROSS_COMPILE)readelf -sW $$elf | awk '$$7=="UND" && $$8!=""' | wc -l); \
	  [ "$$u" -eq 0 ] \
	    && echo "  ok: AArch64, all 5 ABI symbols, 0 disallowed imports" \
	    || { echo "  FAIL: $$u disallowed import(s)"; fail=1; }; \
	done; \
	[ $$fail -eq 0 ] \
	  && echo "=== ALL REFERENCE PLUGINS CONFORM TO ABI v1 ===" \
	  || { echo "=== ABI CONFORMANCE FAILURES PRESENT ==="; exit 1; }

# ---- M8: SD/FAT plugin loading (issue #34) --------------------------------
# Host unit tests for the FAT16 reader.
ARM_FAT_TEST_SRCS = tests/arm64/fat_test.c $(ARCH_ARM_DIR)/fat.c
test-arm-fat: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -I$(ARCH_ARM_DIR) $(ARM_FAT_TEST_SRCS) -o $(ARM_BUILD_DIR)/fat_test
	$(ARM_BUILD_DIR)/fat_test

# Host unit tests for the plugin VFS path resolver (ramdisk + SD).
ARM_VFS_TEST_SRCS = tests/arm64/vfs_test.c $(ARCH_ARM_DIR)/vfs.c $(ARCH_ARM_DIR)/fat.c
test-arm-vfs: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -I$(ARCH_ARM_DIR) $(ARM_VFS_TEST_SRCS) -o $(ARM_BUILD_DIR)/arm_vfs_test
	$(ARM_BUILD_DIR)/arm_vfs_test

# Host unit tests for the sandbox audit helpers (containment + PTE classify).
ARM_SANDBOX_TEST_SRCS = tests/arm64/sandbox_test.c $(ARCH_ARM_DIR)/sandbox.c
test-arm-sandbox: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -I$(ARCH_ARM_DIR) $(ARM_SANDBOX_TEST_SRCS) -o $(ARM_BUILD_DIR)/sandbox_test
	$(ARM_BUILD_DIR)/sandbox_test

# Host unit tests for the plugin SDK library (issue #38): tessera_sinf accuracy,
# tessera_clampf, tessera_param_queue_read.  Built against the SDK headers only.
ARM_SDK_TEST_SRCS = tests/arm64/sdk_test.c sdk/lib/tessera_math.c sdk/lib/tessera_param.c
test-sdk: verify-sdk-abi-sync | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -Isdk $(ARM_SDK_TEST_SRCS) -o $(ARM_BUILD_DIR)/sdk_test -lm
	$(ARM_BUILD_DIR)/sdk_test

# Run the getting-started guide's build-and-verify steps verbatim (issue #39):
# the single script that docs/getting-started.md quotes, so the guide is proven
# on every change.  Pair with test-arm-sdk-qemu for the QEMU load step.
.PHONY: test-getting-started
test-getting-started:
	CROSS_COMPILE=$(CROSS_COMPILE) sh scripts/build-example-plugin.sh

# The SDK bundles a copy of the frozen ABI header so it is standalone; guard it
# against drift from the canonical include/plugin_abi.h (issue #38).
.PHONY: verify-sdk-abi-sync
verify-sdk-abi-sync:
	@cmp -s include/plugin_abi.h sdk/plugin_abi.h \
	  && echo "sdk/plugin_abi.h in sync with include/plugin_abi.h" \
	  || { echo "ERROR: sdk/plugin_abi.h differs from include/plugin_abi.h"; \
	       echo "  run: cp include/plugin_abi.h sdk/plugin_abi.h"; exit 1; }

# ---- M5: plugin loader (issue #24) ----------------------------------------
# Build the example plugins as isolated AArch64 executables (PT_LOAD segments,
# linked at USER_VA_BASE) for the loader to map into a fresh address space.
PLUGIN_LD = plugins/plugin.ld

$(ARM_BUILD_DIR)/plugin_pass.elf: plugins/example_pass/pass.c $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -c $< -o $(ARM_BUILD_DIR)/pass.o
	$(ARM_LD) -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/pass.o

$(ARM_BUILD_DIR)/plugin_evil.elf: plugins/example_evil/evil.c $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -c $< -o $(ARM_BUILD_DIR)/evil.o
	$(ARM_LD) -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/evil.o

# Full-stack loader test on QEMU 'virt' (MMU on): load a passthrough plugin into
# its own address space and run plugin_init at EL0; load a misbehaving plugin
# and confirm the MMU kills it when it touches kernel memory.
VIRT_PLUGIN_ELF  = $(ARM_BUILD_DIR)/virt_plugin.elf
VIRT_PLUGIN_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                   $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
                   $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
                   $(ARCH_ARM_DIR)/elf64.c $(ARCH_ARM_DIR)/plugin_loader.c \
                   $(ARCH_ARM_DIR)/string.c

test-arm-plugin-load-qemu: $(ARM_BUILD_DIR)/plugin_pass.elf $(ARM_BUILD_DIR)/plugin_evil.elf
	@echo "--- plugin ELF self-containment (no undefined imports) ---"
	@for e in plugin_pass plugin_evil; do \
	  u=$$($(CROSS_COMPILE)readelf -sW $(ARM_BUILD_DIR)/$$e.elf | grep -c ' UND ') ; \
	  echo "$$e.elf UND symbols: $$u"; \
	done
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/pg_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/pg_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/pg_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/pg_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/plugin_blob.S          -o $(ARM_BUILD_DIR)/pg_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c   -o $(ARM_BUILD_DIR)/pg_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/plugin_main.c -o $(ARM_BUILD_DIR)/pg_main.o
	for s in $(VIRT_PLUGIN_SRCS); do \
	  o=$(ARM_BUILD_DIR)/pg_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_PLUGIN_ELF) \
	    $(ARM_BUILD_DIR)/pg_start.o $(ARM_BUILD_DIR)/pg_main.o \
	    $(ARM_BUILD_DIR)/pg_uart.o $(ARM_BUILD_DIR)/pg_vectors.o \
	    $(ARM_BUILD_DIR)/pg_entry.o $(ARM_BUILD_DIR)/pg_tramp.o \
	    $(ARM_BUILD_DIR)/pg_blob.o \
	    $(ARM_BUILD_DIR)/pg_pmm.o $(ARM_BUILD_DIR)/pg_mmu.o \
	    $(ARM_BUILD_DIR)/pg_vmem.o $(ARM_BUILD_DIR)/pg_process.o \
	    $(ARM_BUILD_DIR)/pg_exceptions.o $(ARM_BUILD_DIR)/pg_syscalls.o \
	    $(ARM_BUILD_DIR)/pg_elf64.o $(ARM_BUILD_DIR)/pg_plugin_loader.o \
	    $(ARM_BUILD_DIR)/pg_string.o
	rm -f $(ARM_BUILD_DIR)/virt_plugin.log
	-timeout 25 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_plugin.log -net none \
	    -kernel $(VIRT_PLUGIN_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_plugin.log
	@grep -q "PLUGIN-LOAD: PASS" $(ARM_BUILD_DIR)/virt_plugin.log \
	  && echo "QEMU virt plugin-loader test PASSED" \
	  || { echo "QEMU virt plugin-loader test FAILED"; exit 1; }

# ---- M5: shared-memory audio ring buffer (issue #25) ----------------------
# Plugins that read/write the shared ring link the (FP-enabled) ring code.
$(ARM_BUILD_DIR)/arb_plugin.o: $(ARCH_ARM_DIR)/audio_ringbuf.c | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -I$(ARCH_ARM_DIR) -Iplugins -c $< -o $@

$(ARM_BUILD_DIR)/plugin_producer.elf: plugins/example_producer/producer.c \
                                      $(ARM_BUILD_DIR)/arb_plugin.o $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -I$(ARCH_ARM_DIR) -Iplugins -c $< -o $(ARM_BUILD_DIR)/producer.o
	$(ARM_LD) -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/producer.o $(ARM_BUILD_DIR)/arb_plugin.o

$(ARM_BUILD_DIR)/plugin_crasher.elf: plugins/example_crasher/crasher.c \
                                     $(ARM_BUILD_DIR)/arb_plugin.o $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -I$(ARCH_ARM_DIR) -Iplugins -c $< -o $(ARM_BUILD_DIR)/crasher.o
	$(ARM_LD) -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/crasher.o $(ARM_BUILD_DIR)/arb_plugin.o

# The harness main verifies float ramps, so it is built WITH FP (the kernel
# objects stay -mgeneral-regs-only; mixing is ABI-compatible at link time).
HARNESS_FP_CFLAGS = -mcpu=$(ARM_CPU) -ffreestanding -fno-pic -fno-pie -fno-builtin \
                    -Wall -Wextra -std=c11 -O2 -g -I$(ARCH_ARM_DIR) -Iplugins -Iinclude

VIRT_RING_ELF  = $(ARM_BUILD_DIR)/virt_ring.elf
VIRT_RING_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                 $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
                 $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
                 $(ARCH_ARM_DIR)/elf64.c $(ARCH_ARM_DIR)/plugin_loader.c \
                 $(ARCH_ARM_DIR)/audio_ringbuf.c $(ARCH_ARM_DIR)/string.c

test-arm-ring-share-qemu: $(ARM_BUILD_DIR)/plugin_producer.elf $(ARM_BUILD_DIR)/plugin_crasher.elf
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/rs_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/rs_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/rs_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/rs_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/ring_blob.S            -o $(ARM_BUILD_DIR)/rs_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/rs_uart.o
	$(ARM_CC) $(HARNESS_FP_CFLAGS) $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/ringshare_main.c -o $(ARM_BUILD_DIR)/rs_main.o
	for s in $(VIRT_RING_SRCS); do \
	  o=$(ARM_BUILD_DIR)/rs_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_RING_ELF) \
	    $(ARM_BUILD_DIR)/rs_start.o $(ARM_BUILD_DIR)/rs_main.o \
	    $(ARM_BUILD_DIR)/rs_uart.o $(ARM_BUILD_DIR)/rs_vectors.o \
	    $(ARM_BUILD_DIR)/rs_entry.o $(ARM_BUILD_DIR)/rs_tramp.o \
	    $(ARM_BUILD_DIR)/rs_blob.o \
	    $(ARM_BUILD_DIR)/rs_pmm.o $(ARM_BUILD_DIR)/rs_mmu.o \
	    $(ARM_BUILD_DIR)/rs_vmem.o $(ARM_BUILD_DIR)/rs_process.o \
	    $(ARM_BUILD_DIR)/rs_exceptions.o $(ARM_BUILD_DIR)/rs_syscalls.o \
	    $(ARM_BUILD_DIR)/rs_elf64.o $(ARM_BUILD_DIR)/rs_plugin_loader.o \
	    $(ARM_BUILD_DIR)/rs_audio_ringbuf.o $(ARM_BUILD_DIR)/rs_string.o
	rm -f $(ARM_BUILD_DIR)/virt_ring.log
	-timeout 25 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_ring.log -net none \
	    -kernel $(VIRT_RING_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_ring.log
	@grep -q "RING-SHARE: PASS" $(ARM_BUILD_DIR)/virt_ring.log \
	  && echo "QEMU virt shared-ring test PASSED" \
	  || { echo "QEMU virt shared-ring test FAILED"; exit 1; }

# ---- M5: resilient plugin host (issue #26) --------------------------------
# Sine plugins (normal + crashing) link the FP ring code and the sine generator.
$(ARM_BUILD_DIR)/plugin_sine.elf: plugins/example_sine/sine.c audio/sine_gen.c \
                                  $(ARM_BUILD_DIR)/arb_plugin.o $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -I$(ARCH_ARM_DIR) -Iplugins -c plugins/example_sine/sine.c -o $(ARM_BUILD_DIR)/sine.o
	$(ARM_CC) $(PLUGIN_CFLAGS) -ffunction-sections -fdata-sections -Iinclude -c audio/sine_gen.c -o $(ARM_BUILD_DIR)/sine_gen_pl.o
	$(ARM_LD) -T $(PLUGIN_LD) --gc-sections -o $@ $(ARM_BUILD_DIR)/sine.o \
	    $(ARM_BUILD_DIR)/sine_gen_pl.o $(ARM_BUILD_DIR)/arb_plugin.o

$(ARM_BUILD_DIR)/plugin_sine_crash.elf: plugins/example_sine/sine.c audio/sine_gen.c \
                                        $(ARM_BUILD_DIR)/arb_plugin.o $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -DCRASH -Iinclude -I$(ARCH_ARM_DIR) -Iplugins -c plugins/example_sine/sine.c -o $(ARM_BUILD_DIR)/sine_crash.o
	$(ARM_CC) $(PLUGIN_CFLAGS) -ffunction-sections -fdata-sections -Iinclude -c audio/sine_gen.c -o $(ARM_BUILD_DIR)/sine_gen_pl.o
	$(ARM_LD) -T $(PLUGIN_LD) --gc-sections -o $@ $(ARM_BUILD_DIR)/sine_crash.o \
	    $(ARM_BUILD_DIR)/sine_gen_pl.o $(ARM_BUILD_DIR)/arb_plugin.o

# Full-stack host-resilience test on QEMU 'virt' (MMU on): a sine plugin
# produces sound the host plays; a crashing plugin is killed and the host keeps
# running on silence (the M5 "done when").
VIRT_HOST_ELF  = $(ARM_BUILD_DIR)/virt_host.elf
VIRT_HOST_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                 $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
                 $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
                 $(ARCH_ARM_DIR)/elf64.c $(ARCH_ARM_DIR)/plugin_loader.c \
                 $(ARCH_ARM_DIR)/audio_ringbuf.c $(ARCH_ARM_DIR)/plugin_host.c \
                 $(ARCH_ARM_DIR)/string.c

test-arm-plugin-host-qemu: $(ARM_BUILD_DIR)/plugin_sine.elf $(ARM_BUILD_DIR)/plugin_sine_crash.elf
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/ph_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/ph_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/ph_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/ph_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/host_blob.S           -o $(ARM_BUILD_DIR)/ph_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/ph_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) -Iplugins $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/plugin_host_main.c -o $(ARM_BUILD_DIR)/ph_main.o
	for s in $(VIRT_HOST_SRCS); do \
	  o=$(ARM_BUILD_DIR)/ph_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_HOST_ELF) \
	    $(ARM_BUILD_DIR)/ph_start.o $(ARM_BUILD_DIR)/ph_main.o \
	    $(ARM_BUILD_DIR)/ph_uart.o $(ARM_BUILD_DIR)/ph_vectors.o \
	    $(ARM_BUILD_DIR)/ph_entry.o $(ARM_BUILD_DIR)/ph_tramp.o \
	    $(ARM_BUILD_DIR)/ph_blob.o \
	    $(ARM_BUILD_DIR)/ph_pmm.o $(ARM_BUILD_DIR)/ph_mmu.o \
	    $(ARM_BUILD_DIR)/ph_vmem.o $(ARM_BUILD_DIR)/ph_process.o \
	    $(ARM_BUILD_DIR)/ph_exceptions.o $(ARM_BUILD_DIR)/ph_syscalls.o \
	    $(ARM_BUILD_DIR)/ph_elf64.o $(ARM_BUILD_DIR)/ph_plugin_loader.o \
	    $(ARM_BUILD_DIR)/ph_audio_ringbuf.o $(ARM_BUILD_DIR)/ph_plugin_host.o \
	    $(ARM_BUILD_DIR)/ph_string.o
	rm -f $(ARM_BUILD_DIR)/virt_host.log
	-timeout 25 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_host.log -net none \
	    -kernel $(VIRT_HOST_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_host.log
	@grep -q "PLUGIN-HOST: PASS" $(ARM_BUILD_DIR)/virt_host.log \
	  && echo "QEMU virt resilient-host test PASSED" \
	  || { echo "QEMU virt resilient-host test FAILED"; exit 1; }

# ---- M6: audio graph (issue #27) ------------------------------------------
$(ARM_BUILD_DIR)/plugin_effect.elf: plugins/example_effect/effect.c \
                                    $(ARM_BUILD_DIR)/arb_plugin.o $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -I$(ARCH_ARM_DIR) -Iplugins -c $< -o $(ARM_BUILD_DIR)/effect.o
	$(ARM_LD) -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/effect.o $(ARM_BUILD_DIR)/arb_plugin.o

# Audio-graph processing test on QEMU 'virt' (MMU on): drive isolated synth and
# effect plugins through ring-buffer edges in the graph's topological order;
# 2-node and 3-node chains produce sound, the wrong order produces silence.
VIRT_GRAPH_ELF  = $(ARM_BUILD_DIR)/virt_graph.elf
VIRT_GRAPH_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                  $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
                  $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
                  $(ARCH_ARM_DIR)/elf64.c $(ARCH_ARM_DIR)/plugin_loader.c \
                  $(ARCH_ARM_DIR)/audio_ringbuf.c $(ARCH_ARM_DIR)/audio_graph.c \
                  $(ARCH_ARM_DIR)/string.c

test-arm-graph-qemu: $(ARM_BUILD_DIR)/plugin_sine.elf $(ARM_BUILD_DIR)/plugin_effect.elf
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/gr_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/gr_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/gr_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/gr_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/graph_blob.S          -o $(ARM_BUILD_DIR)/gr_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/gr_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) -Iplugins $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/graph_main.c -o $(ARM_BUILD_DIR)/gr_main.o
	for s in $(VIRT_GRAPH_SRCS); do \
	  o=$(ARM_BUILD_DIR)/gr_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_GRAPH_ELF) \
	    $(ARM_BUILD_DIR)/gr_start.o $(ARM_BUILD_DIR)/gr_main.o \
	    $(ARM_BUILD_DIR)/gr_uart.o $(ARM_BUILD_DIR)/gr_vectors.o \
	    $(ARM_BUILD_DIR)/gr_entry.o $(ARM_BUILD_DIR)/gr_tramp.o \
	    $(ARM_BUILD_DIR)/gr_blob.o \
	    $(ARM_BUILD_DIR)/gr_pmm.o $(ARM_BUILD_DIR)/gr_mmu.o \
	    $(ARM_BUILD_DIR)/gr_vmem.o $(ARM_BUILD_DIR)/gr_process.o \
	    $(ARM_BUILD_DIR)/gr_exceptions.o $(ARM_BUILD_DIR)/gr_syscalls.o \
	    $(ARM_BUILD_DIR)/gr_elf64.o $(ARM_BUILD_DIR)/gr_plugin_loader.o \
	    $(ARM_BUILD_DIR)/gr_audio_ringbuf.o $(ARM_BUILD_DIR)/gr_audio_graph.o \
	    $(ARM_BUILD_DIR)/gr_string.o
	rm -f $(ARM_BUILD_DIR)/virt_graph.log
	-timeout 25 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_graph.log -net none \
	    -kernel $(VIRT_GRAPH_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_graph.log
	@grep -q "AUDIO-GRAPH: PASS" $(ARM_BUILD_DIR)/virt_graph.log \
	  && echo "QEMU virt audio-graph test PASSED" \
	  || { echo "QEMU virt audio-graph test FAILED"; exit 1; }

# ---- M6: reference low-pass filter plugin (issue #29) ---------------------
# Host unit test for the filter DSP (uses libm only to synthesise test signals;
# the plugin itself uses none).
ARM_FILTER_TEST_SRCS = tests/arm64/filter_test.c plugins/effect_filter/main.c
ARM_FILTER_TEST_BIN  = $(ARM_BUILD_DIR)/filter_test

test-arm-filter: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -Iinclude $(ARM_FILTER_TEST_SRCS) -o $(ARM_FILTER_TEST_BIN) -lm
	$(ARM_FILTER_TEST_BIN)

# Standalone AArch64 ELF for the filter plugin (and the other example plugins).
$(ARM_BUILD_DIR)/plugin_effect_filter.elf: plugins/effect_filter/main.c $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -c $< -o $(ARM_BUILD_DIR)/effect_filter.o
	$(ARM_LD) -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/effect_filter.o

# `make plugins`: build every example plugin as a standalone AArch64 ELF and
# confirm the filter plugin has no kernel imports.
.PHONY: plugins
plugins: $(ARM_BUILD_DIR)/plugin_pass.elf $(ARM_BUILD_DIR)/plugin_sine.elf \
         $(ARM_BUILD_DIR)/plugin_effect.elf $(ARM_BUILD_DIR)/plugin_producer.elf \
         $(ARM_BUILD_DIR)/plugin_controller.elf \
         $(ARM_BUILD_DIR)/plugin_effect_filter.elf
	@echo "--- plugin ELFs built ---"
	@ls -1 $(ARM_BUILD_DIR)/plugin_*.elf
	@echo "--- effect_filter import check ---"
	@# Count UNDEFINED symbols that have a name (the index-0 null symbol is UND
	@# with an empty name and is not an import).
	@u=$$($(CROSS_COMPILE)readelf -sW $(ARM_BUILD_DIR)/plugin_effect_filter.elf \
	      | awk '$$7 == "UND" && $$8 != "" { c++ } END { print c + 0 }') ; \
	  echo "effect_filter named-import symbols: $$u" ; \
	  test "$$u" -eq 0 \
	    && echo "make plugins: effect_filter is self-contained (no kernel imports)" \
	    || { echo "FAILED: effect_filter has undefined imports"; exit 1; }

# ---- M6: graph control syscalls (issue #28) -------------------------------
$(ARM_BUILD_DIR)/plugin_controller.elf: plugins/example_controller/controller.c $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -I$(ARCH_ARM_DIR) -Iplugins -c $< -o $(ARM_BUILD_DIR)/controller.o
	$(ARM_LD) -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/controller.o

# Control-syscall test on QEMU 'virt' (MMU on): an EL0 client wires/unwires the
# graph at runtime via syscalls; the kernel services them through the control
# plane and verifies connect/duplicate/list/disconnect.
VIRT_GCTL_ELF  = $(ARM_BUILD_DIR)/virt_gctl.elf
VIRT_GCTL_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                 $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
                 $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
                 $(ARCH_ARM_DIR)/elf64.c $(ARCH_ARM_DIR)/plugin_loader.c \
                 $(ARCH_ARM_DIR)/audio_ringbuf.c $(ARCH_ARM_DIR)/audio_graph.c \
                 $(ARCH_ARM_DIR)/graph_control.c $(ARCH_ARM_DIR)/string.c

test-arm-graph-ctl-qemu: $(ARM_BUILD_DIR)/plugin_controller.elf
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/gc_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/gc_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/gc_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/gc_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/ctl_blob.S            -o $(ARM_BUILD_DIR)/gc_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/gc_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) -Iplugins $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/graph_ctl_main.c -o $(ARM_BUILD_DIR)/gc_main.o
	for s in $(VIRT_GCTL_SRCS); do \
	  o=$(ARM_BUILD_DIR)/gc_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_GCTL_ELF) \
	    $(ARM_BUILD_DIR)/gc_start.o $(ARM_BUILD_DIR)/gc_main.o \
	    $(ARM_BUILD_DIR)/gc_uart.o $(ARM_BUILD_DIR)/gc_vectors.o \
	    $(ARM_BUILD_DIR)/gc_entry.o $(ARM_BUILD_DIR)/gc_tramp.o \
	    $(ARM_BUILD_DIR)/gc_blob.o \
	    $(ARM_BUILD_DIR)/gc_pmm.o $(ARM_BUILD_DIR)/gc_mmu.o \
	    $(ARM_BUILD_DIR)/gc_vmem.o $(ARM_BUILD_DIR)/gc_process.o \
	    $(ARM_BUILD_DIR)/gc_exceptions.o $(ARM_BUILD_DIR)/gc_syscalls.o \
	    $(ARM_BUILD_DIR)/gc_elf64.o $(ARM_BUILD_DIR)/gc_plugin_loader.o \
	    $(ARM_BUILD_DIR)/gc_audio_ringbuf.o $(ARM_BUILD_DIR)/gc_audio_graph.o \
	    $(ARM_BUILD_DIR)/gc_graph_control.o $(ARM_BUILD_DIR)/gc_string.o
	rm -f $(ARM_BUILD_DIR)/virt_gctl.log
	-timeout 25 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_gctl.log -net none \
	    -kernel $(VIRT_GCTL_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_gctl.log
	@grep -q "GRAPH-CTL: PASS" $(ARM_BUILD_DIR)/virt_gctl.log \
	  && echo "QEMU virt graph-control test PASSED" \
	  || { echo "QEMU virt graph-control test FAILED"; exit 1; }

# The capture input node end to end on QEMU 'virt' (issue #84): MMU on, single
# core.  The input node pulls captured blocks from the I2S capture ring and is
# wired through the control plane: input -> filter -> DAC carries audio,
# input -> DAC is bit-exact against the capture, an empty ring underruns to
# silence, and a patch containing the input node saves to the FAT SD card,
# reloads, and rebuilds the identical graph.
VIRT_GIN_ELF  = $(ARM_BUILD_DIR)/virt_graph_input.elf
VIRT_GIN_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
                $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
                $(ARCH_ARM_DIR)/elf64.c $(ARCH_ARM_DIR)/plugin_loader.c \
                $(ARCH_ARM_DIR)/audio_ringbuf.c $(ARCH_ARM_DIR)/audio_graph.c \
                $(ARCH_ARM_DIR)/graph_control.c $(ARCH_ARM_DIR)/param_queue.c \
                $(ARCH_ARM_DIR)/plugin_mgr.c $(ARCH_ARM_DIR)/patch.c \
                $(ARCH_ARM_DIR)/vfs.c $(ARCH_ARM_DIR)/fat.c \
                $(ARCH_ARM_DIR)/sandbox.c $(ARCH_ARM_DIR)/string.c \
                drivers/i2s_capture.c

test-arm-graph-input-qemu: $(ARM_BUILD_DIR)/plugin_effect_filter.elf
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/gi_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/gi_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/gi_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/gi_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/graph_input_blob.S     -o $(ARM_BUILD_DIR)/gi_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/gi_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) -Iinclude -Iplugins $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/graph_input_main.c -o $(ARM_BUILD_DIR)/gi_main.o
	$(ARM_CC) $(ARM_TARGET_FLAGS) -mcpu=$(ARM_CPU) -ffreestanding -fno-pic -fno-pie -O2 -std=c11 -Iinclude -c $(VIRT_DIR)/gi_conv.c -o $(ARM_BUILD_DIR)/gi_conv.o
	for s in $(VIRT_GIN_SRCS); do \
	  o=$(ARM_BUILD_DIR)/gi_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -Iinclude -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_GIN_ELF) \
	    $(ARM_BUILD_DIR)/gi_start.o $(ARM_BUILD_DIR)/gi_main.o \
	    $(ARM_BUILD_DIR)/gi_uart.o $(ARM_BUILD_DIR)/gi_vectors.o \
	    $(ARM_BUILD_DIR)/gi_entry.o $(ARM_BUILD_DIR)/gi_tramp.o \
	    $(ARM_BUILD_DIR)/gi_blob.o \
	    $(ARM_BUILD_DIR)/gi_pmm.o $(ARM_BUILD_DIR)/gi_mmu.o \
	    $(ARM_BUILD_DIR)/gi_vmem.o $(ARM_BUILD_DIR)/gi_process.o \
	    $(ARM_BUILD_DIR)/gi_exceptions.o $(ARM_BUILD_DIR)/gi_syscalls.o \
	    $(ARM_BUILD_DIR)/gi_elf64.o $(ARM_BUILD_DIR)/gi_plugin_loader.o \
	    $(ARM_BUILD_DIR)/gi_audio_ringbuf.o $(ARM_BUILD_DIR)/gi_audio_graph.o \
	    $(ARM_BUILD_DIR)/gi_graph_control.o $(ARM_BUILD_DIR)/gi_param_queue.o \
	    $(ARM_BUILD_DIR)/gi_plugin_mgr.o $(ARM_BUILD_DIR)/gi_patch.o \
	    $(ARM_BUILD_DIR)/gi_vfs.o $(ARM_BUILD_DIR)/gi_fat.o \
	    $(ARM_BUILD_DIR)/gi_sandbox.o $(ARM_BUILD_DIR)/gi_string.o \
	    $(ARM_BUILD_DIR)/gi_i2s_capture.o $(ARM_BUILD_DIR)/gi_conv.o
	rm -f $(ARM_BUILD_DIR)/virt_graph_input.log
	-timeout 25 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_graph_input.log -net none \
	    -kernel $(VIRT_GIN_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_graph_input.log
	@grep -q "GRAPH-INPUT: PASS" $(ARM_BUILD_DIR)/virt_graph_input.log \
	  && echo "QEMU virt graph-input test PASSED" \
	  || { echo "QEMU virt graph-input test FAILED"; exit 1; }

# The round-trip latency demo on QEMU 'virt' (issue #85): MMU on, single core.
# The real graph (input -> filter -> DAC) runs with a one-block capture ring and
# a one-block DAC-output ring, and a modelled loopback feeds the DAC output back
# into the capture source.  A DC step injected at the start travels the whole
# loop; CNTPCT_EL0 timestamps a block at the capture edge and at the DAC output,
# and the harness asserts the per-block delay is exactly 2 (the buffer-
# accounting prediction) and reports min/max/mean round-trip microseconds.
VIRT_RT_ELF  = $(ARM_BUILD_DIR)/virt_roundtrip.elf
VIRT_RT_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
               $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
               $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
               $(ARCH_ARM_DIR)/elf64.c $(ARCH_ARM_DIR)/plugin_loader.c \
               $(ARCH_ARM_DIR)/audio_ringbuf.c $(ARCH_ARM_DIR)/audio_graph.c \
               $(ARCH_ARM_DIR)/graph_control.c $(ARCH_ARM_DIR)/param_queue.c \
               $(ARCH_ARM_DIR)/plugin_mgr.c $(ARCH_ARM_DIR)/patch.c \
               $(ARCH_ARM_DIR)/vfs.c $(ARCH_ARM_DIR)/fat.c \
               $(ARCH_ARM_DIR)/sandbox.c $(ARCH_ARM_DIR)/string.c \
               $(ARCH_ARM_DIR)/latency.c drivers/i2s_capture.c

test-arm-roundtrip-qemu: $(ARM_BUILD_DIR)/plugin_effect_filter.elf
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/rt_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/rt_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/rt_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/rt_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/roundtrip_blob.S       -o $(ARM_BUILD_DIR)/rt_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/rt_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) -Iinclude -Iplugins $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/roundtrip_main.c -o $(ARM_BUILD_DIR)/rt_main.o
	$(ARM_CC) $(ARM_TARGET_FLAGS) -mcpu=$(ARM_CPU) -ffreestanding -fno-pic -fno-pie -O2 -std=c11 -Iinclude -c $(VIRT_DIR)/rt_conv.c -o $(ARM_BUILD_DIR)/rt_conv.o
	for s in $(VIRT_RT_SRCS); do \
	  o=$(ARM_BUILD_DIR)/rt_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -Iinclude -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_RT_ELF) \
	    $(ARM_BUILD_DIR)/rt_start.o $(ARM_BUILD_DIR)/rt_main.o \
	    $(ARM_BUILD_DIR)/rt_uart.o $(ARM_BUILD_DIR)/rt_vectors.o \
	    $(ARM_BUILD_DIR)/rt_entry.o $(ARM_BUILD_DIR)/rt_tramp.o \
	    $(ARM_BUILD_DIR)/rt_blob.o \
	    $(ARM_BUILD_DIR)/rt_pmm.o $(ARM_BUILD_DIR)/rt_mmu.o \
	    $(ARM_BUILD_DIR)/rt_vmem.o $(ARM_BUILD_DIR)/rt_process.o \
	    $(ARM_BUILD_DIR)/rt_exceptions.o $(ARM_BUILD_DIR)/rt_syscalls.o \
	    $(ARM_BUILD_DIR)/rt_elf64.o $(ARM_BUILD_DIR)/rt_plugin_loader.o \
	    $(ARM_BUILD_DIR)/rt_audio_ringbuf.o $(ARM_BUILD_DIR)/rt_audio_graph.o \
	    $(ARM_BUILD_DIR)/rt_graph_control.o $(ARM_BUILD_DIR)/rt_param_queue.o \
	    $(ARM_BUILD_DIR)/rt_plugin_mgr.o $(ARM_BUILD_DIR)/rt_patch.o \
	    $(ARM_BUILD_DIR)/rt_vfs.o $(ARM_BUILD_DIR)/rt_fat.o \
	    $(ARM_BUILD_DIR)/rt_sandbox.o $(ARM_BUILD_DIR)/rt_string.o \
	    $(ARM_BUILD_DIR)/rt_latency.o $(ARM_BUILD_DIR)/rt_i2s_capture.o \
	    $(ARM_BUILD_DIR)/rt_conv.o
	rm -f $(ARM_BUILD_DIR)/virt_roundtrip.log
	-timeout 25 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_roundtrip.log -net none \
	    -kernel $(VIRT_RT_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_roundtrip.log
	@grep -q "ROUNDTRIP: PASS" $(ARM_BUILD_DIR)/virt_roundtrip.log \
	  && echo "QEMU virt round-trip test PASSED" \
	  || { echo "QEMU virt round-trip test FAILED"; exit 1; }

# ---- M7: live parameter changes (issue #33) -------------------------------
# Integration host test: CC -> param map -> lock-free queue -> filter, checking
# click-free modulation, overflow handling, and latency.  Uses libm for the
# test sine only (the filter and queue use none).
ARM_PARAMLIVE_TEST_SRCS = tests/arm64/param_live_test.c $(ARCH_ARM_DIR)/param_queue.c \
                          plugins/effect_filter/main.c
test-arm-param-live: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -Iinclude -I$(ARCH_ARM_DIR) \
	      $(ARM_PARAMLIVE_TEST_SRCS) -o $(ARM_BUILD_DIR)/param_live_test -lm
	$(ARM_BUILD_DIR)/param_live_test

# Live parameter modulation on QEMU 'virt': CC -> map -> queue -> filter, while
# a tone plays; checks click-free sweep + cutoff change on-target (FP enabled).
VIRT_PARAMLIVE_ELF = $(ARM_BUILD_DIR)/virt_paramlive.elf
test-arm-param-live-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/pl_start.o
	$(ARM_CC) $(ARM_CFLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/pl_uart.o
	$(ARM_CC) $(HARNESS_FP_CFLAGS) -c $(VIRT_DIR)/param_live_main.c -o $(ARM_BUILD_DIR)/pl_main.o
	$(ARM_CC) $(ARM_CFLAGS) -c $(ARCH_ARM_DIR)/param_queue.c -o $(ARM_BUILD_DIR)/pl_pq.o
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -c plugins/effect_filter/main.c -o $(ARM_BUILD_DIR)/pl_filter.o
	$(ARM_LD) -T $(VIRT_DIR)/virt.ld -o $(VIRT_PARAMLIVE_ELF) \
	    $(ARM_BUILD_DIR)/pl_start.o $(ARM_BUILD_DIR)/pl_main.o \
	    $(ARM_BUILD_DIR)/pl_uart.o $(ARM_BUILD_DIR)/pl_pq.o \
	    $(ARM_BUILD_DIR)/pl_filter.o
	rm -f $(ARM_BUILD_DIR)/virt_paramlive.log
	-timeout 15 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_paramlive.log -net none \
	    -kernel $(VIRT_PARAMLIVE_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_paramlive.log
	@grep -q "PARAM-LIVE: PASS" $(ARM_BUILD_DIR)/virt_paramlive.log \
	  && echo "QEMU virt live-parameter test PASSED" \
	  || { echo "QEMU virt live-parameter test FAILED"; exit 1; }

# ---- M7: MIDI input (issue #31) -------------------------------------------
# Host unit tests for the MIDI parser and event ring.
ARM_MIDI_TEST_SRCS = tests/arm64/midi_test.c $(ARCH_ARM_DIR)/midi.c
test-arm-midi: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -I$(ARCH_ARM_DIR) $(ARM_MIDI_TEST_SRCS) -o $(ARM_BUILD_DIR)/midi_test
	$(ARM_BUILD_DIR)/midi_test

# ---- M7: CV/Gate input (issue #32) ----------------------------------------
# Host unit tests for the MCP3208 decode, 1V/oct scaling, gate edges, and
# MIDI/CV coexistence on one ring.
ARM_CVGATE_TEST_SRCS = tests/arm64/cvgate_test.c $(ARCH_ARM_DIR)/cvgate.c $(ARCH_ARM_DIR)/midi.c
test-arm-cvgate: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -I$(ARCH_ARM_DIR) $(ARM_CVGATE_TEST_SRCS) -o $(ARM_BUILD_DIR)/cvgate_test
	$(ARM_BUILD_DIR)/cvgate_test

# CV/Gate listener on QEMU 'virt': simulated gate edges + pitch CV interleaved
# with MIDI, decoded onto one event ring and logged over the UART.
VIRT_CVGATE_ELF = $(ARM_BUILD_DIR)/virt_cvgate.elf
test-arm-cvgate-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/cg_start.o
	$(ARM_CC) $(ARM_CFLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/cg_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) $(ARM_CFLAGS) -c $(VIRT_DIR)/cvgate_main.c -o $(ARM_BUILD_DIR)/cg_main.o
	$(ARM_CC) $(ARM_CFLAGS) -c $(ARCH_ARM_DIR)/cvgate.c -o $(ARM_BUILD_DIR)/cg_cvgate.o
	$(ARM_CC) $(ARM_CFLAGS) -c $(ARCH_ARM_DIR)/midi.c   -o $(ARM_BUILD_DIR)/cg_midi.o
	$(ARM_LD) -T $(VIRT_DIR)/virt.ld -o $(VIRT_CVGATE_ELF) \
	    $(ARM_BUILD_DIR)/cg_start.o $(ARM_BUILD_DIR)/cg_main.o \
	    $(ARM_BUILD_DIR)/cg_uart.o $(ARM_BUILD_DIR)/cg_cvgate.o \
	    $(ARM_BUILD_DIR)/cg_midi.o
	rm -f $(ARM_BUILD_DIR)/virt_cvgate.log
	-timeout 15 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_cvgate.log -net none \
	    -kernel $(VIRT_CVGATE_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_cvgate.log
	@grep -q "CVGATE: PASS" $(ARM_BUILD_DIR)/virt_cvgate.log \
	  && echo "QEMU virt CV/Gate listener test PASSED" \
	  || { echo "QEMU virt CV/Gate listener test FAILED"; exit 1; }

# MIDI listener on QEMU 'virt': feed a byte stream through the parser into the
# event ring and log the decoded events over the UART.
VIRT_MIDI_ELF = $(ARM_BUILD_DIR)/virt_midi.elf
test-arm-midi-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/md_start.o
	$(ARM_CC) $(ARM_CFLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/md_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) $(ARM_CFLAGS) -c $(VIRT_DIR)/midi_main.c -o $(ARM_BUILD_DIR)/md_main.o
	$(ARM_CC) $(ARM_CFLAGS) -c $(ARCH_ARM_DIR)/midi.c -o $(ARM_BUILD_DIR)/md_midi.o
	$(ARM_LD) -T $(VIRT_DIR)/virt.ld -o $(VIRT_MIDI_ELF) \
	    $(ARM_BUILD_DIR)/md_start.o $(ARM_BUILD_DIR)/md_main.o \
	    $(ARM_BUILD_DIR)/md_uart.o $(ARM_BUILD_DIR)/md_midi.o
	rm -f $(ARM_BUILD_DIR)/virt_midi.log
	-timeout 15 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_midi.log -net none \
	    -kernel $(VIRT_MIDI_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_midi.log
	@grep -q "MIDI: PASS" $(ARM_BUILD_DIR)/virt_midi.log \
	  && echo "QEMU virt MIDI listener test PASSED" \
	  || { echo "QEMU virt MIDI listener test FAILED"; exit 1; }

# ---- M7: control syscalls (issue #30) -------------------------------------
# Host unit test for the lock-free parameter queue.
ARM_PQ_TEST_SRCS = tests/arm64/param_queue_test.c $(ARCH_ARM_DIR)/param_queue.c
test-arm-param-queue: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -I$(ARCH_ARM_DIR) $(ARM_PQ_TEST_SRCS) -o $(ARM_BUILD_DIR)/param_queue_test -lpthread
	$(ARM_BUILD_DIR)/param_queue_test

# Control test plugins.
$(ARM_BUILD_DIR)/pq_plugin.o: $(ARCH_ARM_DIR)/param_queue.c | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -I$(ARCH_ARM_DIR) -Iplugins -c $< -o $@

$(ARM_BUILD_DIR)/plugin_echo.elf: plugins/example_echo/echo.c $(ARM_BUILD_DIR)/pq_plugin.o $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -I$(ARCH_ARM_DIR) -Iplugins -c $< -o $(ARM_BUILD_DIR)/echo.o
	$(ARM_LD) -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/echo.o $(ARM_BUILD_DIR)/pq_plugin.o

$(ARM_BUILD_DIR)/plugin_ctl2.elf: plugins/example_ctl2/ctl2.c $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -I$(ARCH_ARM_DIR) -Iplugins -c $< -o $(ARM_BUILD_DIR)/ctl2.o
	$(ARM_LD) -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/ctl2.o

# M8 rejection-test plugins (issue #34).  badabi reports an incompatible ABI
# major version; badimport pulls in a symbol outside the plugin ABI - linked
# with --unresolved-symbols=ignore-all so the forbidden import survives into the
# symbol table for the loader to catch instead of failing the link.
$(ARM_BUILD_DIR)/plugin_badabi.elf: plugins/example_badabi/badabi.c $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -c $< -o $(ARM_BUILD_DIR)/badabi.o
	$(ARM_LD) -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/badabi.o

$(ARM_BUILD_DIR)/plugin_badimport.elf: plugins/example_badimport/badimport.c $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -c $< -o $(ARM_BUILD_DIR)/badimport.o
	$(ARM_LD) -shared -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/badimport.o

# M8 sandbox-test plugins (issue #35): sbsvc issues a syscall from its own body;
# sbmem dereferences memory it was never granted.  Both are killed at run.
$(ARM_BUILD_DIR)/plugin_sbsvc.elf: plugins/example_sbsvc/sbsvc.c $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -c $< -o $(ARM_BUILD_DIR)/sbsvc.o
	$(ARM_LD) -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/sbsvc.o

$(ARM_BUILD_DIR)/plugin_sbmem.elf: plugins/example_sbmem/sbmem.c $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -c $< -o $(ARM_BUILD_DIR)/sbmem.o
	$(ARM_LD) -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/sbmem.o

# M8 resilience-demo plugins (issue #36): a clean sine, a null-deref crasher,
# and an actively malicious plugin.  good pulls in the sine generator; both
# hostile plugins are self-contained.
$(ARM_BUILD_DIR)/plugin_good.elf: plugins/test/good_plugin.c audio/sine_gen.c $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -c plugins/test/good_plugin.c -o $(ARM_BUILD_DIR)/good_plugin.o
	$(ARM_CC) $(PLUGIN_CFLAGS) -ffunction-sections -fdata-sections -Iinclude -c audio/sine_gen.c -o $(ARM_BUILD_DIR)/sine_gen_good.o
	$(ARM_LD) -T $(PLUGIN_LD) --gc-sections -o $@ $(ARM_BUILD_DIR)/good_plugin.o $(ARM_BUILD_DIR)/sine_gen_good.o

$(ARM_BUILD_DIR)/plugin_crash.elf: plugins/test/crash_plugin.c $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -c $< -o $(ARM_BUILD_DIR)/crash_plugin.o
	$(ARM_LD) -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/crash_plugin.o

$(ARM_BUILD_DIR)/plugin_evil2.elf: plugins/test/evil_plugin.c $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -c $< -o $(ARM_BUILD_DIR)/evil_plugin.o
	$(ARM_LD) -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/evil_plugin.o

# Issue #78 (M12) demo plugins: a permanent CPU hog and a transient one.
$(ARM_BUILD_DIR)/plugin_hog.elf: plugins/test/hog_plugin.c $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -c $< -o $(ARM_BUILD_DIR)/hog_plugin.o
	$(ARM_LD) -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/hog_plugin.o

$(ARM_BUILD_DIR)/plugin_blip.elf: plugins/test/blip_plugin.c $(PLUGIN_LD) | $(ARM_BUILD_DIR)
	$(ARM_CC) $(PLUGIN_CFLAGS) -Iinclude -c $< -o $(ARM_BUILD_DIR)/blip_plugin.o
	$(ARM_LD) -T $(PLUGIN_LD) -o $@ $(ARM_BUILD_DIR)/blip_plugin.o

# Full control-syscall test on QEMU 'virt' (MMU on): leak-free load/unload,
# parameter delivery, and all five syscalls callable from EL0.
VIRT_CTL_ELF  = $(ARM_BUILD_DIR)/virt_control.elf
VIRT_CTL_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
                $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
                $(ARCH_ARM_DIR)/elf64.c $(ARCH_ARM_DIR)/plugin_loader.c \
                $(ARCH_ARM_DIR)/audio_ringbuf.c $(ARCH_ARM_DIR)/audio_graph.c \
                $(ARCH_ARM_DIR)/graph_control.c $(ARCH_ARM_DIR)/param_queue.c \
                $(ARCH_ARM_DIR)/plugin_mgr.c $(ARCH_ARM_DIR)/patch.c $(ARCH_ARM_DIR)/vfs.c \
                $(ARCH_ARM_DIR)/fat.c $(ARCH_ARM_DIR)/string.c

test-arm-control-qemu: $(ARM_BUILD_DIR)/plugin_pass.elf $(ARM_BUILD_DIR)/plugin_echo.elf \
                       $(ARM_BUILD_DIR)/plugin_ctl2.elf
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/ct_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/ct_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/ct_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/ct_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/control_blob.S        -o $(ARM_BUILD_DIR)/ct_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/ct_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) -Iplugins $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/control_main.c -o $(ARM_BUILD_DIR)/ct_main.o
	for s in $(VIRT_CTL_SRCS); do \
	  o=$(ARM_BUILD_DIR)/ct_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_CTL_ELF) \
	    $(ARM_BUILD_DIR)/ct_start.o $(ARM_BUILD_DIR)/ct_main.o \
	    $(ARM_BUILD_DIR)/ct_uart.o $(ARM_BUILD_DIR)/ct_vectors.o \
	    $(ARM_BUILD_DIR)/ct_entry.o $(ARM_BUILD_DIR)/ct_tramp.o \
	    $(ARM_BUILD_DIR)/ct_blob.o \
	    $(ARM_BUILD_DIR)/ct_pmm.o $(ARM_BUILD_DIR)/ct_mmu.o \
	    $(ARM_BUILD_DIR)/ct_vmem.o $(ARM_BUILD_DIR)/ct_process.o \
	    $(ARM_BUILD_DIR)/ct_exceptions.o $(ARM_BUILD_DIR)/ct_syscalls.o \
	    $(ARM_BUILD_DIR)/ct_elf64.o $(ARM_BUILD_DIR)/ct_plugin_loader.o \
	    $(ARM_BUILD_DIR)/ct_audio_ringbuf.o $(ARM_BUILD_DIR)/ct_audio_graph.o \
	    $(ARM_BUILD_DIR)/ct_graph_control.o $(ARM_BUILD_DIR)/ct_param_queue.o \
	    $(ARM_BUILD_DIR)/ct_plugin_mgr.o $(ARM_BUILD_DIR)/ct_patch.o $(ARM_BUILD_DIR)/ct_vfs.o \
	    $(ARM_BUILD_DIR)/ct_fat.o $(ARM_BUILD_DIR)/ct_string.o
	rm -f $(ARM_BUILD_DIR)/virt_control.log
	-timeout 25 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_control.log -net none \
	    -kernel $(VIRT_CTL_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_control.log
	@grep -q "CONTROL: PASS" $(ARM_BUILD_DIR)/virt_control.log \
	  && echo "QEMU virt control-syscall test PASSED" \
	  || { echo "QEMU virt control-syscall test FAILED"; exit 1; }

# Full-stack SD/FAT loader test on QEMU 'virt' (MMU on, issue #34): load a
# plugin from an in-RAM FAT16 "SD card" by path, load the same loader from the
# ramdisk, and reject a wrong-ABI plugin, a disallowed-import plugin, and a
# missing path - all in isolated address spaces with no leaks.
VIRT_SD_ELF  = $(ARM_BUILD_DIR)/virt_sd.elf
VIRT_SD_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
               $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
               $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
               $(ARCH_ARM_DIR)/elf64.c $(ARCH_ARM_DIR)/plugin_loader.c \
               $(ARCH_ARM_DIR)/audio_ringbuf.c $(ARCH_ARM_DIR)/audio_graph.c \
               $(ARCH_ARM_DIR)/graph_control.c $(ARCH_ARM_DIR)/param_queue.c \
               $(ARCH_ARM_DIR)/plugin_mgr.c $(ARCH_ARM_DIR)/patch.c $(ARCH_ARM_DIR)/vfs.c \
               $(ARCH_ARM_DIR)/fat.c $(ARCH_ARM_DIR)/string.c

test-arm-sd-load-qemu: $(ARM_BUILD_DIR)/plugin_pass.elf \
                       $(ARM_BUILD_DIR)/plugin_badabi.elf \
                       $(ARM_BUILD_DIR)/plugin_badimport.elf
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/sd_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/sd_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/sd_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/sd_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/sd_blob.S             -o $(ARM_BUILD_DIR)/sd_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/sd_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) -Iplugins $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/sd_main.c -o $(ARM_BUILD_DIR)/sd_main.o
	for s in $(VIRT_SD_SRCS); do \
	  o=$(ARM_BUILD_DIR)/sd_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_SD_ELF) \
	    $(ARM_BUILD_DIR)/sd_start.o $(ARM_BUILD_DIR)/sd_main.o \
	    $(ARM_BUILD_DIR)/sd_uart.o $(ARM_BUILD_DIR)/sd_vectors.o \
	    $(ARM_BUILD_DIR)/sd_entry.o $(ARM_BUILD_DIR)/sd_tramp.o \
	    $(ARM_BUILD_DIR)/sd_blob.o \
	    $(ARM_BUILD_DIR)/sd_pmm.o $(ARM_BUILD_DIR)/sd_mmu.o \
	    $(ARM_BUILD_DIR)/sd_vmem.o $(ARM_BUILD_DIR)/sd_process.o \
	    $(ARM_BUILD_DIR)/sd_exceptions.o $(ARM_BUILD_DIR)/sd_syscalls.o \
	    $(ARM_BUILD_DIR)/sd_elf64.o $(ARM_BUILD_DIR)/sd_plugin_loader.o \
	    $(ARM_BUILD_DIR)/sd_audio_ringbuf.o $(ARM_BUILD_DIR)/sd_audio_graph.o \
	    $(ARM_BUILD_DIR)/sd_graph_control.o $(ARM_BUILD_DIR)/sd_param_queue.o \
	    $(ARM_BUILD_DIR)/sd_plugin_mgr.o $(ARM_BUILD_DIR)/sd_patch.o $(ARM_BUILD_DIR)/sd_vfs.o \
	    $(ARM_BUILD_DIR)/sd_fat.o $(ARM_BUILD_DIR)/sd_string.o
	rm -f $(ARM_BUILD_DIR)/virt_sd.log
	-timeout 25 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_sd.log -net none \
	    -kernel $(VIRT_SD_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_sd.log
	@grep -q "SD: PASS" $(ARM_BUILD_DIR)/virt_sd.log \
	  && echo "QEMU virt SD/FAT loader test PASSED" \
	  || { echo "QEMU virt SD/FAT loader test FAILED"; exit 1; }

# Full-stack sandbox test on QEMU 'virt' (MMU on, issue #35): audit a loaded
# plugin's page tables (clean + catches an injected mapping), kill a plugin
# that issues an SVC from its body, and kill a plugin that touches memory it
# was never granted.
VIRT_SB_ELF  = $(ARM_BUILD_DIR)/virt_sandbox.elf
VIRT_SB_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
               $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
               $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
               $(ARCH_ARM_DIR)/elf64.c $(ARCH_ARM_DIR)/plugin_loader.c \
               $(ARCH_ARM_DIR)/audio_ringbuf.c $(ARCH_ARM_DIR)/audio_graph.c \
               $(ARCH_ARM_DIR)/graph_control.c $(ARCH_ARM_DIR)/param_queue.c \
               $(ARCH_ARM_DIR)/plugin_mgr.c $(ARCH_ARM_DIR)/patch.c $(ARCH_ARM_DIR)/vfs.c \
               $(ARCH_ARM_DIR)/fat.c $(ARCH_ARM_DIR)/sandbox.c \
               $(ARCH_ARM_DIR)/string.c

test-arm-sandbox-qemu: $(ARM_BUILD_DIR)/plugin_pass.elf \
                       $(ARM_BUILD_DIR)/plugin_sbsvc.elf \
                       $(ARM_BUILD_DIR)/plugin_sbmem.elf
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/sb_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/sb_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/sb_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/sb_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/sandbox_blob.S         -o $(ARM_BUILD_DIR)/sb_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/sb_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) -Iplugins $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/sandbox_main.c -o $(ARM_BUILD_DIR)/sb_main.o
	for s in $(VIRT_SB_SRCS); do \
	  o=$(ARM_BUILD_DIR)/sb_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_SB_ELF) \
	    $(ARM_BUILD_DIR)/sb_start.o $(ARM_BUILD_DIR)/sb_main.o \
	    $(ARM_BUILD_DIR)/sb_uart.o $(ARM_BUILD_DIR)/sb_vectors.o \
	    $(ARM_BUILD_DIR)/sb_entry.o $(ARM_BUILD_DIR)/sb_tramp.o \
	    $(ARM_BUILD_DIR)/sb_blob.o \
	    $(ARM_BUILD_DIR)/sb_pmm.o $(ARM_BUILD_DIR)/sb_mmu.o \
	    $(ARM_BUILD_DIR)/sb_vmem.o $(ARM_BUILD_DIR)/sb_process.o \
	    $(ARM_BUILD_DIR)/sb_exceptions.o $(ARM_BUILD_DIR)/sb_syscalls.o \
	    $(ARM_BUILD_DIR)/sb_elf64.o $(ARM_BUILD_DIR)/sb_plugin_loader.o \
	    $(ARM_BUILD_DIR)/sb_audio_ringbuf.o $(ARM_BUILD_DIR)/sb_audio_graph.o \
	    $(ARM_BUILD_DIR)/sb_graph_control.o $(ARM_BUILD_DIR)/sb_param_queue.o \
	    $(ARM_BUILD_DIR)/sb_plugin_mgr.o $(ARM_BUILD_DIR)/sb_patch.o $(ARM_BUILD_DIR)/sb_vfs.o \
	    $(ARM_BUILD_DIR)/sb_fat.o $(ARM_BUILD_DIR)/sb_sandbox.o \
	    $(ARM_BUILD_DIR)/sb_string.o
	rm -f $(ARM_BUILD_DIR)/virt_sandbox.log
	-timeout 25 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_sandbox.log -net none \
	    -kernel $(VIRT_SB_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_sandbox.log
	@grep -q "SANDBOX: PASS" $(ARM_BUILD_DIR)/virt_sandbox.log \
	  && echo "QEMU virt sandbox test PASSED" \
	  || { echo "QEMU virt sandbox test FAILED"; exit 1; }

# M8 capstone resilience demo on QEMU 'virt' (issue #36): load good/crash/evil
# plugins into isolated sandboxes, run the graph, kill the two hostile plugins
# mid-stream and confirm the good plugin + DAC keep producing audio, repeated
# 10x with no leak.  This is the milestone's "done when" demo.
VIRT_RES_ELF  = $(ARM_BUILD_DIR)/virt_resilience.elf
VIRT_RES_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
                $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
                $(ARCH_ARM_DIR)/elf64.c $(ARCH_ARM_DIR)/plugin_loader.c \
                $(ARCH_ARM_DIR)/audio_ringbuf.c $(ARCH_ARM_DIR)/audio_graph.c \
                $(ARCH_ARM_DIR)/graph_control.c $(ARCH_ARM_DIR)/param_queue.c \
                $(ARCH_ARM_DIR)/plugin_mgr.c $(ARCH_ARM_DIR)/patch.c $(ARCH_ARM_DIR)/vfs.c \
                $(ARCH_ARM_DIR)/fat.c $(ARCH_ARM_DIR)/sandbox.c \
                $(ARCH_ARM_DIR)/string.c

test-arm-resilience-qemu: $(ARM_BUILD_DIR)/plugin_good.elf \
                          $(ARM_BUILD_DIR)/plugin_crash.elf \
                          $(ARM_BUILD_DIR)/plugin_evil2.elf \
                          $(ARM_BUILD_DIR)/plugin_hog.elf
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/rs_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/rs_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/rs_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/rs_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/smp_entry.S       -o $(ARM_BUILD_DIR)/rs_smpentry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/resilience_blob.S      -o $(ARM_BUILD_DIR)/rs_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/rs_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) -Iplugins $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/resilience_main.c -o $(ARM_BUILD_DIR)/rs_main.o
	for s in $(VIRT_RES_SRCS) $(ARCH_ARM_DIR)/budget.c $(ARCH_ARM_DIR)/smp.c \
	         $(ARCH_ARM_DIR)/irq.c $(ARCH_ARM_DIR)/timer.c \
	         $(ARCH_ARM_DIR)/latency.c drivers/gic.c; do \
	  o=$(ARM_BUILD_DIR)/rs_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) $(VIRT_GIC_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_RES_ELF) \
	    $(ARM_BUILD_DIR)/rs_start.o $(ARM_BUILD_DIR)/rs_main.o \
	    $(ARM_BUILD_DIR)/rs_uart.o $(ARM_BUILD_DIR)/rs_vectors.o \
	    $(ARM_BUILD_DIR)/rs_entry.o $(ARM_BUILD_DIR)/rs_tramp.o \
	    $(ARM_BUILD_DIR)/rs_blob.o \
	    $(ARM_BUILD_DIR)/rs_pmm.o $(ARM_BUILD_DIR)/rs_mmu.o \
	    $(ARM_BUILD_DIR)/rs_vmem.o $(ARM_BUILD_DIR)/rs_process.o \
	    $(ARM_BUILD_DIR)/rs_exceptions.o $(ARM_BUILD_DIR)/rs_syscalls.o \
	    $(ARM_BUILD_DIR)/rs_elf64.o $(ARM_BUILD_DIR)/rs_plugin_loader.o \
	    $(ARM_BUILD_DIR)/rs_audio_ringbuf.o $(ARM_BUILD_DIR)/rs_audio_graph.o \
	    $(ARM_BUILD_DIR)/rs_graph_control.o $(ARM_BUILD_DIR)/rs_param_queue.o \
	    $(ARM_BUILD_DIR)/rs_plugin_mgr.o $(ARM_BUILD_DIR)/rs_patch.o $(ARM_BUILD_DIR)/rs_vfs.o \
	    $(ARM_BUILD_DIR)/rs_fat.o $(ARM_BUILD_DIR)/rs_sandbox.o \
	    $(ARM_BUILD_DIR)/rs_string.o \
	    $(ARM_BUILD_DIR)/rs_budget.o $(ARM_BUILD_DIR)/rs_smp.o \
	    $(ARM_BUILD_DIR)/rs_smpentry.o \
	    $(ARM_BUILD_DIR)/rs_irq.o $(ARM_BUILD_DIR)/rs_timer.o \
	    $(ARM_BUILD_DIR)/rs_latency.o $(ARM_BUILD_DIR)/rs_gic.o
	rm -f $(ARM_BUILD_DIR)/virt_resilience.log
	-timeout 30 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_resilience.log -net none \
	    -kernel $(VIRT_RES_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_resilience.log
	@grep -q "RESILIENCE: PASS" $(ARM_BUILD_DIR)/virt_resilience.log \
	  && echo "QEMU virt resilience demo PASSED" \
	  || { echo "QEMU virt resilience demo FAILED"; exit 1; }

# M11 capacity + resilience on QEMU 'virt' (issue #76): the milestone's "done
# when".  Phase 1 proves six calibrated-cost nodes (~1.5 blocks of work)
# overrun a single worker; phase 2 proves the same nodes spread across
# CPU1-3 run clean; then repeated load/distribute/kill/unload cycles run the
# M8 good+crash EL0 plugins per block on the CPU1 worker (secondaries join
# the kernel MMU via mmu_join) with the expensive nodes as background load:
# crash faults ON THE WORKER CORE and is killed with the M8 fault banner,
# good produces audio on every worker block, CPU0 never overruns, and the
# frame allocator returns to baseline.  MMU on, four cores.
VIRT_MC_ELF  = $(ARM_BUILD_DIR)/virt_multicore.elf
VIRT_MC_SRCS = $(VIRT_RES_SRCS) $(ARCH_ARM_DIR)/smp.c $(ARCH_ARM_DIR)/spsc_ring.c \
               $(ARCH_ARM_DIR)/audio_core.c $(ARCH_ARM_DIR)/audio_worker.c \
               $(ARCH_ARM_DIR)/irq.c $(ARCH_ARM_DIR)/timer.c drivers/gic.c

test-arm-multicore-qemu: $(ARM_BUILD_DIR)/plugin_good.elf \
                         $(ARM_BUILD_DIR)/plugin_crash.elf \
                         $(ARM_BUILD_DIR)/plugin_evil2.elf \
                         $(ARM_BUILD_DIR)/plugin_hog.elf
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/mc_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/smp_entry.S       -o $(ARM_BUILD_DIR)/mc_smpentry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/mc_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/mc_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/mc_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/resilience_blob.S      -o $(ARM_BUILD_DIR)/mc_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/mc_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) -Iplugins $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/multicore_main.c -o $(ARM_BUILD_DIR)/mc_main.o
	for s in $(VIRT_MC_SRCS); do \
	  o=$(ARM_BUILD_DIR)/mc_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) $(VIRT_GIC_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_MC_ELF) \
	    $(ARM_BUILD_DIR)/mc_start.o $(ARM_BUILD_DIR)/mc_main.o \
	    $(ARM_BUILD_DIR)/mc_uart.o $(ARM_BUILD_DIR)/mc_vectors.o \
	    $(ARM_BUILD_DIR)/mc_entry.o $(ARM_BUILD_DIR)/mc_tramp.o \
	    $(ARM_BUILD_DIR)/mc_blob.o $(ARM_BUILD_DIR)/mc_smpentry.o \
	    $(ARM_BUILD_DIR)/mc_pmm.o $(ARM_BUILD_DIR)/mc_mmu.o \
	    $(ARM_BUILD_DIR)/mc_vmem.o $(ARM_BUILD_DIR)/mc_process.o \
	    $(ARM_BUILD_DIR)/mc_exceptions.o $(ARM_BUILD_DIR)/mc_syscalls.o \
	    $(ARM_BUILD_DIR)/mc_elf64.o $(ARM_BUILD_DIR)/mc_plugin_loader.o \
	    $(ARM_BUILD_DIR)/mc_audio_ringbuf.o $(ARM_BUILD_DIR)/mc_audio_graph.o \
	    $(ARM_BUILD_DIR)/mc_graph_control.o $(ARM_BUILD_DIR)/mc_param_queue.o \
	    $(ARM_BUILD_DIR)/mc_plugin_mgr.o $(ARM_BUILD_DIR)/mc_patch.o $(ARM_BUILD_DIR)/mc_vfs.o \
	    $(ARM_BUILD_DIR)/mc_fat.o $(ARM_BUILD_DIR)/mc_sandbox.o \
	    $(ARM_BUILD_DIR)/mc_string.o \
	    $(ARM_BUILD_DIR)/mc_smp.o $(ARM_BUILD_DIR)/mc_spsc_ring.o \
	    $(ARM_BUILD_DIR)/mc_audio_core.o $(ARM_BUILD_DIR)/mc_audio_worker.o \
	    $(ARM_BUILD_DIR)/mc_irq.o $(ARM_BUILD_DIR)/mc_timer.o \
	    $(ARM_BUILD_DIR)/mc_gic.o
	rm -f $(ARM_BUILD_DIR)/virt_multicore.log
	-timeout 90 qemu-system-aarch64 -machine virt -cpu cortex-a72 -smp 4 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_multicore.log -net none \
	    -kernel $(VIRT_MC_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_multicore.log
	@grep -q "MULTICORE: PASS" $(ARM_BUILD_DIR)/virt_multicore.log \
	  && echo "QEMU virt multi-core capacity+resilience test PASSED" \
	  || { echo "QEMU virt multi-core capacity+resilience test FAILED"; exit 1; }

# CPU-budget enforcement on QEMU 'virt' (issue #78): three EL0 plugins run
# per block on the CPU1 worker, each under a budget - good never trips it,
# blip spins forever on two consecutive blocks, hog spins forever on every
# block.  The worker core's banked generic timer preempts a spinning plugin
# at its budget boundary (mid-block, verified by measured preempted run
# times); the policy mutes first and kills after 3 consecutive offences
# ([budget] banners); blip is forgiven and audibly returns; CPU0 never
# misses a callback; unloading everything (including the killed hog)
# returns the frame allocator to baseline.  MMU on, two cores.
VIRT_BUDGET_ELF  = $(ARM_BUILD_DIR)/virt_budget.elf
VIRT_BUDGET_SRCS = $(VIRT_RES_SRCS) $(ARCH_ARM_DIR)/budget.c \
                   $(ARCH_ARM_DIR)/smp.c $(ARCH_ARM_DIR)/spsc_ring.c \
                   $(ARCH_ARM_DIR)/audio_core.c $(ARCH_ARM_DIR)/audio_worker.c \
                   $(ARCH_ARM_DIR)/latency.c \
                   $(ARCH_ARM_DIR)/irq.c $(ARCH_ARM_DIR)/timer.c drivers/gic.c

test-arm-budget-qemu: $(ARM_BUILD_DIR)/plugin_good.elf \
                      $(ARM_BUILD_DIR)/plugin_blip.elf \
                      $(ARM_BUILD_DIR)/plugin_hog.elf
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/bg_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/smp_entry.S       -o $(ARM_BUILD_DIR)/bg_smpentry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/bg_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/bg_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/bg_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/budget_blob.S          -o $(ARM_BUILD_DIR)/bg_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/bg_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) -Iplugins $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/budget_main.c -o $(ARM_BUILD_DIR)/bg_main.o
	for s in $(VIRT_BUDGET_SRCS); do \
	  o=$(ARM_BUILD_DIR)/bg_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) $(VIRT_GIC_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_BUDGET_ELF) \
	    $(ARM_BUILD_DIR)/bg_start.o $(ARM_BUILD_DIR)/bg_main.o \
	    $(ARM_BUILD_DIR)/bg_uart.o $(ARM_BUILD_DIR)/bg_vectors.o \
	    $(ARM_BUILD_DIR)/bg_entry.o $(ARM_BUILD_DIR)/bg_tramp.o \
	    $(ARM_BUILD_DIR)/bg_blob.o $(ARM_BUILD_DIR)/bg_smpentry.o \
	    $(ARM_BUILD_DIR)/bg_pmm.o $(ARM_BUILD_DIR)/bg_mmu.o \
	    $(ARM_BUILD_DIR)/bg_vmem.o $(ARM_BUILD_DIR)/bg_process.o \
	    $(ARM_BUILD_DIR)/bg_exceptions.o $(ARM_BUILD_DIR)/bg_syscalls.o \
	    $(ARM_BUILD_DIR)/bg_elf64.o $(ARM_BUILD_DIR)/bg_plugin_loader.o \
	    $(ARM_BUILD_DIR)/bg_audio_ringbuf.o $(ARM_BUILD_DIR)/bg_audio_graph.o \
	    $(ARM_BUILD_DIR)/bg_graph_control.o $(ARM_BUILD_DIR)/bg_param_queue.o \
	    $(ARM_BUILD_DIR)/bg_plugin_mgr.o $(ARM_BUILD_DIR)/bg_patch.o $(ARM_BUILD_DIR)/bg_vfs.o \
	    $(ARM_BUILD_DIR)/bg_fat.o $(ARM_BUILD_DIR)/bg_sandbox.o \
	    $(ARM_BUILD_DIR)/bg_string.o $(ARM_BUILD_DIR)/bg_budget.o \
	    $(ARM_BUILD_DIR)/bg_smp.o $(ARM_BUILD_DIR)/bg_spsc_ring.o \
	    $(ARM_BUILD_DIR)/bg_audio_core.o $(ARM_BUILD_DIR)/bg_audio_worker.o \
	    $(ARM_BUILD_DIR)/bg_latency.o \
	    $(ARM_BUILD_DIR)/bg_irq.o $(ARM_BUILD_DIR)/bg_timer.o \
	    $(ARM_BUILD_DIR)/bg_gic.o
	rm -f $(ARM_BUILD_DIR)/virt_budget.log
	-timeout 40 qemu-system-aarch64 -machine virt -cpu cortex-a72 -smp 2 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_budget.log -net none \
	    -kernel $(VIRT_BUDGET_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_budget.log
	@grep -q "BUDGET: PASS" $(ARM_BUILD_DIR)/virt_budget.log \
	  && echo "QEMU virt budget-enforcement test PASSED" \
	  || { echo "QEMU virt budget-enforcement test FAILED"; exit 1; }

# Serial control shell on QEMU 'virt' (issue #80): the shell runs on CPU1
# reading a real PL011 RX FIFO fed over `-serial stdio`, while CPU0 services
# the audio callback and CPU2 emits periodic `audio_latency:` lines through
# the same UART lock.  A canned script (help / echo / stat / a backspace-
# edited line / an error / an unknown command / done) drives it; the harness
# verifies every path ran, the reporter shared the wire without corruption,
# and CPU0 saw zero overruns, then powers off via PSCI so stdio is flushed.
VIRT_SHELL_ELF  = $(ARM_BUILD_DIR)/virt_shell.elf
VIRT_SHELL_SRCS = $(ARCH_ARM_DIR)/smp.c $(ARCH_ARM_DIR)/spsc_ring.c \
                  $(ARCH_ARM_DIR)/audio_core.c $(ARCH_ARM_DIR)/shell.c \
                  $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/irq.c \
                  $(ARCH_ARM_DIR)/timer.c drivers/gic.c

test-arm-shell-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/sh_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/smp_entry.S -o $(ARM_BUILD_DIR)/sh_smpentry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S   -o $(ARM_BUILD_DIR)/sh_vectors.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/sh_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) $(ARM_CFLAGS) $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/shell_main.c -o $(ARM_BUILD_DIR)/sh_main.o
	for s in $(VIRT_SHELL_SRCS); do \
	  o=$(ARM_BUILD_DIR)/sh_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_GIC_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt.ld -o $(VIRT_SHELL_ELF) \
	    $(ARM_BUILD_DIR)/sh_start.o $(ARM_BUILD_DIR)/sh_smpentry.o \
	    $(ARM_BUILD_DIR)/sh_main.o $(ARM_BUILD_DIR)/sh_uart.o \
	    $(ARM_BUILD_DIR)/sh_vectors.o \
	    $(ARM_BUILD_DIR)/sh_smp.o $(ARM_BUILD_DIR)/sh_spsc_ring.o \
	    $(ARM_BUILD_DIR)/sh_audio_core.o $(ARM_BUILD_DIR)/sh_shell.o \
	    $(ARM_BUILD_DIR)/sh_exceptions.o \
	    $(ARM_BUILD_DIR)/sh_irq.o $(ARM_BUILD_DIR)/sh_timer.o \
	    $(ARM_BUILD_DIR)/sh_gic.o
	rm -f $(ARM_BUILD_DIR)/virt_shell.log
	printf 'help\recho hello world\rstat\recho hel\bllo\rbad\rfrobnicate\rdone\r' \
	  | timeout 30 qemu-system-aarch64 -machine virt -cpu cortex-a72 -smp 4 -m 256M \
	    -display none -serial stdio -monitor none -net none \
	    -kernel $(VIRT_SHELL_ELF) > $(ARM_BUILD_DIR)/virt_shell.log 2>&1 || true
	@cat $(ARM_BUILD_DIR)/virt_shell.log
	@grep -q "SHELL: PASS" $(ARM_BUILD_DIR)/virt_shell.log \
	  && grep -q "hello world" $(ARM_BUILD_DIR)/virt_shell.log \
	  && grep -q "unknown command: frobnicate" $(ARM_BUILD_DIR)/virt_shell.log \
	  && echo "QEMU virt serial-shell test PASSED" \
	  || { echo "QEMU virt serial-shell test FAILED"; exit 1; }

# Building an audio graph from the serial console on QEMU 'virt' (issue #81):
# MMU on, a single core reads a scripted console session over `-serial stdio`
# and drives the shell graph commands, which wrap the real plugin manager and
# graph control plane.  The script loads a sine generator and a gain stage,
# wires synth -> filter -> DAC, tweaks a param, then `run` executes audio
# blocks through the toposorted graph; the harness asserts the resulting
# graph (two plugin nodes, two edges) and that real audio came out, then
# powers off via PSCI so the log flushes.
VIRT_SHGRAPH_ELF  = $(ARM_BUILD_DIR)/virt_shell_graph.elf
VIRT_SHGRAPH_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                    $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
                    $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
                    $(ARCH_ARM_DIR)/elf64.c $(ARCH_ARM_DIR)/plugin_loader.c \
                    $(ARCH_ARM_DIR)/audio_ringbuf.c $(ARCH_ARM_DIR)/audio_graph.c \
                    $(ARCH_ARM_DIR)/graph_control.c $(ARCH_ARM_DIR)/param_queue.c \
                    $(ARCH_ARM_DIR)/plugin_mgr.c $(ARCH_ARM_DIR)/patch.c \
                    $(ARCH_ARM_DIR)/vfs.c $(ARCH_ARM_DIR)/fat.c \
                    $(ARCH_ARM_DIR)/sandbox.c $(ARCH_ARM_DIR)/string.c \
                    $(ARCH_ARM_DIR)/shell.c $(ARCH_ARM_DIR)/shell_graph.c

test-arm-shell-graph-qemu: $(ARM_BUILD_DIR)/plugin_good.elf \
                           $(ARM_BUILD_DIR)/plugin_effect_filter.elf
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/sg_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/sg_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/sg_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/sg_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/shell_graph_blob.S     -o $(ARM_BUILD_DIR)/sg_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/sg_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) -Iplugins $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/shell_graph_main.c -o $(ARM_BUILD_DIR)/sg_main.o
	for s in $(VIRT_SHGRAPH_SRCS); do \
	  o=$(ARM_BUILD_DIR)/sg_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) $(VIRT_GIC_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_SHGRAPH_ELF) \
	    $(ARM_BUILD_DIR)/sg_start.o $(ARM_BUILD_DIR)/sg_main.o \
	    $(ARM_BUILD_DIR)/sg_uart.o $(ARM_BUILD_DIR)/sg_vectors.o \
	    $(ARM_BUILD_DIR)/sg_entry.o $(ARM_BUILD_DIR)/sg_tramp.o \
	    $(ARM_BUILD_DIR)/sg_blob.o \
	    $(ARM_BUILD_DIR)/sg_pmm.o $(ARM_BUILD_DIR)/sg_mmu.o \
	    $(ARM_BUILD_DIR)/sg_vmem.o $(ARM_BUILD_DIR)/sg_process.o \
	    $(ARM_BUILD_DIR)/sg_exceptions.o $(ARM_BUILD_DIR)/sg_syscalls.o \
	    $(ARM_BUILD_DIR)/sg_elf64.o $(ARM_BUILD_DIR)/sg_plugin_loader.o \
	    $(ARM_BUILD_DIR)/sg_audio_ringbuf.o $(ARM_BUILD_DIR)/sg_audio_graph.o \
	    $(ARM_BUILD_DIR)/sg_graph_control.o $(ARM_BUILD_DIR)/sg_param_queue.o \
	    $(ARM_BUILD_DIR)/sg_plugin_mgr.o $(ARM_BUILD_DIR)/sg_patch.o \
	    $(ARM_BUILD_DIR)/sg_vfs.o $(ARM_BUILD_DIR)/sg_fat.o \
	    $(ARM_BUILD_DIR)/sg_sandbox.o $(ARM_BUILD_DIR)/sg_string.o \
	    $(ARM_BUILD_DIR)/sg_shell.o $(ARM_BUILD_DIR)/sg_shell_graph.o
	rm -f $(ARM_BUILD_DIR)/virt_shell_graph.log
	printf 'ls\rload /rd/synth\rload /rd/effect\rwire 1 2\rwire 2 dac\rset-param 1 0 660\rls\rrun 8\rstats\rdone\r' \
	  | timeout 30 qemu-system-aarch64 -machine virt -cpu cortex-a72 -smp 1 -m 256M \
	    -display none -serial stdio -monitor none -net none \
	    -kernel $(VIRT_SHGRAPH_ELF) > $(ARM_BUILD_DIR)/virt_shell_graph.log 2>&1 || true
	@cat $(ARM_BUILD_DIR)/virt_shell_graph.log
	@grep -q "SHELL-GRAPH: PASS" $(ARM_BUILD_DIR)/virt_shell_graph.log \
	  && grep -q "loaded pid 1" $(ARM_BUILD_DIR)/virt_shell_graph.log \
	  && grep -q "1 -> 2" $(ARM_BUILD_DIR)/virt_shell_graph.log \
	  && echo "QEMU virt shell-graph test PASSED" \
	  || { echo "QEMU virt shell-graph test FAILED"; exit 1; }

# Patch persistence from the serial console on QEMU 'virt' (issue #82): the M13
# "no C" acceptance.  MMU on, single core, reading a scripted console session
# over `-serial stdio`; the graph and patch commands drive the real plugin
# manager and a writable in-RAM FAT16 SD card.  The script builds a synth ->
# filter -> DAC chain, tunes the synth, saves the patch, "reboots" (tears down
# the graph while the SD card survives), lists the card, reloads the patch, and
# renders again - the reloaded audio hash must equal the pre-reboot one.  Powers
# off via PSCI so the log flushes.
VIRT_SHPATCH_ELF  = $(ARM_BUILD_DIR)/virt_shell_patch.elf
VIRT_SHPATCH_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                    $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
                    $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
                    $(ARCH_ARM_DIR)/elf64.c $(ARCH_ARM_DIR)/plugin_loader.c \
                    $(ARCH_ARM_DIR)/audio_ringbuf.c $(ARCH_ARM_DIR)/audio_graph.c \
                    $(ARCH_ARM_DIR)/graph_control.c $(ARCH_ARM_DIR)/param_queue.c \
                    $(ARCH_ARM_DIR)/plugin_mgr.c $(ARCH_ARM_DIR)/patch.c \
                    $(ARCH_ARM_DIR)/vfs.c $(ARCH_ARM_DIR)/fat.c \
                    $(ARCH_ARM_DIR)/sandbox.c $(ARCH_ARM_DIR)/string.c \
                    $(ARCH_ARM_DIR)/shell.c $(ARCH_ARM_DIR)/shell_graph.c

test-arm-shell-patch-qemu: sdk-example $(ARM_BUILD_DIR)/plugin_effect_filter.elf
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/sp_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/sp_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/sp_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/sp_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/shell_patch_blob.S     -o $(ARM_BUILD_DIR)/sp_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/sp_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) -Iplugins $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/shell_patch_main.c -o $(ARM_BUILD_DIR)/sp_main.o
	for s in $(VIRT_SHPATCH_SRCS); do \
	  o=$(ARM_BUILD_DIR)/sp_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) $(VIRT_GIC_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_SHPATCH_ELF) \
	    $(ARM_BUILD_DIR)/sp_start.o $(ARM_BUILD_DIR)/sp_main.o \
	    $(ARM_BUILD_DIR)/sp_uart.o $(ARM_BUILD_DIR)/sp_vectors.o \
	    $(ARM_BUILD_DIR)/sp_entry.o $(ARM_BUILD_DIR)/sp_tramp.o \
	    $(ARM_BUILD_DIR)/sp_blob.o \
	    $(ARM_BUILD_DIR)/sp_pmm.o $(ARM_BUILD_DIR)/sp_mmu.o \
	    $(ARM_BUILD_DIR)/sp_vmem.o $(ARM_BUILD_DIR)/sp_process.o \
	    $(ARM_BUILD_DIR)/sp_exceptions.o $(ARM_BUILD_DIR)/sp_syscalls.o \
	    $(ARM_BUILD_DIR)/sp_elf64.o $(ARM_BUILD_DIR)/sp_plugin_loader.o \
	    $(ARM_BUILD_DIR)/sp_audio_ringbuf.o $(ARM_BUILD_DIR)/sp_audio_graph.o \
	    $(ARM_BUILD_DIR)/sp_graph_control.o $(ARM_BUILD_DIR)/sp_param_queue.o \
	    $(ARM_BUILD_DIR)/sp_plugin_mgr.o $(ARM_BUILD_DIR)/sp_patch.o \
	    $(ARM_BUILD_DIR)/sp_vfs.o $(ARM_BUILD_DIR)/sp_fat.o \
	    $(ARM_BUILD_DIR)/sp_sandbox.o $(ARM_BUILD_DIR)/sp_string.o \
	    $(ARM_BUILD_DIR)/sp_shell.o $(ARM_BUILD_DIR)/sp_shell_graph.o
	rm -f $(ARM_BUILD_DIR)/virt_shell_patch.log
	printf 'load /rd/synth\rload /rd/effect\rwire 1 2\rwire 2 dac\rset-param 1 0 880\rrun\rpatch save /sd/live.patch\rreboot\rpatch ls\rpatch load /sd/live.patch\rrun\rpatch load /sd/missing.patch\rdone\r' \
	  | timeout 30 qemu-system-aarch64 -machine virt -cpu cortex-a72 -smp 1 -m 256M \
	    -display none -serial stdio -monitor none -net none \
	    -kernel $(VIRT_SHPATCH_ELF) > $(ARM_BUILD_DIR)/virt_shell_patch.log 2>&1 || true
	@cat $(ARM_BUILD_DIR)/virt_shell_patch.log
	@grep -q "SHELL-PATCH: PASS" $(ARM_BUILD_DIR)/virt_shell_patch.log \
	  && grep -q "saved /sd/live.patch" $(ARM_BUILD_DIR)/virt_shell_patch.log \
	  && grep -q "LIVE.PAT" $(ARM_BUILD_DIR)/virt_shell_patch.log \
	  && grep -q "error: patch load:" $(ARM_BUILD_DIR)/virt_shell_patch.log \
	  && echo "QEMU virt shell-patch test PASSED" \
	  || { echo "QEMU virt shell-patch test FAILED"; exit 1; }

# M9 SDK acceptance on QEMU 'virt' (issue #38): build the example plugin with
# ONLY the published SDK (its own Makefile, no kernel headers), then load that
# ELF and prove it audits clean, produces audio, and responds to a parameter
# change delivered through the host queue.
SDK_SINE_ELF = sdk/examples/sine_plugin/sine_plugin.elf
VIRT_SDK_ELF  = $(ARM_BUILD_DIR)/virt_sdk.elf
VIRT_SDK_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
                $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
                $(ARCH_ARM_DIR)/elf64.c $(ARCH_ARM_DIR)/plugin_loader.c \
                $(ARCH_ARM_DIR)/audio_ringbuf.c $(ARCH_ARM_DIR)/audio_graph.c \
                $(ARCH_ARM_DIR)/graph_control.c $(ARCH_ARM_DIR)/param_queue.c \
                $(ARCH_ARM_DIR)/plugin_mgr.c $(ARCH_ARM_DIR)/patch.c $(ARCH_ARM_DIR)/vfs.c \
                $(ARCH_ARM_DIR)/fat.c $(ARCH_ARM_DIR)/sandbox.c \
                $(ARCH_ARM_DIR)/string.c

# Build the example strictly through the SDK, exactly as a third party would.
.PHONY: sdk-example
sdk-example:
	$(MAKE) -C sdk/examples/sine_plugin CROSS_COMPILE=$(CROSS_COMPILE)

test-arm-sdk-qemu: sdk-example
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/sk_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/sk_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/sk_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/sk_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/sdk_blob.S            -o $(ARM_BUILD_DIR)/sk_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/sk_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) -Iplugins $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/sdk_main.c -o $(ARM_BUILD_DIR)/sk_main.o
	for s in $(VIRT_SDK_SRCS); do \
	  o=$(ARM_BUILD_DIR)/sk_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_SDK_ELF) \
	    $(ARM_BUILD_DIR)/sk_start.o $(ARM_BUILD_DIR)/sk_main.o \
	    $(ARM_BUILD_DIR)/sk_uart.o $(ARM_BUILD_DIR)/sk_vectors.o \
	    $(ARM_BUILD_DIR)/sk_entry.o $(ARM_BUILD_DIR)/sk_tramp.o \
	    $(ARM_BUILD_DIR)/sk_blob.o \
	    $(ARM_BUILD_DIR)/sk_pmm.o $(ARM_BUILD_DIR)/sk_mmu.o \
	    $(ARM_BUILD_DIR)/sk_vmem.o $(ARM_BUILD_DIR)/sk_process.o \
	    $(ARM_BUILD_DIR)/sk_exceptions.o $(ARM_BUILD_DIR)/sk_syscalls.o \
	    $(ARM_BUILD_DIR)/sk_elf64.o $(ARM_BUILD_DIR)/sk_plugin_loader.o \
	    $(ARM_BUILD_DIR)/sk_audio_ringbuf.o $(ARM_BUILD_DIR)/sk_audio_graph.o \
	    $(ARM_BUILD_DIR)/sk_graph_control.o $(ARM_BUILD_DIR)/sk_param_queue.o \
	    $(ARM_BUILD_DIR)/sk_plugin_mgr.o $(ARM_BUILD_DIR)/sk_patch.o $(ARM_BUILD_DIR)/sk_vfs.o \
	    $(ARM_BUILD_DIR)/sk_fat.o $(ARM_BUILD_DIR)/sk_sandbox.o \
	    $(ARM_BUILD_DIR)/sk_string.o
	rm -f $(ARM_BUILD_DIR)/virt_sdk.log
	-timeout 25 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_sdk.log -net none \
	    -kernel $(VIRT_SDK_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_sdk.log
	@grep -q "SDK: PASS" $(ARM_BUILD_DIR)/virt_sdk.log \
	  && echo "QEMU virt SDK plugin test PASSED" \
	  || { echo "QEMU virt SDK plugin test FAILED"; exit 1; }

# M9 patch persistence on QEMU 'virt' (issue #40): save a synth+effect graph to
# the FAT SD card, tear it down (reboot), reload, and confirm identical audio -
# plus a corrupt patch rejected without panic.
VIRT_PATCH_ELF  = $(ARM_BUILD_DIR)/virt_patch.elf
VIRT_PATCH_SRCS = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                  $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
                  $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
                  $(ARCH_ARM_DIR)/elf64.c $(ARCH_ARM_DIR)/plugin_loader.c \
                  $(ARCH_ARM_DIR)/audio_ringbuf.c $(ARCH_ARM_DIR)/audio_graph.c \
                  $(ARCH_ARM_DIR)/graph_control.c $(ARCH_ARM_DIR)/param_queue.c \
                  $(ARCH_ARM_DIR)/plugin_mgr.c $(ARCH_ARM_DIR)/patch.c \
                  $(ARCH_ARM_DIR)/vfs.c $(ARCH_ARM_DIR)/fat.c \
                  $(ARCH_ARM_DIR)/string.c

test-arm-patch-qemu: sdk-example $(ARM_BUILD_DIR)/plugin_effect.elf
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/pt_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S          -o $(ARM_BUILD_DIR)/pt_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S            -o $(ARM_BUILD_DIR)/pt_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/plugin_trampoline.S -o $(ARM_BUILD_DIR)/pt_tramp.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(VIRT_DIR)/patch_blob.S          -o $(ARM_BUILD_DIR)/pt_blob.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/pt_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) -Iplugins $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $(VIRT_DIR)/patch_main.c -o $(ARM_BUILD_DIR)/pt_main.o
	for s in $(VIRT_PATCH_SRCS); do \
	  o=$(ARM_BUILD_DIR)/pt_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_MMU_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_PATCH_ELF) \
	    $(ARM_BUILD_DIR)/pt_start.o $(ARM_BUILD_DIR)/pt_main.o \
	    $(ARM_BUILD_DIR)/pt_uart.o $(ARM_BUILD_DIR)/pt_vectors.o \
	    $(ARM_BUILD_DIR)/pt_entry.o $(ARM_BUILD_DIR)/pt_tramp.o \
	    $(ARM_BUILD_DIR)/pt_blob.o \
	    $(ARM_BUILD_DIR)/pt_pmm.o $(ARM_BUILD_DIR)/pt_mmu.o \
	    $(ARM_BUILD_DIR)/pt_vmem.o $(ARM_BUILD_DIR)/pt_process.o \
	    $(ARM_BUILD_DIR)/pt_exceptions.o $(ARM_BUILD_DIR)/pt_syscalls.o \
	    $(ARM_BUILD_DIR)/pt_elf64.o $(ARM_BUILD_DIR)/pt_plugin_loader.o \
	    $(ARM_BUILD_DIR)/pt_audio_ringbuf.o $(ARM_BUILD_DIR)/pt_audio_graph.o \
	    $(ARM_BUILD_DIR)/pt_graph_control.o $(ARM_BUILD_DIR)/pt_param_queue.o \
	    $(ARM_BUILD_DIR)/pt_plugin_mgr.o $(ARM_BUILD_DIR)/pt_patch.o \
	    $(ARM_BUILD_DIR)/pt_vfs.o $(ARM_BUILD_DIR)/pt_fat.o \
	    $(ARM_BUILD_DIR)/pt_string.o
	rm -f $(ARM_BUILD_DIR)/virt_patch.log
	-timeout 25 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_patch.log -net none \
	    -kernel $(VIRT_PATCH_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_patch.log
	@grep -q "PATCH: PASS" $(ARM_BUILD_DIR)/virt_patch.log \
	  && echo "QEMU virt patch-persistence test PASSED" \
	  || { echo "QEMU virt patch-persistence test FAILED"; exit 1; }

# Full-stack preemption test on QEMU 'virt' (issue #20): MMU + process stack
# AND the GICv2 + 1 kHz generic timer, running four EL0 busy loops that can
# only leave the CPU by being preempted.  Verifies priority preemption,
# round-robin interleaving, and idle-only-when-empty.  Uses the virt memory map
# and the virt GIC bases together.
VIRT_PREEMPT_FLAGS = $(VIRT_MMU_FLAGS) $(VIRT_GIC_FLAGS)
VIRT_PREEMPT_ELF   = $(ARM_BUILD_DIR)/virt_preempt.elf
VIRT_PREEMPT_SRCS  = $(ARCH_ARM_DIR)/pmm.c $(ARCH_ARM_DIR)/mmu.c \
                     $(ARCH_ARM_DIR)/vmem.c $(ARCH_ARM_DIR)/process.c \
                     $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/syscalls.c \
                     $(ARCH_ARM_DIR)/sched.c $(ARCH_ARM_DIR)/runqueue.c \
                     $(ARCH_ARM_DIR)/timer.c $(ARCH_ARM_DIR)/irq.c \
                     $(ARCH_ARM_DIR)/string.c drivers/gic.c

test-arm-preempt-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/p_start.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_PREEMPT_FLAGS) -c $(VIRT_DIR)/uart_virt.c   -o $(ARM_BUILD_DIR)/p_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) $(ARM_CFLAGS) $(VIRT_PREEMPT_FLAGS) -c $(VIRT_DIR)/preempt_main.c -o $(ARM_BUILD_DIR)/p_main.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S        -o $(ARM_BUILD_DIR)/p_vectors.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/entry.S          -o $(ARM_BUILD_DIR)/p_entry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/context_switch.S -o $(ARM_BUILD_DIR)/p_ctx.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/user_demo.S      -o $(ARM_BUILD_DIR)/p_userdemo.o
	for s in $(VIRT_PREEMPT_SRCS); do \
	  o=$(ARM_BUILD_DIR)/p_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_PREEMPT_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt_mmu.ld -o $(VIRT_PREEMPT_ELF) \
	    $(ARM_BUILD_DIR)/p_start.o $(ARM_BUILD_DIR)/p_main.o \
	    $(ARM_BUILD_DIR)/p_uart.o $(ARM_BUILD_DIR)/p_vectors.o \
	    $(ARM_BUILD_DIR)/p_entry.o $(ARM_BUILD_DIR)/p_ctx.o \
	    $(ARM_BUILD_DIR)/p_userdemo.o \
	    $(ARM_BUILD_DIR)/p_pmm.o $(ARM_BUILD_DIR)/p_mmu.o \
	    $(ARM_BUILD_DIR)/p_vmem.o $(ARM_BUILD_DIR)/p_process.o \
	    $(ARM_BUILD_DIR)/p_exceptions.o $(ARM_BUILD_DIR)/p_syscalls.o \
	    $(ARM_BUILD_DIR)/p_sched.o $(ARM_BUILD_DIR)/p_runqueue.o \
	    $(ARM_BUILD_DIR)/p_timer.o $(ARM_BUILD_DIR)/p_irq.o \
	    $(ARM_BUILD_DIR)/p_string.o $(ARM_BUILD_DIR)/p_gic.o
	rm -f $(ARM_BUILD_DIR)/virt_preempt.log
	-timeout 25 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_preempt.log -net none \
	    -kernel $(VIRT_PREEMPT_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_preempt.log
	@grep -q "PREEMPT: PASS" $(ARM_BUILD_DIR)/virt_preempt.log \
	  && echo "QEMU virt preemptive-scheduler test PASSED" \
	  || { echo "QEMU virt preemptive-scheduler test FAILED"; exit 1; }

# Dedicated audio-core SMP test on QEMU 'virt' (issue #21): boot all four cores
# via PSCI, pin the audio loop to CPU0 (1 kHz timer IRQ + lock-free ring +
# watchdog), run a producer on CPU1 and busy-load on CPU2/CPU3, and verify the
# audio cadence holds (no underrun, no overrun) under that load.  MMU off, virt
# GIC bases, four cores.
VIRT_SMP_ELF  = $(ARM_BUILD_DIR)/virt_smp.elf
VIRT_SMP_SRCS = $(ARCH_ARM_DIR)/smp.c $(ARCH_ARM_DIR)/spsc_ring.c \
                $(ARCH_ARM_DIR)/audio_core.c $(ARCH_ARM_DIR)/exceptions.c \
                $(ARCH_ARM_DIR)/irq.c $(ARCH_ARM_DIR)/timer.c drivers/gic.c

test-arm-smp-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/sm_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/smp_entry.S -o $(ARM_BUILD_DIR)/sm_smpentry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S   -o $(ARM_BUILD_DIR)/sm_vectors.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/sm_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) $(ARM_CFLAGS) $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/smp_main.c -o $(ARM_BUILD_DIR)/sm_main.o
	for s in $(VIRT_SMP_SRCS); do \
	  o=$(ARM_BUILD_DIR)/sm_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_GIC_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt.ld -o $(VIRT_SMP_ELF) \
	    $(ARM_BUILD_DIR)/sm_start.o $(ARM_BUILD_DIR)/sm_smpentry.o \
	    $(ARM_BUILD_DIR)/sm_main.o $(ARM_BUILD_DIR)/sm_uart.o \
	    $(ARM_BUILD_DIR)/sm_vectors.o \
	    $(ARM_BUILD_DIR)/sm_smp.o $(ARM_BUILD_DIR)/sm_spsc_ring.o \
	    $(ARM_BUILD_DIR)/sm_audio_core.o $(ARM_BUILD_DIR)/sm_exceptions.o \
	    $(ARM_BUILD_DIR)/sm_irq.o $(ARM_BUILD_DIR)/sm_timer.o \
	    $(ARM_BUILD_DIR)/sm_gic.o
	rm -f $(ARM_BUILD_DIR)/virt_smp.log
	-timeout 40 qemu-system-aarch64 -machine virt -cpu cortex-a72 -smp 4 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_smp.log -net none \
	    -kernel $(VIRT_SMP_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_smp.log
	@grep -q "AUDIO-CORE: PASS" $(ARM_BUILD_DIR)/virt_smp.log \
	  && echo "QEMU virt audio-core SMP test PASSED" \
	  || { echo "QEMU virt audio-core SMP test FAILED"; exit 1; }

# Per-core audio workers on QEMU 'virt' (issue #74): CPU0 kicks three pinned
# workers each 1 kHz block and refills the DAC ring without ever blocking on
# them; CPU1's worker node produces the audio, CPU2's worker runs two counter
# nodes, CPU3's worker node hangs at a trigger block.  Verifies zero CPU0
# overruns/underruns throughout, per-node overrun attribution on the stalled
# worker, and the drained invariant on all three.  MMU off, four cores.
VIRT_WORKER_ELF  = $(ARM_BUILD_DIR)/virt_worker.elf
VIRT_WORKER_SRCS = $(ARCH_ARM_DIR)/smp.c $(ARCH_ARM_DIR)/spsc_ring.c \
                   $(ARCH_ARM_DIR)/audio_core.c $(ARCH_ARM_DIR)/audio_worker.c \
                   $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/irq.c \
                   $(ARCH_ARM_DIR)/timer.c drivers/gic.c

test-arm-worker-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/wk_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/smp_entry.S -o $(ARM_BUILD_DIR)/wk_smpentry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S   -o $(ARM_BUILD_DIR)/wk_vectors.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/wk_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) $(ARM_CFLAGS) $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/worker_main.c -o $(ARM_BUILD_DIR)/wk_main.o
	for s in $(VIRT_WORKER_SRCS); do \
	  o=$(ARM_BUILD_DIR)/wk_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_GIC_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt.ld -o $(VIRT_WORKER_ELF) \
	    $(ARM_BUILD_DIR)/wk_start.o $(ARM_BUILD_DIR)/wk_smpentry.o \
	    $(ARM_BUILD_DIR)/wk_main.o $(ARM_BUILD_DIR)/wk_uart.o \
	    $(ARM_BUILD_DIR)/wk_vectors.o \
	    $(ARM_BUILD_DIR)/wk_smp.o $(ARM_BUILD_DIR)/wk_spsc_ring.o \
	    $(ARM_BUILD_DIR)/wk_audio_core.o $(ARM_BUILD_DIR)/wk_audio_worker.o \
	    $(ARM_BUILD_DIR)/wk_exceptions.o \
	    $(ARM_BUILD_DIR)/wk_irq.o $(ARM_BUILD_DIR)/wk_timer.o \
	    $(ARM_BUILD_DIR)/wk_gic.o
	rm -f $(ARM_BUILD_DIR)/virt_worker.log
	-timeout 40 qemu-system-aarch64 -machine virt -cpu cortex-a72 -smp 4 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_worker.log -net none \
	    -kernel $(VIRT_WORKER_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_worker.log
	@grep -q "WORKER: PASS" $(ARM_BUILD_DIR)/virt_worker.log \
	  && echo "QEMU virt audio-worker test PASSED" \
	  || { echo "QEMU virt audio-worker test FAILED"; exit 1; }

# Graph partitioning on QEMU 'virt' (issue #75): a live synth -> filter -> DAC
# chain driven through graph_control + graph_sched + the issue #74 workers is
# rewired at runtime and then split across two cores mid-run.  Verifies the
# DAC stream stays a consecutive function of the synth block counter across
# both transitions (bit-identical steady state modulo the documented pipeline
# latency), zero CPU0 overruns throughout, the never-scheduled third worker
# stays parked, and the stats line shows the split.  MMU off, four cores.
VIRT_GSCHED_ELF  = $(ARM_BUILD_DIR)/virt_gsched.elf
VIRT_GSCHED_SRCS = $(ARCH_ARM_DIR)/smp.c $(ARCH_ARM_DIR)/spsc_ring.c \
                   $(ARCH_ARM_DIR)/audio_core.c $(ARCH_ARM_DIR)/audio_worker.c \
                   $(ARCH_ARM_DIR)/audio_graph.c $(ARCH_ARM_DIR)/graph_control.c \
                   $(ARCH_ARM_DIR)/graph_sched.c $(ARCH_ARM_DIR)/string.c \
                   $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/irq.c \
                   $(ARCH_ARM_DIR)/timer.c drivers/gic.c

test-arm-gsched-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/gs_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/smp_entry.S -o $(ARM_BUILD_DIR)/gs_smpentry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S   -o $(ARM_BUILD_DIR)/gs_vectors.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/gs_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) $(ARM_CFLAGS) $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/gsched_main.c -o $(ARM_BUILD_DIR)/gs_main.o
	for s in $(VIRT_GSCHED_SRCS); do \
	  o=$(ARM_BUILD_DIR)/gs_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_GIC_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt.ld -o $(VIRT_GSCHED_ELF) \
	    $(ARM_BUILD_DIR)/gs_start.o $(ARM_BUILD_DIR)/gs_smpentry.o \
	    $(ARM_BUILD_DIR)/gs_main.o $(ARM_BUILD_DIR)/gs_uart.o \
	    $(ARM_BUILD_DIR)/gs_vectors.o \
	    $(ARM_BUILD_DIR)/gs_smp.o $(ARM_BUILD_DIR)/gs_spsc_ring.o \
	    $(ARM_BUILD_DIR)/gs_audio_core.o $(ARM_BUILD_DIR)/gs_audio_worker.o \
	    $(ARM_BUILD_DIR)/gs_audio_graph.o $(ARM_BUILD_DIR)/gs_graph_control.o \
	    $(ARM_BUILD_DIR)/gs_graph_sched.o $(ARM_BUILD_DIR)/gs_string.o \
	    $(ARM_BUILD_DIR)/gs_exceptions.o \
	    $(ARM_BUILD_DIR)/gs_irq.o $(ARM_BUILD_DIR)/gs_timer.o \
	    $(ARM_BUILD_DIR)/gs_gic.o
	rm -f $(ARM_BUILD_DIR)/virt_gsched.log
	-timeout 40 qemu-system-aarch64 -machine virt -cpu cortex-a72 -smp 4 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_gsched.log -net none \
	    -kernel $(VIRT_GSCHED_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_gsched.log
	@grep -q "GSCHED: PASS" $(ARM_BUILD_DIR)/virt_gsched.log \
	  && echo "QEMU virt graph-partitioning test PASSED" \
	  || { echo "QEMU virt graph-partitioning test FAILED"; exit 1; }

# Per-plugin time accounting on QEMU 'virt' (issue #77): two workers with
# clocks installed run tagged nodes of calibrated cost (light ~1/16 block,
# heavy ~1/3 block) plus the DAC producer; a reporter on CPU3 snapshots each
# worker's seqlocked stats board once per second and renders the
# `plugin_time:` lines over UART.  Verifies the reported means match the
# calibration, every snapshot is consistent, and the accounting/reporting
# perturbs nothing (zero CPU0 overruns).  MMU off, four cores.
VIRT_PTIME_ELF  = $(ARM_BUILD_DIR)/virt_ptime.elf
VIRT_PTIME_SRCS = $(ARCH_ARM_DIR)/smp.c $(ARCH_ARM_DIR)/spsc_ring.c \
                  $(ARCH_ARM_DIR)/audio_core.c $(ARCH_ARM_DIR)/audio_worker.c \
                  $(ARCH_ARM_DIR)/plugin_time.c $(ARCH_ARM_DIR)/latency.c \
                  $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/irq.c \
                  $(ARCH_ARM_DIR)/timer.c drivers/gic.c

test-arm-ptime-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/pt_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/smp_entry.S -o $(ARM_BUILD_DIR)/pt_smpentry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S   -o $(ARM_BUILD_DIR)/pt_vectors.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/pt_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) $(ARM_CFLAGS) $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/ptime_main.c -o $(ARM_BUILD_DIR)/pt_main.o
	for s in $(VIRT_PTIME_SRCS); do \
	  o=$(ARM_BUILD_DIR)/pt_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_GIC_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt.ld -o $(VIRT_PTIME_ELF) \
	    $(ARM_BUILD_DIR)/pt_start.o $(ARM_BUILD_DIR)/pt_smpentry.o \
	    $(ARM_BUILD_DIR)/pt_main.o $(ARM_BUILD_DIR)/pt_uart.o \
	    $(ARM_BUILD_DIR)/pt_vectors.o \
	    $(ARM_BUILD_DIR)/pt_smp.o $(ARM_BUILD_DIR)/pt_spsc_ring.o \
	    $(ARM_BUILD_DIR)/pt_audio_core.o $(ARM_BUILD_DIR)/pt_audio_worker.o \
	    $(ARM_BUILD_DIR)/pt_plugin_time.o $(ARM_BUILD_DIR)/pt_latency.o \
	    $(ARM_BUILD_DIR)/pt_exceptions.o \
	    $(ARM_BUILD_DIR)/pt_irq.o $(ARM_BUILD_DIR)/pt_timer.o \
	    $(ARM_BUILD_DIR)/pt_gic.o
	rm -f $(ARM_BUILD_DIR)/virt_ptime.log
	-timeout 40 qemu-system-aarch64 -machine virt -cpu cortex-a72 -smp 4 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_ptime.log -net none \
	    -kernel $(VIRT_PTIME_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_ptime.log
	@grep -q "PTIME: PASS" $(ARM_BUILD_DIR)/virt_ptime.log \
	  && echo "QEMU virt plugin-time test PASSED" \
	  || { echo "QEMU virt plugin-time test FAILED"; exit 1; }

# Audio latency/jitter reporting test on QEMU 'virt' (issue #22): CPU0 measures
# every callback (CNTPCT period + IRQ-to-thread wakeup) and publishes stats;
# CPU1 renders the `audio_latency:` line over UART; CPU2 produces samples.
# Proves the reporting does not corrupt the audio cadence (overruns stay zero).
VIRT_LAT_ELF  = $(ARM_BUILD_DIR)/virt_latency.elf
VIRT_LAT_SRCS = $(ARCH_ARM_DIR)/smp.c $(ARCH_ARM_DIR)/spsc_ring.c \
                $(ARCH_ARM_DIR)/audio_core.c $(ARCH_ARM_DIR)/latency.c \
                $(ARCH_ARM_DIR)/exceptions.c $(ARCH_ARM_DIR)/irq.c \
                $(ARCH_ARM_DIR)/timer.c drivers/gic.c

test-arm-latency-qemu: | $(ARM_BUILD_DIR)
	$(ARM_CC) $(ARM_ASFLAGS) -I$(ARCH_ARM_DIR) -c $(VIRT_DIR)/start_virt.S -o $(ARM_BUILD_DIR)/lt_start.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/smp_entry.S -o $(ARM_BUILD_DIR)/lt_smpentry.o
	$(ARM_CC) $(ARM_ASFLAGS) -c $(ARCH_ARM_DIR)/vectors.S   -o $(ARM_BUILD_DIR)/lt_vectors.o
	$(ARM_CC) $(ARM_CFLAGS)  $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/uart_virt.c -o $(ARM_BUILD_DIR)/lt_uart.o
	$(ARM_CC) -I$(ARCH_ARM_DIR) $(ARM_CFLAGS) $(VIRT_GIC_FLAGS) -c $(VIRT_DIR)/latency_main.c -o $(ARM_BUILD_DIR)/lt_main.o
	for s in $(VIRT_LAT_SRCS); do \
	  o=$(ARM_BUILD_DIR)/lt_$$(basename $${s%.c}).o; \
	  $(ARM_CC) $(ARM_CFLAGS) $(VIRT_GIC_FLAGS) -c $$s -o $$o || exit 1; \
	done
	$(ARM_LD) -T $(VIRT_DIR)/virt.ld -o $(VIRT_LAT_ELF) \
	    $(ARM_BUILD_DIR)/lt_start.o $(ARM_BUILD_DIR)/lt_smpentry.o \
	    $(ARM_BUILD_DIR)/lt_main.o $(ARM_BUILD_DIR)/lt_uart.o \
	    $(ARM_BUILD_DIR)/lt_vectors.o \
	    $(ARM_BUILD_DIR)/lt_smp.o $(ARM_BUILD_DIR)/lt_spsc_ring.o \
	    $(ARM_BUILD_DIR)/lt_audio_core.o $(ARM_BUILD_DIR)/lt_latency.o \
	    $(ARM_BUILD_DIR)/lt_exceptions.o $(ARM_BUILD_DIR)/lt_irq.o \
	    $(ARM_BUILD_DIR)/lt_timer.o $(ARM_BUILD_DIR)/lt_gic.o
	rm -f $(ARM_BUILD_DIR)/virt_latency.log
	-timeout 60 qemu-system-aarch64 -machine virt -cpu cortex-a72 -smp 4 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_latency.log -net none \
	    -kernel $(VIRT_LAT_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_latency.log
	@grep -q "AUDIO-LAT: PASS" $(ARM_BUILD_DIR)/virt_latency.log \
	  && grep -q "audio_latency:" $(ARM_BUILD_DIR)/virt_latency.log \
	  && echo "QEMU virt audio-latency test PASSED" \
	  || { echo "QEMU virt audio-latency test FAILED"; exit 1; }

arm-clean:
	rm -rf $(ARM_BUILD_DIR)

# Boot the ARM kernel image in QEMU (raspi4b) with serial output on stdio.
# For the CI smoke test, use: make qemu-arm | grep -m1 "Tessera ARM boot OK"
qemu-arm: $(ARM_KERNEL_IMG)
	qemu-system-aarch64 -machine raspi4b -nographic -serial stdio \
		-no-reboot -kernel $(ARM_KERNEL_IMG)

# Install the default (LLVM) ARM toolchain on Ubuntu/Debian.
arm-install-deps:
	sudo apt-get update
	sudo apt-get install -y clang lld llvm binutils

# Install the GCC cross-toolchain + QEMU (used in CI and by make qemu-arm).
arm-install-cross:
	sudo apt-get update
	sudo apt-get install -y gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
		qemu-system-arm

.PHONY: arm arm-clean qemu-arm test-arm-m1 test-arm-m2 test-arm-exc test-arm-exc-qemu test-arm-user-qemu test-arm-fault-qemu test-arm-sched-qemu test-arm-i2s test-arm-i2s-qemu test-arm-audio test-arm-audio-qemu test-arm-sine test-arm-sine-qemu arm-install-deps arm-install-cross

# ---- M9: patch/preset persistence (issue #40) -----------------------------
# Host unit tests for the patch text format (serialize/parse/corrupt).
ARM_PATCH_TEST_SRCS = tests/arm64/patch_test.c $(ARCH_ARM_DIR)/patch.c
test-arm-patch: | $(ARM_BUILD_DIR)
	$(CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined \
	      -I$(ARCH_ARM_DIR) $(ARM_PATCH_TEST_SRCS) -o $(ARM_BUILD_DIR)/patch_test
	$(ARM_BUILD_DIR)/patch_test
