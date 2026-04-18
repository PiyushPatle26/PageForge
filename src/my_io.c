/*
 * my_io.c — output functions built on my_write().
 * No stdio.h, no printf from libc.
 */

#include "my_io.h"
#include "my_syscall.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/* Write a single character to stdout */
void my_putchar(char c)
{
    my_write(1, &c, 1);
}

/* Print a string followed by newline */
void my_puts(const char *s)
{
    const char *p = s;
    while (*p) p++;
    my_write(1, s, (size_t)(p - s));
    my_putchar('\n');
}

/* Print an unsigned integer in the given base (2, 10, or 16) */
static void print_uint(unsigned long n, int base)
{
    static const char digits[] = "0123456789abcdef";
    char buf[32];
    int  i = 0;

    if (n == 0) {
        my_putchar('0');
        return;
    }
    while (n > 0) {
        buf[i++] = digits[n % (unsigned long)base];
        n /= (unsigned long)base;
    }
    /* buf is reversed — print it backwards */
    while (--i >= 0)
        my_putchar(buf[i]);
}

/* ------------------------------------------------------------------ */
/* my_printf                                                            */
/* Supports: %s %d %u %x %p %c %%                                      */
/* ------------------------------------------------------------------ */
void my_printf(const char *fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            my_putchar(*fmt);
            continue;
        }

        fmt++;  /* skip the '%' */

        switch (*fmt) {
        case 's': {
            const char *s = __builtin_va_arg(args, const char *);
            if (!s) s = "(null)";
            while (*s) my_putchar(*s++);
            break;
        }
        case 'd': {
            long n = __builtin_va_arg(args, int);
            if (n < 0) { my_putchar('-'); n = -n; }
            print_uint((unsigned long)n, 10);
            break;
        }
        case 'u': {
            unsigned long n = __builtin_va_arg(args, unsigned int);
            print_uint(n, 10);
            break;
        }
        case 'x': {
            unsigned long n = __builtin_va_arg(args, unsigned int);
            print_uint(n, 16);
            break;
        }
        case 'p': {
            unsigned long n = (unsigned long)__builtin_va_arg(args, void *);
            my_putchar('0'); my_putchar('x');
            print_uint(n, 16);
            break;
        }
        case 'c': {
            char c = (char)__builtin_va_arg(args, int);
            my_putchar(c);
            break;
        }
        case '%':
            my_putchar('%');
            break;
        default:
            my_putchar('%');
            my_putchar(*fmt);
            break;
        }
    }

    __builtin_va_end(args);
}

/* ------------------------------------------------------------------ */
/* my_panic — print message and halt                                    */
/* ------------------------------------------------------------------ */
void my_panic(const char *msg)
{
    my_printf("\n[PANIC] %s\n", msg);
    my_exit(1);
}
