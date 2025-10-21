/* GGUF Model Loader for TinyLlama
 * Loads real model weights from GGUF format
 */

#include <embodios/types.h>
#include <embodios/kernel.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/gguf.h>

/* String functions */
void* memcpy(void* dest, const void* src, size_t n);
int strcmp(const char* s1, const char* s2);

/* GGUF format structures */
#define GGUF_MAGIC 0x46554747  /* "GGUF" */
#define GGUF_VERSION 3

struct gguf_header {
    uint32_t magic;
    uint32_t version;
    uint64_t n_tensors;
    uint64_t n_kv;
} __attribute__((packed));

struct gguf_kv {
    uint64_t key_length;
    char key[64];
    uint32_t value_type;
    union {
        uint8_t uint8;
        int8_t int8;
        uint16_t uint16;
        int16_t int16;
        uint32_t uint32;
        int32_t int32;
        float float32;
        uint64_t uint64;
        int64_t int64;
        double float64;
        struct {
            uint64_t len;
            char data[256];
        } str;
    } value;
} __attribute__((packed));

struct gguf_tensor {
    uint64_t name_length;
    char name[64];
    uint32_t n_dims;
    uint64_t dims[4];
    uint32_t type;
    uint64_t offset;
} __attribute__((packed));

/* Tensor types */
enum ggml_type {
    GGML_TYPE_F32  = 0,
    GGML_TYPE_F16  = 1,
    GGML_TYPE_Q4_0 = 2,
    GGML_TYPE_Q4_1 = 3,
    GGML_TYPE_Q5_0 = 6,
    GGML_TYPE_Q5_1 = 7,
    GGML_TYPE_Q8_0 = 8,
    GGML_TYPE_Q8_1 = 9,
    GGML_TYPE_Q2_K = 10,
    GGML_TYPE_Q3_K = 11,
    GGML_TYPE_Q4_K = 12,
    GGML_TYPE_Q5_K = 13,
    GGML_TYPE_Q6_K = 14,
    GGML_TYPE_Q8_K = 15,
};

/* Tensor info cache */
#define MAX_TENSORS 512
struct tensor_info {
    char name[128];
    uint32_t n_dims;
    uint64_t dims[4];
    uint32_t type;
    uint64_t offset;
    size_t size;
};

/* Global model data */
static struct {
    void* data;
    size_t size;
    struct gguf_header* header;
    void* tensor_data;

    /* Model parameters */
    uint32_t n_vocab;
    uint32_t n_embd;
    uint32_t n_layer;
    uint32_t n_head;
    uint32_t n_head_kv;
    uint32_t n_ff;
    float rope_theta;
    float norm_eps;

    /* Tensor cache */
    struct tensor_info tensors[MAX_TENSORS];
    int n_tensors_cached;
} g_model;

/* Type sizes */
static size_t ggml_type_size(enum ggml_type type)
{
    switch (type) {
        case GGML_TYPE_F32:  return 4;
        case GGML_TYPE_F16:  return 2;
        case GGML_TYPE_Q4_0: return 18; /* block size 32 */
        case GGML_TYPE_Q4_K: return 144; /* block size 256 */
        case GGML_TYPE_Q5_K: return 176; /* block size 256 */
        default: return 0;
    }
}

/* Calculate tensor size */
static size_t calculate_tensor_size(struct gguf_tensor* tensor)
{
    size_t n_elements = 1;
    for (uint32_t i = 0; i < tensor->n_dims; i++) {
        n_elements *= tensor->dims[i];
    }

    size_t type_size = ggml_type_size(tensor->type);

    /* For quantized types, calculate based on blocks */
    switch (tensor->type) {
        case GGML_TYPE_Q4_0:
        case GGML_TYPE_Q4_K:
        case GGML_TYPE_Q5_K:
            return (n_elements / 256) * type_size;
        default:
            return n_elements * type_size;
    }
}

/* Parse and cache tensor metadata */
static int parse_tensor_metadata(void)
{
    console_printf("GGUF: [DEBUG] parse_tensor_metadata() START\n");
    console_printf("GGUF: [DEBUG] g_model.data=%p size=%zu\n", g_model.data, g_model.size);
    console_printf("GGUF: [DEBUG] n_kv=%llu n_tensors=%llu\n",
                   (unsigned long long)g_model.header->n_kv,
                   (unsigned long long)g_model.header->n_tensors);

    uint8_t* ptr = (uint8_t*)g_model.data + sizeof(struct gguf_header);
    uint8_t* end = (uint8_t*)g_model.data + g_model.size;

    console_printf("GGUF: [DEBUG] ptr=%p end=%p\n", ptr, end);

    /* Skip metadata KV pairs */
    console_printf("GGUF: [DEBUG] Skipping %llu KV pairs...\n", (unsigned long long)g_model.header->n_kv);
    for (uint64_t i = 0; i < g_model.header->n_kv && ptr < end; i++) {
        if (i % 10 == 0) {
            console_printf("GGUF: [DEBUG] KV %llu/%llu ptr=%p\n", (unsigned long long)i,
                          (unsigned long long)g_model.header->n_kv, ptr);
        }

        uint64_t key_len = *(uint64_t*)ptr;
        ptr += 8;

        if (key_len > 1024 || ptr + key_len >= end) {
            console_printf("GGUF: [DEBUG] Invalid key_len=%llu at KV %llu\n",
                          (unsigned long long)key_len, (unsigned long long)i);
            return -1;
        }

        ptr += key_len;  /* Skip key */

        if (ptr + 4 >= end) {
            console_printf("GGUF: Unexpected EOF at value type\n");
            return -1;
        }

        uint32_t value_type = *(uint32_t*)ptr;
        ptr += 4;

        /* Skip value based on type */
        switch (value_type) {
            case 4:  /* uint32 */
            case 5:  /* int32 */
            case 6:  /* float32 */
                ptr += 4;
                break;
            case 7:  /* bool */
                ptr += 1;
                break;
            case 8:  /* string */ {
                uint64_t str_len = *(uint64_t*)ptr;
                ptr += 8;
                if (str_len > 100000 || ptr + str_len >= end) {
                    console_printf("GGUF: Invalid string length\n");
                    return -1;
                }
                ptr += str_len;
                break;
            }
            case 9:  /* array */ {
                uint32_t arr_type = *(uint32_t*)ptr;
                ptr += 4;
                uint64_t arr_len = *(uint64_t*)ptr;
                ptr += 8;

                /* Skip array elements */
                switch (arr_type) {
                    case 4: case 5: case 6:
                        if (ptr + arr_len * 4 > end) {
                            console_printf("GGUF: Array overflow\n");
                            return -1;
                        }
                        ptr += arr_len * 4;
                        break;
                    case 8: {  /* string array */
                        for (uint64_t j = 0; j < arr_len && ptr < end; j++) {
                            if (ptr + 8 > end) {
                                console_printf("GGUF: String array overflow\n");
                                return -1;
                            }
                            uint64_t s_len = *(uint64_t*)ptr;
                            ptr += 8;
                            if (s_len > 100000 || ptr + s_len > end) {
                                console_printf("GGUF: Invalid string in array\n");
                                return -1;
                            }
                            ptr += s_len;
                        }
                        break;
                    }
                    default:
                        if (ptr + arr_len * 8 > end) {
                            console_printf("GGUF: Array overflow (default)\n");
                            return -1;
                        }
                        ptr += arr_len * 8;
                }
                break;
            }
            default:
                if (ptr + 8 > end) {
                    console_printf("GGUF: Default value overflow\n");
                    return -1;
                }
                ptr += 8;
        }
    }

    console_printf("GGUF: [DEBUG] KV pairs done, metadata ends at offset %zu\n",
                   (size_t)(ptr - (uint8_t*)g_model.data));

    /* Parse tensor info */
    int n_tensors = (int)g_model.header->n_tensors;
    console_printf("GGUF: [DEBUG] Parsing %d tensor entries...\n", n_tensors);
    if (n_tensors > MAX_TENSORS) {
        console_printf("GGUF: [WARNING] Too many tensors (%d > %d), limiting\n", n_tensors, MAX_TENSORS);
        n_tensors = MAX_TENSORS;
    }

    for (int i = 0; i < n_tensors && ptr < end; i++) {
        uint64_t name_len = *(uint64_t*)ptr;
        ptr += 8;

        if (name_len >= 128 || ptr + name_len >= end) {
            console_printf("GGUF: Invalid tensor name length\n");
            return -1;
        }

        /* Copy tensor name */
        for (uint64_t j = 0; j < name_len; j++) {
            g_model.tensors[i].name[j] = ptr[j];
        }
        g_model.tensors[i].name[name_len] = '\0';
        ptr += name_len;

        /* Parse dims */
        g_model.tensors[i].n_dims = *(uint32_t*)ptr;
        ptr += 4;

        for (uint32_t j = 0; j < g_model.tensors[i].n_dims && j < 4; j++) {
            g_model.tensors[i].dims[j] = *(uint64_t*)ptr;
            ptr += 8;
        }

        /* Parse type and offset */
        g_model.tensors[i].type = *(uint32_t*)ptr;
        ptr += 4;

        g_model.tensors[i].offset = *(uint64_t*)ptr;
        ptr += 8;

        /* Calculate size */
        struct gguf_tensor tmp;
        tmp.n_dims = g_model.tensors[i].n_dims;
        for (uint32_t j = 0; j < tmp.n_dims; j++) {
            tmp.dims[j] = g_model.tensors[i].dims[j];
        }
        tmp.type = g_model.tensors[i].type;
        g_model.tensors[i].size = calculate_tensor_size(&tmp);

        if (i < 5) {  /* Only print first few */
            console_printf("  [%d] %s: %u dims, type %u, offset %llu, size %zu\n",
                          i, g_model.tensors[i].name, g_model.tensors[i].n_dims,
                          g_model.tensors[i].type, g_model.tensors[i].offset,
                          g_model.tensors[i].size);
        }
    }

    g_model.n_tensors_cached = n_tensors;
    console_printf("GGUF: [DEBUG] Cached %d tensor entries successfully\n", n_tensors);

    /* Align to 256-byte boundary for tensor data */
    size_t metadata_end = (size_t)(ptr - (uint8_t*)g_model.data);
    size_t aligned_offset = (metadata_end + 255) & ~255;
    g_model.tensor_data = (uint8_t*)g_model.data + aligned_offset;

    console_printf("GGUF: [DEBUG] Tensor data starts at offset %zu (aligned from %zu)\n",
                   aligned_offset, metadata_end);
    console_printf("GGUF: [DEBUG] parse_tensor_metadata() SUCCESS\n");

    return 0;
}

/* Load GGUF model from memory */
int gguf_load_model(void* data, size_t size)
{
    console_printf("GGUF: Loading model (%zu MB)\n", size / (1024 * 1024));
    
    if (size < sizeof(struct gguf_header)) {
        console_printf("GGUF: File too small\n");
        return -1;
    }
    
    struct gguf_header* header = (struct gguf_header*)data;
    
    if (header->magic != GGUF_MAGIC) {
        console_printf("GGUF: Invalid magic 0x%x\n", header->magic);
        return -1;
    }
    
    console_printf("GGUF: Version %d, %u tensors, %u metadata\n", 
                   header->version, (unsigned int)header->n_tensors, 
                   (unsigned int)header->n_kv);
    
    console_printf("GGUF: Debug - header at %p, version=%d\n", header, header->version);
    console_printf("GGUF: Debug - n_tensors low 32 bits: %u\n", (unsigned int)(header->n_tensors & 0xFFFFFFFF));
    console_printf("GGUF: Debug - n_kv low 32 bits: %u\n", (unsigned int)(header->n_kv & 0xFFFFFFFF));
    
    g_model.data = data;
    g_model.size = size;
    g_model.header = header;
    
    console_printf("GGUF: About to validate header values...\n");
    
    /* Validate header values */
    if (header->n_kv > 1000 || header->n_tensors > 10000) {
        console_printf("GGUF: Invalid header values (n_kv=%u, n_tensors=%u)\n",
                      (unsigned)header->n_kv, (unsigned)header->n_tensors);
        return -1;
    }
    
    console_printf("GGUF: Header validation passed\n");
    console_printf("GGUF: Parsing metadata...\n");
    
    /* Parse metadata */
    uint8_t* ptr = (uint8_t*)data + sizeof(struct gguf_header);
    uint8_t* end = (uint8_t*)data + size;
    
    /* For now, skip full metadata parsing and use defaults for TinyLlama */
    console_printf("GGUF: Using TinyLlama-1.1B defaults\n");
    g_model.n_vocab = 32000;
    g_model.n_embd = 2048;
    g_model.n_layer = 22;
    g_model.n_head = 32;
    g_model.n_head_kv = 4;
    g_model.n_ff = 5632;
    
    console_printf("  vocab_size: %u\n", g_model.n_vocab);
    console_printf("  embedding_length: %u\n", g_model.n_embd);
    console_printf("  layers: %u\n", g_model.n_layer);
    console_printf("  heads: %u\n", g_model.n_head);
    
    /* Parse tensor metadata to build cache */
    if (parse_tensor_metadata() < 0) {
        console_printf("GGUF: Failed to parse tensor metadata\n");
        return -1;
    }

    console_printf("GGUF: Loaded model successfully\n");
    console_printf("GGUF: Returning from gguf_load_model\n");
    return 0;
}

#if 0
    /* Full parsing code - temporarily disabled */
    for (uint64_t i = 0; i < header->n_kv; i++) {
        if (i % 5 == 0) {
            console_printf("  Processing metadata %u/%u\n", (unsigned)i, (unsigned)header->n_kv);
        }
        
        uint64_t key_len = *(uint64_t*)ptr;
        ptr += 8;
        
        char key[256];
        if (key_len < 256) {
            memcpy(key, ptr, key_len);
            key[key_len] = '\0';
        }
        ptr += key_len;
        
        uint32_t value_type = *(uint32_t*)ptr;
        ptr += 4;
        
        /* Parse model parameters */
        if (strcmp(key, "llama.vocab_size") == 0) {
            g_model.n_vocab = *(uint32_t*)ptr;
            console_printf("  vocab_size: %u\n", g_model.n_vocab);
        }
        else if (strcmp(key, "llama.embedding_length") == 0) {
            g_model.n_embd = *(uint32_t*)ptr;
            console_printf("  embedding_length: %u\n", g_model.n_embd);
        }
        else if (strcmp(key, "llama.block_count") == 0) {
            g_model.n_layer = *(uint32_t*)ptr;
            console_printf("  layers: %u\n", g_model.n_layer);
        }
        else if (strcmp(key, "llama.attention.head_count") == 0) {
            g_model.n_head = *(uint32_t*)ptr;
            console_printf("  heads: %u\n", g_model.n_head);
        }
        else if (strcmp(key, "llama.attention.head_count_kv") == 0) {
            g_model.n_head_kv = *(uint32_t*)ptr;
            console_printf("  kv_heads: %u\n", g_model.n_head_kv);
        }
        else if (strcmp(key, "llama.feed_forward_length") == 0) {
            g_model.n_ff = *(uint32_t*)ptr;
            console_printf("  ff_length: %u\n", g_model.n_ff);
        }
        
        /* Skip value based on type */
        switch (value_type) {
            case 4: ptr += 4; break;  /* uint32 */
            case 5: ptr += 4; break;  /* int32 */
            case 6: ptr += 4; break;  /* float32 */
            case 8: /* string */
                ptr += 8 + *(uint64_t*)ptr;
                break;
            default:
                ptr += 8;
        }
    }
    
    /* Find tensor data offset */
    /* Align to 256-byte boundary */
    size_t offset = (ptr - (uint8_t*)data);
    offset = (offset + 255) & ~255;
    
    g_model.tensor_data = (uint8_t*)data + offset;
    
    console_printf("GGUF: Model loaded successfully\n");
    console_printf("  Parameters: %u layers, %u dim, %u heads\n",
                   g_model.n_layer, g_model.n_embd, g_model.n_head);
    
    return 0;
#endif

/* Get tensor by name */
void* gguf_get_tensor(const char* name, size_t* out_size)
{
    if (!g_model.data || !g_model.tensor_data) {
        console_printf("GGUF: Model not loaded\n");
        return NULL;
    }

    /* Search cached tensors */
    for (int i = 0; i < g_model.n_tensors_cached; i++) {
        if (strcmp(g_model.tensors[i].name, name) == 0) {
            /* Found it! */
            if (out_size) {
                *out_size = g_model.tensors[i].size;
            }

            void* tensor_ptr = (uint8_t*)g_model.tensor_data + g_model.tensors[i].offset;
            console_printf("GGUF: Found '%s' at offset %llu, size %zu\n",
                          name, g_model.tensors[i].offset, g_model.tensors[i].size);
            return tensor_ptr;
        }
    }

    console_printf("GGUF: Tensor '%s' not found\n", name);
    return NULL;
    
#if 0
    uint8_t* ptr = (uint8_t*)g_model.data + sizeof(struct gguf_header);
    
    /* Skip metadata */
    for (uint64_t i = 0; i < g_model.header->n_kv; i++) {
        /* Skip KV pairs (simplified) */
        ptr += 1024; /* Approximate */
    }
    
    /* Parse tensors */
    for (uint64_t i = 0; i < g_model.header->n_tensors; i++) {
        uint64_t name_len = *(uint64_t*)ptr;
        ptr += 8;
        
        char tensor_name[256];
        if (name_len < 256) {
            memcpy(tensor_name, ptr, name_len);
            tensor_name[name_len] = '\0';
        }
        ptr += name_len;
        
        struct gguf_tensor tensor;
        tensor.n_dims = *(uint32_t*)ptr;
        ptr += 4;
        
        for (uint32_t j = 0; j < tensor.n_dims; j++) {
            tensor.dims[j] = *(uint64_t*)ptr;
            ptr += 8;
        }
        
        tensor.type = *(uint32_t*)ptr;
        ptr += 4;
        
        tensor.offset = *(uint64_t*)ptr;
        ptr += 8;
        
        if (strcmp(tensor_name, name) == 0) {
            if (out_size) {
                *out_size = calculate_tensor_size(&tensor);
            }
            return (uint8_t*)g_model.tensor_data + tensor.offset;
        }
    }
    
    return NULL;
#endif
}

/* Get model configuration */
void gguf_get_model_config(struct gguf_model_config* config)
{
    config->n_vocab = g_model.n_vocab;
    config->n_embd = g_model.n_embd;
    config->n_layer = g_model.n_layer;
    config->n_head = g_model.n_head;
    config->n_head_kv = g_model.n_head_kv;
    config->n_ff = g_model.n_ff;
}

/* Load token embeddings for TinyLlama */
float* load_token_embeddings(const uint8_t* gguf_data, size_t gguf_size)
{
    console_printf("GGUF: Loading token embeddings\n");

    /* Initialize GGUF loader if needed */
    if (!g_model.data) {
        if (gguf_load_model((void*)gguf_data, gguf_size) < 0) {
            console_printf("GGUF: Failed to load model\n");
            return NULL;
        }
    }

    /* Find token embedding tensor */
    size_t tensor_size;
    void* tensor_data = gguf_get_tensor("token_embd.weight", &tensor_size);
    if (!tensor_data) {
        /* Try alternative name */
        tensor_data = gguf_get_tensor("model.embed_tokens.weight", &tensor_size);
    }

    if (!tensor_data) {
        console_printf("GGUF: Token embeddings not found\n");
        return NULL;
    }

    /* Allocate float array */
    size_t n_elements = g_model.n_vocab * g_model.n_embd;
    float* embeddings = (float*)kmalloc(n_elements * sizeof(float));
    if (!embeddings) {
        console_printf("GGUF: Failed to allocate %zu MB for embeddings\n",
                      (n_elements * sizeof(float)) / (1024*1024));
        return NULL;
    }

    /* For now, just use simple dequantization or copy if F32 */
    /* TODO: Implement proper Q4_K dequantization */
    memcpy(embeddings, tensor_data, n_elements * sizeof(float));

    console_printf("GGUF: Loaded %zu embeddings (%zu MB)\n",
                  n_elements, (n_elements * sizeof(float)) / (1024*1024));
    return embeddings;
}

/* Load output normalization weights */
float* load_output_norm(const uint8_t* gguf_data, size_t gguf_size)
{
    (void)gguf_data;
    (void)gguf_size;

    console_printf("GGUF: Loading output norm\n");

    size_t tensor_size;
    void* tensor_data = gguf_get_tensor("output_norm.weight", &tensor_size);
    if (!tensor_data) {
        tensor_data = gguf_get_tensor("model.norm.weight", &tensor_size);
    }

    if (!tensor_data) {
        console_printf("GGUF: Output norm not found\n");
        return NULL;
    }

    /* Allocate and copy */
    float* norm = (float*)kmalloc(g_model.n_embd * sizeof(float));
    if (!norm) return NULL;

    memcpy(norm, tensor_data, g_model.n_embd * sizeof(float));
    console_printf("GGUF: Loaded output norm (%d elements)\n", g_model.n_embd);
    return norm;
}

/* Load output projection weights (LM head) */
float* load_output_weight(const uint8_t* gguf_data, size_t gguf_size)
{
    (void)gguf_data;
    (void)gguf_size;

    console_printf("GGUF: Loading output weight\n");

    size_t tensor_size;
    void* tensor_data = gguf_get_tensor("output.weight", &tensor_size);
    if (!tensor_data) {
        tensor_data = gguf_get_tensor("lm_head.weight", &tensor_size);
    }

    if (!tensor_data) {
        console_printf("GGUF: Output weight not found (may share embeddings)\n");
        /* Some models share embeddings with LM head */
        return NULL;
    }

    size_t n_elements = g_model.n_vocab * g_model.n_embd;
    float* weights = (float*)kmalloc(n_elements * sizeof(float));
    if (!weights) return NULL;

    memcpy(weights, tensor_data, n_elements * sizeof(float));
    console_printf("GGUF: Loaded output weight (%zu MB)\n",
                  (n_elements * sizeof(float)) / (1024*1024));
    return weights;
}

/* Load specific layer weight by name */
float* load_layer_weight(const uint8_t* gguf_data, size_t gguf_size,
                        const char* weight_name, size_t expected_elements)
{
    (void)gguf_data;
    (void)gguf_size;

    console_printf("GGUF: Loading %s\n", weight_name);

    size_t tensor_size;
    void* tensor_data = gguf_get_tensor(weight_name, &tensor_size);

    if (!tensor_data) {
        console_printf("GGUF: %s not found\n", weight_name);
        return NULL;
    }

    /* Allocate and copy */
    float* weights = (float*)kmalloc(expected_elements * sizeof(float));
    if (!weights) {
        console_printf("GGUF: Failed to allocate memory for %s\n", weight_name);
        return NULL;
    }

    /* Copy up to expected_elements */
    size_t copy_size = (tensor_size < expected_elements * sizeof(float)) ?
                       tensor_size : expected_elements * sizeof(float);
    memcpy(weights, tensor_data, copy_size);

    console_printf("GGUF: Loaded %s (%zu elements)\n", weight_name, expected_elements);
    return weights;
}