/* tests/arm64/virt/uart_virt.c — PL011 UART driver for the QEMU 'virt'
 * board (MMIO 0x09000000).  Provides the same uart_putc/puts/printf symbols
 * the real exception dispatcher links against, so the harness exercises the
 * genuine arch/arm64/exceptions.c + vectors.S on an emulator that exists in
 * every QEMU version (unlike raspi4b). */

#include <stdint.h>
#include <stdarg.h>

#define UART0      0x09000000UL
#define UARTDR     (*(volatile uint32_t *)(UART0 + 0x00))
#define UARTFR     (*(volatile uint32_t *)(UART0 + 0x18))
#define UARTCR     (*(volatile uint32_t *)(UART0 + 0x30))
#define FR_RXFE    (1u << 4)
#define FR_TXFF    (1u << 5)

/* Enable the PL011 (UARTEN | TXE | RXE) — QEMU's model needs UARTEN set. */
void uart_virt_init(void)
{
    UARTCR = (1u << 0) | (1u << 8) | (1u << 9);
}

void uart_putc(char c)
{
    while (UARTFR & FR_TXFF)
        ;
    UARTDR = (uint32_t)(unsigned char)c;
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

int uart_try_getc(void)
{
    if (UARTFR & FR_RXFE)
        return -1;
    return (int)(UARTDR & 0xFFu);
}

char uart_getc(void)
{
    while (UARTFR & FR_RXFE)
        ;
    return (char)(UARTDR & 0xFFu);
}

static void put_uint(uint64_t n, unsigned base, int upper)
{
    const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[20];
    int  len = 0;
    if (n == 0) { uart_putc('0'); return; }
    while (n) { buf[len++] = hex[n % base]; n /= base; }
    while (len--) uart_putc(buf[len]);
}

void uart_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { uart_putc(*fmt); continue; }
        switch (*++fmt) {
        case 's': uart_puts(va_arg(ap, const char *));              break;
        case 'c': uart_putc((char)va_arg(ap, int));                 break;
        case 'u': put_uint((uint64_t)va_arg(ap, unsigned), 10u, 0); break;
        case 'd': { int v = va_arg(ap, int);
                    if (v < 0) { uart_putc('-'); v = -v; }
                    put_uint((uint64_t)v, 10u, 0); }                break;
        case 'x': put_uint((uint64_t)va_arg(ap, unsigned), 16u, 0); break;
        case 'X': put_uint((uint64_t)va_arg(ap, unsigned), 16u, 1); break;
        case '%': uart_putc('%');                                    break;
        default:  uart_putc('%'); uart_putc(*fmt);                   break;
        }
    }
    va_end(ap);
}
