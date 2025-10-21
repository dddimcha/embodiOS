#ifndef EMBODIOS_GGUF_H
#define EMBODIOS_GGUF_H

#include <embodios/types.h>

/* GGUF model configuration */
struct gguf_model_config {
    uint32_t n_vocab;
    uint32_t n_embd;
    uint32_t n_layer;
    uint32_t n_head;
    uint32_t n_head_kv;
    uint32_t n_ff;
};

/* GGUF loader functions */
int gguf_load_model(void* data, size_t size);
void gguf_get_model_config(struct gguf_model_config* config);
void* gguf_get_tensor(const char* name, size_t* out_size);

#endif /* EMBODIOS_GGUF_H */