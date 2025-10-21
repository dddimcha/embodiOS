#ifndef EMBODIOS_KV_CACHE_H
#define EMBODIOS_KV_CACHE_H

#include <embodios/types.h>

int kv_cache_init(uint32_t n_layers, uint32_t n_embd);
void kv_cache_reset(void);
int kv_cache_append(uint32_t layer_idx, const fixed_t* k, const fixed_t* v, uint32_t n_embd);
const fixed_t* kv_cache_get_keys(uint32_t layer_idx, uint32_t* out_seq_len);
const fixed_t* kv_cache_get_values(uint32_t layer_idx, uint32_t* out_seq_len);
uint32_t kv_cache_get_seq_len(uint32_t layer_idx);

#endif
