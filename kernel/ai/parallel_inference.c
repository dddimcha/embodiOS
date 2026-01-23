/* Parallel Inference Engine for EMBODIOS
 *
 * Implements multi-threaded inference using a thread pool.
 * Can scale from 1 thread (QEMU single-core) to N threads (native multi-core).
 *
 * Key features:
 * - Work-stealing thread pool for matmul
 * - Parallel attention head computation
 * - Barrier synchronization between layers
 */

#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/types.h>
#include <embodios/mutex.h>
#include <embodios/atomic.h>
#include <embodios/spinlock.h>
#include <embodios/task.h>
#include <embodios/benchmark.h>
#include <embodios/parallel_inference.h>

/* Configuration */
#ifndef PARALLEL_MAX_THREADS
#define PARALLEL_MAX_THREADS 8
#endif

/* Forward declarations */
extern float sqrtf(float x);
extern float expf(float x);
extern void* memset(void*, int, size_t);
extern void* memcpy(void*, const void*, size_t);
extern int snprintf(char* str, size_t size, const char* format, ...);

/* Fast exp approximation - valid for all x in [-10, 10]
 * Uses the identity: exp(x) = 2^(x * log2(e))
 * Split into integer and fractional parts for accuracy */
static inline float fast_expf(float x) {
    /* Clamp to avoid overflow/underflow */
    if (x < -10.0f) return 0.0f;
    if (x > 10.0f) x = 10.0f;

    /* exp(x) = 2^(x * log2(e)) where log2(e) ≈ 1.4427 */
    float t = x * 1.44269504f;

    /* Split into integer and fractional parts */
    int ti = (int)t;
    if (t < 0 && t != (float)ti) ti--;  /* floor for negative numbers */
    float tf = t - (float)ti;  /* fractional part in [0, 1) */

    /* Compute 2^tf using polynomial: 2^x ≈ 1 + 0.6931472*x + 0.2402265*x^2 + 0.0555041*x^3 */
    /* This is the Taylor series for 2^x evaluated at small x */
    float p = 0.6931472f * tf;
    float p2 = p * p;
    float frac = 1.0f + p + p2 * 0.5f + p2 * p * 0.166667f;

    /* Compute 2^ti using bit manipulation */
    union { float f; uint32_t i; } u;
    u.i = (uint32_t)((ti + 127) & 0xFF) << 23;

    return frac * u.f;
}

/* ============================================================================
 * Thread Pool Types
 * ============================================================================ */

typedef void (*work_func_t)(void* arg, int thread_id, int start, int end);

/* Work item for parallel execution */
typedef struct {
    work_func_t func;           /* Function to execute */
    void* arg;                  /* User argument */
    int total_items;            /* Total work items */
    atomic_t next_item;         /* Next item to process (work stealing) */
    atomic_t completed;         /* Completed items */
    int chunk_size;             /* Items per chunk */
} parallel_work_t;

/* Thread pool state */
typedef struct {
    int num_threads;            /* Number of worker threads */
    volatile int shutdown;      /* Shutdown flag */

    /* Current work */
    parallel_work_t* current_work;
    atomic_t work_available;    /* Signal that work is available */
    atomic_t workers_done;      /* Count of workers that finished */

    /* Barrier for synchronization */
    atomic_t barrier_count;
    atomic_t barrier_phase;
} thread_pool_t;

/* Global thread pool */
static thread_pool_t g_pool = {0};
static int g_pool_initialized = 0;

/* Worker task handles */
static task_t* g_worker_tasks[PARALLEL_MAX_THREADS];

/* Core affinity configuration */
static int g_core_affinity[PARALLEL_MAX_THREADS];  /* Core ID for each thread (-1 = not set) */
static int g_core_pinning_enabled = 1;             /* Auto-pinning enabled by default */

/* Thread index tracking - maps task pointer to logical thread index */
static int g_worker_thread_indices[PARALLEL_MAX_THREADS];

/* Per-core timing statistics */
static core_timing_stats_t g_per_core_stats[PARALLEL_MAX_THREADS];

/* Deterministic mode for timing guarantees */
static int g_deterministic_mode = 0;               /* Deterministic scheduling disabled by default */

/* Forward declaration of worker entry function */
static void worker_thread_entry(void);

/* ============================================================================
 * Barrier Implementation
 * ============================================================================ */

/* Simple spinning barrier */
static void barrier_wait(int num_threads) {
    static atomic_t barrier_count = ATOMIC_INIT(0);
    static atomic_t barrier_phase = ATOMIC_INIT(0);

    int phase = atomic_read(&barrier_phase);

    if (atomic_inc_return(&barrier_count) == num_threads) {
        /* Last thread to arrive - reset and release */
        atomic_set(&barrier_count, 0);
        atomic_inc(&barrier_phase);
    } else {
        /* Wait for phase to change */
        while (atomic_read(&barrier_phase) == phase) {
            cpu_relax();
        }
    }
    smp_mb();
}

/* ============================================================================
 * Parallel Work Distribution
 * ============================================================================ */

/* Worker thread entry point - runs work-stealing loop */
static void worker_thread_entry(void) {
    task_t* self = get_current_task();

    /* Find our logical thread index */
    int thread_id = -1;
    for (int i = 1; i < PARALLEL_MAX_THREADS; i++) {
        if (g_worker_tasks[i] == self) {
            thread_id = i;
            break;
        }
    }

    if (thread_id < 0) {
        console_printf("[PARALLEL] ERROR: Worker task not found in task list\n");
        task_exit();
        return;
    }

    uint32_t core_id = task_get_cpu(self);
    console_printf("[PARALLEL] Worker %d started on core %u\n", thread_id, core_id);

    /* Initialize per-core statistics */
    g_per_core_stats[thread_id].core_id = core_id;
    g_per_core_stats[thread_id].total_cycles = 0;
    g_per_core_stats[thread_id].work_items = 0;
    g_per_core_stats[thread_id].idle_cycles = 0;
    g_per_core_stats[thread_id].invocations = 0;

    while (!g_pool.shutdown) {
        /* Track idle time while waiting for work */
        uint64_t idle_start = rdtsc();

        /* Wait for work to become available */
        while (!atomic_read(&g_pool.work_available) && !g_pool.shutdown) {
            task_yield();
        }

        uint64_t idle_end = rdtsc();
        g_per_core_stats[thread_id].idle_cycles += (idle_end - idle_start);

        if (g_pool.shutdown) break;

        /* Double-check work availability after wake-up */
        if (!atomic_read(&g_pool.work_available)) {
            continue;
        }

        parallel_work_t* work = g_pool.current_work;
        if (!work) {
            task_yield();
            continue;
        }

        /* Memory barrier to ensure we see the work descriptor */
        smp_rmb();

        /* Track work time and items */
        uint64_t work_start = rdtsc();
        uint64_t items_processed = 0;
        g_per_core_stats[thread_id].invocations++;

        /* Check if this is deterministic mode (chunk_size == -1 is the marker) */
        if (work->chunk_size == -1) {
            /* Deterministic mode: each thread gets a fixed range */
            int items_per_thread = work->total_items / g_pool.num_threads;
            int remainder = work->total_items % g_pool.num_threads;

            /* Calculate this thread's assigned range */
            int start = thread_id * items_per_thread + (thread_id < remainder ? thread_id : remainder);
            int end = start + items_per_thread + (thread_id < remainder ? 1 : 0);

            /* Execute only assigned work - no stealing */
            work->func(work->arg, thread_id, start, end);
            atomic_add(end - start, &work->completed);
            items_processed = (end - start);
        } else {
            /* Work-stealing loop - same pattern as main thread */
            while (1) {
                int start = atomic_add_return(work->chunk_size, &work->next_item) - work->chunk_size;
                if (start >= work->total_items) break;

                int end = start + work->chunk_size;
                if (end > work->total_items) end = work->total_items;

                /* Execute work chunk */
                work->func(work->arg, thread_id, start, end);

                atomic_add(end - start, &work->completed);
                items_processed += (end - start);
            }
        }

        uint64_t work_end = rdtsc();
        g_per_core_stats[thread_id].total_cycles += (work_end - work_start);
        g_per_core_stats[thread_id].work_items += items_processed;

        /* Signal this worker is done */
        atomic_inc(&g_pool.workers_done);
        smp_mb();

        /* Wait for work to be cleared before starting next iteration */
        while (atomic_read(&g_pool.work_available) && !g_pool.shutdown) {
            task_yield();
        }
    }

    console_printf("[PARALLEL] Worker %d (core %u) exiting\n", thread_id, core_id);
    task_exit();
}

/* Initialize thread pool */
int parallel_init(int num_threads) {
    if (g_pool_initialized) {
        return 0;
    }

    if (num_threads < 1) num_threads = 1;
    if (num_threads > PARALLEL_MAX_THREADS) num_threads = PARALLEL_MAX_THREADS;

    g_pool.num_threads = num_threads;
    g_pool.shutdown = 0;
    g_pool.current_work = NULL;
    atomic_set(&g_pool.work_available, 0);
    atomic_set(&g_pool.workers_done, 0);
    atomic_set(&g_pool.barrier_count, 0);
    atomic_set(&g_pool.barrier_phase, 0);

    /* Initialize core affinity tracking */
    for (int i = 0; i < PARALLEL_MAX_THREADS; i++) {
        g_core_affinity[i] = -1;  /* Not set */
    }

    /* Initialize per-core statistics */
    for (int i = 0; i < PARALLEL_MAX_THREADS; i++) {
        g_per_core_stats[i].total_cycles = 0;
        g_per_core_stats[i].work_items = 0;
        g_per_core_stats[i].idle_cycles = 0;
        g_per_core_stats[i].core_id = i;  /* Default to thread index */
        g_per_core_stats[i].invocations = 0;
    }

    /* Initialize main thread (thread 0) core ID */
    task_t* main_task = get_current_task();
    if (main_task) {
        g_per_core_stats[0].core_id = task_get_cpu(main_task);
    }

    /* Spawn worker threads (num_threads - 1, since main thread is worker 0) */
    for (int i = 1; i < num_threads; i++) {
        char worker_name[32];
        snprintf(worker_name, sizeof(worker_name), "worker_%d", i);

        g_worker_tasks[i] = task_create(worker_name, worker_thread_entry);
        if (!g_worker_tasks[i]) {
            console_printf("[PARALLEL] Failed to create worker %d\n", i);
            /* Clean up already created workers */
            g_pool.shutdown = 1;
            g_pool.num_threads = i;  /* Only count successfully created threads */
            break;
        }

        /* Pin worker to specific CPU if enabled */
        if (g_core_pinning_enabled) {
            int core_id = (g_core_affinity[i] >= 0) ? g_core_affinity[i] : i;
            task_pin_to_cpu(g_worker_tasks[i], core_id);
            g_core_affinity[i] = core_id;
            console_printf("[PARALLEL] Worker %d pinned to core %d\n", i, core_id);
        }
    }

    g_pool_initialized = 1;
    console_printf("[PARALLEL] Initialized with %d threads (core pinning %s)\n",
                   g_pool.num_threads, g_core_pinning_enabled ? "enabled" : "disabled");

    return 0;
}

/* Execute work in parallel using work-stealing pattern
 * This is the main entry point for parallel computation.
 * Works even with single thread (just runs sequentially).
 */
void parallel_for(work_func_t func, void* arg, int total_items, int chunk_size) {
    if (!g_pool_initialized || g_pool.num_threads <= 1) {
        /* Single-threaded fallback */
        func(arg, 0, 0, total_items);
        return;
    }

    /* In deterministic mode, use fixed work distribution for timing guarantees */
    if (g_deterministic_mode) {
        /* Set up work item with deterministic flag */
        parallel_work_t work = {
            .func = func,
            .arg = arg,
            .total_items = total_items,
            .chunk_size = -1  /* Special marker for deterministic mode */
        };
        atomic_set(&work.next_item, 0);
        atomic_set(&work.completed, 0);

        /* Reset worker completion counter */
        atomic_set(&g_pool.workers_done, 0);

        /* Publish work to workers */
        g_pool.current_work = &work;
        smp_wmb();  /* Ensure work descriptor is visible before signaling */

        /* Wake up worker threads */
        atomic_set(&g_pool.work_available, 1);
        smp_mb();  /* Ensure work_available is visible to all CPUs */

        /* Calculate fixed ranges for each thread */
        int items_per_thread = total_items / g_pool.num_threads;
        int remainder = total_items % g_pool.num_threads;

        /* Main thread (thread 0) processes its assigned range */
        int thread_id = 0;
        int start = 0;
        int end = items_per_thread + (remainder > 0 ? 1 : 0);

        /* Track main thread work time and items */
        uint64_t work_start = rdtsc();
        g_per_core_stats[thread_id].invocations++;

        /* Execute assigned work - no stealing allowed */
        func(arg, thread_id, start, end);
        atomic_add(end - start, &work.completed);

        uint64_t work_end = rdtsc();
        g_per_core_stats[thread_id].total_cycles += (work_end - work_start);
        g_per_core_stats[thread_id].work_items += (end - start);

        /* Wait for all work to complete */
        while (atomic_read(&work.completed) < total_items) {
            cpu_relax();
        }

        /* Wait for all worker threads to finish and acknowledge */
        int expected_workers = g_pool.num_threads - 1;
        while (atomic_read(&g_pool.workers_done) < expected_workers) {
            cpu_relax();
        }

        /* Clear work and signal workers */
        atomic_set(&g_pool.work_available, 0);
        smp_wmb();
        g_pool.current_work = NULL;
        smp_mb();
        return;
    }

    /* Standard work-stealing mode */
    /* Set up work item */
    parallel_work_t work = {
        .func = func,
        .arg = arg,
        .total_items = total_items,
        .chunk_size = chunk_size > 0 ? chunk_size : (total_items / g_pool.num_threads + 1)
    };
    atomic_set(&work.next_item, 0);
    atomic_set(&work.completed, 0);

    /* Reset worker completion counter (num_threads - 1 workers, main thread doesn't count) */
    atomic_set(&g_pool.workers_done, 0);

    /* Publish work to workers */
    g_pool.current_work = &work;
    smp_wmb();  /* Ensure work descriptor is visible before signaling */

    /* Wake up worker threads */
    atomic_set(&g_pool.work_available, 1);
    smp_mb();  /* Ensure work_available is visible to all CPUs */

    /* Main thread also participates as worker 0 */
    int thread_id = 0;

    /* Track main thread work time and items */
    uint64_t work_start = rdtsc();
    uint64_t items_processed = 0;
    g_per_core_stats[thread_id].invocations++;

    /* Work-stealing loop */
    while (1) {
        int start = atomic_add_return(work.chunk_size, &work.next_item) - work.chunk_size;
        if (start >= total_items) break;

        int end = start + work.chunk_size;
        if (end > total_items) end = total_items;

        /* Execute work chunk */
        func(arg, thread_id, start, end);

        atomic_add(end - start, &work.completed);
        items_processed += (end - start);
    }

    uint64_t work_end = rdtsc();
    g_per_core_stats[thread_id].total_cycles += (work_end - work_start);
    g_per_core_stats[thread_id].work_items += items_processed;

    /* Wait for all work to complete */
    while (atomic_read(&work.completed) < total_items) {
        cpu_relax();
    }

    /* Wait for all worker threads to finish and acknowledge */
    int expected_workers = g_pool.num_threads - 1;  /* Exclude main thread */
    while (atomic_read(&g_pool.workers_done) < expected_workers) {
        cpu_relax();
    }

    /* Clear work and signal workers */
    atomic_set(&g_pool.work_available, 0);
    smp_wmb();  /* Ensure work_available=0 is visible before clearing work */
    g_pool.current_work = NULL;
    smp_mb();
}

/* ============================================================================
 * Parallel Matrix Operations
 * ============================================================================ */

/* Arguments for parallel matmul */
typedef struct {
    float* out;
    const float* weights;
    const float* input;
    int rows;
    int cols;
} matmul_args_t;

/* Single-threaded row computation for matmul */
static void matmul_worker(void* arg, int thread_id, int start_row, int end_row) {
    matmul_args_t* args = (matmul_args_t*)arg;
    (void)thread_id;  /* Thread ID available for debugging */

    for (int r = start_row; r < end_row; r++) {
        float sum = 0.0f;
        const float* row = args->weights + r * args->cols;

        /* SIMD dot product would go here */
        for (int c = 0; c < args->cols; c++) {
            sum += row[c] * args->input[c];
        }

        args->out[r] = sum;
    }
}

/* Parallel matrix-vector multiply (float weights)
 * out[rows] = weights[rows, cols] @ input[cols]
 */
void parallel_matmul_f32(float* out, const float* weights, const float* input,
                          int rows, int cols) {
    matmul_args_t args = {
        .out = out,
        .weights = weights,
        .input = input,
        .rows = rows,
        .cols = cols
    };

    /* Use larger chunks to amortize overhead */
    int num_threads = g_pool.num_threads > 0 ? g_pool.num_threads : 1;
    int chunk_size = rows / num_threads;
    if (chunk_size < 16) chunk_size = 16;

    parallel_for(matmul_worker, &args, rows, chunk_size);
}

/* ============================================================================
 * Parallel Attention
 * ============================================================================ */

/* Arguments for parallel attention */
typedef struct {
    float* output;              /* [dim] */
    const float* q;             /* [dim] - query */
    const float* key_cache;     /* [seq_len, kv_dim] */
    const float* value_cache;   /* [seq_len, kv_dim] */
    float* att;                 /* [n_heads, seq_len] - attention scores */
    int n_heads;
    int n_kv_heads;
    int head_dim;
    int kv_dim;
    int seq_len;
    int pos;                    /* Current position */
} attention_args_t;

/* Compute attention for a single head */
static void attention_head_worker(void* arg, int thread_id, int start_head, int end_head) {
    attention_args_t* args = (attention_args_t*)arg;
    (void)thread_id;

    int kv_mul = args->n_heads / args->n_kv_heads;

    for (int h = start_head; h < end_head; h++) {
        const float* q_head = args->q + h * args->head_dim;
        float* att_head = args->att + h * args->seq_len;
        int kv_head = h / kv_mul;

        /* Compute attention scores: Q @ K^T / sqrt(head_dim) */
        float scale = 1.0f / sqrtf((float)args->head_dim);

        for (int t = 0; t <= args->pos; t++) {
            const float* k_t = args->key_cache + t * args->kv_dim + kv_head * args->head_dim;

            float score = 0.0f;
            for (int i = 0; i < args->head_dim; i++) {
                score += q_head[i] * k_t[i];
            }
            att_head[t] = score * scale;
        }

        /* Softmax over attention scores */
        float max_val = att_head[0];
        for (int t = 1; t <= args->pos; t++) {
            if (att_head[t] > max_val) max_val = att_head[t];
        }

        float sum = 0.0f;
        for (int t = 0; t <= args->pos; t++) {
            /* Use proper exp approximation */
            float e = fast_expf(att_head[t] - max_val);
            att_head[t] = e;
            sum += e;
        }

        float inv_sum = 1.0f / sum;
        for (int t = 0; t <= args->pos; t++) {
            att_head[t] *= inv_sum;
        }

        /* Weighted sum of values: att @ V */
        float* out_head = args->output + h * args->head_dim;
        for (int i = 0; i < args->head_dim; i++) {
            out_head[i] = 0.0f;
        }

        for (int t = 0; t <= args->pos; t++) {
            const float* v_t = args->value_cache + t * args->kv_dim + kv_head * args->head_dim;
            float att_w = att_head[t];

            for (int i = 0; i < args->head_dim; i++) {
                out_head[i] += att_w * v_t[i];
            }
        }
    }
}

/* Parallel multi-head attention */
void parallel_attention(float* output, const float* q,
                        const float* key_cache, const float* value_cache,
                        float* att, int n_heads, int n_kv_heads,
                        int head_dim, int kv_dim, int seq_len, int pos) {
    attention_args_t args = {
        .output = output,
        .q = q,
        .key_cache = key_cache,
        .value_cache = value_cache,
        .att = att,
        .n_heads = n_heads,
        .n_kv_heads = n_kv_heads,
        .head_dim = head_dim,
        .kv_dim = kv_dim,
        .seq_len = seq_len,
        .pos = pos
    };

    /* Parallelize across attention heads */
    parallel_for(attention_head_worker, &args, n_heads, 1);
}

/* ============================================================================
 * Parallel RMSNorm
 * ============================================================================ */

/* Arguments for parallel RMSNorm */
typedef struct {
    float* out;
    const float* x;
    const float* weight;
    int size;
    float scale;  /* Pre-computed 1/sqrt(sum_sq/n + eps) */
} rmsnorm_args_t;

static void rmsnorm_worker(void* arg, int thread_id, int start, int end) {
    rmsnorm_args_t* args = (rmsnorm_args_t*)arg;
    (void)thread_id;

    for (int i = start; i < end; i++) {
        args->out[i] = args->x[i] * args->scale * args->weight[i];
    }
}

/* Parallel RMSNorm - computes sum of squares, then applies normalization in parallel */
void parallel_rmsnorm(float* out, const float* x, const float* weight,
                       int size, float eps) {
    /* Phase 1: Compute sum of squares (sequential - small data) */
    float ss = 0.0f;
    for (int i = 0; i < size; i++) {
        ss += x[i] * x[i];
    }
    float scale = 1.0f / sqrtf(ss / size + eps);

    /* Phase 2: Apply normalization in parallel */
    rmsnorm_args_t args = {
        .out = out,
        .x = x,
        .weight = weight,
        .size = size,
        .scale = scale
    };

    int num_threads = g_pool.num_threads > 0 ? g_pool.num_threads : 1;
    parallel_for(rmsnorm_worker, &args, size, size / num_threads + 1);
}

/* ============================================================================
 * Parallel SiLU + Element-wise Multiply (for SwiGLU)
 * ============================================================================ */

typedef struct {
    float* gate;        /* In/out: gate values, result after SiLU */
    const float* up;    /* Up projection values */
    int size;
} swiglu_args_t;

static inline float silu_approx(float x) {
    /* silu(x) = x * sigmoid(x) = x / (1 + exp(-x)) */
    if (x < -10.0f) return 0.0f;
    if (x > 10.0f) return x;
    /* Use proper exp approximation */
    float neg_exp = fast_expf(-x);
    return x / (1.0f + neg_exp);
}

static void swiglu_worker(void* arg, int thread_id, int start, int end) {
    swiglu_args_t* args = (swiglu_args_t*)arg;
    (void)thread_id;

    for (int i = start; i < end; i++) {
        args->gate[i] = silu_approx(args->gate[i]) * args->up[i];
    }
}

/* Parallel SwiGLU: gate = silu(gate) * up */
void parallel_swiglu(float* gate, const float* up, int size) {
    swiglu_args_t args = {
        .gate = gate,
        .up = up,
        .size = size
    };

    int num_threads = g_pool.num_threads > 0 ? g_pool.num_threads : 1;
    parallel_for(swiglu_worker, &args, size, size / num_threads + 1);
}

/* ============================================================================
 * Get/Set Thread Count
 * ============================================================================ */

int parallel_get_num_threads(void) {
    return g_pool_initialized ? g_pool.num_threads : 1;
}

void parallel_set_num_threads(int n) {
    if (n < 1) n = 1;
    if (n > PARALLEL_MAX_THREADS) n = PARALLEL_MAX_THREADS;

    if (!g_pool_initialized) {
        parallel_init(n);
    } else {
        g_pool.num_threads = n;
        console_printf("[PARALLEL] Set thread count to %d\n", n);
    }
}

/* Shutdown thread pool */
void parallel_shutdown(void) {
    if (g_pool_initialized) {
        g_pool.shutdown = 1;
        g_pool_initialized = 0;
    }
}

/* ============================================================================
 * Core Affinity Configuration
 * ============================================================================ */

/* Set core affinity for a specific thread
 * thread_id: Thread index (0 to num_threads-1)
 * core_id: CPU core to pin to
 * Returns: 0 on success, -1 on error
 */
int parallel_set_core_affinity(int thread_id, int core_id) {
    if (thread_id < 0 || thread_id >= PARALLEL_MAX_THREADS) {
        console_printf("[PARALLEL] Invalid thread_id %d\n", thread_id);
        return -1;
    }

    if (core_id < 0) {
        console_printf("[PARALLEL] Invalid core_id %d\n", core_id);
        return -1;
    }

    /* Store affinity setting */
    g_core_affinity[thread_id] = core_id;

    /* If pool is initialized and thread exists, pin it now */
    if (g_pool_initialized && thread_id > 0 && thread_id < g_pool.num_threads) {
        if (g_worker_tasks[thread_id]) {
            task_pin_to_cpu(g_worker_tasks[thread_id], core_id);
            console_printf("[PARALLEL] Pinned thread %d to core %d\n", thread_id, core_id);
        }
    }

    return 0;
}

/* Enable or disable automatic core pinning
 * enable: 1 to enable, 0 to disable
 */
void parallel_pin_cores(int enable) {
    g_core_pinning_enabled = enable ? 1 : 0;
    console_printf("[PARALLEL] Core pinning %s\n",
                   g_core_pinning_enabled ? "enabled" : "disabled");
}

/* Enable or disable deterministic mode for timing guarantees
 * enable: 1 to enable, 0 to disable
 *
 * When enabled, uses fixed work distribution instead of work-stealing
 * to ensure consistent timing across multiple runs. This reduces
 * timing variance but may sacrifice some load balancing efficiency.
 */
void parallel_set_deterministic(int enable) {
    g_deterministic_mode = enable ? 1 : 0;
    console_printf("[PARALLEL] Deterministic mode %s\n",
                   g_deterministic_mode ? "enabled" : "disabled");

    /* In deterministic mode, core pinning should be enabled */
    if (g_deterministic_mode && !g_core_pinning_enabled) {
        console_printf("[PARALLEL] Warning: Enabling core pinning for deterministic mode\n");
        g_core_pinning_enabled = 1;
    }
}

/* ============================================================================
 * Per-Core Timing Statistics
 * ============================================================================ */

/* Get timing statistics for a specific thread
 * thread_id: Thread index (0 to num_threads-1)
 * stats: Output buffer for statistics
 * Returns: 0 on success, -1 on error
 */
int parallel_get_core_stats(int thread_id, core_timing_stats_t* stats) {
    if (!stats) {
        return -1;
    }

    if (thread_id < 0 || thread_id >= PARALLEL_MAX_THREADS) {
        console_printf("[PARALLEL] Invalid thread_id %d\n", thread_id);
        return -1;
    }

    /* Copy statistics atomically */
    stats->total_cycles = g_per_core_stats[thread_id].total_cycles;
    stats->work_items = g_per_core_stats[thread_id].work_items;
    stats->idle_cycles = g_per_core_stats[thread_id].idle_cycles;
    stats->core_id = g_per_core_stats[thread_id].core_id;
    stats->invocations = g_per_core_stats[thread_id].invocations;

    return 0;
}

/* Reset all per-core statistics */
void parallel_reset_core_stats(void) {
    for (int i = 0; i < PARALLEL_MAX_THREADS; i++) {
        uint32_t core_id = g_per_core_stats[i].core_id;
        g_per_core_stats[i].total_cycles = 0;
        g_per_core_stats[i].work_items = 0;
        g_per_core_stats[i].idle_cycles = 0;
        g_per_core_stats[i].invocations = 0;
        /* Preserve core_id */
        g_per_core_stats[i].core_id = core_id;
    }
    console_printf("[PARALLEL] Per-core statistics reset\n");
}

/* Print all core statistics to console */
void parallel_print_core_stats(void) {
    if (!g_pool_initialized) {
        console_printf("[PARALLEL] Thread pool not initialized\n");
        return;
    }

    console_printf("\n[PARALLEL] Per-Core Timing Statistics:\n");
    console_printf("==============================================\n");

    for (int i = 0; i < g_pool.num_threads; i++) {
        core_timing_stats_t* stats = &g_per_core_stats[i];

        console_printf("Thread %d (Core %u):\n", i, stats->core_id);
        console_printf("  Work cycles:  %llu\n", (unsigned long long)stats->total_cycles);
        console_printf("  Idle cycles:  %llu\n", (unsigned long long)stats->idle_cycles);
        console_printf("  Work items:   %llu\n", (unsigned long long)stats->work_items);
        console_printf("  Invocations:  %u\n", stats->invocations);

        /* Calculate utilization percentage (avoid division by zero) */
        uint64_t total_cycles = stats->total_cycles + stats->idle_cycles;
        if (total_cycles > 0) {
            /* Use integer math: utilization = (work_cycles * 100) / total_cycles */
            uint64_t utilization = (stats->total_cycles * 100) / total_cycles;
            console_printf("  Utilization:  %llu%%\n", (unsigned long long)utilization);
        } else {
            console_printf("  Utilization:  N/A\n");
        }

        console_printf("\n");
    }

    console_printf("==============================================\n");
}
