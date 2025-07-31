/* EMBODIOS Physical Memory Manager - Buddy Allocator */
#include <embodios/mm.h>
#include <embodios/kernel.h>
#include <embodios/console.h>

/* Buddy allocator constants */
#define MAX_ORDER       11  /* Max block size: 2^11 * 4KB = 8MB */
#define MIN_ORDER       0   /* Min block size: 2^0 * 4KB = 4KB */
#define BITMAP_SIZE     (1 << 16)  /* Support up to 256MB with 4KB pages */

/* Free list for each order */
struct free_list {
    struct page_block* head;
    size_t count;
};

/* Page block structure */
struct page_block {
    struct page_block* next;
    struct page_block* prev;
    uint32_t order;
    uint32_t flags;
};

/* Physical memory manager state */
static struct {
    struct free_list free_lists[MAX_ORDER + 1];
    uint8_t* bitmap;
    void* mem_start;
    void* mem_end;
    size_t total_pages;
    size_t free_pages;
    bool initialized;
} pmm_state = {
    .initialized = false
};

/* Bitmap operations */
static inline void bitmap_set(size_t bit)
{
    pmm_state.bitmap[bit / 8] |= (1 << (bit % 8));
}

static inline void bitmap_clear(size_t bit)
{
    pmm_state.bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static inline bool bitmap_test(size_t bit)
{
    return pmm_state.bitmap[bit / 8] & (1 << (bit % 8));
}

/* Convert address to page index */
static inline size_t addr_to_page(void* addr)
{
    return ((uintptr_t)addr - (uintptr_t)pmm_state.mem_start) >> PAGE_SHIFT;
}

/* Convert page index to address */
static inline void* page_to_addr(size_t page)
{
    return (void*)((uintptr_t)pmm_state.mem_start + (page << PAGE_SHIFT));
}

/* Find buddy of a block */
static inline size_t find_buddy(size_t page, uint32_t order)
{
    return page ^ (1UL << order);
}

/* Add block to free list */
static void free_list_add(struct page_block* block, uint32_t order)
{
    struct free_list* list = &pmm_state.free_lists[order];
    
    block->order = order;
    block->next = list->head;
    block->prev = NULL;
    
    if (list->head) {
        list->head->prev = block;
    }
    
    list->head = block;
    list->count++;
}

/* Remove block from free list */
static void free_list_remove(struct page_block* block, uint32_t order)
{
    struct free_list* list = &pmm_state.free_lists[order];
    
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        list->head = block->next;
    }
    
    if (block->next) {
        block->next->prev = block->prev;
    }
    
    list->count--;
}

/* Split a block */
static struct page_block* split_block(struct page_block* block, uint32_t order, uint32_t target_order)
{
    while (order > target_order) {
        order--;
        
        /* Calculate buddy address */
        size_t page = addr_to_page(block);
        size_t buddy_page = page + (1UL << order);
        struct page_block* buddy = (struct page_block*)page_to_addr(buddy_page);
        
        /* Add buddy to free list */
        free_list_add(buddy, order);
    }
    
    return block;
}

/* Merge blocks */
static void merge_blocks(size_t page, uint32_t order)
{
    while (order < MAX_ORDER) {
        size_t buddy_page = find_buddy(page, order);
        
        /* Check if buddy is free and same order */
        if (buddy_page >= pmm_state.total_pages || bitmap_test(buddy_page)) {
            break;
        }
        
        struct page_block* buddy = (struct page_block*)page_to_addr(buddy_page);
        if (buddy->order != order) {
            break;
        }
        
        /* Remove buddy from free list */
        free_list_remove(buddy, order);
        
        /* Merge with buddy */
        if (buddy_page < page) {
            page = buddy_page;
        }
        
        order++;
    }
    
    /* Add merged block to free list */
    struct page_block* block = (struct page_block*)page_to_addr(page);
    free_list_add(block, order);
}

/* Initialize physical memory manager */
void pmm_init(void* start, size_t size)
{
    console_printf("PMM: Initializing with %zu MB at %p\n", size / (1024 * 1024), start);
    
    /* Align start and size */
    start = (void*)ALIGN_UP((uintptr_t)start, PAGE_SIZE);
    size = ALIGN_DOWN(size, PAGE_SIZE);
    
    pmm_state.mem_start = start;
    pmm_state.mem_end = (void*)((uintptr_t)start + size);
    pmm_state.total_pages = size >> PAGE_SHIFT;
    pmm_state.free_pages = 0;
    
    /* Allocate bitmap (use first pages of memory) */
    size_t bitmap_pages = ALIGN_UP(pmm_state.total_pages / 8, PAGE_SIZE) >> PAGE_SHIFT;
    pmm_state.bitmap = (uint8_t*)start;
    memset(pmm_state.bitmap, 0, bitmap_pages << PAGE_SHIFT);
    
    /* Mark bitmap pages as used */
    for (size_t i = 0; i < bitmap_pages; i++) {
        bitmap_set(i);
    }
    
    /* Initialize free lists */
    for (int i = 0; i <= MAX_ORDER; i++) {
        pmm_state.free_lists[i].head = NULL;
        pmm_state.free_lists[i].count = 0;
    }
    
    /* Add all free memory to buddy allocator */
    size_t current_page = bitmap_pages;
    while (current_page < pmm_state.total_pages) {
        /* Find largest power-of-2 block that fits */
        uint32_t order = MAX_ORDER;
        while (order > 0 && (current_page + (1UL << order)) > pmm_state.total_pages) {
            order--;
        }
        
        /* Add block to free list */
        struct page_block* block = (struct page_block*)page_to_addr(current_page);
        free_list_add(block, order);
        pmm_state.free_pages += 1UL << order;
        
        current_page += 1UL << order;
    }
    
    pmm_state.initialized = true;
    
    console_printf("PMM: Initialized with %zu free pages (%zu MB)\n", 
                   pmm_state.free_pages, (pmm_state.free_pages << PAGE_SHIFT) / (1024 * 1024));
}

/* Allocate a single page */
void* pmm_alloc_page(void)
{
    return pmm_alloc_pages(1);
}

/* Allocate multiple pages */
void* pmm_alloc_pages(size_t count)
{
    if (!pmm_state.initialized || count == 0 || count > (1UL << MAX_ORDER)) {
        return NULL;
    }
    
    /* Find order for requested pages */
    uint32_t order = 0;
    size_t size = 1;
    while (size < count) {
        size <<= 1;
        order++;
    }
    
    /* Find a free block */
    struct page_block* block = NULL;
    for (uint32_t current_order = order; current_order <= MAX_ORDER; current_order++) {
        if (pmm_state.free_lists[current_order].head) {
            block = pmm_state.free_lists[current_order].head;
            free_list_remove(block, current_order);
            
            /* Split if necessary */
            if (current_order > order) {
                block = split_block(block, current_order, order);
            }
            break;
        }
    }
    
    if (!block) {
        return NULL;  /* Out of memory */
    }
    
    /* Mark pages as allocated */
    size_t page = addr_to_page(block);
    for (size_t i = 0; i < (1UL << order); i++) {
        bitmap_set(page + i);
    }
    
    pmm_state.free_pages -= 1UL << order;
    
    /* Clear the allocated memory */
    memset(block, 0, (1UL << order) << PAGE_SHIFT);
    
    return block;
}

/* Free a single page */
void pmm_free_page(void* page)
{
    pmm_free_pages(page, 1);
}

/* Free multiple pages */
void pmm_free_pages(void* addr, size_t count)
{
    if (!pmm_state.initialized || !addr || count == 0) {
        return;
    }
    
    /* Find order for page count */
    uint32_t order = 0;
    size_t size = 1;
    while (size < count) {
        size <<= 1;
        order++;
    }
    
    /* Mark pages as free */
    size_t page = addr_to_page(addr);
    for (size_t i = 0; i < (1UL << order); i++) {
        bitmap_clear(page + i);
    }
    
    pmm_state.free_pages += 1UL << order;
    
    /* Try to merge with buddy blocks */
    merge_blocks(page, order);
}

/* Get available pages */
size_t pmm_available_pages(void)
{
    return pmm_state.free_pages;
}

/* Print PMM statistics */
void pmm_print_stats(void)
{
    console_printf("Physical Memory Manager:
");
    console_printf("  Total memory: %zu MB
", pmm_state.total_pages * PAGE_SIZE / (1024 * 1024));
    console_printf("  Free memory:  %zu MB
", pmm_state.free_pages * PAGE_SIZE / (1024 * 1024));
    console_printf("  Used memory:  %zu MB
", (pmm_state.total_pages - pmm_state.free_pages) * PAGE_SIZE / (1024 * 1024));
}
