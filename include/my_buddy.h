#ifndef MY_BUDDY_H
#define MY_BUDDY_H

/*
 * my_buddy.h — buddy system page allocator.
 *
 * This is exactly how the Linux kernel's page allocator works.
 *
 * Core idea:
 *   Memory is split into blocks whose sizes are powers of 2 (in pages).
 *   A block of order N holds 2^N pages.
 *
 *   order 0 → 1 page  (4 KB)
 *   order 1 → 2 pages (8 KB)
 *   order 2 → 4 pages (16 KB)
 *   ...up to MAX_ORDER
 *
 * Every block has a "buddy" — a block of the same size adjacent to it.
 * When you free a block, if its buddy is also free, they merge into a
 * bigger block (coalescing). This prevents fragmentation.
 *
 *   buddy_index = block_index XOR (1 << order)
 *
 * We maintain one free-list per order. Each entry in the free-list
 * uses the free block's own memory to store the next pointer.
 */

#include "my_types.h"

#define MAX_ORDER    10                    /* orders 0..10                  */
#define ARENA_PAGES  1024                  /* our "RAM" = 1024 pages = 4 MB */
#define ARENA_SIZE   (ARENA_PAGES * PAGE_SIZE)

/* A free block stores just a pointer to the next free block at the same order */
typedef struct my_free_block {
    struct my_free_block *next;
} my_free_block_t;

/* The buddy allocator's state */
typedef struct {
    my_free_block_t *free_list[MAX_ORDER + 1]; /* one free-list per order   */
    uint8_t          bitmap[ARENA_PAGES / 8];  /* 1 bit per page: 1=in-use  */
    void            *base;                     /* start of the managed arena */
    uint32_t         total_pages;
    uint32_t         free_pages;
} my_buddy_t;

/* One global buddy allocator (like the kernel's zone allocator) */
extern my_buddy_t g_buddy;

/* Initialise the allocator over 'base' memory of 'size' bytes */
void  my_buddy_init(void *base, size_t size);

/* Allocate 2^order contiguous pages. Returns page-aligned pointer or NULL. */
void *my_alloc_pages(uint32_t order);

/* Free the block previously returned by my_alloc_pages. */
void  my_free_pages(void *ptr, uint32_t order);

/* Print current free-list state */
void  my_buddy_dump(void);

#endif /* MY_BUDDY_H */
