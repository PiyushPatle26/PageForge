#ifndef MY_IO_H
#define MY_IO_H

/*
 * my_io.h: printing, built entirely on my_write().
 * No printf from libc. Just enough of one to be useful.
 */

#include "my_types.h"

/* Print a single character */
void my_putchar(char c);

/* Print a null-terminated string followed by newline */
void my_puts(const char *s);

/*
 * my_printf handles:
 *   %s   string
 *   %d   signed decimal
 *   %u   unsigned decimal
 *   %x   unsigned hex, no "0x" prefix added
 *   %p   pointer, printed as 0xADDRESS
 *   %c   character
 *   %%   a literal %
 *
 * Stick an 'l' in there (%lx, %lu, %ld) and the argument is read as a
 * long. Sv39 addresses need all 64 bits, so this matters.
 *
 * %s also takes a field width, so %-30s pads on the right to 30 columns
 * and %30s pads on the left. Handy for keeping output in columns.
 */
void my_printf(const char *fmt, ...);

/* Print error and halt the process */
void my_panic(const char *msg) __attribute__((noreturn));

#endif /* MY_IO_H */
