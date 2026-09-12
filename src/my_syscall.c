/*
 * my_syscall.c: the only file that talks to the Linux kernel.
 *
 * Three syscalls, that is the whole list:
 *   mmap()   get raw anonymous pages from the OS
 *   write()  put characters on stdout
 *   exit()   stop
 *
 * These are issued directly with the RISC-V ecall instruction. No libc
 * headers, no libc wrappers. The standalone binaries link with -nostdlib,
 * so _start lives here too.
 *
 * Syscall numbers are the generic asm-generic/unistd.h numbering that
 * RISC-V uses.
 */

#include "my_syscall.h"

#if !defined(__riscv) || __riscv_xlen != 64
  #error "PageForge targets riscv64 (rv64gc) only"
#endif

/* ------------------------------------------------------------------ */
/* Syscall numbers and mmap flags, as the kernel defines them           */
/* ------------------------------------------------------------------ */

#define SYS_write    64
#define SYS_exit     93
#define SYS_munmap  215
#define SYS_mmap    222

#define MY_PROT_READ      0x1
#define MY_PROT_WRITE     0x2
#define MY_MAP_PRIVATE    0x02
#define MY_MAP_ANONYMOUS  0x20

/* ------------------------------------------------------------------ */
/* The ecall itself                                                     */
/* ------------------------------------------------------------------ */

/*
 * Arguments go in a0..a5, the syscall number in a7, and the result comes
 * back in a0. The "memory" clobber tells the compiler the kernel may touch
 * memory it cannot see.
 */
static inline long my_syscall6(long n, long a, long b, long c,
                               long d, long e, long f)
{
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a4 __asm__("a4") = e;
    register long a5 __asm__("a5") = f;

    __asm__ volatile ("ecall"
                      : "+r"(a0)
                      : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5)
                      : "memory");
    return a0;
}

/* ------------------------------------------------------------------ */
/* The three calls                                                      */
/* ------------------------------------------------------------------ */

/*
 * my_mmap: ask the OS for 'size' bytes of anonymous, zeroed memory.
 *
 * The kernel's own page allocator gets its initial memory from the boot
 * memory map. We are in user space, so mmap() stands in for that same
 * "here is your RAM, do what you like with it" handoff.
 */
void *my_mmap(size_t size)
{
    long ret = my_syscall6(SYS_mmap,
                           0,                                    /* addr   */
                           (long)size,                           /* length */
                           MY_PROT_READ | MY_PROT_WRITE,         /* prot   */
                           MY_MAP_PRIVATE | MY_MAP_ANONYMOUS,    /* flags  */
                           -1,                                   /* fd     */
                           0);                                   /* offset */

    /* A raw syscall returns -errno on failure, not MAP_FAILED */
    if (ret < 0 && ret > -4096)
        return NULL;

    return (void *)ret;
}

/*
 * my_munmap: give a region from my_mmap back to the OS.
 */
void my_munmap(void *addr, size_t size)
{
    my_syscall6(SYS_munmap, (long)addr, (long)size, 0, 0, 0, 0);
}

/*
 * my_write: raw bytes to stdout. This is the only output primitive we
 * have, and my_printf is built on top of it.
 */
void my_write(int fd, const void *buf, size_t len)
{
    my_syscall6(SYS_write, (long)fd, (long)buf, (long)len, 0, 0, 0);
}

/*
 * my_exit: stop the process.
 */
void my_exit(int code)
{
    my_syscall6(SYS_exit, (long)code, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

/* ------------------------------------------------------------------ */
/* Freestanding support                                                 */
/* ------------------------------------------------------------------ */

/*
 * With -nostdlib there is no libc startup code and no libc at all, so the
 * few routines GCC is allowed to emit calls to have to come from us.
 *
 * Only the standalone binaries define PAGEFORGE_FREESTANDING. The Unity
 * test build links against libc and brings its own main and startup.
 */
#ifdef PAGEFORGE_FREESTANDING

void *memset(void *dst, int c, size_t n)
{
    unsigned char *p = dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

/*
 * The kernel jumps straight here with the stack pointing at argc. We do
 * not need argv, so _start just calls main and turns its return value
 * into exit().
 *
 * gp has to be set up first. RISC-V addresses globals that sit near
 * __global_pointer$ as an offset from gp, which is a link-time relaxation
 * the compiler applies on its own. libc's startup code normally loads gp;
 * with -nostdlib nobody does, so every such access lands at a wild address
 * and the first global write segfaults. The relaxation has to be switched
 * off around the load itself, since it would otherwise be rewritten to be
 * relative to the gp it is in the middle of establishing.
 */
__asm__(
".text\n"
".global _start\n"
"_start:\n"
".option push\n"
".option norelax\n"
"  la gp, __global_pointer$\n"
".option pop\n"
"  call main\n"
"  call my_exit\n"
);

#endif /* PAGEFORGE_FREESTANDING */
