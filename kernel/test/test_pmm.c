/*
 * Unit test for Physical Memory Manager
 *
 * This test uses a mock PMM implementation to verify allocation
 * and deallocation logic without affecting the real kernel PMM.
 */

#include <embodios/test.h>
#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/mm.h>

/* Mock kernel environment */
#define PAGE_SIZE 4096
#define MOCK_MEM_SIZE (16 * 1024 * 1024)  /* 16MB for tests */
#define MAX_MOCK_PAGES (MOCK_MEM_SIZE / PAGE_SIZE)
#define BITMAP_SIZE ((MAX_MOCK_PAGES + 7) / 8)

/* Simulate PMM functionality using static buffers */
typedef struct {
    uint8_t memory[MOCK_MEM_SIZE];
    size_t size;
    size_t free_pages;
    uint8_t bitmap[BITMAP_SIZE];
} mock_pmm_t;

static mock_pmm_t pmm;

static void mock_pmm_init(size_t mem_size)
{
    if (mem_size > MOCK_MEM_SIZE) {
        mem_size = MOCK_MEM_SIZE;
    }

    pmm.size = mem_size;
    pmm.free_pages = mem_size / PAGE_SIZE;

    /* Clear bitmap */
    for (size_t i = 0; i < BITMAP_SIZE; i++) {
        pmm.bitmap[i] = 0;
    }
}

static void* mock_pmm_alloc_page(void)
{
    if (pmm.free_pages == 0) {
        return NULL;
    }

    /* Find first free page */
    size_t total_pages = pmm.size / PAGE_SIZE;
    for (size_t i = 0; i < total_pages; i++) {
        if (!(pmm.bitmap[i/8] & (1 << (i%8)))) {
            pmm.bitmap[i/8] |= (1 << (i%8));
            pmm.free_pages--;
            return &pmm.memory[i * PAGE_SIZE];
        }
    }
    return NULL;
}

static void mock_pmm_free_page(void* page)
{
    if (!page) {
        return;
    }

    /* Calculate page index */
    size_t offset = (uint8_t*)page - pmm.memory;
    size_t page_idx = offset / PAGE_SIZE;

    if (page_idx >= pmm.size / PAGE_SIZE) {
        return;
    }

    if (pmm.bitmap[page_idx/8] & (1 << (page_idx%8))) {
        pmm.bitmap[page_idx/8] &= ~(1 << (page_idx%8));
        pmm.free_pages++;
    }
}

/* Test basic PMM operations */
static int test_pmm_basic_ops(void)
{
    mock_pmm_init(16 * 1024 * 1024);  /* 16MB */

    /* Test single page allocation */
    void* page1 = mock_pmm_alloc_page();
    if (page1 == NULL) {
        console_printf("[FAIL] First page allocation failed\n");
        return TEST_FAIL;
    }

    void* page2 = mock_pmm_alloc_page();
    if (page2 == NULL) {
        console_printf("[FAIL] Second page allocation failed\n");
        return TEST_FAIL;
    }

    if (page2 == page1) {
        console_printf("[FAIL] Second page same as first page\n");
        return TEST_FAIL;
    }

    /* Test free */
    size_t free_before = pmm.free_pages;
    mock_pmm_free_page(page1);
    if (pmm.free_pages != free_before + 1) {
        console_printf("[FAIL] Free page count mismatch: expected %zu, got %zu\n",
                      free_before + 1, pmm.free_pages);
        return TEST_FAIL;
    }

    /* Test reallocation - should reuse freed page */
    void* page3 = mock_pmm_alloc_page();
    if (page3 != page1) {
        console_printf("[FAIL] Reallocation did not reuse freed page\n");
        return TEST_FAIL;
    }

    /* Cleanup */
    mock_pmm_free_page(page2);
    mock_pmm_free_page(page3);

    return TEST_PASS;
}

/* Test PMM stress scenarios */
static int test_pmm_stress_ops(void)
{
    mock_pmm_init(4 * 1024 * 1024);  /* 4MB */
    size_t total_pages = pmm.size / PAGE_SIZE;

    /* Use static array for page tracking */
    static void* pages[MAX_MOCK_PAGES];

    /* Allocate all pages */
    for (size_t i = 0; i < total_pages; i++) {
        pages[i] = mock_pmm_alloc_page();
        if (pages[i] == NULL) {
            console_printf("[FAIL] Allocation failed at page %zu/%zu\n", i, total_pages);
            return TEST_FAIL;
        }
    }

    if (pmm.free_pages != 0) {
        console_printf("[FAIL] Expected 0 free pages, got %zu\n", pmm.free_pages);
        return TEST_FAIL;
    }

    /* Try to allocate when full */
    void* extra = mock_pmm_alloc_page();
    if (extra != NULL) {
        console_printf("[FAIL] Allocation should fail when memory is full\n");
        return TEST_FAIL;
    }

    /* Free all pages */
    for (size_t i = 0; i < total_pages; i++) {
        if (pages[i]) {
            mock_pmm_free_page(pages[i]);
        }
    }

    if (pmm.free_pages != total_pages) {
        console_printf("[FAIL] Expected %zu free pages after freeing all, got %zu\n",
                      total_pages, pmm.free_pages);
        return TEST_FAIL;
    }

    return TEST_PASS;
}

/* Test buddy algorithm calculations */
static int test_buddy_algorithm_logic(void)
{
    /* Test buddy calculation */
    size_t test_pages[] = {0, 1, 16, 31, 32, 64};
    int orders[] = {0, 1, 2, 3, 4};

    /* Verify buddy calculations are consistent */
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 5; j++) {
            size_t page = test_pages[i];
            size_t buddy = page ^ (1UL << orders[j]);

            /* Verify buddy is symmetric */
            size_t buddy_of_buddy = buddy ^ (1UL << orders[j]);
            if (buddy_of_buddy != page) {
                console_printf("[FAIL] Buddy calculation not symmetric for page %zu order %d\n",
                              page, orders[j]);
                return TEST_FAIL;
            }
        }
    }

    /* Test block splitting logic */
    size_t block_start = 32;
    int current_order = 4;
    size_t expected_splits[][2] = {
        {32, 40},  /* order 3: split into blocks at 32 and 32+8 */
        {32, 36},  /* order 2: split into blocks at 32 and 32+4 */
        {32, 34}   /* order 1: split into blocks at 32 and 32+2 */
    };

    int split_idx = 0;
    while (current_order > 1) {
        current_order--;
        size_t buddy_offset = 1UL << current_order;
        size_t buddy_block = block_start + buddy_offset;

        if (split_idx >= 3) {
            console_printf("[FAIL] Too many splits\n");
            return TEST_FAIL;
        }

        if (block_start != expected_splits[split_idx][0] ||
            buddy_block != expected_splits[split_idx][1]) {
            console_printf("[FAIL] Block split mismatch at order %d: expected [%zu,%zu], got [%zu,%zu]\n",
                          current_order,
                          expected_splits[split_idx][0],
                          expected_splits[split_idx][1],
                          block_start, buddy_block);
            return TEST_FAIL;
        }

        split_idx++;
    }

    return TEST_PASS;
}

/* Main PMM test that runs all sub-tests */
static int test_pmm(void)
{
    int result;

    /* Run basic operations test */
    console_printf("  Running basic operations...\n");
    result = test_pmm_basic_ops();
    if (result != TEST_PASS) {
        console_printf("[FAIL] PMM basic operations failed\n");
        return TEST_FAIL;
    }

    /* Run stress test */
    console_printf("  Running stress test...\n");
    result = test_pmm_stress_ops();
    if (result != TEST_PASS) {
        console_printf("[FAIL] PMM stress test failed\n");
        return TEST_FAIL;
    }

    /* Run buddy algorithm test */
    console_printf("  Running buddy algorithm test...\n");
    result = test_buddy_algorithm_logic();
    if (result != TEST_PASS) {
        console_printf("[FAIL] Buddy algorithm test failed\n");
        return TEST_FAIL;
    }

    return TEST_PASS;
}

/* Test case structure - manually defined */
static struct test_case test_pmm_case = {
    "pmm",
    __FILE__,
    __LINE__,
    test_pmm,
    NULL
};

/* Register test using constructor attribute */
static void __attribute__((constructor)) register_pmm_tests(void)
{
    console_printf("[DEBUG] Registering PMM test...\n");
    test_register(&test_pmm_case);
    console_printf("[DEBUG] PMM test registered\n");
}
