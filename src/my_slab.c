/*
 * my_slab.c — slab allocator built on top of the buddy page allocator.
 *
 * Linux equivalent: mm/slab.c (SLAB) or mm/slub.c (SLUB).
 *
 * How it works:
 *   Each size class (8, 16, 32 … 1024 bytes) has a "cache".
 *   A cache holds multiple "slabs".  A slab is exactly one 4 KB page.
 *   Within a slab, objects are stored back-to-back, and a free-list
 *   links all the unallocated ones using the objects' own memory
 *   (an embedded linked list — no extra bookkeeping memory needed).
 *
 *   Slab states:
 *     partial — has both used and free objects (allocate from here first)
 *     full    — no free objects left
 *     empty   — all objects are free (can be returned to buddy)
 *
 *   Finding which slab an object belongs to:
 *     Every slab header lives at the start of its page.
 *     So: slab = ptr & ~0xFFF  (mask off the lower 12 bits)
 */

#include "my_slab.h"
#include "my_buddy.h"
#include "my_io.h"

my_kmem_cache_t g_slab_caches[NUM_SIZE_CLASSES];

/* ------------------------------------------------------------------ */
/* Internal: allocate a fresh slab from the buddy allocator            */
/* ------------------------------------------------------------------ */

static my_slab_t *slab_new(my_kmem_cache_t *cache)
{
    /* Ask buddy for one page */
    void *page = my_alloc_pages(0);
    if (!page)
        return NULL;

    /*
     * Place the slab header at the very start of the page.
     * Objects start right after the header, aligned to obj_size.
     */
    my_slab_t *slab = (my_slab_t *)page;
    slab->magic    = SLAB_MAGIC;
    slab->obj_size = (uint32_t)cache->obj_size;
    slab->next     = NULL;

    uintptr_t obj_start = (uintptr_t)page + sizeof(my_slab_t);
    /* Round up to the object's alignment (obj_size is always a power of 2) */
    uintptr_t align = cache->obj_size;
    if (obj_start % align)
        obj_start += align - (obj_start % align);

    slab->num_total = (uint32_t)((PAGE_SIZE - (obj_start - (uintptr_t)page))
                                  / cache->obj_size);
    slab->num_free  = slab->num_total;

    /*
     * Build the embedded free-list.
     * Each free object stores a void* to the next free object at offset 0.
     * We build it in reverse so the first allocation returns the
     * lowest-address object.
     */
    slab->free_list = NULL;
    for (int i = (int)slab->num_total - 1; i >= 0; i--) {
        void *obj  = (void *)(obj_start + (uintptr_t)i * cache->obj_size);
        *(void **)obj  = slab->free_list;
        slab->free_list = obj;
    }

    return slab;
}

/* ------------------------------------------------------------------ */
/* Internal: move a slab between the partial/full/empty lists          */
/* ------------------------------------------------------------------ */

static void slab_list_remove(my_slab_t **head, my_slab_t *target)
{
    my_slab_t **cur = head;
    while (*cur) {
        if (*cur == target) { *cur = target->next; return; }
        cur = &(*cur)->next;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void my_slab_init(void)
{
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        g_slab_caches[i].obj_size     = MY_SIZE_CLASSES[i];
        g_slab_caches[i].partial      = NULL;
        g_slab_caches[i].full         = NULL;
        g_slab_caches[i].empty        = NULL;
        g_slab_caches[i].total_allocs = 0;
        g_slab_caches[i].total_frees  = 0;
    }
    my_printf("[slab]  init: %d size classes (8 .. 1024 bytes)\n",
              NUM_SIZE_CLASSES);
}

void *my_slab_alloc(size_t size)
{
    /* Find the smallest size class that fits */
    my_kmem_cache_t *cache = NULL;
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        if (MY_SIZE_CLASSES[i] >= size) {
            cache = &g_slab_caches[i];
            break;
        }
    }
    if (!cache) return NULL;

    /* Get a slab with free space */
    my_slab_t *slab = cache->partial;
    if (!slab) {
        /* Try recycling an empty slab first */
        if (cache->empty) {
            slab = cache->empty;
            cache->empty = slab->next;
        } else {
            slab = slab_new(cache);
            if (!slab) return NULL;
        }
        slab->next    = cache->partial;
        cache->partial = slab;
    }

    /* Pop one object from the slab's embedded free-list */
    void *obj       = slab->free_list;
    slab->free_list = *(void **)obj;
    slab->num_free--;

    /* If slab is now full, move it to the full list */
    if (slab->num_free == 0) {
        cache->partial = slab->next;
        slab->next     = cache->full;
        cache->full    = slab;
    }

    cache->total_allocs++;
    return obj;
}

void my_slab_free(void *ptr)
{
    if (!ptr) return;

    /*
     * Find the slab header: it is always at the page-aligned base
     * of the page containing ptr.
     */
    uintptr_t  page_base = (uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1);
    my_slab_t *slab      = (my_slab_t *)page_base;

    if (slab->magic != SLAB_MAGIC)
        my_panic("my_slab_free: bad magic — double free or corrupt pointer");

    /* Find the cache for this object size */
    my_kmem_cache_t *cache = NULL;
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        if (g_slab_caches[i].obj_size == slab->obj_size) {
            cache = &g_slab_caches[i];
            break;
        }
    }
    if (!cache)
        my_panic("my_slab_free: no matching cache found");

    int was_full = (slab->num_free == 0);

    /* Push object back onto the slab's free-list */
    *(void **)ptr   = slab->free_list;
    slab->free_list = ptr;
    slab->num_free++;

    /* Handle list transitions */
    if (was_full) {
        /* full → partial */
        slab_list_remove(&cache->full, slab);
        slab->next    = cache->partial;
        cache->partial = slab;
    } else if (slab->num_free == slab->num_total) {
        /* partial → empty */
        slab_list_remove(&cache->partial, slab);
        slab->next   = cache->empty;
        cache->empty = slab;
    }

    cache->total_frees++;
}

void my_slab_dump(void)
{
    my_printf("\n--- Slab Allocator ---\n");
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        my_kmem_cache_t *c = &g_slab_caches[i];
        uint32_t np = 0, nf = 0, ne = 0;
        my_slab_t *s;
        for (s = c->partial; s; s = s->next) np++;
        for (s = c->full;    s; s = s->next) nf++;
        for (s = c->empty;   s; s = s->next) ne++;

        my_printf("  [%u B]  allocs=%u frees=%u  slabs: partial=%u full=%u empty=%u\n",
                  (uint32_t)c->obj_size,
                  c->total_allocs, c->total_frees,
                  np, nf, ne);
    }
    my_printf("----------------------\n\n");
}
