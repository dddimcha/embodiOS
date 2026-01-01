/* EMBODIOS Transformer Inference Engine
 * Pure fixed-point (Q16.16) implementation
 *
 * Industry-standard API with:
 * - Error codes for all operations
 * - Bounds checking
 * - No VLAs
 */

#ifndef EMBODIOS_INFERENCE_H
#define EMBODIOS_INFERENCE_H

#include <embodios/types.h>

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
 * Configuration Limits
 * ============================================================================ */

#define MAX_EMBD        2048
#define MAX_HEADS       32
#define MAX_KV_HEADS    32
#define MAX_HEAD_DIM    128
#define MAX_FF_DIM      8192
#define MAX_SEQ_LEN     2048
#define MAX_VOCAB_SIZE  65536

/* ============================================================================
 * Fixed-Point Types
 * ============================================================================ */

typedef int32_t fixed_t;

#define FIXED_SHIFT 16
#define FIXED_ONE   (1 << FIXED_SHIFT)
#define FIXED_HALF  (1 << (FIXED_SHIFT - 1))
#define F2FX(f)     ((fixed_t)((f) * FIXED_ONE))
#define FX2F(x)     ((float)(x) / (float)FIXED_ONE)

/* ============================================================================
 * Layer Weights Structure
 * ============================================================================ */

typedef struct layer_weights_fx {
    fixed_t* attn_norm;     /* [n_embd] */
    fixed_t* ffn_norm;      /* [n_embd] */
    fixed_t* q_weight;      /* [n_embd, n_heads * head_dim] */
    fixed_t* k_weight;      /* [n_embd, n_kv_heads * head_dim] */
    fixed_t* v_weight;      /* [n_embd, n_kv_heads * head_dim] */
    fixed_t* o_weight;      /* [n_heads * head_dim, n_embd] */
    fixed_t* gate_weight;   /* [n_embd, n_ff] */
    fixed_t* up_weight;     /* [n_embd, n_ff] */
    fixed_t* down_weight;   /* [n_ff, n_embd] */
} layer_weights_fx_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize inference engine with model configuration
 *
 * @param n_vocab     Vocabulary size (1 to MAX_VOCAB_SIZE)
 * @param n_embd      Embedding dimension (1 to MAX_EMBD, must divide by n_heads)
 * @param n_layer     Number of transformer layers (1 to 128)
 * @param n_heads     Number of attention heads (1 to MAX_HEADS)
 * @param n_kv_heads  Number of KV heads for GQA (1 to MAX_KV_HEADS)
 * @param n_ff        Feed-forward hidden dimension (1 to MAX_FF_DIM)
 * @param max_seq_len Maximum sequence length (1 to MAX_SEQ_LEN)
 *
 * @return INFERENCE_OK on success, error code on failure
 */
int inference_init(int n_vocab, int n_embd, int n_layer, int n_heads,
                   int n_kv_heads, int n_ff, int max_seq_len);

/**
 * Set weights for a specific layer
 *
 * @param layer_idx  Layer index (0 to n_layer-1)
 * @param weights    Pointer to layer weights structure
 *
 * @return INFERENCE_OK on success, error code on failure
 */
int inference_set_layer_weights(int layer_idx, layer_weights_fx_t* weights);

/**
 * Set embedding and output weights
 *
 * @param token_emb  Token embeddings [n_vocab, n_embd] (can be NULL for demo)
 * @param out_norm   Output normalization weights [n_embd] (can be NULL)
 * @param lm_head    LM head weights [n_embd, n_vocab] (can be NULL for demo)
 */
void inference_set_embeddings(fixed_t* token_emb, fixed_t* out_norm, fixed_t* lm_head);

/**
 * Forward pass for a single token
 *
 * @param token_id  Input token ID
 * @param logits    Output logits buffer [n_vocab]
 *
 * @return INFERENCE_OK on success, error code on failure
 */
int inference_forward(int token_id, fixed_t* logits);

/**
 * Sample next token from logits
 *
 * @param logits      Logits buffer (will be modified to probabilities)
 * @param vocab_size  Vocabulary size
 * @param temperature Temperature for sampling (FIXED_ONE = 1.0)
 * @param top_p       Top-p for nucleus sampling (reserved, use FIXED_ONE)
 *
 * @return Sampled token ID (0 on error)
 */
int inference_sample(fixed_t* logits, int vocab_size, fixed_t temperature, fixed_t top_p);

/**
 * Reset KV cache for new generation
 */
void inference_reset(void);

/**
 * Get current sequence position
 *
 * @return Current position in KV cache
 */
int inference_get_position(void);

/**
 * Cleanup inference engine and free all resources
 */
void inference_cleanup(void);

/* ============================================================================
 * Core Operations (can be used independently)
 * ============================================================================ */

/**
 * RMS Normalization (in-place)
 *
 * @param x       Input/output vector [size]
 * @param weight  Weight vector [size] (can be NULL)
 * @param size    Vector size (1 to MAX_EMBD)
 * @param epsilon Small constant for numerical stability
 *
 * @return INFERENCE_OK on success, error code on failure
 */
int rms_norm_fx(fixed_t* x, const fixed_t* weight, int size, fixed_t epsilon);

/**
 * Apply RoPE to query and key vectors
 *
 * @param q          Query vectors [n_heads * head_dim]
 * @param k          Key vectors [n_kv_heads * head_dim]
 * @param pos        Position in sequence
 * @param head_dim   Head dimension
 * @param n_heads    Number of query heads
 * @param n_kv_heads Number of KV heads
 *
 * @return INFERENCE_OK on success, error code on failure
 */
int rope_apply(fixed_t* q, fixed_t* k, int pos, int head_dim, int n_heads, int n_kv_heads);

/* ============================================================================
 * Fixed-Point Math Utilities
 * ============================================================================ */

static inline fixed_t fxmul(fixed_t a, fixed_t b) {
    return (fixed_t)(((int64_t)a * (int64_t)b) >> FIXED_SHIFT);
}

static inline fixed_t fxdiv(fixed_t a, fixed_t b) {
    if (b == 0) return 0;
    return (fixed_t)(((int64_t)a << FIXED_SHIFT) / b);
}

#endif /* EMBODIOS_INFERENCE_H */
