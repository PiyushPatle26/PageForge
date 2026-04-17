/*
 * my_alloc.c — general-purpose allocator (my_kmalloc and friends).
 *
 * This is the top layer, equivalent to Linux's kmalloc() / kfree().
 *
 * Routing:
 *   size <= 1024  → my_slab_alloc()   (fast, object cache)
 *   size >  1024  → my_alloc_pages()  (direct page allocation)
 *
 * A small header is hidden just before every returned pointer.
 * This is the same trick glibc's malloc uses (chunk headers).
 */

#include "my_alloc.h"
#include "my_slab.h"
#include "my_buddy.h"
#include "my_io.h"

static uint32_t g_total_allocs = 0;
static uint32_t g_total_frees  = 0;

void my_alloc_init(void)
{
    my_slab_init();
    my_printf("[alloc] init: threshold=%u B  (slab below, pages above)\n",
              LARGE_THRESHOLD);
}

void *my_kmalloc(size_t size)
{
    if (size == 0) return NULL;

    my_alloc_header_t *hdr;

    if (size <= LARGE_THRESHOLD) {
        /*
         * Small allocation: route to slab.
         * We need space for the header + the user's data.
         */
        size_t total = size + sizeof(my_alloc_header_t);
        hdr = (my_alloc_header_t *)my_slab_alloc(total);
        if (!hdr) return NULL;

        hdr->is_large = 0;
        hdr->order    = 0;
    } else {
        /*
         * Large allocation: route directly to the buddy page allocator.
         * Calculate the smallest order that fits size + header.
         */
        size_t  total = size + sizeof(my_alloc_header_t);
        uint32_t order = 0;
        while ((PAGE_SIZE << order) < total)
            order++;

        hdr = (my_alloc_header_t *)my_alloc_pages(order);
        if (!hdr) return NULL;

        hdr->is_large = 1;
        hdr->order    = order;
    }

    hdr->magic = ALLOC_MAGIC;
    hdr->size  = size;

    g_total_allocs++;
    return (void *)(hdr + 1);   /* return pointer just past the header */
}

void my_kfree(void *ptr)
{
    if (!ptr) return;

    my_alloc_header_t *hdr = (my_alloc_header_t *)ptr - 1;

    if (hdr->magic != ALLOC_MAGIC)
        my_panic("my_kfree: bad magic — double free or corrupt pointer");

    hdr->magic = 0xDEADDEADu;   /* poison freed memory */

    if (hdr->is_large)
        my_free_pages((void *)hdr, hdr->order);
    else
        my_slab_free((void *)hdr);

    g_total_frees++;
}

void *my_calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void  *ptr   = my_kmalloc(total);
    if (!ptr) return NULL;

    /* Zero the memory — no memset from libc */
    uint8_t *p = (uint8_t *)ptr;
    for (size_t i = 0; i < total; i++)
        p[i] = 0;

    return ptr;
}

void *my_realloc(void *ptr, size_t new_size)
{
    if (!ptr)       return my_kmalloc(new_size);
    if (!new_size)  { my_kfree(ptr); return NULL; }

    my_alloc_header_t *hdr = (my_alloc_header_t *)ptr - 1;
    if (hdr->magic != ALLOC_MAGIC)
        my_panic("my_realloc: bad magic");

    size_t old_size = hdr->size;

    /*
     * Optimisation: if the new size still fits in the same slab class
     * (or the same page order for large allocs), just update the size
     * in place — no copy needed.
     */
    if (!hdr->is_large && new_size <= LARGE_THRESHOLD) {
        size_t old_needed = old_size + sizeof(my_alloc_header_t);
        size_t new_needed = new_size + sizeof(my_alloc_header_t);

        /* Find which size class each falls into */
        int old_class = -1, new_class = -1;
        for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
            if (old_class < 0 && MY_SIZE_CLASSES[i] >= old_needed) old_class = i;
            if (new_class < 0 && MY_SIZE_CLASSES[i] >= new_needed) new_class = i;
        }
        if (old_class == new_class) {
            hdr->size = new_size;
            return ptr;
        }
    }

    /* General case: allocate new, copy data, free old */
    void *new_ptr = my_kmalloc(new_size);
    if (!new_ptr) return NULL;

    size_t copy = (old_size < new_size) ? old_size : new_size;
    uint8_t *src = (uint8_t *)ptr;
    uint8_t *dst = (uint8_t *)new_ptr;
    for (size_t i = 0; i < copy; i++)
        dst[i] = src[i];

    my_kfree(ptr);
    return new_ptr;
}

void my_alloc_dump(void)
{
    my_printf("\n--- General Allocator ---\n");
    my_printf("  total allocs    : %u\n", g_total_allocs);
    my_printf("  total frees     : %u\n", g_total_frees);
    my_printf("  outstanding     : %u\n", g_total_allocs - g_total_frees);
    my_printf("-------------------------\n\n");
}
