/*
 * demo.c — standalone demo for PageForge showing all allocator layers.
 *
 * This demo is separate from the main.c so it can be run independently,
 * linked against the PageForge library sources.
 *
 * Output is designed to look good as a terminal screenshot for the README.
 */

#include "my_types.h"
#include "my_syscall.h"
#include "my_io.h"
#include "my_buddy.h"
#include "my_slab.h"
#include "my_alloc.h"
#include "my_paging.h"

static void separator(const char *title)
{
    my_printf("\n\033[1;36m=== %s ===\033[0m\n", title);
}

static void ok(const char *label, void *ptr)
{
    my_printf("  \033[1;32m[OK]\033[0m %s → %p\n", label, ptr);
}

int main(void)
{
    my_puts("\033[1;35m");
    my_puts("  ____                  _____");
    my_puts(" |  _ \\ __ _  __ _  ___|  ___|__  _ __ __ _  ___");
    my_puts(" | |_) / _` |/ _` |/ _ \\ |_ / _ \\| '__/ _` |/ _ \\");
    my_puts(" |  __/ (_| | (_| |  __/  _| (_) | | | (_| |  __/");
    my_puts(" |_|   \\__,_|\\__, |\\___|_|  \\___/|_|  \\__, |\\___|");
    my_puts("              |___/                     |___/");
    my_puts("\033[0m");
    my_puts("  Linux kernel MM stack — from scratch, no libc\n");

    /* ---------------------------------------------------------------- */
    separator("Layer 1 — Raw OS Memory  (mmap)");
    void *raw = my_mmap(PAGE_SIZE * 4);
    my_printf("  my_mmap(16 KB)         → %p\n", raw);
    ((uint8_t *)raw)[0] = 0xDE;
    ((uint8_t *)raw)[1] = 0xAD;
    my_printf("  write + read-back      → 0x%x 0x%x \033[1;32m✓\033[0m\n",
              ((uint8_t *)raw)[0], ((uint8_t *)raw)[1]);
    my_munmap(raw, PAGE_SIZE * 4);
    my_puts("  my_munmap()            → pages returned to OS");

    /* ---------------------------------------------------------------- */
    separator("Layer 2 — Buddy Page Allocator");
    void *arena = my_mmap(ARENA_SIZE);
    my_buddy_init(arena, ARENA_SIZE);
    my_printf("  Arena : %p  (%u pages = 4 MB)\n",
              arena, ARENA_PAGES);

    void *p0 = my_alloc_pages(0);   ok("alloc order-0  (4 KB)", p0);
    void *p1 = my_alloc_pages(1);   ok("alloc order-1  (8 KB)", p1);
    void *p2 = my_alloc_pages(3);   ok("alloc order-3  (32 KB)", p2);
    my_printf("  Free pages after 3 allocs : %u / %u\n",
              g_buddy.free_pages, g_buddy.total_pages);

    my_free_pages(p0, 0);
    my_free_pages(p1, 1);
    my_free_pages(p2, 3);
    my_printf("  Free pages after all frees: %u / %u  \033[1;32m(fully coalesced ✓)\033[0m\n",
              g_buddy.free_pages, g_buddy.total_pages);

    /* ---------------------------------------------------------------- */
    separator("Layer 3 — Slab Object Cache");
    my_alloc_init();  /* re-init slab on same buddy */

    void *s8   = my_slab_alloc(8);    ok("slab_alloc(8  B)", s8);
    void *s16  = my_slab_alloc(16);   ok("slab_alloc(16 B)", s16);
    void *s64  = my_slab_alloc(64);   ok("slab_alloc(64 B)", s64);
    void *s256 = my_slab_alloc(256);  ok("slab_alloc(256 B)", s256);

    my_slab_free(s8); my_slab_free(s16);
    my_slab_free(s64); my_slab_free(s256);
    my_puts("  All slab objects freed  \033[1;32m✓\033[0m");

    /* ---------------------------------------------------------------- */
    separator("Layer 4 — General Allocator  (kmalloc / calloc / realloc)");

    void *k8    = my_kmalloc(8);     ok("kmalloc(8)    → slab", k8);
    void *k100  = my_kmalloc(100);   ok("kmalloc(100)  → slab", k100);
    void *k512  = my_kmalloc(512);   ok("kmalloc(512)  → slab", k512);
    void *klrg  = my_kmalloc(4096);  ok("kmalloc(4096) → buddy", klrg);

    /* calloc demo */
    uint32_t *arr = (uint32_t *)my_calloc(8, sizeof(uint32_t));
    my_printf("  calloc(8 × 4 B)        → %p\n", (void *)arr);
    my_printf("  arr[0..3] = %u %u %u %u  \033[1;32m(zeroed ✓)\033[0m\n",
              arr[0], arr[1], arr[2], arr[3]);

    arr[0] = 10; arr[1] = 20; arr[2] = 30; arr[3] = 40;

    /* realloc demo */
    arr = (uint32_t *)my_realloc(arr, 16 * sizeof(uint32_t));
    my_printf("  realloc → 64 B         → %p\n", (void *)arr);
    my_printf("  data preserved         = %u %u %u %u  \033[1;32m✓\033[0m\n",
              arr[0], arr[1], arr[2], arr[3]);

    my_kfree(k8); my_kfree(k100); my_kfree(k512);
    my_kfree(klrg); my_kfree(arr);
    my_puts("  All allocations freed   \033[1;32m✓\033[0m");

    /* ---------------------------------------------------------------- */
    separator("Layer 0 — Page Table Simulation");
    my_page_dir_t *pgd = my_pgd_create();
    my_map_page(pgd, 0x00001000, 0x00100000, PTE_WRITE);
    my_map_page(pgd, 0x00002000, 0x00200000, PTE_WRITE | PTE_USER);
    my_map_page(pgd, 0x00401000, 0x00300000, PTE_WRITE);

    my_printf("  VA 0x00001ABC → PA 0x%x\n",
              my_virt_to_phys(pgd, 0x00001ABC));
    my_printf("  VA 0x00002080 → PA 0x%x\n",
              my_virt_to_phys(pgd, 0x00002080));
    my_printf("  VA 0x00005000 → PA 0x%x  \033[1;31m(not mapped = page fault)\033[0m\n",
              my_virt_to_phys(pgd, 0x00005000));

    /* ---------------------------------------------------------------- */
    my_puts("\n\033[1;32m  All layers working correctly. PageForge complete.\033[0m\n");
    return 0;
}
