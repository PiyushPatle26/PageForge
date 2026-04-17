/*
 * test_pageforge.c — Unity test suite for PageForge.
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
/* Test arena — re-initialised before each buddy/slab test group       */
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
void tearDown(void) { /* nothing — arena freed in next setUp */ }

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
    my_map_page(pgd, 0x00001000, 0x00100000, PTE_WRITE);
    uint32_t pa = my_virt_to_phys(pgd, 0x00001000);
    TEST_ASSERT_EQUAL_HEX32(0x00100000, pa);
}

void test_paging_offset_preserved(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    my_map_page(pgd, 0x00002000, 0x00200000, PTE_WRITE | PTE_USER);
    uint32_t pa = my_virt_to_phys(pgd, 0x00002080);
    TEST_ASSERT_EQUAL_HEX32(0x00200080, pa);
}

void test_paging_unmapped_returns_zero(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    uint32_t pa = my_virt_to_phys(pgd, 0x00005000);  /* not mapped */
    TEST_ASSERT_EQUAL_HEX32(0, pa);
}

void test_paging_multiple_pages(void)
{
    my_page_dir_t *pgd = my_pgd_create();
    my_map_page(pgd, 0x00001000, 0x00100000, PTE_WRITE);
    my_map_page(pgd, 0x00002000, 0x00200000, PTE_WRITE);
    my_map_page(pgd, 0x00401000, 0x00300000, PTE_WRITE);

    TEST_ASSERT_EQUAL_HEX32(0x00100000, my_virt_to_phys(pgd, 0x00001000));
    TEST_ASSERT_EQUAL_HEX32(0x00200000, my_virt_to_phys(pgd, 0x00002000));
    TEST_ASSERT_EQUAL_HEX32(0x00300000, my_virt_to_phys(pgd, 0x00401000));
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

    return UNITY_END();
}
