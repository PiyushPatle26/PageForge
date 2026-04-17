/*
 * my_syscall.c: the only file that talks to the Linux kernel.
 *
 * Three syscalls, that is the whole list:
 *   mmap()   get raw anonymous pages from the OS
 *   write()  put characters on stdout
 *   exit()   stop
 *
 * We include the minimal POSIX headers just for these syscall numbers and
 * flag constants. Nothing else from libc is used anywhere in the project.
 */

#include "my_syscall.h"

/* Pull in just enough for mmap/write/exit */
#include <sys/mman.h>   /* mmap, munmap, PROT_*, MAP_* flags */
#include <unistd.h>     /* write(), _exit() */

/*
 * my_mmap: ask the OS for 'size' bytes of anonymous, zeroed memory.
 *
 * The kernel's own page allocator gets its initial memory from the boot
 * memory map. We are in user space, so mmap() stands in for that same
 * "here is your RAM, do what you like with it" handoff.
 */
void *my_mmap(size_t size)
{
    void *ptr = mmap(
        NULL,                       /* let OS choose the address          */
        size,                       /* how many bytes we want             */
        PROT_READ | PROT_WRITE,     /* readable and writable              */
        MAP_PRIVATE | MAP_ANONYMOUS,/* not backed by a file, private copy */
        -1,                         /* no file descriptor                 */
        0                           /* no offset                          */
    );

    /* mmap returns MAP_FAILED (not NULL) on error */
    if (ptr == (void *)-1)
        return NULL;

    return ptr;
}

/*
 * my_munmap: give a region from my_mmap back to the OS.
 */
void my_munmap(void *addr, size_t size)
{
    munmap(addr, size);
}

/*
 * my_write: raw bytes to stdout. This is the only output primitive we
 * have, and my_printf is built on top of it.
 */
void my_write(int fd, const void *buf, size_t len)
{
    long _r = write(fd, buf, len);
    (void)_r;
}

/*
 * my_exit: stop the process.
 */
void my_exit(int code)
{
    _exit(code);
}
