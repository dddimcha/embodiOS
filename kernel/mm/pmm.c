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
    size_t total_pages;    /* Pages in the managed span (may include holes) */
    size_t usable_pages;   /* Pages actually backed by RAM */
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

/* Check whether page index is usable RAM (bitmap clear) */
static inline bool page_usable(size_t page)
{
    return page < pmm_state.total_pages && !bitmap_test(page);
}

/* Initialize physical memory manager from usable RAM regions.
 * The managed span is [start, top of highest region); pages within the span
 * that are not covered by any region (holes, e.g. the PCI hole at
 * 0xC0000000-0xFFFFFFFF) stay marked unavailable and are never allocated. */
void pmm_init_regions(void* start, const struct pmm_region* regions, size_t num_regions)
{
    /* Align start */
    start = (void*)ALIGN_UP((uintptr_t)start, PAGE_SIZE);

    /* Find top of managed memory */
    uint64_t top = (uintptr_t)start;
    for (size_t i = 0; i < num_regions; i++) {
        uint64_t rend = regions[i].base + regions[i].size;
        if (rend > top) {
            top = rend;
        }
    }
    if (top <= (uint64_t)(uintptr_t)start || num_regions == 0) {
        console_printf("PMM: FATAL - no usable memory regions\n");
        return;
    }

    pmm_state.mem_start = start;
    pmm_state.mem_end = (void*)(uintptr_t)top;
    pmm_state.total_pages = (top - (uint64_t)(uintptr_t)start) >> PAGE_SHIFT;
    pmm_state.free_pages = 0;
    pmm_state.usable_pages = 0;

    console_printf("PMM: Managed span %zu MB at %p (%zu region(s))\n",
                   (pmm_state.total_pages << PAGE_SHIFT) / (1024 * 1024),
                   start, num_regions);

    /* Allocate bitmap (use first pages of memory) */
    size_t bitmap_pages = ALIGN_UP(pmm_state.total_pages / 8, PAGE_SIZE) >> PAGE_SHIFT;

    pmm_state.bitmap = (uint8_t*)start;

    /* Mark ALL pages unavailable first... */
    memset(pmm_state.bitmap, 0xFF, bitmap_pages << PAGE_SHIFT);

    /* ...then clear bits for pages inside usable RAM regions */
    for (size_t r = 0; r < num_regions; r++) {
        uint64_t base = regions[r].base;
        uint64_t end = base + regions[r].size;
        if (end <= (uint64_t)(uintptr_t)start) {
            continue;
        }
        if (base < (uint64_t)(uintptr_t)start) {
            base = (uint64_t)(uintptr_t)start;
        }
        size_t first = (base - (uint64_t)(uintptr_t)start) >> PAGE_SHIFT;
        size_t last = (end - (uint64_t)(uintptr_t)start) >> PAGE_SHIFT;
        if (last > pmm_state.total_pages) {
            last = pmm_state.total_pages;
        }
        for (size_t i = first; i < last; i++) {
            bitmap_clear(i);
        }
        pmm_state.usable_pages += last - first;
    }

    /* Mark bitmap pages themselves as used */
    uint64_t bitmap_end = (uint64_t)(uintptr_t)start + (bitmap_pages << PAGE_SHIFT);
    size_t bitmap_last = (bitmap_end - (uint64_t)(uintptr_t)start) >> PAGE_SHIFT;
    size_t bitmap_usable = 0;
    for (size_t i = 0; i < bitmap_last; i++) {
        if (page_usable(i)) {
            bitmap_set(i);
            bitmap_usable++;
        }
    }
    pmm_state.usable_pages -= bitmap_usable;

    /* Initialize free lists */
    for (int i = 0; i <= MAX_ORDER; i++) {
        pmm_state.free_lists[i].head = NULL;
        pmm_state.free_lists[i].count = 0;
    }

    /* Linear allocator: start at the first usable page */
    size_t first_free = 0;
    while (first_free < pmm_state.total_pages && !page_usable(first_free)) {
        first_free++;
    }
    pmm_state.next_free_page = first_free;
    pmm_state.free_pages = pmm_state.usable_pages;

    pmm_state.initialized = true;

    console_printf("PMM: %zu MB usable RAM (%zu free pages, first at #%zu)\n",
                   (pmm_state.usable_pages << PAGE_SHIFT) / (1024 * 1024),
                   pmm_state.free_pages, pmm_state.next_free_page);
}

/* Initialize physical memory manager (single contiguous region) */
void pmm_init(void* start, size_t size)
{
    struct pmm_region region = {
        .base = (uint64_t)(uintptr_t)start,
        .size = ALIGN_DOWN(size, PAGE_SIZE)
    };
    pmm_init_regions(start, &region, 1);
}

/* Allocate a single page */
void* pmm_alloc_page(void)
{
    return pmm_alloc_pages(1);
}

/* Allocate multiple pages - linear allocator over usable RAM.
 * Skips pages that are not backed by RAM (holes); the returned range is
 * always a contiguous run of usable pages. */
void* pmm_alloc_pages(size_t count)
{
    if (!pmm_state.initialized || count == 0) {
        return NULL;
    }

    /* Scan forward for a contiguous run of usable pages */
    size_t i = pmm_state.next_free_page;
    while (i + count <= pmm_state.total_pages) {
        if (!page_usable(i)) {
            i++;
            continue;
        }
        size_t j = 1;
        while (j < count && page_usable(i + j)) {
            j++;
        }
        if (j == count) {
            /* Found a usable run */
            void* addr = page_to_addr(i);
            pmm_state.next_free_page = i + count;
            pmm_state.free_pages -= count;
            /* Skip memset for large allocations - caller can zero if needed */
            /* This avoids 700MB+ memset which takes forever */
            return addr;
        }
        i += j + 1;  /* skip past the unusable page at i + j */
    }

    return NULL;  /* Out of memory (or no contiguous run large enough) */
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

/* Get total usable pages (RAM-backed, excluding holes) */
size_t pmm_total_pages(void)
{
    return pmm_state.usable_pages;
}

/* Get total usable memory in bytes */
size_t pmm_total_memory(void)
{
    return pmm_state.usable_pages * PAGE_SIZE;
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
    console_printf("  Total memory: %zu MB\n", pmm_state.usable_pages * PAGE_SIZE / (1024 * 1024));
    console_printf("  Free memory:  %zu MB\n", pmm_state.free_pages * PAGE_SIZE / (1024 * 1024));
    console_printf("  Used memory:  %zu MB\n", (pmm_state.usable_pages - pmm_state.free_pages) * PAGE_SIZE / (1024 * 1024));
}
