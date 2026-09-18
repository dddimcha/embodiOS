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

/* Check if inference engine is ready */
bool streaming_inference_is_ready(void);

/* Get token text from vocabulary */
const char* streaming_inference_get_token(int token_id);

/* Get model information */
void streaming_inference_get_info(int* dim, int* layers, int* vocab, int* ctx);

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
