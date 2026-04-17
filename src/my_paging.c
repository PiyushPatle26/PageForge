/*
 * my_paging.c — software simulation of a two-level page table.
 *
 * This shows exactly what the CPU does when it walks a page table.
 * No hardware registers are touched — this is purely educational.
 *
 * How Linux uses this in real life:
 *   - The kernel maintains a page directory (PGD) per process.
 *   - On a context switch, CR3 is updated to point to the new PGD.
 *   - On every memory access, the MMU hardware walks the page tables
 *     automatically using the same algorithm we implement below.
 *   - If a mapping is missing, the MMU raises a page fault and the
 *     kernel's page fault handler runs to install the mapping.
 */

#include "my_paging.h"
#include "my_io.h"
#include "my_syscall.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Allocate a zeroed page using mmap */
static void *zalloc_page(void)
{
    void *p = my_mmap(PAGE_SIZE);
    if (!p)
        my_panic("my_paging: out of memory for page table");
    return p;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

my_page_dir_t *my_pgd_create(void)
{
    my_page_dir_t *pgd = (my_page_dir_t *)my_mmap(sizeof(my_page_dir_t));
    if (!pgd)
        my_panic("my_pgd_create: mmap failed");

    /* Zero everything — no mappings yet */
    uint8_t *p = (uint8_t *)pgd;
    for (size_t i = 0; i < sizeof(my_page_dir_t); i++)
        p[i] = 0;

    return pgd;
}

/*
 * my_map_page — install a vaddr → paddr mapping.
 *
 * Step 1: use bits [31..22] to index into the page directory.
 * Step 2: if no page table exists for that PD slot, allocate one.
 * Step 3: use bits [21..12] to index into the page table.
 * Step 4: store (paddr | flags) in the PTE.
 */
void my_map_page(my_page_dir_t *pgd, uint32_t vaddr,
                 uint32_t paddr, uint32_t flags)
{
    uint32_t pd_idx = (vaddr >> PD_SHIFT) & 0x3FF;
    uint32_t pt_idx = (vaddr >> PT_SHIFT) & 0x3FF;

    /* Allocate a page table if this PD slot is empty */
    if (!(pgd->entries[pd_idx] & PTE_PRESENT)) {
        my_page_table_t *pt = (my_page_table_t *)zalloc_page();
        pgd->tables[pd_idx]  = pt;
        /* Store physical address of PT in PDE (simulate: we use the pointer) */
        pgd->entries[pd_idx] = (uint32_t)(uintptr_t)pt | PTE_PRESENT | PTE_WRITE;
    }

    /* Install the PTE */
    my_page_table_t *pt = pgd->tables[pd_idx];
    pt->entries[pt_idx] = (paddr & ~(uint32_t)OFFSET_MASK) | flags | PTE_PRESENT;
}

/*
 * my_page_walk — show the full translation for a virtual address.
 *
 * This prints every step exactly as the MMU hardware does it:
 *   VA → PD index → PT index → physical frame → physical address
 */
void my_page_walk(my_page_dir_t *pgd, uint32_t vaddr)
{
    uint32_t pd_idx = (vaddr >> PD_SHIFT) & 0x3FF;
    uint32_t pt_idx = (vaddr >> PT_SHIFT) & 0x3FF;
    uint32_t offset = vaddr & OFFSET_MASK;

    my_printf("  Page Walk  VA = 0x%x\n", vaddr);
    my_printf("  ├─ PD index  : %u  (bits 31..22)\n", pd_idx);
    my_printf("  ├─ PT index  : %u  (bits 21..12)\n", pt_idx);
    my_printf("  ├─ Offset    : 0x%x  (bits 11..0)\n", offset);

    uint32_t pde = pgd->entries[pd_idx];
    my_printf("  ├─ PDE[%u]  = 0x%x  [%s%s%s]\n",
              pd_idx, pde,
              (pde & PTE_PRESENT) ? "P"  : "-",
              (pde & PTE_WRITE)   ? "W"  : "-",
              (pde & PTE_USER)    ? "U"  : "-");

    if (!(pde & PTE_PRESENT)) {
        my_printf("  └─ PAGE FAULT — PDE not present\n\n");
        return;
    }

    my_page_table_t *pt = pgd->tables[pd_idx];
    uint32_t pte = pt->entries[pt_idx];
    my_printf("  ├─ PTE[%u]  = 0x%x  [%s%s%s]\n",
              pt_idx, pte,
              (pte & PTE_PRESENT)  ? "P" : "-",
              (pte & PTE_WRITE)    ? "W" : "-",
              (pte & PTE_ACCESSED) ? "A" : "-");

    if (!(pte & PTE_PRESENT)) {
        my_printf("  └─ PAGE FAULT — PTE not present\n\n");
        return;
    }

    uint32_t phys = (pte & ~(uint32_t)OFFSET_MASK) | offset;
    my_printf("  └─ Physical = 0x%x\n\n", phys);
}

uint32_t my_virt_to_phys(my_page_dir_t *pgd, uint32_t vaddr)
{
    uint32_t pd_idx = (vaddr >> PD_SHIFT) & 0x3FF;
    uint32_t pt_idx = (vaddr >> PT_SHIFT) & 0x3FF;
    uint32_t offset = vaddr & OFFSET_MASK;

    if (!(pgd->entries[pd_idx] & PTE_PRESENT)) return 0;

    my_page_table_t *pt = pgd->tables[pd_idx];
    uint32_t pte = pt->entries[pt_idx];
    if (!(pte & PTE_PRESENT)) return 0;

    return (pte & ~(uint32_t)OFFSET_MASK) | offset;
}
