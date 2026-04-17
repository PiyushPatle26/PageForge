#ifndef MY_SYSCALL_H
#define MY_SYSCALL_H

/*
 * my_syscall.h: the only place we talk to the OS.
 *
 * Three syscalls cover everything:
 *   mmap()   ask for a raw chunk of memory, which becomes our "RAM"
 *   write()  print to stdout
 *   exit()   stop the process
 *
 * Everything else (malloc, printf, string ops) is built by us on top of these.
 */

#include "my_types.h"

/*
 * my_mmap: ask for 'size' bytes of zeroed, anonymous memory. This is
 * roughly how the kernel gets its own memory regions. NULL on failure.
 */
void *my_mmap(size_t size);

/*
 * my_munmap: hand memory back to the OS.
 */
void  my_munmap(void *addr, size_t size);

/*
 * my_write: write 'len' bytes from 'buf' to fd. Use fd=1 for stdout.
 */
void  my_write(int fd, const void *buf, size_t len);

/*
 * my_exit: stop the process with the given exit code.
 */
void  my_exit(int code) __attribute__((noreturn));

#endif /* MY_SYSCALL_H */
