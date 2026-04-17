/*
 * my_syscall.c — the ONLY file that talks to the Linux kernel.
 *
 * We use three syscalls:
 *   mmap()   — get raw anonymous memory pages from the OS
 *   write()  — output characters to stdout
 *   exit()   — terminate
 *
 * We include the minimal POSIX headers just for these syscall numbers and
 * flag constants. Nothing else from libc is used anywhere in the project.
 */

#include "my_syscall.h"

/* Pull in just enough for mmap/write/exit */
#include <sys/mman.h>   /* mmap, munmap, PROT_*, MAP_* flags */
#include <unistd.h>     /* write(), _exit() */

/*
 * my_mmap — ask the OS for 'size' bytes of anonymous, zeroed memory.
 *
 * This is how the kernel's page allocator itself gets its initial memory
 * from the hardware (via the boot memory map). In user-space we use mmap()
 * to simulate that "here is raw RAM" handoff.
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
 * my_munmap — return a region obtained via my_mmap back to the OS.
 */
void my_munmap(void *addr, size_t size)
{
    munmap(addr, size);
}

/*
 * my_write — write raw bytes to stdout (fd = 1).
 * This is the only output primitive. my_printf is built on top of this.
 */
void my_write(int fd, const void *buf, size_t len)
{
    long _r = write(fd, buf, len);
    (void)_r;
}

/*
 * my_exit — terminate the process.
 */
void my_exit(int code)
{
    _exit(code);
}
