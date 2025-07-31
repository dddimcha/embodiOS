/* Unit test for Physical Memory Manager */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Mock kernel environment */
#define PAGE_SIZE 4096
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

/* Simulate PMM functionality */
typedef struct {
    void* base;
    size_t size;
    size_t free_pages;
    unsigned char* bitmap;
} mock_pmm_t;

static mock_pmm_t pmm;

void mock_pmm_init(size_t mem_size) {
    pmm.base = malloc(mem_size);
    pmm.size = mem_size;
    pmm.free_pages = mem_size / PAGE_SIZE;
    
    size_t bitmap_size = (pmm.free_pages + 7) / 8;
    pmm.bitmap = calloc(1, bitmap_size);
    
    printf("PMM initialized: %zu MB, %zu pages\n", 
           mem_size / (1024*1024), pmm.free_pages);
}

void* mock_pmm_alloc_page() {
    if (pmm.free_pages == 0) return NULL;
    
    /* Find first free page */
    size_t total_pages = pmm.size / PAGE_SIZE;
    for (size_t i = 0; i < total_pages; i++) {
        if (!(pmm.bitmap[i/8] & (1 << (i%8)))) {
            pmm.bitmap[i/8] |= (1 << (i%8));
            pmm.free_pages--;
            return (char*)pmm.base + (i * PAGE_SIZE);
        }
    }
    return NULL;
}

void mock_pmm_free_page(void* page) {
    if (!page) return;
    
    size_t page_idx = ((char*)page - (char*)pmm.base) / PAGE_SIZE;
    if (page_idx >= pmm.size / PAGE_SIZE) return;
    
    if (pmm.bitmap[page_idx/8] & (1 << (page_idx%8))) {
        pmm.bitmap[page_idx/8] &= ~(1 << (page_idx%8));
        pmm.free_pages++;
    }
}

void test_pmm_basic() {
    printf("\n=== Testing PMM Basic Operations ===\n");
    
    mock_pmm_init(16 * 1024 * 1024);  /* 16MB */
    
    /* Test single page allocation */
    void* page1 = mock_pmm_alloc_page();
    assert(page1 != NULL);
    printf("Allocated page at %p\n", page1);
    
    void* page2 = mock_pmm_alloc_page();
    assert(page2 != NULL);
    assert(page2 != page1);
    printf("Allocated page at %p\n", page2);
    
    /* Test free */
    size_t free_before = pmm.free_pages;
    mock_pmm_free_page(page1);
    assert(pmm.free_pages == free_before + 1);
    printf("Freed page, free pages: %zu\n", pmm.free_pages);
    
    /* Test reallocation */
    void* page3 = mock_pmm_alloc_page();
    assert(page3 == page1);  /* Should reuse freed page */
    printf("Reallocated same page at %p\n", page3);
    
    /* Cleanup */
    free(pmm.base);
    free(pmm.bitmap);
}

void test_pmm_stress() {
    printf("\n=== Testing PMM Stress ===\n");
    
    mock_pmm_init(4 * 1024 * 1024);  /* 4MB */
    size_t total_pages = pmm.size / PAGE_SIZE;
    
    /* Allocate all pages */
    void** pages = malloc(total_pages * sizeof(void*));
    for (size_t i = 0; i < total_pages; i++) {
        pages[i] = mock_pmm_alloc_page();
        if (pages[i] == NULL) {
            printf("Allocation failed at page %zu/%zu\n", i, total_pages);
            break;
        }
    }
    
    assert(pmm.free_pages == 0);
    printf("Allocated all %zu pages\n", total_pages);
    
    /* Try to allocate when full */
    void* extra = mock_pmm_alloc_page();
    assert(extra == NULL);
    printf("Correctly failed to allocate when full\n");
    
    /* Free all pages */
    for (size_t i = 0; i < total_pages; i++) {
        if (pages[i]) mock_pmm_free_page(pages[i]);
    }
    assert(pmm.free_pages == total_pages);
    printf("Freed all pages successfully\n");
    
    /* Cleanup */
    free(pages);
    free(pmm.base);
    free(pmm.bitmap);
}

void test_buddy_algorithm() {
    printf("\n=== Testing Buddy Algorithm Logic ===\n");
    
    /* Test buddy calculation */
    size_t test_pages[] = {0, 1, 16, 31, 32, 64};
    int orders[] = {0, 1, 2, 3, 4};
    
    for (int i = 0; i < 6; i++) {
        printf("Page %zu buddies:\n", test_pages[i]);
        for (int j = 0; j < 5; j++) {
            size_t buddy = test_pages[i] ^ (1UL << orders[j]);
            printf("  Order %d: %zu\n", orders[j], buddy);
        }
    }
    
    /* Test block splitting */
    printf("\nBlock splitting (order 4 -> order 1):\n");
    size_t block_start = 32;
    size_t block_size = 1UL << 4;  /* 16 pages */
    
    int current_order = 4;
    while (current_order > 1) {
        current_order--;
        size_t buddy_offset = 1UL << current_order;
        printf("  Split order %d: blocks at %zu and %zu\n", 
               current_order, block_start, block_start + buddy_offset);
    }
}

int main() {
    printf("=== EMBODIOS PMM Unit Tests ===\n");
    
    test_pmm_basic();
    test_pmm_stress();
    test_buddy_algorithm();
    
    printf("\n=== All PMM tests passed! ===\n");
    return 0;
}