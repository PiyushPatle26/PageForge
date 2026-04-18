#ifndef MY_PAGING_H
#define MY_PAGING_H

/*
 * my_paging.h — software simulation of Linux's two-level page table.
 *
 * Real Linux on x86-64 uses 4 levels (PGD→PUD→PMD→PTE).
 * We simulate a simpler 2-level version so the concept is clear:
 *
 *   Virtual address (32-bit model):
 *   ┌──────────┬──────────┬────────────┐
 *   │ PD index │ PT index │   Offset   │
 *   │ 10 bits  │ 10 bits  │  12 bits   │
 *   └──────────┴──────────┴────────────┘
 *       ↓           ↓           ↓
 *   Page Dir → Page Table → Physical Page + Offset
 *
 * This is purely educational — no hardware registers are touched.
 */

#include "my_types.h"

#define PD_SIZE      1024    /* page directory: 1024 entries */
#define PT_SIZE      1024    /* page table: 1024 entries     */
#define PD_SHIFT     22      /* upper 10 bits of 32-bit VA   */
#define PT_SHIFT     12      /* middle 10 bits               */
#define OFFSET_MASK  0xFFF   /* lower 12 bits = 4096 bytes   */

/* Flags stored in the lower bits of each page entry (same as real x86) */
#define PTE_PRESENT  (1 << 0)   /* page is mapped             */
#define PTE_WRITE    (1 << 1)   /* page is writable           */
#define PTE_USER     (1 << 2)   /* accessible from user-space */
#define PTE_ACCESSED (1 << 5)   /* set by CPU on first access */
#define PTE_DIRTY    (1 << 6)   /* set by CPU on first write  */

/* A page table: 1024 entries, each pointing to a 4KB physical page */
typedef struct {
    uint32_t entries[PT_SIZE];
} my_page_table_t;

/* A page directory: 1024 entries, each pointing to a page table */
typedef struct {
    uint32_t         entries[PD_SIZE];
    my_page_table_t *tables[PD_SIZE];  /* software pointer (not in real hw) */
} my_page_dir_t;

/* Allocate and zero a new page directory */
my_page_dir_t *my_pgd_create(void);

/* Map a virtual page to a physical address */
void my_map_page(my_page_dir_t *pgd, uint32_t vaddr, uint32_t paddr, uint32_t flags);

/*
 * my_page_walk — given a virtual address, print every step of the
 * page table lookup, just like the CPU does it in hardware.
 */
void my_page_walk(my_page_dir_t *pgd, uint32_t vaddr);

/* Translate virtual → physical (returns 0 if not mapped) */
uint32_t my_virt_to_phys(my_page_dir_t *pgd, uint32_t vaddr);

#endif /* MY_PAGING_H */
