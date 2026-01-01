/* SPDX-License-Identifier: GPL-2.0 */
/**
 * EMBODIOS Pre-computed Embeddings Cache
 *
 * Provides pre-computed embedding table functionality for optimized inference.
 * Embeddings are computed once at model load time and cached in page-aligned
 * memory for fast lookup during inference.
 *
 * Performance Target: ~15% inference speedup (1.15x)
 *
 * Architecture:
 * - Token embeddings: Lookup table indexed by token ID
 * - Position embeddings: Pre-computed for max sequence length
 * - Combined cache: Token + position pre-added for common positions
 *
 * Memory Layout (page-aligned, 256MB AI heap):
 * +------------------+
 * | Token Embeddings |  vocab_size * n_embd * sizeof(float)
 * +------------------+
 * | Position Embeds  |  max_seq_len * n_embd * sizeof(float)
 * +------------------+
 * | Combined Cache   |  cache_size * n_embd * sizeof(float)
 * +------------------+
 * | Statistics       |  sizeof(embedding_stats_t)
 * +------------------+
 */

#ifndef EMBODIOS_EMBEDDINGS_H
#define EMBODIOS_EMBEDDINGS_H

#include <embodios/types.h>
#include <embodios/model.h>

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */

#define EMBEDDING_CACHE_VERSION     1
#define EMBEDDING_MAGIC             0x454D4245  /* 'EMBE' */
#define EMBEDDING_PAGE_SIZE         4096
#define EMBEDDING_ALIGNMENT         64          /* Cache line alignment */

/* Default configuration (can be overridden at init) */
#define EMBEDDING_DEFAULT_VOCAB     32000       /* TinyLlama vocab size */
#define EMBEDDING_DEFAULT_DIM       2048        /* TinyLlama embedding dim */
#define EMBEDDING_DEFAULT_MAX_SEQ   2048        /* Max sequence length */
#define EMBEDDING_CACHE_POSITIONS   128         /* Pre-computed positions */

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

/**
 * Embedding data type
 * Using float for compatibility; can be changed to fp16 for memory savings
 */
typedef float embedding_t;

/**
 * Embedding cache configuration
 */
typedef struct embedding_config {
    uint32_t vocab_size;        /* Vocabulary size */
    uint32_t embedding_dim;     /* Embedding dimension (n_embd) */
    uint32_t max_seq_len;       /* Maximum sequence length */
    uint32_t cache_positions;   /* Number of positions to pre-combine */
    bool     use_position_emb;  /* Whether to use position embeddings */
    bool     use_combined_cache;/* Pre-combine token + position */
} embedding_config_t;

/**
 * Embedding cache statistics for benchmarking
 */
typedef struct embedding_stats {
    uint64_t cache_hits;        /* Token embedding cache hits */
    uint64_t cache_misses;      /* Cache misses (should be 0 after init) */
    uint64_t lookups_total;     /* Total lookup operations */
    uint64_t combined_hits;     /* Combined cache hits */
    uint64_t compute_time_ns;   /* Time spent computing embeddings */
    uint64_t lookup_time_ns;    /* Time spent in lookups */
    uint64_t init_time_ns;      /* Initialization time */
    size_t   memory_used;       /* Total memory used in bytes */
} embedding_stats_t;

/**
 * Embedding cache state
 */
typedef struct embedding_cache {
    uint32_t magic;             /* Magic number for validation */
    uint32_t version;           /* Cache version */
    embedding_config_t config;  /* Configuration */
    embedding_stats_t stats;    /* Statistics */

    /* Cached embedding tables (page-aligned) */
    embedding_t* token_embeddings;    /* [vocab_size][embedding_dim] */
    embedding_t* position_embeddings; /* [max_seq_len][embedding_dim] */
    embedding_t* combined_cache;      /* [cache_positions][vocab_size][embedding_dim] */

    /* Source weight pointers (from model) */
    const embedding_t* src_token_weights;
    const embedding_t* src_position_weights;

    /* State flags */
    bool initialized;
    bool precomputed;
} embedding_cache_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * embedding_cache_init - Initialize the embedding cache
 * @config: Configuration parameters
 *
 * Allocates page-aligned memory from the AI heap and initializes
 * the embedding cache structure.
 *
 * Return: Pointer to cache on success, NULL on failure
 */
embedding_cache_t* embedding_cache_init(const embedding_config_t* config);

/**
 * embedding_cache_destroy - Free embedding cache resources
 * @cache: Cache to destroy
 *
 * Frees all allocated memory and resets the cache state.
 */
void embedding_cache_destroy(embedding_cache_t* cache);

/**
 * embedding_cache_load_weights - Load weights from model
 * @cache: Embedding cache
 * @model: Source model containing weight tensors
 *
 * Loads token and position embedding weights from the model
 * and copies them to the cache.
 *
 * Return: 0 on success, negative error code on failure
 */
int embedding_cache_load_weights(embedding_cache_t* cache,
                                  struct embodios_model* model);

/**
 * embedding_cache_precompute - Pre-compute all embeddings
 * @cache: Embedding cache
 *
 * Pre-computes combined token + position embeddings for
 * frequently used positions (0 to cache_positions-1).
 * This is the main performance optimization.
 *
 * Return: 0 on success, negative error code on failure
 */
int embedding_cache_precompute(embedding_cache_t* cache);

/**
 * embedding_lookup - Fast embedding lookup (single token)
 * @cache: Embedding cache (stats are updated)
 * @token_id: Token ID to look up
 * @position: Sequence position
 * @output: Output buffer [embedding_dim]
 *
 * Performs optimized embedding lookup. Uses combined cache
 * for positions < cache_positions, otherwise computes on the fly.
 * Note: Statistics are updated on each call.
 *
 * Return: 0 on success, negative error code on failure
 */
int embedding_lookup(embedding_cache_t* cache,
                     uint32_t token_id,
                     uint32_t position,
                     embedding_t* output);

/**
 * embedding_lookup_batch - Batch embedding lookup
 * @cache: Embedding cache (stats are updated)
 * @token_ids: Array of token IDs
 * @positions: Array of positions (or NULL for sequential)
 * @n_tokens: Number of tokens
 * @start_pos: Starting position (used if positions is NULL)
 * @output: Output buffer [n_tokens][embedding_dim]
 *
 * Batch lookup with SIMD optimization potential.
 * Note: Statistics are updated on each call.
 *
 * Return: 0 on success, negative error code on failure
 */
int embedding_lookup_batch(embedding_cache_t* cache,
                           const uint32_t* token_ids,
                           const uint32_t* positions,
                           uint32_t n_tokens,
                           uint32_t start_pos,
                           embedding_t* output);

/**
 * embedding_get_token_only - Get token embedding without position
 * @cache: Embedding cache
 * @token_id: Token ID
 * @output: Output buffer [embedding_dim]
 *
 * For cases where position embedding is applied separately.
 * Does not update statistics.
 *
 * Return: 0 on success, negative error code on failure
 */
int embedding_get_token_only(const embedding_cache_t* cache,
                             uint32_t token_id,
                             embedding_t* output);

/**
 * embedding_get_position_only - Get position embedding only
 * @cache: Embedding cache
 * @position: Sequence position
 * @output: Output buffer [embedding_dim]
 *
 * For cases where token and position are added separately.
 * Does not update statistics.
 *
 * Return: 0 on success, negative error code on failure
 */
int embedding_get_position_only(const embedding_cache_t* cache,
                                uint32_t position,
                                embedding_t* output);

/* ============================================================================
 * Statistics and Benchmarking
 * ============================================================================ */

/**
 * embedding_get_stats - Get cache statistics
 * @cache: Embedding cache
 *
 * Return: Pointer to statistics structure
 */
const embedding_stats_t* embedding_get_stats(const embedding_cache_t* cache);

/**
 * embedding_reset_stats - Reset statistics counters
 * @cache: Embedding cache
 */
void embedding_reset_stats(embedding_cache_t* cache);

/**
 * embedding_print_stats - Print statistics to console
 * @cache: Embedding cache
 */
void embedding_print_stats(const embedding_cache_t* cache);

/**
 * embedding_benchmark - Run embedding benchmark
 * @cache: Embedding cache
 * @iterations: Number of iterations
 *
 * Benchmarks embedding lookup performance and prints results.
 *
 * Return: Average lookup time in nanoseconds
 */
uint64_t embedding_benchmark(embedding_cache_t* cache, uint32_t iterations);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * embedding_memory_required - Calculate memory requirements
 * @config: Configuration
 *
 * Return: Total bytes required for the cache
 */
size_t embedding_memory_required(const embedding_config_t* config);

/**
 * embedding_validate_cache - Validate cache integrity
 * @cache: Embedding cache
 *
 * Return: true if cache is valid, false otherwise
 */
bool embedding_validate_cache(const embedding_cache_t* cache);

/* ============================================================================
 * Global Instance (for transformer integration)
 * ============================================================================ */

/**
 * embedding_get_global - Get global embedding cache
 *
 * Return: Global cache instance or NULL if not initialized
 */
embedding_cache_t* embedding_get_global(void);

/**
 * embedding_set_global - Set global embedding cache
 * @cache: Cache to set as global
 */
void embedding_set_global(embedding_cache_t* cache);

/* ============================================================================
 * Benchmark Functions
 * ============================================================================ */

/**
 * embedding_benchmark_command - Run full benchmark comparison
 *
 * Compares direct vs cached embedding lookup performance.
 * Target: ~15% improvement
 */
void embedding_benchmark_command(void);

/**
 * embedding_quick_benchmark - Quick benchmark test
 *
 * Uses global cache for quick performance check.
 */
void embedding_quick_benchmark(void);

#endif /* EMBODIOS_EMBEDDINGS_H */
