/* drivers/uart_pl011.c — BCM2711 PL011 UART driver (Issue #3)
 *
 * Target: Raspberry Pi CM4 / Pi 4 (BCM2711, Cortex-A72).
 *
 * UART0 (PL011) sits at MMIO 0xFE201000.  GPIO 14 (TXD0) and GPIO 15
 * (RXD0) are switched to ALT0 before the UART is brought up.
 *
 * Baud-rate divisors are computed for a 48 MHz UART clock, which is the
 * value set by the VideoCore firmware on real BCM2711 hardware and used by
 * the QEMU raspi4b model:
 *
 *   IBRD = floor(48_000_000 / (16 × 115_200)) = 26
 *   FBRD = round((48_000_000 / (16 × 115_200) − 26) × 64) = 3
 *
 * All I/O is polled — no interrupts are used at this stage.
 */

#include "uart_pl011.h"
#include "gpio.h"
#include <stdint.h>
#include <stdarg.h>

#define UART0_BASE  0xFE201000UL

#define UARTDR      (*(volatile uint32_t *)(UART0_BASE + 0x000u))
#define UARTFR      (*(volatile uint32_t *)(UART0_BASE + 0x018u))
#define UARTIBRD    (*(volatile uint32_t *)(UART0_BASE + 0x024u))
#define UARTFBRD    (*(volatile uint32_t *)(UART0_BASE + 0x028u))
#define UARTLCR_H   (*(volatile uint32_t *)(UART0_BASE + 0x02Cu))
#define UARTCR      (*(volatile uint32_t *)(UART0_BASE + 0x030u))
#define UARTIMSC    (*(volatile uint32_t *)(UART0_BASE + 0x038u))
#define UARTICR     (*(volatile uint32_t *)(UART0_BASE + 0x044u))

/* UARTFR flags */
#define FR_TXFF     (1u << 5)   /* TX FIFO full */
#define FR_BUSY     (1u << 3)   /* UART transmitting */

/* UARTCR bits */
#define CR_UARTEN   (1u << 0)
#define CR_TXE      (1u << 8)
#define CR_RXE      (1u << 9)

/* UARTLCR_H: 8-bit word length (bits 6:5 = 0b11) + FIFO enable (bit 4). */
#define LCR_WLEN8   (3u << 5)
#define LCR_FEN     (1u << 4)

/* 48 MHz UART clock, 115200 baud */
#define UART_IBRD   26u
#define UART_FBRD    3u

void uart_init(void)
{
    /* Disable the UART before touching any registers. */
    UARTCR = 0;

    /* Wait for any in-flight byte to finish. */
    while (UARTFR & FR_BUSY)
        ;

    /* Flush the transmit FIFO by clearing FEN while UART is disabled. */
    UARTLCR_H = 0;

    /* Mux GPIO 14 (TXD0) and GPIO 15 (RXD0) to PL011 via ALT0. */
    gpio_set_function(14u, GPIO_FUNC_ALT0);
    gpio_set_function(15u, GPIO_FUNC_ALT0);

    /* Write baud-rate divisors (must precede UARTLCR_H programming). */
    UARTIBRD = UART_IBRD;
    UARTFBRD = UART_FBRD;

    /* 8N1, FIFO enabled. */
    UARTLCR_H = LCR_WLEN8 | LCR_FEN;

    /* Polled mode: mask all interrupts. */
    UARTIMSC = 0;

    /* Clear any stale interrupt flags. */
    UARTICR = 0x7FFu;

    /* Enable UART with TX and RX. */
    UARTCR = CR_UARTEN | CR_TXE | CR_RXE;
}

void uart_putc(char c)
{
    /* Spin until there is space in the TX FIFO. */
    while (UARTFR & FR_TXFF)
        ;
    UARTDR = (uint32_t)(unsigned char)c;
}

void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

/* ---- Minimal printf support -------------------------------------------- */

static void put_uint(uint64_t n, unsigned base, int upper)
{
    const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char  buf[20];
    int   len = 0;

    if (n == 0) { uart_putc('0'); return; }
    while (n) {
        buf[len++] = hex[n % base];
        n /= base;
    }
    while (len--)
        uart_putc(buf[len]);
}

static void put_int(int64_t n)
{
    if (n < 0) { uart_putc('-'); put_uint((uint64_t)(-n), 10u, 0); }
    else        { put_uint((uint64_t)n, 10u, 0); }
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
        case 'd': put_int((int64_t)va_arg(ap, int));                break;
        case 'u': put_uint((uint64_t)va_arg(ap, unsigned), 10u, 0); break;
        case 'x': put_uint((uint64_t)va_arg(ap, unsigned), 16u, 0); break;
        case 'X': put_uint((uint64_t)va_arg(ap, unsigned), 16u, 1); break;
        case '%': uart_putc('%');                                    break;
        default:  uart_putc('%'); uart_putc(*fmt);                   break;
        }
    }

    va_end(ap);
}
