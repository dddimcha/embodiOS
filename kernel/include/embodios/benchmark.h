/* EMBODIOS Performance Benchmark Module
 *
 * Benchmarking infrastructure for validating performance targets.
 * Primary target: 85+ tokens/second for AI inference.
 *
 * Features:
 * - High-resolution timing using TSC
 * - Token throughput measurement
 * - Memory bandwidth benchmarks
 * - SIMD operation benchmarks
 * - Comprehensive performance reports
 */

#ifndef EMBODIOS_BENCHMARK_H
#define EMBODIOS_BENCHMARK_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Performance Targets
 * ============================================================================ */

#define PERF_TARGET_TOKENS_PER_SEC      85      /* Target: 85+ tok/s */
#define PERF_TARGET_BOOT_TIME_MS        1600    /* Target: 1.6s boot */
#define PERF_TARGET_MEMORY_MB           64      /* Target: 64MB RAM */

/* ============================================================================
 * Timing Utilities (Architecture-specific)
 * ============================================================================ */

#if defined(__x86_64__) || defined(__i386__)

/* x86: Read Time Stamp Counter */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* x86: Read TSC with memory barrier */
static inline uint64_t rdtscp(void)
{
    uint32_t lo, hi, aux;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return ((uint64_t)hi << 32) | lo;
}

#elif defined(__aarch64__)

/* ARM64: Read CNTVCT_EL0 (Virtual Counter) */
static inline uint64_t rdtsc(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

/* ARM64: Read counter with memory barrier */
static inline uint64_t rdtscp(void)
{
    uint64_t val;
    __asm__ volatile("isb" ::: "memory");
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

#else

/* Fallback: no high-resolution timer */
static inline uint64_t rdtsc(void) { return 0; }
static inline uint64_t rdtscp(void) { return 0; }

#endif

/* ============================================================================
 * Benchmark Results
 * ============================================================================ */

typedef struct benchmark_result {
    const char *name;           /* Benchmark name */
    uint64_t cycles;            /* Total cycles */
    uint64_t iterations;        /* Number of iterations */
    uint64_t cycles_per_iter;   /* Average cycles per iteration */
    double ops_per_sec;         /* Operations per second */
    bool passed;                /* Met target? */
} benchmark_result_t;

typedef struct inference_benchmark {
    uint64_t total_tokens;      /* Total tokens generated */
    uint64_t total_cycles;      /* Total CPU cycles */
    uint64_t total_time_us;     /* Total time in microseconds */
    double tokens_per_sec;      /* Tokens per second */
    double avg_latency_ms;      /* Average latency per token */
    uint64_t peak_memory;       /* Peak memory usage */
    bool target_met;            /* Met 85+ tok/s target */
} inference_benchmark_t;

typedef struct memory_benchmark {
    uint64_t read_bandwidth;    /* MB/s read bandwidth */
    uint64_t write_bandwidth;   /* MB/s write bandwidth */
    uint64_t copy_bandwidth;    /* MB/s copy bandwidth */
    uint64_t latency_ns;        /* Memory latency in ns */
} memory_benchmark_t;

typedef struct simd_benchmark {
    uint64_t scalar_gflops;     /* Scalar GFLOPS */
    uint64_t sse_gflops;        /* SSE GFLOPS */
    uint64_t avx_gflops;        /* AVX2 GFLOPS */
    double speedup_sse;         /* SSE vs scalar speedup */
    double speedup_avx;         /* AVX vs scalar speedup */
} simd_benchmark_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize benchmark module
 * Calibrates TSC frequency
 * @return 0 on success
 */
int benchmark_init(void);

/**
 * Get TSC frequency in Hz
 * @return Frequency in Hz
 */
uint64_t benchmark_get_tsc_freq(void);

/**
 * Convert cycles to microseconds
 * @param cycles Number of cycles
 * @return Time in microseconds
 */
uint64_t benchmark_cycles_to_us(uint64_t cycles);

/**
 * Convert cycles to milliseconds
 * @param cycles Number of cycles
 * @return Time in milliseconds
 */
uint64_t benchmark_cycles_to_ms(uint64_t cycles);

/**
 * Run inference benchmark
 * Tests token generation throughput
 * @param result Output benchmark results
 * @param num_tokens Number of tokens to generate
 * @return 0 on success
 */
int benchmark_inference(inference_benchmark_t *result, int num_tokens);

/**
 * Run memory bandwidth benchmark
 * Tests memory read/write/copy speeds
 * @param result Output benchmark results
 * @return 0 on success
 */
int benchmark_memory(memory_benchmark_t *result);

/**
 * Run SIMD operation benchmark
 * Tests scalar vs SIMD performance
 * @param result Output benchmark results
 * @return 0 on success
 */
int benchmark_simd(simd_benchmark_t *result);

/**
 * Run matrix multiplication benchmark
 * @param result Output result
 * @param size Matrix size (N x N)
 * @return GFLOPS achieved
 */
double benchmark_matmul(benchmark_result_t *result, int size);

/**
 * Run complete benchmark suite
 * Tests all performance aspects
 * @return Number of targets met
 */
int benchmark_run_all(void);

/**
 * Print benchmark results
 */
void benchmark_print_results(void);

/**
 * Validate performance targets
 * @return true if all targets met
 */
bool benchmark_validate_targets(void);

/**
 * Quick performance check
 * Fast sanity test of system performance
 * @return 0 if performance is acceptable
 */
int benchmark_quick_check(void);

/**
 * Run REAL GGUF inference benchmark
 * Uses streaming inference engine with actual model
 * @param result Output benchmark results
 * @param prompt Test prompt to use
 * @param max_tokens Maximum tokens to generate
 * @return 0 on success, -1 on error
 */
int benchmark_gguf_inference(inference_benchmark_t *result,
                              const char *prompt,
                              int max_tokens);

/**
 * Validate GGUF model performance
 * Runs comprehensive validation against performance targets
 * @param model_name Name of model being tested (for logging)
 * @return Number of tests passed
 */
int benchmark_validate_gguf_model(const char *model_name);

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_BENCHMARK_H */
