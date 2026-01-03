/**
 * GGUF Parser with Enhanced Metadata Extraction
 *
 * Implements full GGUF format parsing with:
 * - Support for GGUF versions 1, 2, 3
 * - Complete metadata KV extraction
 * - Model architecture parsing
 * - Vocabulary extraction
 * - Metadata validation
 * - Debug logging
 *
 * Reference: llama.cpp/gguf-py/gguf/gguf_reader.py
 */

#include <embodios/types.h>
#include <embodios/kernel.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/gguf_parser.h>
#include <embodios/block.h>

/* ============================================================================
 * GGUF Format Constants
 * ============================================================================ */

#define GGUF_MAGIC          0x46554747  /* "GGUF" in little-endian */
#define GGUF_MAGIC_V1       0x67676A74  /* "tjgg" - old GGML format */
#define GGUF_VERSION_1      1
#define GGUF_VERSION_2      2
#define GGUF_VERSION_3      3
#define GGUF_DEFAULT_ALIGN  32

/* Maximum limits for safety */
#define GGUF_MAX_KV_PAIRS       4096
#define GGUF_MAX_TENSORS        65536
#define GGUF_MAX_STRING_LEN     1048576  /* 1 MB */
#define GGUF_MAX_ARRAY_LEN      16777216 /* 16 M elements */
#define GGUF_MAX_KEY_LEN        256
#define GGUF_MAX_VOCAB_SIZE     256000

/* ============================================================================
 * GGUF Type Definitions
 * ============================================================================ */

/* GGUF value types */
enum gguf_type {
    GGUF_TYPE_UINT8   = 0,
    GGUF_TYPE_INT8    = 1,
    GGUF_TYPE_UINT16  = 2,
    GGUF_TYPE_INT16   = 3,
    GGUF_TYPE_UINT32  = 4,
    GGUF_TYPE_INT32   = 5,
    GGUF_TYPE_FLOAT32 = 6,
    GGUF_TYPE_BOOL    = 7,
    GGUF_TYPE_STRING  = 8,
    GGUF_TYPE_ARRAY   = 9,
    GGUF_TYPE_UINT64  = 10,
    GGUF_TYPE_INT64   = 11,
    GGUF_TYPE_FLOAT64 = 12,
    GGUF_TYPE_COUNT
};

/* Use ggml_type_t from header */

/* ============================================================================
 * GGUF Header Structures
 * ============================================================================ */

/* GGUF file header - version 3 */
struct gguf_header_v3 {
    uint32_t magic;
    uint32_t version;
    uint64_t n_tensors;
    uint64_t n_kv;
} __attribute__((packed));

/* GGUF file header - version 1/2 (uses uint32 for counts) */
struct gguf_header_v1 {
    uint32_t magic;
    uint32_t version;
    uint32_t n_tensors;
    uint32_t n_kv;
} __attribute__((packed));

/* ============================================================================
 * Vocabulary Token Entry
 * ============================================================================ */

struct gguf_vocab_token {
    char* text;
    float score;
    uint32_t type;  /* 0=normal, 1=unknown, 2=control, 3=user_defined, etc. */
};

/* Maximum tensors to store (for most models) */
#define GGUF_MAX_STORED_TENSORS 4096

/* ============================================================================
 * GGUF Parser Context
 * ============================================================================ */

struct gguf_parser_ctx {
    /* Raw data */
    const uint8_t* data;
    size_t size;

    /* Header info */
    uint32_t version;
    uint64_t n_tensors;
    uint64_t n_kv;

    /* Parsed positions */
    const uint8_t* kv_start;
    const uint8_t* tensor_info_start;
    const uint8_t* tensor_data_start;
    size_t alignment;

    /* Model architecture */
    struct gguf_model_arch arch;

    /* Vocabulary */
    struct gguf_vocab_token* vocab;
    uint32_t vocab_count;
    float* vocab_scores;
    uint32_t* vocab_types;

    /* Tensor info storage */
    struct gguf_tensor_info* tensors;
    uint64_t tensor_count;

    /* Type statistics for detecting model quantization */
    uint32_t type_counts[GGML_TYPE_COUNT];
    ggml_type_t predominant_type;

    /* Validation */
    uint8_t is_valid;
    char error_msg[256];

    /* Debug flags */
    uint8_t debug_enabled;
};

/* Global parser context */
static struct gguf_parser_ctx g_ctx;

/* ============================================================================
 * Debug Logging
 * ============================================================================ */

#define GGUF_DEBUG(fmt, ...) \
    do { if (g_ctx.debug_enabled) console_printf("[GGUF DEBUG] " fmt "\n", ##__VA_ARGS__); } while(0)

#define GGUF_INFO(fmt, ...) \
    console_printf("[GGUF] " fmt "\n", ##__VA_ARGS__)

#define GGUF_ERROR(fmt, ...) \
    do { \
        console_printf("[GGUF ERROR] " fmt "\n", ##__VA_ARGS__); \
        /* Store error message */ \
    } while(0)

/* ============================================================================
 * Type Size Helpers
 * ============================================================================ */

static const char* gguf_type_name(enum gguf_type type)
{
    static const char* names[] = {
        "uint8", "int8", "uint16", "int16", "uint32", "int32",
        "float32", "bool", "string", "array", "uint64", "int64", "float64"
    };
    if (type < GGUF_TYPE_COUNT) return names[type];
    return "unknown";
}

static size_t gguf_type_size(enum gguf_type type)
{
    static const size_t sizes[] = {
        1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8
    };
    if (type < GGUF_TYPE_COUNT) return sizes[type];
    return 0;
}

/* Block sizes for GGML types */
static const size_t ggml_block_sizes[] = {
    [GGML_TYPE_F32]   = 4,
    [GGML_TYPE_F16]   = 2,
    [GGML_TYPE_Q4_0]  = 18,   /* 2 + 16 */
    [GGML_TYPE_Q4_1]  = 20,   /* 4 + 16 */
    [GGML_TYPE_Q5_0]  = 22,   /* 2 + 4 + 16 */
    [GGML_TYPE_Q5_1]  = 24,   /* 4 + 4 + 16 */
    [GGML_TYPE_Q8_0]  = 34,   /* 2 + 32 */
    [GGML_TYPE_Q8_1]  = 36,   /* 4 + 32 */
    [GGML_TYPE_Q2_K]  = 84,
    [GGML_TYPE_Q3_K]  = 110,
    [GGML_TYPE_Q4_K]  = 144,  /* 4 + 12 + 128 */
    [GGML_TYPE_Q5_K]  = 176,  /* 4 + 12 + 32 + 128 */
    [GGML_TYPE_Q6_K]  = 210,  /* 128 + 64 + 16 + 2 */
    [GGML_TYPE_Q8_K]  = 292,
};

/* Elements per block for GGML types */
static const size_t ggml_block_elements[] = {
    [GGML_TYPE_F32]   = 1,
    [GGML_TYPE_F16]   = 1,
    [GGML_TYPE_Q4_0]  = 32,
    [GGML_TYPE_Q4_1]  = 32,
    [GGML_TYPE_Q5_0]  = 32,
    [GGML_TYPE_Q5_1]  = 32,
    [GGML_TYPE_Q8_0]  = 32,
    [GGML_TYPE_Q8_1]  = 32,
    [GGML_TYPE_Q2_K]  = 256,
    [GGML_TYPE_Q3_K]  = 256,
    [GGML_TYPE_Q4_K]  = 256,
    [GGML_TYPE_Q5_K]  = 256,
    [GGML_TYPE_Q6_K]  = 256,
    [GGML_TYPE_Q8_K]  = 256,
};

const char* ggml_type_name(ggml_type_t type)
{
    static const char* names[] = {
        [GGML_TYPE_F32]   = "F32",
        [GGML_TYPE_F16]   = "F16",
        [GGML_TYPE_Q4_0]  = "Q4_0",
        [GGML_TYPE_Q4_1]  = "Q4_1",
        [GGML_TYPE_Q5_0]  = "Q5_0",
        [GGML_TYPE_Q5_1]  = "Q5_1",
        [GGML_TYPE_Q8_0]  = "Q8_0",
        [GGML_TYPE_Q8_1]  = "Q8_1",
        [GGML_TYPE_Q2_K]  = "Q2_K",
        [GGML_TYPE_Q3_K]  = "Q3_K",
        [GGML_TYPE_Q4_K]  = "Q4_K",
        [GGML_TYPE_Q5_K]  = "Q5_K",
        [GGML_TYPE_Q6_K]  = "Q6_K",
        [GGML_TYPE_Q8_K]  = "Q8_K",
    };
    if (type < GGML_TYPE_COUNT && names[type]) return names[type];
    return "unknown";
}

size_t ggml_type_block_size(ggml_type_t type)
{
    if (type < GGML_TYPE_COUNT) return ggml_block_sizes[type];
    return 0;
}

size_t ggml_type_block_elements(ggml_type_t type)
{
    if (type < GGML_TYPE_COUNT) return ggml_block_elements[type];
    return 0;
}

/* ============================================================================
 * Safe Read Helpers
 * ============================================================================ */

static inline int safe_read_u8(const uint8_t** ptr, const uint8_t* end, uint8_t* out)
{
    if (*ptr + 1 > end) return -1;
    *out = **ptr;
    (*ptr)++;
    return 0;
}

static inline int safe_read_u32(const uint8_t** ptr, const uint8_t* end, uint32_t* out)
{
    if (*ptr + 4 > end) return -1;
    memcpy(out, *ptr, 4);  /* Use memcpy for safe unaligned access */
    (*ptr) += 4;
    return 0;
}

static inline int safe_read_u64(const uint8_t** ptr, const uint8_t* end, uint64_t* out)
{
    if (*ptr + 8 > end) return -1;
    memcpy(out, *ptr, 8);  /* Use memcpy for safe unaligned access */
    (*ptr) += 8;
    return 0;
}

static inline int safe_read_f32(const uint8_t** ptr, const uint8_t* end, float* out)
{
    if (*ptr + 4 > end) return -1;
    memcpy(out, *ptr, 4);  /* Use memcpy for safe unaligned access */
    (*ptr) += 4;
    return 0;
}

static int safe_read_string(const uint8_t** ptr, const uint8_t* end,
                            char* out, size_t out_size, uint64_t* len_out)
{
    uint64_t len;
    if (safe_read_u64(ptr, end, &len) < 0) return -1;

    if (len > GGUF_MAX_STRING_LEN || *ptr + len > end) {
        GGUF_ERROR("String too long: %llu", (unsigned long long)len);
        return -1;
    }

    if (len_out) *len_out = len;

    if (out && out_size > 0) {
        size_t copy_len = (len < out_size - 1) ? len : out_size - 1;
        memcpy(out, *ptr, copy_len);
        out[copy_len] = '\0';
    }

    (*ptr) += len;
    return 0;
}

/* ============================================================================
 * Header Parsing
 * ============================================================================ */

static int gguf_parse_header(void)
{
    const uint8_t* ptr = g_ctx.data;

    /* Check minimum size */
    if (g_ctx.size < 16) {
        GGUF_ERROR("File too small: %zu bytes", g_ctx.size);
        return -1;
    }

    /* Read magic - use memcpy for safe unaligned access */
    uint32_t magic;
    memcpy(&magic, ptr, 4);

    if (magic == GGUF_MAGIC) {
        GGUF_DEBUG("Found GGUF magic");
    } else if (magic == GGUF_MAGIC_V1) {
        GGUF_ERROR("Old GGML format not supported (magic: 0x%08x)", magic);
        return -1;
    } else {
        GGUF_ERROR("Invalid magic: 0x%08x (expected 0x%08x)", magic, GGUF_MAGIC);
        return -1;
    }

    /* Read version - use memcpy for safe unaligned access */
    uint32_t version;
    memcpy(&version, ptr + 4, 4);
    g_ctx.version = version;

    GGUF_INFO("Version: %u", version);

    /* Parse based on version */
    if (version == GGUF_VERSION_1 || version == GGUF_VERSION_2) {
        /* Version 1/2 use 32-bit counts */
        if (g_ctx.size < sizeof(struct gguf_header_v1)) {
            GGUF_ERROR("File too small for v%u header", version);
            return -1;
        }

        const struct gguf_header_v1* hdr = (const struct gguf_header_v1*)ptr;
        g_ctx.n_tensors = hdr->n_tensors;
        g_ctx.n_kv = hdr->n_kv;
        g_ctx.kv_start = ptr + sizeof(struct gguf_header_v1);

    } else if (version == GGUF_VERSION_3) {
        /* Version 3 uses 64-bit counts */
        if (g_ctx.size < sizeof(struct gguf_header_v3)) {
            GGUF_ERROR("File too small for v3 header");
            return -1;
        }

        const struct gguf_header_v3* hdr = (const struct gguf_header_v3*)ptr;
        g_ctx.n_tensors = hdr->n_tensors;
        g_ctx.n_kv = hdr->n_kv;
        g_ctx.kv_start = ptr + sizeof(struct gguf_header_v3);

    } else {
        GGUF_ERROR("Unsupported version: %u", version);
        return -1;
    }

    /* Validate counts */
    if (g_ctx.n_kv > GGUF_MAX_KV_PAIRS) {
        GGUF_ERROR("Too many KV pairs: %llu", (unsigned long long)g_ctx.n_kv);
        return -1;
    }

    if (g_ctx.n_tensors > GGUF_MAX_TENSORS) {
        GGUF_ERROR("Too many tensors: %llu", (unsigned long long)g_ctx.n_tensors);
        return -1;
    }

    GGUF_INFO("Tensors: %llu, KV pairs: %llu",
              (unsigned long long)g_ctx.n_tensors,
              (unsigned long long)g_ctx.n_kv);

    /* Set default alignment */
    g_ctx.alignment = GGUF_DEFAULT_ALIGN;

    return 0;
}

/* ============================================================================
 * Metadata KV Parsing
 * ============================================================================ */

static int gguf_skip_value(const uint8_t** ptr, const uint8_t* end, enum gguf_type type);

static int gguf_skip_array(const uint8_t** ptr, const uint8_t* end)
{
    uint32_t arr_type;
    uint64_t arr_len;

    if (safe_read_u32(ptr, end, &arr_type) < 0) return -1;
    if (safe_read_u64(ptr, end, &arr_len) < 0) return -1;

    if (arr_len > GGUF_MAX_ARRAY_LEN) {
        GGUF_ERROR("Array too long: %llu", (unsigned long long)arr_len);
        return -1;
    }

    /* Skip array elements */
    for (uint64_t i = 0; i < arr_len; i++) {
        if (gguf_skip_value(ptr, end, (enum gguf_type)arr_type) < 0) {
            return -1;
        }
    }

    return 0;
}

static int gguf_skip_value(const uint8_t** ptr, const uint8_t* end, enum gguf_type type)
{
    switch (type) {
        case GGUF_TYPE_UINT8:
        case GGUF_TYPE_INT8:
        case GGUF_TYPE_BOOL:
            if (*ptr + 1 > end) return -1;
            (*ptr)++;
            break;

        case GGUF_TYPE_UINT16:
        case GGUF_TYPE_INT16:
            if (*ptr + 2 > end) return -1;
            (*ptr) += 2;
            break;

        case GGUF_TYPE_UINT32:
        case GGUF_TYPE_INT32:
        case GGUF_TYPE_FLOAT32:
            if (*ptr + 4 > end) return -1;
            (*ptr) += 4;
            break;

        case GGUF_TYPE_UINT64:
        case GGUF_TYPE_INT64:
        case GGUF_TYPE_FLOAT64:
            if (*ptr + 8 > end) return -1;
            (*ptr) += 8;
            break;

        case GGUF_TYPE_STRING:
            if (safe_read_string(ptr, end, NULL, 0, NULL) < 0) return -1;
            break;

        case GGUF_TYPE_ARRAY:
            if (gguf_skip_array(ptr, end) < 0) return -1;
            break;

        default:
            GGUF_ERROR("Unknown type: %u", type);
            return -1;
    }

    return 0;
}

/* Parse a specific KV pair and extract value if it matches a known key */
static int gguf_parse_kv_pair(const uint8_t** ptr, const uint8_t* end, int index)
{
    char key[GGUF_MAX_KEY_LEN];
    uint64_t key_len;

    /* Read key */
    if (safe_read_string(ptr, end, key, sizeof(key), &key_len) < 0) {
        GGUF_ERROR("Failed to read key at index %d", index);
        return -1;
    }

    /* Read value type */
    uint32_t value_type;
    if (safe_read_u32(ptr, end, &value_type) < 0) {
        GGUF_ERROR("Failed to read value type for key '%s'", key);
        return -1;
    }

    GGUF_DEBUG("KV[%d]: '%s' type=%s", index, key, gguf_type_name((enum gguf_type)value_type));

    /* Extract known metadata values */
    struct gguf_model_arch* arch = &g_ctx.arch;

    /* General metadata */
    if (strcmp(key, "general.architecture") == 0 && value_type == GGUF_TYPE_STRING) {
        if (safe_read_string(ptr, end, arch->general_architecture, sizeof(arch->general_architecture), NULL) < 0)
            return -1;
        GGUF_INFO("Architecture: %s", arch->general_architecture);
        return 0;
    }
    if (strcmp(key, "general.name") == 0 && value_type == GGUF_TYPE_STRING) {
        if (safe_read_string(ptr, end, arch->general_name, sizeof(arch->general_name), NULL) < 0)
            return -1;
        GGUF_INFO("Model name: %s", arch->general_name);
        return 0;
    }
    if (strcmp(key, "general.alignment") == 0 && value_type == GGUF_TYPE_UINT32) {
        if (safe_read_u32(ptr, end, &arch->general_alignment) < 0)
            return -1;
        /* Validate alignment is power of 2 and reasonable */
        if (arch->general_alignment == 0 ||
            (arch->general_alignment & (arch->general_alignment - 1)) != 0 ||
            arch->general_alignment > 1024 * 1024) {
            GGUF_ERROR("Invalid alignment: %u (must be power of 2, max 1MB)", arch->general_alignment);
            arch->general_alignment = GGUF_DEFAULT_ALIGN;
        }
        g_ctx.alignment = arch->general_alignment;
        GGUF_DEBUG("Alignment: %u", arch->general_alignment);
        return 0;
    }
    if (strcmp(key, "general.file_type") == 0 && value_type == GGUF_TYPE_UINT32) {
        if (safe_read_u32(ptr, end, &arch->general_file_type) < 0)
            return -1;
        GGUF_DEBUG("File type: %u", arch->general_file_type);
        return 0;
    }
    if (strcmp(key, "general.quantization_version") == 0 && value_type == GGUF_TYPE_UINT32) {
        if (safe_read_u32(ptr, end, &arch->general_quantization_version) < 0)
            return -1;
        GGUF_DEBUG("Quantization version: %u", arch->general_quantization_version);
        return 0;
    }

    /* LLaMA architecture parameters - try both prefixes */
    const char* prefixes[] = {"llama.", "phi.", "mistral.", "qwen.", "gemma.", NULL};

    for (int p = 0; prefixes[p]; p++) {
        size_t plen = strlen(prefixes[p]);
        if (strncmp(key, prefixes[p], plen) != 0) continue;

        const char* subkey = key + plen;

        if (strcmp(subkey, "context_length") == 0 && value_type == GGUF_TYPE_UINT32) {
            if (safe_read_u32(ptr, end, &arch->context_length) < 0) return -1;
            GGUF_INFO("Context length: %u", arch->context_length);
            return 0;
        }
        if (strcmp(subkey, "embedding_length") == 0 && value_type == GGUF_TYPE_UINT32) {
            if (safe_read_u32(ptr, end, &arch->embedding_length) < 0) return -1;
            GGUF_INFO("Embedding length: %u", arch->embedding_length);
            return 0;
        }
        if (strcmp(subkey, "block_count") == 0 && value_type == GGUF_TYPE_UINT32) {
            if (safe_read_u32(ptr, end, &arch->block_count) < 0) return -1;
            GGUF_INFO("Block count: %u", arch->block_count);
            return 0;
        }
        if (strcmp(subkey, "feed_forward_length") == 0 && value_type == GGUF_TYPE_UINT32) {
            if (safe_read_u32(ptr, end, &arch->feed_forward_length) < 0) return -1;
            GGUF_INFO("Feed forward length: %u", arch->feed_forward_length);
            return 0;
        }
        if (strcmp(subkey, "attention.head_count") == 0 && value_type == GGUF_TYPE_UINT32) {
            if (safe_read_u32(ptr, end, &arch->attention_head_count) < 0) return -1;
            GGUF_INFO("Attention heads: %u", arch->attention_head_count);
            return 0;
        }
        if (strcmp(subkey, "attention.head_count_kv") == 0 && value_type == GGUF_TYPE_UINT32) {
            if (safe_read_u32(ptr, end, &arch->attention_head_count_kv) < 0) return -1;
            GGUF_INFO("KV heads: %u", arch->attention_head_count_kv);
            return 0;
        }
        if (strcmp(subkey, "attention.layer_norm_rms_epsilon") == 0 && value_type == GGUF_TYPE_FLOAT32) {
            if (safe_read_f32(ptr, end, &arch->attention_layer_norm_rms_epsilon) < 0) return -1;
            GGUF_DEBUG("RMS epsilon: %f", arch->attention_layer_norm_rms_epsilon);
            return 0;
        }
        if (strcmp(subkey, "rope.freq_base") == 0 && value_type == GGUF_TYPE_FLOAT32) {
            if (safe_read_f32(ptr, end, &arch->rope_freq_base) < 0) return -1;
            GGUF_DEBUG("RoPE freq base: %f", arch->rope_freq_base);
            return 0;
        }
        if (strcmp(subkey, "rope.dimension_count") == 0 && value_type == GGUF_TYPE_UINT32) {
            if (safe_read_u32(ptr, end, &arch->rope_dimension_count) < 0) return -1;
            GGUF_DEBUG("RoPE dimensions: %u", arch->rope_dimension_count);
            return 0;
        }
    }

    /* Tokenizer metadata */
    if (strcmp(key, "tokenizer.ggml.model") == 0 && value_type == GGUF_TYPE_STRING) {
        if (safe_read_string(ptr, end, arch->tokenizer_model, sizeof(arch->tokenizer_model), NULL) < 0)
            return -1;
        GGUF_INFO("Tokenizer model: %s", arch->tokenizer_model);
        return 0;
    }
    if (strcmp(key, "tokenizer.ggml.bos_token_id") == 0 && value_type == GGUF_TYPE_UINT32) {
        if (safe_read_u32(ptr, end, &arch->bos_token_id) < 0) return -1;
        GGUF_DEBUG("BOS token ID: %u", arch->bos_token_id);
        return 0;
    }
    if (strcmp(key, "tokenizer.ggml.eos_token_id") == 0 && value_type == GGUF_TYPE_UINT32) {
        if (safe_read_u32(ptr, end, &arch->eos_token_id) < 0) return -1;
        GGUF_DEBUG("EOS token ID: %u", arch->eos_token_id);
        return 0;
    }
    if (strcmp(key, "tokenizer.ggml.padding_token_id") == 0 && value_type == GGUF_TYPE_UINT32) {
        if (safe_read_u32(ptr, end, &arch->pad_token_id) < 0) return -1;
        GGUF_DEBUG("PAD token ID: %u", arch->pad_token_id);
        return 0;
    }

    /* Vocabulary tokens - string array */
    if (strcmp(key, "tokenizer.ggml.tokens") == 0 && value_type == GGUF_TYPE_ARRAY) {
        uint32_t arr_type;
        uint64_t arr_len;

        if (safe_read_u32(ptr, end, &arr_type) < 0) return -1;
        if (safe_read_u64(ptr, end, &arr_len) < 0) return -1;

        if (arr_type != GGUF_TYPE_STRING) {
            GGUF_ERROR("Tokens array has wrong type: %u", arr_type);
            /* Skip the array */
            for (uint64_t i = 0; i < arr_len; i++) {
                gguf_skip_value(ptr, end, (enum gguf_type)arr_type);
            }
            return 0;
        }

        if (arr_len > GGUF_MAX_VOCAB_SIZE) {
            GGUF_ERROR("Vocab too large: %llu", (unsigned long long)arr_len);
            arr_len = GGUF_MAX_VOCAB_SIZE;
        }

        arch->vocab_size = (uint32_t)arr_len;
        g_ctx.vocab_count = (uint32_t)arr_len;
        GGUF_INFO("Vocabulary size: %u tokens", g_ctx.vocab_count);

        /* Allocate vocabulary storage */
        g_ctx.vocab = (struct gguf_vocab_token*)kmalloc(arr_len * sizeof(struct gguf_vocab_token));
        if (!g_ctx.vocab) {
            GGUF_ERROR("Failed to allocate vocabulary");
            /* Skip tokens */
            for (uint64_t i = 0; i < arr_len; i++) {
                safe_read_string(ptr, end, NULL, 0, NULL);
            }
            return 0;
        }

        memset(g_ctx.vocab, 0, arr_len * sizeof(struct gguf_vocab_token));

        /* Read token strings */
        for (uint64_t i = 0; i < arr_len; i++) {
            uint64_t str_len;
            if (*ptr + 8 > end) break;

            memcpy(&str_len, *ptr, 8);  /* Use memcpy for safe unaligned access */
            (*ptr) += 8;

            if (str_len > 1024 || *ptr + str_len > end) {
                (*ptr) += (str_len <= 1024 && *ptr + str_len <= end) ? str_len : 0;
                continue;
            }

            /* Allocate and copy token text */
            g_ctx.vocab[i].text = (char*)kmalloc(str_len + 1);
            if (g_ctx.vocab[i].text) {
                memcpy(g_ctx.vocab[i].text, *ptr, str_len);
                g_ctx.vocab[i].text[str_len] = '\0';
            }

            (*ptr) += str_len;
        }

        GGUF_DEBUG("Loaded %u vocabulary tokens", g_ctx.vocab_count);
        return 0;
    }

    /* Vocabulary scores - float array */
    if (strcmp(key, "tokenizer.ggml.scores") == 0 && value_type == GGUF_TYPE_ARRAY) {
        uint32_t arr_type;
        uint64_t arr_len;

        if (safe_read_u32(ptr, end, &arr_type) < 0) return -1;
        if (safe_read_u64(ptr, end, &arr_len) < 0) return -1;

        if (arr_type == GGUF_TYPE_FLOAT32 && arr_len <= GGUF_MAX_VOCAB_SIZE) {
            g_ctx.vocab_scores = (float*)kmalloc(arr_len * sizeof(float));
            if (g_ctx.vocab_scores) {
                size_t bytes = arr_len * sizeof(float);
                if (*ptr + bytes <= end) {
                    memcpy(g_ctx.vocab_scores, *ptr, bytes);
                    GGUF_DEBUG("Loaded %llu vocab scores", (unsigned long long)arr_len);
                }
            }
            (*ptr) += arr_len * sizeof(float);
        } else {
            /* Skip */
            for (uint64_t i = 0; i < arr_len; i++) {
                gguf_skip_value(ptr, end, (enum gguf_type)arr_type);
            }
        }
        return 0;
    }

    /* Vocabulary token types - int32 array */
    if (strcmp(key, "tokenizer.ggml.token_type") == 0 && value_type == GGUF_TYPE_ARRAY) {
        uint32_t arr_type;
        uint64_t arr_len;

        if (safe_read_u32(ptr, end, &arr_type) < 0) return -1;
        if (safe_read_u64(ptr, end, &arr_len) < 0) return -1;

        if (arr_type == GGUF_TYPE_INT32 && arr_len <= GGUF_MAX_VOCAB_SIZE) {
            g_ctx.vocab_types = (uint32_t*)kmalloc(arr_len * sizeof(uint32_t));
            if (g_ctx.vocab_types) {
                size_t bytes = arr_len * sizeof(uint32_t);
                if (*ptr + bytes <= end) {
                    memcpy(g_ctx.vocab_types, *ptr, bytes);
                    GGUF_DEBUG("Loaded %llu token types", (unsigned long long)arr_len);
                }
            }
            (*ptr) += arr_len * sizeof(uint32_t);
        } else {
            /* Skip */
            for (uint64_t i = 0; i < arr_len; i++) {
                gguf_skip_value(ptr, end, (enum gguf_type)arr_type);
            }
        }
        return 0;
    }

    /* Unknown key - skip the value */
    if (gguf_skip_value(ptr, end, (enum gguf_type)value_type) < 0) {
        GGUF_ERROR("Failed to skip value for key '%s'", key);
        return -1;
    }

    return 0;
}

static int gguf_parse_metadata(void)
{
    const uint8_t* ptr = g_ctx.kv_start;
    const uint8_t* end = g_ctx.data + g_ctx.size;

    GGUF_INFO("Parsing %llu metadata entries...", (unsigned long long)g_ctx.n_kv);

    /* Initialize arch with defaults */
    memset(&g_ctx.arch, 0, sizeof(g_ctx.arch));
    g_ctx.arch.attention_layer_norm_rms_epsilon = 1e-5f;
    g_ctx.arch.rope_freq_base = 10000.0f;

    for (uint64_t i = 0; i < g_ctx.n_kv; i++) {
        if (gguf_parse_kv_pair(&ptr, end, (int)i) < 0) {
            GGUF_ERROR("Failed at KV pair %llu", (unsigned long long)i);
            return -1;
        }
    }

    g_ctx.tensor_info_start = ptr;
    GGUF_DEBUG("Metadata parsing complete, tensor info starts at offset %zu",
               (size_t)(ptr - g_ctx.data));

    return 0;
}

/* ============================================================================
 * Tensor Info Parsing
 * ============================================================================ */

/* Calculate tensor size in bytes based on type and element count */
static size_t calc_tensor_size(ggml_type_t type, uint64_t n_elements)
{
    size_t block_size = ggml_type_block_size(type);
    size_t block_elems = ggml_type_block_elements(type);

    if (block_elems == 0) return 0;

    size_t n_blocks = (n_elements + block_elems - 1) / block_elems;
    return n_blocks * block_size;
}

static int gguf_parse_tensor_info(void)
{
    const uint8_t* ptr = g_ctx.tensor_info_start;
    const uint8_t* end = g_ctx.data + g_ctx.size;

    GGUF_INFO("Parsing %llu tensor entries...", (unsigned long long)g_ctx.n_tensors);

    /* Allocate tensor info storage */
    uint64_t n_to_store = g_ctx.n_tensors;
    if (n_to_store > GGUF_MAX_STORED_TENSORS) {
        GGUF_DEBUG("Capping tensor storage at %d (model has %llu)",
                   GGUF_MAX_STORED_TENSORS, (unsigned long long)g_ctx.n_tensors);
        n_to_store = GGUF_MAX_STORED_TENSORS;
    }

    g_ctx.tensors = (struct gguf_tensor_info*)kmalloc(n_to_store * sizeof(struct gguf_tensor_info));
    if (!g_ctx.tensors) {
        GGUF_ERROR("Failed to allocate tensor info storage");
        return -1;
    }
    memset(g_ctx.tensors, 0, n_to_store * sizeof(struct gguf_tensor_info));
    g_ctx.tensor_count = 0;

    /* Reset type counters */
    memset(g_ctx.type_counts, 0, sizeof(g_ctx.type_counts));

    for (uint64_t i = 0; i < g_ctx.n_tensors; i++) {
        /* Read tensor name */
        char name[GGUF_MAX_TENSOR_NAME];
        if (safe_read_string(&ptr, end, name, sizeof(name), NULL) < 0) {
            GGUF_ERROR("Failed to read tensor %llu name", (unsigned long long)i);
            return -1;
        }

        /* Read dimensions */
        uint32_t n_dims;
        if (safe_read_u32(&ptr, end, &n_dims) < 0) return -1;

        if (n_dims > GGUF_MAX_TENSOR_DIMS) {
            GGUF_ERROR("Too many dimensions: %u", n_dims);
            return -1;
        }

        uint64_t dims[GGUF_MAX_TENSOR_DIMS] = {0};
        for (uint32_t d = 0; d < n_dims; d++) {
            if (safe_read_u64(&ptr, end, &dims[d]) < 0) return -1;
        }

        /* Read type and offset */
        uint32_t type;
        uint64_t offset;
        if (safe_read_u32(&ptr, end, &type) < 0) return -1;
        if (safe_read_u64(&ptr, end, &offset) < 0) return -1;

        /* Calculate total elements */
        uint64_t n_elements = 1;
        for (uint32_t d = 0; d < n_dims; d++) {
            n_elements *= dims[d];
        }

        /* Store tensor info if within limit */
        if (i < n_to_store) {
            struct gguf_tensor_info* ti = &g_ctx.tensors[i];
            strncpy(ti->name, name, GGUF_MAX_TENSOR_NAME - 1);
            ti->name[GGUF_MAX_TENSOR_NAME - 1] = '\0';
            ti->n_dims = n_dims;
            for (uint32_t d = 0; d < GGUF_MAX_TENSOR_DIMS; d++) {
                ti->dims[d] = dims[d];
            }
            ti->type = (ggml_type_t)type;
            ti->offset = offset;
            ti->size = calc_tensor_size((ggml_type_t)type, n_elements);
            g_ctx.tensor_count++;
        }

        /* Count quantization types (for detecting model quant type) */
        if (type < GGML_TYPE_COUNT) {
            g_ctx.type_counts[type]++;
        }

        /* Log first few tensors */
        if (i < 5 || i == g_ctx.n_tensors - 1) {
            GGUF_DEBUG("Tensor[%llu]: %s [%llux%llux%llux%llu] type=%s offset=%llu",
                      (unsigned long long)i, name,
                      (unsigned long long)dims[0], (unsigned long long)dims[1],
                      (unsigned long long)dims[2], (unsigned long long)dims[3],
                      ggml_type_name((ggml_type_t)type),
                      (unsigned long long)offset);
        }
    }

    /* Calculate aligned tensor data start */
    size_t metadata_size = (size_t)(ptr - g_ctx.data);
    size_t aligned = (metadata_size + g_ctx.alignment - 1) & ~(g_ctx.alignment - 1);
    g_ctx.tensor_data_start = g_ctx.data + aligned;

    /* Determine predominant quantization type (exclude F32/F16 for this) */
    uint32_t max_count = 0;
    g_ctx.predominant_type = GGML_TYPE_F16;  /* Default */

    for (int t = GGML_TYPE_Q4_0; t < GGML_TYPE_COUNT; t++) {
        if (g_ctx.type_counts[t] > max_count) {
            max_count = g_ctx.type_counts[t];
            g_ctx.predominant_type = (ggml_type_t)t;
        }
    }

    GGUF_INFO("Tensor data starts at offset %zu (aligned from %zu)",
              aligned, metadata_size);
    GGUF_INFO("Predominant quantization: %s (%u tensors)",
              ggml_type_name(g_ctx.predominant_type), max_count);

    return 0;
}

/* ============================================================================
 * Validation
 * ============================================================================ */

static int gguf_validate(void)
{
    struct gguf_model_arch* arch = &g_ctx.arch;

    GGUF_INFO("Validating model metadata...");

    /* Check required parameters */
    if (arch->embedding_length == 0) {
        GGUF_ERROR("Missing embedding_length");
        return -1;
    }

    if (arch->block_count == 0) {
        GGUF_ERROR("Missing block_count");
        return -1;
    }

    if (arch->attention_head_count == 0) {
        GGUF_ERROR("Missing attention_head_count");
        return -1;
    }

    /* Set defaults for optional parameters */
    if (arch->attention_head_count_kv == 0) {
        arch->attention_head_count_kv = arch->attention_head_count;
        GGUF_DEBUG("Using head_count_kv = head_count = %u", arch->attention_head_count_kv);
    }

    if (arch->feed_forward_length == 0) {
        /* Estimate: typically 4x embedding for LLaMA */
        arch->feed_forward_length = arch->embedding_length * 4;
        GGUF_DEBUG("Estimated feed_forward_length = %u", arch->feed_forward_length);
    }

    if (arch->context_length == 0) {
        arch->context_length = 2048;  /* Default context */
        GGUF_DEBUG("Using default context_length = %u", arch->context_length);
    }

    if (arch->vocab_size == 0 && g_ctx.vocab_count > 0) {
        arch->vocab_size = g_ctx.vocab_count;
    }

    /* Validate tensor data region */
    if (g_ctx.tensor_data_start >= g_ctx.data + g_ctx.size) {
        GGUF_ERROR("Tensor data offset beyond file size");
        return -1;
    }

    g_ctx.is_valid = 1;

    GGUF_INFO("Validation passed:");
    GGUF_INFO("  Architecture: %s", arch->general_architecture[0] ? arch->general_architecture : "unknown");
    GGUF_INFO("  Embedding: %u, Layers: %u, Heads: %u/%u",
              arch->embedding_length, arch->block_count,
              arch->attention_head_count, arch->attention_head_count_kv);
    GGUF_INFO("  Vocab: %u, Context: %u", arch->vocab_size, arch->context_length);

    return 0;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Enable or disable debug logging
 */
void gguf_parser_set_debug(int enabled)
{
    g_ctx.debug_enabled = enabled ? 1 : 0;
}

/**
 * Parse GGUF file from memory buffer
 * Returns 0 on success, -1 on error
 */
int gguf_parser_load(const void* data, size_t size)
{
    GGUF_INFO("Loading GGUF file (%zu bytes, %.2f MB)", size, (float)size / (1024*1024));

    /* Reset context */
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.data = (const uint8_t*)data;
    g_ctx.size = size;
    g_ctx.debug_enabled = 1;  /* Enable debug by default */

    /* Parse header */
    if (gguf_parse_header() < 0) {
        goto parse_failed;
    }

    /* Parse metadata */
    if (gguf_parse_metadata() < 0) {
        goto parse_failed;
    }

    /* Parse tensor info */
    if (gguf_parse_tensor_info() < 0) {
        goto parse_failed;
    }

    /* Validate */
    if (gguf_validate() < 0) {
        goto parse_failed;
    }

    GGUF_INFO("GGUF file loaded successfully");
    return 0;

parse_failed:
    /* Clean up any allocated resources on failure */
    gguf_parser_free();
    return -1;
}

/**
 * Get parsed model architecture
 */
const struct gguf_model_arch* gguf_parser_get_arch(void)
{
    return g_ctx.is_valid ? &g_ctx.arch : NULL;
}

/**
 * Get GGUF version
 */
uint32_t gguf_parser_get_version(void)
{
    return g_ctx.version;
}

/**
 * Get vocabulary token by index
 */
const char* gguf_parser_get_token(uint32_t index)
{
    if (!g_ctx.vocab || index >= g_ctx.vocab_count) {
        return NULL;
    }
    return g_ctx.vocab[index].text;
}

/**
 * Get vocabulary size
 */
uint32_t gguf_parser_get_vocab_size(void)
{
    return g_ctx.vocab_count;
}

/**
 * Get token score
 */
float gguf_parser_get_token_score(uint32_t index)
{
    if (!g_ctx.vocab_scores || index >= g_ctx.vocab_count) {
        return 0.0f;
    }
    return g_ctx.vocab_scores[index];
}

/**
 * Get tensor data start pointer
 */
const void* gguf_parser_get_tensor_data(void)
{
    return g_ctx.tensor_data_start;
}

/**
 * Get data alignment
 */
size_t gguf_parser_get_alignment(void)
{
    return g_ctx.alignment;
}

/**
 * Free parser resources
 */
void gguf_parser_free(void)
{
    if (g_ctx.vocab) {
        for (uint32_t i = 0; i < g_ctx.vocab_count; i++) {
            if (g_ctx.vocab[i].text) {
                kfree(g_ctx.vocab[i].text);
            }
        }
        kfree(g_ctx.vocab);
    }

    if (g_ctx.vocab_scores) {
        kfree(g_ctx.vocab_scores);
    }

    if (g_ctx.vocab_types) {
        kfree(g_ctx.vocab_types);
    }

    if (g_ctx.tensors) {
        kfree(g_ctx.tensors);
    }

    memset(&g_ctx, 0, sizeof(g_ctx));
    GGUF_INFO("Parser resources freed");
}

/**
 * Print model summary
 */
void gguf_parser_print_summary(void)
{
    if (!g_ctx.is_valid) {
        console_printf("GGUF: No valid model loaded\n");
        return;
    }

    const struct gguf_model_arch* arch = &g_ctx.arch;

    console_printf("\n=== GGUF Model Summary ===\n");
    console_printf("Version: %u\n", g_ctx.version);
    console_printf("Architecture: %s\n", arch->general_architecture[0] ? arch->general_architecture : "unknown");
    console_printf("Name: %s\n", arch->general_name[0] ? arch->general_name : "unknown");
    console_printf("Tokenizer: %s\n", arch->tokenizer_model[0] ? arch->tokenizer_model : "unknown");
    console_printf("\nModel Parameters:\n");
    console_printf("  Embedding dimension: %u\n", arch->embedding_length);
    console_printf("  Number of layers: %u\n", arch->block_count);
    console_printf("  Attention heads: %u (KV: %u)\n", arch->attention_head_count, arch->attention_head_count_kv);
    console_printf("  Feed-forward dimension: %u\n", arch->feed_forward_length);
    console_printf("  Context length: %u\n", arch->context_length);
    console_printf("  Vocabulary size: %u\n", arch->vocab_size);
    console_printf("\nRoPE Parameters:\n");
    console_printf("  Dimensions: %u\n", arch->rope_dimension_count);
    console_printf("  Frequency base: %.1f\n", arch->rope_freq_base);
    console_printf("\nSpecial Tokens:\n");
    console_printf("  BOS: %u, EOS: %u, PAD: %u\n", arch->bos_token_id, arch->eos_token_id, arch->pad_token_id);
    console_printf("\nTensors: %llu\n", (unsigned long long)g_ctx.n_tensors);
    console_printf("Tensor data offset: %zu\n", (size_t)(g_ctx.tensor_data_start - g_ctx.data));
    console_printf("Alignment: %zu bytes\n", g_ctx.alignment);
    console_printf("Quantization: %s\n", ggml_type_name(g_ctx.predominant_type));
    console_printf("==========================\n\n");
}

/* ============================================================================
 * Tensor Info API
 * ============================================================================ */

/**
 * Get number of tensors
 */
uint64_t gguf_parser_get_tensor_count(void)
{
    return g_ctx.tensor_count;
}

/**
 * Get tensor info by index
 */
const struct gguf_tensor_info* gguf_parser_get_tensor_by_index(uint64_t index)
{
    if (!g_ctx.tensors || index >= g_ctx.tensor_count) {
        return NULL;
    }
    return &g_ctx.tensors[index];
}

/**
 * Get tensor info by name
 */
const struct gguf_tensor_info* gguf_parser_get_tensor_by_name(const char* name)
{
    if (!g_ctx.tensors || !name) {
        return NULL;
    }

    for (uint64_t i = 0; i < g_ctx.tensor_count; i++) {
        if (strcmp(g_ctx.tensors[i].name, name) == 0) {
            return &g_ctx.tensors[i];
        }
    }

    return NULL;
}

/**
 * Get pointer to tensor data
 */
const void* gguf_parser_get_tensor_data_ptr(const struct gguf_tensor_info* info)
{
    if (!info || !g_ctx.tensor_data_start) {
        return NULL;
    }

    return g_ctx.tensor_data_start + info->offset;
}

/**
 * Get the predominant quantization type in the model
 */
ggml_type_t gguf_parser_get_model_quant_type(void)
{
    return g_ctx.predominant_type;
}

/* ============================================================================
 * Block Device Loading
 * ============================================================================ */

/* Static buffer for model data loaded from block device */
static uint8_t* g_model_buffer = NULL;
static size_t g_model_buffer_size = 0;

/**
 * Load GGUF model from block device
 *
 * @param dev       Block device to read from
 * @param offset    Byte offset into device (usually 0)
 * @param size      Size of model in bytes (0 = auto-detect from device size)
 *
 * @return 0 on success, negative error on failure
 */
int gguf_load_from_block(block_device_t* dev, uint64_t offset, size_t size)
{
    if (!dev) {
        console_printf("[GGUF] Error: No block device specified\n");
        return -1;
    }

    /* Calculate size if not specified */
    uint64_t dev_capacity = block_capacity(dev);
    if (size == 0) {
        if (offset >= dev_capacity) {
            console_printf("[GGUF] Error: Offset beyond device capacity\n");
            return -1;
        }
        size = (size_t)(dev_capacity - offset);
    }

    console_printf("[GGUF] Loading model from %s (offset=%llu, size=%zu MB)\n",
                   dev->name, offset, size / (1024 * 1024));

    /* Free any previous buffer */
    if (g_model_buffer) {
        heap_free(g_model_buffer);
        g_model_buffer = NULL;
        g_model_buffer_size = 0;
    }

    /* Allocate buffer for model data */
    g_model_buffer = (uint8_t*)heap_alloc(size);
    if (!g_model_buffer) {
        console_printf("[GGUF] Error: Failed to allocate %zu MB for model\n",
                       size / (1024 * 1024));
        return -1;
    }
    g_model_buffer_size = size;

    console_printf("[GGUF] Allocated %zu MB buffer at %p\n",
                   size / (1024 * 1024), g_model_buffer);

    /* Read data in chunks */
    size_t chunk_size = 64 * 1024;  /* 64KB chunks */
    size_t bytes_read = 0;
    uint64_t current_offset = offset;

    while (bytes_read < size) {
        size_t to_read = size - bytes_read;
        if (to_read > chunk_size) {
            to_read = chunk_size;
        }

        /* Calculate sectors */
        uint64_t start_sector = current_offset / BLOCK_SECTOR_SIZE;
        uint32_t num_sectors = (to_read + BLOCK_SECTOR_SIZE - 1) / BLOCK_SECTOR_SIZE;

        /* Read sectors */
        int ret = block_read(dev, start_sector, num_sectors,
                             g_model_buffer + bytes_read);
        if (ret != BLOCK_OK) {
            console_printf("[GGUF] Error: Block read failed at offset %zu\n",
                           bytes_read);
            heap_free(g_model_buffer);
            g_model_buffer = NULL;
            g_model_buffer_size = 0;
            return -1;
        }

        bytes_read += num_sectors * BLOCK_SECTOR_SIZE;
        current_offset += num_sectors * BLOCK_SECTOR_SIZE;

        /* Progress indicator every 10MB */
        if ((bytes_read % (10 * 1024 * 1024)) == 0) {
            console_printf("[GGUF] Read %zu / %zu MB...\n",
                           bytes_read / (1024 * 1024), size / (1024 * 1024));
        }
    }

    console_printf("[GGUF] Read complete, parsing GGUF...\n");

    /* Parse the loaded data */
    int ret = gguf_parser_load(g_model_buffer, size);
    if (ret < 0) {
        console_printf("[GGUF] Error: Failed to parse GGUF data\n");
        heap_free(g_model_buffer);
        g_model_buffer = NULL;
        g_model_buffer_size = 0;
        return -1;
    }

    console_printf("[GGUF] Model loaded successfully from %s\n", dev->name);
    return 0;
}

/**
 * Free model data loaded from block device
 */
void gguf_free_block_buffer(void)
{
    if (g_model_buffer) {
        heap_free(g_model_buffer);
        g_model_buffer = NULL;
        g_model_buffer_size = 0;
    }
}
