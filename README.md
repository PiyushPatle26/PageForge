# PageForge: Linux Kernel Memory Management, From Scratch (RISC-V)

![CI](https://github.com/Piyush-Patle/PageForge/actions/workflows/ci.yml/badge.svg)
![Tests](https://img.shields.io/badge/Unity%20Tests-49%20Passed-brightgreen)
![Arch](https://img.shields.io/badge/Arch-riscv64%20(rv64gc)-8A2BE2)
![QEMU](https://img.shields.io/badge/QEMU-riscv64--static-blue)
![Language](https://img.shields.io/badge/Language-C%20%28C11%29-orange)
![License](https://img.shields.io/badge/License-MIT-lightgrey)

The Linux kernel memory management stack, rebuilt from nothing in C. The standalone binaries link with `-nostdlib` and call no libc at all: the three syscalls they need are issued directly with `ecall`, and `_start`, `memset` and `memcpy` are ours. Every layer is written by hand: raw `mmap` syscalls, a buddy page allocator, a slab object cache, and a `kmalloc`/`kfree`/`calloc`/`realloc` general allocator on top. 49 tests keep it honest. It cross-compiles for riscv64 and runs under QEMU.

I built it to understand how the kernel actually manages memory, by writing each layer instead of reading about it. It targets RISC-V, so the paging layer implements Sv39, the three-level page table format Linux boots with on 64-bit RISC-V hardware.

The docs are written against the Linux source and the RISC-V privileged spec. Every kernel constant they cite was checked against real headers, not recalled, and where a name changed recently the docs say so.

The whole thing is about 1,200 lines of C, and it is meant to be read.

---

## The 5 Layers

```
  User / Kernel Code
       |
       v
+---------------------------------+
|  Layer 4: General Allocator     |  kmalloc / kfree / calloc / realloc
|  (my_alloc.c)                   |  slab for <=1 KB, buddy above that
+---------------------------------+
|  Layer 3: Slab Allocator        |  8 fixed-size caches, 8 B to 1024 B
|  (my_slab.c)                    |  free list inside the objects
+---------------------------------+
|  Layer 2: Buddy Page Allocator  |  1024 pages / 4 MB arena, order 0 to 10
|  (my_buddy.c)                   |  XOR buddy coalescing, O(log N) alloc
+---------------------------------+
|  Layer 1: Raw OS Memory         |  mmap and munmap, nothing else
|  (my_syscall.c)                 |  zero libc dependency
+---------------------------------+
|  Layer 0: Page Table Sim        |  three-level Sv39 walk, RISC-V style
|  (my_paging.c)                  |  VA to PA translation, PTE flags
+---------------------------------+
```

---

## Reading Order

Bottom up, the way the memory actually flows.

**`src/my_syscall.c`** is the entire operating system interface. Four
functions. Read it first because everything else stands on it, and because it
shows how little you need from the OS to build the rest.

**`src/my_buddy.c`** turns one big chunk of memory into pages you can allocate
and free. Go to `my_free_pages()`, where a block finds its neighbour by
flipping one bit of its index and the two merge back together.

**`src/my_slab.c`** carves a single page into same-size objects, because
almost everything a kernel allocates is much smaller than 4 KB. Look at how a
free object stores the pointer to the next free object inside itself.

**`src/my_alloc.c`** is the front door, `kmalloc` and friends. Mostly it
decides whether a request goes to slab or straight to buddy.

**`src/my_paging.c`** is worth the most time and does not depend on the other
four at all. It walks a RISC-V Sv39 page table in software. Read
[`docs/PAGING.md`](docs/PAGING.md) next to it.

`src/my_io.c` is a `printf` built on `write()`. `src/main.c` and
`demo/demo.c` are drivers, not layers, and only exist so you can watch the
thing run.

Every file opens with a comment saying where it sits and which function in it
is worth your time.

---

## Demo Output

Captured from a real rv64 binary running under `qemu-riscv64`. The full run is in
[`assets/demo_output.txt`](assets/demo_output.txt).

```
  Layer 0: Page Table Simulation (RISC-V Sv39)
  VA 0x00001ABC -> PA 0x80100abc
  VA 0x00002080 -> PA 0x80200080
  VA 0x40201004 -> PA 0x80300004
  VA 0x00005000 -> PA 0x0  (not mapped = page fault)
```

The main binary (`make run`) prints the full page walk for each address, one line
per level, plus the permission checks:

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

  Permission checks (same pages, different access types):

    code page R-X, read            allowed   -> PA 0x80100abc
    code page R-X, execute         allowed   -> PA 0x80100abc
    code page R-X, write           FAULT     permission denied (scause 15, store/AMO page fault)
    data page RW-U, user write     allowed   -> PA 0x80200080
    data page RW-U, user exec      FAULT     permission denied (scause 12, instruction page fault)
    stack page RW, user read       FAULT     wrong privilege level (scause 13, load page fault)
```

---

## Unit Tests, 49/49 Passing

All 49 run as rv64 binaries under `qemu-riscv64`. Full log in
[`assets/test_output.txt`](assets/test_output.txt).

---

## Project Structure

```
PageForge/
├── .github/
│   └── workflows/
│       └── ci.yml              # GitHub Actions: cross-build, test, QEMU
├── .gitignore
├── Dockerfile                  # Ubuntu 22.04 with the rv64 cross toolchain
├── Makefile
├── docs/
│   ├── MEMORY_MANAGEMENT.md    # Deep dive into Linux MM, all layers
│   └── PAGING.md               # Focused walkthrough of the Sv39 page walk
├── README.md
├── assets/
│   ├── demo_output.txt         # Captured output of make demo (rv64/QEMU)
│   └── test_output.txt         # Captured output of make test (rv64/QEMU)
├── demo/
│   └── demo.c                  # Standalone colorful demo of all 5 layers
├── include/
│   ├── my_alloc.h
│   ├── my_buddy.h
│   ├── my_io.h
│   ├── my_paging.h
│   ├── my_slab.h
│   ├── my_syscall.h
│   └── my_types.h              # stdint-style types, written by hand
├── src/
│   ├── main.c                  # Entry point for the static binary
│   ├── my_alloc.c              # kmalloc / kfree / calloc / realloc
│   ├── my_buddy.c              # Buddy page allocator
│   ├── my_io.c                 # my_printf / my_puts, no stdio
│   ├── my_paging.c             # Sv39 three-level page table simulation
│   ├── my_slab.c               # Slab object cache
│   └── my_syscall.c            # Raw mmap/munmap/write/exit syscalls
└── tests/
    ├── test_pageforge.c        # 49 Unity tests across all layers
    └── vendor/
        └── unity/              # Unity test framework (ThrowTheSwitch)
```

---

## Technology Stack

| Category         | Tool / Standard                          |
| :--------------- | :--------------------------------------- |
| Language         | C (C11)                                  |
| Build System     | GNU Make                                 |
| Testing          | Unity (ThrowTheSwitch)                   |
| Target Arch      | riscv64 (rv64gc, LP64D ABI)              |
| Toolchain        | `riscv64-linux-gnu-gcc` (cross)          |
| Emulation        | QEMU user-mode (`qemu-riscv64`)          |
| CI/CD            | GitHub Actions                           |
| Environment      | Docker (Ubuntu 22.04)                    |

---

## How to Build

### Docker, same steps as CI

```bash
# Build the image
docker build -t pageforge-dev .

# Cross-build, test and demo in one go (the default CMD)
docker run --rm pageforge-dev

# Or poke around inside
docker run --rm -it pageforge-dev bash
```

### Native (Linux)

```bash
# You need the cross toolchain and QEMU user-mode
sudo apt install build-essential gcc make \
                 gcc-riscv64-linux-gnu qemu-user qemu-user-static

# Cross-compile a static rv64 binary, which is the default
make
file ./pageforge      # ELF 64-bit LSB executable, UCB RISC-V

# Run it under qemu-riscv64
make run

# Or build for this machine instead.
```

`ARCH` works on every target. `make test`, `make demo` and `make run` all
cross-compile for rv64 and go through `qemu-riscv64` by default, or build and

---

## How to Run the Tests

```bash
make test
```

```
tests/test_pageforge.c:360:test_buddy_init_correct_free_pages:PASS
tests/test_pageforge.c:361:test_buddy_alloc_order0_returns_non_null:PASS
...
tests/test_pageforge.c:484:test_paging_multiple_pages:PASS
tests/test_pageforge.c:485:test_paging_sv39_separate_gigabyte_regions:PASS
tests/test_pageforge.c:486:test_paging_sv39_high_address_in_range:PASS
tests/test_pageforge.c:487:test_paging_sv39_sign_extended_kernel_address:PASS
tests/test_pageforge.c:488:test_paging_sv39_rejects_non_canonical_va:PASS
tests/test_pageforge.c:489:test_paging_sv39_leaf_requires_permissions:PASS
tests/test_pageforge.c:490:test_paging_sv39_accessed_dirty_bits_set:PASS
tests/test_pageforge.c:491:test_paging_sv39_upper_levels_are_pointers:PASS

-----------------------
49 Tests 0 Failures 0 Ignored
OK
```

**What the tests cover:**

| Group             | Tests | What it checks                                                  |
| :---------------- | :---: | :-------------------------------------------------------------- |
| Buddy Allocator   |  10   | init, alloc and free, page alignment, coalescing, writable memory |
| Slab Allocator    |   7   | alloc and free, alignment, reuse, 100-cycle stress              |
| General Allocator |  10   | kmalloc, kfree(NULL), calloc zeroing, realloc keeping data      |
| Paging (Sv39)     |  12   | root table creation, the 3-level walk, offsets, faults on unmapped pages, canonical address checks, leaf permissions, A and D bits, pointer entries |
| Access checks     |  10   | read on a readable page, write on a read-only page, exec on a non-exec page, user access to a kernel page, supervisor access to a user page, unmapped and non-canonical faults, scause values, permissions on superpages |

---

## How to Run the Demo

```bash
make demo
```

It walks all five layers with live addresses and pass markers:

- **Layer 1**: `my_mmap(16 KB)`, write, read back, `my_munmap`
- **Layer 2**: buddy alloc at order 0, 1 and 3, then free and watch it coalesce
- **Layer 3**: slab alloc 8 B, 16 B, 64 B, 256 B, then free
- **Layer 4**: `kmalloc`, `calloc` (zeroed), `realloc` (data survives), `kfree`
- **Layer 0**: map 3 pages, translate VA to PA, hit an unmapped one, then run
  read, write and execute against each page to watch the R/W/X and U bits bite

---

## What Each Layer Does

### Buddy allocator, which is Linux's `alloc_pages`

- A **1024 page (4 MB)** arena managed as a binary buddy tree
- `my_alloc_pages(order)` hands back 2^order pages in a row
- `my_free_pages(ptr, order)` frees and merges using the XOR trick:
  ```c
  buddy_idx = page_idx ^ (1 << order);
  ```
- One free list per order (`g_buddy.free[MAX_ORDER]`), O(log N) allocation

### Slab allocator, which is Linux's `kmem_cache`

- **8 fixed-size caches**: 8, 16, 32, 64, 128, 256, 512 and 1024 bytes
- Each slab page keeps its free list inside the objects themselves
- A magic number (`0x51AB1234`) catches header corruption on alloc and free
- Freed memory is poisoned with `0xDEADDEAD` so use-after-free shows up

### General allocator, which is Linux's `kmalloc`

- 1024 bytes or less goes to the slab caches
- Anything bigger goes straight to the buddy allocator
- Every allocation carries a magic number (`0xA110C8ED`) in its header
- `my_calloc` allocates and zeroes
- `my_realloc` allocates, copies the old data over, frees the old block
- `my_kfree(NULL)` does nothing, safely

### Page tables, RISC-V Sv39

- Three levels of 512 entries: PGD, then PMD, then PTE, using Linux's names
- A 39-bit virtual address, split 9 / 9 / 9 / 12:
  - `VPN[2] = (va >> 30) & 0x1FF`
  - `VPN[1] = (va >> 21) & 0x1FF`
  - `VPN[0] = (va >> 12) & 0x1FF`
- 64-bit entries with the PPN at bits 53..10, so `pa = ((pte >> 10) << 12) | offset`.
  Note the shift is 10, not 12. That one catches everybody once.
- Flag bits: `PTE_V`, `PTE_R`, `PTE_W`, `PTE_X`, `PTE_U`, `PTE_G`, `PTE_A`, `PTE_D`
- Permissions are what separate a pointer from a leaf. No R/W/X means the entry
  points at the next level down. Any of them means the walk stops there. A leaf
  above level 0 is a superpage, so 2 MB at level 1 or 1 GB at level 2.
- Bits 63..39 have to copy bit 38. Anything else faults before the walk begins.
- The root table lives in `satp`, and `sfence.vma` clears stale TLB entries.
  Nothing flushes the TLB on its own, so the kernel has to do it after every
  page table change.
- `my_virt_to_phys(pgd, va)` returns 0 when nothing is mapped, which is the fault case
- `my_access(pgd, va, type, mode)` is the one that enforces permissions. Translating
  an address and being allowed to touch it are different questions, and hardware asks
  the second one on every load, store and instruction fetch. A page mapped `R-X`
  translates fine and still faults on a write. The U bit cuts both ways too: user mode
  needs `U=1`, and supervisor mode touching a `U=1` page faults unless the kernel sets
  SUM in `sstatus` first, which is what `copy_to_user()` does.
- Faults report the real `scause` values: 12 for instruction, 13 for load, 15 for store

---

## Linux Names vs PageForge Names

Useful when you go from this code to the kernel source, since almost
everything here has a counterpart with a different name.

| Linux | PageForge | File |
|-------|-----------|------|
| `mmap(MAP_ANONYMOUS)` | `my_mmap()` | `my_syscall.c` |
| `struct page`, the page array | bitmap in `my_buddy_t` | `my_buddy.c` |
| `ZONE_DMA32`, `ZONE_NORMAL` | one flat arena | `my_buddy.c` |
| `alloc_pages(order)` | `my_alloc_pages(order)` | `my_buddy.c` |
| `free_pages(ptr, order)` | `my_free_pages(ptr, order)` | `my_buddy.c` |
| `free_area[NR_PAGE_ORDERS]` | `free_list[MAX_ORDER + 1]` | `my_buddy.c` |
| `struct kmem_cache` | `my_kmem_cache_t` | `my_slab.c` |
| `kmem_cache_alloc()` | `my_slab_alloc(size)` | `my_slab.c` |
| `kmem_cache_free()` | `my_slab_free(ptr)` | `my_slab.c` |
| `kmalloc(size, GFP_KERNEL)` | `my_kmalloc(size)` | `my_alloc.c` |
| `kfree(ptr)` | `my_kfree(ptr)` | `my_alloc.c` |
| `kzalloc()` | `my_calloc(1, size)` | `my_alloc.c` |
| `krealloc()` | `my_realloc(ptr, size)` | `my_alloc.c` |
| `pgd_t`, `pmd_t`, `pte_t` | `my_ptable_t` at levels 2, 1, 0 | `my_paging.c` |
| `pgd_offset()`, `pmd_offset()`, `pte_offset_kernel()` | the loop in `walk()` | `my_paging.c` |
| the `satp` register | a `my_page_dir_t *` variable | `my_paging.c` |
| the MMU's hardware walk | `my_page_walk()` | `my_paging.c` |
| `do_page_fault()` | the fault codes `walk()` returns | `my_paging.c` |
| `sfence.vma` | nothing, there is no TLB here | |
| `struct mm_struct`, VMAs | not implemented | |

---

## Documentation

[`docs/PAGING.md`](docs/PAGING.md) is the one to read if you only read one. It
covers the Sv39 walk from scratch: why the page table is a tree instead of an
array, why every table holds exactly 512 entries, an address split up by hand,
why the PPN shift is 10 when the page shift is 12, how hardware tells a pointer
entry from a leaf, where 2 MB pages come from when there is no size bit, and what
`satp` and `sfence.vma` do. It ends with every function in `my_paging.c` in one
line each, and ten questions to check yourself against. It also disassembles the
walk, so you can see the shifts and masks as the RISC-V instructions they compile
to rather than taking the word "RISC-V" on trust.

[`docs/MEMORY_MANAGEMENT.md`](docs/MEMORY_MANAGEMENT.md) is the long one:

- Why memory management is the hardest part of a kernel
- Physical memory, zones, and `struct page`
- `alloc_pages` and the GFP flags
- The buddy algorithm, splitting and coalescing, with the proof
- Slab internals: caches, slabs, objects, and the partial/full/empty states
- Virtual memory, VMAs (`vm_area_struct`), and how `mmap` works
- The full Sv39 three-level page table walk
- Why translation and permission are different questions, and how SUM fits in
- How the walk maps onto `pgd_offset` / `pmd_offset` / `pte_offset_kernel`
- Which files in `arch/riscv/mm` to read, and in what order
- A walkthrough of every function in this repo
- QEMU user-mode setup and cross-architecture testing

---


---

## License

MIT. See [LICENSE](LICENSE).
