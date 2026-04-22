# Memory Management in Linux Kernel: A Complete Guide
### With PageForge: Building Linux MM From Scratch

> **Sources:** the Linux source tree and the RISC-V privileged specification.
>
> Every kernel constant, struct and header path named below was checked
> against the headers of the kernel this was written on, Linux 7.0
> (`/usr/src/linux-headers-7.0.0-29-generic`), rather than quoted from memory.
> Statements about *when* something was renamed or removed are history and
> cannot be checked that way, so they are given as context.
>
> This matters because a lot of writing about Linux MM still uses names that
> no longer exist, and code written against them will not build.
>
> **Project:** PageForge, a complete, beginner-friendly reimplementation of the Linux
> kernel memory management stack in pure C, no libc, cross-compiled for **riscv64**
> and tested under QEMU user-mode emulation. The paging layer models **Sv39**, the
> three-level page-table format Linux boots with on rv64.

---

## Table of Contents

1. [Why Memory Management Is Hard](#1-why-memory-management-is-hard)
2. [The Big Picture: Four Layers](#2-the-big-picture-four-layers)
3. [Physical Memory: Pages](#3-physical-memory-pages)
4. [Memory Zones](#4-memory-zones)
5. [How Linux Gets Pages: The Page Allocator](#5-how-linux-gets-pages-the-page-allocator)
6. [The Buddy System: How PageForge Implements It](#6-the-buddy-system-how-pageforge-implements-it)
7. [Virtual Memory and Paging](#7-virtual-memory-and-paging)
8. [How Page Tables Work: The Sv39 Three-Level Walk](#8-how-page-tables-work-the-sv39-three-level-walk)
9. [PageForge's Paging Simulation](#9-pageforges-paging-simulation)
10. [The Slab Allocator: Linux's Object Cache](#10-the-slab-allocator-linuxs-object-cache)
11. [PageForge's Slab Implementation](#11-pageforges-slab-implementation)
12. [kmalloc: The General-Purpose Allocator](#12-kmalloc-the-general-purpose-allocator)
13. [PageForge's kmalloc/kfree/calloc/realloc](#13-pageforges-kmallockfreecallocrealloc)
14. [Process Address Space](#14-process-address-space)
15. [The syscall Layer: Talking to the OS](#15-the-syscall-layer-talking-to-the-os)
16. [Building PageForge: Design Decisions](#16-building-pageforge-design-decisions)

---

## 1. Why Memory Management Is Hard

Memory management inside the kernel is not as easy as memory management outside
the kernel. Simply put, the kernel lacks luxuries enjoyed by user-space.

User space can allocate memory casually. If it fails, you get NULL back and
the process can complain and exit. If memory is tight, the process sleeps
until some frees up. The kernel gets neither of those. It often cannot sleep,
because it may be holding a lock or servicing an interrupt, and it has nobody
to report a failure to.

What does that mean in practice?

**In user-space (your C program):**
```c
char *buf = malloc(1024);   // can block, can fail, libc handles everything
free(buf);
```
- `malloc` can sleep the process while the OS finds memory.
- If it fails you just get NULL and handle it.
- You never think about which physical page you got.

**In the kernel:**
- You may be inside an interrupt handler that **cannot sleep**.
- You need to know if the memory is **physically contiguous** (required for DMA).
- You need to control which **memory zone** the allocation comes from.
- You need to be extremely careful: a mistake hangs the machine silently.

This is why the Linux kernel has a carefully layered memory management subsystem,
and why understanding it makes you a fundamentally better systems programmer.

---

## 2. The Big Picture: Four Layers

Linux kernel memory management is a stack of layers. Each layer depends on the one
below it:

```
User programs
     │
     ▼
┌─────────────────────────────────────────────┐
│  Layer 4: kmalloc / kfree / vmalloc          │  General byte-sized allocations
│  (mm/slub.c, via mm/slab_common.c)           │
└────────────────────┬────────────────────────┘
                     │ uses
┌────────────────────▼────────────────────────┐
│  Layer 3: Slab Allocator                     │  Object caches, fixed-size chunks
│  (mm/slub.c)                                 │
└────────────────────┬────────────────────────┘
                     │ uses
┌────────────────────▼────────────────────────┐
│  Layer 2: Buddy Page Allocator               │  Power-of-2 page blocks
│  (mm/page_alloc.c)                           │
└────────────────────┬────────────────────────┘
                     │ uses
┌────────────────────▼────────────────────────┐
│  Layer 1: Physical Memory                    │  struct page for each 4KB frame
│  (boot allocator → mem_map)                  │
└─────────────────────────────────────────────┘
```

**Additionally, running alongside all of this:**
```
┌─────────────────────────────────────────────┐
│  Virtual Memory / Page Tables (MMU)          │  VA → PA translation
│  (mm/memory.c, arch/riscv/mm/)              │
└─────────────────────────────────────────────┘
```

PageForge implements all of these layers from scratch in pure C.

```
┌─────────────────────────────────────────────┐
│  my_alloc.c, my_kmalloc / my_kfree etc.     │  Layer 4
└────────────────────┬────────────────────────┘
                     │
┌────────────────────▼────────────────────────┐
│  my_slab.c, my_slab_alloc / my_slab_free  │  Layer 3
└────────────────────┬────────────────────────┘
                     │
┌────────────────────▼────────────────────────┐
│  my_buddy.c, my_alloc_pages / my_free_pages │  Layer 2
└────────────────────┬────────────────────────┘
                     │
┌────────────────────▼────────────────────────┐
│  my_syscall.c, my_mmap (anonymous pages)   │  Layer 1
└─────────────────────────────────────────────┘
```

---

## 3. Physical Memory: Pages

### 3.1 What Is a Page?

The kernel treats physical pages as the **basic unit of memory management**.

The processor can address a single byte, but the MMU does not work at that
granularity. It translates addresses a page at a time, so a page is the
smallest thing the kernel can meaningfully hand out, protect, or map.

Think of it this way:
- RAM is divided into fixed-size chunks called **pages** (or **page frames**).
- On RISC-V (and most other architectures), a page is **4096 bytes = 4 KB**.
- On 64-bit architectures it can be 8 KB or larger.
- 1 GB of RAM = 262,144 pages of 4 KB each.

The MMU, a piece of hardware inside the CPU, manages all translations from
virtual addresses to physical addresses using page-granularity tables.

### 3.2 struct page: The Kernel's Page Descriptor

The kernel represents **every physical page** in the system with a `struct page`
structure. This is defined in `<linux/mm_types.h>`:

```c
struct page {
    unsigned long    flags;       /* page state bits (dirty, locked, etc.) */
    atomic_t         _count;      /* usage reference count                 */
    atomic_t         _mapcount;   /* how many page tables map this page     */
    unsigned long    private;     /* private data (e.g., buffer head ptr)  */
    struct address_space *mapping;/* page cache this page belongs to        */
    pgoff_t          index;       /* offset within the address_space        */
    struct list_head lru;         /* LRU list linkage                      */
    void             *virtual;    /* kernel virtual address (NULL=highmem) */
};
```

**Key fields explained:**

| Field | Purpose |
|-------|---------|
| `flags` | Bit flags: `PG_dirty`, `PG_locked`, `PG_uptodate`, etc. (defined in `<linux/page-flags.h>`) |
| `_count` | Reference count. -1 = free, 0+ = in use. Use `page_count()` to read it. |
| `_mapcount` | How many page table entries map this physical page |
| `mapping` | If the page is in the page cache, points to `address_space` |
| `virtual` | The page's kernel virtual address. NULL for high memory pages. |

**Why one struct per physical page?**

You might wonder: if `struct page` takes 40 bytes and we have 524,288 pages on
a 4 GB machine (with 8 KB pages), that's only 20 MB of overhead for tracking
all physical memory. About 0.5% of total RAM, a very reasonable trade-off.

**Important:** `struct page` describes a **physical** page, not a virtual one.
The same physical page can be mapped by many virtual addresses simultaneously
(e.g., shared libraries). The struct tracks the physical page's state, not which
virtual address points to it.

### 3.3 PageForge's Equivalent

PageForge does not use `struct page` explicitly, but the concept is there:

```c
// In my_buddy.h:
// The buddy allocator tracks every page via a bitmap
// One bit per page: 0 = free, 1 = allocated
uint8_t bitmap[ARENA_PAGES / 8];
```

When you call `my_alloc_pages(0)`, you get back one physical 4 KB page.
When you call `my_free_pages(ptr, 0)`, that page is returned to the free pool.
The bitmap and free-lists are PageForge's simplified version of `mem_map`, the
kernel's global array of `struct page` structures.

---

## 4. Memory Zones

### 4.1 Why Zones Exist

Not all memory is equal. The kernel divides physical memory into **zones** because
of hardware constraints:

Hardware limitations mean the kernel cannot treat every page as
interchangeable. Where a page physically sits decides what it can be used for.

The constraint is always the same shape: some device, or some part of the
kernel, cannot reach every physical address, so the allocator has to know
which pages are reachable from where.

### 4.2 Zones on rv64

Linux defines the zones in `<linux/mmzone.h>`, but which ones actually exist
depends on the architecture. A 64-bit RISC-V kernel uses two:

| Zone | Description | Physical Range |
|------|-------------|----------------|
| `ZONE_DMA32` | Pages below 4 GB, for devices whose DMA engine only drives 32 address bits | 0 to 4 GB |
| `ZONE_NORMAL` | Everything else, all directly mapped by the kernel | above 4 GB |

That is the whole list. `ZONE_DMA32` is there because plenty of real
peripherals still drive only 32 address bits, so a driver that needs a buffer
its hardware can reach has to get one from below the 4 GB line. Everything
else is `ZONE_NORMAL`.

Two other zones turn up constantly in older material and are not used here:

- **`ZONE_DMA`** covered the first 16 MB, because ISA devices could not
  address past it. RISC-V has no ISA bus and no such devices, so the zone is
  not configured.
- **`ZONE_HIGHMEM`** does not exist on any 64-bit kernel, RISC-V included.
  It solved a problem 64-bit machines simply do not have, described below.

**Why ZONE_HIGHMEM existed, and why rv64 has no use for it**

The problem was never about how much RAM a machine had. It was about how much
of it the kernel could *see at once*. On a 32-bit kernel the entire virtual
address space is 4 GB, split 3 GB for user space and 1 GB for the kernel, and
only about 896 MB of that kernel window could permanently map physical RAM.
Any RAM past that point existed but had nowhere to live in the kernel's
address space, so it was called "high memory" and had to be mapped
temporarily, one window at a time, with `kmap()`.

Sv39 gives the kernel a 256 GB half of the address space to itself. Every byte
of physical RAM on any real RISC-V board fits in there with room to spare, so
the kernel maps all of it once at boot and never thinks about it again. There
is no high memory, no `kmap()`, and no split between RAM you can address and
RAM you cannot.

Worth understanding rather than skipping, because plenty of kernel
documentation still assumes the 32-bit world.
When `kmap()` or `ZONE_HIGHMEM` comes up, that is the problem being solved,
and on rv64 the address space is large enough that it never arises.

### 4.3 struct zone: Representing a Zone

Each zone is represented by `struct zone` in `<linux/mmzone.h>`:

```c
struct zone {
    unsigned long    watermark[NR_WMARK]; /* min/low/high watermarks */
    struct free_area free_area[NR_PAGE_ORDERS];/* buddy free lists per order */
    spinlock_t       lock;                /* protects this structure   */
    unsigned long    zone_start_pfn;      /* first page frame number   */
    unsigned long    present_pages;       /* total usable pages        */
    const char       *name;               /* "DMA", "Normal", "HighMem"*/
    /* ... many more fields ... */
};
```

The `watermark` array holds three thresholds:
- **min**: Memory is critically low; only emergency allocations succeed.
- **low**: kswapd starts reclaiming pages.
- **high**: Zone is sufficiently stocked; kswapd stops.

The `free_area` array is the heart of the buddy allocator. It holds one entry
per order: order 0 is 1 page, order 1 is 2 pages, up to order 10 at 1024
pages.

Two naming details, because older material gets both wrong. The largest order
is `MAX_PAGE_ORDER`, which is 10. It was called `MAX_ORDER` until 6.5, when
the meaning of the constant changed from exclusive to inclusive and it was
renamed so that out-of-tree code would fail loudly instead of silently
allocating the wrong size. The array is sized `NR_PAGE_ORDERS`, which is
`MAX_PAGE_ORDER + 1`, so 11 entries.

And each `free_area` is not one list. It is
`struct list_head free_list[MIGRATE_TYPES]`, because the kernel keeps free
pages separated by whether they can be moved, which is what makes compaction
possible. PageForge has one list per order, which is the same idea with the
migration types left out.

### 4.4 PageForge's Simplified Model

PageForge does not implement multiple zones, it operates with a single flat
4 MB arena. In a real kernel you would have separate zones, each with its own
`free_area[]`. PageForge simplifies this:

```c
// my_buddy.h  (PageForge keeps the old name for its own constant)
#define MAX_ORDER     10
#define ARENA_PAGES   1024        // 1024 pages × 4 KB = 4 MB
#define ARENA_SIZE    (ARENA_PAGES * PAGE_SIZE)

typedef struct {
    my_free_block_t *free_list[MAX_ORDER + 1]; // 11 orders: 1,2,4,...,1024 pages
    uint8_t          bitmap[ARENA_PAGES / 8];  // 1 bit per page
    void            *base;
    uint32_t         total_pages;
    uint32_t         free_pages;
} my_buddy_t;
```

This is `struct zone`'s `free_area[]` with the zones, the watermarks, the
locking and the migration types taken out.

---

## 5. How Linux Gets Pages: The Page Allocator

### 5.1 The Core Function

The kernel's page allocator provides one core function:

```c
struct page *alloc_pages(gfp_t gfp_mask, unsigned int order);
```

This allocates `2^order` contiguous physical pages and returns a pointer to the
first page's `struct page`.

Other convenience wrappers:

```c
// Get the logical address directly (no struct page needed)
unsigned long __get_free_pages(gfp_t gfp_mask, unsigned int order);

// Allocate a single page
struct page *alloc_page(gfp_t gfp_mask);
unsigned long __get_free_page(gfp_t gfp_mask);

// Allocate a single page, zeroed
unsigned long get_zeroed_page(unsigned int gfp_mask);

// Free pages
void free_pages(unsigned long addr, unsigned int order);
void free_page(unsigned long addr);
```

### 5.2 GFP Flags: Controlling Allocation Behavior

Every allocation call takes a `gfp_mask` (GFP = "Get Free Page") parameter.
These flags tell the allocator:
- Whether it is allowed to sleep waiting for memory
- Which memory zone to allocate from
- What the memory will be used for

**Action modifiers:**

These are defined in `<linux/gfp_types.h>`:

| Flag | Meaning |
|------|---------|
| `__GFP_DIRECT_RECLAIM` | The caller may sleep while the allocator reclaims memory |
| `__GFP_KSWAPD_RECLAIM` | Wake kswapd to reclaim in the background |
| `__GFP_HIGH` | The allocation is high priority and may dip into reserves |
| `__GFP_IO` | The allocator may start disk I/O |
| `__GFP_FS` | The allocator may call into the filesystem |
| `__GFP_ZERO` | Return zeroed memory |
| `__GFP_NOWARN` | Do not warn on failure |
| `__GFP_NOFAIL` | Retry forever, never fail |
| `__GFP_NORETRY` | Fail rather than retry hard |

Two flags that appear all over older documentation are gone. `__GFP_WAIT` was
split into the two reclaim flags above, and `__GFP_COLD`, which asked for
cache-cold pages, was removed once it stopped earning its keep. Neither exists
in `gfp_types.h` any more, so code using them will not build.

**Zone modifiers:**

| Flag | Meaning |
|------|---------|
| `__GFP_DMA` | Allocate from `ZONE_DMA` only |
| `__GFP_DMA32` | Allocate from `ZONE_DMA32` only |
| `__GFP_HIGHMEM` | Allocate from `ZONE_HIGHMEM` or `ZONE_NORMAL` |

**Type flags**, the ones you actually use:

| Flag | When to Use |
|------|-------------|
| `GFP_ATOMIC` | Interrupt handlers, softirqs, tasklets, must not sleep |
| `GFP_KERNEL` | Normal process context, can sleep, recommended default |
| `GFP_USER` | Allocating memory for user-space processes |
| `GFP_DMA` | Need DMA-able memory (device drivers) |
| `GFP_NOIO` | Block I/O code, can block but not start disk I/O |
| `GFP_NOFS` | Filesystem code, can block and start disk I/O, not FS I/O |

**When to use which flag:**

| Situation | Use This Flag |
|-----------|--------------|
| Process context, can sleep | `GFP_KERNEL` |
| Process context, cannot sleep | `GFP_ATOMIC` |
| Interrupt handler | `GFP_ATOMIC` |
| Softirq or tasklet | `GFP_ATOMIC` |
| Need DMA memory, can sleep | `GFP_DMA | GFP_KERNEL` |
| Need DMA memory, cannot sleep | `GFP_DMA | GFP_ATOMIC` |

**PageForge does not implement GFP flags**, our `my_alloc_pages(order)` has
no flags parameter. This is intentional: we are a user-space simulation, there
are no interrupt contexts, and all allocations can trivially "sleep".

---

## 6. The Buddy System: How PageForge Implements It

### 6.1 The Problem Buddy Solves

Imagine you have 1024 pages of free memory. You need to allocate and free many
different-sized blocks. Naive approaches have problems:

- **Fixed-size blocks**: Fast but wasteful (what if you need 3 pages but blocks
  are either 2 or 4?).
- **Byte-level allocators**: Very flexible but slow to find contiguous regions
  and prone to fragmentation.

The buddy allocator is a clever middle ground: it only allocates in **powers of
2 pages**, and merges freed blocks with their "buddies" to reduce fragmentation.

### 6.2 Core Concept

Every block in the buddy allocator has:
- A **size**: always `2^order` pages (so 1, 2, 4, 8, 16, ... pages).
- A **starting page index** `P` that must be aligned to its size.
- A **buddy**: the other block of the same size that, together, would form a
  block of the next-higher order.

**The buddy formula:**

```
Given a block at page index P with order N:
    buddy_index = P  XOR  (1 << N)
```

This XOR trick works because aligned blocks always differ in exactly bit N:
- Block at page 0, order 2 (size 4): `0 XOR 4 = 4` → buddy is at page 4.
- Block at page 4, order 2 (size 4): `4 XOR 4 = 0` → buddy is at page 0.
- Block at page 8, order 2 (size 4): `8 XOR 4 = 12` → buddy is at page 12.

### 6.3 Data Structures

The buddy allocator needs:

1. **Free lists**: one linked list of free blocks per order.
   - `free_list[0]` = list of all free 1-page blocks
   - `free_list[1]` = list of all free 2-page blocks
   - ...
   - `free_list[10]` = list of all free 1024-page blocks

2. **A bitmap**: one bit per page frame. 0 = free, 1 = allocated.

```c
// my_buddy.h
#define MAX_ORDER  10

typedef struct {
    void *next;           // Embedded next pointer in the free block itself
} my_free_block_t;

typedef struct {
    my_free_block_t *free_list[MAX_ORDER + 1];
    uint8_t          bitmap[ARENA_PAGES / 8];
    void            *base;           // Start of the arena
    uint32_t         total_pages;
    uint32_t         free_pages;
} my_buddy_t;
```

Notice: free blocks store their `next` pointer **inside themselves** at offset 0.
This is an "intrusive linked list", no extra memory needed for list nodes.
Linux's real buddy allocator uses the same technique via `struct list_head`.

### 6.4 Initialization

```c
void my_buddy_init(void *base, size_t size)
{
    g_buddy.base        = base;
    g_buddy.total_pages = size / PAGE_SIZE;

    // First: mark all pages allocated in the bitmap
    for (i = 0; i < g_buddy.total_pages; i++)
        bitmap_set(i);

    // Then: release pages into free lists as largest aligned chunks
    uint32_t page = 0, remaining = g_buddy.total_pages;
    while (remaining > 0) {
        // Find highest order that fits and is aligned
        uint32_t order = 0;
        while (order < MAX_ORDER
               && (1u << (order+1)) <= remaining
               && (page & ((1u << (order+1)) - 1)) == 0)
            order++;

        // Clear bitmap bits, push block onto free list
        for (i = page; i < page + (1u << order); i++)
            bitmap_clear(i);
        freelist_push(page, order);
        g_buddy.free_pages += 1u << order;
        page += 1u << order;
        remaining -= 1u << order;
    }
}
```

This mirrors the Linux boot sequence: the boot allocator hands pages to the
buddy allocator in the largest power-of-2 aligned chunks possible.

For 1024 pages: the first call releases all 1024 pages as one order-10 block.

### 6.5 Allocation: Splitting

```
my_alloc_pages(order):
  1. Find the smallest available order >= requested order
  2. While we have a block larger than needed:
       a. Pop the block from its free list
       b. Split it: push the right half (buddy) onto the lower free list
       c. The left half moves down one order
  3. Pop and return the block at the requested order
  4. Mark its pages as allocated in the bitmap
```

**Example: Allocate 1 page (order 0) from 1024 free pages (order 10):**

```
Start: free_list[10] = [0..1023]

Step 1: Split order-10 block at page 0:
  - Right half (buddy) at page 512 → push to free_list[9]
  - Left half at page 0 → push to free_list[9]

free_list[9] = [512], [0]

Step 2: Split order-9 block at page 0:
  - Right half at page 256 → free_list[8]
  - Left half at page 0 → free_list[8]

... (continue splitting) ...

Step 10: At order 0:
  free_list[0] = [1], [0]
  Pop page 0 → return to caller
  Mark page 0 as allocated
```

**Code:**

```c
void *my_alloc_pages(uint32_t order)
{
    // Find smallest available order >= requested
    uint32_t found = order;
    while (found <= MAX_ORDER && g_buddy.free_list[found] == NULL)
        found++;

    if (found > MAX_ORDER) return NULL;   // out of memory

    // Split down from 'found' to 'order'
    while (found > order) {
        my_free_block_t *block = g_buddy.free_list[found];
        g_buddy.free_list[found] = block->next;
        found--;

        uint32_t page_idx  = addr_to_page((void *)block);
        uint32_t buddy_idx = page_idx ^ (1u << found);

        freelist_push(buddy_idx, found);  // right half → free list
        freelist_push(page_idx,  found);  // left half → free list (next iteration pops)
    }

    // Pop the final block
    my_free_block_t *result = g_buddy.free_list[order];
    g_buddy.free_list[order] = result->next;

    // Mark pages as allocated
    uint32_t page_idx = addr_to_page((void *)result);
    for (uint32_t i = page_idx; i < page_idx + (1u << order); i++)
        bitmap_set(i);

    g_buddy.free_pages -= 1u << order;
    return (void *)result;
}
```

### 6.6 Freeing: Coalescing

This is the buddy magic. When freeing a block, we check if its buddy is also free.
If so, merge them into a larger block, then check the buddy at the next order too.
Keep going until no merge is possible or we reach MAX_ORDER.

```
my_free_pages(ptr, order):
  1. Compute page_idx from ptr
  2. Clear bitmap bits for all pages in the block
  3. While order < MAX_ORDER:
       buddy_idx = page_idx ^ (1 << order)
       If buddy is within arena AND bitmap says buddy is free
          AND buddy is in the free list at this order:
           → Remove buddy from free_list[order]
           → page_idx = min(page_idx, buddy_idx)  // merged block starts lower
           → order++
       Else: break
  4. Push merged block onto free_list[order]
```

**Example: Free page 1 (order 0):**

```
page_idx = 1, order = 0
buddy_idx = 1 ^ (1 << 0) = 1 ^ 1 = 0

Is page 0 free? Yes (we just freed pages 0-1 in previous example)
→ Remove page 0 from free_list[0]
→ page_idx = 0, order = 1

buddy_idx = 0 ^ (1 << 1) = 0 ^ 2 = 2
Is page 2 free? Yes
→ Remove page 2 from free_list[1]
→ page_idx = 0, order = 2

... keeps merging ...

Eventually: page_idx = 0, order = 10
→ Push to free_list[10]

We're back to having one order-10 block covering all 1024 pages!
```

**Code:**

```c
void my_free_pages(void *ptr, uint32_t order)
{
    uint32_t page_idx = addr_to_page(ptr);

    // Mark pages as free
    for (uint32_t i = page_idx; i < page_idx + (1u << order); i++)
        bitmap_clear(i);
    g_buddy.free_pages += 1u << order;

    // Coalesce with buddy while possible
    while (order < MAX_ORDER) {
        uint32_t buddy_idx = page_idx ^ (1u << order);

        if (buddy_idx >= g_buddy.total_pages) break;
        if (bitmap_test(buddy_idx)) break;            // buddy is allocated
        if (!freelist_contains(buddy_idx, order)) break; // not in free list

        freelist_remove(buddy_idx, order);
        if (buddy_idx < page_idx) page_idx = buddy_idx; // lower address wins
        order++;
    }

    freelist_push(page_idx, order);
}
```

### 6.7 Bitmap Helpers

The bitmap stores one bit per page in a byte array:

```c
static void bitmap_set(uint32_t page_idx) {
    g_buddy.bitmap[page_idx / 8] |= (uint8_t)(1u << (page_idx % 8));
}
static void bitmap_clear(uint32_t page_idx) {
    g_buddy.bitmap[page_idx / 8] &= (uint8_t)~(1u << (page_idx % 8));
}
static int bitmap_test(uint32_t page_idx) {
    return (g_buddy.bitmap[page_idx / 8] >> (page_idx % 8)) & 1;
}
```

`page_idx / 8` → which byte.
`page_idx % 8` → which bit within that byte.

### 6.8 Address ↔ Page Index Conversion

```c
static uint32_t addr_to_page(void *addr) {
    return (uint32_t)(((uintptr_t)addr - (uintptr_t)g_buddy.base) / PAGE_SIZE);
}
static void *page_to_addr(uint32_t page_idx) {
    return (void *)((uintptr_t)g_buddy.base + (uintptr_t)page_idx * PAGE_SIZE);
}
```

The kernel uses similar arithmetic with `page_to_pfn()` and `pfn_to_page()` macros.

---

## 7. Virtual Memory and Paging

Everything up to here has been about physical memory: pages, zones, and the
allocator that hands them out. This section is about the other half, the
addresses programs actually use.

### 7.1 Why Virtual Memory Exists

Without it, every program would have to know where in physical RAM it was
going to run. Programs would collide, nothing would be isolated, and you could
never run more of them than would fit in memory at once.

Virtual memory gives four things:

1. **Isolation.** Every process gets its own address space. A bug in one
   cannot corrupt another's memory, because it has no way to name it.
2. **Room.** A process sees one large flat range of addresses regardless of
   how much RAM exists or how scattered its pages are.
3. **Sharing.** Several processes can map the same physical page, which is how
   one copy of a shared library serves everybody.
4. **Permissions.** Each page is separately readable, writable or executable,
   which is what stops a program writing over its own code.

### 7.2 The MMU and the TLB

The conversion happens in hardware, in the MMU, on every single memory access.
It reads the page tables the kernel built and turns a virtual address into a
physical one.

Doing that properly means several memory reads before the real one, so the
result is cached in the TLB. A hit skips the walk entirely. A miss pays for
the full lookup.

On RISC-V the MMU finds the tables through the `satp` register, and the kernel
must invalidate stale TLB entries itself with `sfence.vma`. Nothing is flushed
implicitly.

### 7.3 Virtual Address Layout on rv64

Sv39 gives 39 bits of address, which is 512 GB, split into two halves with a
large illegal gap between them. The bottom half is user space, the top half is
the kernel, and the kernel half is mapped into every process so a syscall
changes privilege level without changing `satp`.

---

## 8. How Page Tables Work: The Sv39 Three-Level Walk

A flat table with one entry per page would need about 1 GB per process, nearly
all of it empty. So the page table is a tree instead: three levels of 512
entries, where a branch that maps nothing is simply never allocated.

The address splits into three 9-bit indexes and a 12-bit offset:

```
+-------------+--------+--------+--------+------------+
|   63..39    | VPN[2] | VPN[1] | VPN[0] |   offset   |
| sign extend | 9 bits | 9 bits | 9 bits |  12 bits   |
+-------------+--------+--------+--------+------------+
```

Each index picks one entry in one table. The offset is carried through
untouched and added at the end.

An entry is 64 bits: the physical page number in bits 53..10, and flag bits
V, R, W, X, U, G, A and D at the bottom. Whether an entry is a pointer to the
next level or the end of the walk is decided by its permissions, not by a
type field, and that same rule is where 2 MB and 1 GB pages come from.

> **This is covered properly in [PAGING.md](PAGING.md).** That document works
> through an address by hand, explains why the page number shift is 10 while
> the page shift is 12, and covers `satp`, the fault causes and the permission
> checks in detail. It is the one to read if you want to understand the walk
> rather than just place it in context.

### 8.1 Beyond Sv39

Sv48 and Sv57 add one and two more levels of the same 9 bits each, reaching 48
and 57 bits of address. Nothing else changes. Linux picks whichever the
hardware reports at boot and folds the unused levels away at compile time,
which is why the same generic code services all three.

---

## 9. PageForge's Paging Simulation

`src/my_paging.c` implements that walk in software, in about 176 lines.

There is one walk function in the file. Three public entry points wrap it:
`my_virt_to_phys()` translates, `my_access()` translates and then checks the
access against the leaf's permissions the way hardware does, and
`my_page_walk()` does the same walk while printing every step.

Running `make run` prints a full walk per address:

```
  Page Walk  VA = 0x1abc
  ├─ VPN[2]    : 0  (bits 38..30)
  ├─ VPN[1]    : 0  (bits 29..21)
  ├─ VPN[0]    : 1  (bits 20..12)
  ├─ Offset    : 0xabc  (bits 11..0)
  ├─ PGD[0] = 0x1ef3439d2c01  [V-----]
  ├─ PMD[0] = 0x1ef3439d2401  [V-----]
  ├─ PTE[1] = 0x200400cb  [VR-X-A]
  └─ Physical = 0x80100abc
```

The one place the model departs from hardware is worth knowing: each table
carries an array of real C pointers alongside its entries, because a user
process cannot follow an invented physical address. Real hardware needs no
such thing.

For the full walkthrough, the worked examples and how all of it lines up with
`arch/riscv/mm`, see [PAGING.md](PAGING.md).

---

## 10. The Slab Allocator: Linux's Object Cache

### 10.1 The Problem Slab Solves

The page allocator gives you whole pages (4 KB minimum). But kernel code constantly
needs small objects, a `task_struct` (process descriptor) might be 1.7 KB, an
`inode` might be 0.5 KB. If you allocated a whole page for each one, you would
waste enormous amounts of memory.

Enter free lists:

A free list is the usual answer: keep a stash of already-allocated structures
lying around, and when code needs one, take it off the list instead of
allocating from scratch. Freeing puts it back on the list rather than
returning it to the system.

But ad-hoc free lists have a problem: the kernel has no global control. When memory
is low, there's no way to tell every random free list to shrink.

**The slab allocator** consolidates all these free lists into one managed system.

### 10.2 Slab Design Principles

The slab layer is built on a few ideas that all follow from each other:

- Structures that get allocated and freed constantly are worth caching.
- Repeated allocation and freeing fragments memory, so keep the cached objects
  packed together contiguously.
- A freed object can go straight back out to the next allocation, which is
  where most of the speed comes from.
- An allocator that knows the object size, the page size and the size of its
  own cache can make far better decisions than a general purpose one.

Linux shipped three implementations of this idea for years: SLAB, SLUB and
SLOB. That is now history. SLOB was removed in 6.4 and SLAB in 6.8, leaving
SLUB as the only one, so `mm/slab.c` and `mm/slob.c` no longer exist and
`mm/slub.c` is what you read. `mm/slab.h` and `mm/slab_common.c` hold the
shared plumbing.

The slab concept was first implemented in **SunOS 5.4** and described academically
in the paper: Bonwick, J. "The Slab Allocator: An Object-Caching Kernel Memory
Allocator," USENIX, 1994.

### 10.3 The Three-Level Hierarchy

```
Cache (kmem_cache)
├── Slab 1 (one or more pages from the page allocator)
│   ├── [obj][obj][obj][obj]  ← full: all allocated
│   └── free_list → NULL
├── Slab 2
│   ├── [obj][   ][obj][   ]  ← partial: some free
│   └── free_list → obj2 → obj4 → NULL
└── Slab 3
    ├── [   ][   ][   ][   ]  ← empty: all free
    └── free_list → obj1 → obj2 → obj3 → obj4 → NULL
```

**Cache (struct kmem_cache)**: Manages objects of one specific type/size.
Linux has caches for: `task_struct`, `mm_struct`, `inode`, `dentry`, `file`,
`sock`, and many more.

**Slab (struct slab)**: One or more physically contiguous pages from the buddy
allocator. Contains the objects, plus a descriptor tracking their state.

**Objects**: The actual kernel data structures being cached.

### 10.4 Three Slab States

Every slab is in one of three states:

| State | Meaning | Used For |
|-------|---------|---------|
| **full** | All objects are allocated | Do not allocate from here |
| **partial** | Some objects free, some allocated | Allocate from here first |
| **empty** | All objects are free | Can return to buddy allocator |

Allocation priority: partial → empty → new slab from buddy allocator.

### 10.5 The Embedded Free List Trick

How does the slab track free objects without extra memory?

> The free list is embedded **inside the objects themselves**.

Each free object stores a pointer to the next free object at offset 0 within
the object's memory. Since the object is free (unused), we can use its memory
for this purpose.

```
Free slab with 5 objects:

free_list ──► [next=obj2|....] obj1 (free, stores pointer to obj2)
              [next=obj3|....] obj2
              [next=obj4|....] obj3
              [next=obj5|....] obj4
              [next=NULL |....] obj5

After allocating obj1:
- Return pointer to obj1 to caller
- free_list = obj2
- obj1's memory is now used by caller (overwrites the next pointer)
```

### 10.6 The Linux Slab API

```c
// Create a new cache
struct kmem_cache *kmem_cache_create(
    const char *name,         // name (appears in /proc/slabinfo)
    size_t       size,        // size of each object
    size_t       align,       // alignment (usually 0 = natural)
    unsigned long flags,      // SLAB_HWCACHE_ALIGN, SLAB_POISON, etc.
    void (*ctor)(void *)      // constructor (usually NULL)
);

// Destroy a cache (all slabs must be empty)
int kmem_cache_destroy(struct kmem_cache *cachep);

// Allocate an object from a cache
void *kmem_cache_alloc(struct kmem_cache *cachep, gfp_t flags);

// Free an object back to its cache
void kmem_cache_free(struct kmem_cache *cachep, void *objp);
```

**Real example from kernel, process descriptor cache:**

```c
// kernel/fork.c, create the task_struct cache at boot
task_struct_cachep = kmem_cache_create("task_struct",
                                        sizeof(struct task_struct),
                                        ARCH_MIN_TASKALIGN,
                                        SLAB_PANIC | SLAB_NOTRACK,
                                        NULL);

// Allocate a task_struct when creating a new process
struct task_struct *tsk;
tsk = kmem_cache_alloc(task_struct_cachep, GFP_KERNEL);
if (!tsk) return NULL;

// Free it when the process exits
kmem_cache_free(task_struct_cachep, tsk);
```

**SLAB flags:**

| Flag | Meaning |
|------|---------|
| `SLAB_HWCACHE_ALIGN` | Align objects to cache line boundaries (performance) |
| `SLAB_POISON` | Fill freed objects with `0xa5a5a5a5` (detect use-after-free) |
| `SLAB_RED_ZONE` | Insert "red zones" around objects (detect buffer overruns) |
| `SLAB_PANIC` | Panic if cache creation fails (for critical caches) |
| `SLAB_CACHE_DMA` | Allocate slabs from ZONE_DMA |

---

## 11. PageForge's Slab Implementation

### 11.1 Size Classes

PageForge uses **8 fixed size classes**: 8, 16, 32, 64, 128, 256, 512, 1024 bytes.
Each size class has its own `my_kmem_cache_t`.

```c
// my_slab.h
#define NUM_SIZE_CLASSES   8
static const size_t MY_SIZE_CLASSES[NUM_SIZE_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024
};
```

When you ask for 20 bytes, PageForge rounds up to the 32-byte class.

In Linux, `kmalloc()` has ~14 size classes from 8 bytes up to 8 MB.

### 11.2 The Slab Header

PageForge places the slab descriptor at the **very start of its page**:

```c
// my_slab.h
#define SLAB_MAGIC  0x51AB1234u

typedef struct my_slab {
    uint32_t       magic;      // SLAB_MAGIC, detects corrupt/wrong pointers
    uint32_t       obj_size;   // size of each object in this slab
    uint32_t       num_total;  // total objects in this slab
    uint32_t       num_free;   // free objects remaining
    void          *free_list;  // embedded free list head
    struct my_slab *next;      // link to next slab in the list
} my_slab_t;
```

**Layout of a 4 KB page used as a slab:**

```
Page start (page-aligned address)
├─────────────────────────────────── 0
│  my_slab_t header (24 bytes)
├─────────────────────────────────── 24 (padded up to obj_size alignment)
│  Object 0
│  Object 1
│  ...
│  Object N-1
└─────────────────────────────────── 4096
```

**Finding the slab header from any object pointer** is simple:

```c
// Since the header is at the page-aligned start:
uintptr_t page_base = (uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1);
my_slab_t *slab = (my_slab_t *)page_base;

// Verify with magic number
if (slab->magic != SLAB_MAGIC) panic("double free or corrupt pointer");
```

This is exactly how the Linux slab allocator finds the `struct slab` for a
given object, it masks off the page offset bits.

### 11.3 Slab Creation (slab_new)

```c
static my_slab_t *slab_new(my_kmem_cache_t *cache)
{
    // 1. Get one page from the buddy allocator
    void *page = my_alloc_pages(0);
    if (!page) return NULL;

    // 2. Place the header at page start
    my_slab_t *slab = (my_slab_t *)page;
    slab->magic    = SLAB_MAGIC;
    slab->obj_size = (uint32_t)cache->obj_size;
    slab->next     = NULL;

    // 3. Find where objects start (after header, aligned to obj_size)
    uintptr_t obj_start = (uintptr_t)page + sizeof(my_slab_t);
    uintptr_t align = cache->obj_size;
    if (obj_start % align)
        obj_start += align - (obj_start % align);

    // 4. Calculate how many objects fit
    slab->num_total = (PAGE_SIZE - (obj_start - (uintptr_t)page)) / cache->obj_size;
    slab->num_free  = slab->num_total;

    // 5. Build the embedded free list (in reverse for cache-friendliness)
    slab->free_list = NULL;
    for (int i = (int)slab->num_total - 1; i >= 0; i--) {
        void *obj      = (void *)(obj_start + (uintptr_t)i * cache->obj_size);
        *(void **)obj  = slab->free_list;  // store next pointer at obj offset 0
        slab->free_list = obj;
    }
    return slab;
}
```

**How many objects fit in a 4 KB page for each size class?**

| Size class | Header + padding | Objects | Waste |
|-----------|-----------------|---------|-------|
| 8 bytes | 32 bytes (header=24, pad=8) | 507 | 8 bytes |
| 16 bytes | 32 bytes | 253 | 16 bytes |
| 32 bytes | 32 bytes | 126 | 32 bytes |
| 64 bytes | 64 bytes | 62 | 64 bytes |
| 128 bytes | 128 bytes | 30 | 128 bytes |
| 256 bytes | 256 bytes | 15 | 0 bytes |
| 512 bytes | 512 bytes | 7 | 0 bytes |
| 1024 bytes | 1024 bytes | 3 | 1024 bytes |

### 11.4 Allocation

```c
void *my_slab_alloc(size_t size)
{
    // Find the smallest size class that fits
    my_kmem_cache_t *cache = NULL;
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        if (MY_SIZE_CLASSES[i] >= size) { cache = &g_slab_caches[i]; break; }
    }
    if (!cache) return NULL;   // size > 1024, use buddy directly

    // Get a slab with free space (prefer partial, then empty, then new)
    my_slab_t *slab = cache->partial;
    if (!slab) {
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

    // Pop one object off the embedded free list
    void *obj       = slab->free_list;
    slab->free_list = *(void **)obj;
    slab->num_free--;

    // If slab just became full, move it to the full list
    if (slab->num_free == 0) {
        cache->partial = slab->next;
        slab->next     = cache->full;
        cache->full    = slab;
    }

    cache->total_allocs++;
    return obj;
}
```

### 11.5 Freeing

```c
void my_slab_free(void *ptr)
{
    if (!ptr) return;

    // Find slab header (always at page-aligned address of the page containing ptr)
    uintptr_t  page_base = (uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1);
    my_slab_t *slab      = (my_slab_t *)page_base;

    if (slab->magic != SLAB_MAGIC)
        my_panic("my_slab_free: bad magic, double free or corrupt pointer");

    // Find which cache owns this slab
    my_kmem_cache_t *cache = NULL;
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        if (g_slab_caches[i].obj_size == slab->obj_size) {
            cache = &g_slab_caches[i]; break;
        }
    }

    int was_full = (slab->num_free == 0);

    // Push object back onto embedded free list
    *(void **)ptr   = slab->free_list;
    slab->free_list = ptr;
    slab->num_free++;

    // Handle state transitions
    if (was_full) {
        // full → partial
        slab_list_remove(&cache->full, slab);
        slab->next    = cache->partial;
        cache->partial = slab;
    } else if (slab->num_free == slab->num_total) {
        // partial → empty
        slab_list_remove(&cache->partial, slab);
        slab->next   = cache->empty;
        cache->empty = slab;
    }

    cache->total_frees++;
}
```

### 11.6 State Transitions Summary

```
                ┌─────────────────────────────────────────┐
                │          allocate (slab was partial)     │
                ▼                                          │
           ┌─────────┐   slab fills up   ┌──────────┐     │
  new slab  │ partial │ ──────────────► │   full   │     │
  ────────► └─────────┘                 └──────────┘     │
                │                             │            │
                │ all objects freed            │ free one   │
                ▼                             │ object     │
           ┌─────────┐ ◄───────────────────── ┘            │
           │  empty  │                                      │
           └─────────┘                                      │
                │                                           │
                └───────────────────────────────────────────┘
                              reuse (allocate moves to partial)
```

---

## 12. kmalloc: The General-Purpose Allocator

### 12.1 What kmalloc Is

```c
// Declared in <linux/slab.h>
void *kmalloc(size_t size, gfp_t flags);
```

`kmalloc()` behaves much like `malloc()` in user space, apart from the extra
flags argument. It hands back memory in byte-sized chunks and is the usual
choice for kernel allocations. If you want whole pages, the page allocator
interfaces above are the better fit.

Key properties:
- Returns **physically contiguous** memory (unlike `vmalloc()`)
- Backed by the slab allocator internally
- Must check return value for NULL
- Must not call from interrupt context with `GFP_KERNEL` flag

```c
struct dog *p;
p = kmalloc(sizeof(struct dog), GFP_KERNEL);
if (!p)
    /* handle error */
```

### 12.2 kfree

```c
// Declared in <linux/slab.h>
void kfree(const void *ptr);
```

Only ever pass `kfree()` a pointer that came from `kmalloc()`, and only once.
Freeing something twice, or freeing a pointer that was never allocated this
way, corrupts the allocator's bookkeeping and can hand another part of the
kernel's memory to the next caller.

`kfree(NULL)` is safe and does nothing.

**Example:**

```c
char *buf;
buf = kmalloc(BUF_SIZE, GFP_ATOMIC);
if (!buf) { /* handle error */ }

/* ... use buf ... */

kfree(buf);
```

### 12.3 vmalloc: Virtually Contiguous

```c
void *vmalloc(unsigned long size);
void vfree(const void *addr);
```

`vmalloc()` allocates memory that is **virtually** contiguous but **not necessarily
physically** contiguous. It does this by mapping potentially non-contiguous physical
pages into a contiguous region of the kernel's virtual address space.

Most kernel code uses `kmalloc()` not `vmalloc()` because:
- `vmalloc()` pages must be mapped individually via page table entries
- This causes more TLB misses (slower)
- Only use `vmalloc()` when you need large regions that don't fit in buddy allocator

`vmalloc()` is used when inserting kernel modules (they can be large).

---

## 13. PageForge's kmalloc/kfree/calloc/realloc

### 13.1 The Hidden Header

PageForge's `my_kmalloc()` works differently from a naive approach. Instead of
using separate metadata structures, it **hides a header** just before the returned
pointer:

```
my_kmalloc(N) allocates:
┌──────────────────────────────────────────┐
│  my_alloc_header_t  (hidden)             │  ← not returned to caller
│  magic, size, is_large, order            │
├──────────────────────────────────────────┤
│  N bytes of user data                    │  ← returned pointer
└──────────────────────────────────────────┘
```

This means `my_kfree(ptr)` can find the header by doing `ptr - sizeof(header)`.

```c
// my_alloc.h
#define ALLOC_MAGIC       0xA110C8EDu  // "ALLOC8ED", alloc-ated
#define LARGE_THRESHOLD   1024

typedef struct {
    uint32_t magic;       // ALLOC_MAGIC, detect corrupt/double-free
    uint32_t size;        // original requested size (not including header)
    uint8_t  is_large;    // 0 = slab-backed, 1 = buddy-page-backed
    uint8_t  order;       // if is_large: buddy order used
    uint8_t  pad[2];
} my_alloc_header_t;
```

### 13.2 my_kmalloc

```c
void *my_kmalloc(size_t size)
{
    if (size == 0) return NULL;

    size_t total = size + sizeof(my_alloc_header_t);

    my_alloc_header_t *hdr;
    if (total <= LARGE_THRESHOLD) {
        // Small: route through slab allocator
        hdr = (my_alloc_header_t *)my_slab_alloc(total);
    } else {
        // Large: allocate directly from buddy allocator
        uint32_t order = 0;
        while ((PAGE_SIZE << order) < total) order++;
        hdr = (my_alloc_header_t *)my_alloc_pages(order);
        hdr->order = (uint8_t)order;
    }

    if (!hdr) return NULL;

    hdr->magic    = ALLOC_MAGIC;
    hdr->size     = (uint32_t)size;
    hdr->is_large = (total > LARGE_THRESHOLD) ? 1 : 0;

    return (void *)(hdr + 1);  // return pointer past the header
}
```

### 13.3 my_kfree

```c
void my_kfree(void *ptr)
{
    if (!ptr) return;

    // Recover header (sits just before ptr)
    my_alloc_header_t *hdr = (my_alloc_header_t *)ptr - 1;

    if (hdr->magic != ALLOC_MAGIC)
        my_panic("my_kfree: bad magic, double free or corrupt pointer");

    // Poison freed memory to catch use-after-free
    uint32_t *p    = (uint32_t *)ptr;
    uint32_t  n    = hdr->size / sizeof(uint32_t);
    for (uint32_t i = 0; i < n; i++) p[i] = 0xDEADDEADu;
    hdr->magic = 0;   // invalidate header

    if (!hdr->is_large) {
        my_slab_free(hdr);
    } else {
        my_free_pages(hdr, hdr->order);
    }
}
```

**Memory poisoning** (`0xDEADDEAD`) means: if code accesses freed memory, it reads
a distinctive garbage value. This is similar to Linux's `SLAB_POISON` flag which
fills freed objects with `0xa5a5a5a5`.

### 13.4 my_calloc

```c
void *my_calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *ptr = my_kmalloc(total);
    if (!ptr) return NULL;

    // Zero the memory
    uint8_t *b = (uint8_t *)ptr;
    for (size_t i = 0; i < total; i++) b[i] = 0;
    return ptr;
}
```

Note: the slab allocator **does not** zero memory by default (unlike `calloc`).
In Linux, `kzalloc(size, flags)` is provided for this purpose:

```c
// In Linux kernel code:
void *p = kzalloc(sizeof(struct foo), GFP_KERNEL);  // zeroed
// equivalent to:
void *p = kmalloc(sizeof(struct foo), GFP_KERNEL);
if (p) memset(p, 0, sizeof(struct foo));
```

### 13.5 my_realloc

```c
void *my_realloc(void *ptr, size_t new_size)
{
    if (!ptr) return my_kmalloc(new_size);
    if (new_size == 0) { my_kfree(ptr); return NULL; }

    my_alloc_header_t *hdr = (my_alloc_header_t *)ptr - 1;
    size_t old_size = hdr->size;

    // If new size fits in the same slab class: in-place realloc
    // (find the size class that was used)
    for (int i = 0; i < NUM_SIZE_CLASSES; i++) {
        size_t class_size = MY_SIZE_CLASSES[i] - sizeof(my_alloc_header_t);
        if (old_size <= class_size && new_size <= class_size) {
            hdr->size = (uint32_t)new_size;
            return ptr;   // already fits in the same slab slot
        }
    }

    // Otherwise: allocate new, copy, free old
    void *new_ptr = my_kmalloc(new_size);
    if (!new_ptr) return NULL;

    size_t copy_size = old_size < new_size ? old_size : new_size;
    uint8_t *src = (uint8_t *)ptr;
    uint8_t *dst = (uint8_t *)new_ptr;
    for (size_t i = 0; i < copy_size; i++) dst[i] = src[i];

    my_kfree(ptr);
    return new_ptr;
}
```

---

## 14. Process Address Space

### 14.1 What Is a Process Address Space?

Each process in Linux has its own **virtual address space**, a range of virtual
addresses it can use. This virtual address space is divided into regions called
**VMAs (Virtual Memory Areas)**.

The kernel describes a process's whole address space with one structure, the
memory descriptor. It is `struct mm_struct`, defined in
`<linux/mm_types.h>`, and everything about the process's memory hangs off it.

### 14.2 struct mm_struct

Every process has a `struct mm_struct` in its `task_struct`:

```c
struct mm_struct {
    struct vm_area_struct *mmap;    /* list of all VMAs (VMA linked list) */
    struct rb_root         mm_rb;   /* VMA red-black tree for fast lookup */
    unsigned long          mmap_base;  /* base of mmap area               */
    unsigned long          task_size;  /* size of user address space       */
    unsigned long          start_code; /* start address of code segment    */
    unsigned long          end_code;   /* end address of code segment      */
    unsigned long          start_data; /* start address of data segment    */
    unsigned long          end_data;   /* end address of data segment      */
    unsigned long          start_brk;  /* heap start address               */
    unsigned long          brk;        /* current top of heap              */
    unsigned long          start_stack;/* stack start address              */
    pgd_t                 *pgd;        /* page global directory             */
    atomic_t               mm_users;   /* users sharing this mm            */
    atomic_t               mm_count;   /* reference count                  */
    /* ... many more fields ... */
};
```

Key points:
- Each process has its own `mm_struct` (unless it's a thread: threads share `mm_struct`)
- `pgd` points to the process's root page table: loaded into `satp` on context switch
- `mmap` list contains all the VMAs (code, data, heap, stack, mapped files)

### 14.3 struct vm_area_struct (VMA)

A VMA is a contiguous region of the virtual address space with the same
permissions and backed by the same source. Each VMA has:

```c
struct vm_area_struct {
    unsigned long   vm_start;       /* VMA start virtual address            */
    unsigned long   vm_end;         /* VMA end virtual address              */
    struct vm_area_struct *vm_next; /* next VMA in the process's list       */
    pgprot_t        vm_page_prot;   /* access permissions                   */
    unsigned long   vm_flags;       /* VM_READ, VM_WRITE, VM_EXEC, VM_SHARED*/
    struct mm_struct *vm_mm;        /* owning mm_struct                     */
    struct file     *vm_file;       /* mapped file (or NULL for anonymous)  */
    unsigned long   vm_pgoff;       /* offset within the file               */
    /* ... */
};
```

**VMA flags:**

| Flag | Meaning |
|------|---------|
| `VM_READ` | Pages can be read |
| `VM_WRITE` | Pages can be written |
| `VM_EXEC` | Pages can be executed |
| `VM_SHARED` | Pages are shared (not copy-on-write) |
| `VM_GROWSDOWN` | VMA grows downward (stack) |
| `VM_GROWSUP` | VMA grows upward (some architectures) |
| `VM_LOCKED` | Pages are locked in RAM (no swapping) |

### 14.4 Typical Process Address Space

Running `cat /proc/self/maps` shows the VMAs of the `cat` process:

```
00400000-00407000 r-xp  /bin/cat   ← code (text segment)
00606000-00607000 r--p  /bin/cat   ← read-only data
00607000-00608000 rw-p  /bin/cat   ← data segment (initialized)
01234000-01255000 rw-p  [heap]     ← heap (grows up)
7f8b00000000-7f8b001f4000 r-xp /lib/libc.so.6  ← shared library
...
7ffd00000000-7ffd00021000 rw-p [stack]  ← stack (grows down)
```

### 14.5 Demand Paging

Linux uses **demand paging**, pages are not loaded into RAM when a process starts.
They are loaded lazily on first access.

```
Process touches address X
        │
        ▼
  Page table entry has Present=0?
        │
     YES│ (page fault)
        ▼
  Kernel's page fault handler (do_page_fault)
        │
        ├── Is X in a valid VMA? NO → SIGSEGV (segfault)
        │
        └── YES → find backing (file or swap) → allocate page frame
                   → read data → update PTE with Present=1
                   → return to user process (retry the instruction)
```

### 14.6 Copy-On-Write (COW)

When a process calls `fork()`, Linux does **not** copy the parent's pages.
Instead:

1. Child gets a copy of the parent's page tables, but all writable pages
   are marked **read-only** in both parent and child.
2. When either process writes to a shared page:
   - Page fault fires
   - Kernel allocates a new page, copies the content
   - Updates the PTE to point to the new page with write permission
3. Result: pages that are never written are shared; only written pages get copied.

This is why `fork()` + `exec()` (launching a new program) is very cheap on Linux.

---

## 15. The syscall Layer: Talking to the OS

### 15.1 Design Philosophy

PageForge has one golden rule: **only `my_syscall.c` may include system headers**.
Every other file includes only `my_types.h` and our own headers.

```c
// my_syscall.c, the ONLY file with system includes
#include <sys/mman.h>    // for mmap(), munmap()
#include <unistd.h>      // for write(), _exit()
```

This mirrors the kernel philosophy: the kernel itself never calls "user-space
libraries." It uses raw system calls. In PageForge, `my_mmap` is our "raw
hardware interface."

### 15.2 my_mmap: Getting Raw Pages from the OS

```c
void *my_mmap(size_t size)
{
    void *ptr = mmap(
        NULL,                        // let OS choose address
        size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, // no file, private
        -1,                          // no file descriptor
        0                            // no offset
    );
    if (ptr == (void *)-1) return NULL;
    return ptr;
}
```

`MAP_ANONYMOUS` gives us zeroed memory from the OS, similar to how the Linux
kernel gets its initial memory from the boot allocator (which in turn got it
from the hardware memory map).

**Why anonymous memory is zeroed:** The kernel zero-fills pages before giving them
to user processes to prevent information leakage (you don't want to read another
process's old data).

### 15.3 my_write: Printf Without stdio

```c
void my_write(int fd, const void *buf, size_t len)
{
    long _r = write(fd, buf, len);  // fd=1 for stdout
    (void)_r;                       // suppress warn_unused_result
}
```

`my_printf` is built entirely on top of `my_write`:
- `my_putchar(c)` → writes 1 byte
- `my_puts(s)` → loops over string calling `my_putchar`
- `my_printf(fmt, ...)` → formats using `__builtin_va_list`, calls `my_write`

`my_printf` supports: `%s`, `%d`, `%u`, `%x`, `%p`, `%c`, `%%`.
No width specifiers (`%8d`), intentionally kept minimal.

### 15.4 Why No libc?

Standard C library functions (`printf`, `malloc`, `memset`) are NOT available
in the kernel. The kernel implements its own versions (`printk`, `kmalloc`,
`memset`). PageForge simulates this by not using any libc at all:

```c
// We NEVER include these in any file except my_syscall.c:
// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
```

This forces us to truly understand what these functions do, and implement them.

---

## 16. Building PageForge: Design Decisions

### 16.1 Project Structure

```
PageForge/
├── include/
│   ├── my_types.h      ← uint8_t, size_t, NULL, PAGE_SIZE, no system headers
│   ├── my_syscall.h    ← declarations for mmap/write/exit wrappers
│   ├── my_io.h         ← my_printf, my_puts, my_putchar, my_panic
│   ├── my_paging.h     ← page directory/table structs, flags, API
│   ├── my_buddy.h      ← buddy allocator structs, g_buddy global, API
│   ├── my_slab.h       ← slab cache struct, size classes, API
│   └── my_alloc.h      ← kmalloc-level API and header struct
├── src/
│   ├── my_syscall.c    ← ONLY file with <sys/mman.h>, <unistd.h>
│   ├── my_io.c         ← printf built from scratch using __builtin_va_list
│   ├── my_paging.c     ← 2-level page table simulation
│   ├── my_buddy.c      ← buddy allocator with bitmap + free lists
│   ├── my_slab.c       ← slab allocator with embedded free lists
│   ├── my_alloc.c      ← kmalloc/kfree/calloc/realloc with hidden header
│   └── main.c          ← 5-phase demo
└── Makefile
```

### 16.2 Layer-by-Layer Construction

**Step 1: Types (my_types.h)**

Before writing any code, define the types. No standard headers:

```c
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long      size_t;
typedef unsigned long      uintptr_t;
#define NULL ((void *)0)
#define PAGE_SIZE 4096u
```

**Step 2: Syscall wrapper (my_syscall.c)**

One file, one job: wrap the three OS calls we need.

**Step 3: I/O (my_io.c)**

Build `my_printf` without `stdio.h`. The secret: use GCC builtins for varargs:

```c
void my_printf(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    // parse format string, extract args with __builtin_va_arg
    __builtin_va_end(ap);
}
```

This works because `__builtin_va_list` is a GCC intrinsic, it doesn't need
`<stdarg.h>`.

**Step 4: Paging (my_paging.c)**

Pure software simulation. No privileged instructions, no `satp` writes, no
`sfence.vma`. Just arrays (three levels of page tables) and index arithmetic,
but the bit layout and the pointer-vs-leaf rule are the real Sv39 ones.

**Step 5: Buddy (my_buddy.c)**

Needs a memory arena. Gets it from `my_mmap()`. Manages it internally.
Uses the XOR trick for buddy finding.

**Step 6: Slab (my_slab.c)**

Needs pages. Gets them from the buddy allocator. Each slab = one page.
Header at page start. Embedded free list.

**Step 7: General Allocator (my_alloc.c)**

Sits on top of the slab (small) and buddy (large). Hides the header.
Implements calloc (kmalloc + zero) and realloc (copy + free).

### 16.3 The main.c Demo Flow

```
Phase 0: Raw memory from OS
  my_mmap(16 KB) → proves pages are readable/writable
  my_munmap() → pages returned to OS

Phase 1: Paging simulation
  my_pgd_create() → allocate page directory
  my_map_page() → map 3 virtual pages
  my_page_walk() → trace VA→PA translation (shows each step)
  my_page_walk() on unmapped VA → shows "page fault" message

Phase 2: Buddy allocator
  my_mmap(4 MB) → get arena from OS
  my_buddy_init() → set up buddy on the arena
  my_alloc_pages(0) → 4 KB
  my_alloc_pages(1) → 8 KB
  my_alloc_pages(3) → 32 KB
  my_buddy_dump() → shows free list state
  my_free_pages() × 3 → coalescence → back to one big block

Phase 3: Slab allocator
  (re-init buddy with fresh arena)
  my_slab_alloc(8), (16), (64), (256)
  my_slab_dump() → shows partial/full/empty counts
  my_slab_free() × 4 → objects return to caches

Phase 4: General allocator
  my_kmalloc(8), (100), (512), (4096)
  my_calloc(8, 4) → zeroed array
  my_realloc(arr, 64) → data preserved
  my_kfree() × all
  my_alloc_dump() → statistics
```

### 16.4 The Makefile

```makefile
ARCH ?= riscv64

ifeq ($(ARCH),riscv64)
  CROSS_COMPILE ?= riscv64-linux-gnu-
  QEMU          ?= qemu-riscv64
  ARCH_CFLAGS   ?= -march=rv64gc -mabi=lp64d
else
  CROSS_COMPILE ?=
  QEMU          ?=
  ARCH_CFLAGS   ?=
endif

CC     = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Wextra -Iinclude -O2 $(ARCH_CFLAGS)

SRC = src/my_syscall.c src/my_io.c src/my_paging.c \
      src/my_buddy.c src/my_slab.c src/my_alloc.c src/main.c

pageforge: $(SRC)
	$(CC) $(CFLAGS) -static -o $@ $^

run: pageforge
	$(QEMU) ./pageforge
```

**`ARCH`**: `riscv64` by default, which cross-compiles with
`riscv64-linux-gnu-gcc` and runs everything through `qemu-riscv64`. Setting
`ARCH=host` empties both variables, so the same rules build and run natively,
needed for valgrind, which has no riscv64 target.

**`-march=rv64gc -mabi=lp64d`**: the baseline RISC-V profile Linux distributions
target, the G ("general") extension set plus compressed instructions, with a
64-bit long/pointer ABI and hardware double-precision floats.

**`-static`**: Links all libraries statically into the binary. Required for QEMU
user-mode emulation, which doesn't set up the dynamic linker path, and doubly
so when cross-compiling, since the host has no rv64 shared libraries.

**`-O2`**: Optimization. Without this, GCC sometimes generates code that is
hard to reason about for learning purposes. With O2 the generated assembly
matches expectations.

---

*This document was written alongside the PageForge implementation.*
