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
 * Returns 0 on success, -1 on error
 */
int streaming_inference_init(void);

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
