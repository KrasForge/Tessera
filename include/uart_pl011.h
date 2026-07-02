/* include/uart_pl011.h — BCM2711 PL011 UART driver interface (Issue #3) */

#ifndef UART_PL011_H
#define UART_PL011_H

#include <stdint.h>
#include <stdarg.h>

/* Initialise UART0 (PL011) at 115200 8N1, polled mode. */
void uart_init(void);

/* Transmit a single character (blocks until TX FIFO has space). */
void uart_putc(char c);

/* Transmit a NUL-terminated string. */
void uart_puts(const char *s);

/* Receive one byte, blocking until one is available (polled RX FIFO). */
char uart_getc(void);

/* Non-blocking receive: returns the byte (0..255), or -1 if the RX FIFO is
 * empty.  Used by the shell's input loop. */
int uart_try_getc(void);

/* Minimal printf — supports %s %c %d %u %x %X %%. */
void uart_printf(const char *fmt, ...);

#endif /* UART_PL011_H */
