/* Streaming Inference Engine for EMBODIOS
 *
 * Features:
 * - Dynamic architecture detection from GGUF metadata
 * - Streaming layer-by-layer processing
 * - On-the-fly dequantization (keeps weights quantized)
 * - Supports models larger than available RAM
 */

#ifndef _EMBODIOS_STREAMING_INFERENCE_H
#define _EMBODIOS_STREAMING_INFERENCE_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Calculate memory requirements for a model
 * Returns bytes needed for runtime buffers (not including weights)
 */
size_t streaming_calc_memory(int dim, int hidden_dim, int n_layers,
                             int n_heads, int vocab_size, int seq_len);

/* Initialize streaming inference from loaded GGUF model
 * Call after gguf_parser has loaded model data
 * preallocate: if true, allocate all buffers at init time (for deterministic mode)
 * Returns 0 on success, -1 on error
 */
int streaming_inference_init(bool preallocate);

/* Generate tokens from prompt
 * prompt_tokens: input token IDs
 * prompt_len: number of input tokens
 * output_tokens: buffer for generated tokens
 * max_output: maximum tokens to generate
 * Returns number of tokens generated, or -1 on error
 */
int streaming_inference_generate(const int* prompt_tokens, int prompt_len,
                                  int* output_tokens, int max_output);

/* Per-token streaming callback: invoked once per generated token,
 * immediately after sampling. EOS/stop tokens are never delivered
 * (generation halts on them exactly like the non-callback path). */
typedef void (*stream_token_fn)(int token_id, void* ctx);

/* Generate tokens with per-token streaming and phase timing.
 * Same generation semantics as streaming_inference_generate(); additionally:
 *   cb:               if non-NULL, called with each non-stop generated token
 *   cb_ctx:           opaque pointer passed to cb
 *   prefill_cycles:   if non-NULL, receives TSC cycles spent in prompt eval
 *                     (TTFT; 0 when no token was generated)
 *   decode_cycles:    if non-NULL, receives TSC cycles spent after prefill
 * Returns number of tokens generated, or -1 on error
 */
int streaming_inference_generate_cb(const int* prompt_tokens, int prompt_len,
                                    int* output_tokens, int max_output,
                                    stream_token_fn cb, void* cb_ctx,
                                    uint64_t* prefill_cycles,
                                    uint64_t* decode_cycles);

/* Check if inference engine is ready */
bool streaming_inference_is_ready(void);

/* ============================================================================
 * Layer-range API (exo distributed inference)
 * ============================================================================
 * Stages of a single-token forward step, exposed so the exo ring orchestrator
 * can distribute a model across nodes (pipeline parallelism by layer range).
 * The KV cache lives in this engine indexed by absolute layer number and
 * position, so each node only reads/writes the KV region of its own shard;
 * only the hidden vector (dim floats) travels between nodes per token.
 */

/* Embed a token: out receives dim float32 values (token_embd lookup,
 * any supported quantization/layout). Returns 0 or -1. */
int streaming_inference_embed(int token, float* out);

/* Run transformer layers [start_layer, end_layer) over `hidden`
 * (dim floats, updated in place) at sequence position `pos`
 * (RoPE + KV cache). Out-of-range bounds are clamped to the model.
 * Returns 0 or -1. */
int streaming_inference_forward_layers(float* hidden, int pos,
                                       int start_layer, int end_layer);

/* Sample the next token.
 * hidden_or_logits == NULL:      use the engine's current hidden state;
 * hidden_or_logits == logits:    sample from already-computed logits;
 * otherwise:                     pointer to a dim-float hidden state.
 * Applies final RMSNorm + output projection when given a hidden state,
 * then samples according to temperature/top-p settings.
 * Returns the token id or -1. */
int streaming_inference_sample_token(const float* hidden_or_logits);

/* Check a token against EOS and the registered stop tokens */
bool streaming_inference_is_stop_token(int token_id);

/* Override EOS/stop token (e.g. chat template stop token like <|im_end|>) */
void streaming_inference_set_eos(int token_id);

/* Get current EOS/stop token */
int streaming_inference_get_eos(void);

/* Add an extra stop token checked in addition to EOS (max 4).
 * Used for GLM-style chat where generation stops on <|user|>/<|assistant|>. */
void streaming_inference_add_stop_token(int token_id);

/* Debug/verification: forward a prompt and print top-k logits to console */
void streaming_inference_debug_logits(const int* prompt_tokens, int prompt_len, int topk);

/* Get token text from vocabulary */
const char* streaming_inference_get_token(int token_id);

/* Get model information */
void streaming_inference_get_info(int* dim, int* layers, int* vocab, int* ctx);

/* ============================================================================
 * Sampling configuration
 * ============================================================================ */

/* Set sampling temperature [0.0, 2.0]. 0.0 = greedy argmax (deterministic) */
void streaming_inference_set_temperature(float temp);

/* Get current sampling temperature */
float streaming_inference_get_temperature(void);

/* Set nucleus (top-p) threshold [0.0, 1.0]. 1.0 = no nucleus filtering */
void streaming_inference_set_top_p(float p);

/* Get current nucleus (top-p) threshold */
float streaming_inference_get_top_p(void);

/* ============================================================================
 * Deterministic Mode Configuration
 * ============================================================================ */

/* Deterministic execution mode for hard real-time guarantees
 * Provides bounded latency by disabling interrupts and pre-allocating buffers
 */
typedef struct {
    bool interrupt_disable;     /* Disable interrupts during token generation */
    bool preallocate_buffers;   /* Pre-allocate all buffers at init time */
    uint64_t max_latency_us;    /* Maximum acceptable latency in microseconds */
} deterministic_config_t;

/**
 * Configure deterministic execution mode
 * @param config Deterministic mode configuration
 * @return 0 on success, -1 on error
 */
int streaming_inference_set_deterministic(const deterministic_config_t* config);

/**
 * Get current deterministic mode configuration
 * @param config Output buffer for configuration
 * @return 0 on success, -1 on error
 */
int streaming_inference_get_deterministic(deterministic_config_t* config);

/* ============================================================================
 * Detailed Timing Support for Performance Analysis
 * ============================================================================ */

#define MAX_TIMING_TOKENS 64

/* Detailed timing structure for performance analysis */
typedef struct {
    /* High-level timings (in microseconds) */
    uint64_t tokenize_us;         /* Time to tokenize prompt */
    uint64_t prefill_us;          /* Time to process all prompt tokens */
    uint64_t first_token_us;      /* TTFT: Time to first output token */
    uint64_t decode_total_us;     /* Total decode time (excluding prefill) */

    /* Per-token decode latencies (first N tokens) */
    uint64_t decode_latency_us[MAX_TIMING_TOKENS];
    int num_decode_samples;

    /* Summary statistics */
    uint64_t decode_min_us;       /* Minimum decode latency */
    uint64_t decode_max_us;       /* Maximum decode latency */
    uint64_t decode_avg_us;       /* Average decode latency */
    uint64_t decode_jitter_us;    /* Jitter: max - min decode latency */

    /* Deterministic mode tracking */
    bool deterministic_mode_enabled;   /* Was deterministic mode active? */
    int interrupt_disabled_count;      /* Number of times interrupts disabled */

    /* Token counts */
    int prompt_tokens;
    int generated_tokens;
} inference_timing_t;

/* Generate tokens with detailed timing
 * Same as streaming_inference_generate but fills timing struct
 * Returns number of tokens generated, or -1 on error
 */
int streaming_inference_generate_timed(const int* prompt_tokens, int prompt_len,
                                        int* output_tokens, int max_output,
                                        inference_timing_t* timing);

#ifdef __cplusplus
}
#endif

#endif /* _EMBODIOS_STREAMING_INFERENCE_H */
