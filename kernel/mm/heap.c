/* Simple heap allocator for AI workloads */

#include "embodios/types.h"
#include "embodios/kernel.h"
#include "embodios/mm.h"
#include "embodios/console.h"

/* Heap configuration */
#define MIN_HEAP_SIZE   (16 * 1024 * 1024)   /* Minimum 16MB heap */
#define MAX_HEAP_SIZE   (256 * 1024 * 1024)  /* Maximum 256MB - fits in order 16 buddy block */
#define HEAP_PERCENT    50                    /* Use 50% for AI models (max 256MB) */
#define MIN_BLOCK_SIZE  64
#define ALIGNMENT       16

/* Block header */
typedef struct block_header {
    size_t size;
    bool used;
    struct block_header *next;
    struct block_header *prev;
} block_header_t;

/* Heap state */
static struct {
    void *start;
    void *end;
    block_header_t *free_list;
    size_t total_size;
    size_t used_size;
    bool initialized;
} heap_state = {
    .initialized = false
};

/* Calculate optimal heap size based on available memory */
static size_t calculate_heap_size(void)
{
    size_t available = pmm_available_memory();
    size_t heap_size;

    /* Use HEAP_PERCENT of available memory */
    heap_size = (available * HEAP_PERCENT) / 100;

    /* Apply minimum and maximum bounds */
    if (heap_size < MIN_HEAP_SIZE) {
        heap_size = MIN_HEAP_SIZE;
    }
    if (heap_size > MAX_HEAP_SIZE) {
        heap_size = MAX_HEAP_SIZE;
    }

    /* Align to page boundary */
    heap_size = heap_size & ~(PAGE_SIZE - 1);

    return heap_size;
}

/* Initialize heap */
void heap_init(void)
{
    /* Calculate heap size based on available memory */
    size_t heap_size = calculate_heap_size();
    size_t heap_pages = heap_size / PAGE_SIZE;

    /* Allocate heap memory from PMM */
    void* heap_mem = pmm_alloc_pages(heap_pages);
    if (!heap_mem) {
        /* Fallback: try with minimum heap size */
        console_printf("Heap: Failed to allocate %zu MB, trying minimum...\n",
                       heap_size / (1024 * 1024));
        heap_size = MIN_HEAP_SIZE;
        heap_pages = heap_size / PAGE_SIZE;
        heap_mem = pmm_alloc_pages(heap_pages);

        if (!heap_mem) {
            console_printf("Heap: FATAL - Cannot allocate memory for heap!\n");
            return;
        }
    }

    heap_state.start = heap_mem;
    heap_state.end = (void*)((uintptr_t)heap_mem + heap_size);
    heap_state.total_size = heap_size;
    heap_state.used_size = sizeof(block_header_t);

    /* Create initial free block */
    block_header_t *initial = (block_header_t*)heap_state.start;
    initial->size = heap_size - sizeof(block_header_t);
    initial->used = false;
    initial->next = NULL;
    initial->prev = NULL;

    heap_state.free_list = initial;
    heap_state.initialized = true;

    console_printf("Heap: Initialized %zu MB at %p (dynamic from PMM)\n",
                   heap_size / (1024 * 1024), heap_state.start);
}

/* Align size to boundary */
static size_t align_size(size_t size)
{
    return (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

/* Split a block if it's too large */
static void split_block(block_header_t *block, size_t size)
{
    size_t total_size = block->size + sizeof(block_header_t);
    
    if (total_size >= size + sizeof(block_header_t) + MIN_BLOCK_SIZE) {
        /* Create new block after this one */
        block_header_t *new_block = (block_header_t*)((uint8_t*)block + sizeof(block_header_t) + size);
        new_block->size = total_size - size - sizeof(block_header_t) - sizeof(block_header_t);
        new_block->used = false;
        new_block->next = block->next;
        new_block->prev = block;
        
        if (block->next) {
            block->next->prev = new_block;
        }
        
        block->next = new_block;
        block->size = size;
    }
}

/* Allocate memory from heap */
void* heap_alloc(size_t size)
{
    if (!heap_state.initialized) {
        heap_init();
    }
    
    if (size == 0) return NULL;
    
    size = align_size(size);
    
    /* Find first fit */
    block_header_t *current = heap_state.free_list;
    while (current) {
        if (!current->used && current->size >= size) {
            /* Found suitable block */
            split_block(current, size);
            current->used = true;
            heap_state.used_size += current->size + sizeof(block_header_t);
            
            /* Return pointer after header */
            return (uint8_t*)current + sizeof(block_header_t);
        }
        current = current->next;
    }
    
    console_printf("Heap: Failed to allocate %zu bytes\n", size);
    return NULL;
}

/* Free memory to heap */
void heap_free(void *ptr)
{
    if (!ptr) return;
    
    /* Get block header */
    block_header_t *block = (block_header_t*)((uint8_t*)ptr - sizeof(block_header_t));
    
    /* Validate block */
    if ((void*)block < heap_state.start || (void*)block >= heap_state.end) {
        console_printf("Heap: Invalid free pointer 0x%p\n", ptr);
        return;
    }
    
    block->used = false;
    heap_state.used_size -= block->size + sizeof(block_header_t);
    
    /* Coalesce with next block if free */
    if (block->next && !block->next->used) {
        block->size += block->next->size + sizeof(block_header_t);
        block->next = block->next->next;
        if (block->next) {
            block->next->prev = block;
        }
    }
    
    /* Coalesce with previous block if free */
    if (block->prev && !block->prev->used) {
        block->prev->size += block->size + sizeof(block_header_t);
        block->prev->next = block->next;
        if (block->next) {
            block->next->prev = block->prev;
        }
    }
}


/* Allocate aligned memory from heap */
void* heap_alloc_aligned(size_t size, size_t alignment)
{
    if (!heap_state.initialized) {
        heap_init();
    }

    if (size == 0) return NULL;
    if (alignment == 0) alignment = ALIGNMENT;

    /* Alignment must be power of 2 */
    if ((alignment & (alignment - 1)) != 0) {
        return NULL;
    }

    /* Check for overflow in size calculation */
    if (size > SIZE_MAX - alignment - sizeof(void*)) {
        return NULL;
    }

    /* Always use over-allocation pattern for consistency with free_aligned
     * This ensures heap_free_aligned always works correctly */
    size_t total_size = size + alignment + sizeof(void*);
    void* raw = heap_alloc(total_size);
    if (!raw) return NULL;

    /* Compute aligned address, leaving room to store original pointer */
    uintptr_t raw_addr = (uintptr_t)raw;
    uintptr_t aligned_addr = (raw_addr + sizeof(void*) + alignment - 1) & ~(alignment - 1);

    /* Store original pointer just before aligned address */
    void** stored_ptr = (void**)(aligned_addr - sizeof(void*));
    *stored_ptr = raw;

    return (void*)aligned_addr;
}

/* Free aligned memory */
void heap_free_aligned(void* ptr)
{
    if (!ptr) return;

    /* Validate ptr is within heap bounds with room for stored pointer */
    if ((uintptr_t)ptr < (uintptr_t)heap_state.start + sizeof(void*) ||
        (uintptr_t)ptr >= (uintptr_t)heap_state.end) {
        console_printf("Heap: Invalid aligned free pointer 0x%p\n", ptr);
        return;
    }

    /* Retrieve original pointer stored before aligned address */
    void** stored_ptr = (void**)((uintptr_t)ptr - sizeof(void*));
    void* raw = *stored_ptr;

    /* Validate stored pointer is also within heap bounds */
    if ((void*)raw < heap_state.start || (void*)raw >= heap_state.end) {
        console_printf("Heap: Corrupted aligned pointer metadata at 0x%p\n", ptr);
        return;
    }

    heap_free(raw);
}

/* Get heap statistics */
void heap_stats(void)
{
    console_printf("Heap Statistics:\n");
    console_printf("  Total: %zu MB\n", heap_state.total_size / (1024 * 1024));
    console_printf("  Used:  %zu KB\n", heap_state.used_size / 1024);
    console_printf("  Free:  %zu MB\n", (heap_state.total_size - heap_state.used_size) / (1024 * 1024));

    /* Count free blocks */
    int free_blocks = 0;
    block_header_t *current = heap_state.free_list;
    while (current) {
        if (!current->used) free_blocks++;
        current = current->next;
    }
    console_printf("  Free blocks: %d\n", free_blocks);
}