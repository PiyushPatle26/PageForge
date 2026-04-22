# Notes on Sv39 paging

These are my working notes for `src/my_paging.c`. I wrote them while building
it, mostly so that I would not have to re-derive the same things every time I
came back to the file.

Everything here comes from two places: the RISC-V privileged specification,
and `arch/riscv` in the Linux source. Every kernel constant I name was checked
against the headers of Linux 7.0 rather than remembered, and I give the file
it lives in so you can check it yourself in one grep.

If you are reading the code, read this first. Then read `my_paging.c` from
top to bottom. It is 176 lines and none of it is clever.

---

## 1. What paging is for

A pointer in a C program is not an address in RAM. It is a virtual address, a
number that only means something inside that one process. Something has to
turn it into a real address before the memory controller ever sees it.

Three things come out of that indirection. Two processes can both use address
`0x1000` and get different memory, so neither can touch the other's data. A
process sees one flat range of addresses even though its actual pages are
scattered all over RAM. And each page carries its own permissions, which is
what stops a program writing over its own code.

The hardware that does the conversion is the MMU. The structure it reads is
the page table.

---

## 2. Why the page table is a tree

The obvious design is a flat array, one entry per page, indexed by page
number. Do the arithmetic and you can see why nobody builds it that way.

Sv39 addresses are 39 bits, so the address space is 512 GB. Split that into
4 KB pages and you get 134,217,728 of them. Eight bytes per entry means a
1 GB table for every process, nearly all of it describing addresses the
process will never touch.

A tree fixes that. Three levels, and a branch that maps nothing simply is not
allocated. A process using a few megabytes needs three small tables.

The 512 is not arbitrary. 512 entries at 8 bytes each is 4096 bytes, so one
table is exactly one page. The same allocator that hands out memory pages can
hand out page tables, and nothing is wasted.

Everything else follows from that. 512 is 2^9, so indexing one table takes 9
bits. Three levels is 27 bits, the offset within a page is 12, and 27 + 12
gives you the 39 in Sv39.

---

## 3. Splitting up the address

```
+-------------+--------+--------+--------+------------+
|   63..39    | VPN[2] | VPN[1] | VPN[0] |   offset   |
| sign extend | 9 bits | 9 bits | 9 bits |  12 bits   |
+-------------+--------+--------+--------+------------+
```

Each 9-bit piece indexes one table. The offset is the position inside the
final page and it rides through the whole walk untouched.

Linux uses the same numbers under its own names, in
`arch/riscv/include/asm/pgtable-64.h`. My `VPN2_SHIFT` of 30 is its
`PGDIR_SHIFT_L3`, and my `VPN1_SHIFT` of 21 is its `PMD_SHIFT`. The L3 in
that name is the giveaway that Sv39 is the three-level case: there are
`PGDIR_SHIFT_L4` and `PGDIR_SHIFT_L5` next to it, at 39 and 48, for Sv48 and
Sv57.

The code is a shift and a mask:

```c
VPN[2] = (va >> 30) & 0x1FF
VPN[1] = (va >> 21) & 0x1FF
VPN[0] = (va >> 12) & 0x1FF
offset =  va        & 0xFFF
```

Shift the bits you want down to the bottom, mask off everything above them.
`0x1FF` is nine ones, `0xFFF` is twelve.

### Try one on paper

`VA = 0x40123456`. Work it out before looking.

```
VPN[2] = (0x40123456 >> 30) & 0x1FF  =   1
VPN[1] = (0x40123456 >> 21) & 0x1FF  =   0
VPN[0] = (0x40123456 >> 12) & 0x1FF  = 291
offset =  0x40123456        & 0xFFF  = 0x456
```

So the MMU reads entry 1 of the top table, entry 0 of the table that points
at, entry 291 of the one after that, then adds `0x456`.

`make run` prints these numbers. If they disagree with yours, the arithmetic
is wrong somewhere and it is not the program.

### The top 25 bits

They hold nothing. They have to be copies of bit 38, which leaves exactly two
legal patterns, all zeros or all ones.

That splits the address space in half with an enormous illegal gap in the
middle. Zeros at the top is user space, ones is the kernel. Anything landing
in the gap faults straight away, before the walk starts, which is why a
corrupted pointer normally crashes on the spot instead of quietly reading
something it should not.

`my_va_is_canonical()` is that check, and it is three lines.

---

## 4. What an entry looks like

```
 63        54 53              10 9  8 7 6 5 4 3 2 1 0
+------------+------------------+----+-+-+-+-+-+-+-+-+
|  reserved  |       PPN        |RSW |D|A|G|U|X|W|R|V|
+------------+------------------+----+-+-+-+-+-+-+-+-+
```

V is the one that matters first. If it is clear, nothing else in the entry
means anything. R, W and X are read, write and execute, and they are separate
bits rather than one permission field. U decides whether user mode may touch
the page. G marks a mapping that exists in every address space so the TLB can
keep it across a context switch. A and D are set by hardware the first time a
page is used and the first time it is written.

Linux defines exactly these in
`arch/riscv/include/asm/pgtable-bits.h`, and the numbering lines up bit for
bit with what `my_paging.h` calls `PTE_V` through `PTE_D`:

```c
#define _PAGE_PRESENT   (1 << 0)    /* my PTE_V */
#define _PAGE_READ      (1 << 1)    /* my PTE_R */
#define _PAGE_WRITE     (1 << 2)    /* my PTE_W */
#define _PAGE_EXEC      (1 << 3)    /* my PTE_X */
#define _PAGE_USER      (1 << 4)    /* my PTE_U */
#define _PAGE_GLOBAL    (1 << 5)    /* my PTE_G */
#define _PAGE_ACCESSED  (1 << 6)    /* my PTE_A */
#define _PAGE_DIRTY     (1 << 7)    /* my PTE_D */
#define _PAGE_SOFT      (3 << 8)    /* the two RSW bits */
```

Note that Linux calls the valid bit `_PAGE_PRESENT`, which is a name carried
over from other architectures. In the RISC-V spec it is V, for valid.

### The two shifts

This is the line I got wrong first time, and the one most worth being able to
explain:

```c
pte  = ((pa >> 12) << 10) | flags;   /* address into entry */
pa   =  (pte >> 10) << 12;           /* entry back to address */
```

Different numbers, both correct.

A page is 4096 bytes, so a physical address is really a page number plus an
offset into that page. Dividing by 4096 is shifting right 12, and that page
number is all the entry stores. The offset does not need storing because it
comes from the virtual address.

RISC-V then puts that page number at bit 10 of the entry, not bit 12, because
bits 0 to 9 are already taken by the flags and the two software bits. So you
strip the offset with `>> 12`, then slide the result up to where the format
wants it with `<< 10`. Reading it back, undo both in reverse.

The kernel has a name for that 10. It is `_PAGE_PFN_SHIFT` in
`pgtable-bits.h`, and the field itself is `_PAGE_PFN_MASK`, defined in
`pgtable-64.h` as `GENMASK(53, 10)`. So the bits 53..10 in the diagram above
are not something I worked out, they are that mask written differently.

Mapping `PA = 0x87654000` with V, R, W and X set:

```
0x87654000 >> 12  = 0x87654        page number
0x87654    << 10  = 0x21D95000
           | 0x0F = 0x21D9500F     finished entry

back out:
0x21D9500F >> 10  = 0x87654
0x87654    << 12  = 0x87654000
           | 0x456 = 0x87654456
```

---

## 5. Pointer or leaf

Nothing in an entry announces which kind it is. You work it out from the
permission bits:

```
        entry with V=1
              |
      +-------+-------+
      |               |
  R=W=X=0        any of R,W,X
      |               |
   pointer          leaf
      |               |
 next table down   walk ends
```

One line of code:

```c
#define pte_is_leaf(p)  (((p) & (PTE_R|PTE_W|PTE_X)) != 0)
```

The reason this matters more than it looks is that it is also how big pages
work. Stop at level 1 rather than going all the way down, and that one entry
now covers everything the level below would have covered, which is 512 x 4 KB
= 2 MB. Stop at level 2 and it is 512 x 2 MB = 1 GB.

There is no page size field anywhere in the format. A big page is just a leaf
that turned up earlier than usual. The code copes by widening the offset:

```c
page_mask = (1UL << (12 + 9 * level)) - 1;
```

Level 0 gives 12 bits of offset, so 4 KB. Level 1 gives 21 bits, so 2 MB.
Level 2 gives 30, so 1 GB. Same line either way.

---

## 6. Finding a page is not the same as being allowed to use it

`my_virt_to_phys()` answers where an address lives. It walks the tables,
finds a leaf, returns an address, and that is all a translation is.

`my_access()` answers whether you may do the thing you are trying to do.
Hardware asks that on every access, and it always knows which kind of access
it is, because that comes from the instruction itself. A load is a load.

Which is why R, W and X are three separate bits. A page marked `R-X`
translates a write perfectly well and faults anyway, because W is clear:

```
code page R-X, read      allowed
code page R-X, execute   allowed
code page R-X, write     FAULT, permission denied
```

### U goes both ways

User mode needs U set to touch a page. That part is obvious.

The other direction is the interesting one. The kernel touching a page with
U set is *also* a fault, unless it sets the SUM bit in `sstatus` first.

That reads backwards until you think about what the kernel spends its time
doing. It is constantly handed pointers that came from user programs. If it
could dereference them freely, one bad pointer would read or scribble on
kernel memory and nothing would notice. So the default is that the kernel may
not touch user pages at all, and `copy_to_user()` and `copy_from_user()` lift
that restriction around one carefully bounded copy. A stray kernel pointer
into user space then crashes loudly, which is the behaviour you want.

### Three faults, not one

RISC-V raises a different exception depending on what you were doing, and
puts the number in `scause`:

| Access | scause | Linux name in `asm/csr.h` |
|:-------|:------:|:--------------------------|
| Instruction fetch | 12 | `EXC_INST_PAGE_FAULT` |
| Load | 13 | `EXC_LOAD_PAGE_FAULT` |
| Store or AMO | 15 | `EXC_STORE_PAGE_FAULT` |

`do_page_fault()` in `arch/riscv/mm/fault.c` switches on exactly those three
to work out what the program was trying to do, which is how it chooses
between mapping a page in and killing the process. The numbers my
`my_fault_cause()` returns are these, which is why they are worth getting
right rather than inventing.

---

## 7. satp, the TLB, sfence.vma

`satp` is where the walk starts. Three fields packed into one register:

```
+--------+----------+--------------------------+
|  MODE  |   ASID   |           PPN            |
| 63..60 |  59..44  |          43..0           |
+--------+----------+--------------------------+
```

MODE picks the format, where 8 means Sv39 and 0 turns translation off
entirely. ASID tags cached translations with the process they belong to, so
switching processes does not throw away the whole cache. PPN is the physical
page number of the top table.

In `arch/riscv/include/asm/csr.h` those modes are spelled out as whole
register values rather than a field:

```c
#define SATP_MODE_39	_AC(0x8000000000000000, UL)
#define SATP_MODE_48	_AC(0x9000000000000000, UL)
#define SATP_MODE_57	_AC(0xa000000000000000, UL)
```

which is just the 8, 9 and 10 sitting in the top four bits.

Switching address spaces is mostly just writing this register.

The TLB caches recent translations, because doing three memory reads before
every actual memory read would be unbearable. A hit skips the walk.

Then there is `sfence.vma`, which is the part that catches people. RISC-V
never invalidates the TLB by itself. Change an entry and the CPU may keep
using the cached translation for as long as it likes, and it is within its
rights to do so. You have to run the instruction.

The bug that produces is horrible, because the page tables are correct. You
can dump them and check every bit and they are fine, and the machine keeps
behaving as though they say something else.

In the kernel the instruction is wrapped in
`arch/riscv/include/asm/tlbflush.h`, and it is exactly what you would expect:

```c
__asm__ __volatile__ ("sfence.vma" : : : "memory");
```

The `"memory"` clobber is doing real work there. It stops the compiler moving
loads and stores across the flush, which would reintroduce the same bug from
the other direction.

---

## 8. Where this stops and hardware starts

This file is a simulation running as an ordinary Linux process, and it
touches no real registers. That is the right call for learning, but it leaves
a gap, and the gap is roughly where Linux lives.

My code says:

```
my_virt_to_phys(pgd, va)
```

which means "pretend to be the MMU, walk these tables, hand back the
address". Real hardware never gets asked. It translates on every access
whether anyone wanted it to or not:

```
instruction touches memory
        |
        v
MMU translates using satp
        |
        v
TLB hit, or a hardware walk of the tables
        |
        v
physical address, or a fault
```

Linux does not translate addresses by hand either. Its job is to build the
tables, put the right entries in them, write the root into `satp`, run
`sfence.vma` when something changes, and deal with the faults that come back.

### The three instructions to know

You do not need RISC-V assembly to follow this project. Three instructions
are enough, and only enough to read them.

Writing `satp` is one instruction:

```asm
csrw satp, t0
```

Write whatever is in `t0` into the register. That is what switches address
spaces, with `t0` holding the mode, ASID and root page number packed
together.

The CSR instructions are a small family, all the same shape: `csrr` reads one
into a register, `csrw` writes a register into one, `csrs` sets whichever
bits are 1 in the source, and `csrc` clears them. The last two are how a
single flag gets flipped without disturbing everything else in the register.
Setting SUM before `copy_to_user()` is a `csrs`.

Then `sfence.vma`, which throws away cached translations after a table
changes. Nothing here needs to run it. You just need to know why Linux does.

Last is the trap path. When a walk fails the CPU does not land directly in
`do_page_fault()`. Hardware records the cause in `scause` and the address
that faulted in `stval`, jumps to the trap vector, and some assembly there
saves registers and decodes what happened before any C runs:

```
faulting instruction
        |
        v
hardware sets scause and stval, jumps to the trap vector
        |
        v
trap assembly saves state, works out the cause
        |
        v
do_page_fault() in C
```

Knowing the C handler has an assembly doorway is enough for now.

### Why I did not put any of this in PageForge

The moment this program runs `csrw satp` it stops being a program. Real page
tables need real physical addresses, which needs control of physical memory,
which needs a bootloader, a linker script, supervisor mode, a trap vector,
and full system QEMU instead of `qemu-riscv64`. That is a small kernel, and a
different project.

Keeping the simulator in C and learning the CSR instructions separately gets
both halves. The simulator explains what the tables mean, the instructions
explain how they get switched on.

---

## 9. The same thing, in Linux's words

Everything above is the hardware's view. Open `arch/riscv` and the same three
levels turn up under Linux's names.

| Sv39 level | Linux type | Indexed by |
|:-----------|:-----------|:-----------|
| Root table, VPN[2] | `pgd_t` | bits 38..30 |
| Second level, VPN[1] | `pmd_t` | bits 29..21 |
| Leaf table, VPN[0] | `pte_t` | bits 20..12 |
| Offset | none | bits 11..0 |

Linux walks it with three helpers, which are my `walk()` loop written out one
level at a time instead of looping:

```c
pgd_t *pgd = pgd_offset(mm, addr);          /* index the root with VPN[2]   */
pmd_t *pmd = pmd_offset(pgd, addr);         /* index level two with VPN[1]  */
pte_t *pte = pte_offset_kernel(pmd, addr);  /* index the leaf with VPN[0]   */
unsigned long val = pte_val(*pte);          /* the PPN and the flag bits    */
```

Sv39 has no PUD level, so `pud_offset()` exists and folds away to nothing.
That folding is the trick that lets one set of generic code compile for Sv39,
Sv48 and Sv57 without being rewritten, with `CONFIG_PGTABLE_LEVELS` deciding
how many levels are real on this machine.

Once the walk makes sense, the RISC-V files are readable in this order:

| To understand | Read |
|:--------------|:-----|
| The PTE bits, V R W X U G A D | `arch/riscv/include/asm/pgtable-bits.h` |
| The helpers above, and level folding | `arch/riscv/include/asm/pgtable.h` |
| Page size, masks, memory layout | `arch/riscv/include/asm/page.h` |
| Building the tables at boot, switching the MMU on | `arch/riscv/mm/init.c` |
| The fault handler | `arch/riscv/mm/fault.c` |
| `sfence.vma` and TLB shootdown | `arch/riscv/mm/tlbflush.c` |

Generic `mm/` can wait. The architecture code is smaller, it is concrete, and
it matches the hardware you have just been reading about. `mm_struct`,
`vm_area_struct`, copy-on-write and the reclaim paths all come after.

---

## 10. Every function in the file

If I cannot say what one of these does in a sentence, I go and read it again.

| Function | What it does |
|:---------|:-------------|
| `new_table()` | One zeroed page from the OS. Zeroed means every entry has V=0, so nothing is mapped |
| `vpn()` | Pulls one 9-bit index out of an address, for a given level |
| `pte_pack()` | Address plus flags into an entry, `>> 12` then `<< 10` |
| `pte_paddr()` | Address back out, `>> 10` then `<< 12` |
| `bit_needed_for()` | Access type to the flag it needs. Read needs R, write needs W |
| `print_flags()` | Prints an entry's bits as `[VR-X-A]` |
| `my_va_is_canonical()` | Checks bits 63..39 are all copies of bit 38 |
| `my_pgd_create()` | An empty top level table |
| `walk()` | The one that does the work. Descends the tree, optionally checking permissions and printing |
| `my_virt_to_phys()` | Walk, no permission check, no printing |
| `my_access()` | Walk with the permission check, like hardware |
| `my_page_walk()` | Walk, printing every step |
| `my_map_page()` | Creates tables on the way down, writes the leaf at the bottom |
| `my_fault_name()` | Fault code to readable text |
| `my_fault_cause()` | The RISC-V exception and scause for an access type |

The last three walkers are the same function with different arguments. There
is only one walk in the file, which was the whole idea. If someone asks why
`walk()` has a `show` parameter, the answer is that printing the walk and
doing the walk are the same walk and I was not going to write it twice.

---

## 11. Where my model stops matching the hardware

Look at the table structure:

```c
typedef struct my_ptable {
    uint64_t          entries[PT_ENTRIES];   /* what hardware reads */
    struct my_ptable *child[PT_ENTRIES];     /* what I read */
} my_ptable_t;
```

Hardware has no `child` array. It finds the next table by taking the PPN out
of the entry and treating it as a physical address.

I cannot do that from a user process. My physical addresses are invented
numbers that point nowhere, so I keep real C pointers alongside them.

Say this out loud if it comes up. Knowing exactly where your model stops
matching hardware is a better answer than hoping nobody asks.

Two smaller ones, both marked in the source. `my_map_page()` quietly aligns
addresses instead of rejecting unaligned ones. And A and D get set when a
page is mapped, where real hardware sets them on first use.

---

## 12. The short version

If I only get one question about this project:

> Linux builds and maintains the page tables. The RISC-V MMU walks them in
> hardware on every memory access, starting from `satp` and using three 9-bit
> slices of the address to index down through three levels. PageForge is a
> software model of that walk, which I wrote so that the PGD, PMD and PTE
> abstractions in `arch/riscv/mm` would mean something concrete when I read
> them.

### Things I should be able to answer cold

1. Why 512 entries per table, and why 9 bits per index?
2. Why is the PPN shift 10 when the page shift is 12?
3. How does hardware tell a pointer entry from a leaf?
4. Where does a 2 MB page come from, when there is no size bit?
5. A page translates fine and the write faults. What happened?
6. Why does the kernel fault touching a user page, and what lifts that?
7. You changed an entry and nothing happened. What did you forget?
8. What is in `child[]` that hardware does not have, and why is it there?
9. Which instruction switches address spaces, and what goes into it?
10. Why can this program not run that instruction?
