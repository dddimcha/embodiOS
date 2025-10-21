/*
 * KV Cache Implementation for Transformer Attention
 * Caches key/value tensors to avoid recomputation
 */

#include <embodios/types.h>
#include <embodios/mm.h>
#include <embodios/console.h>

#define MAX_SEQ_LEN 2048
#define MAX_LAYERS 32

typedef struct {
    fixed_t* k_cache;
    fixed_t* v_cache;
    uint32_t seq_len;
    uint32_t n_embd;
} layer_kv_cache_t;

static layer_kv_cache_t g_kv_cache[MAX_LAYERS];
static int g_cache_initialized = 0;

int kv_cache_init(uint32_t n_layers, uint32_t n_embd) {
    if (g_cache_initialized) {
        return 0;
    }

    console_printf("[KV Cache] Initializing for %u layers, dim=%u\n", n_layers, n_embd);

    for (uint32_t i = 0; i < n_layers && i < MAX_LAYERS; i++) {
        size_t cache_size = MAX_SEQ_LEN * n_embd * sizeof(fixed_t);

        g_kv_cache[i].k_cache = (fixed_t*)kmalloc(cache_size);
        g_kv_cache[i].v_cache = (fixed_t*)kmalloc(cache_size);

        if (!g_kv_cache[i].k_cache || !g_kv_cache[i].v_cache) {
            console_printf("[KV Cache] Failed to allocate for layer %u\n", i);
            return -1;
        }

        g_kv_cache[i].seq_len = 0;
        g_kv_cache[i].n_embd = n_embd;
    }

    g_cache_initialized = 1;
    console_printf("[KV Cache] Initialized successfully\n");
    return 0;
}

void kv_cache_reset(void) {
    for (uint32_t i = 0; i < MAX_LAYERS; i++) {
        g_kv_cache[i].seq_len = 0;
    }
}

int kv_cache_append(uint32_t layer_idx, const fixed_t* k, const fixed_t* v, uint32_t n_embd) {
    if (layer_idx >= MAX_LAYERS) {
        return -1;
    }

    layer_kv_cache_t* cache = &g_kv_cache[layer_idx];

    if (cache->seq_len >= MAX_SEQ_LEN) {
        console_printf("[KV Cache] Layer %u cache full\n", layer_idx);
        return -1;
    }

    /* Append new key/value */
    fixed_t* k_dst = &cache->k_cache[cache->seq_len * n_embd];
    fixed_t* v_dst = &cache->v_cache[cache->seq_len * n_embd];

    for (uint32_t i = 0; i < n_embd; i++) {
        k_dst[i] = k[i];
        v_dst[i] = v[i];
    }

    cache->seq_len++;
    return 0;
}

const fixed_t* kv_cache_get_keys(uint32_t layer_idx, uint32_t* out_seq_len) {
    if (layer_idx >= MAX_LAYERS) {
        return NULL;
    }

    *out_seq_len = g_kv_cache[layer_idx].seq_len;
    return g_kv_cache[layer_idx].k_cache;
}

const fixed_t* kv_cache_get_values(uint32_t layer_idx, uint32_t* out_seq_len) {
    if (layer_idx >= MAX_LAYERS) {
        return NULL;
    }

    *out_seq_len = g_kv_cache[layer_idx].seq_len;
    return g_kv_cache[layer_idx].v_cache;
}

uint32_t kv_cache_get_seq_len(uint32_t layer_idx) {
    if (layer_idx >= MAX_LAYERS) {
        return 0;
    }
    return g_kv_cache[layer_idx].seq_len;
}
