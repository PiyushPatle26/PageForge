#ifndef MY_PAGING_H
#define MY_PAGING_H

/*
 * my_paging.h - RISC-V Sv39 paging, simulated in software.
 *
 * Sv39 is the page table format Linux uses by default on 64-bit RISC-V.
 * Addresses are 39 bits wide, and translating one means walking down
 * three tables of 512 entries each:
 *
 *   Virtual address (Sv39, 39 meaningful bits of a 64-bit register):
 *   +-------------+--------+--------+--------+------------+
 *   |   63..39    | VPN[2] | VPN[1] | VPN[0] |   Offset   |
 *   | sign-extend | 9 bits | 9 bits | 9 bits |  12 bits   |
 *   +-------------+--------+--------+--------+------------+
 *          |          |        |        |          |
 *      must equal    PGD  ->  PMD  ->  PTE  ->  physical page + offset
 *       bit 38
 *
 * Linux calls the three levels PGD, PMD and PTE, so this code does too.
 * (Linux has a fourth level called PUD for bigger formats. Sv39 does not
 * need it, so Linux quietly folds it away.)
 *
 * The MMU finds the top table through the satp register, which holds
 * three things: the paging mode, an address space ID used to tag cached
 * translations, and the address of the top table itself.
 *
 * Nothing here touches a real register. The physical addresses are made
 * up, so this all runs as an ordinary user program.
 */

#include "my_types.h"

/* Geometry */
#define PT_ENTRIES   512     /* every level holds 512 8-byte entries */
#define VPN2_SHIFT   30      /* bits 38..30 pick the PGD entry       */
#define VPN1_SHIFT   21      /* bits 29..21 pick the PMD entry       */
#define VPN0_SHIFT   12      /* bits 20..12 pick the PTE entry       */
#define VPN_MASK     0x1FFUL /* 9 bits per level                     */
#define OFFSET_MASK  0xFFFUL /* low 12 bits, so 4096 bytes per page  */
#define SV39_VA_BITS 39

/*
 * What one entry looks like:
 *
 *   63 ....... 54 53 ......... 10 9 8 7 6 5 4 3 2 1 0
 *   | reserved  |      PPN      |RSW|D|A|G|U|X|W|R|V|
 *
 * PPN is the physical page number, and it starts at bit 10. A page is
 * 4096 bytes, so the address is that number times 4096. In shifts: take
 * the entry, shift right 10 to get the page number, shift left 12 to turn
 * it into an address. Two different numbers, and both are correct.
 */
#define PTE_PPN_SHIFT 10

#define PTE_V        (1UL << 0)  /* valid, otherwise the walk faults    */
#define PTE_R        (1UL << 1)  /* readable                            */
#define PTE_W        (1UL << 2)  /* writable                            */
#define PTE_X        (1UL << 3)  /* executable                          */
#define PTE_U        (1UL << 4)  /* reachable from user mode            */
#define PTE_G        (1UL << 5)  /* global, mapped in every address space */
#define PTE_A        (1UL << 6)  /* accessed, hardware sets it on use   */
#define PTE_D        (1UL << 7)  /* dirty, hardware sets it on write    */

/*
 * The permission bits also tell you what kind of entry you are looking at.
 * No R, W or X means "this points at the next table down". Any of them
 * set means "this is the end of the walk, here is your page".
 *
 * That one rule is also how big pages work. Stop at level 1 and the page
 * is 2 MB. Stop at level 2 and it is 1 GB. There is no size field to set,
 * you simply stop early.
 */
#define PTE_PERM_MASK (PTE_R | PTE_W | PTE_X)
#define pte_is_leaf(p)  (((p) & PTE_PERM_MASK) != 0)

/*
 * One table. Real hardware only sees the entries array, and finds the
 * next table down by reading the PPN out of an entry.
 *
 * We cannot do that, because our physical addresses are invented numbers
 * that point nowhere. So we keep a matching array of real C pointers
 * beside it. That array is the one piece of this file that has no
 * hardware equivalent.
 */
typedef struct my_ptable {
    uint64_t          entries[PT_ENTRIES];
    struct my_ptable *child[PT_ENTRIES];
} my_ptable_t;

/* The root table is what satp would point at */
typedef my_ptable_t my_page_dir_t;

/* Allocate a zeroed root page table */
my_page_dir_t *my_pgd_create(void);

/*
 * Map one virtual page to a physical address. Pass at least one of PTE_R,
 * PTE_W or PTE_X in flags, because a leaf with no permissions is read as a
 * pointer entry instead of a mapping.
 */
void my_map_page(my_page_dir_t *pgd, uint64_t vaddr, uint64_t paddr, uint64_t flags);

/* Print every step of the three-level lookup, the way the MMU does it */
void my_page_walk(my_page_dir_t *pgd, uint64_t vaddr);

/*
 * Translate virtual to physical, ignoring permissions. Returns 0 when
 * nothing is mapped. Use my_access() when you care about whether the
 * access is actually allowed.
 */
uint64_t my_virt_to_phys(my_page_dir_t *pgd, uint64_t vaddr);

/*
 * What the program is trying to do. Hardware always knows this, because
 * it comes from the instruction itself: fetching an instruction, loading
 * a value, or storing one.
 */
typedef enum {
    MY_ACCESS_READ,
    MY_ACCESS_WRITE,
    MY_ACCESS_EXEC
} my_access_t;

/* Which privilege level is asking */
#define MY_MODE_SUPERVISOR 0
#define MY_MODE_USER       1

/*
 * Why a translation failed. Finding the page is only half the job. The
 * permissions on it still have to allow what you are trying to do.
 */
typedef enum {
    MY_FAULT_NONE = 0,      /* allowed                                    */
    MY_FAULT_NON_CANONICAL, /* bits 63..39 did not copy bit 38            */
    MY_FAULT_NOT_VALID,     /* hit an entry with V=0                      */
    MY_FAULT_NO_LEAF,       /* reached level 0 without finding a leaf     */
    MY_FAULT_PERMISSION,    /* page exists, but not for this access type  */
    MY_FAULT_PRIVILEGE      /* U bit does not match the current mode      */
} my_fault_t;

typedef struct {
    my_fault_t fault;   /* MY_FAULT_NONE means the access is allowed */
    uint64_t   paddr;   /* only meaningful when fault is MY_FAULT_NONE */
} my_access_result_t;

/*
 * Walk, then check whether this access is allowed. This is the part a
 * plain translation skips. A page mapped read-only translates perfectly
 * well, and writing to it is still a fault.
 */
my_access_result_t my_access(my_page_dir_t *pgd, uint64_t vaddr,
                             my_access_t type, int mode);

/* Short name for a fault, for printing */
const char *my_fault_name(my_fault_t fault);

/*
 * The exception RISC-V would raise for this kind of access. These are the
 * real scause numbers, and they are what arch/riscv/mm/fault.c looks at.
 */
const char *my_fault_cause(my_access_t type, unsigned *scause);

/*
 * Sv39 requires bits 63..39 to all match bit 38. Addresses that break the
 * rule fault before the walk even starts.
 */
int my_va_is_canonical(uint64_t vaddr);

#endif /* MY_PAGING_H */
