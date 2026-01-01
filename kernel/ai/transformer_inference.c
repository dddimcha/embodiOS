/* EMBODIOS Complete Transformer Inference Engine
 * Pure fixed-point implementation (Q16.16)
 * Implements: RMSNorm, RoPE, Multi-Head Attention, FFN (SwiGLU)
 *
 * Industry-standard implementation with:
 * - No VLAs (Variable Length Arrays)
 * - Comprehensive error checking
 * - Bounds validation
 * - Integer overflow protection
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/mm.h>

/* ============================================================================
 * Configuration Limits
 * ============================================================================ */

#define MAX_EMBD        2048    /* Maximum embedding dimension */
#define MAX_HEADS       32      /* Maximum attention heads */
#define MAX_KV_HEADS    32      /* Maximum KV heads */
#define MAX_HEAD_DIM    128     /* Maximum head dimension */
#define MAX_FF_DIM      8192    /* Maximum FFN dimension */
#define MAX_SEQ_LEN     2048    /* Maximum sequence length */
#define MAX_VOCAB_SIZE  65536   /* Maximum vocabulary size */

#define ROPE_TABLE_SIZE 256     /* Positions in RoPE table */

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define INFERENCE_OK              0
#define INFERENCE_ERR_NULL       -1
#define INFERENCE_ERR_BOUNDS     -2
#define INFERENCE_ERR_OVERFLOW   -3
#define INFERENCE_ERR_NOT_INIT   -4
#define INFERENCE_ERR_ALREADY_INIT -5
#define INFERENCE_ERR_ALLOC      -6
#define INFERENCE_ERR_INVALID    -7

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

void inference_cleanup(void);

/* ============================================================================
 * Fixed-Point Math (Q16.16)
 * ============================================================================ */

typedef int32_t fixed_t;

#define FIXED_SHIFT 16
#define FIXED_ONE   (1 << FIXED_SHIFT)
#define FIXED_HALF  (1 << (FIXED_SHIFT - 1))
#define FIXED_MAX   INT32_MAX
#define FIXED_MIN   INT32_MIN
#define F2FX(f)     ((fixed_t)((f) * FIXED_ONE))
#define FX2F(x)     ((float)(x) / (float)FIXED_ONE)

/* Fixed-point multiply: (a * b) >> 16 */
static inline fixed_t fxmul(fixed_t a, fixed_t b) {
    return (fixed_t)(((int64_t)a * (int64_t)b) >> FIXED_SHIFT);
}

/* Fixed-point divide: (a << 16) / b with zero check */
static inline fixed_t fxdiv(fixed_t a, fixed_t b) {
    if (b == 0) return 0;
    return (fixed_t)(((int64_t)a << FIXED_SHIFT) / b);
}

/* Fixed-point sqrt using Newton-Raphson */
static fixed_t fxsqrt(fixed_t x) {
    if (x <= 0) return 0;
    fixed_t guess = x >> 1;
    if (guess == 0) guess = FIXED_ONE;

    for (int i = 0; i < 10; i++) {
        if (guess == 0) break;
        fixed_t new_guess = (guess + fxdiv(x, guess)) >> 1;
        if (new_guess == guess) break;
        guess = new_guess;
    }
    return guess;
}

/* Fixed-point exp using Taylor series with range reduction */
static fixed_t fxexp(fixed_t x) {
    /* Clamp to prevent overflow */
    const fixed_t EXP_MIN = F2FX(-8.0f);
    const fixed_t EXP_MAX = F2FX(8.0f);

    if (x < EXP_MIN) return 0;
    if (x > EXP_MAX) return F2FX(2980.0f);  /* e^8 ≈ 2980 */

    /* Range reduction: e^x = e^(x/8) ^ 8 */
    fixed_t scaled = x >> 3;

    /* Taylor: 1 + x + x²/2 + x³/6 + x⁴/24 + x⁵/120 */
    fixed_t result = FIXED_ONE;
    fixed_t term = scaled;

    result += term;
    term = fxmul(term, scaled) >> 1;
    result += term;
    term = fxmul(term, scaled) / 3;
    result += term;
    term = fxmul(term, scaled) >> 2;
    result += term;
    term = fxmul(term, scaled) / 5;
    result += term;

    /* Raise to 8th power */
    result = fxmul(result, result);
    result = fxmul(result, result);
    result = fxmul(result, result);

    return result;
}

/* ============================================================================
 * Precomputed RoPE Tables
 * ============================================================================ */

static fixed_t rope_cos_table[ROPE_TABLE_SIZE][MAX_HEAD_DIM / 2];
static fixed_t rope_sin_table[ROPE_TABLE_SIZE][MAX_HEAD_DIM / 2];
static bool rope_tables_initialized = false;
static int rope_head_dim = 0;

/* Fixed-point sin with bounded input normalization */
static fixed_t fxsin(fixed_t x) {
    const fixed_t PI = 205887;       /* π in Q16.16 */
    const fixed_t TWO_PI = 411775;   /* 2π in Q16.16 */

    /* Bounded normalization to [-π, π] - max 4 iterations */
    int iterations = 0;
    while (x > PI && iterations < 4) {
        x -= TWO_PI;
        iterations++;
    }
    iterations = 0;
    while (x < -PI && iterations < 4) {
        x += TWO_PI;
        iterations++;
    }

    /* Clamp if still out of range */
    if (x > PI) x = PI;
    if (x < -PI) x = -PI;

    /* Taylor: sin(x) ≈ x - x³/6 + x⁵/120 */
    fixed_t x2 = fxmul(x, x);
    fixed_t x3 = fxmul(x2, x);
    fixed_t x5 = fxmul(x3, x2);

    return x - x3 / 6 + x5 / 120;
}

/* Fixed-point cos using sin */
static fixed_t fxcos(fixed_t x) {
    const fixed_t PI_HALF = 102944;  /* π/2 in Q16.16 */
    return fxsin(x + PI_HALF);
}

/* Pre-computed RoPE frequencies for standard head dimensions
 * freq[d] = 1 / (10000^(2d/head_dim))
 *
 * These match Ollama/llama.cpp RoPE implementation.
 * Values in Q16.16 fixed-point format.
 */

/* Frequencies for head_dim=64 (half_dim=32)
 * freq[d] = 1 / (10000^(2*d/64)) = 1 / (10000^(d/32))
 * Matches Ollama/llama.cpp and tinystories_inference.c
 */
static const fixed_t rope_freq_64[32] = {
    65536,  /* d= 0: 1/10000^(0/32)  = 1.000000 */
    49145,  /* d= 1: 1/10000^(1/32)  = 0.749894 */
    36854,  /* d= 2: 1/10000^(2/32)  = 0.562341 */
    27636,  /* d= 3: 1/10000^(3/32)  = 0.421697 */
    20724,  /* d= 4: 1/10000^(4/32)  = 0.316228 */
    15541,  /* d= 5: 1/10000^(5/32)  = 0.237137 */
    11654,  /* d= 6: 1/10000^(6/32)  = 0.177828 */
     8739,  /* d= 7: 1/10000^(7/32)  = 0.133352 */
     6554,  /* d= 8: 1/10000^(8/32)  = 0.100000 */
     4915,  /* d= 9: 1/10000^(9/32)  = 0.074989 */
     3685,  /* d=10: 1/10000^(10/32) = 0.056234 */
     2764,  /* d=11: 1/10000^(11/32) = 0.042170 */
     2072,  /* d=12: 1/10000^(12/32) = 0.031623 */
     1554,  /* d=13: 1/10000^(13/32) = 0.023714 */
     1165,  /* d=14: 1/10000^(14/32) = 0.017783 */
      874,  /* d=15: 1/10000^(15/32) = 0.013335 */
      655,  /* d=16: 1/10000^(16/32) = 0.010000 */
      491,  /* d=17: 1/10000^(17/32) = 0.007499 */
      369,  /* d=18: 1/10000^(18/32) = 0.005623 */
      276,  /* d=19: 1/10000^(19/32) = 0.004217 */
      207,  /* d=20: 1/10000^(20/32) = 0.003162 */
      155,  /* d=21: 1/10000^(21/32) = 0.002371 */
      117,  /* d=22: 1/10000^(22/32) = 0.001778 */
       87,  /* d=23: 1/10000^(23/32) = 0.001334 */
       66,  /* d=24: 1/10000^(24/32) = 0.001000 */
       49,  /* d=25: 1/10000^(25/32) = 0.000750 */
       37,  /* d=26: 1/10000^(26/32) = 0.000562 */
       28,  /* d=27: 1/10000^(27/32) = 0.000422 */
       21,  /* d=28: 1/10000^(28/32) = 0.000316 */
       16,  /* d=29: 1/10000^(29/32) = 0.000237 */
       12,  /* d=30: 1/10000^(30/32) = 0.000178 */
        9   /* d=31: 1/10000^(31/32) = 0.000133 */
};

/* Frequencies for head_dim=128 (half_dim=64)
 * freq[d] = 1 / (10000^(2*d/128)) = 1 / (10000^(d/64))
 * Matches Ollama/llama.cpp and tinystories_inference.c
 */
static const fixed_t rope_freq_128[64] = {
    65536, 56752, 49145, 42558, 36854, 31914, 27636, 23932,
    20724, 17947, 15541, 13458, 11654, 10092,  8739,  7568,
     6554,  5675,  4915,  4256,  3685,  3191,  2764,  2393,
     2072,  1795,  1554,  1346,  1165,  1009,   874,   757,
      655,   568,   491,   426,   369,   319,   276,   239,
      207,   179,   155,   135,   117,   101,    87,    76,
       66,    57,    49,    43,    37,    32,    28,    24,
       21,    18,    16,    13,    12,    10,     9,     8
};

/* Get frequency for given dimension and head_dim */
static fixed_t get_rope_freq(int d, int head_dim) {
    /* Use pre-computed table if available */
    if (head_dim == 64 && d < 32) {
        return rope_freq_64[d];
    }
    if (head_dim == 128 && d < 64) {
        return rope_freq_128[d];
    }

    /* Fallback: compute frequency using successive division
     * freq[d] = 1 / (10000^(2d/head_dim))
     *
     * We compute 10000^(2d/head_dim) by scaling:
     * Let step = 10000^(2/head_dim), then theta = step^d
     *
     * For head_dim=64: step = 10000^(1/32) ≈ 1.2589
     * For head_dim=128: step = 10000^(1/64) ≈ 1.1220
     */
    int half_dim = head_dim / 2;
    if (half_dim <= 0) half_dim = 32;

    /* Approximate using the formula: freq ≈ exp(-d * ln(10000) / half_dim)
     * In fixed-point: use linear interpolation from boundary values
     */
    if (d <= 0) return FIXED_ONE;
    if (d >= half_dim) return 1;

    /* Compute frequency using geometric decay:
     * freq[d] = 1 / (10000^(d/half_dim))
     *
     * We use successive multiplication by the decay factor:
     * decay = 10000^(-1/half_dim)
     *
     * For half_dim=32: decay = 10000^(-1/32) = 0.7499 = 49145 in Q16.16
     * For half_dim=64: decay = 10000^(-1/64) = 0.8660 = 56752 in Q16.16
     */
    int64_t freq = FIXED_ONE;

    /* Compute decay factor: 10000^(-1/half_dim)
     * For half_dim=32: 49145
     * For half_dim=64: 56752
     * General approximation: decay ≈ 65536 - (65536 - 49145) * 32 / half_dim
     */
    fixed_t step_decay;
    if (half_dim <= 32) {
        step_decay = 49145;  /* 10000^(-1/32) */
    } else if (half_dim <= 64) {
        step_decay = 56752;  /* 10000^(-1/64) */
    } else {
        /* Linear interpolation for larger half_dim */
        step_decay = 56752 + (FIXED_ONE - 56752) * (half_dim - 64) / 64;
        if (step_decay >= FIXED_ONE) step_decay = FIXED_ONE - 1;
    }

    for (int i = 0; i < d && freq > 1; i++) {
        freq = (freq * step_decay) >> FIXED_SHIFT;
    }

    return (fixed_t)(freq > 0 ? freq : 1);
}

/* Initialize RoPE lookup tables
 *
 * RoPE formula (matching Ollama/llama.cpp):
 *   freq[d] = 1 / (10000^(2d/head_dim))
 *   angle = pos * freq[d]
 *   cos_table[pos][d] = cos(angle)
 *   sin_table[pos][d] = sin(angle)
 *
 * This creates frequency bands where:
 *   - Low d (d=0,1): High frequency, captures local position
 *   - High d (d=31): Low frequency, captures global position
 */
static void init_rope_tables(int head_dim) {
    if (rope_tables_initialized && rope_head_dim == head_dim) return;

    /* Clamp head_dim to valid range */
    if (head_dim <= 0) head_dim = 64;
    if (head_dim > MAX_HEAD_DIM) head_dim = MAX_HEAD_DIM;

    int half_dim = head_dim / 2;
    if (half_dim > MAX_HEAD_DIM / 2) half_dim = MAX_HEAD_DIM / 2;

    for (int pos = 0; pos < ROPE_TABLE_SIZE; pos++) {
        for (int d = 0; d < half_dim; d++) {
            /* Get pre-computed frequency: freq = 1 / (10000^(2d/head_dim)) */
            fixed_t freq = get_rope_freq(d, head_dim);

            /* angle = pos * freq
             * pos is integer, freq is Q16.16
             * Result is Q16.16 (no shift needed!)
             *
             * Example: pos=5, freq=65536 (1.0)
             *   angle = 5 * 65536 = 327680 = 5.0 radians in Q16.16
             */
            fixed_t angle = (fixed_t)((int64_t)pos * freq);

            rope_cos_table[pos][d] = fxcos(angle);
            rope_sin_table[pos][d] = fxsin(angle);
        }
    }

    rope_head_dim = head_dim;
    rope_tables_initialized = true;
}

/* ============================================================================
 * RMSNorm - Root Mean Square Normalization
 * ============================================================================ */

/**
 * RMS Normalization (in-place)
 * Returns INFERENCE_OK on success, error code on failure
 */
int rms_norm_fx(fixed_t* x, const fixed_t* weight, int size, fixed_t epsilon) {
    /* Parameter validation */
    if (!x) return INFERENCE_ERR_NULL;
    if (size <= 0 || size > MAX_EMBD) return INFERENCE_ERR_BOUNDS;

    /* Compute mean of squares using int64 accumulator */
    int64_t sum_sq = 0;
    for (int i = 0; i < size; i++) {
        int64_t val = x[i];
        sum_sq += (val * val) >> FIXED_SHIFT;
    }

    fixed_t mean_sq = (fixed_t)(sum_sq / size);
    fixed_t rms = fxsqrt(mean_sq + epsilon);

    /* Prevent division by zero */
    if (rms == 0) rms = FIXED_ONE;

    /* Normalize and apply weight */
    for (int i = 0; i < size; i++) {
        fixed_t normed = fxdiv(x[i], rms);
        if (weight) {
            x[i] = fxmul(normed, weight[i]);
        } else {
            x[i] = normed;
        }
    }

    return INFERENCE_OK;
}

/* ============================================================================
 * RoPE - Rotary Position Embeddings
 * ============================================================================ */

/**
 * Apply RoPE to query and key vectors
 * Returns INFERENCE_OK on success, error code on failure
 */
int rope_apply(fixed_t* q, fixed_t* k, int pos, int head_dim, int n_heads, int n_kv_heads) {
    /* Parameter validation */
    if (!q || !k) return INFERENCE_ERR_NULL;
    if (head_dim <= 0 || head_dim > MAX_HEAD_DIM) return INFERENCE_ERR_BOUNDS;
    if (n_heads <= 0 || n_heads > MAX_HEADS) return INFERENCE_ERR_BOUNDS;
    if (n_kv_heads <= 0 || n_kv_heads > MAX_KV_HEADS) return INFERENCE_ERR_BOUNDS;

    /* Ensure tables are initialized */
    if (!rope_tables_initialized || rope_head_dim != head_dim) {
        init_rope_tables(head_dim);
    }

    int pos_idx = pos % ROPE_TABLE_SIZE;
    if (pos_idx < 0) pos_idx = 0;

    int half_dim = head_dim / 2;
    if (half_dim > MAX_HEAD_DIM / 2) half_dim = MAX_HEAD_DIM / 2;

    /* Apply to each query head */
    for (int h = 0; h < n_heads; h++) {
        fixed_t* q_head = &q[h * head_dim];

        for (int d = 0; d < half_dim; d++) {
            fixed_t cos_val = rope_cos_table[pos_idx][d];
            fixed_t sin_val = rope_sin_table[pos_idx][d];

            fixed_t q0 = q_head[d * 2];
            fixed_t q1 = q_head[d * 2 + 1];

            q_head[d * 2]     = fxmul(q0, cos_val) - fxmul(q1, sin_val);
            q_head[d * 2 + 1] = fxmul(q0, sin_val) + fxmul(q1, cos_val);
        }
    }

    /* Apply to each key head */
    for (int h = 0; h < n_kv_heads; h++) {
        fixed_t* k_head = &k[h * head_dim];

        for (int d = 0; d < half_dim; d++) {
            fixed_t cos_val = rope_cos_table[pos_idx][d];
            fixed_t sin_val = rope_sin_table[pos_idx][d];

            fixed_t k0 = k_head[d * 2];
            fixed_t k1 = k_head[d * 2 + 1];

            k_head[d * 2]     = fxmul(k0, cos_val) - fxmul(k1, sin_val);
            k_head[d * 2 + 1] = fxmul(k0, sin_val) + fxmul(k1, cos_val);
        }
    }

    return INFERENCE_OK;
}

/* ============================================================================
 * Data Structures
 * ============================================================================ */

/* KV Cache structure */
typedef struct {
    fixed_t* key_cache;
    fixed_t* value_cache;
    int max_seq_len;
    int n_layers;
    int n_kv_heads;
    int head_dim;
    int current_pos;
} inference_kv_cache_t;

/* Layer weights */
typedef struct {
    fixed_t* attn_norm;
    fixed_t* ffn_norm;
    fixed_t* q_weight;
    fixed_t* k_weight;
    fixed_t* v_weight;
    fixed_t* o_weight;
    fixed_t* gate_weight;
    fixed_t* up_weight;
    fixed_t* down_weight;
} layer_weights_fx_t;

/* Model configuration */
typedef struct {
    int n_vocab;
    int n_embd;
    int n_layer;
    int n_heads;
    int n_kv_heads;
    int n_ff;
    int head_dim;
    int max_seq_len;
    fixed_t rms_epsilon;
} model_config_fx_t;

/* Inference state */
typedef struct {
    model_config_fx_t config;
    layer_weights_fx_t* layer_weights;
    fixed_t* token_embeddings;
    fixed_t* output_norm;
    fixed_t* lm_head;
    inference_kv_cache_t kv_cache;
    /* Pre-allocated work buffers to avoid VLAs */
    fixed_t* work_q;        /* [MAX_HEADS * MAX_HEAD_DIM] */
    fixed_t* work_k;        /* [MAX_KV_HEADS * MAX_HEAD_DIM] */
    fixed_t* work_v;        /* [MAX_KV_HEADS * MAX_HEAD_DIM] */
    fixed_t* work_attn;     /* [MAX_HEADS * MAX_HEAD_DIM] */
    fixed_t* work_hidden;   /* [MAX_EMBD] */
    fixed_t* work_ff;       /* [MAX_FF_DIM] */
    fixed_t* work_scores;   /* [MAX_SEQ_LEN] */
    bool initialized;
} inference_state_t;

static inference_state_t g_inference = {0};

/* ============================================================================
 * Multi-Head Attention (Fixed-Point)
 * ============================================================================ */

/**
 * Multi-Head Attention with KV Cache
 * Uses pre-allocated work buffers instead of VLAs
 */
static int multi_head_attention_fx(
    const fixed_t* hidden,
    const fixed_t* q_weight,
    const fixed_t* k_weight,
    const fixed_t* v_weight,
    const fixed_t* o_weight,
    fixed_t* output,
    inference_kv_cache_t* kv_cache,
    int layer_idx,
    int pos,
    int n_embd,
    int n_heads,
    int n_kv_heads,
    int head_dim)
{
    /* Use pre-allocated work buffers */
    fixed_t* q_buf = g_inference.work_q;
    fixed_t* k_buf = g_inference.work_k;
    fixed_t* v_buf = g_inference.work_v;
    fixed_t* attn_out = g_inference.work_attn;
    fixed_t* scores = g_inference.work_scores;

    if (!q_buf || !k_buf || !v_buf || !attn_out || !scores) {
        return INFERENCE_ERR_NOT_INIT;
    }

    int q_size = n_heads * head_dim;
    int kv_size = n_kv_heads * head_dim;

    /* Bounds check */
    if (q_size > MAX_HEADS * MAX_HEAD_DIM) return INFERENCE_ERR_BOUNDS;
    if (kv_size > MAX_KV_HEADS * MAX_HEAD_DIM) return INFERENCE_ERR_BOUNDS;
    if (pos >= MAX_SEQ_LEN) return INFERENCE_ERR_BOUNDS;

    /* Q projection */
    if (q_weight) {
        for (int i = 0; i < q_size; i++) {
            int64_t sum = 0;
            for (int j = 0; j < n_embd; j++) {
                sum += ((int64_t)hidden[j] * (int64_t)q_weight[j * q_size + i]);
            }
            q_buf[i] = (fixed_t)(sum >> FIXED_SHIFT);
        }
    } else {
        for (int i = 0; i < q_size; i++) q_buf[i] = hidden[i % n_embd];
    }

    /* K projection */
    if (k_weight) {
        for (int i = 0; i < kv_size; i++) {
            int64_t sum = 0;
            for (int j = 0; j < n_embd; j++) {
                sum += ((int64_t)hidden[j] * (int64_t)k_weight[j * kv_size + i]);
            }
            k_buf[i] = (fixed_t)(sum >> FIXED_SHIFT);
        }
    } else {
        for (int i = 0; i < kv_size; i++) k_buf[i] = hidden[i % n_embd];
    }

    /* V projection */
    if (v_weight) {
        for (int i = 0; i < kv_size; i++) {
            int64_t sum = 0;
            for (int j = 0; j < n_embd; j++) {
                sum += ((int64_t)hidden[j] * (int64_t)v_weight[j * kv_size + i]);
            }
            v_buf[i] = (fixed_t)(sum >> FIXED_SHIFT);
        }
    } else {
        for (int i = 0; i < kv_size; i++) v_buf[i] = hidden[i % n_embd];
    }

    /* Apply RoPE */
    rope_apply(q_buf, k_buf, pos, head_dim, n_heads, n_kv_heads);

    /* Store K, V in cache */
    if (kv_cache && kv_cache->key_cache && kv_cache->value_cache &&
        pos < kv_cache->max_seq_len && layer_idx < kv_cache->n_layers) {

        /* Calculate offsets with overflow protection */
        int64_t layer_offset_64 = (int64_t)layer_idx * kv_cache->max_seq_len * kv_size;
        int64_t pos_offset_64 = (int64_t)pos * kv_size;

        if (layer_offset_64 < INT32_MAX && pos_offset_64 < INT32_MAX) {
            int offset = (int)layer_offset_64 + (int)pos_offset_64;

            for (int i = 0; i < kv_size; i++) {
                kv_cache->key_cache[offset + i] = k_buf[i];
                kv_cache->value_cache[offset + i] = v_buf[i];
            }
        }
    }

    /* Attention scale: 1/sqrt(head_dim) */
    fixed_t scale = fxdiv(FIXED_ONE, fxsqrt(head_dim << FIXED_SHIFT));

    /* Protect against division by zero in GQA ratio */
    int heads_per_kv = (n_kv_heads > 0) ? (n_heads / n_kv_heads) : 1;
    if (heads_per_kv <= 0) heads_per_kv = 1;

    /* Clear attention output */
    for (int i = 0; i < q_size; i++) {
        attn_out[i] = 0;
    }

    /* Compute attention for each query head */
    for (int qh = 0; qh < n_heads; qh++) {
        int kv_h = qh / heads_per_kv;
        if (kv_h >= n_kv_heads) kv_h = n_kv_heads - 1;

        int seq_len = pos + 1;
        if (seq_len > MAX_SEQ_LEN) seq_len = MAX_SEQ_LEN;

        /* Compute QK^T scores */
        for (int t = 0; t < seq_len; t++) {
            int64_t score = 0;

            const fixed_t* k_t;
            if (kv_cache && kv_cache->key_cache && t < kv_cache->max_seq_len) {
                int64_t offset = (int64_t)layer_idx * kv_cache->max_seq_len * kv_size +
                                 (int64_t)t * kv_size + kv_h * head_dim;
                k_t = &kv_cache->key_cache[offset];
            } else {
                k_t = &k_buf[kv_h * head_dim];
            }

            for (int d = 0; d < head_dim; d++) {
                score += ((int64_t)q_buf[qh * head_dim + d] * (int64_t)k_t[d]);
            }

            scores[t] = fxmul((fixed_t)(score >> FIXED_SHIFT), scale);
        }

        /* Softmax */
        fixed_t max_score = scores[0];
        for (int t = 1; t < seq_len; t++) {
            if (scores[t] > max_score) max_score = scores[t];
        }

        fixed_t sum_exp = 0;
        for (int t = 0; t < seq_len; t++) {
            scores[t] = fxexp(scores[t] - max_score);
            sum_exp += scores[t];
        }

        if (sum_exp > 0) {
            for (int t = 0; t < seq_len; t++) {
                scores[t] = fxdiv(scores[t], sum_exp);
            }
        }

        /* Weighted sum of values */
        for (int t = 0; t < seq_len; t++) {
            const fixed_t* v_t;
            if (kv_cache && kv_cache->value_cache && t < kv_cache->max_seq_len) {
                int64_t offset = (int64_t)layer_idx * kv_cache->max_seq_len * kv_size +
                                 (int64_t)t * kv_size + kv_h * head_dim;
                v_t = &kv_cache->value_cache[offset];
            } else {
                v_t = &v_buf[kv_h * head_dim];
            }

            for (int d = 0; d < head_dim; d++) {
                attn_out[qh * head_dim + d] += fxmul(scores[t], v_t[d]);
            }
        }
    }

    /* Output projection */
    if (o_weight) {
        for (int i = 0; i < n_embd; i++) {
            int64_t sum = 0;
            for (int j = 0; j < q_size; j++) {
                sum += ((int64_t)attn_out[j] * (int64_t)o_weight[j * n_embd + i]);
            }
            output[i] = (fixed_t)(sum >> FIXED_SHIFT);
        }
    } else {
        for (int i = 0; i < n_embd; i++) {
            output[i] = attn_out[i % q_size];
        }
    }

    return INFERENCE_OK;
}

/* ============================================================================
 * Feed-Forward Network (SwiGLU)
 * ============================================================================ */

static int ffn_swiglu_fx(
    const fixed_t* hidden,
    const fixed_t* gate_weight,
    const fixed_t* up_weight,
    const fixed_t* down_weight,
    fixed_t* output,
    int n_embd,
    int n_ff)
{
    /* Use pre-allocated work buffers */
    fixed_t* gate_buf = g_inference.work_ff;
    fixed_t* up_buf = g_inference.work_hidden;

    if (!gate_buf || !up_buf) return INFERENCE_ERR_NOT_INIT;
    if (n_ff > MAX_FF_DIM) return INFERENCE_ERR_BOUNDS;
    if (n_embd > MAX_EMBD) return INFERENCE_ERR_BOUNDS;

    /* Gate projection */
    if (gate_weight) {
        for (int i = 0; i < n_ff; i++) {
            int64_t sum = 0;
            for (int j = 0; j < n_embd; j++) {
                sum += ((int64_t)hidden[j] * (int64_t)gate_weight[j * n_ff + i]);
            }
            gate_buf[i] = (fixed_t)(sum >> FIXED_SHIFT);
        }
    } else {
        for (int i = 0; i < n_ff; i++) gate_buf[i] = hidden[i % n_embd];
    }

    /* Up projection */
    if (up_weight) {
        for (int i = 0; i < n_ff; i++) {
            int64_t sum = 0;
            for (int j = 0; j < n_embd; j++) {
                sum += ((int64_t)hidden[j] * (int64_t)up_weight[j * n_ff + i]);
            }
            up_buf[i] = (fixed_t)(sum >> FIXED_SHIFT);
        }
    } else {
        for (int i = 0; i < n_ff; i++) up_buf[i] = hidden[i % n_embd];
    }

    /* SwiGLU activation */
    for (int i = 0; i < n_ff; i++) {
        fixed_t g = gate_buf[i];

        /* Sigmoid approximation */
        fixed_t g_half = g >> 1;
        fixed_t abs_g = (g_half < 0) ? -g_half : g_half;
        fixed_t tanh_approx = fxdiv(g_half, FIXED_ONE + abs_g);
        fixed_t sigmoid = FIXED_HALF + (tanh_approx >> 1);

        /* Swish = g * sigmoid(g), SwiGLU = swish(gate) * up */
        fixed_t swish = fxmul(g, sigmoid);
        gate_buf[i] = fxmul(swish, up_buf[i]);
    }

    /* Down projection */
    if (down_weight) {
        for (int i = 0; i < n_embd; i++) {
            int64_t sum = 0;
            for (int j = 0; j < n_ff; j++) {
                sum += ((int64_t)gate_buf[j] * (int64_t)down_weight[j * n_embd + i]);
            }
            output[i] = (fixed_t)(sum >> FIXED_SHIFT);
        }
    } else {
        for (int i = 0; i < n_embd; i++) output[i] = gate_buf[i % n_ff];
    }

    return INFERENCE_OK;
}

/* ============================================================================
 * Transformer Layer
 * ============================================================================ */

static int transformer_layer_fx(
    fixed_t* hidden,
    const layer_weights_fx_t* weights,
    inference_kv_cache_t* kv_cache,
    int layer_idx,
    int pos,
    const model_config_fx_t* config)
{
    if (!hidden || !config) return INFERENCE_ERR_NULL;

    int n_embd = config->n_embd;
    if (n_embd > MAX_EMBD) return INFERENCE_ERR_BOUNDS;

    /* Use work buffers for temporaries */
    fixed_t* residual = g_inference.work_hidden;
    fixed_t* normed = g_inference.work_ff;
    fixed_t* attn_out = g_inference.work_attn;

    if (!residual || !normed || !attn_out) return INFERENCE_ERR_NOT_INIT;

    /* Save residual and copy for norm */
    for (int i = 0; i < n_embd; i++) {
        residual[i] = hidden[i];
        normed[i] = hidden[i];
    }

    /* Pre-attention RMSNorm */
    int err = rms_norm_fx(normed, weights ? weights->attn_norm : NULL,
                          n_embd, config->rms_epsilon);
    if (err != INFERENCE_OK) return err;

    /* Multi-head attention */
    err = multi_head_attention_fx(
        normed,
        weights ? weights->q_weight : NULL,
        weights ? weights->k_weight : NULL,
        weights ? weights->v_weight : NULL,
        weights ? weights->o_weight : NULL,
        attn_out,
        kv_cache,
        layer_idx, pos,
        n_embd, config->n_heads, config->n_kv_heads, config->head_dim
    );
    if (err != INFERENCE_OK) return err;

    /* Residual + attention */
    for (int i = 0; i < n_embd; i++) {
        hidden[i] = residual[i] + attn_out[i];
        residual[i] = hidden[i];
        normed[i] = hidden[i];
    }

    /* Pre-FFN RMSNorm */
    err = rms_norm_fx(normed, weights ? weights->ffn_norm : NULL,
                      n_embd, config->rms_epsilon);
    if (err != INFERENCE_OK) return err;

    /* FFN - reuse attn_out for ffn_out */
    err = ffn_swiglu_fx(
        normed,
        weights ? weights->gate_weight : NULL,
        weights ? weights->up_weight : NULL,
        weights ? weights->down_weight : NULL,
        attn_out,
        n_embd, config->n_ff
    );
    if (err != INFERENCE_OK) return err;

    /* Final residual */
    for (int i = 0; i < n_embd; i++) {
        hidden[i] = residual[i] + attn_out[i];
    }

    return INFERENCE_OK;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize inference engine with model configuration
 */
int inference_init(int n_vocab, int n_embd, int n_layer, int n_heads,
                   int n_kv_heads, int n_ff, int max_seq_len)
{
    /* Check if already initialized */
    if (g_inference.initialized) {
        console_printf("[Inference] Already initialized\n");
        return INFERENCE_ERR_ALREADY_INIT;
    }

    /* Validate parameters */
    if (n_vocab <= 0 || n_vocab > MAX_VOCAB_SIZE) {
        console_printf("[Inference] Invalid vocab size: %d\n", n_vocab);
        return INFERENCE_ERR_BOUNDS;
    }
    if (n_embd <= 0 || n_embd > MAX_EMBD) {
        console_printf("[Inference] Invalid embd size: %d\n", n_embd);
        return INFERENCE_ERR_BOUNDS;
    }
    if (n_layer <= 0 || n_layer > 128) {
        console_printf("[Inference] Invalid layer count: %d\n", n_layer);
        return INFERENCE_ERR_BOUNDS;
    }
    if (n_heads <= 0 || n_heads > MAX_HEADS) {
        console_printf("[Inference] Invalid head count: %d\n", n_heads);
        return INFERENCE_ERR_BOUNDS;
    }
    if (n_kv_heads <= 0 || n_kv_heads > MAX_KV_HEADS) {
        console_printf("[Inference] Invalid kv_head count: %d\n", n_kv_heads);
        return INFERENCE_ERR_BOUNDS;
    }
    if (n_ff <= 0 || n_ff > MAX_FF_DIM) {
        console_printf("[Inference] Invalid ff dim: %d\n", n_ff);
        return INFERENCE_ERR_BOUNDS;
    }
    if (max_seq_len <= 0 || max_seq_len > MAX_SEQ_LEN) {
        console_printf("[Inference] Invalid max_seq_len: %d\n", max_seq_len);
        return INFERENCE_ERR_BOUNDS;
    }
    if (n_embd % n_heads != 0) {
        console_printf("[Inference] n_embd must be divisible by n_heads\n");
        return INFERENCE_ERR_INVALID;
    }

    /* Store configuration */
    g_inference.config.n_vocab = n_vocab;
    g_inference.config.n_embd = n_embd;
    g_inference.config.n_layer = n_layer;
    g_inference.config.n_heads = n_heads;
    g_inference.config.n_kv_heads = n_kv_heads;
    g_inference.config.n_ff = n_ff;
    g_inference.config.head_dim = n_embd / n_heads;
    g_inference.config.max_seq_len = max_seq_len;
    g_inference.config.rms_epsilon = F2FX(1e-5f);

    /* Initialize RoPE tables */
    init_rope_tables(g_inference.config.head_dim);

    /* Allocate KV cache with overflow check */
    int kv_dim = n_kv_heads * g_inference.config.head_dim;
    uint64_t cache_size_64 = (uint64_t)n_layer * max_seq_len * kv_dim * sizeof(fixed_t);

    if (cache_size_64 > SIZE_MAX / 2) {
        console_printf("[Inference] KV cache size overflow\n");
        return INFERENCE_ERR_OVERFLOW;
    }

    size_t cache_size = (size_t)cache_size_64;
    g_inference.kv_cache.key_cache = (fixed_t*)kmalloc(cache_size);
    g_inference.kv_cache.value_cache = (fixed_t*)kmalloc(cache_size);

    if (!g_inference.kv_cache.key_cache || !g_inference.kv_cache.value_cache) {
        console_printf("[Inference] Failed to allocate KV cache\n");
        if (g_inference.kv_cache.key_cache) kfree(g_inference.kv_cache.key_cache);
        if (g_inference.kv_cache.value_cache) kfree(g_inference.kv_cache.value_cache);
        g_inference.kv_cache.key_cache = NULL;
        g_inference.kv_cache.value_cache = NULL;
        return INFERENCE_ERR_ALLOC;
    }

    g_inference.kv_cache.max_seq_len = max_seq_len;
    g_inference.kv_cache.n_layers = n_layer;
    g_inference.kv_cache.n_kv_heads = n_kv_heads;
    g_inference.kv_cache.head_dim = g_inference.config.head_dim;
    g_inference.kv_cache.current_pos = 0;

    /* Allocate work buffers */
    g_inference.work_q = (fixed_t*)kmalloc(MAX_HEADS * MAX_HEAD_DIM * sizeof(fixed_t));
    g_inference.work_k = (fixed_t*)kmalloc(MAX_KV_HEADS * MAX_HEAD_DIM * sizeof(fixed_t));
    g_inference.work_v = (fixed_t*)kmalloc(MAX_KV_HEADS * MAX_HEAD_DIM * sizeof(fixed_t));
    g_inference.work_attn = (fixed_t*)kmalloc(MAX_HEADS * MAX_HEAD_DIM * sizeof(fixed_t));
    g_inference.work_hidden = (fixed_t*)kmalloc(MAX_EMBD * sizeof(fixed_t));
    g_inference.work_ff = (fixed_t*)kmalloc(MAX_FF_DIM * sizeof(fixed_t));
    g_inference.work_scores = (fixed_t*)kmalloc(MAX_SEQ_LEN * sizeof(fixed_t));

    if (!g_inference.work_q || !g_inference.work_k || !g_inference.work_v ||
        !g_inference.work_attn || !g_inference.work_hidden || !g_inference.work_ff ||
        !g_inference.work_scores) {
        console_printf("[Inference] Failed to allocate work buffers\n");
        inference_cleanup();
        return INFERENCE_ERR_ALLOC;
    }

    g_inference.initialized = true;
    console_printf("[Inference] Initialized: vocab=%d embd=%d layers=%d heads=%d\n",
                   n_vocab, n_embd, n_layer, n_heads);

    return INFERENCE_OK;
}

/**
 * Set weights for a specific layer
 */
int inference_set_layer_weights(int layer_idx, layer_weights_fx_t* weights)
{
    if (!g_inference.initialized) return INFERENCE_ERR_NOT_INIT;
    if (!weights) return INFERENCE_ERR_NULL;
    if (layer_idx < 0 || layer_idx >= g_inference.config.n_layer) {
        return INFERENCE_ERR_BOUNDS;
    }

    if (!g_inference.layer_weights) {
        size_t size = (size_t)g_inference.config.n_layer * sizeof(layer_weights_fx_t);
        g_inference.layer_weights = (layer_weights_fx_t*)kmalloc(size);
        if (!g_inference.layer_weights) return INFERENCE_ERR_ALLOC;

        /* Zero initialize */
        for (int i = 0; i < g_inference.config.n_layer; i++) {
            g_inference.layer_weights[i].attn_norm = NULL;
            g_inference.layer_weights[i].ffn_norm = NULL;
            g_inference.layer_weights[i].q_weight = NULL;
            g_inference.layer_weights[i].k_weight = NULL;
            g_inference.layer_weights[i].v_weight = NULL;
            g_inference.layer_weights[i].o_weight = NULL;
            g_inference.layer_weights[i].gate_weight = NULL;
            g_inference.layer_weights[i].up_weight = NULL;
            g_inference.layer_weights[i].down_weight = NULL;
        }
    }

    g_inference.layer_weights[layer_idx] = *weights;
    return INFERENCE_OK;
}

/**
 * Set embedding and output weights
 */
void inference_set_embeddings(fixed_t* token_emb, fixed_t* out_norm, fixed_t* lm_head)
{
    g_inference.token_embeddings = token_emb;
    g_inference.output_norm = out_norm;
    g_inference.lm_head = lm_head;
}

/**
 * Forward pass for a single token
 */
int inference_forward(int token_id, fixed_t* logits)
{
    if (!g_inference.initialized) {
        console_printf("[Inference] Not initialized\n");
        return INFERENCE_ERR_NOT_INIT;
    }
    if (!logits) return INFERENCE_ERR_NULL;

    int pos = g_inference.kv_cache.current_pos;
    int n_embd = g_inference.config.n_embd;
    int n_layer = g_inference.config.n_layer;
    int n_vocab = g_inference.config.n_vocab;

    /* Check position bounds */
    if (pos >= g_inference.config.max_seq_len) {
        console_printf("[Inference] Position %d exceeds max %d\n",
                       pos, g_inference.config.max_seq_len);
        return INFERENCE_ERR_BOUNDS;
    }

    /* Use work buffer for hidden state */
    fixed_t* hidden = g_inference.work_hidden;
    if (!hidden) return INFERENCE_ERR_NOT_INIT;

    /* Token embedding lookup */
    if (g_inference.token_embeddings && token_id >= 0 && token_id < n_vocab) {
        for (int i = 0; i < n_embd; i++) {
            hidden[i] = g_inference.token_embeddings[token_id * n_embd + i];
        }
    } else {
        /* Fallback: pseudo-random embedding */
        for (int i = 0; i < n_embd; i++) {
            int32_t val = (token_id * 13 + i * 7) % 200 - 100;
            hidden[i] = (val << FIXED_SHIFT) / 100;
        }
    }

    /* Forward through all layers */
    for (int l = 0; l < n_layer; l++) {
        int err;
        if (g_inference.layer_weights) {
            err = transformer_layer_fx(
                hidden,
                &g_inference.layer_weights[l],
                &g_inference.kv_cache,
                l, pos,
                &g_inference.config
            );
        } else {
            /* Demo mode */
            err = rms_norm_fx(hidden, NULL, n_embd, g_inference.config.rms_epsilon);
        }
        if (err != INFERENCE_OK) return err;
    }

    /* Final RMSNorm */
    int err = rms_norm_fx(hidden, g_inference.output_norm, n_embd,
                          g_inference.config.rms_epsilon);
    if (err != INFERENCE_OK) return err;

    /* LM head projection to logits */
    if (g_inference.lm_head) {
        for (int v = 0; v < n_vocab; v++) {
            int64_t sum = 0;
            for (int i = 0; i < n_embd; i++) {
                sum += ((int64_t)hidden[i] *
                        (int64_t)g_inference.lm_head[i * n_vocab + v]);
            }
            logits[v] = (fixed_t)(sum >> FIXED_SHIFT);
        }
    } else {
        /* Demo mode */
        for (int v = 0; v < n_vocab; v++) {
            int64_t logit = 0;
            int limit = (n_embd < 64) ? n_embd : 64;
            for (int i = 0; i < limit; i++) {
                int32_t w = ((v * 7 + i * 3) % 100) - 50;
                fixed_t weight = (w << FIXED_SHIFT) / 50;
                logit += fxmul(hidden[i], weight);
            }
            logits[v] = (fixed_t)(logit >> 3);
        }
    }

    /* Increment position */
    g_inference.kv_cache.current_pos++;

    return INFERENCE_OK;
}

/**
 * Sample next token from logits
 */
int inference_sample(fixed_t* logits, int vocab_size, fixed_t temperature, fixed_t top_p)
{
    if (!logits) return 0;
    if (vocab_size <= 0) return 0;

    /* Apply temperature */
    if (temperature > 0 && temperature != FIXED_ONE) {
        for (int i = 0; i < vocab_size; i++) {
            logits[i] = fxdiv(logits[i], temperature);
        }
    }

    /* Find max for numerical stability */
    fixed_t max_logit = logits[0];
    for (int i = 1; i < vocab_size; i++) {
        if (logits[i] > max_logit) max_logit = logits[i];
    }

    /* Softmax */
    fixed_t sum = 0;
    for (int i = 0; i < vocab_size; i++) {
        logits[i] = fxexp(logits[i] - max_logit);
        sum += logits[i];
    }

    if (sum > 0) {
        for (int i = 0; i < vocab_size; i++) {
            logits[i] = fxdiv(logits[i], sum);
        }
    }

    /* Greedy sampling (argmax) */
    int best_idx = 0;
    fixed_t best_prob = logits[0];

    for (int i = 1; i < vocab_size; i++) {
        if (logits[i] > best_prob) {
            best_prob = logits[i];
            best_idx = i;
        }
    }

    (void)top_p;  /* Reserved for future nucleus sampling */

    return best_idx;
}

/**
 * Reset KV cache for new generation
 */
void inference_reset(void)
{
    g_inference.kv_cache.current_pos = 0;
}

/**
 * Get current sequence position
 */
int inference_get_position(void)
{
    return g_inference.kv_cache.current_pos;
}

/**
 * Cleanup inference engine
 */
void inference_cleanup(void)
{
    if (g_inference.kv_cache.key_cache) {
        kfree(g_inference.kv_cache.key_cache);
        g_inference.kv_cache.key_cache = NULL;
    }
    if (g_inference.kv_cache.value_cache) {
        kfree(g_inference.kv_cache.value_cache);
        g_inference.kv_cache.value_cache = NULL;
    }
    if (g_inference.layer_weights) {
        kfree(g_inference.layer_weights);
        g_inference.layer_weights = NULL;
    }
    if (g_inference.work_q) {
        kfree(g_inference.work_q);
        g_inference.work_q = NULL;
    }
    if (g_inference.work_k) {
        kfree(g_inference.work_k);
        g_inference.work_k = NULL;
    }
    if (g_inference.work_v) {
        kfree(g_inference.work_v);
        g_inference.work_v = NULL;
    }
    if (g_inference.work_attn) {
        kfree(g_inference.work_attn);
        g_inference.work_attn = NULL;
    }
    if (g_inference.work_hidden) {
        kfree(g_inference.work_hidden);
        g_inference.work_hidden = NULL;
    }
    if (g_inference.work_ff) {
        kfree(g_inference.work_ff);
        g_inference.work_ff = NULL;
    }
    if (g_inference.work_scores) {
        kfree(g_inference.work_scores);
        g_inference.work_scores = NULL;
    }

    g_inference.token_embeddings = NULL;
    g_inference.output_norm = NULL;
    g_inference.lm_head = NULL;
    g_inference.initialized = false;
}
