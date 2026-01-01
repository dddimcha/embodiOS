/* SPDX-License-Identifier: GPL-2.0 */
/**
 * EMBODIOS Enhanced KV Cache for Transformer Attention
 *
 * Provides high-performance key-value caching for transformer attention,
 * avoiding recalculation of K/V tensors for previous tokens.
 *
 * Performance Target: ~2x inference speedup for autoregressive generation
 *
 * Features:
 * - Page-aligned memory allocation from AI heap (256MB)
 * - Supports both float (fp32) and fixed_t (int32 quantized) types
 * - Sliding window eviction for long sequences
 * - Per-layer statistics and benchmarking
 * - Interrupt-safe global state management
 *
 * Memory Layout (per layer, page-aligned):
 * +------------------+
 * | Key Cache        |  max_seq_len * n_kv_heads * head_dim * sizeof(T)
 * +------------------+
 * | Value Cache      |  max_seq_len * n_kv_heads * head_dim * sizeof(T)
 * +------------------+
 *
 * Architecture:
 * - Multi-Query Attention (MQA): n_kv_heads < n_heads, KV shared across heads
 * - Grouped-Query Attention (GQA): n_kv_heads groups, n_heads/n_kv_heads per group
 * - Standard Attention: n_kv_heads == n_heads
 */

#ifndef EMBODIOS_KV_CACHE_ENHANCED_H
#define EMBODIOS_KV_CACHE_ENHANCED_H

#include <embodios/types.h>

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

#define KV_CACHE_VERSION        1
#define KV_CACHE_MAGIC          0x4B564341  /* 'KVCA' */
#define KV_CACHE_PAGE_SIZE      4096
#define KV_CACHE_ALIGNMENT      64          /* Cache line alignment */

/* Default configuration (can be overridden at init) */
#define KV_CACHE_DEFAULT_LAYERS     22      /* TinyLlama layers */
#define KV_CACHE_DEFAULT_HEADS      4       /* TinyLlama KV heads (GQA) */
#define KV_CACHE_DEFAULT_HEAD_DIM   64      /* TinyLlama head dimension */
#define KV_CACHE_DEFAULT_MAX_SEQ    2048    /* Maximum sequence length */
#define KV_CACHE_DEFAULT_WINDOW     512     /* Sliding window size for eviction */

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/**
 * KV cache data type enumeration
 */
typedef enum {
    KV_TYPE_FLOAT32 = 0,    /* Standard float32 */
    KV_TYPE_FIXED32 = 1,    /* Quantized fixed-point (int32) */
    KV_TYPE_FLOAT16 = 2,    /* Half precision (future) */
} kv_cache_type_t;

/**
 * Eviction policy for long sequences
 */
typedef enum {
    KV_EVICT_NONE = 0,      /* No eviction, fail on overflow */
    KV_EVICT_SLIDING,       /* Sliding window: drop oldest tokens */
    KV_EVICT_RING,          /* Ring buffer: overwrite oldest */
    KV_EVICT_ATTENTION,     /* Attention-based: keep high-attention tokens */
} kv_evict_policy_t;

/**
 * KV cache configuration
 */
typedef struct kv_cache_config {
    uint32_t n_layers;          /* Number of transformer layers */
    uint32_t n_kv_heads;        /* Number of KV heads (may differ from query heads) */
    uint32_t head_dim;          /* Dimension per head */
    uint32_t max_seq_len;       /* Maximum sequence length */
    uint32_t window_size;       /* Sliding window size (for eviction) */
    kv_cache_type_t data_type;  /* Data type for cache storage */
    kv_evict_policy_t eviction; /* Eviction policy */
} kv_cache_config_t;

/**
 * Per-layer cache statistics
 */
typedef struct kv_layer_stats {
    uint64_t cache_hits;        /* KV lookups that found cached data */
    uint64_t cache_stores;      /* New KV pairs stored */
    uint64_t evictions;         /* Tokens evicted due to overflow */
    uint64_t recomputations;    /* KV pairs that had to be recomputed */
} kv_layer_stats_t;

/**
 * Aggregate cache statistics
 */
typedef struct kv_cache_stats {
    uint64_t total_hits;            /* Total cache hits across all layers */
    uint64_t total_stores;          /* Total stores across all layers */
    uint64_t total_evictions;       /* Total evictions across all layers */
    uint64_t attention_time_ns;     /* Time spent in attention with cache */
    uint64_t no_cache_time_ns;      /* Time without cache (for comparison) */
    uint64_t memory_used;           /* Total memory used in bytes */
    uint32_t current_seq_len;       /* Current sequence length */
    uint32_t peak_seq_len;          /* Peak sequence length seen */
    uint32_t n_resets;              /* Number of cache resets */
} kv_cache_stats_t;

/**
 * Per-layer KV cache state
 */
typedef struct kv_layer_cache {
    void* key_cache;            /* Key cache: [max_seq_len][n_kv_heads][head_dim] */
    void* value_cache;          /* Value cache: [max_seq_len][n_kv_heads][head_dim] */
    uint32_t seq_len;           /* Current sequence length for this layer */
    uint32_t start_pos;         /* Start position (for sliding window) */
    kv_layer_stats_t stats;     /* Per-layer statistics */
} kv_layer_cache_t;

/**
 * Main KV cache structure
 */
typedef struct kv_cache {
    uint32_t magic;                 /* Magic number for validation */
    uint32_t version;               /* Cache version */
    kv_cache_config_t config;       /* Configuration */
    kv_cache_stats_t stats;         /* Aggregate statistics */
    kv_layer_cache_t* layers;       /* Per-layer caches [n_layers] */

    /* Computed dimensions */
    size_t layer_size;              /* Size of each layer's KV cache */
    size_t total_size;              /* Total memory allocated */

    /* State flags */
    bool initialized;
    bool enabled;                   /* Can be disabled for benchmarking */
} kv_cache_t;

/* ============================================================================
 * Public API - Lifecycle
 * ============================================================================ */

/**
 * kv_cache_create - Create and initialize KV cache
 * @config: Configuration parameters
 *
 * Allocates page-aligned memory from the AI heap and initializes
 * the KV cache for all layers.
 *
 * Return: Pointer to cache on success, NULL on failure
 */
kv_cache_t* kv_cache_create(const kv_cache_config_t* config);

/**
 * kv_cache_destroy - Free KV cache resources
 * @cache: Cache to destroy
 *
 * Frees all allocated memory and resets the cache state.
 */
void kv_cache_destroy(kv_cache_t* cache);

/**
 * kv_cache_reset - Reset cache for new generation
 * @cache: Cache to reset
 *
 * Clears all cached KV pairs but keeps memory allocated.
 * Call this at the start of each new generation sequence.
 */
void kv_cache_reset(kv_cache_t* cache);

/* ============================================================================
 * Public API - Core Operations (Float)
 * ============================================================================ */

/**
 * kv_cache_store_f32 - Store K/V vectors for a token (float32)
 * @cache: KV cache
 * @layer: Layer index
 * @position: Sequence position
 * @key: Key vector [n_kv_heads * head_dim]
 * @value: Value vector [n_kv_heads * head_dim]
 *
 * Stores the K/V vectors for a single token position.
 * If position exceeds max_seq_len, eviction policy is applied.
 *
 * Return: 0 on success, negative error code on failure
 */
int kv_cache_store_f32(kv_cache_t* cache,
                       uint32_t layer,
                       uint32_t position,
                       const float* key,
                       const float* value);

/**
 * kv_cache_store_batch_f32 - Store K/V vectors for multiple tokens (float32)
 * @cache: KV cache
 * @layer: Layer index
 * @start_pos: Starting sequence position
 * @n_tokens: Number of tokens to store
 * @keys: Key vectors [n_tokens][n_kv_heads * head_dim]
 * @values: Value vectors [n_tokens][n_kv_heads * head_dim]
 *
 * Batch store for prefill phase.
 *
 * Return: 0 on success, negative error code on failure
 */
int kv_cache_store_batch_f32(kv_cache_t* cache,
                             uint32_t layer,
                             uint32_t start_pos,
                             uint32_t n_tokens,
                             const float* keys,
                             const float* values);

/**
 * kv_cache_get_keys_f32 - Get cached key vectors (float32)
 * @cache: KV cache (not const - stats are updated)
 * @layer: Layer index
 * @start_pos: Starting position to retrieve
 * @n_positions: Number of positions to retrieve
 * @output: Output buffer [n_positions][n_kv_heads * head_dim]
 *
 * Retrieves cached key vectors for attention computation.
 * Updates hit statistics.
 *
 * Return: 0 on success, negative error code on failure
 */
int kv_cache_get_keys_f32(kv_cache_t* cache,
                          uint32_t layer,
                          uint32_t start_pos,
                          uint32_t n_positions,
                          float* output);

/**
 * kv_cache_get_values_f32 - Get cached value vectors (float32)
 * @cache: KV cache (not const - stats may be updated)
 * @layer: Layer index
 * @start_pos: Starting position to retrieve
 * @n_positions: Number of positions to retrieve
 * @output: Output buffer [n_positions][n_kv_heads * head_dim]
 *
 * Retrieves cached value vectors for attention computation.
 *
 * Return: 0 on success, negative error code on failure
 */
int kv_cache_get_values_f32(kv_cache_t* cache,
                            uint32_t layer,
                            uint32_t start_pos,
                            uint32_t n_positions,
                            float* output);

/**
 * kv_cache_get_key_ptr_f32 - Get direct pointer to key cache (float32)
 * @cache: KV cache
 * @layer: Layer index
 *
 * Returns direct pointer to the key cache for this layer.
 * Use with caution - does not copy data.
 *
 * Return: Pointer to key cache, or NULL on error
 */
const float* kv_cache_get_key_ptr_f32(const kv_cache_t* cache, uint32_t layer);

/**
 * kv_cache_get_value_ptr_f32 - Get direct pointer to value cache (float32)
 * @cache: KV cache
 * @layer: Layer index
 *
 * Returns direct pointer to the value cache for this layer.
 * Use with caution - does not copy data.
 *
 * Return: Pointer to value cache, or NULL on error
 */
const float* kv_cache_get_value_ptr_f32(const kv_cache_t* cache, uint32_t layer);

/* ============================================================================
 * Public API - Core Operations (Fixed-Point)
 * ============================================================================ */

/**
 * kv_cache_store_fixed - Store K/V vectors for a token (fixed-point)
 * @cache: KV cache
 * @layer: Layer index
 * @position: Sequence position
 * @key: Key vector [n_kv_heads * head_dim]
 * @value: Value vector [n_kv_heads * head_dim]
 *
 * Return: 0 on success, negative error code on failure
 */
int kv_cache_store_fixed(kv_cache_t* cache,
                         uint32_t layer,
                         uint32_t position,
                         const fixed_t* key,
                         const fixed_t* value);

/**
 * kv_cache_get_keys_fixed - Get cached key vectors (fixed-point)
 * @cache: KV cache
 * @layer: Layer index
 * @start_pos: Starting position to retrieve
 * @n_positions: Number of positions to retrieve
 * @output: Output buffer [n_positions][n_kv_heads * head_dim]
 *
 * Return: 0 on success, negative error code on failure
 */
int kv_cache_get_keys_fixed(const kv_cache_t* cache,
                            uint32_t layer,
                            uint32_t start_pos,
                            uint32_t n_positions,
                            fixed_t* output);

/**
 * kv_cache_get_values_fixed - Get cached value vectors (fixed-point)
 * @cache: KV cache
 * @layer: Layer index
 * @start_pos: Starting position to retrieve
 * @n_positions: Number of positions to retrieve
 * @output: Output buffer [n_positions][n_kv_heads * head_dim]
 *
 * Return: 0 on success, negative error code on failure
 */
int kv_cache_get_values_fixed(const kv_cache_t* cache,
                              uint32_t layer,
                              uint32_t start_pos,
                              uint32_t n_positions,
                              fixed_t* output);

/* ============================================================================
 * Public API - Query Functions
 * ============================================================================ */

/**
 * kv_cache_get_seq_len - Get current sequence length
 * @cache: KV cache
 * @layer: Layer index (or 0 for global)
 *
 * Return: Current sequence length
 */
uint32_t kv_cache_get_seq_len(const kv_cache_t* cache, uint32_t layer);

/**
 * kv_cache_get_start_pos - Get start position (for sliding window)
 * @cache: KV cache
 * @layer: Layer index
 *
 * Return: Start position in the sliding window
 */
uint32_t kv_cache_get_start_pos(const kv_cache_t* cache, uint32_t layer);

/**
 * kv_cache_is_valid - Check if cache is valid and initialized
 * @cache: KV cache
 *
 * Return: true if valid, false otherwise
 */
bool kv_cache_is_valid(const kv_cache_t* cache);

/**
 * kv_cache_memory_required - Calculate memory requirements
 * @config: Configuration
 *
 * Return: Total bytes required for the cache
 */
size_t kv_cache_memory_required(const kv_cache_config_t* config);

/* ============================================================================
 * Public API - Statistics and Benchmarking
 * ============================================================================ */

/**
 * kv_cache_get_stats - Get cache statistics
 * @cache: KV cache
 *
 * Return: Pointer to statistics structure
 */
const kv_cache_stats_t* kv_cache_get_stats(const kv_cache_t* cache);

/**
 * kv_cache_get_layer_stats - Get per-layer statistics
 * @cache: KV cache
 * @layer: Layer index
 *
 * Return: Pointer to layer statistics, or NULL on error
 */
const kv_layer_stats_t* kv_cache_get_layer_stats(const kv_cache_t* cache,
                                                  uint32_t layer);

/**
 * kv_cache_reset_stats - Reset all statistics counters
 * @cache: KV cache
 */
void kv_cache_reset_stats(kv_cache_t* cache);

/**
 * kv_cache_print_stats - Print statistics to console
 * @cache: KV cache
 */
void kv_cache_print_stats(const kv_cache_t* cache);

/**
 * kv_cache_enable - Enable/disable cache for A/B testing
 * @cache: KV cache
 * @enabled: Whether to enable caching
 */
void kv_cache_enable(kv_cache_t* cache, bool enabled);

/* ============================================================================
 * Public API - Global Instance
 * ============================================================================ */

/**
 * kv_cache_get_global - Get global KV cache instance
 *
 * Return: Global cache or NULL if not initialized
 */
kv_cache_t* kv_cache_get_global(void);

/**
 * kv_cache_set_global - Set global KV cache instance
 * @cache: Cache to set as global
 */
void kv_cache_set_global(kv_cache_t* cache);

/* ============================================================================
 * Public API - Benchmark Functions
 * ============================================================================ */

/**
 * kv_cache_benchmark - Run KV cache benchmark
 * @iterations: Number of iterations
 *
 * Compares attention performance with and without KV cache.
 * Target: ~2x speedup
 */
void kv_cache_benchmark(uint32_t iterations);

/**
 * kv_cache_benchmark_command - Run full benchmark from command interface
 *
 * Initializes cache if needed and runs comprehensive benchmark.
 */
void kv_cache_benchmark_command(void);

#endif /* EMBODIOS_KV_CACHE_ENHANCED_H */
