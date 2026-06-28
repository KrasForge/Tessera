/* IKOS I/O Port Access Functions
 *
 * Low-level I/O port access functions for IKOS kernel.
 * These functions provide access to x86 I/O ports for device drivers.
 *
 * Issue #6: this header is x86-only.  It must not be included by any
 * source file compiled for the AArch64 (ARM) target.
 */

#ifndef IO_H
#define IO_H

/* Guard: x86 I/O-port instructions do not exist on AArch64. */
#ifndef __aarch64__

#include <stdint.h>

/* 8-bit I/O port operations */
static inline uint8_t inb(uint16_t port) {
    uint8_t result;
    __asm__ volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* 16-bit I/O port operations */
static inline uint16_t inw(uint16_t port) {
    uint16_t result;
    __asm__ volatile("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

/* 32-bit I/O port operations */
static inline uint32_t inl(uint16_t port) {
    uint32_t result;
    __asm__ volatile("inl %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

/* I/O delay function */
static inline void io_delay(void) {
    __asm__ volatile("outb %al, $0x80");
}

/* String I/O operations */
static inline void insb(uint16_t port, void* buffer, uint32_t count) {
    __asm__ volatile("cld; rep insb" 
                     : "+D"(buffer), "+c"(count) 
                     : "d"(port) 
                     : "memory");
}

static inline void outsb(uint16_t port, const void* buffer, uint32_t count) {
    __asm__ volatile("cld; rep outsb" 
                     : "+S"(buffer), "+c"(count) 
                     : "d"(port));
}

static inline void insw(uint16_t port, void* buffer, uint32_t count) {
    __asm__ volatile("cld; rep insw" 
                     : "+D"(buffer), "+c"(count) 
                     : "d"(port) 
                     : "memory");
}

static inline void outsw(uint16_t port, const void* buffer, uint32_t count) {
    __asm__ volatile("cld; rep outsw" 
                     : "+S"(buffer), "+c"(count) 
                     : "d"(port));
}

static inline void insl(uint16_t port, void* buffer, uint32_t count) {
    __asm__ volatile("cld; rep insl" 
                     : "+D"(buffer), "+c"(count) 
                     : "d"(port) 
                     : "memory");
}

static inline void outsl(uint16_t port, const void* buffer, uint32_t count) {
    __asm__ volatile("cld; rep outsl" 
                     : "+S"(buffer), "+c"(count) 
                     : "d"(port));
}

#endif /* !__aarch64__ */
#endif /* IO_H */
