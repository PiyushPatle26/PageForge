#ifndef MY_SYSCALL_H
#define MY_SYSCALL_H

/*
 * my_syscall.h — the ONLY place we talk to the OS.
 *
 * Three syscalls are all we need:
 *   mmap()  — ask the OS for a raw chunk of memory (our "RAM")
 *   write() — print to stdout
 *   exit()  — terminate the process
 *
 * Everything else (malloc, printf, string ops) is built by us on top of these.
 */

#include "my_types.h"

/*
 * my_mmap  — ask the OS for 'size' bytes of zeroed, anonymous memory.
 *             This is how the kernel itself gets memory regions.
 *             Returns NULL on failure.
 */
void *my_mmap(size_t size);

/*
 * my_munmap — return memory to the OS.
 */
void  my_munmap(void *addr, size_t size);

/*
 * my_write — write 'len' bytes from 'buf' to file descriptor 'fd'.
 *             fd=1 → stdout.
 */
void  my_write(int fd, const void *buf, size_t len);

/*
 * my_exit — terminate the process with 'code'.
 */
void  my_exit(int code) __attribute__((noreturn));

#endif /* MY_SYSCALL_H */
