/* SPDX-License-Identifier: GPL-2.0 */
/**
 * EMBODIOS Embedding Benchmark
 *
 * Compares embedding lookup performance:
 * - Direct computation (old method)
 * - Pre-computed cache (new method)
 *
 * Target: ~15% speedup (1.15x)
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/embeddings.h>

/* ============================================================================
 * Timer Interface
 * ============================================================================ */

/* Architecture-specific cycle counter */
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

#define BENCH_WARMUP_ITERS      100
#define BENCH_MEASURE_ITERS     10000
#define BENCH_VOCAB_SIZE        32000
#define BENCH_EMBEDDING_DIM     2048
#define BENCH_MAX_SEQ_LEN       2048

/* ============================================================================
 * Direct Computation (Baseline)
 * ============================================================================ */

/**
 * Simulates direct embedding lookup without cache
 * This is what the old code path does
 */
static void direct_embedding_lookup(
    const float* token_embeddings,
    const float* position_embeddings,
    uint32_t token_id,
    uint32_t position,
    uint32_t dim,
    float* output)
{
    const float* tok = &token_embeddings[(size_t)token_id * dim];
    const float* pos = &position_embeddings[(size_t)position * dim];

    for (uint32_t i = 0; i < dim; i++) {
        output[i] = tok[i] + pos[i];
    }
}

/* ============================================================================
 * Benchmark Results Structure
 * ============================================================================ */

typedef struct {
    uint64_t direct_cycles;
    uint64_t cached_cycles;
    float speedup;
    float improvement_percent;
    uint32_t iterations;
    bool passed;
} benchmark_results_t;

/* ============================================================================
 * Benchmark Implementation
 * ============================================================================ */

/**
 * run_embedding_benchmark - Compare direct vs cached embedding lookup
 *
 * Measures performance difference between:
 * 1. Direct computation from weight arrays
 * 2. Pre-computed embedding cache lookup
 *
 * Return: Benchmark results
 */
benchmark_results_t run_embedding_benchmark(void)
{
    benchmark_results_t results = {0};

    console_printf("\n");
    console_printf("============================================================\n");
    console_printf("         EMBEDDING CACHE PERFORMANCE BENCHMARK\n");
    console_printf("============================================================\n");
    console_printf("\n");

    /* Initialize pointers for cleanup */
    float* token_embeddings = NULL;
    float* position_embeddings = NULL;
    float* output_direct = NULL;
    float* output_cached = NULL;
    embedding_cache_t* cache = NULL;

    /* Allocate test data */
    size_t token_size = (size_t)BENCH_VOCAB_SIZE * BENCH_EMBEDDING_DIM * sizeof(float);
    size_t pos_size = (size_t)BENCH_MAX_SEQ_LEN * BENCH_EMBEDDING_DIM * sizeof(float);

    token_embeddings = (float*)heap_alloc(token_size);
    position_embeddings = (float*)heap_alloc(pos_size);
    output_direct = (float*)heap_alloc(BENCH_EMBEDDING_DIM * sizeof(float));
    output_cached = (float*)heap_alloc(BENCH_EMBEDDING_DIM * sizeof(float));

    if (!token_embeddings || !position_embeddings || !output_direct || !output_cached) {
        console_printf("[Benchmark] ERROR: Failed to allocate test data\n");
        results.passed = false;
        goto cleanup;
    }

    console_printf("[Benchmark] Allocated test buffers:\n");
    console_printf("  Token embeddings: %zu KB\n", token_size / 1024);
    console_printf("  Position embeddings: %zu KB\n", pos_size / 1024);

    /* Initialize with random values */
    console_printf("[Benchmark] Initializing test data...\n");
    uint32_t seed = 12345;
    for (size_t i = 0; i < (size_t)BENCH_VOCAB_SIZE * BENCH_EMBEDDING_DIM; i++) {
        seed = seed * 1103515245 + 12345;
        token_embeddings[i] = ((float)(seed >> 16) / 32768.0f - 1.0f) * 0.02f;
    }
    for (size_t i = 0; i < (size_t)BENCH_MAX_SEQ_LEN * BENCH_EMBEDDING_DIM; i++) {
        seed = seed * 1103515245 + 12345;
        position_embeddings[i] = ((float)(seed >> 16) / 32768.0f - 1.0f) * 0.02f;
    }

    /* Create embedding cache */
    console_printf("[Benchmark] Creating embedding cache...\n");
    embedding_config_t config = {
        .vocab_size = BENCH_VOCAB_SIZE,
        .embedding_dim = BENCH_EMBEDDING_DIM,
        .max_seq_len = BENCH_MAX_SEQ_LEN,
        .cache_positions = 128,
        .use_position_emb = true,
        .use_combined_cache = true
    };

    cache = embedding_cache_init(&config);
    if (!cache) {
        console_printf("[Benchmark] ERROR: Failed to create cache\n");
        results.passed = false;
        goto cleanup;
    }

    /* Copy test data to cache */
    memcpy(cache->token_embeddings, token_embeddings, token_size);
    if (cache->position_embeddings) {
        memcpy(cache->position_embeddings, position_embeddings, pos_size);
    }

    /* Pre-compute cache */
    embedding_cache_precompute(cache);

    /* Warm up */
    console_printf("[Benchmark] Warming up (%d iterations)...\n", BENCH_WARMUP_ITERS);
    for (int i = 0; i < BENCH_WARMUP_ITERS; i++) {
        uint32_t tok = i % BENCH_VOCAB_SIZE;
        uint32_t pos = i % BENCH_MAX_SEQ_LEN;
        direct_embedding_lookup(token_embeddings, position_embeddings,
                                tok, pos, BENCH_EMBEDDING_DIM, output_direct);
        embedding_lookup(cache, tok, pos, output_cached);
    }

    /* Benchmark direct computation */
    console_printf("[Benchmark] Measuring direct computation (%d iterations)...\n",
                   BENCH_MEASURE_ITERS);

    uint64_t start_direct = read_cycles();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        uint32_t tok = (i * 7) % BENCH_VOCAB_SIZE;  /* Pseudo-random access */
        uint32_t pos = (i * 13) % BENCH_MAX_SEQ_LEN;
        direct_embedding_lookup(token_embeddings, position_embeddings,
                                tok, pos, BENCH_EMBEDDING_DIM, output_direct);
    }
    uint64_t end_direct = read_cycles();
    results.direct_cycles = end_direct - start_direct;

    /* Benchmark cached lookup */
    console_printf("[Benchmark] Measuring cached lookup (%d iterations)...\n",
                   BENCH_MEASURE_ITERS);

    embedding_reset_stats(cache);

    uint64_t start_cached = read_cycles();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        uint32_t tok = (i * 7) % BENCH_VOCAB_SIZE;
        uint32_t pos = (i * 13) % BENCH_MAX_SEQ_LEN;
        embedding_lookup(cache, tok, pos, output_cached);
    }
    uint64_t end_cached = read_cycles();
    results.cached_cycles = end_cached - start_cached;

    /* Calculate results */
    results.iterations = BENCH_MEASURE_ITERS;
    results.speedup = (float)results.direct_cycles / (float)results.cached_cycles;
    results.improvement_percent = (results.speedup - 1.0f) * 100.0f;
    results.passed = (results.speedup >= 1.10f);  /* Pass if >= 10% improvement */

    /* Print results */
    console_printf("\n");
    console_printf("============================================================\n");
    console_printf("                    BENCHMARK RESULTS\n");
    console_printf("============================================================\n");
    console_printf("\n");
    console_printf("Configuration:\n");
    console_printf("  Vocab size:       %u\n", BENCH_VOCAB_SIZE);
    console_printf("  Embedding dim:    %u\n", BENCH_EMBEDDING_DIM);
    console_printf("  Max seq length:   %u\n", BENCH_MAX_SEQ_LEN);
    console_printf("  Iterations:       %u\n", BENCH_MEASURE_ITERS);
    console_printf("\n");
    console_printf("Timing (CPU cycles):\n");
    console_printf("  Direct method:    %lu total, %lu avg\n",
                   (unsigned long)results.direct_cycles,
                   (unsigned long)(results.direct_cycles / BENCH_MEASURE_ITERS));
    console_printf("  Cached method:    %lu total, %lu avg\n",
                   (unsigned long)results.cached_cycles,
                   (unsigned long)(results.cached_cycles / BENCH_MEASURE_ITERS));
    console_printf("\n");
    console_printf("Performance:\n");
    console_printf("  Speedup:          %.2fx\n", results.speedup);
    console_printf("  Improvement:      %.1f%%\n", results.improvement_percent);
    console_printf("  Target (15%%):     %s\n",
                   results.improvement_percent >= 15.0f ? "ACHIEVED" : "NOT YET");
    console_printf("  Minimum (10%%):    %s\n",
                   results.passed ? "PASSED" : "FAILED");
    console_printf("\n");

    /* Print cache statistics */
    console_printf("Cache Statistics:\n");
    embedding_print_stats(cache);

    /* Verify correctness */
    console_printf("\n[Benchmark] Verifying correctness...\n");
    bool correct = true;
    for (int i = 0; i < 100; i++) {
        uint32_t tok = (i * 31) % BENCH_VOCAB_SIZE;
        uint32_t pos = (i * 17) % BENCH_MAX_SEQ_LEN;

        direct_embedding_lookup(token_embeddings, position_embeddings,
                                tok, pos, BENCH_EMBEDDING_DIM, output_direct);
        embedding_lookup(cache, tok, pos, output_cached);

        for (uint32_t j = 0; j < BENCH_EMBEDDING_DIM; j++) {
            float diff = output_direct[j] - output_cached[j];
            if (diff > 0.0001f || diff < -0.0001f) {
                console_printf("[Benchmark] ERROR: Mismatch at token=%u pos=%u dim=%u\n",
                               tok, pos, j);
                console_printf("  Direct: %f, Cached: %f\n",
                               output_direct[j], output_cached[j]);
                correct = false;
                break;
            }
        }
        if (!correct) break;
    }

    if (correct) {
        console_printf("[Benchmark] Correctness: VERIFIED\n");
    } else {
        console_printf("[Benchmark] Correctness: FAILED\n");
        results.passed = false;
    }

    console_printf("\n");
    console_printf("============================================================\n");
    console_printf("                    BENCHMARK COMPLETE\n");
    console_printf("============================================================\n");
    console_printf("\n");

cleanup:
    /* Cleanup all allocated resources */
    if (cache) embedding_cache_destroy(cache);
    if (output_cached) heap_free(output_cached);
    if (output_direct) heap_free(output_direct);
    if (position_embeddings) heap_free(position_embeddings);
    if (token_embeddings) heap_free(token_embeddings);

    return results;
}

/**
 * embedding_benchmark_command - Command interface for benchmark
 *
 * Can be called from the EMBODIOS command processor
 */
void embedding_benchmark_command(void)
{
    benchmark_results_t results = run_embedding_benchmark();

    if (results.passed) {
        console_printf("\nBenchmark PASSED: %.1f%% improvement achieved\n",
                       results.improvement_percent);
    } else {
        console_printf("\nBenchmark FAILED: Only %.1f%% improvement\n",
                       results.improvement_percent);
    }
}

/**
 * embedding_quick_benchmark - Quick benchmark for testing
 *
 * Uses smaller iteration count for faster results
 */
void embedding_quick_benchmark(void)
{
    console_printf("[Benchmark] Running quick embedding test...\n");

    embedding_cache_t* cache = embedding_get_global();
    if (!cache) {
        console_printf("[Benchmark] No global embedding cache\n");
        return;
    }

    /* Run quick benchmark */
    uint64_t avg_time = embedding_benchmark(cache, 1000);

    console_printf("[Benchmark] Average lookup: %lu ns\n", (unsigned long)avg_time);
}
