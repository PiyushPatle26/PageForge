# Memory Management in Linux Kernel — A Complete Guide
### With PageForge: Building Linux MM From Scratch

> **Reference:** Robert Love, *Linux Kernel Development*, 3rd Edition (Addison-Wesley, 2010)
> Chapters 12 (Memory Management, p.231), 15 (Process Address Space, p.305)
>
> **Project:** PageForge — a complete, beginner-friendly reimplementation of the Linux
> kernel memory management stack in pure C, no libc, tested under QEMU.

---

## Table of Contents

1. [Why Memory Management Is Hard](#1-why-memory-management-is-hard)
2. [The Big Picture — Four Layers](#2-the-big-picture--four-layers)
3. [Physical Memory — Pages](#3-physical-memory--pages)
4. [Memory Zones](#4-memory-zones)
5. [How Linux Gets Pages — The Page Allocator](#5-how-linux-gets-pages--the-page-allocator)
6. [The Buddy System — How PageForge Implements It](#6-the-buddy-system--how-pageforge-implements-it)
7. [Virtual Memory and Paging](#7-virtual-memory-and-paging)
8. [How Page Tables Work — The Two-Level Walk](#8-how-page-tables-work--the-two-level-walk)
9. [PageForge's Paging Simulation](#9-pageforges-paging-simulation)
10. [The Slab Allocator — Linux's Object Cache](#10-the-slab-allocator--linuxs-object-cache)
11. [PageForge's Slab Implementation](#11-pageforges-slab-implementation)
12. [kmalloc — The General-Purpose Allocator](#12-kmalloc--the-general-purpose-allocator)
13. [PageForge's kmalloc/kfree/calloc/realloc](#13-pageforges-kmallocktreecallocrealloc)
14. [Process Address Space](#14-process-address-space)
15. [The syscall Layer — Talking to the OS](#15-the-syscall-layer--talking-to-the-os)
16. [Building PageForge — Design Decisions](#16-building-pageforge--design-decisions)
17. [Running PageForge Under QEMU](#17-running-pageforge-under-qemu)
18. [Concepts Quick Reference](#18-concepts-quick-reference)
19. [Glossary](#19-glossary)

---

## 1. Why Memory Management Is Hard

Memory management inside the kernel is not as easy as memory management outside
the kernel. Simply put, the kernel lacks luxuries enjoyed by user-space.

> *"Unlike user-space, the kernel is not always afforded the capability to easily allocate
> memory. For example, the kernel cannot easily deal with memory allocation errors, and
> the kernel often cannot sleep."*
> — Robert Love, Linux Kernel Development, p.231

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
- You need to be extremely careful — a mistake hangs the machine silently.

This is why the Linux kernel has a carefully layered memory management subsystem,
and why understanding it makes you a fundamentally better systems programmer.

---

## 2. The Big Picture — Four Layers

Linux kernel memory management is a stack of layers. Each layer depends on the one
below it:

```
User programs
     │
     ▼
┌─────────────────────────────────────────────┐
│  Layer 4: kmalloc / kfree / vmalloc          │  General byte-sized allocations
│  (mm/slab.c or mm/slub.c for kmalloc)        │
└────────────────────┬────────────────────────┘
                     │ uses
┌────────────────────▼────────────────────────┐
│  Layer 3: Slab Allocator                     │  Object caches, fixed-size chunks
│  (mm/slab.c, mm/slub.c, mm/slob.c)          │
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
│  (mm/memory.c, arch/x86/mm/)                │
└─────────────────────────────────────────────┘
```

PageForge implements all of these layers from scratch in pure C.

```
┌─────────────────────────────────────────────┐
│  my_alloc.c — my_kmalloc / my_kfree etc.     │  Layer 4
└────────────────────┬────────────────────────┘
                     │
┌────────────────────▼────────────────────────┐
│  my_slab.c  — my_slab_alloc / my_slab_free  │  Layer 3
└────────────────────┬────────────────────────┘
                     │
┌────────────────────▼────────────────────────┐
│  my_buddy.c — my_alloc_pages / my_free_pages │  Layer 2
└────────────────────┬────────────────────────┘
                     │
┌────────────────────▼────────────────────────┐
│  my_syscall.c — my_mmap (anonymous pages)   │  Layer 1
└─────────────────────────────────────────────┘
```

---

## 3. Physical Memory — Pages

### 3.1 What Is a Page?

The kernel treats physical pages as the **basic unit of memory management**.

> *"Although the processor's smallest addressable unit is a byte or a word, the memory
> management unit (MMU, the hardware that manages memory and performs virtual to
> physical address translations) typically deals in pages."*
> — Robert Love, p.231

Think of it this way:
- RAM is divided into fixed-size chunks called **pages** (or **page frames**).
- On most 32-bit architectures (x86), a page is **4096 bytes = 4 KB**.
- On 64-bit architectures it can be 8 KB or larger.
- 1 GB of RAM = 262,144 pages of 4 KB each.

The MMU — a piece of hardware inside the CPU — manages all translations from
virtual addresses to physical addresses using page-granularity tables.

### 3.2 struct page — The Kernel's Page Descriptor

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
all physical memory. About 0.5% of total RAM — a very reasonable trade-off.

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
The bitmap and free-lists are PageForge's simplified version of `mem_map` — the
kernel's global array of `struct page` structures.

---

## 4. Memory Zones

### 4.1 Why Zones Exist

Not all memory is equal. The kernel divides physical memory into **zones** because
of hardware constraints:

> *"Because of hardware limitations, the kernel cannot treat all pages as identical.
> Some pages, because of their physical address in memory, cannot be used for certain
> tasks."*
> — Robert Love, p.233

Linux has to deal with two hardware shortcomings:
1. Some hardware devices can only perform DMA (Direct Memory Access) to certain
   memory addresses (e.g., ISA devices on x86 can only reach the first 16 MB).
2. On 32-bit x86, the kernel can only directly address 896 MB of RAM, even if
   the machine has more (because of how the kernel's virtual address space works).

### 4.2 The Four Zones

Linux defines these zones in `<linux/mmzone.h>`:

| Zone | Description | Physical Range (x86-32) |
|------|-------------|------------------------|
| `ZONE_DMA` | Pages that can undergo DMA (for old ISA devices) | 0 – 16 MB |
| `ZONE_DMA32` | Like DMA but accessible only by 32-bit devices | 0 – 4 GB |
| `ZONE_NORMAL` | Normal, regularly mapped pages | 16 MB – 896 MB |
| `ZONE_HIGHMEM` | Pages NOT permanently mapped into the kernel address space | > 896 MB |

**Table 12.1 from the book — Zones on x86-32:**

| Zone | Description | Physical Memory |
|------|-------------|-----------------|
| `ZONE_DMA` | DMA-able pages | < 16 MB |
| `ZONE_NORMAL` | Normally addressable pages | 16–896 MB |
| `ZONE_HIGHMEM` | Dynamically mapped pages | > 896 MB |

**Why does ZONE_HIGHMEM exist on 32-bit?**

On a 32-bit system, virtual addresses are only 32 bits wide — that's 4 GB total
address space. Linux splits this: 3 GB for user space, 1 GB for the kernel.
But the kernel's 1 GB virtual address space can only directly map 896 MB of RAM.
Any physical RAM above 896 MB is "high memory" — it exists but is not permanently
mapped. The kernel must use special functions (`kmap()`) to temporarily map these
pages when needed.

On modern 64-bit systems, there is **no** `ZONE_HIGHMEM` because 64-bit virtual
addresses are far more than enough to map all physical RAM.

### 4.3 struct zone — Representing a Zone

Each zone is represented by `struct zone` in `<linux/mmzone.h>`:

```c
struct zone {
    unsigned long    watermark[NR_WMARK]; /* min/low/high watermarks */
    struct free_area free_area[MAX_ORDER];/* buddy free lists per order */
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

The `free_area` array is the heart of the buddy allocator — it holds one free
list per order (order 0 = 1 page, order 1 = 2 pages, ... order 10 = 1024 pages).

### 4.4 PageForge's Simplified Model

PageForge does not implement multiple zones — it operates with a single flat
4 MB arena. In a real kernel you would have separate zones, each with its own
`free_area[]`. PageForge simplifies this:

```c
// my_buddy.h
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

This directly mirrors `struct zone`'s `free_area[]` array.

---

## 5. How Linux Gets Pages — The Page Allocator

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

### 5.2 GFP Flags — Controlling Allocation Behavior

Every allocation call takes a `gfp_mask` (GFP = "Get Free Page") parameter.
These flags tell the allocator:
- Whether it is allowed to sleep waiting for memory
- Which memory zone to allocate from
- What the memory will be used for

**Action Modifiers** (Table 12.3 from the book):

| Flag | Meaning |
|------|---------|
| `__GFP_WAIT` | The allocator can sleep |
| `__GFP_HIGH` | The allocator can access emergency pools |
| `__GFP_IO` | The allocator can start disk I/O |
| `__GFP_FS` | The allocator can start filesystem I/O |
| `__GFP_COLD` | Use cache-cold pages |
| `__GFP_NOWARN` | Don't print failure warnings |
| `__GFP_REPEAT` | Retry if allocation fails |
| `__GFP_NOFAIL` | Repeat until allocation succeeds |
| `__GFP_NORETRY` | Never retry |

**Zone Modifiers** (Table 12.4):

| Flag | Meaning |
|------|---------|
| `__GFP_DMA` | Allocate from `ZONE_DMA` only |
| `__GFP_DMA32` | Allocate from `ZONE_DMA32` only |
| `__GFP_HIGHMEM` | Allocate from `ZONE_HIGHMEM` or `ZONE_NORMAL` |

**Type Flags** (Table 12.5 — the ones you actually use):

| Flag | When to Use |
|------|-------------|
| `GFP_ATOMIC` | Interrupt handlers, softirqs, tasklets — must not sleep |
| `GFP_KERNEL` | Normal process context — can sleep, recommended default |
| `GFP_USER` | Allocating memory for user-space processes |
| `GFP_DMA` | Need DMA-able memory (device drivers) |
| `GFP_NOIO` | Block I/O code — can block but not start disk I/O |
| `GFP_NOFS` | Filesystem code — can block and start disk I/O, not FS I/O |

**When to use which flag (Table 12.7):**

| Situation | Use This Flag |
|-----------|--------------|
| Process context, can sleep | `GFP_KERNEL` |
| Process context, cannot sleep | `GFP_ATOMIC` |
| Interrupt handler | `GFP_ATOMIC` |
| Softirq or tasklet | `GFP_ATOMIC` |
| Need DMA memory, can sleep | `GFP_DMA | GFP_KERNEL` |
| Need DMA memory, cannot sleep | `GFP_DMA | GFP_ATOMIC` |

**PageForge does not implement GFP flags** — our `my_alloc_pages(order)` has
no flags parameter. This is intentional: we are a user-space simulation, there
are no interrupt contexts, and all allocations can trivially "sleep".

---

## 6. The Buddy System — How PageForge Implements It

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
This is an "intrusive linked list" — no extra memory needed for list nodes.
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

### 6.5 Allocation — Splitting

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

### 6.6 Freeing — Coalescing

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

### 7.1 Why Virtual Memory Exists

Without virtual memory, every program would need to know where in physical RAM
it would run. Programs would conflict, there would be no isolation, and it would
be impossible to run more programs than RAM. Virtual memory solves this:

1. **Isolation**: Each process has its own virtual address space. A bug in one
   process cannot corrupt another's memory.
2. **Larger address space**: Programs can use up to 3 GB (on 32-bit) or
   128 TB (on 64-bit) of virtual memory, regardless of actual RAM.
3. **Sharing**: Multiple processes can map the same physical page (e.g., a
   shared library) to different virtual addresses.
4. **Swap**: Pages that haven't been used recently can be swapped to disk,
   freeing physical RAM.

### 7.2 The MMU — Memory Management Unit

The MMU is hardware built into the CPU (since the 80386 on x86). Its job:

```
CPU generates virtual address
        │
        ▼
  ┌─────────────┐
  │     MMU     │  Walks the page table using the CR3 register
  └──────┬──────┘
         │ translates to
         ▼
  Physical address sent to RAM
```

The MMU maintains a **Translation Lookaside Buffer (TLB)** — a small, fast cache
of recent VA→PA translations. A TLB miss causes a page table walk (slow). TLB
flushes happen on context switches and large memory operations.

### 7.3 Virtual Address Layout on x86-32

On a 32-bit x86 Linux system:

```
0xFFFFFFFF  ────────────────────────────────────
            │  Kernel space (1 GB)              │  (process cannot access)
0xC0000000  ────────────────────────────────────
            │                                   │
            │  User space (3 GB)                │
            │                                   │
            │  Stack (grows downward)            │
            │  ......                           │
            │  Memory-mapped files              │
            │  ......                           │
            │  Heap (grows upward)              │
            │  BSS (uninitialized data)         │
            │  Data (initialized data)          │
            │  Text (code)                      │
0x00000000  ────────────────────────────────────
```

The kernel maps itself into the top 1 GB of every process's virtual address space
(above 0xC0000000). This means the kernel is always "there" in the address space —
system calls don't need to change the page table, just the privilege level.

---

## 8. How Page Tables Work — The Two-Level Walk

### 8.1 The Problem

A 32-bit address space has 4 GB = 4,294,967,296 addresses.
If we tracked every possible 4 KB page: 4GB / 4KB = 1,048,576 entries.
At 4 bytes per entry, a flat page table would be 4 MB **per process**.
With 1000 processes: 4 GB just for page tables!

The solution: a **hierarchical page table** — a tree structure where only the
pages that are actually used get allocated.

### 8.2 Two-Level Page Table (x86-32)

Linux on x86-32 uses a two-level page table:

```
Virtual Address (32 bits)
┌─────────────┬─────────────┬──────────────┐
│  PD Index   │  PT Index   │    Offset    │
│  [31..22]   │  [21..12]   │  [11..0]    │
│  10 bits    │  10 bits    │  12 bits    │
└─────────────┴─────────────┴──────────────┘
     │               │              │
     │               │              └──► byte offset within page (0-4095)
     │               └─────────────────► index into Page Table (1024 entries)
     └─────────────────────────────────► index into Page Directory (1024 entries)
```

**Step by step:**

```
1. CR3 register → base address of Page Directory (PGD)

2. VA[31..22] (10 bits) → index into PGD
   PGD[PD_index] → Page Directory Entry (PDE)
   PDE contains: physical address of a Page Table + flags

3. VA[21..12] (10 bits) → index into Page Table
   PT[PT_index] → Page Table Entry (PTE)
   PTE contains: physical address of a 4 KB page + flags

4. VA[11..0] (12 bits) → byte offset within that 4 KB page
   Physical Address = PTE's page address + offset
```

**Size math:**
- Page Directory: 1024 entries × 4 bytes = 4 KB (exactly one page!)
- Each Page Table: 1024 entries × 4 bytes = 4 KB (exactly one page!)
- Maximum Page Tables per process: 1024
- Total physical pages mappable: 1024 × 1024 = 1,048,576 = 4 GB ✓

**Page Table Entry flags:**

```
PTE (32 bits):
┌──────────────────────────┬──┬──┬──┬──┬──┐
│  Physical Page Number    │ D│ A│ U│ W│ P│
│  [31..12] (20 bits)      │  │  │  │  │  │
└──────────────────────────┴──┴──┴──┴──┴──┘
  P  = Present (1 = page is in RAM, 0 = page fault!)
  W  = Write enable (1 = read/write, 0 = read-only)
  U  = User access (1 = user can access, 0 = kernel only)
  A  = Accessed (hardware sets this on any access)
  D  = Dirty (hardware sets this on write)
```

A page fault occurs when the CPU tries to access a page whose Present bit is 0.
The kernel's page fault handler (`do_page_fault()` on x86) either:
- Loads the page from disk (swap or memory-mapped file) — **demand paging**
- Sends `SIGSEGV` to the process (invalid access) — **segfault**

### 8.3 Modern x86-64: Four-Level Page Tables

On 64-bit x86, Linux uses four levels:

```
Virtual Address (48 bits used out of 64)
┌──────────┬──────────┬──────────┬──────────┬──────────┐
│  PGD idx │  PUD idx │  PMD idx │  PTE idx │  Offset  │
│  [47..39]│  [38..30]│  [29..21]│  [20..12]│  [11..0] │
└──────────┴──────────┴──────────┴──────────┴──────────┘
     9 bits     9 bits     9 bits     9 bits    12 bits
```

With 48-bit virtual addresses: 2^48 = 256 TB of virtual address space per process.

PageForge simulates the simpler 2-level scheme (x86-32 style) — perfectly
adequate for teaching the concepts.

---

## 9. PageForge's Paging Simulation

### 9.1 Design

```c
// my_paging.h

#define PD_SIZE    1024          // Page Directory: 1024 entries
#define PT_SIZE    1024          // Each Page Table: 1024 entries

#define PD_INDEX(va)   (((va) >> 22) & 0x3FF)  // bits [31..22]
#define PT_INDEX(va)   (((va) >> 12) & 0x3FF)  // bits [21..12]
#define PG_OFFSET(va)  ((va) & 0xFFF)           // bits [11..0]

// Page table entry flags
#define PTE_PRESENT   (1u << 0)
#define PTE_WRITE     (1u << 1)
#define PTE_USER      (1u << 2)
#define PTE_ACCESSED  (1u << 5)
#define PTE_DIRTY     (1u << 6)

// A page table: 1024 entries, each a uint32_t
typedef uint32_t my_page_table_t[PT_SIZE];

// A page directory: 1024 entries, each is (ptr to PT | flags) or 0
typedef uint32_t my_page_dir_t[PD_SIZE];
```

### 9.2 Creating a Page Directory

```c
my_page_dir_t *my_pgd_create(void)
{
    // Allocate one page for the Page Directory
    my_page_dir_t *pgd = (my_page_dir_t *)my_mmap(PAGE_SIZE);
    // Zero it out (all entries = 0 = not present)
    for (int i = 0; i < PD_SIZE; i++) (*pgd)[i] = 0;
    return pgd;
}
```

### 9.3 Mapping a Virtual Page to a Physical Frame

```c
void my_map_page(my_page_dir_t *pgd,
                 uint32_t va,   // virtual address
                 uint32_t pa,   // physical address
                 uint32_t flags)
{
    uint32_t pd_idx = PD_INDEX(va);
    uint32_t pt_idx = PT_INDEX(va);

    // Does a page table exist for this PD entry?
    if (!((*pgd)[pd_idx] & PTE_PRESENT)) {
        // Allocate a new page table
        my_page_table_t *pt = (my_page_table_t *)my_mmap(PAGE_SIZE);
        for (int i = 0; i < PT_SIZE; i++) (*pt)[i] = 0;
        // Store its address (aligned, so lower 12 bits are free for flags)
        (*pgd)[pd_idx] = (uint32_t)(uintptr_t)pt | PTE_PRESENT | PTE_WRITE;
    }

    // Get the page table
    my_page_table_t *pt = (my_page_table_t *)
                          (uintptr_t)((*pgd)[pd_idx] & ~0xFFFu);

    // Write the PTE: physical page base address + flags
    (*pt)[pt_idx] = (pa & ~0xFFFu) | flags | PTE_PRESENT;
}
```

### 9.4 Walking a Page Table (Simulating the MMU)

```c
uint32_t my_virt_to_phys(my_page_dir_t *pgd, uint32_t va)
{
    uint32_t pd_idx = PD_INDEX(va);
    uint32_t pt_idx = PT_INDEX(va);
    uint32_t offset = PG_OFFSET(va);

    // Step 1: Look up Page Directory Entry
    uint32_t pde = (*pgd)[pd_idx];
    if (!(pde & PTE_PRESENT)) return 0;   // Page fault: PD entry not present

    // Step 2: Get Page Table, look up PTE
    my_page_table_t *pt = (my_page_table_t *)(uintptr_t)(pde & ~0xFFFu);
    uint32_t pte = (*pt)[pt_idx];
    if (!(pte & PTE_PRESENT)) return 0;   // Page fault: PT entry not present

    // Step 3: Combine physical page base with offset
    return (pte & ~0xFFFu) | offset;
}
```

### 9.5 Sample Page Walk Output

When `demo_paging()` runs in PageForge, you see output like:

```
  page_walk(VA=0x00001ABC):
    PD[0] → PT base=0x... (present, write)
    PT[1] → PA base=0x00100000 (present, write)
    offset = 0xABC
    Physical address = 0x00100ABC
```

This is exactly what the hardware MMU does — just done in software so you can
observe each step.

---

## 10. The Slab Allocator — Linux's Object Cache

### 10.1 The Problem Slab Solves

The page allocator gives you whole pages (4 KB minimum). But kernel code constantly
needs small objects — a `task_struct` (process descriptor) might be 1.7 KB, an
`inode` might be 0.5 KB. If you allocated a whole page for each one, you would
waste enormous amounts of memory.

Enter free lists:

> *"To facilitate frequent allocations and deallocations of data, programmers often
> introduce free lists. A free list contains a block of available, already allocated,
> data structures. When code requires a new instance of a data structure, it can grab
> one of the structures off the free list."*
> — Robert Love, p.245

But ad-hoc free lists have a problem: the kernel has no global control. When memory
is low, there's no way to tell every random free list to shrink.

**The slab allocator** consolidates all these free lists into one managed system.

### 10.2 Slab Design Principles

> *"The slab layer attempts to leverage several basic tenets:
> - Frequently used data structures tend to be allocated and freed often, so cache them.
> - Frequent allocation and deallocation can result in memory fragmentation. To prevent
>   this, the cached free lists are arranged contiguously.
> - The free list provides improved performance during frequent allocation and
>   deallocation because a freed object can be immediately returned to the next
>   allocation.
> - If the allocator is aware of concepts such as object size, page size, and total
>   cache size, it can make more intelligent decisions."*
> — Robert Love, p.246

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

**Real example from kernel — process descriptor cache:**

```c
// kernel/fork.c — create the task_struct cache at boot
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
    uint32_t       magic;      // SLAB_MAGIC — detects corrupt/wrong pointers
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
given object — it masks off the page offset bits.

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
        my_panic("my_slab_free: bad magic — double free or corrupt pointer");

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

## 12. kmalloc — The General-Purpose Allocator

### 12.1 What kmalloc Is

```c
// Declared in <linux/slab.h>
void *kmalloc(size_t size, gfp_t flags);
```

> *"The kmalloc() function's operation is similar to that of user-space's familiar
> malloc() routine, with the exception of the additional flags parameter. The
> kmalloc() function is a simple interface for obtaining kernel memory in
> byte-sized chunks. If you need whole pages, the previously discussed interfaces
> might be a better choice. For most kernel allocations, however, kmalloc() is
> the preferred interface."*
> — Robert Love, p.238

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

> *"Do not call this function on memory not previously allocated with kmalloc(), or
> on memory that has already been freed. Doing so is a bug, resulting in bad behavior
> such as freeing memory belonging to another part of the kernel."*
> — Robert Love, p.243

`kfree(NULL)` is safe and does nothing.

**Example:**

```c
char *buf;
buf = kmalloc(BUF_SIZE, GFP_ATOMIC);
if (!buf) { /* handle error */ }

/* ... use buf ... */

kfree(buf);
```

### 12.3 vmalloc — Virtually Contiguous

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
#define ALLOC_MAGIC       0xA110C8EDu  // "ALLOC8ED" — alloc-ated
#define LARGE_THRESHOLD   1024

typedef struct {
    uint32_t magic;       // ALLOC_MAGIC — detect corrupt/double-free
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
        my_panic("my_kfree: bad magic — double free or corrupt pointer");

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

Each process in Linux has its own **virtual address space** — a range of virtual
addresses it can use. This virtual address space is divided into regions called
**VMAs (Virtual Memory Areas)**.

> *"The kernel represents a process's address space with a data structure called the
> memory descriptor. This structure contains all the information related to the
> process address space. The memory descriptor is represented by struct mm_struct
> and is defined in <linux/mm_types.h>."*
> — Robert Love, Chapter 15

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
- Each process has its own `mm_struct` (unless it's a thread — threads share `mm_struct`)
- `pgd` points to the process's page directory — loaded into CR3 on context switch
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

Linux uses **demand paging** — pages are not loaded into RAM when a process starts.
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

## 15. The syscall Layer — Talking to the OS

### 15.1 Design Philosophy

PageForge has one golden rule: **only `my_syscall.c` may include system headers**.
Every other file includes only `my_types.h` and our own headers.

```c
// my_syscall.c — the ONLY file with system includes
#include <sys/mman.h>    // for mmap(), munmap()
#include <unistd.h>      // for write(), _exit()
```

This mirrors the kernel philosophy: the kernel itself never calls "user-space
libraries." It uses raw system calls. In PageForge, `my_mmap` is our "raw
hardware interface."

### 15.2 my_mmap — Getting Raw Pages from the OS

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

### 15.3 my_write — Printf Without stdio

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
No width specifiers (`%8d`) — intentionally kept minimal.

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

This forces us to truly understand what these functions do — and implement them.

---

## 16. Building PageForge — Design Decisions

### 16.1 Project Structure

```
PageForge/
├── include/
│   ├── my_types.h      ← uint8_t, size_t, NULL, PAGE_SIZE — no system headers
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

This works because `__builtin_va_list` is a GCC intrinsic — it doesn't need
`<stdarg.h>`.

**Step 4: Paging (my_paging.c)**

Pure software simulation. No arch-specific code. No CR3 register writes.
Just arrays (page directory and page tables) and index arithmetic.

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
CC     = gcc
CFLAGS = -Wall -Wextra -Iinclude -O2 -static

SRC = src/my_syscall.c src/my_io.c src/my_paging.c \
      src/my_buddy.c src/my_slab.c src/my_alloc.c src/main.c

pageforge: $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

run: pageforge
	./pageforge

qemu: pageforge
	@if command -v qemu-x86_64 >/dev/null 2>&1; then \
	    qemu-x86_64 ./pageforge; \
	elif command -v qemu-x86_64-static >/dev/null 2>&1; then \
	    qemu-x86_64-static ./pageforge; \
	else \
	    ./pageforge; \
	fi
```

**`-static`**: Links all libraries statically into the binary. Required for QEMU
user-mode emulation, which doesn't set up the dynamic linker path.

**`-O2`**: Optimization. Without this, GCC sometimes generates code that is
hard to reason about for learning purposes. With O2 the generated assembly
matches expectations.

---

## 17. Running PageForge Under QEMU

### 17.1 What Is QEMU User-Mode?

QEMU has two modes:

1. **Full system emulation**: emulates an entire computer (CPU, RAM, devices,
   BIOS). Used to run a complete OS image.

2. **User-mode emulation** (`qemu-x86_64`): emulates only the CPU instruction
   set. Linux system calls are forwarded to the host kernel. No BIOS, no boot
   process.

PageForge uses **user-mode emulation**. This means:
- QEMU translates x86_64 instructions to host instructions
- When the binary calls `mmap()`, QEMU forwards it to the Linux kernel
- Output from `write()` appears on your terminal
- No need for a disk image or bootloader

### 17.2 Why This Is Relevant

In a real kernel scenario, you would run your allocator code on bare metal or
inside a QEMU full-system image with a bootloader. User-mode QEMU is a quick
way to test Linux binaries with CPU emulation turned on.

This is exactly how Linux kernel developers sometimes test architecture-specific
code — they use QEMU to emulate ARM or RISC-V and run their code.

### 17.3 Installation

```bash
# Install QEMU user-mode emulators
sudo apt install qemu-user          # dynamic binaries
sudo apt install qemu-user-static   # static binaries (what we need)
```

### 17.4 Building and Running

```bash
cd /path/to/PageForge
make          # builds ./pageforge (static binary)
make run      # runs ./pageforge directly
make qemu     # runs under QEMU user-mode emulation
```

The `run_qemu.sh` script does:
```bash
make clean
make
make qemu
```

### 17.5 Expected Output

```
 ██████╗  █████╗  ██████╗ ███████╗
 ██╔══██╗██╔══██╗██╔════╝ ██╔════╝
 ██████╔╝███████║██║  ███╗█████╗
 ██╔═══╝ ██╔══██║██║   ██║██╔══╝
 ██║     ██║  ██║╚██████╔╝███████╗
 ╚═╝     ╚═╝  ╚═╝ ╚═════╝ ╚══════╝
  F O R G E  —  Linux MM from scratch

  No libc. No malloc. No printf.
  Only mmap() + write() from the OS.

============================================================
 Phase 0 — Raw memory from OS  (my_mmap)
============================================================
  my_mmap(16 KB) → 0x7f...
  bytes[0..1] = 0xca 0xfe  (writable ✓)
  my_munmap() → pages returned to OS

============================================================
 Phase 1 — Page Table Simulation  (my_paging)
============================================================
  ...page walk output...

============================================================
 Phase 2 — Buddy Page Allocator  (my_buddy)
============================================================
[buddy] init: base=0x...  pages=1024  free=1024

--- Buddy Allocator ---
  total pages : 1024  (4096 KB)
  free  pages : 1024  (4096 KB)
  used  pages : 0  (0 KB)
  order 10 (4096 KB each): 1 block(s)
-----------------------

  [alloc] order 0 → 4 KB
          returned 0x...
  ...

[free]  freeing all three blocks — expect coalescing

--- Buddy Allocator ---
  total pages : 1024  (4096 KB)
  free  pages : 1024  (4096 KB)
  ...
  order 10 (4096 KB each): 1 block(s)  ← fully coalesced!
-----------------------
```

---

## 18. Concepts Quick Reference

### 18.1 Linux MM vs PageForge Mapping

| Linux Kernel | PageForge | File |
|-------------|-----------|------|
| `mmap(MAP_ANONYMOUS)` | `my_mmap()` | `my_syscall.c` |
| `struct page`, `mem_map` | bitmap in `my_buddy_t` | `my_buddy.c` |
| `ZONE_DMA/NORMAL/HIGHMEM` | single flat arena | `my_buddy.c` |
| `alloc_pages(order)` | `my_alloc_pages(order)` | `my_buddy.c` |
| `free_pages(ptr, order)` | `my_free_pages(ptr, order)` | `my_buddy.c` |
| `struct kmem_cache` | `my_kmem_cache_t` | `my_slab.c` |
| `struct slab` | `my_slab_t` | `my_slab.c` |
| `kmem_cache_alloc()` | `my_slab_alloc(size)` | `my_slab.c` |
| `kmem_cache_free()` | `my_slab_free(ptr)` | `my_slab.c` |
| `kmalloc(size, GFP_KERNEL)` | `my_kmalloc(size)` | `my_alloc.c` |
| `kfree(ptr)` | `my_kfree(ptr)` | `my_alloc.c` |
| `kzalloc()` | `my_calloc(1, size)` | `my_alloc.c` |
| `krealloc()` | `my_realloc(ptr, size)` | `my_alloc.c` |
| `struct mm_struct` / VMAs | (not implemented) | — |
| `pgd_t`, `pmd_t`, `pte_t` | `my_page_dir_t`, `my_page_table_t` | `my_paging.c` |
| CR3 register | `my_page_dir_t *pgd` variable | `my_paging.c` |
| MMU hardware page walk | `my_page_walk()` in software | `my_paging.c` |
| Page fault handler | `my_page_walk()` prints "fault" | `my_paging.c` |

### 18.2 Key Formulas

```
Buddy formula:       buddy_idx = page_idx ^ (1 << order)
Page-align a ptr:    page_base = (uintptr_t)ptr & ~(PAGE_SIZE - 1)
Page index from ptr: page_idx  = ((uintptr_t)ptr - base) / PAGE_SIZE
Bitmap byte:         byte      = page_idx / 8
Bitmap bit:          bit       = page_idx % 8
PD index from VA:    pd_idx    = (va >> 22) & 0x3FF
PT index from VA:    pt_idx    = (va >> 12) & 0x3FF
Page offset from VA: offset    = va & 0xFFF
Physical address:    pa        = (pte & ~0xFFF) | offset
```

### 18.3 Sizes and Limits

```
PAGE_SIZE                = 4096 bytes = 4 KB
MAX_ORDER                = 10 (blocks of 1 to 1024 pages)
Largest buddy block      = 2^10 pages = 4 MB
PageForge arena          = 1024 pages = 4 MB
Slab size classes        = 8, 16, 32, 64, 128, 256, 512, 1024 bytes
LARGE_THRESHOLD          = 1024 bytes (above this: buddy, not slab)
PD_SIZE                  = 1024 entries
PT_SIZE                  = 1024 entries
Max VA coverage          = 1024 × 1024 × 4 KB = 4 GB (32-bit full space)
struct page size (Linux) ≈ 40 bytes
mem_map for 4 GB @ 4KB pages = 40 × 262,144 = 10 MB
```

### 18.4 Error Detection in PageForge

| Magic Number | Value | Purpose |
|-------------|-------|---------|
| `SLAB_MAGIC` | `0x51AB1234` | Validates slab headers on free |
| `ALLOC_MAGIC` | `0xA110C8ED` | Validates kmalloc headers on free |
| Poison value | `0xDEADDEAD` | Written to freed user data |

If you pass a wrong pointer to `my_kfree()` or `my_slab_free()`, the magic
number check will catch it and call `my_panic()` instead of silently corrupting
memory.

---

## 19. Glossary

| Term | Definition |
|------|-----------|
| **Arena** | A large contiguous region of raw memory given to an allocator to manage. PageForge gets a 4 MB arena via `my_mmap()`. |
| **Buddy** | A partner block of the same size that can be merged with a freed block. Two buddies together form a block at the next order. |
| **Cache (slab)** | A slab allocator cache manages objects of one specific size. Example: `task_struct_cachep` in Linux, `g_slab_caches[i]` in PageForge. |
| **Coalescing** | Merging a freed block with its free buddy to form a larger block. Reduces fragmentation. |
| **CR3** | x86 control register that holds the physical address of the Page Global Directory. Loaded on every context switch. |
| **DMA** | Direct Memory Access — hardware that transfers data between devices and memory without CPU involvement. Requires physically contiguous pages. |
| **Embedded free list** | A free list where the "next" pointer lives inside the free object's own memory (no separate node allocation needed). Used in slab allocator. |
| **GFP flags** | "Get Free Page" flags passed to kernel allocation functions. Control whether the allocator can sleep, do I/O, and which zone to allocate from. |
| **High memory** | Physical RAM above 896 MB on 32-bit x86. Not permanently mapped into the kernel's virtual address space. |
| **Intrusive list** | A linked list where the `next` pointer is embedded in the data structure itself (no wrapper node). Used extensively in Linux kernel. |
| **kmalloc** | The kernel's general-purpose byte-level allocator. Uses the slab layer for small allocations. |
| **kswapd** | Kernel swap daemon. Wakes when memory is low (below `low` watermark) and reclaims pages by swapping or dropping page cache. |
| **MMU** | Memory Management Unit. Hardware that translates virtual addresses to physical addresses using page tables. |
| **Order** | Buddy allocator size unit. Order N = 2^N pages = 2^N × 4 KB. |
| **Page** | The basic unit of memory management. Usually 4 KB. Both hardware (MMU) and software (kernel) work with page-granularity. |
| **Page fault** | Exception raised by the MMU when accessing a page whose Present bit is 0. Handled by the kernel to load the page from disk or signal the process. |
| **Page frame** | A physical page — a 4 KB chunk of actual RAM. Distinguished from "page" which can mean virtual or physical. |
| **PDE** | Page Directory Entry. One entry in the Page Directory. Points to a Page Table. |
| **PGD** | Page Global Directory — the top-level page table structure. On x86-32, this is the only level above PTE. On x86-64, it's the first of four levels. |
| **PTE** | Page Table Entry. One entry in a Page Table. Contains the physical page number + flags (Present, Write, User, etc.). |
| **Slab** | A contiguous range of pages (usually one page) carved into fixed-size objects. Each object is tracked in an embedded free list. |
| **Splitting** | Dividing a larger buddy block into two smaller ones to satisfy an allocation request. Opposite of coalescing. |
| **struct page** | The kernel's per-page descriptor. One instance for every physical page in the system. Stored in `mem_map[]` array. |
| **TLB** | Translation Lookaside Buffer. A cache of recent VA→PA translations inside the MMU. A TLB miss causes a page table walk. |
| **VMA** | Virtual Memory Area (`struct vm_area_struct`). A contiguous region of a process's virtual address space with uniform permissions and backing. |
| **vmalloc** | Kernel allocator for large regions. Pages are virtually contiguous but NOT physically contiguous. Used for loading kernel modules. |
| **ZONE_DMA** | Memory zone covering 0–16 MB on x86-32. Required for ISA DMA devices. |
| **ZONE_HIGHMEM** | Memory zone above 896 MB on 32-bit x86. Not permanently mapped into the kernel. |
| **ZONE_NORMAL** | Memory zone from 16 MB to 896 MB on x86-32. Normally mapped, preferred for most kernel allocations. |

---

## Appendix A: File-by-File Code Summary

### my_types.h — Zero-dependency types

Defines all integer types from scratch. No standard headers. This is the
foundation everything else builds on.

### my_syscall.h / my_syscall.c — OS interface

The **only** files allowed to include `<sys/mman.h>` and `<unistd.h>`.
Wraps `mmap()`, `munmap()`, `write()`, `_exit()`.

**Pitfall fixed**: GCC has a `warn_unused_result` attribute on `write()`.
The fix: `long _r = write(fd, buf, len); (void)_r;`

### my_io.h / my_io.c — printf from scratch

Implements `my_printf` using `__builtin_va_list` (GCC intrinsic, no `<stdarg.h>`).

**Limitation**: Does not support width/precision specifiers (`%8d`, `%-4s`).
This is intentional simplification — adding them would make the format parser
much more complex without adding educational value.

### my_paging.h / my_paging.c — Page table simulation

2-level page table, pure software. 10-bit PD index, 10-bit PT index, 12-bit
offset. Demonstrates every step the hardware MMU performs.

### my_buddy.h / my_buddy.c — Buddy page allocator

One global `my_buddy_t g_buddy`. Uses XOR formula for buddy finding.
Bitmap for allocation tracking. Embedded next pointer in free blocks.

### my_slab.h / my_slab.c — Slab object cache

8 size classes (8–1024 bytes). Slab header at page-aligned start.
Embedded free list. Three lists per cache (partial/full/empty).
Magic number validates frees.

### my_alloc.h / my_alloc.c — kmalloc level

Hidden header before returned pointer. Routes to slab (≤ 1024 bytes) or
buddy (> 1024 bytes). Poisons freed memory. Implements calloc and realloc.

### main.c — The demo

Five phases demonstrating each layer in order, with dumps showing the internal
state at each step.

---

## Appendix B: Common Bugs and How to Detect Them

### Double Free

```
my_kfree(ptr);
my_kfree(ptr);  // BUG: magic was cleared on first free
```

Detection: `hdr->magic != ALLOC_MAGIC` → `my_panic()` fires.
In Linux: `SLAB_POISON` fills freed objects; if the poison is wrong on re-use,
something wrote to freed memory.

### Wrong Pointer to Free

```
char buf[100];
my_kfree(buf);  // BUG: buf was not allocated by my_kmalloc
```

Detection: magic number at `buf - sizeof(header)` will not be `ALLOC_MAGIC`.

### Use After Free

```
int *p = my_kmalloc(sizeof(int));
my_kfree(p);
*p = 42;   // BUG: p points to freed memory
```

Detection: The memory was poisoned with `0xDEADDEAD` on free. Reading it gives
a tell-tale garbage value. In Linux: `SLAB_POISON = 0xa5a5a5a5`.

### Buffer Overflow

```
char *buf = my_kmalloc(8);
buf[8] = 'x';   // BUG: writes one byte past the end
```

This is not detected by PageForge. In Linux: `SLAB_RED_ZONE` places known
patterns before and after the object and checks them on free.

---

## Appendix C: How the Real Linux Kernel Differs

PageForge deliberately simplifies many things. Here's what the real kernel adds:

| Feature | Linux | PageForge |
|---------|-------|-----------|
| Multiple memory zones | ZONE_DMA, ZONE_NORMAL, ZONE_HIGHMEM | Single flat arena |
| NUMA support | Per-node buddy allocator, NUMA-aware slab | None |
| SMP/multi-core | Per-CPU free lists in slab (magazine layer) | None (single-threaded) |
| GFP flags | ~15 flags controlling behavior | None |
| Vmalloc | Virtually contiguous large allocs | None |
| Page reclaim | kswapd, shrinkers, LRU lists | None |
| Memory compaction | Defragment physical memory | None |
| Huge pages | 2 MB / 1 GB pages via huge page support | None |
| slab vs slub vs slob | Three allocator implementations, selectable | One simplified implementation |
| Page cache | Cache file data in page-sized chunks | None |
| Memory-mapped files | `mmap()` system call backed by files | Only anonymous mmap |
| Copy-on-write | Fork optimization | None |
| Demand paging | Load pages only on first access | None |
| Swap | Write pages to disk when RAM is full | None |

Despite all these simplifications, PageForge gets the fundamental algorithms
right: buddy coalescing, slab embedded free lists, and two-level page table walks
are all implemented faithfully.

---

*This document was written alongside the PageForge implementation.*
*Primary reference: Robert Love, Linux Kernel Development, 3rd Edition.*
*Chapter 12: Memory Management (p.231–260)*
*Chapter 15: The Process Address Space (p.305–323)*
