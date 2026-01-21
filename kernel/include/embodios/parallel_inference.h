/* Parallel Inference API for EMBODIOS
 *
 * Provides multi-threaded inference operations for transformer models.
 */

#ifndef _EMBODIOS_PARALLEL_INFERENCE_H
#define _EMBODIOS_PARALLEL_INFERENCE_H

#include <embodios/types.h>

/* Maximum threads supported */
#define PARALLEL_MAX_THREADS 8

/* Initialize parallel inference with N threads */
int parallel_init(int num_threads);

/* Shutdown parallel inference */
void parallel_shutdown(void);

/* Get/set number of threads */
int parallel_get_num_threads(void);
void parallel_set_num_threads(int n);

/* Work function type for parallel_for */
typedef void (*work_func_t)(void* arg, int thread_id, int start, int end);

/* Execute work in parallel across threads
 * func: Worker function to call
 * arg: User argument passed to func
 * total_items: Total number of items to process
 * chunk_size: Items per work unit (0 = auto)
 */
void parallel_for(work_func_t func, void* arg, int total_items, int chunk_size);

/* ============================================================================
 * Parallel Matrix Operations
 * ============================================================================ */

/* Parallel matrix-vector multiply (float32)
 * out[rows] = weights[rows, cols] @ input[cols]
 */
void parallel_matmul_f32(float* out, const float* weights, const float* input,
                          int rows, int cols);

/* ============================================================================
 * Parallel Attention
 * ============================================================================ */

/* Parallel multi-head attention
 * Parallelizes computation across attention heads.
 *
 * output[dim]: Output after attention
 * q[dim]: Query vectors
 * key_cache[seq_len, kv_dim]: Cached keys
 * value_cache[seq_len, kv_dim]: Cached values
 * att[n_heads, seq_len]: Attention score buffer
 * n_heads: Number of query heads
 * n_kv_heads: Number of key/value heads (for GQA)
 * head_dim: Dimension per head
 * kv_dim: Key/value total dimension
 * seq_len: Maximum sequence length
 * pos: Current position in sequence
 */
void parallel_attention(float* output, const float* q,
                        const float* key_cache, const float* value_cache,
                        float* att, int n_heads, int n_kv_heads,
                        int head_dim, int kv_dim, int seq_len, int pos);

/* ============================================================================
 * Parallel Normalization
 * ============================================================================ */

/* Parallel RMSNorm
 * out = x * weight * (1/sqrt(mean(x^2) + eps))
 */
void parallel_rmsnorm(float* out, const float* x, const float* weight,
                       int size, float eps);

/* ============================================================================
 * Parallel Activations
 * ============================================================================ */

/* Parallel SwiGLU: gate = silu(gate) * up
 * Combines SiLU activation with element-wise multiply
 */
void parallel_swiglu(float* gate, const float* up, int size);

#endif /* _EMBODIOS_PARALLEL_INFERENCE_H */
