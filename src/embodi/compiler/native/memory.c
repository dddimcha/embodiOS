/* EMBODIOS Memory Management - Bare Metal Implementation */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Memory constants */
#define PAGE_SIZE 4096
#define HEAP_START 0x100000  /* 1MB mark */
#define HEAP_SIZE (512 * 1024 * 1024)  /* 512MB heap */

/* Memory block header */
typedef struct block {
    size_t size;
    struct block* next;
    struct block* prev;
    bool free;
} block_t;

/* Memory allocator state */
static block_t* heap_start = NULL;
static block_t* free_list = NULL;
static size_t total_allocated = 0;
static size_t total_free = 0;

/* Page allocator for large allocations */
typedef struct page {
    struct page* next;
    size_t num_pages;
    bool allocated;
} page_t;

static page_t* page_list = NULL;
static uint8_t* next_page = (uint8_t*)HEAP_START;

/* Initialize memory management */
void memory_init(void) {
    /* Set up initial heap block */
    heap_start = (block_t*)HEAP_START;
    heap_start->size = HEAP_SIZE - sizeof(block_t);
    heap_start->next = NULL;
    heap_start->prev = NULL;
    heap_start->free = true;
    
    free_list = heap_start;
    total_free = heap_start->size;
    
    /* Initialize page allocator */
    page_list = NULL;
    next_page = (uint8_t*)HEAP_START + HEAP_SIZE;
}

/* Align size to 8-byte boundary */
static inline size_t align_size(size_t size) {
    return (size + 7) & ~7;
}

/* Find a free block of at least the requested size */
static block_t* find_free_block(size_t size) {
    block_t* current = free_list;
    
    while (current) {
        if (current->free && current->size >= size) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

/* Split a block if it's significantly larger than needed */
static void split_block(block_t* block, size_t size) {
    /* Only split if remaining size is worth it */
    if (block->size > size + sizeof(block_t) + 64) {
        /* Create new block */
        block_t* new_block = (block_t*)((uint8_t*)block + sizeof(block_t) + size);
        new_block->size = block->size - size - sizeof(block_t);
        new_block->free = true;
        new_block->next = block->next;
        new_block->prev = block;
        
        if (block->next) {
            block->next->prev = new_block;
        }
        
        block->next = new_block;
        block->size = size;
        
        /* Update free size */
        total_free += sizeof(block_t);
    }
}

/* Coalesce adjacent free blocks */
static void coalesce_blocks(block_t* block) {
    /* Merge with next block if free */
    if (block->next && block->next->free) {
        block->size += sizeof(block_t) + block->next->size;
        block->next = block->next->next;
        if (block->next) {
            block->next->prev = block;
        }
    }
    
    /* Merge with previous block if free */
    if (block->prev && block->prev->free) {
        block->prev->size += sizeof(block_t) + block->size;
        block->prev->next = block->next;
        if (block->next) {
            block->next->prev = block->prev;
        }
        block = block->prev;
    }
}

/* Allocate memory */
void* malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    
    size = align_size(size);
    
    /* For large allocations, use page allocator */
    if (size > PAGE_SIZE * 2) {
        return page_alloc((size + PAGE_SIZE - 1) / PAGE_SIZE);
    }
    
    /* Find a free block */
    block_t* block = find_free_block(size);
    if (!block) {
        /* No suitable block found */
        return NULL;
    }
    
    /* Split block if needed */
    split_block(block, size);
    
    /* Mark as allocated */
    block->free = false;
    total_allocated += block->size;
    total_free -= block->size;
    
    /* Return pointer to usable memory */
    return (uint8_t*)block + sizeof(block_t);
}

/* Free memory */
void free(void* ptr) {
    if (!ptr) {
        return;
    }
    
    /* Get block header */
    block_t* block = (block_t*)((uint8_t*)ptr - sizeof(block_t));
    
    /* Check if it's a page allocation */
    if ((uint8_t*)block >= next_page - HEAP_SIZE) {
        page_free(ptr);
        return;
    }
    
    /* Mark as free */
    block->free = true;
    total_allocated -= block->size;
    total_free += block->size;
    
    /* Coalesce adjacent free blocks */
    coalesce_blocks(block);
}

/* Reallocate memory */
void* realloc(void* ptr, size_t new_size) {
    if (!ptr) {
        return malloc(new_size);
    }
    
    if (new_size == 0) {
        free(ptr);
        return NULL;
    }
    
    /* Get current block */
    block_t* block = (block_t*)((uint8_t*)ptr - sizeof(block_t));
    size_t old_size = block->size;
    
    /* If new size fits in current block, we're done */
    new_size = align_size(new_size);
    if (new_size <= old_size) {
        /* Optionally split the block if much smaller */
        if (old_size - new_size > sizeof(block_t) + 64) {
            split_block(block, new_size);
        }
        return ptr;
    }
    
    /* Need to allocate new block */
    void* new_ptr = malloc(new_size);
    if (!new_ptr) {
        return NULL;
    }
    
    /* Copy old data */
    uint8_t* src = (uint8_t*)ptr;
    uint8_t* dst = (uint8_t*)new_ptr;
    for (size_t i = 0; i < old_size; i++) {
        dst[i] = src[i];
    }
    
    /* Free old block */
    free(ptr);
    
    return new_ptr;
}

/* Allocate zeroed memory */
void* calloc(size_t count, size_t size) {
    size_t total = count * size;
    void* ptr = malloc(total);
    
    if (ptr) {
        /* Zero the memory */
        uint8_t* p = (uint8_t*)ptr;
        for (size_t i = 0; i < total; i++) {
            p[i] = 0;
        }
    }
    
    return ptr;
}

/* Page allocator for large allocations */
void* page_alloc(size_t num_pages) {
    /* Align to page boundary */
    if ((uintptr_t)next_page & (PAGE_SIZE - 1)) {
        next_page = (uint8_t*)(((uintptr_t)next_page + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    }
    
    /* Check if we have enough space */
    if (next_page + (num_pages * PAGE_SIZE) > (uint8_t*)HEAP_START + HEAP_SIZE * 2) {
        return NULL;
    }
    
    /* Create page record */
    page_t* page = (page_t*)malloc(sizeof(page_t));
    if (!page) {
        return NULL;
    }
    
    page->next = page_list;
    page->num_pages = num_pages;
    page->allocated = true;
    page_list = page;
    
    /* Allocate pages */
    void* ptr = next_page;
    next_page += num_pages * PAGE_SIZE;
    
    /* Zero the pages */
    uint8_t* p = (uint8_t*)ptr;
    for (size_t i = 0; i < num_pages * PAGE_SIZE; i++) {
        p[i] = 0;
    }
    
    return ptr;
}

/* Free pages */
void page_free(void* ptr) {
    /* Find page record */
    page_t* prev = NULL;
    page_t* page = page_list;
    
    while (page) {
        /* Check if this page contains the pointer */
        /* Note: This is simplified - real implementation would track page addresses */
        if (page->allocated) {
            page->allocated = false;
            /* In a real implementation, we'd return pages to the free pool */
            return;
        }
        prev = page;
        page = page->next;
    }
}

/* Get memory statistics */
void memory_stats(size_t* allocated, size_t* free_mem, size_t* total) {
    if (allocated) *allocated = total_allocated;
    if (free_mem) *free_mem = total_free;
    if (total) *total = HEAP_SIZE;
}

/* Memory copy optimized for different sizes */
void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    
    /* Copy 64-bit chunks if aligned */
    if (((uintptr_t)d & 7) == 0 && ((uintptr_t)s & 7) == 0) {
        uint64_t* d64 = (uint64_t*)d;
        const uint64_t* s64 = (const uint64_t*)s;
        size_t n64 = n / 8;
        
        while (n64--) {
            *d64++ = *s64++;
        }
        
        d = (uint8_t*)d64;
        s = (const uint8_t*)s64;
        n &= 7;
    }
    
    /* Copy remaining bytes */
    while (n--) {
        *d++ = *s++;
    }
    
    return dest;
}

/* Memory set */
void* memset(void* s, int c, size_t n) {
    uint8_t* p = (uint8_t*)s;
    uint8_t byte = (uint8_t)c;
    
    /* Set 64-bit chunks if aligned */
    if (((uintptr_t)p & 7) == 0 && n >= 8) {
        uint64_t* p64 = (uint64_t*)p;
        uint64_t val64 = byte;
        val64 |= val64 << 8;
        val64 |= val64 << 16;
        val64 |= val64 << 32;
        
        size_t n64 = n / 8;
        while (n64--) {
            *p64++ = val64;
        }
        
        p = (uint8_t*)p64;
        n &= 7;
    }
    
    /* Set remaining bytes */
    while (n--) {
        *p++ = byte;
    }
    
    return s;
}

/* Memory compare */
int memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* p1 = (const uint8_t*)s1;
    const uint8_t* p2 = (const uint8_t*)s2;
    
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    
    return 0;
}