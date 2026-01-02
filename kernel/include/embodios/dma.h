/* EMBODIOS DMA (Direct Memory Access) Subsystem
 *
 * Provides DMA-capable memory allocation and scatter-gather support
 * for efficient data transfers by network and storage drivers.
 *
 * Features:
 * - Coherent memory allocation (physically contiguous)
 * - Streaming DMA mapping for existing buffers
 * - Scatter-gather list construction
 * - Cache synchronization primitives
 * - Debug/validation support
 */

#ifndef EMBODIOS_DMA_H
#define EMBODIOS_DMA_H

#include <embodios/types.h>

/* ============================================================================
 * DMA Constants
 * ============================================================================ */

#define DMA_MAX_ALLOCATIONS     1024    /* Max tracked allocations */
#define DMA_SG_MAX_ENTRIES      256     /* Max scatter-gather entries per list */
#define DMA_CACHE_LINE_SIZE     64      /* x86_64 cache line size */
#define DMA_MIN_ALIGNMENT       64      /* Minimum buffer alignment */
#define DMA_ZONE_LIMIT          0x1000000  /* 16MB ZONE_DMA limit */
#define DMA_MAX_ADDRESS         0x1000000000ULL  /* 64GB max addressable */

/* ============================================================================
 * DMA Error Codes
 * ============================================================================ */

#define DMA_OK                  0
#define DMA_ERR_NOMEM          -1       /* Out of memory */
#define DMA_ERR_INVALID        -2       /* Invalid parameter */
#define DMA_ERR_ALIGNMENT      -3       /* Alignment requirement not met */
#define DMA_ERR_OVERFLOW       -4       /* Size/address overflow */
#define DMA_ERR_NOT_MAPPED     -5       /* Address not mapped */
#define DMA_ERR_ALREADY_INIT   -6       /* Already initialized */
#define DMA_ERR_NOT_INIT       -7       /* Not initialized */
#define DMA_ERR_FULL           -8       /* List/table full */

/* ============================================================================
 * DMA Types
 * ============================================================================ */

/* DMA address type (physical address visible to device) */
typedef uint64_t dma_addr_t;

/* Invalid DMA address marker */
#define DMA_ADDR_INVALID        ((dma_addr_t)~0ULL)

/* DMA transfer direction (for cache management) */
typedef enum dma_direction {
    DMA_TO_DEVICE = 0,      /* CPU writes, device reads (flush cache) */
    DMA_FROM_DEVICE = 1,    /* Device writes, CPU reads (invalidate cache) */
    DMA_BIDIRECTIONAL = 2   /* Both directions (flush + invalidate) */
} dma_direction_t;

/* ============================================================================
 * Scatter-Gather Structures
 * ============================================================================ */

/**
 * Single scatter-gather entry
 * Represents one contiguous memory segment in a DMA transfer
 */
typedef struct dma_sg_entry {
    dma_addr_t dma_addr;    /* Physical/DMA address for device */
    size_t length;          /* Length of this segment in bytes */
    void* virt_addr;        /* Kernel virtual address */
} dma_sg_entry_t;

/**
 * Scatter-gather list
 * Collection of memory segments for multi-buffer DMA transfers
 */
typedef struct dma_sg_list {
    dma_sg_entry_t* entries;    /* Array of SG entries */
    int count;                   /* Number of valid entries */
    int capacity;                /* Maximum entries (allocated size) */
    bool mapped;                 /* True if currently mapped for DMA */
    dma_direction_t direction;   /* Direction when mapped */
} dma_sg_list_t;

/* ============================================================================
 * DMA Statistics (for debugging)
 * ============================================================================ */

typedef struct dma_stats {
    uint64_t alloc_count;       /* Total coherent allocations */
    uint64_t free_count;        /* Total coherent frees */
    uint64_t map_count;         /* Total streaming maps */
    uint64_t unmap_count;       /* Total streaming unmaps */
    uint64_t sg_map_count;      /* Total SG list maps */
    uint64_t bytes_allocated;   /* Current bytes allocated */
    uint64_t peak_allocated;    /* Peak bytes allocated */
    int active_allocations;     /* Current active allocations */
} dma_stats_t;

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * Initialize DMA subsystem
 * Must be called before any other DMA functions
 *
 * @return DMA_OK on success, error code on failure
 */
int dma_init(void);

/**
 * Check if DMA subsystem is initialized
 *
 * @return true if initialized
 */
bool dma_is_initialized(void);

/* ============================================================================
 * Coherent Memory Allocation
 * ============================================================================ */

/**
 * Allocate DMA-coherent memory
 *
 * Allocates physically contiguous, cache-aligned memory suitable for
 * DMA operations. Memory is accessible by both CPU and device without
 * explicit cache management.
 *
 * @param size       Size in bytes to allocate
 * @param dma_handle Output: DMA address for device access
 *
 * @return Virtual address on success, NULL on failure
 */
void* dma_alloc_coherent(size_t size, dma_addr_t* dma_handle);

/**
 * Free DMA-coherent memory
 *
 * @param vaddr      Virtual address returned by dma_alloc_coherent()
 * @param size       Size that was allocated
 * @param dma_handle DMA address that was returned
 */
void dma_free_coherent(void* vaddr, size_t size, dma_addr_t dma_handle);

/* ============================================================================
 * Streaming DMA Mapping
 * ============================================================================ */

/**
 * Map a single buffer for DMA
 *
 * Maps an existing kernel buffer for device access. Performs cache
 * operations based on the specified direction.
 *
 * @param vaddr  Virtual address of buffer
 * @param size   Size of buffer in bytes
 * @param dir    DMA transfer direction
 *
 * @return DMA address on success, DMA_ADDR_INVALID on failure
 */
dma_addr_t dma_map_single(void* vaddr, size_t size, dma_direction_t dir);

/**
 * Unmap a single DMA buffer
 *
 * @param dma_addr  DMA address returned by dma_map_single()
 * @param size      Size of buffer
 * @param dir       DMA transfer direction (must match map call)
 */
void dma_unmap_single(dma_addr_t dma_addr, size_t size, dma_direction_t dir);

/* ============================================================================
 * Cache Synchronization
 * ============================================================================ */

/**
 * Synchronize buffer for device access
 *
 * Call after CPU writes, before device reads.
 * Flushes CPU caches to ensure device sees latest data.
 *
 * @param dma_addr  DMA address of buffer
 * @param size      Size of buffer
 * @param dir       DMA direction
 */
void dma_sync_for_device(dma_addr_t dma_addr, size_t size, dma_direction_t dir);

/**
 * Synchronize buffer for CPU access
 *
 * Call after device writes, before CPU reads.
 * Invalidates CPU caches to ensure CPU sees device-written data.
 *
 * @param dma_addr  DMA address of buffer
 * @param size      Size of buffer
 * @param dir       DMA direction
 */
void dma_sync_for_cpu(dma_addr_t dma_addr, size_t size, dma_direction_t dir);

/* ============================================================================
 * Scatter-Gather Operations
 * ============================================================================ */

/**
 * Initialize a scatter-gather list
 *
 * @param sg           Pointer to SG list structure to initialize
 * @param max_entries  Maximum number of entries (up to DMA_SG_MAX_ENTRIES)
 *
 * @return DMA_OK on success, error code on failure
 */
int dma_sg_init(dma_sg_list_t* sg, int max_entries);

/**
 * Add a buffer segment to scatter-gather list
 *
 * @param sg      SG list to add to
 * @param vaddr   Virtual address of segment
 * @param length  Length of segment in bytes
 *
 * @return DMA_OK on success, error code on failure
 */
int dma_sg_add(dma_sg_list_t* sg, void* vaddr, size_t length);

/**
 * Map entire scatter-gather list for DMA
 *
 * Translates all virtual addresses to DMA addresses and performs
 * appropriate cache operations.
 *
 * @param sg   SG list to map
 * @param dir  DMA transfer direction
 *
 * @return DMA_OK on success, error code on failure
 */
int dma_sg_map(dma_sg_list_t* sg, dma_direction_t dir);

/**
 * Unmap scatter-gather list
 *
 * @param sg   SG list to unmap
 * @param dir  DMA transfer direction (must match map call)
 */
void dma_sg_unmap(dma_sg_list_t* sg, dma_direction_t dir);

/**
 * Free scatter-gather list resources
 *
 * @param sg  SG list to free
 */
void dma_sg_free(dma_sg_list_t* sg);

/**
 * Get number of entries in scatter-gather list
 *
 * @param sg  SG list
 *
 * @return Number of entries
 */
static inline int dma_sg_count(const dma_sg_list_t* sg) {
    return sg ? sg->count : 0;
}

/**
 * Get total length of all segments in scatter-gather list
 *
 * @param sg  SG list
 *
 * @return Total bytes
 */
size_t dma_sg_total_length(const dma_sg_list_t* sg);

/* ============================================================================
 * Address Translation
 * ============================================================================ */

/**
 * Convert kernel virtual address to DMA address
 *
 * @param vaddr  Kernel virtual address
 *
 * @return DMA address, or DMA_ADDR_INVALID on error
 */
dma_addr_t virt_to_dma(void* vaddr);

/**
 * Convert DMA address to kernel virtual address
 *
 * @param dma_addr  DMA address
 *
 * @return Virtual address, or NULL on error
 */
void* dma_to_virt(dma_addr_t dma_addr);

/* ============================================================================
 * Debugging and Validation
 * ============================================================================ */

/**
 * Validate a DMA address range
 *
 * Checks if address is valid and within expected bounds.
 *
 * @param addr  DMA address to validate
 * @param size  Size of region
 *
 * @return DMA_OK if valid, error code if invalid
 */
int dma_validate_address(dma_addr_t addr, size_t size);

/**
 * Dump all active DMA allocations
 *
 * Prints allocation list to console for debugging.
 */
void dma_dump_allocations(void);

/**
 * Get DMA statistics
 *
 * @param stats  Output structure for statistics
 */
void dma_get_stats(dma_stats_t* stats);

/**
 * Print DMA statistics to console
 */
void dma_print_stats(void);

/* ============================================================================
 * Test Entry Point
 * ============================================================================ */

/**
 * Run DMA subsystem tests
 */
int dma_run_tests(void);

#endif /* EMBODIOS_DMA_H */
