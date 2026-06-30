/* arch/arm64/string.c — freestanding mem* helpers for the ARM kernel.
 *
 * The AArch64 build links with -nostdlib, but the compiler may still emit
 * calls to memset/memcpy/memmove for aggregate initialisation and struct
 * copies.  These definitions satisfy those references.  The translation
 * unit is compiled with -fno-builtin (see the Makefile) so the loops below
 * are not "optimised" into self-recursive calls.
 */

#include <stddef.h>

void *memset(void *dst, int c, size_t n)
{
    unsigned char *p = dst;
    while (n--)
        *p++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}
