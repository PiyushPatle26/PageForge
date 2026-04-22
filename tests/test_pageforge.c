/*
 * test_pageforge.c: the Unity test suite.
 *
 * Tests cover all four layers:
 *   - Buddy page allocator
 *   - Slab allocator
 *   - General allocator (kmalloc/kfree/calloc/realloc)
 *   - Paging simulation
 *
 * Build & run:
 *   make test
 */

#include "vendor/unity/unity.h"

/* PageForge headers */
#include "../include/my_types.h"
#include "../include/my_syscall.h"
#include "../include/my_buddy.h"
#include "../include/my_slab.h"
#include "../include/my_alloc.h"
#include "../include/my_paging.h"
#include "../include/my_io.h"

/* ------------------------------------------------------------------ */
/* Test arena, rebuilt before each buddy and slab test group           */
/* ------------------------------------------------------------------ */
static void *g_arena = NULL;

static void init_fresh_allocator(void)
{
    if (g_arena) my_munmap(g_arena, ARENA_SIZE);
    g_arena = my_mmap(ARENA_SIZE);
    my_buddy_init(g_arena, ARENA_SIZE);
    my_alloc_init();
}

/* ------------------------------------------------------------------ */
/* Unity setup / teardown                                              */
/* ------------------------------------------------------------------ */
void setUp(void)    { init_fresh_allocator(); }
void tearDown(void) { /* nothing to do, next setUp frees the arena */ }

/* ================================================================== */
/* BUDDY ALLOCATOR TESTS                                               */
/* ================================================================== */

void test_buddy_init_correct_free_pages(void)
{
    TEST_ASSERT_EQUAL_UINT32(ARENA_PAGES, g_buddy.free_pages);
}

void test_buddy_alloc_order0_returns_non_null(void)
{
    void *p = my_alloc_pages(0);
    TEST_ASSERT_NOT_NULL(p);
}

void test_buddy_alloc_order0_page_aligned(void)
{
    void *p = my_alloc_pages(0);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT32(0, (uintptr_t)p % PAGE_SIZE);
}

void test_buddy_alloc_decrements_free_pages(void)
{
    my_alloc_pages(0);   /* 1 page */
    TEST_ASSERT_EQUAL_UINT32(ARENA_PAGES - 1, g_buddy.free_pages);
}

void test_buddy_alloc_order3_decrements_8_pages(void)
{
    my_alloc_pages(3);   /* 2^3 = 8 pages */
    TEST_ASSERT_EQUAL_UINT32(ARENA_PAGES - 8, g_buddy.free_pages);
}

void test_buddy_free_restores_free_pages(void)
{
    void *p = my_alloc_pages(0);
    my_free_pages(p, 0);
    TEST_ASSERT_EQUAL_UINT32(ARENA_PAGES, g_buddy.free_pages);
}

void test_buddy_coalesce_full_arena(void)
{
    void *a = my_alloc_pages(0);
    void *b = my_alloc_pages(1);
    void *c = my_alloc_pages(3);
    my_free_pages(a, 0);
    my_free_pages(b, 1);
    my_free_pages(c, 3);
    /* After coalescing, the whole arena should be free again */
    TEST_ASSERT_EQUAL_UINT32(ARENA_PAGES, g_buddy.free_pages);
}

void test_buddy_alloc_two_pages_different_addresses(void)
{
    void *p1 = my_alloc_pages(0);
    void *p2 = my_alloc_pages(0);
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_NOT_EQUAL(p1, p2);
}

void test_buddy_free_and_realloc_same_address(void)
{
    void *p1 = my_alloc_pages(0);
    my_free_pages(p1, 0);
    void *p2 = my_alloc_pages(0);
    /* After a free and re-alloc, the same page should come back */
    TEST_ASSERT_EQUAL_PTR(p1, p2);
}

void test_buddy_writable_memory(void)
{
    uint8_t *p = (uint8_t *)my_alloc_pages(0);
    TEST_ASSERT_NOT_NULL(p);
    p[0] = 0xAB;
    p[PAGE_SIZE - 1] = 0xCD;
    TEST_ASSERT_EQUAL_HEX8(0xAB, p[0]);
    TEST_ASSERT_EQUAL_HEX8(0xCD, p[PAGE_SIZE - 1]);
    my_free_pages(p, 0);
}

/* ================================================================== */
/* SLAB ALLOCATOR TESTS                                                */
/* ================================================================== */

void test_slab_alloc_8_not_null(void)
{
    void *p = my_slab_alloc(8);
    TEST_ASSERT_NOT_NULL(p);
    my_slab_free(p);
}

void test_slab_alloc_returns_aligned_pointer(void)
{
    void *p = my_slab_alloc(8);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT32(0, (uintptr_t)p % 8);
    my_slab_free(p);
}

void test_slab_alloc_various_sizes(void)
{
    void *p8   = my_slab_alloc(8);
    void *p16  = my_slab_alloc(16);
    void *p32  = my_slab_alloc(32);
    void *p64  = my_slab_alloc(64);
    void *p128 = my_slab_alloc(128);
    void *p256 = my_slab_alloc(256);
    void *p512 = my_slab_alloc(512);

    TEST_ASSERT_NOT_NULL(p8);
    TEST_ASSERT_NOT_NULL(p16);
    TEST_ASSERT_NOT_NULL(p32);
    TEST_ASSERT_NOT_NULL(p64);
    TEST_ASSERT_NOT_NULL(p128);
    TEST_ASSERT_NOT_NULL(p256);
    TEST_ASSERT_NOT_NULL(p512);

    my_slab_free(p8);
    my_slab_free(p16);
    my_slab_free(p32);
    my_slab_free(p64);
    my_slab_free(p128);
    my_slab_free(p256);
    my_slab_free(p512);
}

void test_slab_alloc_two_different_pointers(void)
{
    void *p1 = my_slab_alloc(64);
    void *p2 = my_slab_alloc(64);
    TEST_ASSERT_NOT_EQUAL(p1, p2);
    my_slab_free(p1);
    my_slab_free(p2);
}

void test_slab_free_and_reuse(void)
{
    void *p1 = my_slab_alloc(64);
    my_slab_free(p1);
    void *p2 = my_slab_alloc(64);
    /* The same slot should be recycled */
    TEST_ASSERT_EQUAL_PTR(p1, p2);
    my_slab_free(p2);
}

void test_slab_write_read_back(void)
{
    uint8_t *p = (uint8_t *)my_slab_alloc(64);
    TEST_ASSERT_NOT_NULL(p);
    for (int i = 0; i < 64; i++) p[i] = (uint8_t)i;
    for (int i = 0; i < 64; i++)
        TEST_ASSERT_EQUAL_UINT8((uint8_t)i, p[i]);
    my_slab_free(p);
}

void test_slab_many_alloc_free_cycle(void)
{
    void *ptrs[100];
    for (int i = 0; i < 100; i++) {
        ptrs[i] = my_slab_alloc(32);
        TEST_ASSERT_NOT_NULL(ptrs[i]);
    }
    for (int i = 0; i < 100; i++)
        my_slab_free(ptrs[i]);
    /* Should be able to allocate again after 100 frees */
    void *p = my_slab_alloc(32);
    TEST_ASSERT_NOT_NULL(p);
    my_slab_free(p);
}

/* ================================================================== */
/* GENERAL ALLOCATOR (kmalloc/kfree/calloc/realloc)                   */
/* ================================================================== */

void test_kmalloc_small_not_null(void)
{
    void *p = my_kmalloc(16);
    TEST_ASSERT_NOT_NULL(p);
    my_kfree(p);
}

void test_kmalloc_large_not_null(void)
{
    void *p = my_kmalloc(8192);  /* > LARGE_THRESHOLD → buddy */
    TEST_ASSERT_NOT_NULL(p);
    my_kfree(p);
}

void test_kmalloc_null_returns_null(void)
{
    void *p = my_kmalloc(0);
    TEST_ASSERT_NULL(p);
}

void test_kmalloc_write_read_back(void)
{
    uint32_t *arr = (uint32_t *)my_kmalloc(4 * sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(arr);
    arr[0] = 10; arr[1] = 20; arr[2] = 30; arr[3] = 40;
    TEST_ASSERT_EQUAL_UINT32(10, arr[0]);
    TEST_ASSERT_EQUAL_UINT32(20, arr[1]);
    TEST_ASSERT_EQUAL_UINT32(30, arr[2]);
    TEST_ASSERT_EQUAL_UINT32(40, arr[3]);
    my_kfree(arr);
}

void test_kfree_null_is_safe(void)
{
    my_kfree(NULL);   /* must not crash */
    TEST_PASS();
}

void test_calloc_zeroed(void)
{
    uint8_t *p = (uint8_t *)my_calloc(64, 1);
    TEST_ASSERT_NOT_NULL(p);
    for (int i = 0; i < 64; i++)
        TEST_ASSERT_EQUAL_UINT8(0, p[i]);
    my_kfree(p);
}

void test_calloc_nmemb_size(void)
{
    uint32_t *arr = (uint32_t *)my_calloc(8, sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(arr);
    for (int i = 0; i < 8; i++)
        TEST_ASSERT_EQUAL_UINT32(0, arr[i]);
    my_kfree(arr);
}

void test_realloc_grows_preserves_data(void)
{
    uint32_t *arr = (uint32_t *)my_kmalloc(4 * sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(arr);
    arr[0] = 1; arr[1] = 2; arr[2] = 3; arr[3] = 4;

    arr = (uint32_t *)my_realloc(arr, 16 * sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_EQUAL_UINT32(1, arr[0]);
    TEST_ASSERT_EQUAL_UINT32(2, arr[1]);
    TEST_ASSERT_EQUAL_UINT32(3, arr[2]);
    TEST_ASSERT_EQUAL_UINT32(4, arr[3]);
    my_kfree(arr);
}

void test_realloc_null_ptr_acts_as_kmalloc(void)
{
    void *p = my_realloc(NULL, 64);
    TEST_ASSERT_NOT_NULL(p);
    my_kfree(p);
}

void test_realloc_zero_size_acts_as_kfree(void)
{
    void *p = my_kmalloc(64);
    TEST_ASSERT_NOT_NULL(p);
    void *r = my_realloc(p, 0);
    TEST_ASSERT_NULL(r);
}

/* ================================================================== */
/* PAGING SIMULATION TESTS                                             */
/* ================================================================== */

void test_paging_pgd_create_not_null(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    TEST_ASSERT_NOT_NULL(pgd);
}

void test_paging_map_and_translate(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    my_map_page(pgd, 0x00001000, 0x80100000, PTE_R | PTE_W);
    uint64_t pa = my_virt_to_phys(pgd, 0x00001000);
    TEST_ASSERT_EQUAL_HEX64(0x80100000, pa);
}

void test_paging_offset_preserved(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    my_map_page(pgd, 0x00002000, 0x80200000, PTE_R | PTE_W | PTE_U);
    uint64_t pa = my_virt_to_phys(pgd, 0x00002080);
    TEST_ASSERT_EQUAL_HEX64(0x80200080, pa);
}

void test_paging_unmapped_returns_zero(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    uint64_t pa = my_virt_to_phys(pgd, 0x00005000);  /* not mapped */
    TEST_ASSERT_EQUAL_HEX64(0, pa);
}

void test_paging_multiple_pages(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    my_map_page(pgd, 0x00001000, 0x80100000, PTE_R | PTE_W);
    my_map_page(pgd, 0x00002000, 0x80200000, PTE_R | PTE_W);
    my_map_page(pgd, 0x40201000, 0x80300000, PTE_R | PTE_W);

    TEST_ASSERT_EQUAL_HEX64(0x80100000, my_virt_to_phys(pgd, 0x00001000));
    TEST_ASSERT_EQUAL_HEX64(0x80200000, my_virt_to_phys(pgd, 0x00002000));
    TEST_ASSERT_EQUAL_HEX64(0x80300000, my_virt_to_phys(pgd, 0x40201000));
}

/* ---- Sv39 specific behaviour ------------------------------------- */

/*
 * Two addresses that differ only in VPN[2] have to land in different PGD
 * slots. 1 GB apart is exactly one level-2 entry.
 */
void test_paging_sv39_separate_gigabyte_regions(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    my_map_page(pgd, 0x00000000C0000000UL, 0x81000000UL, PTE_R | PTE_W);
    my_map_page(pgd, 0x0000000100000000UL, 0x82000000UL, PTE_R | PTE_W);

    TEST_ASSERT_EQUAL_HEX64(0x81000000UL, my_virt_to_phys(pgd, 0x00000000C0000000UL));
    TEST_ASSERT_EQUAL_HEX64(0x82000000UL, my_virt_to_phys(pgd, 0x0000000100000000UL));
}

/* Sv39 covers 39 bits, so the top of the low half sits just under 256 GB */
void test_paging_sv39_high_address_in_range(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    uint64_t va = 0x0000003FFFFFF000UL;   /* last page of the low half */
    my_map_page(pgd, va, 0x88000000UL, PTE_R | PTE_W);
    TEST_ASSERT_EQUAL_HEX64(0x88000000UL, my_virt_to_phys(pgd, va));
}

/* Kernel-half addresses are sign extended and should translate normally */
void test_paging_sv39_sign_extended_kernel_address(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    uint64_t va = 0xFFFFFFC000001000UL;   /* Linux rv64 kernel-half style VA */
    TEST_ASSERT_TRUE(my_va_is_canonical(va));

    my_map_page(pgd, va, 0x80400000UL, PTE_R | PTE_W);
    TEST_ASSERT_EQUAL_HEX64(0x80400000UL, my_virt_to_phys(pgd, va));
}

/* Bits 63..39 have to copy bit 38, anything else faults before the walk */
void test_paging_sv39_rejects_non_canonical_va(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    uint64_t bad = 0x0000800000001000UL;  /* bit 47 set, bit 38 clear */

    TEST_ASSERT_FALSE(my_va_is_canonical(bad));
    TEST_ASSERT_EQUAL_HEX64(0, my_virt_to_phys(pgd, bad));
}

/* A leaf needs at least one of R/W/X, otherwise nothing is mapped */
void test_paging_sv39_leaf_requires_permissions(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    my_map_page(pgd, 0x00003000, 0x80500000UL, 0);   /* no R/W/X */
    TEST_ASSERT_EQUAL_HEX64(0, my_virt_to_phys(pgd, 0x00003000));
}

/* Hardware sets A and D on use. my_map_page sets them upfront instead. */
void test_paging_sv39_accessed_dirty_bits_set(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    my_map_page(pgd, 0x00004000, 0x80600000UL, PTE_R | PTE_W | PTE_U);

    /* Walk down to the leaf by hand so we can look at the raw entry */
    my_ptable_t *pmd = pgd->child[(0x00004000UL >> VPN2_SHIFT) & VPN_MASK];
    TEST_ASSERT_NOT_NULL(pmd);
    my_ptable_t *pt = pmd->child[(0x00004000UL >> VPN1_SHIFT) & VPN_MASK];
    TEST_ASSERT_NOT_NULL(pt);

    uint64_t pte = pt->entries[(0x00004000UL >> VPN0_SHIFT) & VPN_MASK];
    TEST_ASSERT_TRUE(pte & PTE_V);
    TEST_ASSERT_TRUE(pte & PTE_A);
    TEST_ASSERT_TRUE(pte & PTE_D);
    TEST_ASSERT_TRUE(pte & PTE_U);
    TEST_ASSERT_FALSE(pte & PTE_X);
}

/* Upper level entries are pointers: valid, but no R/W/X of their own */
void test_paging_sv39_upper_levels_are_pointers(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    my_map_page(pgd, 0x00001000, 0x80100000UL, PTE_R | PTE_W);

    uint64_t pgde = pgd->entries[(0x00001000UL >> VPN2_SHIFT) & VPN_MASK];
    TEST_ASSERT_TRUE(pgde & PTE_V);
    TEST_ASSERT_FALSE(pgde & PTE_PERM_MASK);   /* so it is not a leaf */
}

/* ---- Access permission checks ------------------------------------- */

/*
 * Translation and permission are separate questions. These pages all
 * translate, but only some of the accesses are allowed.
 */

void test_access_read_allowed_on_readable_page(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    my_map_page(pgd, 0x00001000, 0x80100000UL, PTE_R);

    my_access_result_t r = my_access(pgd, 0x00001000, MY_ACCESS_READ, MY_MODE_SUPERVISOR);
    TEST_ASSERT_EQUAL_INT(MY_FAULT_NONE, r.fault);
    TEST_ASSERT_EQUAL_HEX64(0x80100000UL, r.paddr);
}

void test_access_write_faults_on_read_only_page(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    my_map_page(pgd, 0x00001000, 0x80100000UL, PTE_R);

    /* The page is there and translates fine... */
    TEST_ASSERT_EQUAL_HEX64(0x80100000UL, my_virt_to_phys(pgd, 0x00001000));

    /* ...but writing to it is still a fault */
    my_access_result_t r = my_access(pgd, 0x00001000, MY_ACCESS_WRITE, MY_MODE_SUPERVISOR);
    TEST_ASSERT_EQUAL_INT(MY_FAULT_PERMISSION, r.fault);
    TEST_ASSERT_EQUAL_HEX64(0, r.paddr);
}

void test_access_exec_faults_on_non_executable_page(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    my_map_page(pgd, 0x00002000, 0x80200000UL, PTE_R | PTE_W);

    my_access_result_t r = my_access(pgd, 0x00002000, MY_ACCESS_EXEC, MY_MODE_SUPERVISOR);
    TEST_ASSERT_EQUAL_INT(MY_FAULT_PERMISSION, r.fault);
}

void test_access_exec_allowed_on_executable_page(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    my_map_page(pgd, 0x00003000, 0x80300000UL, PTE_R | PTE_X);

    my_access_result_t r = my_access(pgd, 0x00003000, MY_ACCESS_EXEC, MY_MODE_SUPERVISOR);
    TEST_ASSERT_EQUAL_INT(MY_FAULT_NONE, r.fault);
}

/* User mode needs U=1 on the leaf */
void test_access_user_faults_on_kernel_page(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    my_map_page(pgd, 0x00004000, 0x80400000UL, PTE_R | PTE_W);   /* no PTE_U */

    my_access_result_t r = my_access(pgd, 0x00004000, MY_ACCESS_READ, MY_MODE_USER);
    TEST_ASSERT_EQUAL_INT(MY_FAULT_PRIVILEGE, r.fault);
}

/*
 * And the other direction: supervisor mode touching a user page also
 * faults, unless the kernel sets SUM in sstatus first. That is the check
 * behind copy_to_user() and friends.
 */
void test_access_supervisor_faults_on_user_page(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    my_map_page(pgd, 0x00005000, 0x80500000UL, PTE_R | PTE_W | PTE_U);

    my_access_result_t r = my_access(pgd, 0x00005000, MY_ACCESS_READ, MY_MODE_SUPERVISOR);
    TEST_ASSERT_EQUAL_INT(MY_FAULT_PRIVILEGE, r.fault);
}

void test_access_unmapped_page_faults(void)
{
    my_page_dir_t *pgd = my_pgd_create();

    my_access_result_t r = my_access(pgd, 0x00009000, MY_ACCESS_READ, MY_MODE_SUPERVISOR);
    TEST_ASSERT_EQUAL_INT(MY_FAULT_NOT_VALID, r.fault);
}

void test_access_non_canonical_va_faults(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    uint64_t bad = 0x0000800000001000UL;

    my_access_result_t r = my_access(pgd, bad, MY_ACCESS_READ, MY_MODE_SUPERVISOR);
    TEST_ASSERT_EQUAL_INT(MY_FAULT_NON_CANONICAL, r.fault);
}

/* Each access type reports the scause value RISC-V would actually raise */
void test_access_fault_causes_match_riscv(void)
{
    unsigned scause = 0;

    my_fault_cause(MY_ACCESS_EXEC, &scause);
    TEST_ASSERT_EQUAL_UINT(12, scause);
    my_fault_cause(MY_ACCESS_READ, &scause);
    TEST_ASSERT_EQUAL_UINT(13, scause);
    my_fault_cause(MY_ACCESS_WRITE, &scause);
    TEST_ASSERT_EQUAL_UINT(15, scause);
}

/* A superpage carries permissions the same way an ordinary leaf does */
void test_access_permissions_apply_to_superpages(void)
{
    my_page_dir_t *pgd = my_pgd_create();

    /* Build a 2 MB leaf at level 1 by hand */
    uint64_t va = 0x00000000C0000000UL;
    my_map_page(pgd, va, 0x81000000UL, PTE_R);   /* creates the levels for us */
    my_ptable_t *pmd = pgd->child[(va >> VPN2_SHIFT) & VPN_MASK];
    pmd->entries[(va >> VPN1_SHIFT) & VPN_MASK] =
        ((0x81000000UL >> 12) << PTE_PPN_SHIFT) | PTE_V | PTE_R | PTE_A | PTE_D;

    my_access_result_t rd = my_access(pgd, va + 0x1234, MY_ACCESS_READ, MY_MODE_SUPERVISOR);
    TEST_ASSERT_EQUAL_INT(MY_FAULT_NONE, rd.fault);
    TEST_ASSERT_EQUAL_HEX64(0x81001234UL, rd.paddr);

    my_access_result_t wr = my_access(pgd, va + 0x1234, MY_ACCESS_WRITE, MY_MODE_SUPERVISOR);
    TEST_ASSERT_EQUAL_INT(MY_FAULT_PERMISSION, wr.fault);
}

/* ================================================================== */
/* TEST RUNNER                                                          */
/* ================================================================== */

int main(void)
{
    UNITY_BEGIN();

    /* Buddy */
    RUN_TEST(test_buddy_init_correct_free_pages);
    RUN_TEST(test_buddy_alloc_order0_returns_non_null);
    RUN_TEST(test_buddy_alloc_order0_page_aligned);
    RUN_TEST(test_buddy_alloc_decrements_free_pages);
    RUN_TEST(test_buddy_alloc_order3_decrements_8_pages);
    RUN_TEST(test_buddy_free_restores_free_pages);
    RUN_TEST(test_buddy_coalesce_full_arena);
    RUN_TEST(test_buddy_alloc_two_pages_different_addresses);
    RUN_TEST(test_buddy_free_and_realloc_same_address);
    RUN_TEST(test_buddy_writable_memory);

    /* Slab */
    RUN_TEST(test_slab_alloc_8_not_null);
    RUN_TEST(test_slab_alloc_returns_aligned_pointer);
    RUN_TEST(test_slab_alloc_various_sizes);
    RUN_TEST(test_slab_alloc_two_different_pointers);
    RUN_TEST(test_slab_free_and_reuse);
    RUN_TEST(test_slab_write_read_back);
    RUN_TEST(test_slab_many_alloc_free_cycle);

    /* General allocator */
    RUN_TEST(test_kmalloc_small_not_null);
    RUN_TEST(test_kmalloc_large_not_null);
    RUN_TEST(test_kmalloc_null_returns_null);
    RUN_TEST(test_kmalloc_write_read_back);
    RUN_TEST(test_kfree_null_is_safe);
    RUN_TEST(test_calloc_zeroed);
    RUN_TEST(test_calloc_nmemb_size);
    RUN_TEST(test_realloc_grows_preserves_data);
    RUN_TEST(test_realloc_null_ptr_acts_as_kmalloc);
    RUN_TEST(test_realloc_zero_size_acts_as_kfree);

    /* Paging */
    RUN_TEST(test_paging_pgd_create_not_null);
    RUN_TEST(test_paging_map_and_translate);
    RUN_TEST(test_paging_offset_preserved);
    RUN_TEST(test_paging_unmapped_returns_zero);
    RUN_TEST(test_paging_multiple_pages);

    /* Paging, the Sv39 specific bits */
    RUN_TEST(test_paging_sv39_separate_gigabyte_regions);
    RUN_TEST(test_paging_sv39_high_address_in_range);
    RUN_TEST(test_paging_sv39_sign_extended_kernel_address);
    RUN_TEST(test_paging_sv39_rejects_non_canonical_va);
    RUN_TEST(test_paging_sv39_leaf_requires_permissions);
    RUN_TEST(test_paging_sv39_accessed_dirty_bits_set);
    RUN_TEST(test_paging_sv39_upper_levels_are_pointers);

    /* Access permissions */
    RUN_TEST(test_access_read_allowed_on_readable_page);
    RUN_TEST(test_access_write_faults_on_read_only_page);
    RUN_TEST(test_access_exec_faults_on_non_executable_page);
    RUN_TEST(test_access_exec_allowed_on_executable_page);
    RUN_TEST(test_access_user_faults_on_kernel_page);
    RUN_TEST(test_access_supervisor_faults_on_user_page);
    RUN_TEST(test_access_unmapped_page_faults);
    RUN_TEST(test_access_non_canonical_va_faults);
    RUN_TEST(test_access_fault_causes_match_riscv);
    RUN_TEST(test_access_permissions_apply_to_superpages);

    return UNITY_END();
}
