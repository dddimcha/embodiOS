/* SPDX-License-Identifier: GPL-2.0 */
/**
 * EMBODIOS Pre-computed Embeddings Cache
 *
 * Implementation of pre-computed embedding table for optimized inference.
 * Target: ~15% inference speedup through embedding pre-computation.
 *
 * Design Principles:
 * 1. Page-aligned memory allocation for cache efficiency
 * 2. Pre-computed combined embeddings for hot paths
 * 3. SIMD-friendly memory layout
 * 4. Comprehensive statistics for benchmarking
 */

#include <embodios/embeddings.h>
#include <embodios/mm.h>
#include <embodios/console.h>
#include <embodios/model.h>
#include <embodios/gguf.h>

/* ============================================================================
 * Internal Constants
 * ============================================================================ */

#define CACHE_LINE_SIZE     64
#define PREFETCH_DISTANCE   4

/* Error codes */
#define EMB_SUCCESS         0
#define EMB_ERR_NOMEM       (-1)
#define EMB_ERR_INVALID     (-2)
#define EMB_ERR_BOUNDS      (-3)
#define EMB_ERR_NOTINIT     (-4)
#define EMB_ERR_NOWEIGHTS   (-5)

/* ============================================================================
 * Global State
 * ============================================================================ */

/*
 * Global embedding cache pointer.
 * Marked volatile to ensure reads/writes are not optimized away.
 * Access should be serialized with interrupt disable for full safety.
 */
static volatile embedding_cache_t* g_embedding_cache = NULL;

/* Memory barrier for cache coherency */
#define barrier() __asm__ volatile("" ::: "memory")

/* Timer stub - will use actual timer when available */
static inline uint64_t get_timestamp_ns(void)
{
    /* TODO: Implement proper high-resolution timer */
    static uint64_t counter = 0;
    return counter++;
}

/* ============================================================================
 * Memory Allocation Helpers
 * ============================================================================ */

/**
 * alloc_page_aligned - Allocate page-aligned memory
 * @size: Bytes to allocate
 *
 * Uses heap_alloc with alignment padding for page-aligned allocation.
 */
static void* alloc_page_aligned(size_t size)
{
    /* Round up to page boundary */
    size_t aligned_size = (size + EMBEDDING_PAGE_SIZE - 1) & ~(EMBEDDING_PAGE_SIZE - 1);

    void* ptr = heap_alloc(aligned_size);
    if (!ptr) {
        console_printf("[Embeddings] Failed to allocate %zu bytes\n", aligned_size);
        return NULL;
    }

    /* Zero the memory */
    memset(ptr, 0, aligned_size);

    return ptr;
}

/**
 * free_page_aligned - Free page-aligned memory
 * @ptr: Pointer to free
 */
static void free_page_aligned(void* ptr)
{
    if (ptr) {
        heap_free(ptr);
    }
}

/* ============================================================================
 * Memory Calculation
 * ============================================================================ */

size_t embedding_memory_required(const embedding_config_t* config)
{
    if (!config) {
        return 0;
    }

    size_t token_size = (size_t)config->vocab_size * config->embedding_dim * sizeof(embedding_t);
    size_t position_size = 0;
    size_t combined_size = 0;

    if (config->use_position_emb) {
        position_size = (size_t)config->max_seq_len * config->embedding_dim * sizeof(embedding_t);
    }

    if (config->use_combined_cache) {
        /* Combined cache: positions x vocab x dim would be huge
         * Instead, cache positions x dim for position embeddings only
         * The lookup adds them at runtime */
        combined_size = (size_t)config->cache_positions * config->embedding_dim * sizeof(embedding_t);
    }

    /* Add alignment overhead */
    token_size = (token_size + EMBEDDING_PAGE_SIZE - 1) & ~(EMBEDDING_PAGE_SIZE - 1);
    position_size = (position_size + EMBEDDING_PAGE_SIZE - 1) & ~(EMBEDDING_PAGE_SIZE - 1);
    combined_size = (combined_size + EMBEDDING_PAGE_SIZE - 1) & ~(EMBEDDING_PAGE_SIZE - 1);

    return sizeof(embedding_cache_t) + token_size + position_size + combined_size;
}

/* ============================================================================
 * Cache Initialization
 * ============================================================================ */

embedding_cache_t* embedding_cache_init(const embedding_config_t* config)
{
    uint64_t start_time = get_timestamp_ns();

    if (!config) {
        console_printf("[Embeddings] ERROR: NULL config\n");
        return NULL;
    }

    /* Validate configuration */
    if (config->vocab_size == 0 || config->embedding_dim == 0) {
        console_printf("[Embeddings] ERROR: Invalid config (vocab=%u, dim=%u)\n",
                       config->vocab_size, config->embedding_dim);
        return NULL;
    }

    console_printf("[Embeddings] Initializing cache:\n");
    console_printf("  Vocab size: %u\n", config->vocab_size);
    console_printf("  Embedding dim: %u\n", config->embedding_dim);
    console_printf("  Max seq len: %u\n", config->max_seq_len);
    console_printf("  Cache positions: %u\n", config->cache_positions);

    /* Calculate memory requirements */
    size_t total_mem = embedding_memory_required(config);
    console_printf("  Memory required: %zu KB\n", total_mem / 1024);

    /* Allocate cache structure */
    embedding_cache_t* cache = (embedding_cache_t*)heap_alloc(sizeof(embedding_cache_t));
    if (!cache) {
        console_printf("[Embeddings] ERROR: Failed to allocate cache struct\n");
        return NULL;
    }
    memset(cache, 0, sizeof(embedding_cache_t));

    /* Initialize header */
    cache->magic = EMBEDDING_MAGIC;
    cache->version = EMBEDDING_CACHE_VERSION;
    cache->config = *config;

    /* Allocate token embeddings */
    size_t token_size = (size_t)config->vocab_size * config->embedding_dim * sizeof(embedding_t);
    cache->token_embeddings = (embedding_t*)alloc_page_aligned(token_size);
    if (!cache->token_embeddings) {
        console_printf("[Embeddings] ERROR: Failed to allocate token embeddings\n");
        heap_free(cache);
        return NULL;
    }
    cache->stats.memory_used += token_size;

    /* Allocate position embeddings if needed */
    if (config->use_position_emb && config->max_seq_len > 0) {
        size_t pos_size = (size_t)config->max_seq_len * config->embedding_dim * sizeof(embedding_t);
        cache->position_embeddings = (embedding_t*)alloc_page_aligned(pos_size);
        if (!cache->position_embeddings) {
            console_printf("[Embeddings] ERROR: Failed to allocate position embeddings\n");
            free_page_aligned(cache->token_embeddings);
            heap_free(cache);
            return NULL;
        }
        cache->stats.memory_used += pos_size;
    }

    /* Allocate combined cache if needed */
    if (config->use_combined_cache && config->cache_positions > 0) {
        size_t comb_size = (size_t)config->cache_positions * config->embedding_dim * sizeof(embedding_t);
        cache->combined_cache = (embedding_t*)alloc_page_aligned(comb_size);
        if (!cache->combined_cache) {
            console_printf("[Embeddings] WARNING: Failed to allocate combined cache\n");
            console_printf("[Embeddings] Falling back to on-the-fly computation\n");
            /* Not fatal - we can compute on the fly */
            /* Do NOT update memory_used since allocation failed */
        } else {
            cache->stats.memory_used += comb_size;
        }
    }

    cache->initialized = true;
    cache->precomputed = false;

    cache->stats.init_time_ns = get_timestamp_ns() - start_time;

    console_printf("[Embeddings] Cache initialized: %zu KB used\n",
                   cache->stats.memory_used / 1024);

    return cache;
}

void embedding_cache_destroy(embedding_cache_t* cache)
{
    if (!cache) {
        return;
    }

    console_printf("[Embeddings] Destroying cache\n");

    if (cache->token_embeddings) {
        free_page_aligned(cache->token_embeddings);
    }
    if (cache->position_embeddings) {
        free_page_aligned(cache->position_embeddings);
    }
    if (cache->combined_cache) {
        free_page_aligned(cache->combined_cache);
    }

    /* Clear global reference if this was the global cache */
    if (g_embedding_cache == cache) {
        g_embedding_cache = NULL;
    }

    heap_free(cache);
}

/* ============================================================================
 * Weight Loading
 * ============================================================================ */

int embedding_cache_load_weights(embedding_cache_t* cache, struct embodios_model* model)
{
    if (!cache || !cache->initialized) {
        console_printf("[Embeddings] ERROR: Cache not initialized\n");
        return EMB_ERR_NOTINIT;
    }

    if (!model) {
        console_printf("[Embeddings] ERROR: NULL model\n");
        return EMB_ERR_INVALID;
    }

    console_printf("[Embeddings] Loading weights from model '%s'\n", model->name);

    /* Try to get token embeddings from GGUF */
    size_t token_size = 0;
    void* token_weights = gguf_get_tensor("token_embd.weight", &token_size);

    if (!token_weights) {
        /* Try alternate names */
        token_weights = gguf_get_tensor("model.embed_tokens.weight", &token_size);
    }
    if (!token_weights) {
        token_weights = gguf_get_tensor("tok_embeddings.weight", &token_size);
    }

    if (token_weights && token_size > 0) {
        size_t expected_size = (size_t)cache->config.vocab_size *
                               cache->config.embedding_dim * sizeof(embedding_t);

        console_printf("[Embeddings] Token weights: %zu bytes (expected %zu)\n",
                       token_size, expected_size);

        /* Copy weights to cache */
        size_t copy_size = (token_size < expected_size) ? token_size : expected_size;
        memcpy(cache->token_embeddings, token_weights, copy_size);
        cache->src_token_weights = (const embedding_t*)token_weights;

        console_printf("[Embeddings] Token embeddings loaded: %zu bytes\n", copy_size);
    } else {
        console_printf("[Embeddings] WARNING: Token embeddings not found in model\n");
        console_printf("[Embeddings] Initializing with random embeddings for testing\n");

        /* Initialize with small random values for testing */
        uint32_t seed = 42;
        size_t total = (size_t)cache->config.vocab_size * cache->config.embedding_dim;
        for (size_t i = 0; i < total; i++) {
            seed = seed * 1103515245 + 12345;
            cache->token_embeddings[i] = ((float)(seed >> 16) / 32768.0f - 1.0f) * 0.02f;
        }
    }

    /* Try to get position embeddings */
    if (cache->config.use_position_emb && cache->position_embeddings) {
        size_t pos_size = 0;
        void* pos_weights = gguf_get_tensor("position_embd.weight", &pos_size);

        if (!pos_weights) {
            pos_weights = gguf_get_tensor("model.embed_positions.weight", &pos_size);
        }

        if (pos_weights && pos_size > 0) {
            size_t expected_size = (size_t)cache->config.max_seq_len *
                                   cache->config.embedding_dim * sizeof(embedding_t);
            size_t copy_size = (pos_size < expected_size) ? pos_size : expected_size;
            memcpy(cache->position_embeddings, pos_weights, copy_size);
            cache->src_position_weights = (const embedding_t*)pos_weights;

            console_printf("[Embeddings] Position embeddings loaded: %zu bytes\n", copy_size);
        } else {
            /* Generate sinusoidal position embeddings */
            console_printf("[Embeddings] Generating sinusoidal position embeddings\n");

            for (uint32_t pos = 0; pos < cache->config.max_seq_len; pos++) {
                for (uint32_t i = 0; i < cache->config.embedding_dim; i++) {
                    float freq = 1.0f / (10000.0f * ((float)i / cache->config.embedding_dim));
                    size_t idx = (size_t)pos * cache->config.embedding_dim + i;

                    if (i % 2 == 0) {
                        /* sin for even indices */
                        float x = pos * freq;
                        /* Taylor series approximation for sin */
                        float x2 = x * x;
                        cache->position_embeddings[idx] = x * (1.0f - x2/6.0f + x2*x2/120.0f);
                    } else {
                        /* cos for odd indices */
                        float x = pos * freq;
                        float x2 = x * x;
                        cache->position_embeddings[idx] = 1.0f - x2/2.0f + x2*x2/24.0f;
                    }
                }
            }
        }
    }

    return EMB_SUCCESS;
}

/* ============================================================================
 * Pre-computation
 * ============================================================================ */

int embedding_cache_precompute(embedding_cache_t* cache)
{
    uint64_t start_time = get_timestamp_ns();

    if (!cache || !cache->initialized) {
        return EMB_ERR_NOTINIT;
    }

    console_printf("[Embeddings] Pre-computing embeddings...\n");

    /* Pre-compute position embeddings for fast lookup */
    if (cache->combined_cache && cache->config.cache_positions > 0) {
        console_printf("[Embeddings] Pre-computing %u position embeddings\n",
                       cache->config.cache_positions);

        /* Copy frequently used position embeddings to combined cache */
        size_t copy_size = (size_t)cache->config.cache_positions *
                           cache->config.embedding_dim * sizeof(embedding_t);

        if (cache->position_embeddings) {
            memcpy(cache->combined_cache, cache->position_embeddings, copy_size);
        }
    }

    /* Mark embeddings as valid for fast path */
    cache->precomputed = true;

    cache->stats.compute_time_ns = get_timestamp_ns() - start_time;

    console_printf("[Embeddings] Pre-computation complete\n");

    return EMB_SUCCESS;
}

/* ============================================================================
 * Embedding Lookup
 * ============================================================================ */

int embedding_lookup(embedding_cache_t* cache,
                     uint32_t token_id,
                     uint32_t position,
                     embedding_t* output)
{
    if (!cache || !cache->initialized || !output) {
        return EMB_ERR_NOTINIT;
    }

    if (token_id >= cache->config.vocab_size) {
        return EMB_ERR_BOUNDS;
    }

    /* Update statistics */
    cache->stats.lookups_total++;

    uint64_t start_time = get_timestamp_ns();

    /* Get token embedding base pointer */
    const embedding_t* token_emb = &cache->token_embeddings[
        (size_t)token_id * cache->config.embedding_dim];

    /* Copy token embedding to output */
    uint32_t dim = cache->config.embedding_dim;

    /* Unrolled copy for better performance */
    uint32_t i = 0;
    for (; i + 3 < dim; i += 4) {
        output[i]     = token_emb[i];
        output[i + 1] = token_emb[i + 1];
        output[i + 2] = token_emb[i + 2];
        output[i + 3] = token_emb[i + 3];
    }
    for (; i < dim; i++) {
        output[i] = token_emb[i];
    }

    /* Add position embedding if available */
    if (cache->config.use_position_emb && cache->position_embeddings) {
        if (position >= cache->config.max_seq_len) {
            position = cache->config.max_seq_len - 1;  /* Clamp */
        }

        const embedding_t* pos_emb;

        /* Check if in combined cache (hot path) */
        if (cache->precomputed && cache->combined_cache &&
            position < cache->config.cache_positions) {
            pos_emb = &cache->combined_cache[(size_t)position * cache->config.embedding_dim];
            cache->stats.combined_hits++;
        } else {
            pos_emb = &cache->position_embeddings[(size_t)position * cache->config.embedding_dim];
        }

        /* Add position embedding to output */
        i = 0;
        for (; i + 3 < dim; i += 4) {
            output[i]     += pos_emb[i];
            output[i + 1] += pos_emb[i + 1];
            output[i + 2] += pos_emb[i + 2];
            output[i + 3] += pos_emb[i + 3];
        }
        for (; i < dim; i++) {
            output[i] += pos_emb[i];
        }
    }

    cache->stats.cache_hits++;
    cache->stats.lookup_time_ns += get_timestamp_ns() - start_time;

    return EMB_SUCCESS;
}

int embedding_lookup_batch(embedding_cache_t* cache,
                           const uint32_t* token_ids,
                           const uint32_t* positions,
                           uint32_t n_tokens,
                           uint32_t start_pos,
                           embedding_t* output)
{
    if (!cache || !cache->initialized || !token_ids || !output) {
        return EMB_ERR_NOTINIT;
    }

    uint32_t dim = cache->config.embedding_dim;

    for (uint32_t t = 0; t < n_tokens; t++) {
        uint32_t pos = positions ? positions[t] : (start_pos + t);
        embedding_t* out_ptr = &output[(size_t)t * dim];

        int ret = embedding_lookup(cache, token_ids[t], pos, out_ptr);
        if (ret != EMB_SUCCESS) {
            return ret;
        }
    }

    return EMB_SUCCESS;
}

int embedding_get_token_only(const embedding_cache_t* cache,
                             uint32_t token_id,
                             embedding_t* output)
{
    if (!cache || !cache->initialized || !output) {
        return EMB_ERR_NOTINIT;
    }

    if (token_id >= cache->config.vocab_size) {
        return EMB_ERR_BOUNDS;
    }

    const embedding_t* token_emb = &cache->token_embeddings[
        (size_t)token_id * cache->config.embedding_dim];

    memcpy(output, token_emb, cache->config.embedding_dim * sizeof(embedding_t));

    return EMB_SUCCESS;
}

int embedding_get_position_only(const embedding_cache_t* cache,
                                uint32_t position,
                                embedding_t* output)
{
    if (!cache || !cache->initialized || !output) {
        return EMB_ERR_NOTINIT;
    }

    if (!cache->position_embeddings) {
        return EMB_ERR_NOWEIGHTS;
    }

    if (position >= cache->config.max_seq_len) {
        position = cache->config.max_seq_len - 1;
    }

    const embedding_t* pos_emb;

    /* Use combined cache if available */
    if (cache->precomputed && cache->combined_cache &&
        position < cache->config.cache_positions) {
        pos_emb = &cache->combined_cache[(size_t)position * cache->config.embedding_dim];
    } else {
        pos_emb = &cache->position_embeddings[(size_t)position * cache->config.embedding_dim];
    }

    memcpy(output, pos_emb, cache->config.embedding_dim * sizeof(embedding_t));

    return EMB_SUCCESS;
}

/* ============================================================================
 * Statistics and Benchmarking
 * ============================================================================ */

const embedding_stats_t* embedding_get_stats(const embedding_cache_t* cache)
{
    if (!cache) {
        return NULL;
    }
    return &cache->stats;
}

void embedding_reset_stats(embedding_cache_t* cache)
{
    if (!cache) {
        return;
    }

    /* Preserve memory_used and init_time_ns */
    size_t mem = cache->stats.memory_used;
    uint64_t init_time = cache->stats.init_time_ns;

    memset(&cache->stats, 0, sizeof(embedding_stats_t));

    cache->stats.memory_used = mem;
    cache->stats.init_time_ns = init_time;
}

void embedding_print_stats(const embedding_cache_t* cache)
{
    if (!cache) {
        console_printf("[Embeddings] No cache\n");
        return;
    }

    const embedding_stats_t* s = &cache->stats;

    console_printf("=== Embedding Cache Statistics ===\n");
    console_printf("Memory used:      %zu KB\n", s->memory_used / 1024);
    console_printf("Init time:        %lu ns\n", (unsigned long)s->init_time_ns);
    console_printf("Compute time:     %lu ns\n", (unsigned long)s->compute_time_ns);
    console_printf("Total lookups:    %lu\n", (unsigned long)s->lookups_total);
    console_printf("Cache hits:       %lu\n", (unsigned long)s->cache_hits);
    console_printf("Combined hits:    %lu\n", (unsigned long)s->combined_hits);
    console_printf("Lookup time:      %lu ns\n", (unsigned long)s->lookup_time_ns);

    if (s->lookups_total > 0) {
        console_printf("Avg lookup:       %lu ns\n",
                       (unsigned long)(s->lookup_time_ns / s->lookups_total));
        console_printf("Hit rate:         %.1f%%\n",
                       100.0f * (float)s->cache_hits / (float)s->lookups_total);
    }
}

uint64_t embedding_benchmark(embedding_cache_t* cache, uint32_t iterations)
{
    if (!cache || !cache->initialized || iterations == 0) {
        return 0;
    }

    console_printf("[Embeddings] Running benchmark (%u iterations)...\n", iterations);

    /* Allocate output buffer */
    embedding_t* output = (embedding_t*)heap_alloc(
        cache->config.embedding_dim * sizeof(embedding_t));
    if (!output) {
        console_printf("[Embeddings] Benchmark: Failed to allocate output\n");
        return 0;
    }

    /* Reset stats */
    embedding_reset_stats(cache);

    uint64_t start_time = get_timestamp_ns();

    /* Benchmark token lookup */
    for (uint32_t i = 0; i < iterations; i++) {
        uint32_t token = i % cache->config.vocab_size;
        uint32_t pos = i % cache->config.max_seq_len;
        embedding_lookup(cache, token, pos, output);
    }

    uint64_t end_time = get_timestamp_ns();
    uint64_t total_time = end_time - start_time;
    uint64_t avg_time = total_time / iterations;

    console_printf("[Embeddings] Benchmark results:\n");
    console_printf("  Iterations:     %u\n", iterations);
    console_printf("  Total time:     %lu ns\n", (unsigned long)total_time);
    console_printf("  Avg per lookup: %lu ns\n", (unsigned long)avg_time);
    console_printf("  Lookups/sec:    %lu\n",
                   (unsigned long)(iterations * 1000000000ULL / (total_time + 1)));

    /* Print full stats */
    embedding_print_stats(cache);

    heap_free(output);

    return avg_time;
}

/* ============================================================================
 * Validation
 * ============================================================================ */

bool embedding_validate_cache(const embedding_cache_t* cache)
{
    if (!cache) {
        return false;
    }

    if (cache->magic != EMBEDDING_MAGIC) {
        console_printf("[Embeddings] Invalid magic: 0x%08x\n", cache->magic);
        return false;
    }

    if (cache->version != EMBEDDING_CACHE_VERSION) {
        console_printf("[Embeddings] Version mismatch: %u vs %u\n",
                       cache->version, EMBEDDING_CACHE_VERSION);
        return false;
    }

    if (!cache->initialized) {
        console_printf("[Embeddings] Cache not initialized\n");
        return false;
    }

    if (!cache->token_embeddings) {
        console_printf("[Embeddings] No token embeddings\n");
        return false;
    }

    return true;
}

/* ============================================================================
 * Global Instance
 * ============================================================================ */

embedding_cache_t* embedding_get_global(void)
{
    barrier();
    embedding_cache_t* cache = (embedding_cache_t*)g_embedding_cache;
    barrier();
    return cache;
}

void embedding_set_global(embedding_cache_t* cache)
{
    barrier();
    g_embedding_cache = cache;
    barrier();
}
