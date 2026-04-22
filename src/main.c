/*
 * main.c: the PageForge walkthrough.
 *
 * Runs through the four layers of memory management in order, which is the
 * same order the Linux MM subsystem stacks them:
 *
 *   Layer 1: Raw memory from OS  (my_mmap)
 *   Layer 2: Page allocator      (my_buddy)
 *   Layer 3: Object caches       (my_slab)
 *   Layer 4: General allocator   (my_kmalloc / my_kfree / my_calloc / my_realloc)
 *
 * Plus a software version of the RISC-V Sv39 page table walk (my_paging).
 *
 * No #include <stdio.h>, no #include <stdlib.h>.
 * The only OS calls are mmap(), write() and exit(), all through my_syscall.
 *
 * This file is a driver, not a layer. Nothing here is part of the allocator,
 * it just calls into each layer in turn and prints what happened, so you can
 * watch the thing work. Start with demo_paging() if you are here for the
 * page tables.
 */

#include "my_types.h"
#include "my_syscall.h"
#include "my_io.h"
#include "my_paging.h"
#include "my_buddy.h"
#include "my_slab.h"
#include "my_alloc.h"

static void section(const char *title)
{
    my_puts("\n============================================================");
    my_printf(" %s\n", title);
    my_puts("============================================================");
}

/* ------------------------------------------------------------------ */
/* Phase 0: get raw memory from the OS                                 */
/* ------------------------------------------------------------------ */
static void demo_raw_memory(void)
{
    section("Phase 0: Raw memory from OS  (my_mmap)");
    my_puts("  The OS hands us a blank region of pages. This is the");
    my_puts("  same idea as the kernel's boot memory map. Raw RAM.");

    void *p = my_mmap(PAGE_SIZE * 4);   /* ask for 4 pages = 16 KB */
    my_printf("  my_mmap(16 KB)  → %p\n", p);
    my_puts("  (zeroed anonymous pages, no file backing, no libc)");

    /* Write something into it to prove it is usable */
    uint8_t *bytes = (uint8_t *)p;
    bytes[0] = 0xCA;
    bytes[1] = 0xFE;
    my_printf("  bytes[0..1] = 0x%x 0x%x  (writable ✓)\n",
              bytes[0], bytes[1]);

    my_munmap(p, PAGE_SIZE * 4);
    my_printf("  my_munmap()  → pages returned to OS\n");
}

/* ------------------------------------------------------------------ */
/* Phase 1: paging simulation                                          */
/* ------------------------------------------------------------------ */
/*
 * Try one access and report what hardware would have done. A page can
 * translate perfectly and still fault, if the leaf does not carry the bit
 * this particular access needs.
 */
static void try_access(my_page_dir_t *pgd, uint64_t va,
                       my_access_t type, int mode, const char *what)
{
    my_access_result_t r = my_access(pgd, va, type, mode);

    if (r.fault == MY_FAULT_NONE) {
        my_printf("    %-30s allowed   -> PA 0x%lx\n", what, r.paddr);
    } else {
        unsigned scause = 0;
        const char *cause = my_fault_cause(type, &scause);
        my_printf("    %-30s FAULT     %s (scause %u, %s)\n",
                  what, my_fault_name(r.fault), scause, cause);
    }
}

static void demo_paging(void)
{
    section("Phase 1: Page Table Simulation  (my_paging, RISC-V Sv39)");
    my_puts("  A 3-level Sv39 page table, built in software: PGD -> PMD -> PTE.");
    my_puts("  On real rv64 hardware the MMU does this, with satp holding the root.");
    my_puts("  Every my_page_walk() below shows the steps it would take.\n");

    my_page_dir_t *pgd = my_pgd_create();

    /*
     * Map three virtual pages onto made-up "physical" frames. In a real
     * kernel those frames would come from the page allocator.
     *
     * Watch the permission bits: a leaf entry needs at least one of R, W
     * or X to count as a mapping at all, and U is what makes a page
     * reachable from user mode.
     */
    my_map_page(pgd, 0x00001000, 0x80100000, PTE_R | PTE_X);           /* code page  */
    my_map_page(pgd, 0x00002000, 0x80200000, PTE_R | PTE_W | PTE_U);   /* data page  */
    my_map_page(pgd, 0x40201000, 0x80300000, PTE_R | PTE_W);           /* stack page */

    my_puts("  Mapped:");
    my_puts("    VA 0x00001000  ->  PA 0x80100000  (kernel code, R-X)");
    my_puts("    VA 0x00002000  ->  PA 0x80200000  (user data,  RW-U)");
    my_puts("    VA 0x40201000  ->  PA 0x80300000  (stack, different PGD entry)");
    my_puts("    VA 0x00005000  ->  (not mapped, so expect a page fault)\n");

    my_page_walk(pgd, 0x00001ABC);  /* inside the code page      */
    my_page_walk(pgd, 0x00002080);  /* inside the data page      */
    my_page_walk(pgd, 0x40201004);  /* way up the address space  */
    my_page_walk(pgd, 0x00005000);  /* nothing here, so it faults */

    uint64_t pa = my_virt_to_phys(pgd, 0x00001ABC);
    my_printf("  my_virt_to_phys(0x00001ABC) = 0x%lx\n", pa);

    /*
     * Translating an address and being allowed to touch it are two
     * different questions. my_virt_to_phys() answers the first one.
     * my_access() answers the second, which is what hardware actually
     * enforces on every load, store and instruction fetch.
     */
    my_puts("\n  Permission checks (same pages, different access types):\n");
    try_access(pgd, 0x00001ABC, MY_ACCESS_READ,  MY_MODE_SUPERVISOR, "code page R-X, read");
    try_access(pgd, 0x00001ABC, MY_ACCESS_EXEC,  MY_MODE_SUPERVISOR, "code page R-X, execute");
    try_access(pgd, 0x00001ABC, MY_ACCESS_WRITE, MY_MODE_SUPERVISOR, "code page R-X, write");
    try_access(pgd, 0x00002080, MY_ACCESS_WRITE, MY_MODE_USER,       "data page RW-U, user write");
    try_access(pgd, 0x00002080, MY_ACCESS_EXEC,  MY_MODE_USER,       "data page RW-U, user exec");
    try_access(pgd, 0x40201004, MY_ACCESS_READ,  MY_MODE_USER,       "stack page RW, user read");
}

/* ------------------------------------------------------------------ */
/* Phase 2: buddy page allocator                                       */
/* ------------------------------------------------------------------ */
static void demo_buddy(void)
{
    section("Phase 2: Buddy Page Allocator  (my_buddy)");
    my_puts("  We get a 4 MB arena from the OS (one my_mmap call).");
    my_puts("  The buddy allocator manages it as 1024 x 4 KB pages.");
    my_puts("  Allocate in power-of-2 sizes; free coalesces buddies.\n");

    /* Get the arena from the OS */
    void *arena = my_mmap(ARENA_SIZE);
    if (!arena)
        my_panic("demo_buddy: mmap failed");

    my_buddy_init(arena, ARENA_SIZE);
    my_buddy_dump();

    my_puts("  [alloc] order 0 → 4 KB");
    void *a = my_alloc_pages(0);
    my_printf("          returned %p\n", a);

    my_puts("  [alloc] order 1 → 8 KB");
    void *b = my_alloc_pages(1);
    my_printf("          returned %p\n", b);

    my_puts("  [alloc] order 3 → 32 KB");
    void *c = my_alloc_pages(3);
    my_printf("          returned %p\n\n", c);

    my_buddy_dump();

    my_puts("  [free]  freeing all three blocks, watch them coalesce");
    my_free_pages(a, 0);
    my_free_pages(b, 1);
    my_free_pages(c, 3);

    my_buddy_dump();
}

/* ------------------------------------------------------------------ */
/* Phase 3: slab allocator                                             */
/* ------------------------------------------------------------------ */
static void demo_slab(void)
{
    section("Phase 3: Slab Allocator  (my_slab)");
    my_puts("  Slab takes pages from buddy and carves them into");
    my_puts("  fixed-size objects, the same way Linux SLUB does.\n");

    /* my_slab_init() was called inside my_alloc_init() in Phase 4 */

    my_puts("  Allocating objects of different sizes:");
    void *o8   = my_slab_alloc(8);
    void *o16  = my_slab_alloc(16);
    void *o64  = my_slab_alloc(64);
    void *o256 = my_slab_alloc(256);

    my_printf("  slab_alloc(8)    = %p\n", o8);
    my_printf("  slab_alloc(16)   = %p\n", o16);
    my_printf("  slab_alloc(64)   = %p\n", o64);
    my_printf("  slab_alloc(256)  = %p\n\n", o256);

    my_slab_dump();

    my_puts("  Freeing them back to caches:");
    my_slab_free(o8);
    my_slab_free(o16);
    my_slab_free(o64);
    my_slab_free(o256);

    my_slab_dump();
}

/* ------------------------------------------------------------------ */
/* Phase 4: general allocator (kmalloc / kfree / calloc / realloc)    */
/* ------------------------------------------------------------------ */
static void demo_alloc(void)
{
    section("Phase 4: General Allocator  (my_kmalloc / my_kfree / ...)");
    my_puts("  This is the public API, the same job kmalloc() does in Linux");
    my_puts("  or malloc() in user-space.\n");

    /* --- my_kmalloc --- */
    my_puts("  [my_kmalloc]");
    void *p8    = my_kmalloc(8);
    void *p100  = my_kmalloc(100);
    void *p512  = my_kmalloc(512);
    void *large = my_kmalloc(4096);   /* larger than threshold → buddy */

    my_printf("  kmalloc(8)     = %p  (slab)\n",  p8);
    my_printf("  kmalloc(100)   = %p  (slab)\n",  p100);
    my_printf("  kmalloc(512)   = %p  (slab)\n",  p512);
    my_printf("  kmalloc(4096)  = %p  (buddy page)\n\n", large);

    /* --- my_calloc --- */
    my_puts("  [my_calloc]");
    uint32_t *arr = (uint32_t *)my_calloc(8, sizeof(uint32_t));
    my_printf("  calloc(8, 4)   = %p\n", (void *)arr);
    my_printf("  arr[0..3]      = %u %u %u %u  (all zero ✓)\n",
              arr[0], arr[1], arr[2], arr[3]);

    arr[0] = 10; arr[1] = 20; arr[2] = 30; arr[3] = 40;
    my_printf("  arr[0..3] set  = %u %u %u %u\n\n",
              arr[0], arr[1], arr[2], arr[3]);

    /* --- my_realloc --- */
    my_puts("  [my_realloc]");
    arr = (uint32_t *)my_realloc(arr, 16 * sizeof(uint32_t));
    my_printf("  realloc → 16 ints  = %p\n", (void *)arr);
    my_printf("  data preserved     = %u %u %u %u\n\n",
              arr[0], arr[1], arr[2], arr[3]);

    /* --- my_kfree --- */
    my_puts("  [my_kfree]");
    my_kfree(p8);
    my_kfree(p100);
    my_kfree(p512);
    my_kfree(large);
    my_kfree(arr);
    my_puts("  all freed ✓\n");

    my_alloc_dump();
    my_slab_dump();
    my_buddy_dump();
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */
int main(void)
{
    my_puts("\n ██████╗  █████╗  ██████╗ ███████╗");
    my_puts(" ██╔══██╗██╔══██╗██╔════╝ ██╔════╝");
    my_puts(" ██████╔╝███████║██║  ███╗█████╗  ");
    my_puts(" ██╔═══╝ ██╔══██║██║   ██║██╔══╝  ");
    my_puts(" ██║     ██║  ██║╚██████╔╝███████╗");
    my_puts(" ╚═╝     ╚═╝  ╚═╝ ╚═════╝ ╚══════╝");
    my_puts("  F O R G E  |  Linux MM from scratch\n");
    my_puts("  No libc. No malloc. No printf.");
    my_puts("  Only mmap() + write() from the OS.\n");

    demo_raw_memory();
    demo_paging();
    demo_buddy();   /* phase 2, buddy gets its own arena                     */

    /*
     * Phases 3 and 4 share state (buddy allocator + slab caches).
     * Reinitialise buddy with a fresh arena so slab/alloc start clean.
     */
    {
        section("Initialising Buddy + Slab for Phases 3 & 4");
        void *arena = my_mmap(ARENA_SIZE);
        if (!arena) my_panic("main: mmap for arena failed");
        my_buddy_init(arena, ARENA_SIZE);
        my_alloc_init();   /* also calls my_slab_init() internally */
    }

    demo_slab();
    demo_alloc();

    section("Done");
    my_puts("  All layers demonstrated successfully.");
    my_puts("  This is the same mental model as Linux mm/:");
    my_puts("    page_alloc.c → slab.c / slub.c → kmalloc");
    my_puts("  ... just without the multi-CPU, NUMA, and swap complexity.\n");

    return 0;
}
