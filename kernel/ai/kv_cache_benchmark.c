/* SPDX-License-Identifier: GPL-2.0 */
/**
 * EMBODIOS KV Cache Benchmark
 *
 * Compares transformer attention performance:
 * - Without KV cache (recompute K/V each time)
 * - With KV cache (lookup cached K/V)
 *
 * Target: ~2x speedup for autoregressive generation
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/kv_cache_enhanced.h>

/* ============================================================================
 * Timer Interface
 * ============================================================================ */

#if defined(__x86_64__) || defined(__i386__)

static inline uint64_t read_cycles(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#elif defined(__aarch64__)

static inline uint64_t read_cycles(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

#else

static inline uint64_t read_cycles(void)
{
    static uint64_t counter = 0;
    return counter++;
}

#endif

/* ============================================================================
 * Benchmark Configuration
 * ============================================================================ */

#define BENCH_N_LAYERS          12      /* Number of transformer layers */
#define BENCH_N_HEADS           8       /* Query heads */
#define BENCH_N_KV_HEADS        8       /* KV heads (same for standard attention) */
#define BENCH_HEAD_DIM          64      /* Dimension per head */
#define BENCH_N_EMBD            (BENCH_N_HEADS * BENCH_HEAD_DIM)  /* 512 */
#define BENCH_MAX_SEQ_LEN       512     /* Max sequence length */
#define BENCH_WARMUP_ITERS      10
#define BENCH_MEASURE_ITERS     100

/* ============================================================================
 * Benchmark Results
 * ============================================================================ */

typedef struct {
    uint64_t no_cache_cycles;       /* Cycles without cache */
    uint64_t with_cache_cycles;     /* Cycles with cache */
    float speedup;                  /* Speedup factor */
    float improvement_percent;      /* Improvement percentage */
    uint32_t seq_lengths_tested;    /* Number of sequence lengths tested */
    bool passed;                    /* Whether target was achieved */
} kv_benchmark_results_t;

/* ============================================================================
 * Simulated Attention Operations
 * ============================================================================ */

/* External sqrtf for attention scaling */
float sqrtf(float x);

/**
 * Simulates K/V computation from input (the expensive operation we're caching)
 */
static void compute_kv_vectors(const float* input,
                               const float* w_k,
                               const float* w_v,
                               float* k_out,
                               float* v_out,
                               int n_embd)
{
    /* Simplified: K = input * W_k (just do element-wise for benchmark) */
    for (int i = 0; i < n_embd; i++) {
        k_out[i] = input[i] * w_k[i] + input[(i + 1) % n_embd] * w_k[(i + 1) % n_embd];
        v_out[i] = input[i] * w_v[i] + input[(i + 1) % n_embd] * w_v[(i + 1) % n_embd];
    }
}

/**
 * Simulates attention score computation (Q @ K^T)
 */
static float compute_attention_score(const float* q,
                                     const float* k,
                                     int head_dim)
{
    float score = 0.0f;
    for (int i = 0; i < head_dim; i++) {
        score += q[i] * k[i];
    }
    return score / sqrtf((float)head_dim);
}

/**
 * Simulates attention without KV cache (recomputes K/V each time)
 */
static void attention_no_cache(const float* x,            /* Input [seq_len, n_embd] */
                               const float* w_k,          /* Key weights */
                               const float* w_v,          /* Value weights */
                               int seq_len,
                               int n_heads,
                               int head_dim,
                               float* output)             /* [seq_len, n_embd] */
{
    int n_embd = n_heads * head_dim;
    float* k_temp = (float*)heap_alloc(n_embd * sizeof(float));
    float* v_temp = (float*)heap_alloc(n_embd * sizeof(float));
    float* q_temp = (float*)heap_alloc(n_embd * sizeof(float));

    if (!k_temp || !v_temp || !q_temp) {
        if (k_temp) heap_free(k_temp);
        if (v_temp) heap_free(v_temp);
        if (q_temp) heap_free(q_temp);
        return;
    }

    for (int pos = 0; pos < seq_len; pos++) {
        const float* x_pos = &x[pos * n_embd];

        /* Compute Q for current position */
        for (int i = 0; i < n_embd; i++) {
            q_temp[i] = x_pos[i];  /* Simplified Q = x */
        }

        /* Without cache: recompute K/V for ALL previous positions */
        float total_score = 0.0f;
        for (int prev_pos = 0; prev_pos <= pos; prev_pos++) {
            const float* x_prev = &x[prev_pos * n_embd];

            /* Recompute K/V for this position (expensive!) */
            compute_kv_vectors(x_prev, w_k, w_v, k_temp, v_temp, n_embd);

            /* Compute attention score */
            for (int h = 0; h < n_heads; h++) {
                float score = compute_attention_score(
                    &q_temp[h * head_dim],
                    &k_temp[h * head_dim],
                    head_dim);
                total_score += score;
            }
        }

        /* Write some output (simplified) */
        for (int i = 0; i < n_embd; i++) {
            output[pos * n_embd + i] = total_score * 0.01f;
        }
    }

    heap_free(q_temp);
    heap_free(v_temp);
    heap_free(k_temp);
}

/**
 * Simulates attention with KV cache (looks up cached K/V)
 */
static void attention_with_cache(const float* x,           /* Input [seq_len, n_embd] */
                                 const float* w_k,         /* Key weights */
                                 const float* w_v,         /* Value weights */
                                 kv_cache_t* cache,
                                 int layer,
                                 int seq_len,
                                 int n_heads,
                                 int head_dim,
                                 float* output)            /* [seq_len, n_embd] */
{
    int n_embd = n_heads * head_dim;
    float* k_temp = (float*)heap_alloc(n_embd * sizeof(float));
    float* v_temp = (float*)heap_alloc(n_embd * sizeof(float));
    float* q_temp = (float*)heap_alloc(n_embd * sizeof(float));

    if (!k_temp || !v_temp || !q_temp) {
        if (k_temp) heap_free(k_temp);
        if (v_temp) heap_free(v_temp);
        if (q_temp) heap_free(q_temp);
        return;
    }

    for (int pos = 0; pos < seq_len; pos++) {
        const float* x_pos = &x[pos * n_embd];

        /* Compute Q for current position */
        for (int i = 0; i < n_embd; i++) {
            q_temp[i] = x_pos[i];
        }

        /* Compute K/V only for current position and cache it */
        compute_kv_vectors(x_pos, w_k, w_v, k_temp, v_temp, n_embd);
        kv_cache_store_f32(cache, layer, pos, k_temp, v_temp);

        /* With cache: lookup cached K/V for previous positions */
        float total_score = 0.0f;
        const float* cached_keys = kv_cache_get_key_ptr_f32(cache, layer);

        /* Validate cached_keys is not NULL */
        if (!cached_keys) {
            heap_free(q_temp);
            heap_free(v_temp);
            heap_free(k_temp);
            return;
        }

        for (int prev_pos = 0; prev_pos <= pos; prev_pos++) {
            /* Get cached K from direct pointer (fast!) */
            const float* cached_k = &cached_keys[prev_pos * n_embd];

            /* Compute attention score using cached K */
            for (int h = 0; h < n_heads; h++) {
                float score = compute_attention_score(
                    &q_temp[h * head_dim],
                    &cached_k[h * head_dim],
                    head_dim);
                total_score += score;
            }
        }

        /* Write output */
        for (int i = 0; i < n_embd; i++) {
            output[pos * n_embd + i] = total_score * 0.01f;
        }
    }

    heap_free(q_temp);
    heap_free(v_temp);
    heap_free(k_temp);
}

/* ============================================================================
 * Benchmark Implementation
 * ============================================================================ */

/**
 * Initialize test data with pseudo-random values
 */
static void init_test_data(float* data, size_t count, uint32_t seed)
{
    for (size_t i = 0; i < count; i++) {
        seed = seed * 1103515245 + 12345;
        data[i] = ((float)(seed >> 16) / 32768.0f - 1.0f) * 0.1f;
    }
}

/**
 * Run the benchmark comparing cached vs non-cached attention
 */
kv_benchmark_results_t run_kv_cache_benchmark(void)
{
    kv_benchmark_results_t results = {0};

    console_printf("\n");
    console_printf("============================================================\n");
    console_printf("           KV CACHE PERFORMANCE BENCHMARK\n");
    console_printf("============================================================\n");
    console_printf("\n");

    /* Allocate test data */
    size_t input_size = BENCH_MAX_SEQ_LEN * BENCH_N_EMBD * sizeof(float);
    size_t weight_size = BENCH_N_EMBD * sizeof(float);

    float* input = (float*)heap_alloc(input_size);
    float* w_k = (float*)heap_alloc(weight_size);
    float* w_v = (float*)heap_alloc(weight_size);
    float* output = (float*)heap_alloc(input_size);

    if (!input || !w_k || !w_v || !output) {
        console_printf("[Benchmark] ERROR: Failed to allocate test buffers\n");
        results.passed = false;
        goto cleanup;
    }

    console_printf("[Benchmark] Configuration:\n");
    console_printf("  Layers:        %d\n", BENCH_N_LAYERS);
    console_printf("  Heads:         %d\n", BENCH_N_HEADS);
    console_printf("  KV Heads:      %d\n", BENCH_N_KV_HEADS);
    console_printf("  Head Dim:      %d\n", BENCH_HEAD_DIM);
    console_printf("  Embedding:     %d\n", BENCH_N_EMBD);
    console_printf("  Max Seq Len:   %d\n", BENCH_MAX_SEQ_LEN);
    console_printf("\n");

    /* Initialize test data */
    console_printf("[Benchmark] Initializing test data...\n");
    init_test_data(input, BENCH_MAX_SEQ_LEN * BENCH_N_EMBD, 12345);
    init_test_data(w_k, BENCH_N_EMBD, 67890);
    init_test_data(w_v, BENCH_N_EMBD, 11111);

    /* Create KV cache */
    console_printf("[Benchmark] Creating KV cache...\n");
    kv_cache_config_t config = {
        .n_layers = BENCH_N_LAYERS,
        .n_kv_heads = BENCH_N_KV_HEADS,
        .head_dim = BENCH_HEAD_DIM,
        .max_seq_len = BENCH_MAX_SEQ_LEN,
        .window_size = 0,  /* No eviction for benchmark */
        .data_type = KV_TYPE_FLOAT32,
        .eviction = KV_EVICT_NONE
    };

    kv_cache_t* cache = kv_cache_create(&config);
    if (!cache) {
        console_printf("[Benchmark] ERROR: Failed to create KV cache\n");
        results.passed = false;
        goto cleanup;
    }

    /* Test different sequence lengths */
    int test_seq_lens[] = {16, 32, 64, 128, 256};
    int n_tests = sizeof(test_seq_lens) / sizeof(test_seq_lens[0]);

    uint64_t total_no_cache = 0;
    uint64_t total_with_cache = 0;

    console_printf("\n[Benchmark] Running tests...\n\n");
    console_printf("Seq Len    No Cache (cycles)    With Cache (cycles)    Speedup\n");
    console_printf("-------    -----------------    -------------------    -------\n");

    for (int t = 0; t < n_tests; t++) {
        int seq_len = test_seq_lens[t];

        /* Warmup */
        for (int w = 0; w < BENCH_WARMUP_ITERS; w++) {
            kv_cache_reset(cache);
            attention_with_cache(input, w_k, w_v, cache, 0, seq_len,
                               BENCH_N_HEADS, BENCH_HEAD_DIM, output);
        }

        /* Benchmark without cache */
        uint64_t start_no_cache = read_cycles();
        for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
            attention_no_cache(input, w_k, w_v, seq_len,
                             BENCH_N_HEADS, BENCH_HEAD_DIM, output);
        }
        uint64_t end_no_cache = read_cycles();
        uint64_t no_cache_cycles = end_no_cache - start_no_cache;

        /* Benchmark with cache */
        uint64_t start_with_cache = read_cycles();
        for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
            kv_cache_reset(cache);
            attention_with_cache(input, w_k, w_v, cache, 0, seq_len,
                               BENCH_N_HEADS, BENCH_HEAD_DIM, output);
        }
        uint64_t end_with_cache = read_cycles();
        uint64_t with_cache_cycles = end_with_cache - start_with_cache;

        /* Calculate speedup */
        float speedup = (float)no_cache_cycles / (float)with_cache_cycles;

        console_printf("%-10d %-20lu %-22lu %.2fx\n",
                      seq_len,
                      (unsigned long)(no_cache_cycles / BENCH_MEASURE_ITERS),
                      (unsigned long)(with_cache_cycles / BENCH_MEASURE_ITERS),
                      speedup);

        total_no_cache += no_cache_cycles;
        total_with_cache += with_cache_cycles;
        results.seq_lengths_tested++;
    }

    /* Calculate overall results */
    results.no_cache_cycles = total_no_cache;
    results.with_cache_cycles = total_with_cache;
    results.speedup = (float)total_no_cache / (float)total_with_cache;
    results.improvement_percent = (results.speedup - 1.0f) * 100.0f;
    results.passed = (results.speedup >= 1.5f);  /* Pass if >= 1.5x */

    /* Print summary */
    console_printf("\n");
    console_printf("============================================================\n");
    console_printf("                    BENCHMARK RESULTS\n");
    console_printf("============================================================\n");
    console_printf("\n");
    console_printf("Overall Performance:\n");
    console_printf("  Speedup:          %.2fx\n", results.speedup);
    console_printf("  Improvement:      %.1f%%\n", results.improvement_percent);
    console_printf("  Target (2x):      %s\n",
                  results.speedup >= 2.0f ? "ACHIEVED" : "NOT YET");
    console_printf("  Minimum (1.5x):   %s\n",
                  results.passed ? "PASSED" : "FAILED");
    console_printf("\n");

    /* Print cache statistics */
    kv_cache_print_stats(cache);

    console_printf("\n");
    console_printf("============================================================\n");
    console_printf("                    BENCHMARK COMPLETE\n");
    console_printf("============================================================\n");
    console_printf("\n");

    kv_cache_destroy(cache);

cleanup:
    if (output) heap_free(output);
    if (w_v) heap_free(w_v);
    if (w_k) heap_free(w_k);
    if (input) heap_free(input);

    return results;
}

/**
 * kv_cache_benchmark - Run benchmark with specified iterations
 */
void kv_cache_benchmark(uint32_t iterations)
{
    (void)iterations;  /* Use built-in iteration count */
    run_kv_cache_benchmark();
}

/**
 * kv_cache_benchmark_command - Command interface for benchmark
 */
void kv_cache_benchmark_command(void)
{
    kv_benchmark_results_t results = run_kv_cache_benchmark();

    if (results.passed) {
        console_printf("\nKV Cache Benchmark PASSED: %.2fx speedup achieved\n",
                      results.speedup);
    } else {
        console_printf("\nKV Cache Benchmark: %.2fx speedup (target: 2x)\n",
                      results.speedup);
    }
}
