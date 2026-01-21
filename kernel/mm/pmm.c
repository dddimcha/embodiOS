/* EMBODIOS Physical Memory Manager - Buddy Allocator */
#include <embodios/mm.h>
#include <embodios/kernel.h>
#include <embodios/console.h>

/* Buddy allocator constants */
#define MAX_ORDER       18  /* Max block size: 2^18 * 4KB = 1GB */
#define MIN_ORDER       0   /* Min block size: 2^0 * 4KB = 4KB */
#define BITMAP_SIZE     (1 << 19)  /* Support up to 2GB with 4KB pages */

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
    /* Simple linear allocator state */
    size_t next_free_page;  /* Next page index to allocate */
} pmm_state = {
    .initialized = false,
    .next_free_page = 0
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

    console_printf("PMM: Aligned start=%p, size=%zu\n", start, size);

    pmm_state.mem_start = start;
    pmm_state.mem_end = (void*)((uintptr_t)start + size);
    pmm_state.total_pages = size >> PAGE_SHIFT;
    pmm_state.free_pages = 0;

    console_printf("PMM: Total pages=%zu\n", pmm_state.total_pages);

    /* Allocate bitmap (use first pages of memory) */
    size_t bitmap_pages = ALIGN_UP(pmm_state.total_pages / 8, PAGE_SIZE) >> PAGE_SHIFT;
    console_printf("PMM: Bitmap pages=%zu\n", bitmap_pages);

    pmm_state.bitmap = (uint8_t*)start;
    console_printf("PMM: Clearing bitmap...\n");
    memset(pmm_state.bitmap, 0, bitmap_pages << PAGE_SHIFT);
    console_printf("PMM: Bitmap cleared\n");
    
    console_printf("PMM: Marking bitmap pages...\n");
    /* Mark bitmap pages as used */
    for (size_t i = 0; i < bitmap_pages; i++) {
        bitmap_set(i);
    }

    console_printf("PMM: Initializing free lists...\n");
    /* Initialize free lists */
    for (int i = 0; i <= MAX_ORDER; i++) {
        pmm_state.free_lists[i].head = NULL;
        pmm_state.free_lists[i].count = 0;
    }

    console_printf("PMM: Setting up linear allocator...\n");
    /* Simple linear allocator: start allocating after bitmap pages */
    pmm_state.next_free_page = bitmap_pages;
    pmm_state.free_pages = pmm_state.total_pages - bitmap_pages;
    console_printf("PMM: %zu pages available starting at page %zu\n",
                   pmm_state.free_pages, pmm_state.next_free_page);

    pmm_state.initialized = true;
    
    console_printf("PMM: Initialized with %zu free pages (%zu MB)\n", 
                   pmm_state.free_pages, (pmm_state.free_pages << PAGE_SHIFT) / (1024 * 1024));
}

/* Allocate a single page */
void* pmm_alloc_page(void)
{
    return pmm_alloc_pages(1);
}

/* Allocate multiple pages - simple linear allocator */
void* pmm_alloc_pages(size_t count)
{
    if (!pmm_state.initialized || count == 0) {
        return NULL;
    }

    /* Check if we have enough pages */
    if (pmm_state.next_free_page + count > pmm_state.total_pages) {
        return NULL;  /* Out of memory */
    }

    /* Allocate consecutive pages */
    size_t page = pmm_state.next_free_page;
    void* addr = page_to_addr(page);

    /* Skip bitmap marking for speed - linear allocator doesn't need it */

    /* Advance the allocator */
    pmm_state.next_free_page += count;
    pmm_state.free_pages -= count;

    /* Skip memset for large allocations - caller can zero if needed */
    /* This avoids 700MB+ memset which takes forever */

    return addr;
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

/* Get total pages */
size_t pmm_total_pages(void)
{
    return pmm_state.total_pages;
}

/* Get total memory in bytes */
size_t pmm_total_memory(void)
{
    return pmm_state.total_pages * PAGE_SIZE;
}

/* Get available memory in bytes */
size_t pmm_available_memory(void)
{
    return pmm_state.free_pages * PAGE_SIZE;
}

/* Print PMM statistics */
void pmm_print_stats(void)
{
    console_printf("Physical Memory Manager:\n");
    console_printf("  Total memory: %zu MB\n", pmm_state.total_pages * PAGE_SIZE / (1024 * 1024));
    console_printf("  Free memory:  %zu MB\n", pmm_state.free_pages * PAGE_SIZE / (1024 * 1024));
    console_printf("  Used memory:  %zu MB\n", (pmm_state.total_pages - pmm_state.free_pages) * PAGE_SIZE / (1024 * 1024));
}
