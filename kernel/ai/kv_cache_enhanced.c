/* SPDX-License-Identifier: GPL-2.0 */
/**
 * EMBODIOS Enhanced KV Cache Implementation
 *
 * High-performance key-value caching for transformer attention.
 * Target: ~2x inference speedup for autoregressive generation.
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/kv_cache_enhanced.h>

/* ============================================================================
 * Memory Management Helpers
 * ============================================================================ */

/* Align value up to alignment boundary */
#define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))

/* Memory barrier for interrupt safety */
#define barrier() __asm__ volatile("" ::: "memory")

/* Global cache instance (volatile for interrupt safety) */
static volatile kv_cache_t* g_kv_cache = NULL;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/**
 * Get type size in bytes
 */
static size_t kv_type_size(kv_cache_type_t type)
{
    switch (type) {
    case KV_TYPE_FLOAT32:
        return sizeof(float);
    case KV_TYPE_FIXED32:
        return sizeof(fixed_t);
    case KV_TYPE_FLOAT16:
        return sizeof(uint16_t);  /* fp16 stored as uint16 */
    default:
        return sizeof(float);
    }
}

/**
 * Calculate layer KV cache size (page-aligned)
 */
static size_t kv_layer_cache_size(const kv_cache_config_t* config)
{
    size_t elem_size = kv_type_size(config->data_type);
    /* Use explicit size_t casts to prevent intermediate overflow */
    size_t kv_size = (size_t)config->max_seq_len *
                     (size_t)config->n_kv_heads *
                     (size_t)config->head_dim *
                     elem_size;

    /* Each of K and V needs this much */
    return ALIGN_UP(kv_size, KV_CACHE_PAGE_SIZE);
}

/**
 * Get offset into cache for position
 */
static inline size_t kv_position_offset(const kv_cache_config_t* config,
                                        uint32_t position,
                                        size_t elem_size)
{
    return (size_t)position * (size_t)config->n_kv_heads *
           (size_t)config->head_dim * elem_size;
}

/**
 * Get KV vector size for one position
 */
static inline size_t kv_vector_size(const kv_cache_config_t* config,
                                    size_t elem_size)
{
    return (size_t)config->n_kv_heads * (size_t)config->head_dim * elem_size;
}

/**
 * memcpy implementation (avoiding external dependency)
 */
static void* kv_memcpy(void* dest, const void* src, size_t n)
{
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

/**
 * memset implementation
 */
static void* kv_memset(void* s, int c, size_t n)
{
    uint8_t* p = (uint8_t*)s;
    while (n--) {
        *p++ = (uint8_t)c;
    }
    return s;
}

/* ============================================================================
 * Lifecycle Functions
 * ============================================================================ */

/**
 * kv_cache_create - Create and initialize KV cache
 */
kv_cache_t* kv_cache_create(const kv_cache_config_t* config)
{
    if (!config) {
        console_printf("[KV Cache] ERROR: NULL config\n");
        return NULL;
    }

    /* Validate configuration */
    if (config->n_layers == 0 || config->n_layers > 128) {
        console_printf("[KV Cache] ERROR: Invalid n_layers %u\n", config->n_layers);
        return NULL;
    }
    if (config->n_kv_heads == 0 || config->n_kv_heads > 256) {
        console_printf("[KV Cache] ERROR: Invalid n_kv_heads %u\n", config->n_kv_heads);
        return NULL;
    }
    if (config->head_dim == 0 || config->head_dim > 512) {
        console_printf("[KV Cache] ERROR: Invalid head_dim %u\n", config->head_dim);
        return NULL;
    }
    if (config->max_seq_len == 0 || config->max_seq_len > 32768) {
        console_printf("[KV Cache] ERROR: Invalid max_seq_len %u\n", config->max_seq_len);
        return NULL;
    }

    console_printf("[KV Cache] Creating cache:\n");
    console_printf("  Layers: %u\n", config->n_layers);
    console_printf("  KV Heads: %u\n", config->n_kv_heads);
    console_printf("  Head Dim: %u\n", config->head_dim);
    console_printf("  Max Seq: %u\n", config->max_seq_len);
    console_printf("  Window: %u\n", config->window_size);
    console_printf("  Type: %s\n",
                   config->data_type == KV_TYPE_FLOAT32 ? "float32" :
                   config->data_type == KV_TYPE_FIXED32 ? "fixed32" : "float16");

    /* Calculate memory requirements */
    size_t layer_kv_size = kv_layer_cache_size(config);
    size_t total_kv_size = layer_kv_size * 2 * config->n_layers;  /* K and V */
    size_t layer_array_size = ALIGN_UP(sizeof(kv_layer_cache_t) * config->n_layers,
                                       KV_CACHE_ALIGNMENT);
    size_t cache_struct_size = ALIGN_UP(sizeof(kv_cache_t), KV_CACHE_ALIGNMENT);
    size_t total_size = cache_struct_size + layer_array_size + total_kv_size;

    console_printf("  Memory required: %zu KB (%zu MB)\n",
                   total_size / 1024, total_size / (1024 * 1024));

    /* Allocate main structure (page-aligned) */
    kv_cache_t* cache = (kv_cache_t*)heap_alloc_aligned(cache_struct_size,
                                                         KV_CACHE_PAGE_SIZE);
    if (!cache) {
        console_printf("[KV Cache] ERROR: Failed to allocate cache struct\n");
        return NULL;
    }
    kv_memset(cache, 0, sizeof(kv_cache_t));

    /* Allocate layer array */
    cache->layers = (kv_layer_cache_t*)heap_alloc_aligned(layer_array_size,
                                                           KV_CACHE_ALIGNMENT);
    if (!cache->layers) {
        console_printf("[KV Cache] ERROR: Failed to allocate layer array\n");
        heap_free_aligned(cache);
        return NULL;
    }
    kv_memset(cache->layers, 0, layer_array_size);

    /* Initialize structure */
    cache->magic = KV_CACHE_MAGIC;
    cache->version = KV_CACHE_VERSION;
    cache->config = *config;
    cache->layer_size = layer_kv_size;
    cache->total_size = total_size;
    cache->enabled = true;

    /* Allocate per-layer caches */
    for (uint32_t i = 0; i < config->n_layers; i++) {
        /* Allocate key cache (page-aligned) */
        cache->layers[i].key_cache = heap_alloc_aligned(layer_kv_size,
                                                         KV_CACHE_PAGE_SIZE);
        if (!cache->layers[i].key_cache) {
            console_printf("[KV Cache] ERROR: Failed to allocate key cache for layer %u\n", i);
            goto alloc_error;
        }

        /* Allocate value cache (page-aligned) */
        cache->layers[i].value_cache = heap_alloc_aligned(layer_kv_size,
                                                           KV_CACHE_PAGE_SIZE);
        if (!cache->layers[i].value_cache) {
            console_printf("[KV Cache] ERROR: Failed to allocate value cache for layer %u\n", i);
            goto alloc_error;
        }

        /* Initialize layer state */
        cache->layers[i].seq_len = 0;
        cache->layers[i].start_pos = 0;
        kv_memset(&cache->layers[i].stats, 0, sizeof(kv_layer_stats_t));
    }

    cache->stats.memory_used = total_size;
    cache->initialized = true;

    console_printf("[KV Cache] Created successfully (%zu KB used)\n",
                   total_size / 1024);

    return cache;

alloc_error:
    /* Clean up partial allocation */
    for (uint32_t i = 0; i < config->n_layers; i++) {
        if (cache->layers[i].key_cache) {
            heap_free_aligned(cache->layers[i].key_cache);
        }
        if (cache->layers[i].value_cache) {
            heap_free_aligned(cache->layers[i].value_cache);
        }
    }
    heap_free_aligned(cache->layers);
    heap_free_aligned(cache);
    return NULL;
}

/**
 * kv_cache_destroy - Free KV cache resources
 */
void kv_cache_destroy(kv_cache_t* cache)
{
    if (!cache) {
        return;
    }

    /* Clear global reference if this is it */
    if (g_kv_cache == cache) {
        g_kv_cache = NULL;
        barrier();
    }

    /* Free layer caches (page-aligned) */
    if (cache->layers) {
        for (uint32_t i = 0; i < cache->config.n_layers; i++) {
            if (cache->layers[i].key_cache) {
                heap_free_aligned(cache->layers[i].key_cache);
            }
            if (cache->layers[i].value_cache) {
                heap_free_aligned(cache->layers[i].value_cache);
            }
        }
        heap_free_aligned(cache->layers);
    }

    /* Clear and free main structure (page-aligned) */
    cache->magic = 0;
    cache->initialized = false;
    heap_free_aligned(cache);

    console_printf("[KV Cache] Destroyed\n");
}

/**
 * kv_cache_reset - Reset cache for new generation
 */
void kv_cache_reset(kv_cache_t* cache)
{
    if (!cache || !cache->initialized) {
        return;
    }

    for (uint32_t i = 0; i < cache->config.n_layers; i++) {
        cache->layers[i].seq_len = 0;
        cache->layers[i].start_pos = 0;
    }

    cache->stats.current_seq_len = 0;
    cache->stats.n_resets++;
}

/* ============================================================================
 * Core Operations (Float32)
 * ============================================================================ */

/**
 * Apply sliding window eviction
 */
static void kv_apply_eviction(kv_cache_t* cache, uint32_t layer)
{
    if (!cache->config.window_size || cache->config.eviction == KV_EVICT_NONE) {
        return;
    }

    kv_layer_cache_t* lc = &cache->layers[layer];

    if (lc->seq_len <= cache->config.window_size) {
        return;  /* Not yet full */
    }

    uint32_t evict_count = lc->seq_len - cache->config.window_size;

    switch (cache->config.eviction) {
    case KV_EVICT_SLIDING:
        /* Shift data left, dropping oldest tokens */
        {
            size_t elem_size = kv_type_size(cache->config.data_type);
            size_t vec_size = kv_vector_size(&cache->config, elem_size);
            size_t keep_size = (size_t)cache->config.window_size * vec_size;
            size_t drop_offset = (size_t)evict_count * vec_size;

            /* Move remaining data to start */
            uint8_t* k = (uint8_t*)lc->key_cache;
            uint8_t* v = (uint8_t*)lc->value_cache;

            /* Use overlapping copy (memmove equivalent) */
            for (size_t i = 0; i < keep_size; i++) {
                k[i] = k[drop_offset + i];
                v[i] = v[drop_offset + i];
            }

            lc->seq_len = cache->config.window_size;
            lc->start_pos += evict_count;
            lc->stats.evictions += evict_count;
        }
        break;

    case KV_EVICT_RING:
        /* Ring buffer: just update start position, overwrite happens naturally */
        lc->start_pos = (lc->start_pos + evict_count) % cache->config.max_seq_len;
        lc->stats.evictions += evict_count;
        break;

    case KV_EVICT_ATTENTION:
        /* Attention-based eviction would require attention scores */
        /* For now, fall back to sliding window */
        break;

    default:
        break;
    }

    cache->stats.total_evictions += evict_count;
}

/**
 * kv_cache_store_f32 - Store K/V vectors for a token (float32)
 */
int kv_cache_store_f32(kv_cache_t* cache,
                       uint32_t layer,
                       uint32_t position,
                       const float* key,
                       const float* value)
{
    if (!cache || !cache->initialized || !cache->enabled) {
        return -1;
    }

    if (layer >= cache->config.n_layers) {
        return -2;
    }

    if (!key || !value) {
        return -3;
    }

    kv_layer_cache_t* lc = &cache->layers[layer];
    const kv_cache_config_t* cfg = &cache->config;

    /* Check if we need eviction */
    if (position >= cfg->max_seq_len) {
        if (cfg->eviction == KV_EVICT_NONE) {
            console_printf("[KV Cache] Layer %u: Position %u exceeds max %u\n",
                          layer, position, cfg->max_seq_len);
            return -4;
        }
        kv_apply_eviction(cache, layer);
        /* Adjust position after eviction */
        position = lc->seq_len;
    }

    /* Calculate storage offset */
    size_t vec_size = cfg->n_kv_heads * cfg->head_dim;
    size_t offset = (size_t)position * vec_size;

    /* Store K and V */
    float* k_dst = (float*)lc->key_cache + offset;
    float* v_dst = (float*)lc->value_cache + offset;

    kv_memcpy(k_dst, key, vec_size * sizeof(float));
    kv_memcpy(v_dst, value, vec_size * sizeof(float));

    /* Update sequence length */
    if (position >= lc->seq_len) {
        lc->seq_len = position + 1;
    }

    /* Update stats */
    lc->stats.cache_stores++;
    cache->stats.total_stores++;

    if (lc->seq_len > cache->stats.current_seq_len) {
        cache->stats.current_seq_len = lc->seq_len;
    }
    if (cache->stats.current_seq_len > cache->stats.peak_seq_len) {
        cache->stats.peak_seq_len = cache->stats.current_seq_len;
    }

    return 0;
}

/**
 * kv_cache_store_batch_f32 - Store K/V vectors for multiple tokens
 */
int kv_cache_store_batch_f32(kv_cache_t* cache,
                             uint32_t layer,
                             uint32_t start_pos,
                             uint32_t n_tokens,
                             const float* keys,
                             const float* values)
{
    if (!cache || !cache->initialized || !cache->enabled) {
        return -1;
    }

    if (layer >= cache->config.n_layers) {
        return -2;
    }

    if (!keys || !values || n_tokens == 0) {
        return -3;
    }

    const kv_cache_config_t* cfg = &cache->config;
    size_t vec_size = (size_t)cfg->n_kv_heads * (size_t)cfg->head_dim;

    /* Check bounds (prevent integer overflow in comparison) */
    if (n_tokens > cfg->max_seq_len ||
        start_pos > cfg->max_seq_len - n_tokens) {
        if (cfg->eviction == KV_EVICT_NONE) {
            return -4;
        }
        /* Apply eviction before batch store */
        kv_apply_eviction(cache, layer);
    }

    kv_layer_cache_t* lc = &cache->layers[layer];
    size_t offset = (size_t)start_pos * vec_size;
    size_t batch_size = (size_t)n_tokens * vec_size;

    /* Batch copy */
    float* k_dst = (float*)lc->key_cache + offset;
    float* v_dst = (float*)lc->value_cache + offset;

    kv_memcpy(k_dst, keys, batch_size * sizeof(float));
    kv_memcpy(v_dst, values, batch_size * sizeof(float));

    /* Update sequence length */
    uint32_t end_pos = start_pos + n_tokens;
    if (end_pos > lc->seq_len) {
        lc->seq_len = end_pos;
    }

    /* Update stats */
    lc->stats.cache_stores += n_tokens;
    cache->stats.total_stores += n_tokens;

    if (lc->seq_len > cache->stats.current_seq_len) {
        cache->stats.current_seq_len = lc->seq_len;
    }
    if (cache->stats.current_seq_len > cache->stats.peak_seq_len) {
        cache->stats.peak_seq_len = cache->stats.current_seq_len;
    }

    return 0;
}

/**
 * kv_cache_get_keys_f32 - Get cached key vectors
 * Note: cache is not const because statistics are updated
 */
int kv_cache_get_keys_f32(kv_cache_t* cache,
                          uint32_t layer,
                          uint32_t start_pos,
                          uint32_t n_positions,
                          float* output)
{
    if (!cache || !cache->initialized) {
        return -1;
    }

    if (layer >= cache->config.n_layers) {
        return -2;
    }

    if (!output || n_positions == 0) {
        return -3;
    }

    kv_layer_cache_t* lc = &cache->layers[layer];

    /* Bounds check (prevent integer overflow) */
    if (n_positions > lc->seq_len ||
        start_pos > lc->seq_len - n_positions) {
        return -4;
    }

    const kv_cache_config_t* cfg = &cache->config;
    size_t vec_size = (size_t)cfg->n_kv_heads * (size_t)cfg->head_dim;
    size_t offset = (size_t)start_pos * vec_size;
    size_t copy_size = (size_t)n_positions * vec_size;

    const float* k_src = (const float*)lc->key_cache + offset;
    kv_memcpy(output, k_src, copy_size * sizeof(float));

    /* Update statistics */
    lc->stats.cache_hits += n_positions;
    cache->stats.total_hits += n_positions;

    return 0;
}

/**
 * kv_cache_get_values_f32 - Get cached value vectors
 */
int kv_cache_get_values_f32(kv_cache_t* cache,
                            uint32_t layer,
                            uint32_t start_pos,
                            uint32_t n_positions,
                            float* output)
{
    if (!cache || !cache->initialized) {
        return -1;
    }

    if (layer >= cache->config.n_layers) {
        return -2;
    }

    if (!output || n_positions == 0) {
        return -3;
    }

    kv_layer_cache_t* lc = &cache->layers[layer];

    /* Bounds check (prevent integer overflow) */
    if (n_positions > lc->seq_len ||
        start_pos > lc->seq_len - n_positions) {
        return -4;
    }

    const kv_cache_config_t* cfg = &cache->config;
    size_t vec_size = (size_t)cfg->n_kv_heads * (size_t)cfg->head_dim;
    size_t offset = (size_t)start_pos * vec_size;
    size_t copy_size = (size_t)n_positions * vec_size;

    const float* v_src = (const float*)lc->value_cache + offset;
    kv_memcpy(output, v_src, copy_size * sizeof(float));

    return 0;
}

/**
 * kv_cache_get_key_ptr_f32 - Get direct pointer to key cache
 */
const float* kv_cache_get_key_ptr_f32(const kv_cache_t* cache, uint32_t layer)
{
    if (!cache || !cache->initialized || layer >= cache->config.n_layers) {
        return NULL;
    }
    return (const float*)cache->layers[layer].key_cache;
}

/**
 * kv_cache_get_value_ptr_f32 - Get direct pointer to value cache
 */
const float* kv_cache_get_value_ptr_f32(const kv_cache_t* cache, uint32_t layer)
{
    if (!cache || !cache->initialized || layer >= cache->config.n_layers) {
        return NULL;
    }
    return (const float*)cache->layers[layer].value_cache;
}

/* ============================================================================
 * Core Operations (Fixed-Point)
 * ============================================================================ */

/**
 * kv_cache_store_fixed - Store K/V vectors for a token (fixed-point)
 */
int kv_cache_store_fixed(kv_cache_t* cache,
                         uint32_t layer,
                         uint32_t position,
                         const fixed_t* key,
                         const fixed_t* value)
{
    if (!cache || !cache->initialized || !cache->enabled) {
        return -1;
    }

    if (cache->config.data_type != KV_TYPE_FIXED32) {
        console_printf("[KV Cache] ERROR: Cache not configured for fixed-point\n");
        return -5;
    }

    if (layer >= cache->config.n_layers) {
        return -2;
    }

    if (!key || !value) {
        return -3;
    }

    kv_layer_cache_t* lc = &cache->layers[layer];
    const kv_cache_config_t* cfg = &cache->config;

    /* Check bounds */
    if (position >= cfg->max_seq_len) {
        if (cfg->eviction == KV_EVICT_NONE) {
            return -4;
        }
        kv_apply_eviction(cache, layer);
        position = lc->seq_len;
    }

    /* Calculate storage offset */
    size_t vec_size = cfg->n_kv_heads * cfg->head_dim;
    size_t offset = (size_t)position * vec_size;

    /* Store K and V */
    fixed_t* k_dst = (fixed_t*)lc->key_cache + offset;
    fixed_t* v_dst = (fixed_t*)lc->value_cache + offset;

    kv_memcpy(k_dst, key, vec_size * sizeof(fixed_t));
    kv_memcpy(v_dst, value, vec_size * sizeof(fixed_t));

    /* Update sequence length */
    if (position >= lc->seq_len) {
        lc->seq_len = position + 1;
    }

    /* Update stats */
    lc->stats.cache_stores++;
    cache->stats.total_stores++;

    return 0;
}

/**
 * kv_cache_get_keys_fixed - Get cached key vectors (fixed-point)
 */
int kv_cache_get_keys_fixed(const kv_cache_t* cache,
                            uint32_t layer,
                            uint32_t start_pos,
                            uint32_t n_positions,
                            fixed_t* output)
{
    if (!cache || !cache->initialized) {
        return -1;
    }

    if (cache->config.data_type != KV_TYPE_FIXED32) {
        return -5;
    }

    if (layer >= cache->config.n_layers) {
        return -2;
    }

    if (!output || n_positions == 0) {
        return -3;
    }

    const kv_layer_cache_t* lc = &cache->layers[layer];

    if (start_pos + n_positions > lc->seq_len) {
        return -4;
    }

    const kv_cache_config_t* cfg = &cache->config;
    size_t vec_size = cfg->n_kv_heads * cfg->head_dim;
    size_t offset = (size_t)start_pos * vec_size;
    size_t copy_size = (size_t)n_positions * vec_size;

    const fixed_t* k_src = (const fixed_t*)lc->key_cache + offset;
    kv_memcpy(output, k_src, copy_size * sizeof(fixed_t));

    return 0;
}

/**
 * kv_cache_get_values_fixed - Get cached value vectors (fixed-point)
 */
int kv_cache_get_values_fixed(const kv_cache_t* cache,
                              uint32_t layer,
                              uint32_t start_pos,
                              uint32_t n_positions,
                              fixed_t* output)
{
    if (!cache || !cache->initialized) {
        return -1;
    }

    if (cache->config.data_type != KV_TYPE_FIXED32) {
        return -5;
    }

    if (layer >= cache->config.n_layers) {
        return -2;
    }

    if (!output || n_positions == 0) {
        return -3;
    }

    const kv_layer_cache_t* lc = &cache->layers[layer];

    if (start_pos + n_positions > lc->seq_len) {
        return -4;
    }

    const kv_cache_config_t* cfg = &cache->config;
    size_t vec_size = cfg->n_kv_heads * cfg->head_dim;
    size_t offset = (size_t)start_pos * vec_size;
    size_t copy_size = (size_t)n_positions * vec_size;

    const fixed_t* v_src = (const fixed_t*)lc->value_cache + offset;
    kv_memcpy(output, v_src, copy_size * sizeof(fixed_t));

    return 0;
}

/* ============================================================================
 * Query Functions
 * ============================================================================ */

/**
 * kv_cache_get_seq_len - Get current sequence length
 */
uint32_t kv_cache_get_seq_len(const kv_cache_t* cache, uint32_t layer)
{
    if (!cache || !cache->initialized) {
        return 0;
    }

    if (layer >= cache->config.n_layers) {
        return cache->stats.current_seq_len;  /* Return global if invalid layer */
    }

    return cache->layers[layer].seq_len;
}

/**
 * kv_cache_get_start_pos - Get start position (for sliding window)
 */
uint32_t kv_cache_get_start_pos(const kv_cache_t* cache, uint32_t layer)
{
    if (!cache || !cache->initialized || layer >= cache->config.n_layers) {
        return 0;
    }
    return cache->layers[layer].start_pos;
}

/**
 * kv_cache_is_valid - Check if cache is valid
 */
bool kv_cache_is_valid(const kv_cache_t* cache)
{
    if (!cache) {
        return false;
    }
    if (cache->magic != KV_CACHE_MAGIC) {
        return false;
    }
    if (cache->version != KV_CACHE_VERSION) {
        return false;
    }
    return cache->initialized;
}

/**
 * kv_cache_memory_required - Calculate memory requirements
 */
size_t kv_cache_memory_required(const kv_cache_config_t* config)
{
    if (!config) {
        return 0;
    }

    size_t layer_kv_size = kv_layer_cache_size(config);
    size_t total_kv_size = layer_kv_size * 2 * config->n_layers;
    size_t layer_array_size = ALIGN_UP(sizeof(kv_layer_cache_t) * config->n_layers,
                                       KV_CACHE_ALIGNMENT);
    size_t cache_struct_size = ALIGN_UP(sizeof(kv_cache_t), KV_CACHE_ALIGNMENT);

    return cache_struct_size + layer_array_size + total_kv_size;
}

/* ============================================================================
 * Statistics and Benchmarking
 * ============================================================================ */

/**
 * kv_cache_get_stats - Get cache statistics
 */
const kv_cache_stats_t* kv_cache_get_stats(const kv_cache_t* cache)
{
    if (!cache) {
        return NULL;
    }
    return &cache->stats;
}

/**
 * kv_cache_get_layer_stats - Get per-layer statistics
 */
const kv_layer_stats_t* kv_cache_get_layer_stats(const kv_cache_t* cache,
                                                  uint32_t layer)
{
    if (!cache || !cache->initialized || layer >= cache->config.n_layers) {
        return NULL;
    }
    return &cache->layers[layer].stats;
}

/**
 * kv_cache_reset_stats - Reset statistics
 */
void kv_cache_reset_stats(kv_cache_t* cache)
{
    if (!cache || !cache->initialized) {
        return;
    }

    /* Keep memory and seq stats, reset operation counters */
    cache->stats.total_hits = 0;
    cache->stats.total_stores = 0;
    cache->stats.total_evictions = 0;
    cache->stats.attention_time_ns = 0;
    cache->stats.no_cache_time_ns = 0;

    for (uint32_t i = 0; i < cache->config.n_layers; i++) {
        kv_memset(&cache->layers[i].stats, 0, sizeof(kv_layer_stats_t));
    }
}

/**
 * kv_cache_print_stats - Print statistics
 */
void kv_cache_print_stats(const kv_cache_t* cache)
{
    if (!cache) {
        console_printf("[KV Cache] No cache\n");
        return;
    }

    console_printf("[KV Cache] Statistics:\n");
    console_printf("  Memory used:      %zu KB\n", cache->stats.memory_used / 1024);
    console_printf("  Current seq len:  %u\n", cache->stats.current_seq_len);
    console_printf("  Peak seq len:     %u\n", cache->stats.peak_seq_len);
    console_printf("  Total stores:     %lu\n", (unsigned long)cache->stats.total_stores);
    console_printf("  Total hits:       %lu\n", (unsigned long)cache->stats.total_hits);
    console_printf("  Total evictions:  %lu\n", (unsigned long)cache->stats.total_evictions);
    console_printf("  Cache resets:     %u\n", cache->stats.n_resets);

    if (cache->stats.total_stores > 0) {
        float hit_rate = (float)cache->stats.total_hits /
                        (float)(cache->stats.total_hits + cache->stats.total_stores) * 100.0f;
        console_printf("  Hit rate:         %.1f%%\n", hit_rate);
    }
}

/**
 * kv_cache_enable - Enable/disable caching
 */
void kv_cache_enable(kv_cache_t* cache, bool enabled)
{
    if (cache) {
        cache->enabled = enabled;
    }
}

/* ============================================================================
 * Global Instance
 * ============================================================================ */

/**
 * kv_cache_get_global - Get global KV cache
 */
kv_cache_t* kv_cache_get_global(void)
{
    barrier();
    return (kv_cache_t*)g_kv_cache;
}

/**
 * kv_cache_set_global - Set global KV cache
 */
void kv_cache_set_global(kv_cache_t* cache)
{
    g_kv_cache = cache;
    barrier();
}
