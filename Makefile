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
                $(ARCH_ARM_DIR)/string.c drivers/i2s.c drivers/gpio.c

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
	    $(ARM_BUILD_DIR)/i_gpio.o
	rm -f $(ARM_BUILD_DIR)/virt_i2s.log
	-timeout 20 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M \
	    -display none -serial file:$(ARM_BUILD_DIR)/virt_i2s.log -net none \
	    -kernel $(VIRT_I2S_ELF) >/dev/null 2>&1
	@cat $(ARM_BUILD_DIR)/virt_i2s.log
	@grep -q "I2S-SMOKE: PASS" $(ARM_BUILD_DIR)/virt_i2s.log \
	  && echo "QEMU virt I2S smoke test PASSED" \
	  || { echo "QEMU virt I2S smoke test FAILED"; exit 1; }

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
