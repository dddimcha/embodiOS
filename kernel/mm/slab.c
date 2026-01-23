/* EMBODIOS Slab Allocator */
#include <embodios/mm.h>
#include <embodios/kernel.h>
#include <embodios/console.h>

/* Direct serial output for debug - bypasses console */
#if defined(__x86_64__)
static inline void serial_out(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t serial_in(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void slab_debug_char(char c) {
    while (!(serial_in(0x3FD) & 0x20));
    serial_out(0x3F8, c);
}
#elif defined(__aarch64__)
/* Use assembly UART for HVF compatibility */
extern void uart_putchar(char c);
static inline void slab_debug_char(char c) {
    uart_putchar(c);
}
#else
static inline void slab_debug_char(char c) { (void)c; }
#endif
static inline void slab_debug_str(const char* s) {
    while (*s) slab_debug_char(*s++);
}

/* Slab sizes (powers of 2) */
#define SLAB_MIN_SIZE   32
#define SLAB_MAX_SIZE   8192
#define SLAB_NUM_SIZES  9  /* 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192 */

/* Slab cache structure */
struct slab {
    struct slab* next;
    void* free_list;
    uint16_t in_use;
    uint16_t total;
};

struct slab_cache {
    struct slab* partial;   /* Partially full slabs */
    struct slab* full;      /* Completely full slabs */
    struct slab* empty;     /* Empty slabs (for reuse) */
    size_t obj_size;
    size_t obj_per_slab;
    size_t total_objs;
    size_t free_objs;
};

/* Free object header */
struct free_obj {
    struct free_obj* next;
};

/* Slab allocator state */
static struct {
    struct slab_cache caches[SLAB_NUM_SIZES];
    bool initialized;
} slab_state = {
    .initialized = false
};

/* Get slab cache index for size */
static int get_cache_index(size_t size)
{
    if (size <= SLAB_MIN_SIZE) return 0;
    
    int index = 0;
    size_t cache_size = SLAB_MIN_SIZE;
    
    while (cache_size < size && index < SLAB_NUM_SIZES - 1) {
        cache_size <<= 1;
        index++;
    }
    
    return index;
}

/* Initialize a slab */
static void init_slab(struct slab* slab, const struct slab_cache* cache)
{
    slab->next = NULL;
    slab->in_use = 0;
    slab->total = cache->obj_per_slab;
    
    /* Initialize free list */
    char* obj = (char*)(slab + 1);
    slab->free_list = obj;
    
    for (size_t i = 0; i < cache->obj_per_slab - 1; i++) {
        struct free_obj* free = (struct free_obj*)obj;
        free->next = (struct free_obj*)(obj + cache->obj_size);
        obj += cache->obj_size;
    }
    
    /* Last object points to NULL */
    struct free_obj* last = (struct free_obj*)obj;
    last->next = NULL;
}

/* Allocate a new slab */
static struct slab* alloc_slab(struct slab_cache* cache)
{
    /* Allocate a page for the slab */
    void* page = pmm_alloc_page();
    if (!page) return NULL;
    
    struct slab* slab = (struct slab*)page;
    init_slab(slab, cache);
    
    return slab;
}

/* Free a slab */
static void free_slab(struct slab* slab)
{
    pmm_free_page(slab);
}

/* Move slab between lists */
static void move_slab(struct slab** from, struct slab** to, struct slab* slab)
{
    /* Remove from source list */
    if (*from == slab) {
        *from = slab->next;
    } else {
        struct slab* prev = *from;
        while (prev && prev->next != slab) {
            prev = prev->next;
        }
        if (prev) {
            prev->next = slab->next;
        }
    }
    
    /* Add to destination list */
    slab->next = *to;
    *to = slab;
}

/* Initialize slab allocator - debug version */
void slab_init(void)
{
    slab_debug_str("SLAB: Enter\n");

    /* Test writing to slab_state directly */
    slab_state.initialized = false;  /* Write to simple bool */

    /* Try getting address of caches array */
    volatile struct slab_cache* cache_ptr = &slab_state.caches[0];

    /* Try writing through pointer */
    cache_ptr->partial = NULL;
    cache_ptr->obj_size = 32;

    /* Skip full init, just mark as done */
    slab_debug_str("\nSLAB: Done (disabled)\n");
    slab_state.initialized = false;  /* Disabled for now */
}

/* Allocate memory from slab */
void* kmalloc(size_t size)
{
    /* Fall back to heap allocator when SLAB is disabled */
    if (!slab_state.initialized) {
        extern void* heap_alloc(size_t size);
        return heap_alloc(size);
    }
    if (size == 0) {
        return NULL;
    }
    
    /* For large allocations, use page allocator directly */
    if (size > SLAB_MAX_SIZE) {
        size_t pages = ALIGN_UP(size + sizeof(size_t), PAGE_SIZE) / PAGE_SIZE;
        void* ptr = pmm_alloc_pages(pages);
        if (ptr) {
            /* Store size for kfree */
            *(size_t*)ptr = size;
            return (char*)ptr + sizeof(size_t);
        }
        return NULL;
    }
    
    /* Get appropriate cache */
    int index = get_cache_index(size);
    struct slab_cache* cache = &slab_state.caches[index];
    
    /* Try partial slabs first */
    struct slab* slab = cache->partial;
    
    if (!slab) {
        /* Try empty slabs */
        if (cache->empty) {
            slab = cache->empty;
            move_slab(&cache->empty, &cache->partial, slab);
        } else {
            /* Allocate new slab */
            slab = alloc_slab(cache);
            if (!slab) return NULL;
            
            /* Add to partial list */
            slab->next = cache->partial;
            cache->partial = slab;
            
            cache->total_objs += slab->total;
            cache->free_objs += slab->total;
        }
    }
    
    /* Allocate object from slab */
    struct free_obj* obj = (struct free_obj*)slab->free_list;
    slab->free_list = obj->next;
    slab->in_use++;
    cache->free_objs--;
    
    /* Move to full list if necessary */
    if (slab->in_use == slab->total) {
        move_slab(&cache->partial, &cache->full, slab);
    }
    
    return obj;
}

/* Free memory to slab */
void kfree(void* ptr)
{
    if (!ptr) {
        return;
    }
    /* Fall back to heap allocator when SLAB is disabled */
    if (!slab_state.initialized) {
        extern void heap_free(void* ptr);
        heap_free(ptr);
        return;
    }
    
    /* Check if it's a large allocation */
    if ((uintptr_t)ptr & (PAGE_SIZE - 1)) {
        /* Find which slab this belongs to */
        struct slab* slab = (struct slab*)((uintptr_t)ptr & ~(PAGE_SIZE - 1));
        
        /* Find the cache */
        struct slab_cache* cache = NULL;
        for (int i = 0; i < SLAB_NUM_SIZES; i++) {
            size_t obj_size = slab_state.caches[i].obj_size;
            void* slab_start = (void*)(slab + 1);
            const void* slab_end = (void*)((char*)slab + PAGE_SIZE);
            
            if (ptr >= slab_start && ptr < slab_end &&
                ((uintptr_t)ptr - (uintptr_t)slab_start) % obj_size == 0) {
                cache = &slab_state.caches[i];
                break;
            }
        }
        
        if (!cache) {
            console_printf("SLAB: Invalid free of %p\n", ptr);
            return;
        }
        
        /* Add to free list */
        struct free_obj* obj = (struct free_obj*)ptr;
        obj->next = slab->free_list;
        slab->free_list = obj;
        slab->in_use--;
        cache->free_objs++;
        
        /* Move slab if necessary */
        if (slab->in_use == 0) {
            /* Move to empty list */
            if (cache->partial && slab->in_use < slab->total) {
                move_slab(&cache->partial, &cache->empty, slab);
            } else if (cache->full) {
                move_slab(&cache->full, &cache->empty, slab);
            }
            
            /* Free excess empty slabs */
            if (cache->free_objs > cache->total_objs / 2) {
                struct slab* to_free = cache->empty;
                cache->empty = to_free->next;
                cache->total_objs -= to_free->total;
                cache->free_objs -= to_free->total;
                free_slab(to_free);
            }
        } else if (slab->in_use == slab->total - 1) {
            /* Was full, now partial */
            move_slab(&cache->full, &cache->partial, slab);
        }
    } else {
        /* Large allocation */
        void* real_ptr = (char*)ptr - sizeof(size_t);
        size_t size = *(size_t*)real_ptr;
        size_t pages = ALIGN_UP(size + sizeof(size_t), PAGE_SIZE) / PAGE_SIZE;
        pmm_free_pages(real_ptr, pages);
    }
}

/* Allocate zeroed memory */
void* kzalloc(size_t size)
{
    void* ptr = kmalloc(size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

/* Reallocate memory */
void* krealloc(void* ptr, size_t new_size)
{
    if (!ptr) {
        return kmalloc(new_size);
    }
    
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }
    
    /* For simplicity, always allocate new and copy */
    void* new_ptr = kmalloc(new_size);
    if (!new_ptr) {
        return NULL;
    }
    
    /* Copy old data (we don't track exact sizes, so copy conservatively) */
    size_t copy_size = new_size;
    if ((uintptr_t)ptr & (PAGE_SIZE - 1)) {
        /* Slab allocation - find cache to get max size */
        for (int i = 0; i < SLAB_NUM_SIZES; i++) {
            if (copy_size > slab_state.caches[i].obj_size) {
                copy_size = slab_state.caches[i].obj_size;
                break;
            }
        }
    }
    
    memcpy(new_ptr, ptr, copy_size);
    kfree(ptr);
    
    return new_ptr;
}