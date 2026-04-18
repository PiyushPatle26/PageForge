#ifndef MY_SLAB_H
#define MY_SLAB_H

/*
 * my_slab.h — slab allocator, built on top of the buddy allocator.
 *
 * This mirrors Linux's SLAB/SLUB subsystem (the layer above pages).
 *
 * Why slabs?
 *   The buddy allocator works in whole pages (4 KB minimum).
 *   Most kernel objects are tiny (64 bytes, 128 bytes, etc.).
 *   Slab takes one page from buddy, carves it into same-size objects,
 *   and maintains a free-list of those objects.
 *
 * Layout of one slab page:
 *   ┌──────────────────────────────────────────────┐
 *   │  slab_t header  │  obj0  │  obj1  │  obj2 …  │
 *   └──────────────────────────────────────────────┘
 *     (header sits at the page-aligned start)
 *
 * We use an embedded free-list: each free object stores a pointer to
 * the next free object (like a linked list inside the objects themselves).
 *
 * Size classes (bytes): 8, 16, 32, 64, 128, 256, 512, 1024
 */

#include "my_types.h"

#define SLAB_MAGIC      0x51AB1234u
#define NUM_SIZE_CLASSES 8
static const size_t MY_SIZE_CLASSES[NUM_SIZE_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024
};

/* Header stored at the beginning of each slab page */
typedef struct my_slab {
    struct my_slab *next;       /* next slab in the cache's list    */
    void           *free_list;  /* pointer to first free object     */
    uint32_t        num_free;   /* number of free objects left      */
    uint32_t        num_total;  /* total objects in this slab       */
    uint32_t        obj_size;   /* size of each object (bytes)      */
    uint32_t        magic;      /* sanity check                     */
} my_slab_t;

/* A cache manages all slabs for one fixed object size */
typedef struct {
    size_t      obj_size;
    my_slab_t  *partial;    /* slabs with some free objects     */
    my_slab_t  *full;       /* completely used slabs            */
    my_slab_t  *empty;      /* fully free slabs (ready to reuse)*/
    uint32_t    total_allocs;
    uint32_t    total_frees;
} my_kmem_cache_t;

extern my_kmem_cache_t g_slab_caches[NUM_SIZE_CLASSES];

/* Initialise all size-class caches */
void  my_slab_init(void);

/* Allocate one object from the cache matching 'size' */
void *my_slab_alloc(size_t size);

/* Return an object to its cache */
void  my_slab_free(void *ptr);

/* Print cache stats */
void  my_slab_dump(void);

#endif /* MY_SLAB_H */
