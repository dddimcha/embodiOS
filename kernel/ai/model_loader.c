/* EMBODIOS Model Loader
 *
 * Loads pre-trained models in standard formats
 * Supports: GGUF, SafeTensors, PyTorch formats
 *
 * Performance Optimization:
 * - Pre-computes embedding tables at load time
 * - Target: ~15% inference speedup (1.15x)
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/ai.h>
#include <embodios/embeddings.h>
#include <embodios/gguf.h>

/* Use kernel's string functions */
extern char* strcpy(char* dest, const char* src);
extern const char* strstr(const char* haystack, const char* needle);
extern size_t strlen(const char* s);

/**
 * safe_strncpy - Safe string copy with bounds checking
 * @dest: Destination buffer
 * @src: Source string
 * @size: Size of destination buffer
 *
 * Always null-terminates. Returns dest.
 */
static char* safe_strncpy(char* dest, const char* src, size_t size)
{
    if (size == 0) return dest;

    size_t i;
    for (i = 0; i < size - 1 && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
    return dest;
}

/* Model format detection */
#define GGUF_MAGIC 0x46554747  /* "GGUF" */
#define GGML_MAGIC 0x67676d6c  /* "ggml" */
#define SAFETENSORS_MAGIC 0x7B226865  /* '{"he' */

/* GGUF format structures */
struct gguf_header {
    uint32_t magic;
    uint32_t version;
    uint64_t n_tensors;
    uint64_t n_kv;
};

struct gguf_kv {
    char key[64];
    uint32_t type;
    union {
        uint64_t u64;
        int64_t i64;
        float f32;
        double f64;
        char str[256];
    } value;
};

/* Forward declarations */
struct embodios_model* load_gguf_model(void* data, size_t size);
struct embodios_model* load_ggml_model(void* data, size_t size);
struct embodios_model* load_safetensors_model(void* data, size_t size);
static int init_embedding_cache(struct embodios_model* model);

/* Load model from memory */
struct embodios_model* load_model_from_memory(void* data, size_t size)
{
    console_printf("Model Loader: Analyzing model format...\n");
    
    if (size < 16) {
        console_printf("Model Loader: File too small\n");
        return NULL;
    }
    
    uint32_t magic = *(uint32_t*)data;
    
    /* Check for GGUF format (most common for LLMs) */
    if (magic == GGUF_MAGIC) {
        console_printf("Model Loader: Detected GGUF format\n");
        return load_gguf_model(data, size);
    }
    
    /* Check for GGML format (older) */
    if (magic == GGML_MAGIC) {
        console_printf("Model Loader: Detected GGML format\n");
        return load_ggml_model(data, size);
    }
    
    /* Check for SafeTensors format */
    if (magic == SAFETENSORS_MAGIC) {
        console_printf("Model Loader: Detected SafeTensors format\n");
        return load_safetensors_model(data, size);
    }
    
    console_printf("Model Loader: Unknown format (magic: 0x%x)\n", magic);
    return NULL;
}

/* Load GGUF format model */
struct embodios_model* load_gguf_model(void* data, size_t size)
{
    struct gguf_header* header = (struct gguf_header*)data;
    
    console_printf("GGUF Model:\n");
    console_printf("  Version: %d\n", header->version);
    console_printf("  Tensors: %lu\n", header->n_tensors);
    console_printf("  Metadata: %lu entries\n", header->n_kv);
    
    /* Allocate model structure */
    struct embodios_model* model = kmalloc(sizeof(struct embodios_model));
    if (!model) {
        console_printf("Model Loader: Failed to allocate model\n");
        return NULL;
    }
    
    /* Initialize model */
    model->magic = 0x454D424F;
    model->version_major = 1;
    model->version_minor = 0;
    model->data = data;
    model->size = size;
    
    /* Parse metadata to get model info */
    uint8_t* ptr = (uint8_t*)data + sizeof(struct gguf_header);
    
    /* Default to TinyLlama if not specified */
    safe_strncpy(model->name, "TinyLlama-1.1B", sizeof(model->name));
    safe_strncpy(model->arch, "llama", sizeof(model->arch));
    model->param_count = 1100000000;
    model->capabilities = MODEL_CAP_TEXT_GEN;

    /* Calculate end boundary for bounds checking */
    uint8_t* data_end = (uint8_t*)data + size;

    /* Read key-value metadata with bounds checking */
    for (uint64_t i = 0; i < header->n_kv; i++) {
        /* Bounds check: ensure we have space for the KV structure */
        if (ptr + sizeof(struct gguf_kv) > data_end) {
            console_printf("Model Loader: KV metadata extends beyond buffer\n");
            break;
        }

        struct gguf_kv* kv = (struct gguf_kv*)ptr;

        if (strstr(kv->key, "model.name")) {
            safe_strncpy(model->name, kv->value.str, sizeof(model->name));
            console_printf("  Model name: %s\n", model->name);
        }
        else if (strstr(kv->key, "general.architecture")) {
            safe_strncpy(model->arch, kv->value.str, sizeof(model->arch));
            console_printf("  Architecture: %s\n", model->arch);
        }
        else if (strstr(kv->key, "model.n_params")) {
            model->param_count = kv->value.u64;
            console_printf("  Parameters: %lu\n", model->param_count);
        }

        /* Move to next KV pair */
        ptr += sizeof(struct gguf_kv);
    }
    
    /* Store tensor data location */
    model->tensor_data = ptr;

    console_printf("Model Loader: GGUF model loaded successfully\n");

    /* Initialize embedding cache for optimized inference */
    if (init_embedding_cache(model) < 0) {
        console_printf("Model Loader: Continuing without embedding cache\n");
    }

    return model;
}

/* Load GGML format model */
struct embodios_model* load_ggml_model(void* data, size_t size)
{
    console_printf("Model Loader: GGML format support coming soon\n");
    
    /* For now, create a placeholder model */
    struct embodios_model* model = kmalloc(sizeof(struct embodios_model));
    if (!model) return NULL;
    
    model->magic = 0x454D424F;
    safe_strncpy(model->name, "GGML Model", sizeof(model->name));
    safe_strncpy(model->arch, "unknown", sizeof(model->arch));
    model->data = data;
    model->size = size;

    return model;
}

/* Load SafeTensors format model */
struct embodios_model* load_safetensors_model(void* data, size_t size)
{
    console_printf("Model Loader: SafeTensors format support coming soon\n");

    /* For now, create a placeholder model */
    struct embodios_model* model = kmalloc(sizeof(struct embodios_model));
    if (!model) return NULL;

    model->magic = 0x454D424F;
    safe_strncpy(model->name, "SafeTensors Model", sizeof(model->name));
    safe_strncpy(model->arch, "unknown", sizeof(model->arch));
    model->data = data;
    model->size = size;

    return model;
}

/* Get tensor from model by name */
void* model_get_tensor(struct embodios_model* model, const char* name, size_t* out_size)
{
    if (!model || !model->tensor_data) {
        console_printf("Model Loader: No tensor data\n");
        return NULL;
    }
    
    /* TODO: Implement tensor lookup from GGUF format */
    console_printf("Model Loader: Tensor lookup for '%s' not yet implemented\n", name);
    
    return NULL;
}

/* String functions implemented in lib/string.c */

/* ============================================================================
 * Embedding Cache Integration
 * ============================================================================
 *
 * Pre-computes embeddings at model load time for optimized inference.
 */

/**
 * init_embedding_cache - Initialize embedding cache for model
 * @model: Loaded model structure
 *
 * Creates and initializes the embedding cache using model configuration.
 * Pre-computes embeddings for fast lookup during inference.
 *
 * Return: 0 on success, negative error code on failure
 */
static int init_embedding_cache(struct embodios_model* model)
{
    if (!model) {
        console_printf("Model Loader: Cannot init embeddings - no model\n");
        return -1;
    }

    console_printf("Model Loader: Initializing embedding cache...\n");

    /* Get model configuration from GGUF */
    struct gguf_model_config gguf_config;
    gguf_get_model_config(&gguf_config);

    /* Configure embedding cache */
    embedding_config_t config = {
        .vocab_size = gguf_config.n_vocab > 0 ? gguf_config.n_vocab : 32000,
        .embedding_dim = gguf_config.n_embd > 0 ? gguf_config.n_embd : 2048,
        .max_seq_len = 2048,
        .cache_positions = EMBEDDING_CACHE_POSITIONS,
        .use_position_emb = true,
        .use_combined_cache = true
    };

    console_printf("Model Loader: Embedding config:\n");
    console_printf("  Vocab: %u, Dim: %u, MaxSeq: %u\n",
                   config.vocab_size, config.embedding_dim, config.max_seq_len);

    /* Calculate and report memory usage */
    size_t mem_required = embedding_memory_required(&config);
    console_printf("Model Loader: Embedding memory required: %zu KB\n",
                   mem_required / 1024);

    /* Initialize cache */
    embedding_cache_t* cache = embedding_cache_init(&config);
    if (!cache) {
        console_printf("Model Loader: WARNING - Failed to init embedding cache\n");
        console_printf("Model Loader: Inference will compute embeddings on-the-fly\n");
        return -1;
    }

    /* Load weights from model */
    int ret = embedding_cache_load_weights(cache, model);
    if (ret < 0) {
        console_printf("Model Loader: WARNING - Failed to load embedding weights\n");
        /* Continue anyway - may use generated weights */
    }

    /* Pre-compute embeddings */
    ret = embedding_cache_precompute(cache);
    if (ret < 0) {
        console_printf("Model Loader: WARNING - Failed to pre-compute embeddings\n");
    }

    /* Set as global cache for transformer */
    embedding_set_global(cache);

    console_printf("Model Loader: Embedding cache ready\n");

    /* Print initial stats */
    embedding_print_stats(cache);

    return 0;
}