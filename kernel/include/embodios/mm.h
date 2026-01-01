#ifndef EMBODIOS_MM_H
#define EMBODIOS_MM_H

#include <embodios/types.h>

/* Memory zones */
#define ZONE_DMA        0   /* 0-16MB */
#define ZONE_NORMAL     1   /* 16MB-896MB */
#define ZONE_HIGH       2   /* >896MB */

/* Page flags */
#define PAGE_PRESENT    (1 << 0)
#define PAGE_WRITABLE   (1 << 1)
#define PAGE_USER       (1 << 2)
#define PAGE_ACCESSED   (1 << 3)
#define PAGE_DIRTY      (1 << 4)

/* Physical memory management */
void pmm_init(void* start, size_t size);
void* pmm_alloc_page(void);
void* pmm_alloc_pages(size_t count);
void pmm_free_page(void* page);
void pmm_free_pages(void* page, size_t count);
size_t pmm_available_pages(void);

/* Virtual memory management */
void vmm_init(void);
void* vmm_alloc(size_t size);
void vmm_free(void* addr, size_t size);
void vmm_map(void* vaddr, void* paddr, size_t size, uint32_t flags);
void vmm_unmap(void* vaddr, size_t size);

/* Slab allocator */
void slab_init(void);
void* kmalloc(size_t size);
void kfree(void* ptr);
void* kzalloc(size_t size);  /* Zeroed allocation */
void* krealloc(void* ptr, size_t new_size);

/* Memory operations */
void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);
void* memmove(void* dest, const void* src, size_t n);
int memcmp(const void* s1, const void* s2, size_t n);

/* Heap allocator (separate from slab) - for AI workloads */
void heap_init(void);
void* heap_alloc(size_t size);
void* heap_alloc_aligned(size_t size, size_t alignment);
void heap_free(void* ptr);
void heap_free_aligned(void* ptr);
void heap_stats(void);

/* PMM statistics */
void pmm_print_stats(void);

#endif /* EMBODIOS_MM_H */