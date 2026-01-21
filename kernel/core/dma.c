/* EMBODIOS DMA (Direct Memory Access) Subsystem Implementation
 *
 * Provides DMA-capable memory allocation and scatter-gather support
 * for efficient data transfers by network and storage drivers.
 *
 * Implementation notes:
 * - Uses identity mapping (virt_addr - KERNEL_BASE = phys_addr)
 * - Cache coherency via x86_64 CLFLUSH instruction
 * - Allocation tracking for debugging
 */

#include <embodios/dma.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/kernel.h>

/* ============================================================================
 * Internal Constants
 * ============================================================================ */

/* Kernel base address for virtual-to-physical translation */
#ifndef KERNEL_BASE
#define KERNEL_BASE 0x100000UL
#endif

/* ============================================================================
 * Internal Structures
 * ============================================================================ */

/* Allocation tracking entry */
typedef struct dma_alloc_entry {
    void* virt_addr;
    dma_addr_t dma_addr;
    size_t size;
    bool in_use;
} dma_alloc_entry_t;

/* DMA subsystem state */
typedef struct dma_state {
    bool initialized;
    dma_alloc_entry_t allocations[DMA_MAX_ALLOCATIONS];
    int alloc_count;
    dma_stats_t stats;
} dma_state_t;

static dma_state_t g_dma = {0};

/* ============================================================================
 * Architecture-Specific Cache Operations
 * ============================================================================ */

#ifdef __x86_64__

/**
 * Flush cache line containing address
 */
static inline void cache_clflush(void* addr) {
    __asm__ volatile("clflush (%0)" : : "r"(addr) : "memory");
}

/**
 * Memory fence - ensure all memory operations complete
 */
static inline void cache_mfence(void) {
    __asm__ volatile("mfence" ::: "memory");
}

/**
 * Flush cache for a memory range
 */
static void cache_flush_range(void* addr, size_t size) {
    if (!addr || size == 0) return;

    /* Align to cache line boundary */
    uintptr_t start = (uintptr_t)addr & ~(DMA_CACHE_LINE_SIZE - 1);
    uintptr_t end = (uintptr_t)addr + size;

    for (uintptr_t p = start; p < end; p += DMA_CACHE_LINE_SIZE) {
        cache_clflush((void*)p);
    }
    cache_mfence();
}

/**
 * Invalidate cache for a memory range
 * Note: x86 CLFLUSH both flushes and invalidates
 */
static void cache_invalidate_range(void* addr, size_t size) {
    cache_flush_range(addr, size);
}

#else /* ARM64 or other */

/**
 * Flush cache line containing address (ARM64)
 */
static inline void cache_dc_cvac(void* addr) {
    __asm__ volatile("dc cvac, %0" : : "r"(addr) : "memory");
}

/**
 * Data synchronization barrier (ARM64)
 */
static inline void cache_dsb(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

/**
 * Flush cache for a memory range (ARM64)
 */
static void cache_flush_range(void* addr, size_t size) {
    if (!addr || size == 0) return;

    /* Align to cache line boundary */
    uintptr_t start = (uintptr_t)addr & ~(DMA_CACHE_LINE_SIZE - 1);
    uintptr_t end = (uintptr_t)addr + size;

    for (uintptr_t p = start; p < end; p += DMA_CACHE_LINE_SIZE) {
        cache_dc_cvac((void*)p);
    }
    cache_dsb();
}

static void cache_invalidate_range(void* addr, size_t size) {
    (void)addr;
    (void)size;
    /* TODO: Implement ARM64 cache invalidation */
}

#endif

/**
 * Perform cache operations based on DMA direction
 * @param vaddr     Virtual address
 * @param size      Size of region
 * @param dir       DMA direction
 * @param for_device true = sync for device, false = sync for CPU
 */
static void cache_sync_direction(void* vaddr, size_t size,
                                  dma_direction_t dir, bool for_device) {
    if (!vaddr || size == 0) return;

    if (for_device) {
        /* Sync for device: flush if CPU wrote data */
        if (dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL) {
            cache_flush_range(vaddr, size);
        }
    } else {
        /* Sync for CPU: invalidate if device wrote data */
        if (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL) {
            cache_invalidate_range(vaddr, size);
        }
    }
}

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * Find a free allocation slot
 */
static int find_free_slot(void) {
    for (int i = 0; i < DMA_MAX_ALLOCATIONS; i++) {
        if (!g_dma.allocations[i].in_use) {
            return i;
        }
    }
    return -1;
}

/**
 * Find allocation by virtual address
 */
static int find_alloc_by_vaddr(void* vaddr) {
    for (int i = 0; i < DMA_MAX_ALLOCATIONS; i++) {
        if (g_dma.allocations[i].in_use &&
            g_dma.allocations[i].virt_addr == vaddr) {
            return i;
        }
    }
    return -1;
}

/**
 * Find allocation by DMA address
 */
static int find_alloc_by_dma(dma_addr_t dma_addr) {
    for (int i = 0; i < DMA_MAX_ALLOCATIONS; i++) {
        if (g_dma.allocations[i].in_use &&
            g_dma.allocations[i].dma_addr == dma_addr) {
            return i;
        }
    }
    return -1;
}

/**
 * Align size up to cache line boundary
 */
static inline size_t align_size(size_t size) {
    return (size + DMA_CACHE_LINE_SIZE - 1) & ~(DMA_CACHE_LINE_SIZE - 1);
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

int dma_init(void) {
    if (g_dma.initialized) {
        return DMA_ERR_ALREADY_INIT;
    }

    /* Clear all state (memset zeros in_use flags to false) */
    memset(&g_dma, 0, sizeof(g_dma));

    g_dma.initialized = true;

    console_printf("[DMA] Subsystem initialized\n");
    console_printf("[DMA] Max allocations: %d, Cache line: %d bytes\n",
                   DMA_MAX_ALLOCATIONS, DMA_CACHE_LINE_SIZE);

    return DMA_OK;
}

bool dma_is_initialized(void) {
    return g_dma.initialized;
}

/* ============================================================================
 * Address Translation
 * ============================================================================ */

dma_addr_t virt_to_dma(void* vaddr) {
    if (!vaddr) {
        return DMA_ADDR_INVALID;
    }

    /*
     * EMBODIOS runs with identity mapping (virt == phys).
     * The boot.S comment confirms: "we're identity-mapped, no higher half needed"
     * So we just return the address as-is.
     */
    return (dma_addr_t)(uintptr_t)vaddr;
}

void* dma_to_virt(dma_addr_t dma_addr) {
    if (dma_addr == DMA_ADDR_INVALID) {
        return NULL;
    }

    /* Identity mapping: virtual == physical */
    return (void*)(uintptr_t)dma_addr;
}

/* ============================================================================
 * Coherent Memory Allocation
 * ============================================================================ */

void* dma_alloc_coherent(size_t size, dma_addr_t* dma_handle) {
    if (!g_dma.initialized) {
        console_printf("[DMA] Error: Not initialized\n");
        return NULL;
    }

    if (size == 0) {
        return NULL;
    }

    /* Align size to cache line */
    size_t aligned_size = align_size(size);

    /* Find free tracking slot */
    int slot = find_free_slot();
    if (slot < 0) {
        console_printf("[DMA] Error: No free allocation slots\n");
        return NULL;
    }

    /* Allocate aligned memory using heap allocator */
    void* vaddr = heap_alloc_aligned(aligned_size, DMA_MIN_ALIGNMENT);
    if (!vaddr) {
        console_printf("[DMA] Error: Failed to allocate %zu bytes\n", aligned_size);
        return NULL;
    }

    /* Zero the memory for safety */
    memset(vaddr, 0, aligned_size);

    /* Get DMA address */
    dma_addr_t dma_addr = virt_to_dma(vaddr);

    /* Track allocation */
    g_dma.allocations[slot].virt_addr = vaddr;
    g_dma.allocations[slot].dma_addr = dma_addr;
    g_dma.allocations[slot].size = aligned_size;
    g_dma.allocations[slot].in_use = true;
    g_dma.alloc_count++;

    /* Update statistics */
    g_dma.stats.alloc_count++;
    g_dma.stats.bytes_allocated += aligned_size;
    g_dma.stats.active_allocations++;
    if (g_dma.stats.bytes_allocated > g_dma.stats.peak_allocated) {
        g_dma.stats.peak_allocated = g_dma.stats.bytes_allocated;
    }

    if (dma_handle) {
        *dma_handle = dma_addr;
    }
    return vaddr;
}

void dma_free_coherent(void* vaddr, size_t size, dma_addr_t dma_handle) {
    if (!g_dma.initialized || !vaddr) {
        return;
    }

    (void)size;  /* Size not strictly needed for our allocator */
    (void)dma_handle;

    /* Find and remove from tracking */
    int slot = find_alloc_by_vaddr(vaddr);
    if (slot >= 0) {
        size_t freed_size = g_dma.allocations[slot].size;

        g_dma.allocations[slot].in_use = false;
        g_dma.allocations[slot].virt_addr = NULL;
        g_dma.allocations[slot].dma_addr = 0;
        g_dma.allocations[slot].size = 0;
        g_dma.alloc_count--;

        /* Update statistics */
        g_dma.stats.free_count++;
        g_dma.stats.bytes_allocated -= freed_size;
        g_dma.stats.active_allocations--;
    }

    /* Free the memory */
    heap_free_aligned(vaddr);
}

/* ============================================================================
 * Streaming DMA Mapping
 * ============================================================================ */

dma_addr_t dma_map_single(void* vaddr, size_t size, dma_direction_t dir) {
    if (!g_dma.initialized || !vaddr || size == 0) {
        return DMA_ADDR_INVALID;
    }

    /* Sync cache for device access */
    cache_sync_direction(vaddr, size, dir, true);

    g_dma.stats.map_count++;

    return virt_to_dma(vaddr);
}

void dma_unmap_single(dma_addr_t dma_addr, size_t size, dma_direction_t dir) {
    if (!g_dma.initialized || dma_addr == DMA_ADDR_INVALID) {
        return;
    }

    void* vaddr = dma_to_virt(dma_addr);
    if (!vaddr) {
        return;
    }

    /* Sync cache for CPU access */
    cache_sync_direction(vaddr, size, dir, false);

    g_dma.stats.unmap_count++;
}

/* ============================================================================
 * Cache Synchronization
 * ============================================================================ */

void dma_sync_for_device(dma_addr_t dma_addr, size_t size, dma_direction_t dir) {
    if (dma_addr == DMA_ADDR_INVALID || size == 0) {
        return;
    }

    void* vaddr = dma_to_virt(dma_addr);
    cache_sync_direction(vaddr, size, dir, true);
}

void dma_sync_for_cpu(dma_addr_t dma_addr, size_t size, dma_direction_t dir) {
    if (dma_addr == DMA_ADDR_INVALID || size == 0) {
        return;
    }

    void* vaddr = dma_to_virt(dma_addr);
    cache_sync_direction(vaddr, size, dir, false);
}

/* ============================================================================
 * Scatter-Gather Operations
 * ============================================================================ */

int dma_sg_init(dma_sg_list_t* sg, int max_entries) {
    if (!g_dma.initialized) {
        return DMA_ERR_NOT_INIT;
    }

    if (!sg || max_entries <= 0) {
        return DMA_ERR_INVALID;
    }

    if (max_entries > DMA_SG_MAX_ENTRIES) {
        return DMA_ERR_INVALID;  /* Reject instead of silent clamp */
    }

    /* Allocate entries array */
    size_t entries_size = max_entries * sizeof(dma_sg_entry_t);
    sg->entries = (dma_sg_entry_t*)kmalloc(entries_size);
    if (!sg->entries) {
        return DMA_ERR_NOMEM;
    }

    memset(sg->entries, 0, entries_size);
    sg->count = 0;
    sg->capacity = max_entries;
    sg->mapped = false;
    sg->direction = DMA_TO_DEVICE;

    return DMA_OK;
}

int dma_sg_add(dma_sg_list_t* sg, void* vaddr, size_t length) {
    if (!sg || !sg->entries || !vaddr || length == 0) {
        return DMA_ERR_INVALID;
    }

    if (sg->count >= sg->capacity) {
        return DMA_ERR_FULL;
    }

    if (sg->mapped) {
        /* Cannot modify while mapped */
        return DMA_ERR_INVALID;
    }

    /* Add entry */
    dma_sg_entry_t* entry = &sg->entries[sg->count];
    entry->virt_addr = vaddr;
    entry->length = length;
    entry->dma_addr = DMA_ADDR_INVALID;  /* Set during map */

    sg->count++;

    return DMA_OK;
}

int dma_sg_map(dma_sg_list_t* sg, dma_direction_t dir) {
    if (!sg || !sg->entries || sg->count == 0) {
        return DMA_ERR_INVALID;
    }

    if (sg->mapped) {
        return DMA_ERR_INVALID;  /* Already mapped */
    }

    /* Map each entry */
    for (int i = 0; i < sg->count; i++) {
        dma_sg_entry_t* entry = &sg->entries[i];

        /* Translate address */
        entry->dma_addr = virt_to_dma(entry->virt_addr);
        if (entry->dma_addr == DMA_ADDR_INVALID) {
            /* Rollback on failure */
            for (int j = 0; j < i; j++) {
                sg->entries[j].dma_addr = DMA_ADDR_INVALID;
            }
            return DMA_ERR_INVALID;
        }

        /* Sync cache for device access */
        cache_sync_direction(entry->virt_addr, entry->length, dir, true);
    }

    sg->mapped = true;
    sg->direction = dir;
    g_dma.stats.sg_map_count++;

    return DMA_OK;
}

void dma_sg_unmap(dma_sg_list_t* sg, dma_direction_t dir) {
    if (!sg || !sg->entries || !sg->mapped) {
        return;
    }

    /* Sync cache for CPU access on each entry */
    for (int i = 0; i < sg->count; i++) {
        dma_sg_entry_t* entry = &sg->entries[i];
        cache_sync_direction(entry->virt_addr, entry->length, dir, false);
        entry->dma_addr = DMA_ADDR_INVALID;
    }

    sg->mapped = false;
}

void dma_sg_free(dma_sg_list_t* sg) {
    if (!sg) {
        return;
    }

    if (sg->mapped) {
        dma_sg_unmap(sg, sg->direction);
    }

    if (sg->entries) {
        kfree(sg->entries);
        sg->entries = NULL;
    }

    sg->count = 0;
    sg->capacity = 0;
}

size_t dma_sg_total_length(const dma_sg_list_t* sg) {
    if (!sg || !sg->entries) {
        return 0;
    }

    size_t total = 0;
    for (int i = 0; i < sg->count; i++) {
        total += sg->entries[i].length;
    }
    return total;
}

/* ============================================================================
 * Debugging and Validation
 * ============================================================================ */

int dma_validate_address(dma_addr_t addr, size_t size) {
    if (addr == DMA_ADDR_INVALID) {
        return DMA_ERR_INVALID;
    }

    if (size == 0) {
        return DMA_ERR_INVALID;
    }

    /* Check for overflow */
    if (addr + size < addr) {
        return DMA_ERR_OVERFLOW;
    }

    /* Check if within reasonable bounds */
    if (addr + size > DMA_MAX_ADDRESS) {
        return DMA_ERR_OVERFLOW;
    }

    /* Optionally check if this address is from a tracked allocation */
    int slot = find_alloc_by_dma(addr);
    if (slot >= 0) {
        /* Verify size doesn't exceed allocation */
        if (size > g_dma.allocations[slot].size) {
            return DMA_ERR_OVERFLOW;
        }
    }

    return DMA_OK;
}

void dma_dump_allocations(void) {
    console_printf("\n[DMA] Active Allocations:\n");
    console_printf("  %-4s %-18s %-18s %-10s\n",
                   "Slot", "VirtAddr", "DMAAddr", "Size");
    console_printf("  %-4s %-18s %-18s %-10s\n",
                   "----", "------------------", "------------------", "----------");

    int count = 0;
    for (int i = 0; i < DMA_MAX_ALLOCATIONS; i++) {
        if (g_dma.allocations[i].in_use) {
            console_printf("  %-4d 0x%016llx 0x%016llx %zu\n",
                          i,
                          (unsigned long long)(uintptr_t)g_dma.allocations[i].virt_addr,
                          (unsigned long long)g_dma.allocations[i].dma_addr,
                          g_dma.allocations[i].size);
            count++;
        }
    }

    if (count == 0) {
        console_printf("  (no active allocations)\n");
    }

    console_printf("\n");
}

void dma_get_stats(dma_stats_t* stats) {
    if (stats) {
        *stats = g_dma.stats;
    }
}

void dma_print_stats(void) {
    console_printf("\n[DMA] Statistics:\n");
    console_printf("  Coherent allocs:   %llu\n", (unsigned long long)g_dma.stats.alloc_count);
    console_printf("  Coherent frees:    %llu\n", (unsigned long long)g_dma.stats.free_count);
    console_printf("  Streaming maps:    %llu\n", (unsigned long long)g_dma.stats.map_count);
    console_printf("  Streaming unmaps:  %llu\n", (unsigned long long)g_dma.stats.unmap_count);
    console_printf("  SG list maps:      %llu\n", (unsigned long long)g_dma.stats.sg_map_count);
    console_printf("  Bytes allocated:   %llu\n", (unsigned long long)g_dma.stats.bytes_allocated);
    console_printf("  Peak allocated:    %llu\n", (unsigned long long)g_dma.stats.peak_allocated);
    console_printf("  Active allocations: %d\n", g_dma.stats.active_allocations);
    console_printf("\n");
}
