#ifndef MY_ALLOC_H
#define MY_ALLOC_H

/*
 * my_alloc.h — general-purpose allocator (the public API).
 *
 * This is the equivalent of kmalloc/kfree in the Linux kernel,
 * or malloc/free in user-space libc.
 *
 * Routing logic:
 *   size <= 1024  → my_slab_alloc()   (fast, no fragmentation)
 *   size >  1024  → my_alloc_pages()  (goes straight to buddy)
 *
 * A small header is placed before every allocation to store metadata,
 * similar to how glibc's malloc stores a chunk header.
 *
 *   Returned pointer:
 *   ┌──────────────────┬─────────── user data ───────────────┐
 *   │  alloc_header_t  │                                     │
 *   └──────────────────┴─────────────────────────────────────┘
 *     ↑ hidden from user        ↑ what we return
 */

#include "my_types.h"

#define ALLOC_MAGIC       0xA110C8EDu   /* magic to detect corruption */
#define LARGE_THRESHOLD   1024u

typedef struct {
    uint32_t magic;     /* ALLOC_MAGIC — for double-free / corruption check */
    size_t   size;      /* requested size (not including this header)        */
    uint32_t is_large;  /* 1 = came from buddy directly, 0 = came from slab  */
    uint32_t order;     /* buddy order (only valid when is_large == 1)       */
} my_alloc_header_t;

/* Initialise the allocator (call once at startup) */
void  my_alloc_init(void);

/* Allocate 'size' bytes. Returns NULL on failure. */
void *my_kmalloc(size_t size);

/* Free a pointer returned by my_kmalloc / my_calloc / my_realloc. */
void  my_kfree(void *ptr);

/* Allocate nmemb*size bytes, zero-initialised. */
void *my_calloc(size_t nmemb, size_t size);

/*
 * Resize an existing allocation.
 *   ptr == NULL   → behaves like my_kmalloc(new_size)
 *   new_size == 0 → behaves like my_kfree(ptr), returns NULL
 */
void *my_realloc(void *ptr, size_t new_size);

/* Print allocator stats */
void  my_alloc_dump(void);

#endif /* MY_ALLOC_H */
