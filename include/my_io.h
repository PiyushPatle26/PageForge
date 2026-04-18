#ifndef MY_IO_H
#define MY_IO_H

/*
 * my_io.h — output functions built entirely on my_write().
 *            No printf from libc. We implement just enough to be useful.
 */

#include "my_types.h"

/* Print a single character */
void my_putchar(char c);

/* Print a null-terminated string followed by newline */
void my_puts(const char *s);

/*
 * my_printf — minimal printf supporting:
 *   %s  — string
 *   %d  — signed decimal integer
 *   %u  — unsigned decimal integer
 *   %x  — unsigned hex (no "0x" prefix added)
 *   %p  — pointer (prints as 0xADDRESS)
 *   %c  — character
 *   %%  — literal %
 */
void my_printf(const char *fmt, ...);

/* Print error and halt the process */
void my_panic(const char *msg) __attribute__((noreturn));

#endif /* MY_IO_H */
