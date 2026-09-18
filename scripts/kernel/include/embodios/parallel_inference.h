/* Parallel Inference API for EMBODIOS
 *
 * Provides multi-threaded inference operations for transformer models.
 */

#ifndef _EMBODIOS_PARALLEL_INFERENCE_H
#define _EMBODIOS_PARALLEL_INFERENCE_H

#include <embodios/types.h>

/* Maximum threads supported */
#define PARALLEL_MAX_THREADS 8

/* Per-core timing statistics */
typedef struct {
    uint64_t total_cycles;      /* Total cycles spent working */
    uint64_t work_items;        /* Total work items processed */
    uint64_t idle_cycles;       /* Cycles spent idle */
    uint32_t core_id;           /* Physical core ID */
    uint32_t invocations;       /* Number of work invocations */
} core_timing_stats_t;

/* Initialize parallel inference with N threads */
int parallel_init(int num_threads);

/* Shutdown parallel inference */
void parallel_shutdown(void);

/* Get/set number of threads */
int parallel_get_num_threads(void);
void parallel_set_num_threads(int n);

/* Core affinity configuration
 * parallel_set_core_affinity: Pin a specific thread to a CPU core
 * parallel_pin_cores: Enable/disable automatic core pinning (1 = enable, 0 = disable)
 */
int parallel_set_core_affinity(int thread_id, int core_id);
void parallel_pin_cores(int enable);

/* Deterministic mode for timing guarantees
 * parallel_set_deterministic: Enable/disable deterministic work distribution (1 = enable, 0 = disable)
 *
 * When enabled, uses fixed work assignment instead of work-stealing to ensure
 * consistent timing across multiple runs. This reduces timing variance but may
 * sacrifice some load balancing efficiency. Core pinning is automatically enabled.
 */
void parallel_set_deterministic(int enable);

/* Per-core timing statistics
 * parallel_get_core_stats: Get timing statistics for a specific thread
 * parallel_reset_core_stats: Reset all per-core statistics
 * parallel_print_core_stats: Print all core statistics to console
 */
int parallel_get_core_stats(int thread_id, core_timing_stats_t* stats);
void parallel_reset_core_stats(void);
void parallel_print_core_stats(void);

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
