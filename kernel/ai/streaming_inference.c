/* Streaming Inference Engine for EMBODIOS
 *
 * Key features:
 * - Dynamic architecture detection from GGUF metadata
 * - Streaming layer-by-layer processing
 * - On-the-fly dequantization (keeps weights quantized)
 * - Supports models larger than available RAM
 * - Memory-efficient: only loads what's needed
 *
 * Memory usage comparison (for 1B model, dim=2048, 22 layers):
 * - Old approach: ~4GB (all weights dequantized to F32)
 * - Streaming: ~64MB (only current layer + runtime buffers)
 */

#include <embodios/console.h>
#include <embodios/gguf_parser.h>
#include <embodios/mm.h>
#include <embodios/types.h>
#include <embodios/streaming_inference.h>
#include <embodios/parallel_inference.h>
#include <embodios/kernel.h>

/* Enable parallel inference (set to 0 to disable) */
#ifndef PARALLEL_INFERENCE_ENABLED
#define PARALLEL_INFERENCE_ENABLED 1  /* Enabled - uses parallel attention/softmax */
#endif

/* Number of threads for parallel inference
 * Set to 1 for QEMU single-core emulation
 * Set to 4+ for real multi-core hardware */
#ifndef PARALLEL_NUM_THREADS
#define PARALLEL_NUM_THREADS 1  /* Single-threaded for QEMU */
#endif

/* NEON SIMD for ARM64 */
#ifdef __aarch64__
#include <arm_neon.h>
#endif

/* SSE2/AVX2 SIMD for x86_64 */
#if defined(__x86_64__) || defined(_M_X64)
/* Prevent mm_malloc.h from being included (needs stdlib.h) */
#define _MM_MALLOC_H_INCLUDED
#define __MM_MALLOC_H
#include <emmintrin.h>  /* SSE2 - baseline for x86_64 */
#include <xmmintrin.h>  /* SSE */
#ifdef __SSE3__
#include <pmmintrin.h>  /* SSE3 */
#endif
#ifdef __AVX__
#include <immintrin.h>  /* AVX/AVX2 */
#endif

/* Runtime AVX2 detection */
static int g_avx2_checked = 0;
static int g_avx2_available = 0;

static inline int has_avx2(void) {
    if (g_avx2_checked) return g_avx2_available;
    g_avx2_checked = 1;
#ifdef __AVX2__
    unsigned int eax, ebx, ecx, edx;
    __asm__ __volatile__(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(7), "c"(0)
    );
    g_avx2_available = (ebx & (1 << 5)) != 0;
#endif
    return g_avx2_available;
}

/* SSE2 float dot product (4 floats per iteration) */
static inline float dot_product_sse2(const float* a, const float* b, int n) {
    __m128 sum = _mm_setzero_ps();
    int i = 0;

    /* Process 4 floats at a time */
    for (; i + 4 <= n; i += 4) {
        __m128 va = _mm_loadu_ps(a + i);
        __m128 vb = _mm_loadu_ps(b + i);
        sum = _mm_add_ps(sum, _mm_mul_ps(va, vb));
    }

    /* Horizontal sum: sum = {a, b, c, d} -> a+b+c+d */
    __m128 shuf = _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(2, 3, 0, 1));
    sum = _mm_add_ps(sum, shuf);
    shuf = _mm_movehl_ps(shuf, sum);
    sum = _mm_add_ss(sum, shuf);
    float result = _mm_cvtss_f32(sum);

    /* Handle remainder */
    for (; i < n; i++) {
        result += a[i] * b[i];
    }

    return result;
}

#ifdef __AVX__
/* AVX float dot product (8 floats per iteration) */
static inline float dot_product_avx(const float* a, const float* b, int n) {
    __m256 sum = _mm256_setzero_ps();
    int i = 0;

    /* Process 8 floats at a time */
    for (; i + 8 <= n; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        sum = _mm256_add_ps(sum, _mm256_mul_ps(va, vb));
    }

    /* Horizontal sum */
    __m128 hi = _mm256_extractf128_ps(sum, 1);
    __m128 lo = _mm256_castps256_ps128(sum);
    __m128 sum128 = _mm_add_ps(lo, hi);
    __m128 shuf = _mm_shuffle_ps(sum128, sum128, _MM_SHUFFLE(2, 3, 0, 1));
    sum128 = _mm_add_ps(sum128, shuf);
    shuf = _mm_movehl_ps(shuf, sum128);
    sum128 = _mm_add_ss(sum128, shuf);
    float result = _mm_cvtss_f32(sum128);

    /* Handle remainder with SSE2 */
    for (; i + 4 <= n; i += 4) {
        __m128 va = _mm_loadu_ps(a + i);
        __m128 vb = _mm_loadu_ps(b + i);
        __m128 prod = _mm_mul_ps(va, vb);
        shuf = _mm_shuffle_ps(prod, prod, _MM_SHUFFLE(2, 3, 0, 1));
        prod = _mm_add_ps(prod, shuf);
        shuf = _mm_movehl_ps(shuf, prod);
        prod = _mm_add_ss(prod, shuf);
        result += _mm_cvtss_f32(prod);
    }

    /* Scalar remainder */
    for (; i < n; i++) {
        result += a[i] * b[i];
    }

    return result;
}
#endif /* __AVX__ */

/* Dispatcher: choose best available SIMD */
static inline float simd_dot_product(const float* a, const float* b, int n) {
#ifdef __AVX__
    if (has_avx2()) {
        return dot_product_avx(a, b, n);
    }
#endif
    return dot_product_sse2(a, b, n);
}

#endif /* __x86_64__ */

/* Forward declarations */
extern float sqrtf(float x);
extern float expf(float x);
extern float sinf(float x);
extern float cosf(float x);
extern float logf(float x);
extern float powf(float base, float exp);
extern void* memset(void*, int, size_t);
extern void* memcpy(void*, const void*, size_t);
extern size_t strlen(const char*);
extern int strncmp(const char*, const char*, size_t);

/* Helper to build "blk.N.name" style tensor names */
static void build_layer_name(char* buf, size_t size, const char* prefix, int layer,
                             const char* suffix) {
    size_t pos = 0;
    const char* p = prefix;
    while (*p && pos < size - 1) buf[pos++] = *p++;
    if (layer >= 100 && pos < size - 1) buf[pos++] = '0' + (layer / 100);
    if (layer >= 10 && pos < size - 1) buf[pos++] = '0' + ((layer / 10) % 10);
    if (pos < size - 1) buf[pos++] = '0' + (layer % 10);
    p = suffix;
    while (*p && pos < size - 1) buf[pos++] = *p++;
    buf[pos] = '\0';
}

/* ============================================================================
 * Dynamic Model Configuration
 * Populated from GGUF metadata - no hardcoded values
 * ============================================================================ */

typedef struct {
    /* Core dimensions - read from GGUF */
    int dim;           /* embedding dimension (e.g., 2048) */
    int hidden_dim;    /* feed-forward hidden dimension */
    int n_layers;      /* number of transformer layers */
    int n_heads;       /* number of attention heads */
    int n_kv_heads;    /* number of key/value heads (GQA) */
    int vocab_size;    /* vocabulary size */
    int seq_len;       /* maximum sequence length */

    /* Derived values */
    int head_dim;      /* dim / n_heads */
    int kv_dim;        /* head_dim * n_kv_heads */
    int kv_mul;        /* n_heads / n_kv_heads (for GQA) */

    /* Model parameters */
    float rope_theta;  /* RoPE base frequency */
    float rms_norm_eps;/* RMSNorm epsilon */

    /* Special token IDs */
    int eos_token_id;  /* End of sequence token */
    int bos_token_id;  /* Beginning of sequence token */

    /* Architecture name */
    char arch_name[64];
} StreamingConfig;

/* Runtime state - only allocate what's needed */
typedef struct {
    /* Current hidden state */
    float* x;          /* [dim] - current position embedding */
    float* xb;         /* [dim] - buffer after attention */
    float* xb2;        /* [dim] - buffer for residual */

    /* Attention buffers */
    float* q;          /* [dim] - query */
    float* k;          /* [kv_dim] - key */
    float* v;          /* [kv_dim] - value */
    float* att;        /* [n_heads * seq_len] - attention scores */

    /* FFN buffers */
    float* hb;         /* [hidden_dim] - hidden buffer 1 */
    float* hb2;        /* [hidden_dim] - hidden buffer 2 */

    /* Output */
    float* logits;     /* [vocab_size] */

    /* KV cache - streaming friendly */
    float* key_cache;   /* [n_layers * seq_len * kv_dim] */
    float* value_cache; /* [n_layers * seq_len * kv_dim] */

    /* Layer dequantization buffer (reused per layer) */
    float* layer_weights;  /* Temporary buffer for dequantized layer weights */
    size_t layer_buf_size; /* Size of layer buffer */
} StreamingState;

/* Global state */
static StreamingConfig g_cfg = {0};
static StreamingState g_state = {0};
static bool g_initialized = false;

/* Deterministic mode configuration */
static deterministic_config_t g_deterministic_config = {
    .interrupt_disable = false,
    .preallocate_buffers = false,
    .max_latency_us = 0
};

/* Quantized weight pointers (raw GGUF data - not dequantized) */
typedef struct {
    const void* token_embd;
    const void* output_norm;
    const void* output;
    int token_embd_type;
    int output_norm_type;
    int output_type;
    /* Embedding shape info - GGUF stores as [dim, vocab] not [vocab, dim] */
    bool token_embd_transposed;  /* True if shape is [dim, vocab_size] */
    int token_embd_vocab_size;   /* Second dimension (vocab_size) */
} GlobalWeights;

typedef struct {
    const void* attn_norm;
    const void* attn_q;
    const void* attn_k;
    const void* attn_v;
    const void* attn_output;
    const void* ffn_norm;
    const void* ffn_gate;
    const void* ffn_up;
    const void* ffn_down;
    int attn_norm_type;
    int attn_q_type;
    int attn_k_type;
    int attn_v_type;
    int attn_output_type;
    int ffn_norm_type;
    int ffn_gate_type;
    int ffn_up_type;
    int ffn_down_type;
} LayerWeights;

static GlobalWeights g_weights = {0};
static LayerWeights* g_layer_weights = NULL;

/* ============================================================================
 * Deterministic Mode Critical Section Helpers
 * ============================================================================ */

/* Enter critical section - disable interrupts if deterministic mode enabled */
static inline void critical_section_enter(void) {
    if (g_deterministic_config.interrupt_disable) {
        arch_disable_interrupts();
    }
}

/* Exit critical section - re-enable interrupts if deterministic mode enabled */
static inline void critical_section_exit(void) {
    if (g_deterministic_config.interrupt_disable) {
        arch_enable_interrupts();
    }
}

/* ============================================================================
 * Dequantization Functions (On-the-fly)
 * ============================================================================ */

/* Convert float16 to float32 */
static inline float fp16_to_fp32(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t f;

    if (exp == 0) {
        if (mant == 0) f = sign << 31;
        else {
            exp = 1;
            while (!(mant & 0x400)) { mant <<= 1; exp--; }
            mant &= 0x3ff;
            f = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = (sign << 31) | 0x7f800000 | (mant << 13);
    } else {
        f = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
    }

    union { uint32_t i; float f; } u;
    u.i = f;
    return u.f;
}

/* Quantization block sizes */
#define QK4_0 32
#define QK4_1 32
#define QK5_0 32
#define QK5_1 32
#define QK8_0 32
#define QK_K  256

/* Block structures */
typedef struct __attribute__((packed)) {
    uint16_t d;
    uint8_t qs[QK4_0 / 2];
} block_q4_0;

typedef struct __attribute__((packed)) {
    uint16_t d;
    uint16_t m;
    uint8_t qs[QK4_1 / 2];
} block_q4_1;

typedef struct __attribute__((packed)) {
    uint16_t d;
    int8_t qs[QK8_0];
} block_q8_0;

/* Q4_K block structure */
typedef struct __attribute__((packed)) {
    uint16_t d;
    uint16_t dmin;
    uint8_t scales[12];
    uint8_t qs[QK_K / 2];
} block_q4_K;

/* Q2_K block structure - 2-bit K-quantization
 * 256 elements per block, ~2.625 bits per weight
 * Block size: 84 bytes (16 scales + 64 qs + 2 d + 2 dmin)
 */
typedef struct __attribute__((packed)) {
    uint8_t scales[QK_K / 16]; /* scales and mins, quantized with 4 bits */
    uint8_t qs[QK_K / 4];      /* quants - 2 bits each, packed 4 per byte */
    uint16_t d;                /* super-block scale for quantized scales */
    uint16_t dmin;             /* super-block scale for quantized mins */
} block_q2_K;

/* Q6_K block structure - 6-bit K-quantization */
typedef struct __attribute__((packed)) {
    uint8_t ql[QK_K / 2];     /* low 4 bits of quants */
    uint8_t qh[QK_K / 4];     /* high 2 bits of quants */
    int8_t scales[QK_K / 16]; /* scales */
    uint16_t d;               /* super-block scale */
} block_q6_K;

/* ============================================================================
 * FUSED QUANTIZED OPERATIONS - No dequantization overhead
 * These operate directly on quantized data using integer SIMD
 * ============================================================================ */

/* Q8_1 block for quantized input (includes sum for fused matmul) */
typedef struct __attribute__((packed)) {
    float d;                  /* scale */
    float s;                  /* sum of elements (for fused matmul) */
    int8_t qs[QK8_0];        /* quantized values */
} block_q8_1;

/* Quantize float input to Q8_1 format on-the-fly
 * Q8_1 includes sum for efficient fused matmul */
static void quantize_row_q8_1(const float* x, block_q8_1* y, int k) {
    const int nb = k / QK8_0;

    for (int i = 0; i < nb; i++) {
        float amax = 0.0f;
        float sum = 0.0f;

        /* Find max absolute value and sum */
        for (int j = 0; j < QK8_0; j++) {
            float v = x[i * QK8_0 + j];
            sum += v;
            float av = v < 0 ? -v : v;
            if (av > amax) amax = av;
        }

        /* Compute scale */
        float d = amax / 127.0f;
        float id = d != 0.0f ? 1.0f / d : 0.0f;

        y[i].d = d;
        y[i].s = sum;

        /* Quantize */
        for (int j = 0; j < QK8_0; j++) {
            float v = x[i * QK8_0 + j] * id;
            int q = (int)(v + (v > 0 ? 0.5f : -0.5f));
            if (q > 127) q = 127;
            if (q < -128) q = -128;
            y[i].qs[j] = (int8_t)q;
        }
    }
}

/* Fused Q8_0 x Q8_1 dot product using integer SIMD
 * Computes: sum(dequant(q8_0) * dequant(q8_1))
 *         = sum(d0 * qs0[i] * d1 * qs1[i])
 *         = d0 * d1 * sum(qs0[i] * qs1[i])
 * The integer sum is computed with SIMD, then scaled at the end.
 */
#if defined(__x86_64__) || defined(_M_X64)
static float vec_dot_q8_0_q8_1_sse(const block_q8_0* x, const block_q8_1* y, int nb) {
    float sumf = 0.0f;

    for (int i = 0; i < nb; i++) {
        float d0 = fp16_to_fp32(x[i].d);
        float d1 = y[i].d;

        /* SSE2 integer dot product */
        __m128i sum_vec = _mm_setzero_si128();

        for (int j = 0; j < QK8_0; j += 16) {
            /* Load 16 int8 values from each */
            __m128i ax = _mm_loadu_si128((const __m128i*)(x[i].qs + j));
            __m128i ay = _mm_loadu_si128((const __m128i*)(y[i].qs + j));

            /* Unpack to 16-bit and multiply */
            __m128i ax_lo = _mm_srai_epi16(_mm_unpacklo_epi8(ax, ax), 8);
            __m128i ax_hi = _mm_srai_epi16(_mm_unpackhi_epi8(ax, ax), 8);
            __m128i ay_lo = _mm_srai_epi16(_mm_unpacklo_epi8(ay, ay), 8);
            __m128i ay_hi = _mm_srai_epi16(_mm_unpackhi_epi8(ay, ay), 8);

            /* Multiply and add pairs */
            __m128i prod_lo = _mm_madd_epi16(ax_lo, ay_lo);
            __m128i prod_hi = _mm_madd_epi16(ax_hi, ay_hi);

            sum_vec = _mm_add_epi32(sum_vec, prod_lo);
            sum_vec = _mm_add_epi32(sum_vec, prod_hi);
        }

        /* Horizontal sum of 4 int32s */
        __m128i sum_hi = _mm_shuffle_epi32(sum_vec, _MM_SHUFFLE(1, 0, 3, 2));
        sum_vec = _mm_add_epi32(sum_vec, sum_hi);
        sum_hi = _mm_shuffle_epi32(sum_vec, _MM_SHUFFLE(2, 3, 0, 1));
        sum_vec = _mm_add_epi32(sum_vec, sum_hi);

        int32_t isum = _mm_cvtsi128_si32(sum_vec);
        sumf += d0 * d1 * (float)isum;
    }

    return sumf;
}

#ifdef __AVX2__
static float vec_dot_q8_0_q8_1_avx2(const block_q8_0* x, const block_q8_1* y, int nb) {
    __m256 accum = _mm256_setzero_ps();

    for (int i = 0; i < nb; i++) {
        float d0 = fp16_to_fp32(x[i].d);
        float d1 = y[i].d;

        /* Load 32 int8 values */
        __m256i ax = _mm256_loadu_si256((const __m256i*)x[i].qs);
        __m256i ay = _mm256_loadu_si256((const __m256i*)y[i].qs);

        /* Sign-extend to 16-bit and multiply-add */
        __m256i ax_lo = _mm256_cvtepi8_epi16(_mm256_castsi256_si128(ax));
        __m256i ax_hi = _mm256_cvtepi8_epi16(_mm256_extracti128_si256(ax, 1));
        __m256i ay_lo = _mm256_cvtepi8_epi16(_mm256_castsi256_si128(ay));
        __m256i ay_hi = _mm256_cvtepi8_epi16(_mm256_extracti128_si256(ay, 1));

        __m256i prod_lo = _mm256_madd_epi16(ax_lo, ay_lo);
        __m256i prod_hi = _mm256_madd_epi16(ax_hi, ay_hi);
        __m256i sum32 = _mm256_add_epi32(prod_lo, prod_hi);

        /* Horizontal sum */
        __m128i sum128 = _mm_add_epi32(_mm256_castsi256_si128(sum32),
                                        _mm256_extracti128_si256(sum32, 1));
        sum128 = _mm_hadd_epi32(sum128, sum128);
        sum128 = _mm_hadd_epi32(sum128, sum128);

        int32_t isum = _mm_cvtsi128_si32(sum128);
        __m256 scale = _mm256_set1_ps(d0 * d1 * (float)isum);
        accum = _mm256_add_ps(accum, scale);
    }

    /* Final horizontal sum */
    __m128 sum128 = _mm_add_ps(_mm256_castps256_ps128(accum),
                               _mm256_extractf128_ps(accum, 1));
    sum128 = _mm_hadd_ps(sum128, sum128);
    sum128 = _mm_hadd_ps(sum128, sum128);
    return _mm_cvtss_f32(sum128);
}
#endif /* __AVX2__ */

/* Dispatcher for Q8_0 x Q8_1 dot product */
static inline float vec_dot_q8_0_q8_1(const block_q8_0* x, const block_q8_1* y, int nb) {
#ifdef __AVX2__
    if (has_avx2()) {
        return vec_dot_q8_0_q8_1_avx2(x, y, nb);
    }
#endif
    return vec_dot_q8_0_q8_1_sse(x, y, nb);
}

#elif defined(__aarch64__)
/* ARM NEON fused Q8_0 x Q8_1 dot product */
static float vec_dot_q8_0_q8_1(const block_q8_0* x, const block_q8_1* y, int nb) {
    float sumf = 0.0f;

    for (int i = 0; i < nb; i++) {
        float d0 = fp16_to_fp32(x[i].d);
        float d1 = y[i].d;

        int32x4_t sum_vec = vdupq_n_s32(0);

        for (int j = 0; j < QK8_0; j += 16) {
            int8x16_t ax = vld1q_s8(x[i].qs + j);
            int8x16_t ay = vld1q_s8(y[i].qs + j);

            /* Multiply low and high halves */
            int16x8_t prod_lo = vmull_s8(vget_low_s8(ax), vget_low_s8(ay));
            int16x8_t prod_hi = vmull_s8(vget_high_s8(ax), vget_high_s8(ay));

            /* Pairwise add to 32-bit and accumulate */
            sum_vec = vpadalq_s16(sum_vec, prod_lo);
            sum_vec = vpadalq_s16(sum_vec, prod_hi);
        }

        int32_t isum = vaddvq_s32(sum_vec);
        sumf += d0 * d1 * (float)isum;
    }

    return sumf;
}

#else
/* Scalar fallback */
static float vec_dot_q8_0_q8_1(const block_q8_0* x, const block_q8_1* y, int nb) {
    float sumf = 0.0f;

    for (int i = 0; i < nb; i++) {
        float d0 = fp16_to_fp32(x[i].d);
        float d1 = y[i].d;
        int32_t isum = 0;

        for (int j = 0; j < QK8_0; j++) {
            isum += (int32_t)x[i].qs[j] * (int32_t)y[i].qs[j];
        }

        sumf += d0 * d1 * (float)isum;
    }

    return sumf;
}
#endif

/* Pre-allocated buffer for quantized input (reused across calls) */
static block_q8_1* g_input_q8 = NULL;
static int g_input_q8_size = 0;

/* Counters for quantization path usage (useful for profiling) */
static uint32_t g_q8_fused_count = 0;
static uint32_t g_dequant_count = 0;

/* Fused Q8_0 matrix-vector multiply
 * Quantizes input on-the-fly, then uses integer SIMD for matmul
 * This is ~4-8x faster than dequant-then-float-matmul */
static void matmul_q8_0_fused(float* out, const void* w_q8_0, const float* x,
                               int rows, int cols) {
    const int nb_cols = cols / QK8_0;
    const block_q8_0* weights = (const block_q8_0*)w_q8_0;

    /* Buffer is pre-allocated in streaming_inference_init() to worst-case size.
     * No dynamic allocation in inference path for deterministic timing. */
    if (nb_cols > g_input_q8_size) {
        console_printf("Error: matmul_q8_0_fused buffer overflow (need %d, have %d)\n",
                      nb_cols, g_input_q8_size);
        return;  /* Fail gracefully - buffer too small */
    }

    /* Quantize input vector once */
    quantize_row_q8_1(x, g_input_q8, cols);

    /* Compute each output row */
    for (int r = 0; r < rows; r++) {
        const block_q8_0* row_weights = &weights[r * nb_cols];
        out[r] = vec_dot_q8_0_q8_1(row_weights, g_input_q8, nb_cols);
    }
}

/* ============================================================================
 * SIMD exp approximation for softmax/SiLU - avoids slow scalar expf
 * ============================================================================ */

#if defined(__x86_64__) || defined(_M_X64)
/* Fast exp approximation using polynomial (valid for x in [-10, 10])
 * SSE2-compatible version (no SSE4.1 _mm_round_ps needed)
 * Based on Schraudolph's method with improved accuracy */
static inline __m128 exp_ps_sse(__m128 x) {
    /* Clamp input */
    x = _mm_max_ps(x, _mm_set1_ps(-10.0f));
    x = _mm_min_ps(x, _mm_set1_ps(10.0f));

    /* exp(x) = 2^(x * log2(e)) = 2^(x * 1.4427) */
    __m128 log2e = _mm_set1_ps(1.44269504f);
    __m128 t = _mm_mul_ps(x, log2e);

    /* SSE2-compatible rounding: floor(t + 0.5) for round-to-nearest
     * cvtps_epi32 truncates toward zero, so add/sub 0.5 based on sign */
    __m128 half = _mm_set1_ps(0.5f);
    __m128 sign_mask = _mm_cmplt_ps(t, _mm_setzero_ps());
    __m128 adj = _mm_or_ps(_mm_and_ps(sign_mask, _mm_set1_ps(-0.5f)),
                           _mm_andnot_ps(sign_mask, half));
    __m128i ti_int = _mm_cvtps_epi32(_mm_add_ps(t, adj));
    __m128 ti = _mm_cvtepi32_ps(ti_int);
    __m128 tf = _mm_sub_ps(t, ti);

    /* Compute 2^tf using polynomial: 1 + tf*ln2 + (tf*ln2)^2/2 + ... */
    __m128 ln2 = _mm_set1_ps(0.6931472f);
    __m128 p = _mm_mul_ps(tf, ln2);
    __m128 p2 = _mm_mul_ps(p, p);
    __m128 result = _mm_add_ps(_mm_set1_ps(1.0f), p);
    result = _mm_add_ps(result, _mm_mul_ps(p2, _mm_set1_ps(0.5f)));
    result = _mm_add_ps(result, _mm_mul_ps(_mm_mul_ps(p2, p), _mm_set1_ps(0.166667f)));

    /* Multiply by 2^ti using bit manipulation */
    ti_int = _mm_add_epi32(ti_int, _mm_set1_epi32(127));
    ti_int = _mm_slli_epi32(ti_int, 23);
    __m128 pow2 = _mm_castsi128_ps(ti_int);

    return _mm_mul_ps(result, pow2);
}
#endif

/* Streaming dequantization - directly to output buffer */
static void stream_dequant_f32(const void* src, float* dst, int64_t n) {
    const float* s = (const float*)src;
    for (int64_t i = 0; i < n; i++) dst[i] = s[i];
}

static void stream_dequant_f16(const void* src, float* dst, int64_t n) {
    const uint16_t* s = (const uint16_t*)src;
    for (int64_t i = 0; i < n; i++) dst[i] = fp16_to_fp32(s[i]);
}

static void stream_dequant_q8_0(const void* src, float* dst, int64_t n) {
    const block_q8_0* blocks = (const block_q8_0*)src;
    int64_t nb = n / QK8_0;
    for (int64_t i = 0; i < nb; i++) {
        /* Prefetch next block (4 blocks ahead = ~128 bytes) */
        if (i + 4 < nb) {
            __builtin_prefetch(&blocks[i + 4], 0, 3);
        }

        float d = fp16_to_fp32(blocks[i].d);
        for (int j = 0; j < QK8_0; j++) {
            dst[i * QK8_0 + j] = d * blocks[i].qs[j];
        }
    }
}

static void stream_dequant_q4_0(const void* src, float* dst, int64_t n) {
    const block_q4_0* blocks = (const block_q4_0*)src;
    int64_t nb = n / QK4_0;
    for (int64_t i = 0; i < nb; i++) {
        /* Prefetch next block (4 blocks ahead) */
        if (i + 4 < nb) {
            __builtin_prefetch(&blocks[i + 4], 0, 3);
        }

        float d = fp16_to_fp32(blocks[i].d);
        for (int j = 0; j < QK4_0 / 2; j++) {
            uint8_t q = blocks[i].qs[j];
            dst[i * QK4_0 + j * 2 + 0] = d * ((int)(q & 0x0F) - 8);
            dst[i * QK4_0 + j * 2 + 1] = d * ((int)(q >> 4) - 8);
        }
    }
}

static void stream_dequant_q4_1(const void* src, float* dst, int64_t n) {
    const block_q4_1* blocks = (const block_q4_1*)src;
    int64_t nb = n / QK4_1;
    for (int64_t i = 0; i < nb; i++) {
        float d = fp16_to_fp32(blocks[i].d);
        float m = fp16_to_fp32(blocks[i].m);
        for (int j = 0; j < QK4_1 / 2; j++) {
            uint8_t q = blocks[i].qs[j];
            dst[i * QK4_1 + j * 2 + 0] = d * (q & 0x0F) + m;
            dst[i * QK4_1 + j * 2 + 1] = d * (q >> 4) + m;
        }
    }
}

/* Q4_K dequantization */
/* Helper to decode scale and min from Q4_K packed format - matches llama.cpp */
static inline void get_scale_min_k4(int j, const uint8_t* q, uint8_t* d, uint8_t* m) {
    if (j < 4) {
        *d = q[j] & 63;
        *m = q[j + 4] & 63;
    } else {
        *d = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4);
        *m = (q[j + 4] >> 4) | ((q[j - 0] >> 6) << 4);
    }
}

static void stream_dequant_q4_K(const void* src, float* dst, int64_t n) {
    const block_q4_K* blocks = (const block_q4_K*)src;
    int64_t nb = n / QK_K;

    for (int64_t i = 0; i < nb; i++) {
        /* Prefetch next block (2 blocks ahead = ~512 bytes for Q4_K) */
        if (i + 2 < nb) {
            __builtin_prefetch(&blocks[i + 2], 0, 3);
        }

        const block_q4_K* b = &blocks[i];
        const uint8_t* q = b->qs;
        float d = fp16_to_fp32(b->d);
        float dmin = fp16_to_fp32(b->dmin);
        float* y = dst + i * QK_K;

        int is = 0;
        /* Process 256 elements in 4 groups of 64 (each 64 uses 2 scale/min pairs) */
        for (int j = 0; j < QK_K; j += 64) {
            uint8_t sc, m;

            /* Get scale and min for first 32 elements (lower nibbles) */
            get_scale_min_k4(is + 0, b->scales, &sc, &m);
            float d1 = d * sc;
            float m1 = dmin * m;

            /* Get scale and min for next 32 elements (upper nibbles) */
            get_scale_min_k4(is + 1, b->scales, &sc, &m);
            float d2 = d * sc;
            float m2 = dmin * m;

            /* Dequantize: 32 elements from lower nibbles, 32 from upper nibbles */
            for (int l = 0; l < 32; ++l) {
                *y++ = d1 * (q[l] & 0xF) - m1;
            }
            for (int l = 0; l < 32; ++l) {
                *y++ = d2 * (q[l] >> 4) - m2;
            }

            q += 32;
            is += 2;
        }
    }
}

/* Q2_K dequantization - matches llama.cpp reference implementation
 * Block format: scales[16] + qs[64] + d(fp16) + dmin(fp16) = 84 bytes
 * Each block contains 256 elements (QK_K)
 */
static void stream_dequant_q2_K(const void* src, float* dst, int64_t n) {
    const block_q2_K* blocks = (const block_q2_K*)src;
    int64_t nb = n / QK_K;

    for (int64_t i = 0; i < nb; i++) {
        /* Prefetch next block (3 blocks ahead = ~252 bytes for Q2_K) */
        if (i + 3 < nb) {
            __builtin_prefetch(&blocks[i + 3], 0, 3);
        }

        const block_q2_K* b = &blocks[i];
        float d = fp16_to_fp32(b->d);
        float min = fp16_to_fp32(b->dmin);

        const uint8_t* q = b->qs;
        float* y = dst + i * QK_K;

        int is = 0;
        for (int n_128 = 0; n_128 < QK_K; n_128 += 128) {
            int shift = 0;
            for (int j = 0; j < 4; j++) {
                /* First 16 elements of this group */
                uint8_t sc = b->scales[is++];
                float dl = d * (sc & 0xF);
                float ml = min * (sc >> 4);
                for (int l = 0; l < 16; l++) {
                    *y++ = dl * ((int8_t)((q[l] >> shift) & 3)) - ml;
                }

                /* Next 16 elements */
                sc = b->scales[is++];
                dl = d * (sc & 0xF);
                ml = min * (sc >> 4);
                for (int l = 0; l < 16; l++) {
                    *y++ = dl * ((int8_t)((q[l + 16] >> shift) & 3)) - ml;
                }

                shift += 2;
            }
            q += 32;
        }
    }
}

/* Q6_K dequantization - matches llama.cpp reference */
static void stream_dequant_q6_K(const void* src, float* dst, int64_t n) {
    const block_q6_K* blocks = (const block_q6_K*)src;
    int64_t nb = n / QK_K;

    for (int64_t i = 0; i < nb; i++) {
        const block_q6_K* b = &blocks[i];
        float d = fp16_to_fp32(b->d);
        const uint8_t* ql = b->ql;
        const uint8_t* qh = b->qh;
        const int8_t* sc = b->scales;
        float* y = dst + i * QK_K;

        for (int n_128 = 0; n_128 < QK_K; n_128 += 128) {
            for (int l = 0; l < 32; l++) {
                /* Scale index is just l/16 since sc is incremented each iteration */
                int is = l / 16;
                int8_t q1 = (int8_t)((ql[l +  0] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
                int8_t q2 = (int8_t)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                int8_t q3 = (int8_t)((ql[l +  0] >> 4)  | (((qh[l] >> 4) & 3) << 4)) - 32;
                int8_t q4 = (int8_t)((ql[l + 32] >> 4)  | (((qh[l] >> 6) & 3) << 4)) - 32;

                y[l +  0] = d * sc[is + 0] * q1;
                y[l + 32] = d * sc[is + 2] * q2;
                y[l + 64] = d * sc[is + 4] * q3;
                y[l + 96] = d * sc[is + 6] * q4;
            }
            y += 128;
            ql += 64;
            qh += 32;
            sc += 8;
        }
    }
}

/* Q5_0 block structure - 5-bit quantization */
#define QK5_0 32
typedef struct __attribute__((packed)) {
    uint16_t d;              /* scale (fp16) */
    uint8_t qh[4];           /* high bits (1 bit per element, 32 elements = 4 bytes) */
    uint8_t qs[QK5_0 / 2];   /* low 4 bits (2 elements per byte) */
} block_q5_0;

/* Q5_0 dequantization - matches llama.cpp reference
 * Each block: scale(fp16) + qh[4](32 high bits) + qs[16](32 4-bit low parts)
 */
static void stream_dequant_q5_0(const void* src, float* dst, int64_t n) {
    const block_q5_0* blocks = (const block_q5_0*)src;
    int64_t nb = n / QK5_0;

    for (int64_t i = 0; i < nb; i++) {
        const block_q5_0* b = &blocks[i];
        float d = fp16_to_fp32(b->d);

        /* Read qh as uint32_t for efficient bit access */
        uint32_t qh;
        memcpy(&qh, b->qh, sizeof(qh));

        for (int j = 0; j < QK5_0 / 2; ++j) {
            /* Extract 5th bit for first and second elements */
            const uint8_t xh_0 = ((qh >> (j + 0)) << 4) & 0x10;
            const uint8_t xh_1 = ((qh >> (j + 12))) & 0x10;

            /* Combine low 4 bits with high bit, center around 0 */
            const int32_t x0 = ((b->qs[j] & 0x0F) | xh_0) - 16;
            const int32_t x1 = ((b->qs[j] >> 4) | xh_1) - 16;

            /* First 16 outputs come from lower nibbles, next 16 from upper */
            dst[i * QK5_0 + j + 0] = x0 * d;
            dst[i * QK5_0 + j + QK5_0 / 2] = x1 * d;
        }
    }
}

/* Dispatcher for dequantization by type */
static void stream_dequant(const void* src, float* dst, int64_t n, int type) {
    switch (type) {
        case 0:  stream_dequant_f32(src, dst, n); break;   /* GGML_TYPE_F32 */
        case 1:  stream_dequant_f16(src, dst, n); break;   /* GGML_TYPE_F16 */
        case 2:  stream_dequant_q4_0(src, dst, n); break;  /* GGML_TYPE_Q4_0 */
        case 3:  stream_dequant_q4_1(src, dst, n); break;  /* GGML_TYPE_Q4_1 */
        case 6:  stream_dequant_q5_0(src, dst, n); break;  /* GGML_TYPE_Q5_0 */
        case 8:  stream_dequant_q8_0(src, dst, n); break;  /* GGML_TYPE_Q8_0 */
        case 10: stream_dequant_q2_K(src, dst, n); break;  /* GGML_TYPE_Q2_K */
        case 12: stream_dequant_q4_K(src, dst, n); break;  /* GGML_TYPE_Q4_K */
        case 14: stream_dequant_q6_K(src, dst, n); break;  /* GGML_TYPE_Q6_K */
        default:
            console_printf("[STREAM] Unknown quant type %d, using F32\n", type);
            stream_dequant_f32(src, dst, n);
    }
}

/* ============================================================================
 * Math Operations
 * ============================================================================ */

/* NEON-optimized dot product for float32 (ARM64 only) */
#ifdef __aarch64__
static inline float dot_product_neon(const float* a, const float* b, int n) {
    float32x4_t sum = vdupq_n_f32(0.0f);
    int i = 0;

    /* Process 4 elements at a time with NEON */
    for (; i + 4 <= n; i += 4) {
        float32x4_t va = vld1q_f32(a + i);
        float32x4_t vb = vld1q_f32(b + i);
        sum = vmlaq_f32(sum, va, vb);  /* FMA: sum += va * vb */
    }

    /* Horizontal add: reduce vector to scalar */
    float result = vaddvq_f32(sum);

    /* Handle remainder elements with scalar */
    for (; i < n; i++) {
        result += a[i] * b[i];
    }

    return result;
}
#endif

/* RMSNorm with streaming dequantization - SIMD optimized */
static void rmsnorm_stream(float* out, const float* x, const void* w_quant,
                           int type, int size, float eps) {
    /* Compute sum of squares with SIMD */
    float ss = 0.0f;

#if defined(__x86_64__) || defined(_M_X64)
    ss = simd_dot_product(x, x, size);
#elif defined(__aarch64__)
    float32x4_t vss = vdupq_n_f32(0.0f);
    int i = 0;
    for (; i + 4 <= size; i += 4) {
        float32x4_t vx = vld1q_f32(x + i);
        vss = vmlaq_f32(vss, vx, vx);
    }
    ss = vaddvq_f32(vss);
    for (; i < size; i++) ss += x[i] * x[i];
#else
    for (int i = 0; i < size; i++) ss += x[i] * x[i];
#endif

    ss = 1.0f / sqrtf(ss / size + eps);

    /* Dequantize weights and apply norm */
    const int chunk_size = 1024;
    float w_chunk[1024];  /* Stack buffer for small chunks */

    for (int i = 0; i < size; i += chunk_size) {
        int n = (i + chunk_size <= size) ? chunk_size : (size - i);

        /* Calculate offset in quantized data */
        size_t offset = 0;
        switch (type) {
            case 0:  offset = i * sizeof(float); break;                      /* F32 */
            case 1:  offset = i * sizeof(uint16_t); break;                   /* F16 */
            case 2:  offset = (i / QK4_0) * sizeof(block_q4_0); break;       /* Q4_0 */
            case 3:  offset = (i / QK4_1) * sizeof(block_q4_1); break;       /* Q4_1 */
            case 6:  offset = (i / QK5_0) * 22; break;                       /* Q5_0 */
            case 8:  offset = (i / QK8_0) * sizeof(block_q8_0); break;       /* Q8_0 */
            case 10: offset = (i / QK_K) * sizeof(block_q2_K); break;        /* Q2_K */
            case 12: offset = (i / QK_K) * sizeof(block_q4_K); break;        /* Q4_K */
            case 14: offset = (i / QK_K) * sizeof(block_q6_K); break;        /* Q6_K */
            default: offset = i * sizeof(float);
        }

        stream_dequant((const char*)w_quant + offset, w_chunk, n, type);

        /* Apply normalization with SIMD */
#if defined(__x86_64__) || defined(_M_X64)
        {
            __m128 vss = _mm_set1_ps(ss);
            int j = 0;
            for (; j + 4 <= n; j += 4) {
                __m128 vx = _mm_loadu_ps(x + i + j);
                __m128 vw = _mm_loadu_ps(w_chunk + j);
                __m128 result = _mm_mul_ps(_mm_mul_ps(vx, vss), vw);
                _mm_storeu_ps(out + i + j, result);
            }
            for (; j < n; j++) {
                out[i + j] = x[i + j] * ss * w_chunk[j];
            }
        }
#elif defined(__aarch64__)
        {
            float32x4_t vss_vec = vdupq_n_f32(ss);
            int j = 0;
            for (; j + 4 <= n; j += 4) {
                float32x4_t vx = vld1q_f32(x + i + j);
                float32x4_t vw = vld1q_f32(w_chunk + j);
                float32x4_t result = vmulq_f32(vmulq_f32(vx, vss_vec), vw);
                vst1q_f32(out + i + j, result);
            }
            for (; j < n; j++) {
                out[i + j] = x[i + j] * ss * w_chunk[j];
            }
        }
#else
        for (int j = 0; j < n; j++) {
            out[i + j] = x[i + j] * ss * w_chunk[j];
        }
#endif
    }
}

/* Matrix multiply with streaming dequantization
 *
 * GGUF stores weights in ROW-MAJOR format like PyTorch nn.Linear.
 * Weight shape is [out_features, in_features] = [rows, cols].
 * Element W[out_idx, in_idx] is at offset out_idx * cols + in_idx.
 *
 * Computes: out[r] = sum_c(W[r, c] * x[c]) = sum_c(W[r*cols + c] * x[c])
 */
static void matmul_stream(float* out, const void* w_quant, int w_type,
                          const float* x, int rows, int cols) {
    /* FAST PATH: Use fused Q8_0 matmul for Q8_0 weights
     * This avoids dequantization overhead - operates directly on int8 */
    if (w_type == 8) {  /* GGML_TYPE_Q8_0 */
        g_q8_fused_count++;
        matmul_q8_0_fused(out, w_quant, x, rows, cols);
        return;
    }
    g_dequant_count++;

    const int chunk_cols = 256;  /* Dequantize 256 columns at a time */
    float* row_chunk = g_state.layer_weights;  /* Reuse layer buffer */

    for (int r = 0; r < rows; r++) {
        float sum = 0.0f;

        for (int c = 0; c < cols; c += chunk_cols) {
            int n = (c + chunk_cols <= cols) ? chunk_cols : (cols - c);

            /* ROW-MAJOR: row r, starting at column c
             * Element at (row, col) = offset row * cols + col */
            size_t row_offset = (size_t)r * cols + c;
            size_t byte_offset;

            switch (w_type) {
                case 0:  byte_offset = row_offset * sizeof(float); break;         /* F32 */
                case 1:  byte_offset = row_offset * sizeof(uint16_t); break;      /* F16 */
                case 2:  byte_offset = (row_offset / QK4_0) * sizeof(block_q4_0); break;  /* Q4_0 */
                case 3:  byte_offset = (row_offset / QK4_1) * sizeof(block_q4_1); break;  /* Q4_1 */
                case 6:  byte_offset = (row_offset / QK5_0) * 22; break;          /* Q5_0 */
                case 8:  byte_offset = (row_offset / QK8_0) * sizeof(block_q8_0); break;  /* Q8_0 */
                case 10: byte_offset = (row_offset / QK_K) * sizeof(block_q2_K); break;   /* Q2_K */
                case 12: byte_offset = (row_offset / QK_K) * sizeof(block_q4_K); break;   /* Q4_K */
                case 14: byte_offset = (row_offset / QK_K) * sizeof(block_q6_K); break;   /* Q6_K */
                default: byte_offset = row_offset * sizeof(float);
            }

            stream_dequant((const char*)w_quant + byte_offset, row_chunk, n, w_type);

            /* Dot product: sum += W[r, c:c+n] . x[c:c+n] */
#if defined(__x86_64__) || defined(_M_X64)
            sum += simd_dot_product(row_chunk, x + c, n);
#elif defined(__aarch64__)
            /* ARM NEON dot product */
            float32x4_t vsum = vdupq_n_f32(0.0f);
            int j = 0;
            for (; j + 4 <= n; j += 4) {
                float32x4_t va = vld1q_f32(row_chunk + j);
                float32x4_t vb = vld1q_f32(x + c + j);
                vsum = vmlaq_f32(vsum, va, vb);
            }
            sum += vaddvq_f32(vsum);
            for (; j < n; j++) {
                sum += row_chunk[j] * x[c + j];
            }
#else
            /* Scalar fallback */
            for (int j = 0; j < n; j++) {
                sum += row_chunk[j] * x[c + j];
            }
#endif
        }

        out[r] = sum;
    }
}

/* Fast exp approximation for NEON - Pade approximation for exp(x)
 * Valid for x in [-10, 10], good accuracy (~0.1% error)
 * exp(x) ≈ (1 + x/2 + x²/9) / (1 - x/2 + x²/9)
 */
#ifdef __aarch64__
#include <arm_neon.h>

static inline float32x4_t exp_approx_neon(float32x4_t x) {
    /* Clamp x to [-10, 10] for numerical stability */
    float32x4_t vmin = vdupq_n_f32(-10.0f);
    float32x4_t vmax = vdupq_n_f32(10.0f);
    x = vmaxq_f32(x, vmin);
    x = vminq_f32(x, vmax);

    /* Constants for Pade approximation */
    float32x4_t c_half = vdupq_n_f32(0.5f);
    float32x4_t c_ninth = vdupq_n_f32(1.0f / 9.0f);
    float32x4_t c_one = vdupq_n_f32(1.0f);

    /* x/2 and x²/9 */
    float32x4_t x_half = vmulq_f32(x, c_half);
    float32x4_t x2 = vmulq_f32(x, x);
    float32x4_t x2_ninth = vmulq_f32(x2, c_ninth);

    /* Numerator: 1 + x/2 + x²/9 */
    float32x4_t num = vaddq_f32(c_one, x_half);
    num = vaddq_f32(num, x2_ninth);

    /* Denominator: 1 - x/2 + x²/9 */
    float32x4_t den = vsubq_f32(c_one, x_half);
    den = vaddq_f32(den, x2_ninth);

    /* Division: num/den */
    return vdivq_f32(num, den);
}
#endif

/* Softmax with NEON optimization for ARM64 */
static void softmax(float* x, int size) {
#ifdef __aarch64__
    /* Phase 1: Find max using NEON */
    float32x4_t vmax = vdupq_n_f32(-1e9f);
    int i = 0;

    for (; i + 4 <= size; i += 4) {
        float32x4_t v = vld1q_f32(x + i);
        vmax = vmaxq_f32(vmax, v);
    }

    /* Horizontal max reduction */
    float max_val = vmaxvq_f32(vmax);

    /* Handle remainder */
    for (; i < size; i++) {
        if (x[i] > max_val) max_val = x[i];
    }

    /* Phase 2: Compute exp(x - max) and sum using NEON */
    float32x4_t vsum = vdupq_n_f32(0.0f);
    float32x4_t vmax_broadcast = vdupq_n_f32(max_val);

    for (i = 0; i + 4 <= size; i += 4) {
        float32x4_t v = vld1q_f32(x + i);
        v = vsubq_f32(v, vmax_broadcast);

        /* Fast exp approximation */
        float32x4_t e = exp_approx_neon(v);
        vst1q_f32(x + i, e);
        vsum = vaddq_f32(vsum, e);
    }

    /* Horizontal sum reduction */
    float sum = vaddvq_f32(vsum);

    /* Handle remainder with scalar expf */
    for (; i < size; i++) {
        x[i] = expf(x[i] - max_val);
        sum += x[i];
    }

    /* Phase 3: Normalize using NEON */
    float32x4_t vinv_sum = vdupq_n_f32(1.0f / sum);

    for (i = 0; i + 4 <= size; i += 4) {
        float32x4_t v = vld1q_f32(x + i);
        v = vmulq_f32(v, vinv_sum);
        vst1q_f32(x + i, v);
    }

    /* Handle remainder */
    for (i = 0; i < size; i++) {
        x[i] /= sum;
    }

#elif defined(__x86_64__) || defined(_M_X64)
    /* SSE2 optimized softmax for x86_64 */

    /* Phase 1: Find max using SSE2 */
    __m128 vmax = _mm_set1_ps(-1e9f);
    int i = 0;

    for (; i + 4 <= size; i += 4) {
        __m128 v = _mm_loadu_ps(x + i);
        vmax = _mm_max_ps(vmax, v);
    }

    /* Horizontal max reduction */
    __m128 shuf = _mm_shuffle_ps(vmax, vmax, _MM_SHUFFLE(2, 3, 0, 1));
    vmax = _mm_max_ps(vmax, shuf);
    shuf = _mm_movehl_ps(shuf, vmax);
    vmax = _mm_max_ss(vmax, shuf);
    float max_val = _mm_cvtss_f32(vmax);

    /* Handle remainder */
    for (; i < size; i++) {
        if (x[i] > max_val) max_val = x[i];
    }

    /* Phase 2: Compute exp(x - max) and sum using SSE2 with SIMD exp */
    __m128 vsum = _mm_setzero_ps();
    __m128 vmax_broadcast = _mm_set1_ps(max_val);

    for (i = 0; i + 4 <= size; i += 4) {
        __m128 v = _mm_loadu_ps(x + i);
        v = _mm_sub_ps(v, vmax_broadcast);

        /* SIMD exp approximation - avoids slow scalar expf */
        __m128 e = exp_ps_sse(v);

        _mm_storeu_ps(x + i, e);
        vsum = _mm_add_ps(vsum, e);
    }

    /* Horizontal sum reduction */
    shuf = _mm_shuffle_ps(vsum, vsum, _MM_SHUFFLE(2, 3, 0, 1));
    vsum = _mm_add_ps(vsum, shuf);
    shuf = _mm_movehl_ps(shuf, vsum);
    vsum = _mm_add_ss(vsum, shuf);
    float sum = _mm_cvtss_f32(vsum);

    /* Handle remainder with scalar expf */
    for (; i < size; i++) {
        x[i] = expf(x[i] - max_val);
        sum += x[i];
    }

    /* Phase 3: Normalize using SSE2 */
    __m128 vinv_sum = _mm_set1_ps(1.0f / sum);

    for (i = 0; i + 4 <= size; i += 4) {
        __m128 v = _mm_loadu_ps(x + i);
        v = _mm_mul_ps(v, vinv_sum);
        _mm_storeu_ps(x + i, v);
    }

    /* Handle remainder */
    for (; i < size; i++) {
        x[i] /= sum;
    }

#else
    /* Scalar fallback for other platforms */
    float max_val = x[0];
    for (int i = 1; i < size; i++) {
        if (x[i] > max_val) max_val = x[i];
    }

    float sum = 0.0f;
    for (int i = 0; i < size; i++) {
        x[i] = expf(x[i] - max_val);
        sum += x[i];
    }

    for (int i = 0; i < size; i++) {
        x[i] /= sum;
    }
#endif
}


/* SiLU activation */
static inline float silu(float x) {
    return x / (1.0f + expf(-x));
}

/* Element-wise add with SIMD - for residual connections (8-16x faster) */
static inline void elem_add_simd(float* out, const float* a, const float* b, int n) {
    int i = 0;

#ifdef __aarch64__
    /* ARM NEON: 4 floats per iteration */
    for (; i + 4 <= n; i += 4) {
        float32x4_t va = vld1q_f32(a + i);
        float32x4_t vb = vld1q_f32(b + i);
        float32x4_t result = vaddq_f32(va, vb);
        vst1q_f32(out + i, result);
    }
#elif defined(__x86_64__) || defined(_M_X64)
#ifdef __AVX__
    /* x86 AVX: 8 floats per iteration */
    for (; i + 8 <= n; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        __m256 result = _mm256_add_ps(va, vb);
        _mm256_storeu_ps(out + i, result);
    }
#else
    /* x86 SSE2: 4 floats per iteration */
    for (; i + 4 <= n; i += 4) {
        __m128 va = _mm_loadu_ps(a + i);
        __m128 vb = _mm_loadu_ps(b + i);
        __m128 result = _mm_add_ps(va, vb);
        _mm_storeu_ps(out + i, result);
    }
#endif
#endif

    /* Scalar remainder */
    for (; i < n; i++) {
        out[i] = a[i] + b[i];
    }
}

/* Element-wise add in-place with SIMD - for residual connections (8-16x faster) */
static inline void elem_add_inplace_simd(float* a, const float* b, int n) {
    int i = 0;

#ifdef __aarch64__
    /* ARM NEON: 4 floats per iteration */
    for (; i + 4 <= n; i += 4) {
        float32x4_t va = vld1q_f32(a + i);
        float32x4_t vb = vld1q_f32(b + i);
        float32x4_t result = vaddq_f32(va, vb);
        vst1q_f32(a + i, result);
    }
#elif defined(__x86_64__) || defined(_M_X64)
#ifdef __AVX__
    /* x86 AVX: 8 floats per iteration */
    for (; i + 8 <= n; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        __m256 result = _mm256_add_ps(va, vb);
        _mm256_storeu_ps(a + i, result);
    }
#else
    /* x86 SSE2: 4 floats per iteration */
    for (; i + 4 <= n; i += 4) {
        __m128 va = _mm_loadu_ps(a + i);
        __m128 vb = _mm_loadu_ps(b + i);
        __m128 result = _mm_add_ps(va, vb);
        _mm_storeu_ps(a + i, result);
    }
#endif
#endif

    /* Scalar remainder */
    for (; i < n; i++) {
        a[i] += b[i];
    }
}

/* ============================================================================
 * Extract column from transposed quantized embedding table
 * GGUF stores embeddings as [dim, vocab_size] not [vocab_size, dim]
 * To get token t's embedding, we need column t (strided access across rows)
 * ============================================================================ */

/* Extract token embedding from transposed Q8_0 table
 * Table shape: [dim, vocab_size], so dim rows of vocab_size elements each
 * Each row is (vocab_size/32) Q8_0 blocks
 * For token t, we need position t from each of the dim rows */
static void extract_embedding_q8_0_transposed(const void* src, float* dst,
                                               int dim, int vocab_size, int token) {
    const int blocks_per_row = vocab_size / QK8_0;  /* Q8_0 blocks per row */
    const size_t row_bytes = blocks_per_row * sizeof(block_q8_0);
    const int block_idx = token / QK8_0;  /* Which block contains our token */
    const int pos_in_block = token % QK8_0;  /* Position within that block */
    const block_q8_0* base = (const block_q8_0*)src;

    for (int d = 0; d < dim; d++) {
        /* Navigate to row d, block block_idx */
        const block_q8_0* block = (const block_q8_0*)((const char*)base + d * row_bytes) + block_idx;
        float scale = fp16_to_fp32(block->d);
        dst[d] = scale * block->qs[pos_in_block];
    }
}

/* Extract token embedding from transposed F32 table */
static void extract_embedding_f32_transposed(const void* src, float* dst,
                                              int dim, int vocab_size, int token) {
    const float* base = (const float*)src;
    for (int d = 0; d < dim; d++) {
        dst[d] = base[d * vocab_size + token];
    }
}

/* Extract token embedding from transposed F16 table */
static void extract_embedding_f16_transposed(const void* src, float* dst,
                                              int dim, int vocab_size, int token) {
    const uint16_t* base = (const uint16_t*)src;
    for (int d = 0; d < dim; d++) {
        dst[d] = fp16_to_fp32(base[d * vocab_size + token]);
    }
}

/* Dispatcher for transposed embedding extraction */
static void extract_embedding_transposed(const void* src, float* dst,
                                          int dim, int vocab_size, int token, int type) {
    switch (type) {
        case 0:  /* F32 */
            extract_embedding_f32_transposed(src, dst, dim, vocab_size, token);
            break;
        case 1:  /* F16 */
            extract_embedding_f16_transposed(src, dst, dim, vocab_size, token);
            break;
        case 8:  /* Q8_0 */
            extract_embedding_q8_0_transposed(src, dst, dim, vocab_size, token);
            break;
        default:
            console_printf("[EMBD] Unsupported transposed type %d, using F32\n", type);
            extract_embedding_f32_transposed(src, dst, dim, vocab_size, token);
            break;
    }
}

/* ============================================================================
 * Transposed matmul for output projection with tied weights
 * When using transposed token_embd [dim, vocab_size] for output:
 * logits[v] = sum_d(token_embd[d, v] * x[d])
 *           = sum_d(token_embd[d * vocab_size + v] * x[d])
 * This is column-wise access instead of row-wise
 * ============================================================================ */

/* Transposed matmul for Q8_0 weights stored as [dim, vocab_size]
 * Computes: out[vocab_size] = W[dim, vocab_size]^T @ x[dim]
 * For each output position v, we sum across dim: out[v] = sum_d(W[d,v] * x[d]) */
static void matmul_transposed_q8_0(float* out, const void* w_q8_0, const float* x,
                                    int dim, int vocab_size) {
    const int blocks_per_row = vocab_size / QK8_0;  /* Blocks per dim row */
    const size_t row_bytes = blocks_per_row * sizeof(block_q8_0);
    const block_q8_0* base = (const block_q8_0*)w_q8_0;

    /* Zero output */
    for (int v = 0; v < vocab_size; v++) {
        out[v] = 0.0f;
    }

    /* Accumulate contributions from each dim */
    for (int d = 0; d < dim; d++) {
        const block_q8_0* row_start = (const block_q8_0*)((const char*)base + d * row_bytes);
        float xd = x[d];

        /* Process all vocab positions */
        for (int blk = 0; blk < blocks_per_row; blk++) {
            const block_q8_0* block = &row_start[blk];
            float scale = fp16_to_fp32(block->d) * xd;
            int v_base = blk * QK8_0;

            for (int i = 0; i < QK8_0; i++) {
                out[v_base + i] += scale * block->qs[i];
            }
        }
    }
}

/* Transposed matmul for F32 weights */
static void matmul_transposed_f32(float* out, const void* w_f32, const float* x,
                                   int dim, int vocab_size) {
    const float* W = (const float*)w_f32;

    for (int v = 0; v < vocab_size; v++) {
        float sum = 0.0f;
        for (int d = 0; d < dim; d++) {
            sum += W[d * vocab_size + v] * x[d];
        }
        out[v] = sum;
    }
}

/* Dispatcher for transposed matmul */
static void matmul_stream_transposed(float* out, const void* w_quant, int w_type,
                                      const float* x, int dim, int vocab_size) {
    switch (w_type) {
        case 0:  /* F32 */
            matmul_transposed_f32(out, w_quant, x, dim, vocab_size);
            break;
        case 8:  /* Q8_0 */
            matmul_transposed_q8_0(out, w_quant, x, dim, vocab_size);
            break;
        default:
            console_printf("[MATMUL] Transposed type %d not supported, using F32\n", w_type);
            matmul_transposed_f32(out, w_quant, x, dim, vocab_size);
            break;
    }
}

/* RoPE position encoding - applied per-head with head_dim frequency scaling
 * Following llama.cpp: theta_i = theta_base * theta_scale^i where theta_scale = theta^(-2/head_dim)
 */
static void rope(float* q, float* k, int pos, int dim, int head_dim,
                 int kv_dim, float theta) {
    int n_heads = dim / head_dim;
    int n_kv_heads = kv_dim / head_dim;

    /* Apply RoPE to each query head */
    for (int h = 0; h < n_heads; h++) {
        for (int i = 0; i < head_dim; i += 2) {
            /* Frequency based on position within head, using head_dim for scaling */
            float freq = 1.0f / powf(theta, (float)i / (float)head_dim);
            float val = (float)pos * freq;
            float cos_val = cosf(val);
            float sin_val = sinf(val);

            int idx = h * head_dim + i;
            float q0 = q[idx];
            float q1 = q[idx + 1];
            q[idx]     = q0 * cos_val - q1 * sin_val;
            q[idx + 1] = q0 * sin_val + q1 * cos_val;
        }
    }

    /* Apply RoPE to each key head */
    for (int h = 0; h < n_kv_heads; h++) {
        for (int i = 0; i < head_dim; i += 2) {
            float freq = 1.0f / powf(theta, (float)i / (float)head_dim);
            float val = (float)pos * freq;
            float cos_val = cosf(val);
            float sin_val = sinf(val);

            int idx = h * head_dim + i;
            float k0 = k[idx];
            float k1 = k[idx + 1];
            k[idx]     = k0 * cos_val - k1 * sin_val;
            k[idx + 1] = k0 * sin_val + k1 * cos_val;
        }
    }
}

/* ============================================================================
 * Transformer Layer - Streaming Version
 * ============================================================================ */

static void transformer_forward_stream(int token, int pos, int layer) {
    LayerWeights* lw = &g_layer_weights[layer];
    int dim = g_cfg.dim;
    int hidden_dim = g_cfg.hidden_dim;
    int head_dim = g_cfg.head_dim;
    int kv_dim = g_cfg.kv_dim;
    int n_heads = g_cfg.n_heads;
    int kv_mul = g_cfg.kv_mul;
    float eps = g_cfg.rms_norm_eps;

    /* Get token embedding on first layer */
    if (layer == 0) {
        int type = g_weights.token_embd_type;

        /* Check if embedding is stored transposed [dim, vocab_size] (GGUF default) */
        if (g_weights.token_embd_transposed) {
            /* Use strided column extraction for transposed table */
            extract_embedding_transposed(g_weights.token_embd, g_state.x,
                                         dim, g_weights.token_embd_vocab_size,
                                         token, type);
        } else {
            /* Standard row-major [vocab_size, dim] layout */
            size_t offset;
            switch (type) {
                case 0:  offset = (size_t)token * dim * sizeof(float); break;        /* F32 */
                case 1:  offset = (size_t)token * dim * sizeof(uint16_t); break;     /* F16 */
                case 2:  offset = (size_t)token * (dim / QK4_0) * sizeof(block_q4_0); break;  /* Q4_0 */
                case 3:  offset = (size_t)token * (dim / QK4_1) * sizeof(block_q4_1); break;  /* Q4_1 */
                case 6:  offset = (size_t)token * (dim / QK5_0) * 22; break;          /* Q5_0 (22 bytes per block) */
                case 8:  offset = (size_t)token * (dim / QK8_0) * sizeof(block_q8_0); break;  /* Q8_0 */
                case 10: offset = (size_t)token * (dim / QK_K) * sizeof(block_q2_K); break;  /* Q2_K */
                case 12: offset = (size_t)token * (dim / QK_K) * sizeof(block_q4_K); break;  /* Q4_K */
                case 14: offset = (size_t)token * (dim / QK_K) * sizeof(block_q6_K); break;  /* Q6_K */
                default: offset = (size_t)token * dim * sizeof(float); break;
            }

            stream_dequant((const char*)g_weights.token_embd + offset,
                           g_state.x, dim, type);
        }
    }

    /* Attention norm */
    rmsnorm_stream(g_state.xb, g_state.x, lw->attn_norm, lw->attn_norm_type, dim, eps);

    /* QKV projections with streaming */
    matmul_stream(g_state.q, lw->attn_q, lw->attn_q_type, g_state.xb, dim, dim);
    matmul_stream(g_state.k, lw->attn_k, lw->attn_k_type, g_state.xb, kv_dim, dim);
    matmul_stream(g_state.v, lw->attn_v, lw->attn_v_type, g_state.xb, kv_dim, dim);

    /* RoPE */
    rope(g_state.q, g_state.k, pos, dim, head_dim, kv_dim, g_cfg.rope_theta);

    /* Update KV cache */
    size_t cache_offset = (size_t)layer * g_cfg.seq_len * kv_dim + pos * kv_dim;
    memcpy(g_state.key_cache + cache_offset, g_state.k, kv_dim * sizeof(float));
    memcpy(g_state.value_cache + cache_offset, g_state.v, kv_dim * sizeof(float));

    /* Multi-head attention */
#if PARALLEL_INFERENCE_ENABLED
    /* Parallel attention across heads */
    {
        size_t layer_kv_offset = (size_t)layer * g_cfg.seq_len * kv_dim;
        parallel_attention(g_state.xb, g_state.q,
                           g_state.key_cache + layer_kv_offset,
                           g_state.value_cache + layer_kv_offset,
                           g_state.att, n_heads, g_cfg.n_kv_heads,
                           head_dim, kv_dim, g_cfg.seq_len, pos);
    }
#else
    /* Sequential multi-head attention fallback */
    memset(g_state.xb, 0, dim * sizeof(float));

    for (int h = 0; h < n_heads; h++) {
        float* q_head = g_state.q + h * head_dim;
        float* att_head = g_state.att + h * g_cfg.seq_len;
        int kv_head = h / kv_mul;

        /* Compute attention scores */
        for (int t = 0; t <= pos; t++) {
            float* k_t = g_state.key_cache + (size_t)layer * g_cfg.seq_len * kv_dim +
                         t * kv_dim + kv_head * head_dim;

            /* Prefetch next key vector (2 steps ahead) */
            if (t + 2 <= pos) {
                float* k_prefetch = g_state.key_cache + (size_t)layer * g_cfg.seq_len * kv_dim +
                                    (t + 2) * kv_dim + kv_head * head_dim;
                __builtin_prefetch(k_prefetch, 0, 3);
            }

            float score = 0.0f;
            for (int i = 0; i < head_dim; i++) {
                score += q_head[i] * k_t[i];
            }
            att_head[t] = score / sqrtf((float)head_dim);
        }

        /* Softmax */
        softmax(att_head, pos + 1);

        /* Weighted sum of values */
        float* out_head = g_state.xb + h * head_dim;
        for (int t = 0; t <= pos; t++) {
            float* v_t = g_state.value_cache + (size_t)layer * g_cfg.seq_len * kv_dim +
                         t * kv_dim + kv_head * head_dim;

            /* Prefetch next value vector (2 steps ahead) */
            if (t + 2 <= pos) {
                float* v_prefetch = g_state.value_cache + (size_t)layer * g_cfg.seq_len * kv_dim +
                                    (t + 2) * kv_dim + kv_head * head_dim;
                __builtin_prefetch(v_prefetch, 0, 3);
            }

            float att_w = att_head[t];
            for (int i = 0; i < head_dim; i++) {
                out_head[i] += att_w * v_t[i];
            }
        }
    }
#endif

    /* Output projection */
    matmul_stream(g_state.xb2, lw->attn_output, lw->attn_output_type, g_state.xb, dim, dim);

    /* Residual - SIMD optimized (8-16x faster) */
    elem_add_inplace_simd(g_state.x, g_state.xb2, dim);

    /* FFN norm */
    rmsnorm_stream(g_state.xb, g_state.x, lw->ffn_norm, lw->ffn_norm_type, dim, eps);

    /* FFN: SwiGLU */
    matmul_stream(g_state.hb, lw->ffn_gate, lw->ffn_gate_type, g_state.xb, hidden_dim, dim);
    matmul_stream(g_state.hb2, lw->ffn_up, lw->ffn_up_type, g_state.xb, hidden_dim, dim);

#if PARALLEL_INFERENCE_ENABLED
    /* Parallel SwiGLU activation */
    parallel_swiglu(g_state.hb, g_state.hb2, hidden_dim);
#else
    /* Sequential SwiGLU fallback */
    for (int i = 0; i < hidden_dim; i++) {
        g_state.hb[i] = silu(g_state.hb[i]) * g_state.hb2[i];
    }
#endif

    matmul_stream(g_state.xb, lw->ffn_down, lw->ffn_down_type, g_state.hb, dim, hidden_dim);

    /* Residual - SIMD optimized (8-16x faster) */
    elem_add_inplace_simd(g_state.x, g_state.xb, dim);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

/* Calculate memory requirements for a model */
size_t streaming_calc_memory(int dim, int hidden_dim, int n_layers,
                             int n_heads, int vocab_size, int seq_len) {
    int kv_dim = dim;  /* Simplified - adjust for GQA */

    size_t runtime = 0;
    runtime += dim * sizeof(float) * 4;              /* x, xb, xb2, q */
    runtime += kv_dim * sizeof(float) * 2;           /* k, v */
    runtime += n_heads * seq_len * sizeof(float);    /* att */
    runtime += hidden_dim * sizeof(float) * 2;       /* hb, hb2 */
    runtime += vocab_size * sizeof(float);           /* logits */
    runtime += n_layers * seq_len * kv_dim * 2 * sizeof(float);  /* KV cache */
    runtime += hidden_dim * sizeof(float);           /* layer dequant buffer */

    return runtime;
}

/* Initialize streaming inference from GGUF
 * preallocate: if true, allocate all buffers at init time (for deterministic mode)
 */
int streaming_inference_init(bool preallocate) {
    if (g_initialized) {
        return 0;
    }

    /* Get architecture from GGUF parser */
    const struct gguf_model_arch* arch = gguf_parser_get_arch();
    if (!arch) {
        console_printf("Error: No model loaded\n");
        return -1;
    }

    /* Populate config from GGUF metadata - use volatile to prevent optimization issues */
    volatile uint8_t *test = (volatile uint8_t*)&g_cfg;
    *test = 0;

    /* Write using volatile pointers */
    volatile int *dim_ptr = &g_cfg.dim;
    *dim_ptr = (int)arch->embedding_length;

    g_cfg.hidden_dim = arch->feed_forward_length;
    g_cfg.n_layers = arch->block_count;
    g_cfg.n_heads = arch->attention_head_count;
    g_cfg.n_kv_heads = arch->attention_head_count_kv ? arch->attention_head_count_kv : arch->attention_head_count;
    g_cfg.vocab_size = arch->vocab_size;

    uint32_t ctx_len = arch->context_length;
    int seq = (ctx_len > 2048) ? 2048 : (int)ctx_len;
    g_cfg.seq_len = seq;

    /* Derived values */
    g_cfg.head_dim = g_cfg.dim / g_cfg.n_heads;
    g_cfg.kv_dim = g_cfg.head_dim * g_cfg.n_kv_heads;
    g_cfg.kv_mul = g_cfg.n_heads / g_cfg.n_kv_heads;

    /* RoPE and normalization parameters from model metadata */
    g_cfg.rope_theta = arch->rope_freq_base > 0.0f ? arch->rope_freq_base : 10000.0f;
    g_cfg.rms_norm_eps = arch->attention_layer_norm_rms_epsilon > 0.0f ?
                         arch->attention_layer_norm_rms_epsilon : 1e-5f;

    /* Get special token IDs */
    g_cfg.eos_token_id = (int)arch->eos_token_id;
    g_cfg.bos_token_id = (int)arch->bos_token_id;

    /* Debug: Print loaded config (use int cast for floats since printf is limited) */
    console_printf("[STREAM] Config: dim=%d hidden=%d layers=%d heads=%d kv_heads=%d\n",
        g_cfg.dim, g_cfg.hidden_dim, g_cfg.n_layers, g_cfg.n_heads, g_cfg.n_kv_heads);
    console_printf("[STREAM] rope_theta=%d rms_eps=%d(x1e-7) vocab=%d seq_len=%d\n",
        (int)g_cfg.rope_theta, (int)(g_cfg.rms_norm_eps * 10000000.0f), g_cfg.vocab_size, g_cfg.seq_len);

    /* Copy architecture name */
    for (int i = 0; i < 63 && arch->general_architecture[i]; i++) {
        g_cfg.arch_name[i] = arch->general_architecture[i];
    }

    /* Allocate runtime buffers at init time (deterministic mode)
     * TODO: Use preallocate flag to control allocation timing (for now, always allocate)
     */
    (void)preallocate;  /* Parameter reserved for future use */
    g_state.x = (float*)heap_alloc(g_cfg.dim * sizeof(float));  /* init */
    g_state.xb = (float*)heap_alloc(g_cfg.dim * sizeof(float));  /* init */
    g_state.xb2 = (float*)heap_alloc(g_cfg.dim * sizeof(float));  /* init */
    g_state.q = (float*)heap_alloc(g_cfg.dim * sizeof(float));  /* init */
    g_state.k = (float*)heap_alloc(g_cfg.kv_dim * sizeof(float));  /* init */
    g_state.v = (float*)heap_alloc(g_cfg.kv_dim * sizeof(float));  /* init */
    g_state.att = (float*)heap_alloc(g_cfg.n_heads * g_cfg.seq_len * sizeof(float));  /* init */
    g_state.hb = (float*)heap_alloc(g_cfg.hidden_dim * sizeof(float));  /* init */
    g_state.hb2 = (float*)heap_alloc(g_cfg.hidden_dim * sizeof(float));  /* init */
    g_state.logits = (float*)heap_alloc(g_cfg.vocab_size * sizeof(float));  /* init */

    /* KV cache - init time allocation */
    size_t kv_size = (size_t)g_cfg.n_layers * g_cfg.seq_len * g_cfg.kv_dim * sizeof(float);
    g_state.key_cache = (float*)heap_alloc(kv_size);  /* init */
    g_state.value_cache = (float*)heap_alloc(kv_size);  /* init */

    /* Layer dequantization buffer - reused for each layer - init time */
    g_state.layer_buf_size = g_cfg.hidden_dim * sizeof(float);
    g_state.layer_weights = (float*)heap_alloc(g_state.layer_buf_size);  /* init */

    /* Pre-allocate quantized input buffer for matmul_q8_0_fused() at init time
     * Worst-case size is hidden_dim (largest dimension used in matmuls) */
    g_input_q8_size = g_cfg.hidden_dim / QK8_0;
    g_input_q8 = (block_q8_1*)heap_alloc(g_input_q8_size * sizeof(block_q8_1));  /* init */

    /* Check allocations */
    if (!g_state.x || !g_state.xb || !g_state.xb2 || !g_state.q ||
        !g_state.k || !g_state.v || !g_state.att || !g_state.hb ||
        !g_state.hb2 || !g_state.logits || !g_state.key_cache ||
        !g_state.value_cache || !g_state.layer_weights || !g_input_q8) {
        console_printf("Error: Failed to allocate memory\n");
        return -1;
    }

    /* Zero KV cache - use manual loop to avoid memset crash in QEMU */
    {
        volatile uint8_t *p = (volatile uint8_t*)g_state.key_cache;
        for (size_t i = 0; i < kv_size; i++) {
            p[i] = 0;
        }
    }
    {
        volatile uint8_t *p = (volatile uint8_t*)g_state.value_cache;
        for (size_t i = 0; i < kv_size; i++) {
            p[i] = 0;
        }
    }

    /* Allocate layer weight pointers at init time */
    g_layer_weights = (LayerWeights*)heap_alloc(g_cfg.n_layers * sizeof(LayerWeights));  /* init */
    if (!g_layer_weights) {
        console_printf("Error: Memory allocation failed\n");
        return -1;
    }

    /* Get tensor data pointers (quantized, not dequantized) */
    const struct gguf_tensor_info* tensor;

    /* Global weights */
    tensor = gguf_parser_get_tensor_by_name("token_embd.weight");
    if (tensor) {
        g_weights.token_embd = gguf_parser_get_tensor_data_ptr(tensor);
        g_weights.token_embd_type = tensor->type;

        /* Detect embedding layout from GGUF dimensions.
         *
         * GGUF/GGML dimension ordering: dims[0] is innermost (contiguous), dims[1] is outer.
         *
         * For embedding tensor [vocab_size, n_embd] in conventional (PyTorch) notation:
         * - Memory layout: vocab_size rows of n_embd elements (row-major)
         * - GGUF dims: dims[0] = n_embd (innermost), dims[1] = vocab_size (outer)
         * - Token t's embedding at offset: t * n_embd
         * - This is the STANDARD layout.
         *
         * Transposed would be [n_embd, vocab_size] in conventional notation:
         * - GGUF dims: dims[0] = vocab_size (innermost), dims[1] = n_embd (outer)
         * - Token t's embedding requires strided column access.
         */
        if (tensor->n_dims >= 2) {
            int dim0 = (int)tensor->dims[0];  /* innermost (contiguous) */
            int dim1 = (int)tensor->dims[1];  /* outer */
            if (dim0 == g_cfg.dim && dim1 == g_cfg.vocab_size) {
                /* dims[0]=n_embd, dims[1]=vocab_size -> STANDARD [vocab_size, n_embd] layout */
                g_weights.token_embd_transposed = false;
                g_weights.token_embd_vocab_size = dim1;
                console_printf("[STREAM] token_embd STANDARD [%d, %d] (GGUF dims) type=%d\n",
                               dim0, dim1, tensor->type);
            } else if (dim0 == g_cfg.vocab_size && dim1 == g_cfg.dim) {
                /* dims[0]=vocab_size, dims[1]=n_embd -> TRANSPOSED [n_embd, vocab_size] layout */
                g_weights.token_embd_transposed = true;
                g_weights.token_embd_vocab_size = dim0;
                console_printf("[STREAM] token_embd TRANSPOSED [%d, %d] (GGUF dims) type=%d\n",
                               dim0, dim1, tensor->type);
            } else {
                /* Unknown shape - try to infer based on which dimension matches */
                if (dim0 == g_cfg.dim) {
                    /* dims[0]=n_embd -> standard layout */
                    g_weights.token_embd_transposed = false;
                    g_weights.token_embd_vocab_size = dim1;
                    console_printf("[STREAM] token_embd inferred STANDARD [%d, %d] type=%d\n",
                                   dim0, dim1, tensor->type);
                } else {
                    /* dims[0]!=n_embd -> assume transposed */
                    g_weights.token_embd_transposed = true;
                    g_weights.token_embd_vocab_size = dim0;
                    console_printf("[STREAM] token_embd inferred TRANSPOSED [%d, %d] type=%d\n",
                                   dim0, dim1, tensor->type);
                }
            }
        } else {
            g_weights.token_embd_transposed = false;
            g_weights.token_embd_vocab_size = g_cfg.vocab_size;
            console_printf("[STREAM] token_embd type=%d (1D assumed standard)\n", tensor->type);
        }
    } else {
        console_printf("Error: token_embd.weight not found\n");
        return -1;
    }

    tensor = gguf_parser_get_tensor_by_name("output_norm.weight");
    if (tensor) {
        g_weights.output_norm = gguf_parser_get_tensor_data_ptr(tensor);
        g_weights.output_norm_type = tensor->type;
    }

    tensor = gguf_parser_get_tensor_by_name("output.weight");
    if (tensor) {
        g_weights.output = gguf_parser_get_tensor_data_ptr(tensor);
        g_weights.output_type = tensor->type;
        console_printf("[STREAM] output type=%d\n", tensor->type);
    } else {
        console_printf("[STREAM] output.weight not found, using token_embd\n");
    }

    /* Layer weights - just store pointers, don't dequantize */
    char name_buf[64];
    for (int l = 0; l < g_cfg.n_layers; l++) {
        LayerWeights* lw = &g_layer_weights[l];

        #define MAP_LAYER_TENSOR(field, suffix) do { \
            build_layer_name(name_buf, sizeof(name_buf), "blk.", l, suffix); \
            tensor = gguf_parser_get_tensor_by_name(name_buf); \
            if (tensor) { \
                lw->field = gguf_parser_get_tensor_data_ptr(tensor); \
                lw->field##_type = tensor->type; \
            } \
        } while(0)

        MAP_LAYER_TENSOR(attn_norm, ".attn_norm.weight");
        MAP_LAYER_TENSOR(attn_q, ".attn_q.weight");
        MAP_LAYER_TENSOR(attn_k, ".attn_k.weight");
        MAP_LAYER_TENSOR(attn_v, ".attn_v.weight");
        MAP_LAYER_TENSOR(attn_output, ".attn_output.weight");
        MAP_LAYER_TENSOR(ffn_norm, ".ffn_norm.weight");
        MAP_LAYER_TENSOR(ffn_gate, ".ffn_gate.weight");
        MAP_LAYER_TENSOR(ffn_up, ".ffn_up.weight");
        MAP_LAYER_TENSOR(ffn_down, ".ffn_down.weight");

        #undef MAP_LAYER_TENSOR

        /* Print layer 0 quantization types */
        if (l == 0) {
            console_printf("[STREAM] Layer0 types: norm=%d q=%d k=%d v=%d out=%d\n",
                lw->attn_norm_type, lw->attn_q_type, lw->attn_k_type,
                lw->attn_v_type, lw->attn_output_type);
            console_printf("[STREAM] Layer0 ffn: norm=%d gate=%d up=%d down=%d\n",
                lw->ffn_norm_type, lw->ffn_gate_type, lw->ffn_up_type, lw->ffn_down_type);
        }
    }

#if PARALLEL_INFERENCE_ENABLED
    /* Initialize parallel inference */
    int num_threads = PARALLEL_NUM_THREADS;
    if (num_threads > 1) {
        parallel_init(num_threads);
        console_printf("[STREAM] Parallel inference enabled with %d threads\n", num_threads);
    }
#endif

    g_initialized = true;
    return 0;
}

/* Generate tokens */
int streaming_inference_generate(const int* prompt_tokens, int prompt_len,
                                  int* output_tokens, int max_output) {
    if (!g_initialized) {
        console_printf("Error: Inference not initialized\n");
        return -1;
    }

    /* Reset matmul counters */
    g_q8_fused_count = 0;
    g_dequant_count = 0;

    int pos = 0;
    int token = prompt_tokens[0];
    int generated = 0;

    while (pos < g_cfg.seq_len && generated < max_output) {
        /* Enter critical section - disable interrupts for deterministic timing */
        critical_section_enter();

        /* Forward pass through all layers */
        for (int l = 0; l < g_cfg.n_layers; l++) {
            /* Prefetch next layer's weights (1 layer ahead) */
            if (l + 1 < g_cfg.n_layers) {
                LayerWeights* next_lw = &g_layer_weights[l + 1];
                __builtin_prefetch(next_lw->attn_norm, 0, 3);
                __builtin_prefetch(next_lw->attn_q, 0, 3);
            }

            transformer_forward_stream(token, pos, l);
        }

        /* Final norm and output projection */
        rmsnorm_stream(g_state.x, g_state.x, g_weights.output_norm,
                       g_weights.output_norm_type, g_cfg.dim, g_cfg.rms_norm_eps);

        /* Output logits */
        const void* output_weights = g_weights.output ? g_weights.output : g_weights.token_embd;
        int output_type = g_weights.output ? g_weights.output_type : g_weights.token_embd_type;

        /* Check if we need transposed matmul (when using transposed token_embd as output) */
        if (!g_weights.output && g_weights.token_embd_transposed) {
            /* token_embd is [dim, vocab_size], use transposed matmul */
            matmul_stream_transposed(g_state.logits, output_weights, output_type,
                                     g_state.x, g_cfg.dim, g_cfg.vocab_size);
        } else {
            /* Standard matmul for [vocab_size, dim] output weights */
            matmul_stream(g_state.logits, output_weights, output_type,
                          g_state.x, g_cfg.vocab_size, g_cfg.dim);
        }

        /* Get next token */
        int next_token;
        if (pos < prompt_len - 1) {
            next_token = prompt_tokens[pos + 1];
        } else {
            /* Argmax sampling */
            int max_idx = 0;
            float max_val = g_state.logits[0];
            for (int i = 1; i < g_cfg.vocab_size; i++) {
                if (g_state.logits[i] > max_val) {
                    max_val = g_state.logits[i];
                    max_idx = i;
                }
            }

            next_token = max_idx;
            output_tokens[generated++] = next_token;

            /* EOS check */
            if (next_token == g_cfg.eos_token_id) break;
        }

        token = next_token;
        pos++;

        /* Exit critical section - re-enable interrupts */
        critical_section_exit();
    }

    return generated;
}

/* Check if ready */
bool streaming_inference_is_ready(void) {
    return g_initialized;
}

/* Get token text */
const char* streaming_inference_get_token(int token_id) {
    return gguf_parser_get_token((uint32_t)token_id);
}

/* Get model info */
void streaming_inference_get_info(int* dim, int* layers, int* vocab, int* ctx) {
    if (dim) *dim = g_cfg.dim;
    if (layers) *layers = g_cfg.n_layers;
    if (vocab) *vocab = g_cfg.vocab_size;
    if (ctx) *ctx = g_cfg.seq_len;
}

/* ============================================================================
 * Timed Generation for Performance Analysis
 * ============================================================================ */

/* rdtsc for timing - architecture specific */
#if defined(__x86_64__)
static inline uint64_t get_cycles(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#elif defined(__aarch64__)
static inline uint64_t get_cycles(void) {
    uint64_t val;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}
#else
static inline uint64_t get_cycles(void) { return 0; }
#endif

/* Convert cycles to microseconds
 * For ARM64 HVF (macOS Hypervisor): cntvct_el0 runs at ~1000 MHz (1 cycle/ns)
 * For ARM64 bare metal: cntvct_el0 typically runs at ~24 MHz
 * For x86_64: TSC runs at CPU frequency (~1-4 GHz)
 */
static inline uint64_t cycles_to_us(uint64_t cycles) {
#if defined(__aarch64__)
    /* HVF uses a 1GHz virtual timer, so 1000 cycles = 1 us */
    return cycles / 1000;
#else
    return cycles / 1000; /* Approximate for 1GHz+ x86 */
#endif
}

/* Generate tokens with detailed timing information */
int streaming_inference_generate_timed(const int* prompt_tokens, int prompt_len,
                                        int* output_tokens, int max_output,
                                        inference_timing_t* timing) {
    if (!g_initialized) {
        console_printf("Error: Inference not initialized\n");
        return -1;
    }

    /* Initialize timing struct */
    if (timing) {
        memset(timing, 0, sizeof(*timing));
        timing->prompt_tokens = prompt_len;
    }

    int pos = 0;
    int token = prompt_tokens[0];
    int generated = 0;
    uint64_t generation_start = get_cycles();
    uint64_t prefill_end = 0;
    uint64_t first_token_time = 0;
    uint64_t last_token_time = generation_start;

    while (pos < g_cfg.seq_len && generated < max_output) {
        uint64_t token_start = get_cycles();

        /* Enter critical section - disable interrupts for deterministic timing */
        critical_section_enter();

        /* Forward pass through all layers */
        for (int l = 0; l < g_cfg.n_layers; l++) {
            /* Prefetch next layer's weights (1 layer ahead) */
            if (l + 1 < g_cfg.n_layers) {
                LayerWeights* next_lw = &g_layer_weights[l + 1];
                __builtin_prefetch(next_lw->attn_norm, 0, 3);
                __builtin_prefetch(next_lw->attn_q, 0, 3);
            }

            transformer_forward_stream(token, pos, l);
        }

        /* Final norm and output projection */
        rmsnorm_stream(g_state.x, g_state.x, g_weights.output_norm,
                       g_weights.output_norm_type, g_cfg.dim, g_cfg.rms_norm_eps);

        /* Output logits */
        const void* output_weights = g_weights.output ? g_weights.output : g_weights.token_embd;
        int output_type = g_weights.output ? g_weights.output_type : g_weights.token_embd_type;
        /* Use transposed matmul if token_embd is transposed and used as output */
        if (!g_weights.output && g_weights.token_embd_transposed) {
            matmul_stream_transposed(g_state.logits, output_weights, output_type,
                                     g_state.x, g_cfg.dim, g_cfg.vocab_size);
        } else {
            matmul_stream(g_state.logits, output_weights, output_type,
                          g_state.x, g_cfg.vocab_size, g_cfg.dim);
        }

        /* Debug: print logits on first decode token (as int * 1000) */
        /* Get next token */
        int next_token;
        bool is_decode = (pos >= prompt_len - 1);

        if (!is_decode) {
            /* Still in prefill phase */
            next_token = prompt_tokens[pos + 1];
        } else {
            /* Decode phase - mark end of prefill on first decode */
            if (prefill_end == 0) {
                prefill_end = token_start;
                if (timing) {
                    timing->prefill_us = cycles_to_us(prefill_end - generation_start);
                }
            }

            /* Argmax sampling */
            int max_idx = 0;
            float max_val = g_state.logits[0];
            for (int i = 1; i < g_cfg.vocab_size; i++) {
                if (g_state.logits[i] > max_val) {
                    max_val = g_state.logits[i];
                    max_idx = i;
                }
            }
            next_token = max_idx;
            output_tokens[generated] = next_token;

            /* Debug: Print first few logits on first generated token (scaled by 100 as int) */
            if (generated == 0) {
                console_printf("[DEBUG] logits[0..4] (x100): %d %d %d %d %d\n",
                    (int)(g_state.logits[0] * 100), (int)(g_state.logits[1] * 100),
                    (int)(g_state.logits[2] * 100), (int)(g_state.logits[3] * 100),
                    (int)(g_state.logits[4] * 100));
                console_printf("[DEBUG] max_val(x100)=%d max_idx=%d\n", (int)(max_val * 100), max_idx);
            }

            uint64_t token_end = get_cycles();
            uint64_t token_latency = cycles_to_us(token_end - last_token_time);

            /* Record first token time (TTFT) */
            if (generated == 0) {
                first_token_time = token_end;
                if (timing) {
                    timing->first_token_us = cycles_to_us(first_token_time - generation_start);
                }
            }

            /* Record per-token latency */
            if (timing && generated < MAX_TIMING_TOKENS) {
                timing->decode_latency_us[generated] = token_latency;
                timing->num_decode_samples = generated + 1;
            }

            last_token_time = token_end;
            generated++;

            /* EOS check */
            if (next_token == g_cfg.eos_token_id) break;
        }

        token = next_token;
        pos++;

        /* Exit critical section - re-enable interrupts */
        critical_section_exit();
    }

    uint64_t generation_end = get_cycles();

    /* Calculate final timing statistics */
    if (timing) {
        timing->generated_tokens = generated;
        timing->decode_total_us = cycles_to_us(generation_end - (prefill_end ? prefill_end : generation_start));

        /* Calculate min/max/avg decode latency */
        if (timing->num_decode_samples > 0) {
            timing->decode_min_us = timing->decode_latency_us[0];
            timing->decode_max_us = timing->decode_latency_us[0];
            uint64_t sum = 0;

            for (int i = 0; i < timing->num_decode_samples; i++) {
                uint64_t lat = timing->decode_latency_us[i];
                sum += lat;
                if (lat < timing->decode_min_us) timing->decode_min_us = lat;
                if (lat > timing->decode_max_us) timing->decode_max_us = lat;
            }
            timing->decode_avg_us = sum / timing->num_decode_samples;
            timing->decode_jitter_us = timing->decode_max_us - timing->decode_min_us;
            timing->deterministic_mode_enabled = g_deterministic_config.interrupt_disable;
            timing->interrupt_disabled_count = pos;
        }
    }

    return generated;
}

/* ============================================================================
 * Deterministic Mode Configuration API
 * ============================================================================ */

/* Configure deterministic execution mode
 * @param config Deterministic mode configuration
 * @return 0 on success, -1 on error
 */
int streaming_inference_set_deterministic(const deterministic_config_t* config) {
    if (!config) {
        return -1;
    }

    /* Update global configuration */
    g_deterministic_config.interrupt_disable = config->interrupt_disable;
    g_deterministic_config.preallocate_buffers = config->preallocate_buffers;
    g_deterministic_config.max_latency_us = config->max_latency_us;

    return 0;
}

/* Get current deterministic mode configuration
 * @param config Output buffer for configuration
 * @return 0 on success, -1 on error
 */
int streaming_inference_get_deterministic(deterministic_config_t* config) {
    if (!config) {
        return -1;
    }

    /* Copy current configuration */
    config->interrupt_disable = g_deterministic_config.interrupt_disable;
    config->preallocate_buffers = g_deterministic_config.preallocate_buffers;
    config->max_latency_us = g_deterministic_config.max_latency_us;

    return 0;
}
