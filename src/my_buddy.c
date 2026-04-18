/*
 * my_buddy.c: the buddy system page allocator.
 *
 * This is the lowest layer of memory management, exactly mirroring
 * the Linux kernel's zone page allocator (mm/page_alloc.c).
 *
 * Linux calls the largest order MAX_PAGE_ORDER, which is 10, the same as
 * ours. Note the name: it was plain MAX_ORDER until 6.5, when the meaning
 * of the constant changed and it was renamed to avoid silent breakage.
 *
 * Key concepts implemented:
 *   1. Free lists, one linked list of free blocks per order
 *   2. Splitting, when no order N block is free, cut an order N+1 block in two
 *   3. Coalescing, when freeing, merge back with the buddy if it is free too
 *   4. Bitmap, one bit per page saying whether it is allocated
 *
 * buddy formula: given a block starting at page index P with order N,
 *   its buddy's page index = P XOR (1 << N)
 *
 * Where this sits: layer 2. It takes one big chunk from my_syscall.c and
 * hands out pages to my_slab.c and my_alloc.c above it.
 *
 * If you read one function here, read my_free_pages(). The merging loop at
 * the bottom of it is the whole buddy idea in about ten lines.
 */

#include "my_buddy.h"
#include "my_io.h"

/* The one global buddy allocator. Linux keeps this state per zone, in
 * struct zone's free_area[NR_PAGE_ORDERS] (include/linux/mmzone.h). */
my_buddy_t g_buddy;

/* ------------------------------------------------------------------ */
/* Bitmap helpers                                                       */
/* ------------------------------------------------------------------ */

static void bitmap_set(uint32_t page_idx)
{
    g_buddy.bitmap[page_idx / 8] |= (uint8_t)(1u << (page_idx % 8));
}

static void bitmap_clear(uint32_t page_idx)
{
    g_buddy.bitmap[page_idx / 8] &= (uint8_t)~(1u << (page_idx % 8));
}

static int bitmap_test(uint32_t page_idx)
{
    return (g_buddy.bitmap[page_idx / 8] >> (page_idx % 8)) & 1;
}

/* ------------------------------------------------------------------ */
/* Address ↔ page index conversions                                    */
/* ------------------------------------------------------------------ */

static uint32_t addr_to_page(void *addr)
{
    return (uint32_t)(((uintptr_t)addr - (uintptr_t)g_buddy.base) / PAGE_SIZE);
}

static void *page_to_addr(uint32_t page_idx)
{
    return (void *)((uintptr_t)g_buddy.base + (uintptr_t)page_idx * PAGE_SIZE);
}

/* ------------------------------------------------------------------ */
/* Free list helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * Take a block off the free list at 'order'.
 *
 * Returns 1 if the block was there and has been removed, 0 if it was not
 * on the list at all. That return value is also how we ask "is this block
 * free?", which saves walking the list twice to ask and then remove.
 */
static int freelist_remove(uint32_t page_idx, uint32_t order)
{
    void *target = page_to_addr(page_idx);
    my_free_block_t **cur = &g_buddy.free_list[order];

    while (*cur) {
        if ((void *)*cur == target) {
            *cur = (*cur)->next;
            return 1;
        }
        cur = &(*cur)->next;
    }
    return 0;
}

/* Push a block onto the free list */
static void freelist_push(uint32_t page_idx, uint32_t order)
{
    my_free_block_t *block = (my_free_block_t *)page_to_addr(page_idx);
    block->next = g_buddy.free_list[order];
    g_buddy.free_list[order] = block;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void my_buddy_init(void *base, size_t size)
{
    uint32_t i;

    g_buddy.base        = base;
    g_buddy.total_pages = (uint32_t)(size / PAGE_SIZE);
    g_buddy.free_pages  = 0;

    for (i = 0; i <= MAX_ORDER; i++)
        g_buddy.free_list[i] = NULL;

    /* Mark all pages as allocated in bitmap first */
    for (i = 0; i < g_buddy.total_pages; i++)
        bitmap_set(i);

    /*
     * Release pages into the free lists in the largest power-of-2 chunks.
     * This mirrors how the boot allocator hands pages to the buddy system.
     */
    uint32_t page = 0;
    uint32_t remaining = g_buddy.total_pages;

    while (remaining > 0) {
        /* Find the highest order that both fits and is naturally aligned */
        uint32_t order = 0;
        while (order < MAX_ORDER
               && (1u << (order + 1)) <= remaining
               && (page & ((1u << (order + 1)) - 1)) == 0)
            order++;

        uint32_t count = 1u << order;
        for (i = page; i < page + count; i++)
            bitmap_clear(i);

        freelist_push(page, order);
        g_buddy.free_pages += count;

        page      += count;
        remaining -= count;
    }

    my_printf("[buddy] init: base=%p  pages=%u  free=%u\n",
              g_buddy.base, g_buddy.total_pages, g_buddy.free_pages);
}

/*
 * my_alloc_pages: allocate 2^order pages in a row.
 *
 * Algorithm:
 *   1. Look for a free block at 'order'.
 *   2. If none, look at order+1, order+2, … until we find one.
 *   3. Split the found block down to the requested order,
 *      returning the unused half to the lower free list each time.
 */
void *my_alloc_pages(uint32_t order)
{
    if (order > MAX_ORDER)
        return NULL;

    /* Find the smallest available order >= requested */
    uint32_t found = order;
    while (found <= MAX_ORDER && g_buddy.free_list[found] == NULL)
        found++;

    if (found > MAX_ORDER)
        return NULL;   /* out of memory */

    /* Split down from 'found' to 'order' */
    while (found > order) {
        /* Pop one block from free_list[found] */
        my_free_block_t *block = g_buddy.free_list[found];
        g_buddy.free_list[found] = block->next;
        found--;

        /* The block splits into two buddies at the lower order */
        uint32_t page_idx   = addr_to_page((void *)block);
        uint32_t buddy_idx  = page_idx ^ (1u << found);

        /* Push the right half (buddy) back to free_list[found] */
        freelist_push(buddy_idx, found);
        /* Push the left half so the loop can pop it next iteration */
        freelist_push(page_idx, found);
    }

    /* Pop the final block */
    my_free_block_t *result = g_buddy.free_list[order];
    g_buddy.free_list[order] = result->next;

    /* Mark all pages in this block as allocated */
    uint32_t page_idx = addr_to_page((void *)result);
    uint32_t count    = 1u << order;
    for (uint32_t i = page_idx; i < page_idx + count; i++)
        bitmap_set(i);

    g_buddy.free_pages -= count;
    return (void *)result;
}

/*
 * my_free_pages: give a block back to the buddy allocator.
 *
 * Algorithm:
 *   1. Mark pages as free.
 *   2. Look up the buddy of this block.
 *   3. If the buddy is also free, remove it from the free list and merge.
 *   4. Repeat at the next order until the buddy is not free or we hit MAX_ORDER.
 *   5. Push the (possibly merged) block onto its free list.
 */
void my_free_pages(void *ptr, uint32_t order)
{
    if (!ptr) return;

    uint32_t page_idx = addr_to_page(ptr);
    uint32_t count    = 1u << order;

    if (page_idx + count > g_buddy.total_pages)
        my_panic("my_free_pages: pointer out of range");

    /* Mark pages as free */
    for (uint32_t i = page_idx; i < page_idx + count; i++)
        bitmap_clear(i);

    g_buddy.free_pages += count;

    /*
     * Merge with the buddy for as long as the buddy is also free.
     *
     * The buddy of a block is found by flipping one bit of its page index,
     * which is what makes this cheap. If freelist_remove() succeeds then
     * the buddy was free and is now ours, so the two halves become one
     * block at the next order up and we go round again.
     */
    while (order < MAX_ORDER) {
        uint32_t buddy_idx = page_idx ^ (1u << order);

        if (buddy_idx >= g_buddy.total_pages)
            break;                              /* buddy is off the end   */
        if (bitmap_test(buddy_idx))
            break;                              /* buddy is in use        */
        if (!freelist_remove(buddy_idx, order))
            break;                              /* buddy is free, but was
                                                   split into smaller bits */

        if (buddy_idx < page_idx)
            page_idx = buddy_idx;   /* merged block starts at the lower index */
        order++;
    }

    freelist_push(page_idx, order);
}

void my_buddy_dump(void)
{
    my_printf("\n--- Buddy Allocator ---\n");
    my_printf("  total pages : %u  (%u KB)\n",
              g_buddy.total_pages, g_buddy.total_pages * 4);
    my_printf("  free  pages : %u  (%u KB)\n",
              g_buddy.free_pages,  g_buddy.free_pages  * 4);
    my_printf("  used  pages : %u  (%u KB)\n",
              g_buddy.total_pages - g_buddy.free_pages,
              (g_buddy.total_pages - g_buddy.free_pages) * 4);

    for (uint32_t o = 0; o <= MAX_ORDER; o++) {
        uint32_t count = 0;
        my_free_block_t *b = g_buddy.free_list[o];
        while (b) { count++; b = b->next; }
        if (count)
            my_printf("  order %u (%u KB each): %u block(s)\n",
                      o, (1u << o) * 4u, count);
    }
    my_printf("-----------------------\n\n");
}
