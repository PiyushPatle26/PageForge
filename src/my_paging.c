/*
 * my_paging.c - the RISC-V Sv39 page walk, done in software.
 *
 * When a program uses a pointer, that pointer is a virtual address. It is
 * not where the data actually lives in RAM. Something has to turn it into
 * a real physical address first, and on RISC-V that something is the MMU,
 * which walks a tree of page tables on every single memory access.
 *
 * This file does that walk in C so you can watch it happen.
 *
 * The whole idea in five lines:
 *
 *   - Chop the virtual address into three 9-bit numbers and an offset.
 *   - Use the first number to pick an entry in the top table (the PGD).
 *   - That entry points to the next table down. Repeat.
 *   - The third table gives you the physical page.
 *   - Add the offset and you have the physical address.
 *
 * What the real kernel does with this (see arch/riscv/mm):
 *   - Every process gets its own tree of page tables.
 *   - On a context switch the kernel writes the satp register with the
 *     address of the new tree, so the MMU starts walking somewhere else.
 *   - When a walk fails the CPU raises a page fault, and Linux runs
 *     do_page_fault() in arch/riscv/mm/fault.c to sort it out.
 *   - After changing any entry the kernel must run sfence.vma. RISC-V
 *     caches translations in the TLB and will happily keep using a stale
 *     one until you tell it not to.
 */

#include "my_paging.h"
#include "my_io.h"
#include "my_syscall.h"

/* ------------------------------------------------------------------ */
/* Small helpers                                                        */
/* ------------------------------------------------------------------ */

/* Ask the OS for one zeroed table. Zero means every entry has V=0. */
static my_ptable_t *new_table(void)
{
    my_ptable_t *t = (my_ptable_t *)my_mmap(sizeof(my_ptable_t));
    if (!t)
        my_panic("my_paging: out of memory for a page table");
    return t;
}

/*
 * Pull one 9-bit index out of the address.
 *
 * level 2 is bits 38..30, level 1 is bits 29..21, level 0 is bits 20..12.
 * Shift the bits you want down to the bottom, then mask off the rest.
 */
static uint64_t vpn(uint64_t vaddr, int level)
{
    static const int shift[3] = { VPN0_SHIFT, VPN1_SHIFT, VPN2_SHIFT };
    return (vaddr >> shift[level]) & VPN_MASK;
}

/*
 * Put a physical address into an entry, and get it back out again.
 *
 * Watch the two different shifts. A page is 4096 bytes, so the page
 * number is the address with its low 12 bits dropped. But RISC-V stores
 * that number starting at bit 10 of the entry, not bit 12. So going in
 * you shift down 12 and up 10, and coming out you do the reverse.
 */
static uint64_t pte_pack(uint64_t paddr, uint64_t flags)
{
    return ((paddr >> 12) << PTE_PPN_SHIFT) | flags;
}

static uint64_t pte_paddr(uint64_t pte)
{
    return (pte >> PTE_PPN_SHIFT) << 12;
}

/* Which flag does this kind of access need? Reading needs R, writing W. */
static uint64_t bit_needed_for(my_access_t type)
{
    if (type == MY_ACCESS_READ)  return PTE_R;
    if (type == MY_ACCESS_WRITE) return PTE_W;
    return PTE_X;
}

/* Print an entry's flags as a short string like [VR-X-A] */
static void print_flags(uint64_t pte)
{
    my_printf("%s%s%s%s%s%s",
              (pte & PTE_V) ? "V" : "-",
              (pte & PTE_R) ? "R" : "-",
              (pte & PTE_W) ? "W" : "-",
              (pte & PTE_X) ? "X" : "-",
              (pte & PTE_U) ? "U" : "-",
              (pte & PTE_A) ? "A" : "-");
}

/* ------------------------------------------------------------------ */
/* The walk. Everything else in this file goes through it.              */
/* ------------------------------------------------------------------ */

int my_va_is_canonical(uint64_t vaddr)
{
    /*
     * Sv39 only uses 39 of the 64 bits. The spare top bits are not yours
     * to use: they must all be copies of bit 38. So there are exactly two
     * legal patterns up there, all zeros or all ones, nothing in between.
     */
    uint64_t top = vaddr >> (SV39_VA_BITS - 1);
    return top == 0 || top == (~0UL >> (SV39_VA_BITS - 1));
}

my_page_dir_t *my_pgd_create(void)
{
    return new_table();
}

/*
 * walk - start at the top table and work down until we hit a leaf.
 *
 * There is only one walk in this file and this is it. The three public
 * functions below are thin wrappers that flip two switches:
 *
 *   check - also test the leaf's permissions against the access
 *   show  - print each step as we go
 *
 * On success the physical address goes into *out and we return
 * MY_FAULT_NONE. On failure we return the reason.
 */
static my_fault_t walk(my_page_dir_t *pgd, uint64_t vaddr,
                       my_access_t type, int mode,
                       int check, int show, uint64_t *out)
{
    static const char *name[3] = { "PTE", "PMD", "PGD" };

    if (show)
        my_printf("  Page Walk  VA = 0x%lx\n", vaddr);

    if (!my_va_is_canonical(vaddr)) {
        if (show) my_printf("  └─ FAULT: bits 63..39 must copy bit 38\n\n");
        return MY_FAULT_NON_CANONICAL;
    }

    if (show) {
        my_printf("  ├─ VPN[2]    : %lu  (bits 38..30)\n", vpn(vaddr, 2));
        my_printf("  ├─ VPN[1]    : %lu  (bits 29..21)\n", vpn(vaddr, 1));
        my_printf("  ├─ VPN[0]    : %lu  (bits 20..12)\n", vpn(vaddr, 0));
        my_printf("  ├─ Offset    : 0x%lx  (bits 11..0)\n", vaddr & OFFSET_MASK);
    }

    my_ptable_t *table = pgd;

    /* Level 2 is the top table, level 0 is the bottom one */
    for (int level = 2; level >= 0; level--) {
        uint64_t idx = vpn(vaddr, level);
        uint64_t pte = table->entries[idx];

        if (show) {
            my_printf("  ├─ %s[%lu] = 0x%lx  [", name[level], idx, pte);
            print_flags(pte);
            my_printf("]\n");
        }

        /* V=0 means nobody ever filled this entry in */
        if (!(pte & PTE_V)) {
            if (show) my_printf("  └─ PAGE FAULT: %s entry not valid\n\n", name[level]);
            return MY_FAULT_NOT_VALID;
        }

        /* No R, W or X means this entry just points at the next table */
        if (!pte_is_leaf(pte)) {
            if (level == 0) {
                /* The bottom level has to be a leaf, so this is broken */
                if (show) my_printf("  └─ PAGE FAULT: leaf has no R/W/X\n\n");
                return MY_FAULT_NO_LEAF;
            }
            table = table->child[idx];
            continue;
        }

        /* Found a leaf, so the walk stops here. Is the access allowed? */
        if (check) {
            if (!(pte & bit_needed_for(type))) {
                if (show) my_printf("  └─ PAGE FAULT: permission denied\n\n");
                return MY_FAULT_PERMISSION;
            }
            /*
             * U says who may touch this page, and it is checked both
             * ways. User code needs U=1. The kernel touching a U=1 page
             * is also a fault, unless it sets the SUM bit first. That is
             * what copy_to_user() does, and it is why a stray kernel
             * pointer into user memory crashes instead of quietly working.
             */
            if (mode == MY_MODE_USER && !(pte & PTE_U)) {
                if (show) my_printf("  └─ PAGE FAULT: user touching a kernel page\n\n");
                return MY_FAULT_PRIVILEGE;
            }
            if (mode == MY_MODE_SUPERVISOR && (pte & PTE_U)) {
                if (show) my_printf("  └─ PAGE FAULT: kernel touching a user page\n\n");
                return MY_FAULT_PRIVILEGE;
            }
        }

        /*
         * A leaf above the bottom level is a big page: 2 MB at level 1,
         * 1 GB at level 2. Nothing clever happens, we just stopped early,
         * so more of the address is offset and less of it is page number.
         */
        uint64_t page_mask = (1UL << (VPN0_SHIFT + 9 * level)) - 1;
        *out = (pte_paddr(pte) & ~page_mask) | (vaddr & page_mask);

        if (show) {
            if (level > 0)
                my_printf("  ├─ big page, leaf at level %d\n", level);
            my_printf("  └─ Physical = 0x%lx\n\n", *out);
        }
        return MY_FAULT_NONE;
    }

    return MY_FAULT_NO_LEAF;   /* the loop always returns before here */
}

/* ------------------------------------------------------------------ */
/* Three ways to use the walk                                           */
/* ------------------------------------------------------------------ */

/* Just translate. No permission check, no printing. */
uint64_t my_virt_to_phys(my_page_dir_t *pgd, uint64_t vaddr)
{
    uint64_t pa = 0;
    if (walk(pgd, vaddr, MY_ACCESS_READ, MY_MODE_SUPERVISOR, 0, 0, &pa) != MY_FAULT_NONE)
        return 0;
    return pa;
}

/* Translate and check permissions, the way real hardware does. */
my_access_result_t my_access(my_page_dir_t *pgd, uint64_t vaddr,
                             my_access_t type, int mode)
{
    my_access_result_t r;
    r.paddr = 0;
    r.fault = walk(pgd, vaddr, type, mode, 1, 0, &r.paddr);
    if (r.fault != MY_FAULT_NONE)
        r.paddr = 0;
    return r;
}

/* The same walk, printing every step. */
void my_page_walk(my_page_dir_t *pgd, uint64_t vaddr)
{
    uint64_t pa = 0;
    walk(pgd, vaddr, MY_ACCESS_READ, MY_MODE_SUPERVISOR, 0, 1, &pa);
}

/* ------------------------------------------------------------------ */
/* Building the tables                                                  */
/* ------------------------------------------------------------------ */

/*
 * my_map_page - make one virtual page point at one physical page.
 *
 * Walk down from the top, creating a table wherever one is missing, then
 * write the real entry at the bottom.
 *
 * Note that we set A and D here. Real hardware sets them itself, the
 * first time a page is read or written. We are not real hardware, so
 * nothing would ever set them and the printed flags would look wrong.
 */
void my_map_page(my_page_dir_t *pgd, uint64_t vaddr,
                 uint64_t paddr, uint64_t flags)
{
    my_ptable_t *table = pgd;

    for (int level = 2; level > 0; level--) {
        uint64_t idx = vpn(vaddr, level);

        if (!(table->entries[idx] & PTE_V)) {
            my_ptable_t *next = new_table();
            table->child[idx] = next;
            /* V but no R/W/X, so the walk reads this as "keep going down" */
            table->entries[idx] = pte_pack((uint64_t)(uintptr_t)next, PTE_V);
        }
        table = table->child[idx];
    }

    table->entries[vpn(vaddr, 0)] =
        pte_pack(paddr & ~OFFSET_MASK, flags | PTE_V | PTE_A | PTE_D);
}

/* ------------------------------------------------------------------ */
/* Names, for printing                                                  */
/* ------------------------------------------------------------------ */

const char *my_fault_name(my_fault_t fault)
{
    switch (fault) {
    case MY_FAULT_NONE:          return "ok";
    case MY_FAULT_NON_CANONICAL: return "non-canonical address";
    case MY_FAULT_NOT_VALID:     return "entry not valid";
    case MY_FAULT_NO_LEAF:       return "no leaf entry";
    case MY_FAULT_PERMISSION:    return "permission denied";
    case MY_FAULT_PRIVILEGE:     return "wrong privilege level";
    }
    return "unknown";
}

/*
 * RISC-V has three separate page fault exceptions, one per access type,
 * and puts the number in the scause register. Linux reads it to find out
 * whether the program was reading, writing or executing.
 */
const char *my_fault_cause(my_access_t type, unsigned *scause)
{
    switch (type) {
    case MY_ACCESS_EXEC:  if (scause) *scause = 12; return "instruction page fault";
    case MY_ACCESS_READ:  if (scause) *scause = 13; return "load page fault";
    case MY_ACCESS_WRITE: if (scause) *scause = 15; return "store/AMO page fault";
    }
    if (scause) *scause = 0;
    return "unknown";
}
