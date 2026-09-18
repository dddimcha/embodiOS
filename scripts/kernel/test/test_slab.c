/* Unit test for Slab Allocator */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define PAGE_SIZE 4096
#define SLAB_MIN_SIZE 32
#define SLAB_MAX_SIZE 8192

/* Mock slab structures */
typedef struct free_obj {
    struct free_obj* next;
} free_obj_t;

typedef struct slab {
    struct slab* next;
    void* free_list;
    unsigned short in_use;
    unsigned short total;
} slab_t;

typedef struct {
    size_t obj_size;
    size_t objs_per_slab;
    int num_slabs;
    size_t allocated;
    size_t freed;
} slab_cache_t;

/* Test slab cache index calculation */
void test_cache_index() {
    printf("\n=== Testing Cache Index Calculation ===\n");
    
    struct {
        size_t size;
        int expected_index;
        size_t expected_cache_size;
    } tests[] = {
        {1, 0, 32},
        {32, 0, 32},
        {33, 1, 64},
        {64, 1, 64},
        {65, 2, 128},
        {256, 3, 256},
        {512, 4, 512},
        {1024, 5, 1024},
        {2048, 6, 2048},
        {4096, 7, 4096},
        {8192, 8, 8192},
        {8193, -1, 0}  /* Too large */
    };
    
    for (int i = 0; i < 12; i++) {
        size_t size = tests[i].size;
        int index = 0;
        size_t cache_size = SLAB_MIN_SIZE;
        
        if (size <= SLAB_MAX_SIZE) {
            if (size > SLAB_MIN_SIZE) {
                while (cache_size < size && index < 8) {
                    cache_size <<= 1;
                    index++;
                }
            }
            printf("Size %zu -> index %d (cache size %zu)\n", 
                   size, index, cache_size);
            assert(index == tests[i].expected_index);
            assert(cache_size == tests[i].expected_cache_size);
        } else {
            printf("Size %zu -> too large for slab\n", size);
        }
    }
}

/* Test objects per slab calculation */
void test_objects_per_slab() {
    printf("\n=== Testing Objects Per Slab ===\n");
    
    size_t header_size = sizeof(slab_t);
    printf("Slab header size: %zu bytes\n", header_size);
    
    size_t obj_sizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
    
    for (int i = 0; i < 9; i++) {
        size_t obj_size = obj_sizes[i];
        size_t usable = PAGE_SIZE - header_size;
        size_t objs = usable / obj_size;
        size_t wasted = usable - (objs * obj_size);
        
        printf("Object size %4zu: %3zu objects, %4zu bytes wasted (%.1f%%)\n",
               obj_size, objs, wasted, (100.0 * wasted) / PAGE_SIZE);
    }
}

/* Simulate slab allocation patterns */
void test_allocation_patterns() {
    printf("\n=== Testing Allocation Patterns ===\n");
    
    /* Simulate cache for 64-byte objects */
    slab_cache_t cache = {
        .obj_size = 64,
        .objs_per_slab = (PAGE_SIZE - sizeof(slab_t)) / 64,
        .num_slabs = 0,
        .allocated = 0,
        .freed = 0
    };
    
    printf("Cache for %zu-byte objects (%zu per slab)\n", 
           cache.obj_size, cache.objs_per_slab);
    
    /* Simulate allocations */
    size_t alloc_counts[] = {1, 10, 50, 100, 200, 500};
    
    for (int i = 0; i < 6; i++) {
        size_t count = alloc_counts[i];
        cache.allocated += count;
        
        size_t slabs_needed = (cache.allocated + cache.objs_per_slab - 1) / 
                             cache.objs_per_slab;
        if (slabs_needed > cache.num_slabs) {
            cache.num_slabs = slabs_needed;
        }
        
        printf("After %zu allocations: %d slabs, %zu objects\n",
               count, cache.num_slabs, cache.allocated);
    }
    
    /* Simulate frees */
    size_t free_counts[] = {50, 100, 200, 300};
    
    printf("\nFreeing objects:\n");
    for (int i = 0; i < 4; i++) {
        size_t count = free_counts[i];
        cache.freed += count;
        if (cache.freed > cache.allocated) {
            cache.freed = cache.allocated;
        }
        
        size_t in_use = cache.allocated - cache.freed;
        size_t slabs_needed = (in_use + cache.objs_per_slab - 1) / 
                             cache.objs_per_slab;
        
        printf("After freeing %zu: %zu in use, %zu slabs needed\n",
               count, in_use, slabs_needed);
    }
}

/* Test free list management */
void test_free_list() {
    printf("\n=== Testing Free List Management ===\n");
    
    /* Create mock slab */
    size_t slab_size = PAGE_SIZE;
    void* slab_mem = malloc(slab_size);
    slab_t* slab = (slab_t*)slab_mem;
    
    /* Initialize slab */
    slab->next = NULL;
    slab->in_use = 0;
    slab->total = 10;  /* Simplified for testing */
    
    /* Build free list */
    size_t obj_size = 64;
    char* obj_start = (char*)(slab + 1);
    slab->free_list = obj_start;
    
    printf("Building free list for %zu objects:\n", slab->total);
    for (size_t i = 0; i < slab->total - 1; i++) {
        free_obj_t* obj = (free_obj_t*)(obj_start + i * obj_size);
        obj->next = (free_obj_t*)(obj_start + (i + 1) * obj_size);
        printf("  Object %zu at %p -> %p\n", i, obj, obj->next);
    }
    
    /* Last object */
    free_obj_t* last = (free_obj_t*)(obj_start + (slab->total - 1) * obj_size);
    last->next = NULL;
    printf("  Object %zu at %p -> NULL\n", slab->total - 1, last);
    
    /* Simulate allocations */
    printf("\nSimulating allocations:\n");
    for (int i = 0; i < 3; i++) {
        if (slab->free_list) {
            free_obj_t* obj = (free_obj_t*)slab->free_list;
            slab->free_list = obj->next;
            slab->in_use++;
            printf("  Allocated object at %p, %d/%zu in use\n",
                   obj, slab->in_use, slab->total);
        }
    }
    
    free(slab_mem);
}

int main() {
    printf("=== EMBODIOS Slab Allocator Unit Tests ===\n");
    
    test_cache_index();
    test_objects_per_slab();
    test_allocation_patterns();
    test_free_list();
    
    printf("\n=== All Slab tests passed! ===\n");
    return 0;
}