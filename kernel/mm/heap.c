/* Simple heap allocator for AI workloads */

#include "embodios/types.h"
#include "embodios/mm.h"
#include "embodios/console.h"

/* Heap configuration */
#define HEAP_START      0x10000000  /* 256MB mark */
#define HEAP_SIZE       (256 * 1024 * 1024)  /* 256MB heap */
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

/* Initialize heap */
void heap_init(void)
{
    heap_state.start = (void*)HEAP_START;
    heap_state.end = (void*)(HEAP_START + HEAP_SIZE);
    heap_state.total_size = HEAP_SIZE;
    heap_state.used_size = sizeof(block_header_t);
    
    /* Create initial free block */
    block_header_t *initial = (block_header_t*)heap_state.start;
    initial->size = HEAP_SIZE - sizeof(block_header_t);
    initial->used = false;
    initial->next = NULL;
    initial->prev = NULL;
    
    heap_state.free_list = initial;
    heap_state.initialized = true;
    
    console_printf("Heap: Initialized %zu MB at 0x%p\n", 
                   HEAP_SIZE / (1024 * 1024), heap_state.start);
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