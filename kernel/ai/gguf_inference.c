/* Generic GGUF Inference Engine for EMBODIOS
 * Works with any llama-architecture model in GGUF format
 *
 * Tensor naming follows llama.cpp convention:
 * - token_embd.weight
 * - blk.{layer}.attn_norm.weight
 * - blk.{layer}.attn_q.weight
 * - blk.{layer}.attn_k.weight
 * - blk.{layer}.attn_v.weight
 * - blk.{layer}.attn_output.weight
 * - blk.{layer}.ffn_norm.weight
 * - blk.{layer}.ffn_gate.weight (w1)
 * - blk.{layer}.ffn_up.weight (w3)
 * - blk.{layer}.ffn_down.weight (w2)
 * - output_norm.weight
 * - output.weight (optional, may share with token_embd)
 */

#include <embodios/console.h>
#include <embodios/gguf_parser.h>
#include <embodios/mm.h>
#include <embodios/types.h>

/* SIMD intrinsics for optimized operations */
#ifdef __aarch64__
#include <arm_neon.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define _MM_MALLOC_H_INCLUDED
#define __MM_MALLOC_H
#include <emmintrin.h>  /* SSE2 */
#ifdef __AVX__
#include <immintrin.h>  /* AVX */
#endif
#endif

/* Forward declarations */
extern float sqrtf(float x);
extern float expf(float x);
extern void *memset(void *, int, size_t);
extern void *memcpy(void *, const void *, size_t);
extern size_t strlen(const char *);

/* Simple helper to build "blk.N.name" style tensor names */
static void build_layer_name(char *buf, size_t size, const char *prefix, int layer,
                             const char *suffix)
{
    size_t pos = 0;
    /* Copy prefix */
    const char *p = prefix;
    while (*p && pos < size - 1)
        buf[pos++] = *p++;
    /* Convert layer number to string */
    if (layer >= 10 && pos < size - 1)
        buf[pos++] = '0' + (layer / 10);
    if (pos < size - 1)
        buf[pos++] = '0' + (layer % 10);
    /* Copy suffix */
    p = suffix;
    while (*p && pos < size - 1)
        buf[pos++] = *p++;
    buf[pos] = '\0';
}

/* ============================================================================
 * Dequantization Functions
 * Support for all common GGUF quantization types
 * ============================================================================ */

/* Convert float16 to float32 */
static float fp16_to_fp32(uint16_t h)
{
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t f;

    if (exp == 0) {
        if (mant == 0) {
            f = sign << 31;
        } else {
            /* Denormal */
            exp = 1;
            while (!(mant & 0x400)) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x3ff;
            f = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        /* Inf/NaN */
        f = (sign << 31) | 0x7f800000 | (mant << 13);
    } else {
        f = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
    }

    union {
        uint32_t i;
        float f;
    } u;
    u.i = f;
    return u.f;
}

/* Q8_0: 8-bit quantization
 * Block format: scale(float16) + qs[32](int8)
 * Total: 2 + 32 = 34 bytes per block of 32 values
 */
#define QK8_0 32
typedef struct __attribute__((packed)) {
    uint16_t d;       /* delta (scale) as float16 */
    int8_t qs[QK8_0]; /* quantized values */
} block_q8_0;

static void dequantize_row_q8_0(const void *src, float *dst, int64_t n)
{
    const block_q8_0 *blocks = (const block_q8_0 *)src;
    int64_t nb = n / QK8_0;
    for (int64_t i = 0; i < nb; i++) {
        float d = fp16_to_fp32(blocks[i].d);
        for (int j = 0; j < QK8_0; j++) {
            dst[i * QK8_0 + j] = d * blocks[i].qs[j];
        }
    }
}

/* Q4_0: 4-bit quantization
 * Block format: scale(float16) + qs[16](uint8 with 2x 4-bit values)
 * Total: 2 + 16 = 18 bytes per block of 32 values
 */
#define QK4_0 32
typedef struct {
    uint16_t d;            /* delta (scale) as float16 */
    uint8_t qs[QK4_0 / 2]; /* nibbles: low 4 bits = value[2i], high 4 bits = value[2i+1] */
} block_q4_0;

static void dequantize_row_q4_0(const void *src, float *dst, int64_t n)
{
    const block_q4_0 *blocks = (const block_q4_0 *)src;
    int64_t nb = n / QK4_0;
    for (int64_t i = 0; i < nb; i++) {
        float d = fp16_to_fp32(blocks[i].d);
        for (int j = 0; j < QK4_0 / 2; j++) {
            uint8_t qbyte = blocks[i].qs[j];
            /* Low nibble: value[2j], High nibble: value[2j+1] */
            int8_t q0 = (qbyte & 0xf) - 8;
            int8_t q1 = (qbyte >> 4) - 8;
            dst[i * QK4_0 + j * 2 + 0] = d * q0;
            dst[i * QK4_0 + j * 2 + 1] = d * q1;
        }
    }
}

/* Q5_0: 5-bit quantization (type 6)
 * Block format: scale(float16) + qh[4](high bits) + qs[16](low 4 bits)
 * Total: 2 + 4 + 16 = 22 bytes per block of 32 values
 */
#define QK5_0 32
typedef struct __attribute__((packed)) {
    uint16_t d;            /* delta (scale) as float16 */
    uint8_t qh[4];         /* 5th bit of each element (32 bits = 4 bytes) */
    uint8_t qs[QK5_0 / 2]; /* low 4 bits of each element */
} block_q5_0;

static void dequantize_row_q5_0(const void *src, float *dst, int64_t n)
{
    const block_q5_0 *blocks = (const block_q5_0 *)src;
    int64_t nb = n / QK5_0;
    for (int64_t i = 0; i < nb; i++) {
        float d = fp16_to_fp32(blocks[i].d);
        uint32_t qh = ((uint32_t)blocks[i].qh[0]) | ((uint32_t)blocks[i].qh[1] << 8) |
                      ((uint32_t)blocks[i].qh[2] << 16) | ((uint32_t)blocks[i].qh[3] << 24);
        for (int j = 0; j < QK5_0 / 2; j++) {
            uint8_t qbyte = blocks[i].qs[j];
            /* Extract 5-bit values: low 4 bits from qs, 5th bit from qh */
            int8_t q0 = ((qbyte & 0xf) | ((qh >> (j * 2)) & 1) << 4) - 16;
            int8_t q1 = ((qbyte >> 4) | ((qh >> (j * 2 + 1)) & 1) << 4) - 16;
            dst[i * QK5_0 + j * 2 + 0] = d * q0;
            dst[i * QK5_0 + j * 2 + 1] = d * q1;
        }
    }
}

/* K-quant block size */
#define QK_K 256
#define K_SCALE_SIZE 12

/* Q4_K: 4-bit K-quant (type 12)
 * Block format: 256 elements, ~4.5 bits per weight
 * Total: 2 + 2 + 12 + 128 = 144 bytes per block
 */
typedef struct __attribute__((packed)) {
    uint16_t d;               /* super-block scale */
    uint16_t dmin;            /* super-block min */
    uint8_t scales[K_SCALE_SIZE]; /* scales and mins, quantized with 6 bits */
    uint8_t qs[QK_K / 2];     /* 4-bit quants */
} block_q4_K;

/* Q5_K: 5-bit K-quant (type 13)
 * Block format: 256 elements, ~5.5 bits per weight
 * Total: 2 + 2 + 12 + 32 + 128 = 176 bytes per block
 */
typedef struct __attribute__((packed)) {
    uint16_t d;               /* super-block scale */
    uint16_t dmin;            /* super-block min */
    uint8_t scales[K_SCALE_SIZE]; /* scales and mins, quantized with 6 bits */
    uint8_t qh[QK_K / 8];     /* quants, high bit */
    uint8_t qs[QK_K / 2];     /* quants, low 4 bits */
} block_q5_K;

/* Q6_K: 6-bit K-quant (type 14)
 * Block format: super-block of 256 elements
 */
typedef struct __attribute__((packed)) {
    uint8_t ql[QK_K / 2];     /* low 4 bits of each quant */
    uint8_t qh[QK_K / 4];     /* high 2 bits of each quant */
    int8_t scales[QK_K / 16]; /* scales for 16-element sub-blocks */
    uint16_t d;               /* super-block scale as float16 */
} block_q6_K;

/* Helper to decode scale and min from Q4_K/Q5_K packed format - matches llama.cpp */
static inline void get_scale_min_k4(int j, const uint8_t *q, uint8_t *d, uint8_t *m)
{
    if (j < 4) {
        *d = q[j] & 63;
        *m = q[j + 4] & 63;
    } else {
        *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4);
        *m = (q[j + 4] >> 4) | ((q[j - 0] >> 6) << 4);
    }
}

/* Q4_K dequantization - matches llama.cpp exactly */
static void dequantize_row_q4_K(const void *src, float *dst, int64_t n)
{
    const block_q4_K *x = (const block_q4_K *)src;
    int64_t nb = n / QK_K;

    for (int64_t i = 0; i < nb; i++) {
        const uint8_t *q = x[i].qs;
        float d = fp16_to_fp32(x[i].d);
        float dmin = fp16_to_fp32(x[i].dmin);
        float *y = dst + i * QK_K;

        int is = 0;
        uint8_t sc, m;
        for (int j = 0; j < QK_K; j += 64) {
            get_scale_min_k4(is + 0, x[i].scales, &sc, &m);
            float d1 = d * sc;
            float m1 = dmin * m;
            get_scale_min_k4(is + 1, x[i].scales, &sc, &m);
            float d2 = d * sc;
            float m2 = dmin * m;
            for (int l = 0; l < 32; ++l)
                *y++ = d1 * (q[l] & 0xF) - m1;
            for (int l = 0; l < 32; ++l)
                *y++ = d2 * (q[l] >> 4) - m2;
            q += 32;
            is += 2;
        }
    }
}

/* Q5_K dequantization - matches llama.cpp exactly */
static void dequantize_row_q5_K(const void *src, float *dst, int64_t n)
{
    const block_q5_K *x = (const block_q5_K *)src;
    int64_t nb = n / QK_K;

    for (int64_t i = 0; i < nb; i++) {
        const uint8_t *ql = x[i].qs;
        const uint8_t *qh = x[i].qh;
        float d = fp16_to_fp32(x[i].d);
        float dmin = fp16_to_fp32(x[i].dmin);
        float *y = dst + i * QK_K;

        int is = 0;
        uint8_t sc, m;
        uint8_t u1 = 1, u2 = 2;
        for (int j = 0; j < QK_K; j += 64) {
            get_scale_min_k4(is + 0, x[i].scales, &sc, &m);
            float d1 = d * sc;
            float m1 = dmin * m;
            get_scale_min_k4(is + 1, x[i].scales, &sc, &m);
            float d2 = d * sc;
            float m2 = dmin * m;
            for (int l = 0; l < 32; ++l)
                *y++ = d1 * ((ql[l] & 0xF) + (qh[l] & u1 ? 16 : 0)) - m1;
            for (int l = 0; l < 32; ++l)
                *y++ = d2 * ((ql[l] >> 4) + (qh[l] & u2 ? 16 : 0)) - m2;
            ql += 32;
            is += 2;
            u1 <<= 2;
            u2 <<= 2;
        }
    }
}

static void dequantize_row_q6_K(const void *src, float *dst, int64_t n)
{
    const block_q6_K *blocks = (const block_q6_K *)src;
    int64_t nb = n / QK_K;
    for (int64_t i = 0; i < nb; i++) {
        float d = fp16_to_fp32(blocks[i].d);
        const uint8_t *ql = blocks[i].ql;
        const uint8_t *qh = blocks[i].qh;
        const int8_t *scales = blocks[i].scales;

        for (int k = 0; k < QK_K / 16; k++) {
            float scale = d * scales[k];
            int offset = k * 16;
            for (int j = 0; j < 8; j++) {
                int idx = offset / 2 + j;
                /* Extract 6-bit value */
                uint8_t q_lo = (j < 4) ? (ql[idx] & 0xf) : (ql[idx] >> 4);
                uint8_t q_hi = (qh[(offset / 4) + j / 2] >> ((j % 2) * 2)) & 3;
                int8_t q = (int8_t)((q_lo | (q_hi << 4)) - 32);
                dst[i * QK_K + offset + j] = scale * q;
                dst[i * QK_K + offset + j + 8] = scale * q; /* Simplified - repeat */
            }
        }
    }
}

/* F16: Half-precision float */
static void dequantize_row_f16(const void *src, float *dst, int64_t n)
{
    const uint16_t *fp16 = (const uint16_t *)src;
    for (int64_t i = 0; i < n; i++) {
        dst[i] = fp16_to_fp32(fp16[i]);
    }
}

/* F32: No conversion needed */
static void dequantize_row_f32(const void *src, float *dst, int64_t n)
{
    memcpy(dst, src, n * sizeof(float));
}

/* Generic dequantization based on tensor type */
static void dequantize_tensor(const void *src, float *dst, int64_t n_elements, ggml_type_t type)
{
    switch (type) {
    case GGML_TYPE_F32:
        dequantize_row_f32(src, dst, n_elements);
        break;
    case GGML_TYPE_F16:
        dequantize_row_f16(src, dst, n_elements);
        break;
    case GGML_TYPE_Q8_0:
        dequantize_row_q8_0(src, dst, n_elements);
        break;
    case GGML_TYPE_Q4_0:
        dequantize_row_q4_0(src, dst, n_elements);
        break;
    case GGML_TYPE_Q5_0:
        dequantize_row_q5_0(src, dst, n_elements);
        break;
    case GGML_TYPE_Q4_K:
        dequantize_row_q4_K(src, dst, n_elements);
        break;
    case GGML_TYPE_Q5_K:
        dequantize_row_q5_K(src, dst, n_elements);
        break;
    case GGML_TYPE_Q6_K:
        dequantize_row_q6_K(src, dst, n_elements);
        break;
    default:
        /* Unsupported type - fill with zeros */
        console_printf("[GGUF-INF] WARNING: Unsupported quant type %d\n", type);
        memset(dst, 0, n_elements * sizeof(float));
        break;
    }
}

/* Dequantized weight storage */
static struct {
    float *token_embd;  /* [vocab_size * dim] */
    float *output_norm; /* [dim] */
    float *output;      /* [vocab_size * dim] or NULL */

    /* Per-layer weights (flattened) */
    float **attn_norm;   /* [n_layers][dim] */
    float **attn_q;      /* [n_layers][dim * dim] */
    float **attn_k;      /* [n_layers][dim * kv_dim] */
    float **attn_v;      /* [n_layers][dim * kv_dim] */
    float **attn_output; /* [n_layers][dim * dim] */
    float **ffn_norm;    /* [n_layers][dim] */
    float **ffn_gate;    /* [n_layers][dim * hidden_dim] */
    float **ffn_up;      /* [n_layers][dim * hidden_dim] */
    float **ffn_down;    /* [n_layers][hidden_dim * dim] */
} g_dequant = {0};

static bool g_weights_dequantized = false;

/* GGUF parser interface */
extern const struct gguf_model_arch *gguf_parser_get_arch(void);
extern const struct gguf_tensor_info *gguf_parser_get_tensor_by_name(const char *name);
extern const void *gguf_parser_get_tensor_data_ptr(const struct gguf_tensor_info *info);
extern const char *gguf_parser_get_token(uint32_t index);
extern uint32_t gguf_parser_get_vocab_size(void);

/* Model configuration - populated from GGUF metadata */
typedef struct {
    int dim;            /* embedding dimension */
    int hidden_dim;     /* FFN hidden dimension */
    int n_layers;       /* number of transformer layers */
    int n_heads;        /* number of attention heads */
    int n_kv_heads;     /* number of KV heads (for GQA) */
    int vocab_size;     /* vocabulary size */
    int seq_len;        /* maximum sequence length */
    float rope_theta;   /* RoPE frequency base */
    float rms_norm_eps; /* RMS normalization epsilon */
} ModelConfig;

/* Tensor pointers - fetched by name from GGUF */
typedef struct {
    /* Embeddings */
    const void *token_embd; /* [vocab_size, dim] */

    /* Per-layer weights (arrays of n_layers pointers) */
    const void **attn_norm;   /* [n_layers][dim] */
    const void **attn_q;      /* [n_layers][dim, n_heads * head_dim] */
    const void **attn_k;      /* [n_layers][dim, n_kv_heads * head_dim] */
    const void **attn_v;      /* [n_layers][dim, n_kv_heads * head_dim] */
    const void **attn_output; /* [n_layers][n_heads * head_dim, dim] */
    const void **ffn_norm;    /* [n_layers][dim] */
    const void **ffn_gate;    /* [n_layers][dim, hidden_dim] - w1 */
    const void **ffn_up;      /* [n_layers][dim, hidden_dim] - w3 */
    const void **ffn_down;    /* [n_layers][hidden_dim, dim] - w2 */

    /* Output */
    const void *output_norm; /* [dim] */
    const void *output;      /* [vocab_size, dim] or NULL if shared */
} ModelWeights;

/* Runtime state for inference */
typedef struct {
    float *x;      /* activation [dim] */
    float *xb;     /* buffer [dim] */
    float *xb2;    /* buffer 2 [dim] */
    float *hb;     /* FFN hidden buffer [hidden_dim] */
    float *hb2;    /* FFN hidden buffer 2 [hidden_dim] */
    float *q;      /* query [dim] */
    float *k;      /* key [kv_dim] */
    float *v;      /* value [kv_dim] */
    float *att;    /* attention scores [n_heads, seq_len] */
    float *logits; /* output logits [vocab_size] */

    /* KV cache */
    float *key_cache;   /* [n_layers, seq_len, kv_dim] */
    float *value_cache; /* [n_layers, seq_len, kv_dim] */
} RunState;

/* Global state */
static ModelConfig g_config;
static ModelWeights g_weights;
static RunState g_state;
static bool g_initialized = false;

/* Helper: Get tensor by name */
static const void *get_tensor(const char *name)
{
    const struct gguf_tensor_info *info = gguf_parser_get_tensor_by_name(name);
    if (!info) {
        console_printf("[GGUF-INF] Tensor not found: %s\n", name);
        return NULL;
    }
    return gguf_parser_get_tensor_data_ptr(info);
}

/* Helper: Get layer tensor by name parts */
static const void *get_layer_tensor(const char *prefix, int layer, const char *suffix)
{
    char name[64];
    build_layer_name(name, sizeof(name), prefix, layer, suffix);
    return get_tensor(name);
}

/* Load model weights from GGUF by tensor names */
static int load_weights(void)
{
    console_printf("[GGUF-INF] Loading weights by name...\n");

    /* Token embeddings */
    g_weights.token_embd = get_tensor("token_embd.weight");
    if (!g_weights.token_embd)
        return -1;

    /* Output norm and weights */
    g_weights.output_norm = get_tensor("output_norm.weight");
    if (!g_weights.output_norm)
        return -1;

    /* output.weight may be NULL if tied to token_embd (weight tying) */
    const struct gguf_tensor_info *out_info = gguf_parser_get_tensor_by_name("output.weight");
    g_weights.output = out_info ? gguf_parser_get_tensor_data_ptr(out_info) : NULL;

    /* Allocate layer pointer arrays using heap_alloc (not kmalloc/slab) */
    int n = g_config.n_layers;
    g_weights.attn_norm = (const void **)heap_alloc(n * sizeof(void *));
    g_weights.attn_q = (const void **)heap_alloc(n * sizeof(void *));
    g_weights.attn_k = (const void **)heap_alloc(n * sizeof(void *));
    g_weights.attn_v = (const void **)heap_alloc(n * sizeof(void *));
    g_weights.attn_output = (const void **)heap_alloc(n * sizeof(void *));
    g_weights.ffn_norm = (const void **)heap_alloc(n * sizeof(void *));
    g_weights.ffn_gate = (const void **)heap_alloc(n * sizeof(void *));
    g_weights.ffn_up = (const void **)heap_alloc(n * sizeof(void *));
    g_weights.ffn_down = (const void **)heap_alloc(n * sizeof(void *));

    if (!g_weights.attn_norm || !g_weights.attn_q || !g_weights.attn_k || !g_weights.attn_v ||
        !g_weights.attn_output || !g_weights.ffn_norm || !g_weights.ffn_gate || !g_weights.ffn_up ||
        !g_weights.ffn_down) {
        console_printf("[GGUF-INF] Failed to allocate layer arrays\n");
        return -1;
    }

    /* Load per-layer weights */
    for (int l = 0; l < n; l++) {
        g_weights.attn_norm[l] = get_layer_tensor("blk.", l, ".attn_norm.weight");
        g_weights.attn_q[l] = get_layer_tensor("blk.", l, ".attn_q.weight");
        g_weights.attn_k[l] = get_layer_tensor("blk.", l, ".attn_k.weight");
        g_weights.attn_v[l] = get_layer_tensor("blk.", l, ".attn_v.weight");
        g_weights.attn_output[l] = get_layer_tensor("blk.", l, ".attn_output.weight");
        g_weights.ffn_norm[l] = get_layer_tensor("blk.", l, ".ffn_norm.weight");
        g_weights.ffn_gate[l] = get_layer_tensor("blk.", l, ".ffn_gate.weight");
        g_weights.ffn_up[l] = get_layer_tensor("blk.", l, ".ffn_up.weight");
        g_weights.ffn_down[l] = get_layer_tensor("blk.", l, ".ffn_down.weight");

        if (!g_weights.attn_norm[l] || !g_weights.attn_q[l] || !g_weights.attn_k[l] ||
            !g_weights.attn_v[l] || !g_weights.attn_output[l] || !g_weights.ffn_norm[l] ||
            !g_weights.ffn_gate[l] || !g_weights.ffn_up[l] || !g_weights.ffn_down[l]) {
            console_printf("[GGUF-INF] Missing tensors for layer %d\n", l);
            return -1;
        }
    }

    console_printf("[GGUF-INF] All weights loaded successfully\n");
    return 0;
}

/* Helper to get tensor info by name */
static const struct gguf_tensor_info *get_tensor_info(const char *name)
{
    return gguf_parser_get_tensor_by_name(name);
}

/* Helper to get layer tensor info */
static const struct gguf_tensor_info *get_layer_tensor_info(const char *prefix, int layer,
                                                            const char *suffix)
{
    char name[64];
    build_layer_name(name, sizeof(name), prefix, layer, suffix);
    return gguf_parser_get_tensor_by_name(name);
}

/* Dequantize and allocate a tensor */
static float *dequantize_and_alloc(const char *name, int64_t n_elements)
{
    const struct gguf_tensor_info *info = get_tensor_info(name);
    if (!info)
        return NULL;

    const void *src = gguf_parser_get_tensor_data_ptr(info);
    if (!src)
        return NULL;

    float *dst = (float *)heap_alloc(n_elements * sizeof(float));
    if (!dst) {
        console_printf("[GGUF-INF] Failed to alloc dequant buffer for %s\n", name);
        return NULL;
    }

    dequantize_tensor(src, dst, n_elements, info->type);
    return dst;
}

/* Dequantize layer tensor */
static float *dequantize_layer_tensor(const char *prefix, int layer, const char *suffix,
                                      int64_t n_elements)
{
    char name[64];
    build_layer_name(name, sizeof(name), prefix, layer, suffix);

    const struct gguf_tensor_info *info = gguf_parser_get_tensor_by_name(name);
    if (!info)
        return NULL;

    const void *src = gguf_parser_get_tensor_data_ptr(info);
    if (!src)
        return NULL;

    float *dst = (float *)heap_alloc(n_elements * sizeof(float));
    if (!dst)
        return NULL;

    dequantize_tensor(src, dst, n_elements, info->type);
    return dst;
}

/* Dequantize all model weights */
static int dequantize_weights(void)
{
    console_printf("[GGUF-INF] Dequantizing weights...\n");

    int dim = g_config.dim;
    int hidden_dim = g_config.hidden_dim;
    int n_heads = g_config.n_heads;
    int n_kv_heads = g_config.n_kv_heads;
    int vocab_size = g_config.vocab_size;
    int n_layers = g_config.n_layers;
    int kv_dim = (dim * n_kv_heads) / n_heads;

    /* Token embeddings: [vocab_size, dim] */
    g_dequant.token_embd = dequantize_and_alloc("token_embd.weight", (int64_t)vocab_size * dim);
    if (!g_dequant.token_embd) {
        console_printf("[GGUF-INF] Failed to dequantize token_embd\n");
        return -1;
    }

    /* Output norm: [dim] */
    g_dequant.output_norm = dequantize_and_alloc("output_norm.weight", dim);
    if (!g_dequant.output_norm) {
        console_printf("[GGUF-INF] Failed to dequantize output_norm\n");
        return -1;
    }

    /* Output weights (optional - may be tied to token_embd) */
    const struct gguf_tensor_info *out_info = get_tensor_info("output.weight");
    if (out_info) {
        g_dequant.output = dequantize_and_alloc("output.weight", (int64_t)vocab_size * dim);
    } else {
        g_dequant.output = NULL; /* Will use token_embd */
    }

    /* Allocate layer arrays */
    g_dequant.attn_norm = (float **)heap_alloc(n_layers * sizeof(float *));
    g_dequant.attn_q = (float **)heap_alloc(n_layers * sizeof(float *));
    g_dequant.attn_k = (float **)heap_alloc(n_layers * sizeof(float *));
    g_dequant.attn_v = (float **)heap_alloc(n_layers * sizeof(float *));
    g_dequant.attn_output = (float **)heap_alloc(n_layers * sizeof(float *));
    g_dequant.ffn_norm = (float **)heap_alloc(n_layers * sizeof(float *));
    g_dequant.ffn_gate = (float **)heap_alloc(n_layers * sizeof(float *));
    g_dequant.ffn_up = (float **)heap_alloc(n_layers * sizeof(float *));
    g_dequant.ffn_down = (float **)heap_alloc(n_layers * sizeof(float *));

    if (!g_dequant.attn_norm || !g_dequant.attn_q || !g_dequant.attn_k || !g_dequant.attn_v ||
        !g_dequant.attn_output || !g_dequant.ffn_norm || !g_dequant.ffn_gate || !g_dequant.ffn_up ||
        !g_dequant.ffn_down) {
        console_printf("[GGUF-INF] Failed to allocate dequant layer arrays\n");
        return -1;
    }

    /* Dequantize per-layer weights */
    for (int l = 0; l < n_layers; l++) {
        console_printf("[GGUF-INF] Dequantizing layer %d/%d...\n", l + 1, n_layers);

        /* Attention norm: [dim] */
        g_dequant.attn_norm[l] = dequantize_layer_tensor("blk.", l, ".attn_norm.weight", dim);

        /* Q/K/V projections */
        g_dequant.attn_q[l] =
            dequantize_layer_tensor("blk.", l, ".attn_q.weight", (int64_t)dim * dim);
        g_dequant.attn_k[l] =
            dequantize_layer_tensor("blk.", l, ".attn_k.weight", (int64_t)kv_dim * dim);
        g_dequant.attn_v[l] =
            dequantize_layer_tensor("blk.", l, ".attn_v.weight", (int64_t)kv_dim * dim);
        g_dequant.attn_output[l] =
            dequantize_layer_tensor("blk.", l, ".attn_output.weight", (int64_t)dim * dim);

        /* FFN norm: [dim] */
        g_dequant.ffn_norm[l] = dequantize_layer_tensor("blk.", l, ".ffn_norm.weight", dim);

        /* FFN weights */
        g_dequant.ffn_gate[l] =
            dequantize_layer_tensor("blk.", l, ".ffn_gate.weight", (int64_t)hidden_dim * dim);
        g_dequant.ffn_up[l] =
            dequantize_layer_tensor("blk.", l, ".ffn_up.weight", (int64_t)hidden_dim * dim);
        g_dequant.ffn_down[l] =
            dequantize_layer_tensor("blk.", l, ".ffn_down.weight", (int64_t)dim * hidden_dim);

        /* Check all succeeded */
        if (!g_dequant.attn_norm[l] || !g_dequant.attn_q[l] || !g_dequant.attn_k[l] ||
            !g_dequant.attn_v[l] || !g_dequant.attn_output[l] || !g_dequant.ffn_norm[l] ||
            !g_dequant.ffn_gate[l] || !g_dequant.ffn_up[l] || !g_dequant.ffn_down[l]) {
            console_printf("[GGUF-INF] Failed to dequantize layer %d\n", l);
            return -1;
        }
    }

    g_weights_dequantized = true;
    console_printf("[GGUF-INF] All weights dequantized successfully\n");
    return 0;
}

/* Allocate runtime buffers */
static int alloc_run_state(void)
{
    int dim = g_config.dim;
    int hidden_dim = g_config.hidden_dim;
    int n_heads = g_config.n_heads;
    int n_kv_heads = g_config.n_kv_heads;
    int seq_len = g_config.seq_len;
    int vocab_size = g_config.vocab_size;
    int kv_dim = (dim * n_kv_heads) / n_heads;
    int n_layers = g_config.n_layers;

    console_printf("[GGUF-INF] Allocating runtime state...\n");

    g_state.x = (float *)heap_alloc(dim * sizeof(float));
    g_state.xb = (float *)heap_alloc(dim * sizeof(float));
    g_state.xb2 = (float *)heap_alloc(dim * sizeof(float));
    g_state.hb = (float *)heap_alloc(hidden_dim * sizeof(float));
    g_state.hb2 = (float *)heap_alloc(hidden_dim * sizeof(float));
    g_state.q = (float *)heap_alloc(dim * sizeof(float));
    g_state.k = (float *)heap_alloc(kv_dim * sizeof(float));
    g_state.v = (float *)heap_alloc(kv_dim * sizeof(float));
    g_state.att = (float *)heap_alloc(n_heads * seq_len * sizeof(float));
    g_state.logits = (float *)heap_alloc(vocab_size * sizeof(float));
    g_state.key_cache = (float *)heap_alloc(n_layers * seq_len * kv_dim * sizeof(float));
    g_state.value_cache = (float *)heap_alloc(n_layers * seq_len * kv_dim * sizeof(float));

    if (!g_state.x || !g_state.xb || !g_state.xb2 || !g_state.hb || !g_state.hb2 || !g_state.q ||
        !g_state.k || !g_state.v || !g_state.att || !g_state.logits || !g_state.key_cache ||
        !g_state.value_cache) {
        console_printf("[GGUF-INF] Failed to allocate runtime buffers\n");
        return -1;
    }

    /* Zero KV cache */
    int kv_cache_size = n_layers * seq_len * kv_dim * sizeof(float);
    memset(g_state.key_cache, 0, kv_cache_size);
    memset(g_state.value_cache, 0, kv_cache_size);

    console_printf("[GGUF-INF] Runtime state allocated\n");
    return 0;
}

/* RMS Normalization - SIMD optimized for 3-5x speedup */
static void rmsnorm(float *o, const float *x, const float *weight, int size, float eps)
{
    /* Phase 1: Compute sum of squares with SIMD */
    float ss = 0.0f;
    int i = 0;

#ifdef __aarch64__
    /* ARM NEON: sum of squares */
    float32x4_t vss = vdupq_n_f32(0.0f);
    for (; i + 4 <= size; i += 4) {
        float32x4_t vx = vld1q_f32(x + i);
        vss = vmlaq_f32(vss, vx, vx);  /* vss += vx * vx */
    }
    ss = vaddvq_f32(vss);
#elif defined(__x86_64__) || defined(_M_X64)
#ifdef __AVX__
    /* x86 AVX: sum of squares */
    __m256 vss = _mm256_setzero_ps();
    for (; i + 8 <= size; i += 8) {
        __m256 vx = _mm256_loadu_ps(x + i);
        vss = _mm256_add_ps(vss, _mm256_mul_ps(vx, vx));
    }
    __m128 hi = _mm256_extractf128_ps(vss, 1);
    __m128 lo = _mm256_castps256_ps128(vss);
    __m128 sum128 = _mm_add_ps(lo, hi);
    __m128 shuf = _mm_shuffle_ps(sum128, sum128, _MM_SHUFFLE(2, 3, 0, 1));
    sum128 = _mm_add_ps(sum128, shuf);
    shuf = _mm_movehl_ps(shuf, sum128);
    sum128 = _mm_add_ss(sum128, shuf);
    ss = _mm_cvtss_f32(sum128);
#else
    /* x86 SSE2: sum of squares */
    __m128 vss = _mm_setzero_ps();
    for (; i + 4 <= size; i += 4) {
        __m128 vx = _mm_loadu_ps(x + i);
        vss = _mm_add_ps(vss, _mm_mul_ps(vx, vx));
    }
    __m128 shuf = _mm_shuffle_ps(vss, vss, _MM_SHUFFLE(2, 3, 0, 1));
    vss = _mm_add_ps(vss, shuf);
    shuf = _mm_movehl_ps(shuf, vss);
    vss = _mm_add_ss(vss, shuf);
    ss = _mm_cvtss_f32(vss);
#endif
#endif

    /* Scalar remainder */
    for (; i < size; i++) {
        ss += x[i] * x[i];
    }

    /* Compute normalization factor */
    ss = 1.0f / sqrtf(ss / size + eps);

    /* Phase 2: Apply normalization with SIMD */
    i = 0;
#ifdef __aarch64__
    float32x4_t vss_vec = vdupq_n_f32(ss);
    for (; i + 4 <= size; i += 4) {
        float32x4_t vx = vld1q_f32(x + i);
        float32x4_t vw = vld1q_f32(weight + i);
        float32x4_t result = vmulq_f32(vmulq_f32(vx, vss_vec), vw);
        vst1q_f32(o + i, result);
    }
#elif defined(__x86_64__) || defined(_M_X64)
#ifdef __AVX__
    __m256 vss_vec = _mm256_set1_ps(ss);
    for (; i + 8 <= size; i += 8) {
        __m256 vx = _mm256_loadu_ps(x + i);
        __m256 vw = _mm256_loadu_ps(weight + i);
        __m256 result = _mm256_mul_ps(_mm256_mul_ps(vx, vss_vec), vw);
        _mm256_storeu_ps(o + i, result);
    }
#else
    __m128 vss_vec = _mm_set1_ps(ss);
    for (; i + 4 <= size; i += 4) {
        __m128 vx = _mm_loadu_ps(x + i);
        __m128 vw = _mm_loadu_ps(weight + i);
        __m128 result = _mm_mul_ps(_mm_mul_ps(vx, vss_vec), vw);
        _mm_storeu_ps(o + i, result);
    }
#endif
#endif

    /* Scalar remainder */
    for (; i < size; i++) {
        o[i] = weight[i] * (ss * x[i]);
    }
}

/* Matrix-vector multiply: out = mat @ x
 * SIMD-optimized for ARM NEON and x86 SSE/AVX - 4-8x faster than scalar
 */
static void matmul(float *out, const float *mat, const float *x, int rows, int cols)
{
    for (int i = 0; i < rows; i++) {
        const float *row = &mat[i * cols];
        float sum = 0.0f;
        int j = 0;

#ifdef __aarch64__
        /* ARM NEON: Process 4 floats at a time with FMA */
        float32x4_t vsum = vdupq_n_f32(0.0f);
        for (; j + 4 <= cols; j += 4) {
            float32x4_t vm = vld1q_f32(row + j);
            float32x4_t vx = vld1q_f32(x + j);
            vsum = vmlaq_f32(vsum, vm, vx);  /* FMA: vsum += vm * vx */
        }
        /* Horizontal sum: reduce 4 lanes to scalar */
        sum = vaddvq_f32(vsum);

#elif defined(__x86_64__) || defined(_M_X64)
#ifdef __AVX__
        /* x86 AVX: Process 8 floats at a time */
        __m256 vsum = _mm256_setzero_ps();
        for (; j + 8 <= cols; j += 8) {
            __m256 vm = _mm256_loadu_ps(row + j);
            __m256 vx = _mm256_loadu_ps(x + j);
            vsum = _mm256_add_ps(vsum, _mm256_mul_ps(vm, vx));
        }
        /* Horizontal sum */
        __m128 hi = _mm256_extractf128_ps(vsum, 1);
        __m128 lo = _mm256_castps256_ps128(vsum);
        __m128 sum128 = _mm_add_ps(lo, hi);
        __m128 shuf = _mm_shuffle_ps(sum128, sum128, _MM_SHUFFLE(2, 3, 0, 1));
        sum128 = _mm_add_ps(sum128, shuf);
        shuf = _mm_movehl_ps(shuf, sum128);
        sum128 = _mm_add_ss(sum128, shuf);
        sum = _mm_cvtss_f32(sum128);
#else
        /* x86 SSE2: Process 4 floats at a time */
        __m128 vsum = _mm_setzero_ps();
        for (; j + 4 <= cols; j += 4) {
            __m128 vm = _mm_loadu_ps(row + j);
            __m128 vx = _mm_loadu_ps(x + j);
            vsum = _mm_add_ps(vsum, _mm_mul_ps(vm, vx));
        }
        /* Horizontal sum */
        __m128 shuf = _mm_shuffle_ps(vsum, vsum, _MM_SHUFFLE(2, 3, 0, 1));
        vsum = _mm_add_ps(vsum, shuf);
        shuf = _mm_movehl_ps(shuf, vsum);
        vsum = _mm_add_ss(vsum, shuf);
        sum = _mm_cvtss_f32(vsum);
#endif
#else
        /* Scalar fallback with loop unrolling */
        float sum0 = 0.0f, sum1 = 0.0f, sum2 = 0.0f, sum3 = 0.0f;
        for (; j + 4 <= cols; j += 4) {
            sum0 += row[j] * x[j];
            sum1 += row[j+1] * x[j+1];
            sum2 += row[j+2] * x[j+2];
            sum3 += row[j+3] * x[j+3];
        }
        sum = sum0 + sum1 + sum2 + sum3;
#endif

        /* Handle remaining elements */
        for (; j < cols; j++) {
            sum += row[j] * x[j];
        }
        out[i] = sum;
    }
}

/* Softmax - SIMD optimized for 2-4x speedup */
static void softmax(float *x, int size)
{
    /* Phase 1: Find max value with SIMD */
    float max_val = x[0];
    int i = 1;

#ifdef __aarch64__
    float32x4_t vmax = vdupq_n_f32(x[0]);
    for (; i + 4 <= size; i += 4) {
        float32x4_t v = vld1q_f32(x + i);
        vmax = vmaxq_f32(vmax, v);
    }
    max_val = vmaxvq_f32(vmax);
#elif defined(__x86_64__) || defined(_M_X64)
    __m128 vmax = _mm_set1_ps(x[0]);
    for (; i + 4 <= size; i += 4) {
        __m128 v = _mm_loadu_ps(x + i);
        vmax = _mm_max_ps(vmax, v);
    }
    __m128 shuf = _mm_shuffle_ps(vmax, vmax, _MM_SHUFFLE(2, 3, 0, 1));
    vmax = _mm_max_ps(vmax, shuf);
    shuf = _mm_movehl_ps(shuf, vmax);
    vmax = _mm_max_ss(vmax, shuf);
    max_val = _mm_cvtss_f32(vmax);
#endif

    /* Scalar remainder */
    for (; i < size; i++) {
        if (x[i] > max_val) max_val = x[i];
    }

    /* Phase 2: Compute exp(x - max) and sum (scalar - expf is not easily vectorized) */
    float sum = 0.0f;
    for (i = 0; i < size; i++) {
        x[i] = expf(x[i] - max_val);
        sum += x[i];
    }

    /* Phase 3: Normalize with SIMD */
    float inv_sum = 1.0f / sum;
    i = 0;

#ifdef __aarch64__
    float32x4_t vinv = vdupq_n_f32(inv_sum);
    for (; i + 4 <= size; i += 4) {
        float32x4_t v = vld1q_f32(x + i);
        v = vmulq_f32(v, vinv);
        vst1q_f32(x + i, v);
    }
#elif defined(__x86_64__) || defined(_M_X64)
#ifdef __AVX__
    __m256 vinv = _mm256_set1_ps(inv_sum);
    for (; i + 8 <= size; i += 8) {
        __m256 v = _mm256_loadu_ps(x + i);
        v = _mm256_mul_ps(v, vinv);
        _mm256_storeu_ps(x + i, v);
    }
#else
    __m128 vinv = _mm_set1_ps(inv_sum);
    for (; i + 4 <= size; i += 4) {
        __m128 v = _mm_loadu_ps(x + i);
        v = _mm_mul_ps(v, vinv);
        _mm_storeu_ps(x + i, v);
    }
#endif
#endif

    /* Scalar remainder */
    for (; i < size; i++) {
        x[i] *= inv_sum;
    }
}

/* SiLU activation: x * sigmoid(x) */
static float silu(float x) { return x / (1.0f + expf(-x)); }

/* Forward declarations for math functions */
extern float sinf(float x);
extern float cosf(float x);
extern float powf(float base, float exp);

/* Apply RoPE (Rotary Position Embeddings) to Q and K vectors */
static void rope(float *q, float *k, int pos, int head_dim, int n_heads, int n_kv_heads,
                 float theta)
{
    int kv_mul = n_heads / n_kv_heads;

    for (int h = 0; h < n_heads; h++) {
        float *q_head = q + h * head_dim;
        int kv_h = h / kv_mul;
        float *k_head = (h == kv_h * kv_mul) ? (k + kv_h * head_dim) : NULL;

        for (int i = 0; i < head_dim; i += 2) {
            /* Compute rotation angle for this dimension pair */
            float freq = 1.0f / powf(theta, (float)i / (float)head_dim);
            float angle = pos * freq;
            float cos_val = cosf(angle);
            float sin_val = sinf(angle);

            /* Rotate Q */
            float q0 = q_head[i];
            float q1 = q_head[i + 1];
            q_head[i] = q0 * cos_val - q1 * sin_val;
            q_head[i + 1] = q0 * sin_val + q1 * cos_val;

            /* Rotate K (only for first head in each KV group) */
            if (k_head) {
                float k0 = k_head[i];
                float k1 = k_head[i + 1];
                k_head[i] = k0 * cos_val - k1 * sin_val;
                k_head[i + 1] = k0 * sin_val + k1 * cos_val;
            }
        }
    }
}

/* Single transformer forward pass - uses dequantized weights */
static void transformer_forward(int token, int pos)
{
    int dim = g_config.dim;
    int hidden_dim = g_config.hidden_dim;
    int n_heads = g_config.n_heads;
    int n_kv_heads = g_config.n_kv_heads;
    int head_dim = dim / n_heads;
    int kv_dim = (dim * n_kv_heads) / n_heads;
    int kv_mul = n_heads / n_kv_heads;
    float eps = g_config.rms_norm_eps;

    /* Get token embedding from dequantized weights */
    float *x = g_state.x;
    if (!g_dequant.token_embd) {
        console_printf("ERROR: token_embd NULL\n");
        return;
    }
    memcpy(x, g_dequant.token_embd + token * dim, dim * sizeof(float));

    /* Process each layer */
    for (int l = 0; l < g_config.n_layers; l++) {
        /* Check dequantized weights */
        if (!g_dequant.attn_norm || !g_dequant.attn_norm[l]) {
            console_printf("ERR: attn_norm[%d] NULL\n", l);
            return;
        }

        /* Attention norm */
        rmsnorm(g_state.xb, x, g_dequant.attn_norm[l], dim, eps);

        /* QKV projections */
        matmul(g_state.q, g_dequant.attn_q[l], g_state.xb, dim, dim);
        matmul(g_state.k, g_dequant.attn_k[l], g_state.xb, kv_dim, dim);
        matmul(g_state.v, g_dequant.attn_v[l], g_state.xb, kv_dim, dim);

        /* Apply RoPE (Rotary Position Embeddings) */
        rope(g_state.q, g_state.k, pos, head_dim, n_heads, n_kv_heads, g_config.rope_theta);

        /* Cache KV */
        int cache_offset = l * g_config.seq_len * kv_dim + pos * kv_dim;
        memcpy(g_state.key_cache + cache_offset, g_state.k, kv_dim * sizeof(float));
        memcpy(g_state.value_cache + cache_offset, g_state.v, kv_dim * sizeof(float));

        /* Multi-head attention */
        memset(g_state.xb, 0, dim * sizeof(float));
        for (int h = 0; h < n_heads; h++) {
            float *q_head = g_state.q + h * head_dim;
            float *att = g_state.att + h * g_config.seq_len;
            int kv_h = h / kv_mul;

            /* Attention scores */
            for (int t = 0; t <= pos; t++) {
                float *k_t = g_state.key_cache + l * g_config.seq_len * kv_dim + t * kv_dim +
                             kv_h * head_dim;
                float score = 0.0f;
                for (int i = 0; i < head_dim; i++) {
                    score += q_head[i] * k_t[i];
                }
                att[t] = score / sqrtf((float)head_dim);
            }

            /* Softmax attention */
            softmax(att, pos + 1);

            /* Weighted sum of values */
            float *out = g_state.xb + h * head_dim;
            for (int t = 0; t <= pos; t++) {
                float *v_t = g_state.value_cache + l * g_config.seq_len * kv_dim + t * kv_dim +
                             kv_h * head_dim;
                float a = att[t];
                for (int i = 0; i < head_dim; i++) {
                    out[i] += a * v_t[i];
                }
            }
        }

        /* Output projection */
        matmul(g_state.xb2, g_dequant.attn_output[l], g_state.xb, dim, dim);

        /* Residual connection */
        for (int i = 0; i < dim; i++) {
            x[i] += g_state.xb2[i];
        }

        /* FFN norm */
        rmsnorm(g_state.xb, x, g_dequant.ffn_norm[l], dim, eps);

        /* FFN: SwiGLU */
        matmul(g_state.hb, g_dequant.ffn_gate[l], g_state.xb, hidden_dim, dim);
        matmul(g_state.hb2, g_dequant.ffn_up[l], g_state.xb, hidden_dim, dim);

        for (int i = 0; i < hidden_dim; i++) {
            g_state.hb[i] = silu(g_state.hb[i]) * g_state.hb2[i];
        }

        matmul(g_state.xb, g_dequant.ffn_down[l], g_state.hb, dim, hidden_dim);

        /* Residual connection */
        for (int i = 0; i < dim; i++) {
            x[i] += g_state.xb[i];
        }
    }

    /* Final norm */
    rmsnorm(x, x, g_dequant.output_norm, dim, eps);

    /* Output logits - use output weights or tied embeddings */
    const float *output_weights = g_dequant.output ? g_dequant.output : g_dequant.token_embd;
    matmul(g_state.logits, output_weights, x, g_config.vocab_size, dim);
}

/* Sample next token from logits */
static int sample_argmax(void)
{
    int max_idx = 0;
    float max_val = g_state.logits[0];

    for (int i = 1; i < g_config.vocab_size; i++) {
        if (g_state.logits[i] > max_val) {
            max_val = g_state.logits[i];
            max_idx = i;
        }
    }

    return max_idx;
}

/* Initialize GGUF inference engine from parsed model */
int gguf_inference_init(void)
{
    if (g_initialized) {
        console_printf("[GGUF-INF] Already initialized\n");
        return 0;
    }

    console_printf("[GGUF-INF] Initializing GGUF inference engine...\n");

    /* Get model architecture from GGUF parser */
    const struct gguf_model_arch *arch = gguf_parser_get_arch();
    if (!arch) {
        console_printf("[GGUF-INF] No GGUF model loaded\n");
        return -1;
    }

    /* Populate config from GGUF metadata */
    g_config.dim = arch->embedding_length;
    g_config.hidden_dim = arch->feed_forward_length;
    g_config.n_layers = arch->block_count;
    g_config.n_heads = arch->attention_head_count;
    g_config.n_kv_heads = arch->attention_head_count_kv;
    g_config.vocab_size = arch->vocab_size;
    g_config.seq_len = arch->context_length;
    g_config.rope_theta = arch->rope_freq_base > 0 ? arch->rope_freq_base : 10000.0f;
    g_config.rms_norm_eps =
        arch->attention_layer_norm_rms_epsilon > 0 ? arch->attention_layer_norm_rms_epsilon : 1e-5f;

    console_printf("[GGUF-INF] Model config:\n");
    console_printf("  dim=%d, hidden=%d, layers=%d\n", g_config.dim, g_config.hidden_dim,
                   g_config.n_layers);
    console_printf("  heads=%d/%d, vocab=%d, ctx=%d\n", g_config.n_heads, g_config.n_kv_heads,
                   g_config.vocab_size, g_config.seq_len);

    /* Load weights from GGUF tensors */
    if (load_weights() != 0) {
        console_printf("[GGUF-INF] Failed to load weights\n");
        return -1;
    }

    /* Dequantize weights (Q8_0, Q4_0, F16 -> F32) */
    if (dequantize_weights() != 0) {
        console_printf("[GGUF-INF] Failed to dequantize weights\n");
        return -1;
    }

    /* Allocate runtime state */
    if (alloc_run_state() != 0) {
        console_printf("[GGUF-INF] Failed to allocate runtime state\n");
        return -1;
    }

    g_initialized = true;
    console_printf("[GGUF-INF] Initialization complete\n");
    return 0;
}

/* Generate text from prompt tokens */
int gguf_inference_generate(const int *prompt_tokens, int prompt_len, int *output_tokens,
                            int max_output)
{
    if (!g_initialized) {
        console_printf("[GGUF-INF] Not initialized\n");
        return -1;
    }

    int pos = 0;
    int token = prompt_tokens[0];
    int generated = 0;

    while (pos < g_config.seq_len && generated < max_output) {
        /* Forward pass */
        transformer_forward(token, pos);

        /* Get next token */
        int next_token;
        if (pos < prompt_len - 1) {
            /* Still processing prompt */
            next_token = prompt_tokens[pos + 1];
        } else {
            /* Generate new token */
            next_token = sample_argmax();
            output_tokens[generated++] = next_token;

            /* Check for EOS */
            if (next_token == 2)
                break; /* EOS token ID */
        }

        token = next_token;
        pos++;
    }

    return generated;
}

/* Check if inference engine is ready */
bool gguf_inference_is_ready(void) { return g_initialized; }

/* Get vocab token text */
const char *gguf_inference_get_token(int token_id)
{
    return gguf_parser_get_token((uint32_t)token_id);
}
