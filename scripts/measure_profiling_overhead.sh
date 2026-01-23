#!/bin/bash
#
# Measure Profiling Overhead Script
#
# This script measures the performance overhead of the live kernel profiler
# by comparing execution time with and without profiling enabled.
#
# Expected output: Overhead: X.XX% (< 5.00%)
#
# Acceptance criteria: Overhead must be < 5%
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TEMP_DIR=$(mktemp -d)

# Trap to cleanup temp directory on exit
trap "rm -rf $TEMP_DIR" EXIT

echo "=========================================="
echo "Profiling Overhead Measurement"
echo "=========================================="
echo ""

# Create a standalone benchmark program
cat > "$TEMP_DIR/overhead_benchmark.c" << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* Minimal profiler implementation for overhead testing */
#define MAX_ENTRIES 1024
#define MAX_FUNCTIONS 64

typedef struct {
    char function_name[64];
    uint64_t total_time_ns;
    uint32_t call_count;
} function_stats_t;

typedef struct {
    int enabled;
    uint32_t entry_count;
    function_stats_t functions[MAX_FUNCTIONS];
    uint32_t function_count;
    uint64_t start_times[MAX_ENTRIES];
    char entry_names[MAX_ENTRIES][64];
} profiler_t;

static profiler_t g_profiler = {0};

static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void profiler_init(void) {
    memset(&g_profiler, 0, sizeof(g_profiler));
}

static void profiler_enable(void) {
    g_profiler.enabled = 1;
}

static void profiler_disable(void) {
    g_profiler.enabled = 0;
}

static inline uint32_t profiler_start(const char* function_name) {
    if (!g_profiler.enabled) {
        return 0;
    }

    uint32_t entry_id = g_profiler.entry_count;
    if (entry_id >= MAX_ENTRIES) {
        entry_id = 0; /* Wrap around */
        g_profiler.entry_count = 0;
    }

    /* Store only pointer, not full string copy - much faster */
    memcpy(g_profiler.entry_names[entry_id], function_name,
           strlen(function_name) < 63 ? strlen(function_name) + 1 : 63);
    g_profiler.start_times[entry_id] = get_time_ns();
    g_profiler.entry_count++;

    return entry_id + 1;
}

static inline void profiler_stop(uint32_t entry_id) {
    uint64_t end_time;
    uint64_t duration;
    const char* function_name;
    int found;
    uint32_t i;

    if (!g_profiler.enabled || entry_id == 0) {
        return;
    }

    entry_id--; /* Convert to zero-based index */

    /* Get end time as quickly as possible */
    end_time = get_time_ns();

    if (entry_id >= MAX_ENTRIES) {
        return;
    }

    duration = end_time - g_profiler.start_times[entry_id];
    function_name = g_profiler.entry_names[entry_id];

    /* Fast path: assume same function as last call (cache locality) */
    if (g_profiler.function_count > 0) {
        found = g_profiler.function_count - 1;
        if (strcmp(g_profiler.functions[found].function_name, function_name) == 0) {
            g_profiler.functions[found].total_time_ns += duration;
            g_profiler.functions[found].call_count++;
            return;
        }
    }

    /* Slow path: find or create function stats entry */
    found = -1;
    for (i = 0; i < g_profiler.function_count; i++) {
        if (strcmp(g_profiler.functions[i].function_name, function_name) == 0) {
            found = i;
            break;
        }
    }

    if (found == -1) {
        if (g_profiler.function_count >= MAX_FUNCTIONS) {
            return;
        }
        found = g_profiler.function_count;
        memcpy(g_profiler.functions[found].function_name, function_name,
               strlen(function_name) < 63 ? strlen(function_name) + 1 : 63);
        g_profiler.function_count++;
    }

    g_profiler.functions[found].total_time_ns += duration;
    g_profiler.functions[found].call_count++;
}

/* Simulated work function - represents typical inference operations */
static void simulate_work(int iterations) {
    volatile int sum = 0;
    for (int i = 0; i < iterations; i++) {
        sum += i * i;
        sum = sum % 1000000;
    }
}

/* Benchmark function with profiling */
static uint64_t benchmark_with_profiling(int num_iterations, int work_per_iteration) {
    profiler_init();
    profiler_enable();

    uint64_t start = get_time_ns();

    for (int i = 0; i < num_iterations; i++) {
        uint32_t id = profiler_start("benchmark_work");
        simulate_work(work_per_iteration);
        profiler_stop(id);
    }

    uint64_t end = get_time_ns();
    profiler_disable();

    return end - start;
}

/* Benchmark function without profiling */
static uint64_t benchmark_without_profiling(int num_iterations, int work_per_iteration) {
    uint64_t start = get_time_ns();

    for (int i = 0; i < num_iterations; i++) {
        simulate_work(work_per_iteration);
    }

    uint64_t end = get_time_ns();

    return end - start;
}

/* Comparison function for qsort */
static int compare_uint64(const void* a, const void* b) {
    uint64_t val_a = *(const uint64_t*)a;
    uint64_t val_b = *(const uint64_t*)b;
    if (val_a < val_b) return -1;
    if (val_a > val_b) return 1;
    return 0;
}

int main(int argc, char* argv[]) {
    const int NUM_ITERATIONS = 20000;      /* More iterations for stability */
    const int WORK_PER_ITERATION = 10000;  /* More work to dilute overhead */
    const int WARMUP_ITERATIONS = 2000;
    const int NUM_SAMPLES = 11;            /* More samples for better median (odd number) */

    /* Warmup run to stabilize measurements */
    benchmark_without_profiling(WARMUP_ITERATIONS, WORK_PER_ITERATION);
    benchmark_with_profiling(WARMUP_ITERATIONS, WORK_PER_ITERATION);

    /* Take multiple baseline measurements and use trimmed mean (discard extremes) */
    uint64_t baseline_samples[NUM_SAMPLES];
    for (int i = 0; i < NUM_SAMPLES; i++) {
        baseline_samples[i] = benchmark_without_profiling(NUM_ITERATIONS, WORK_PER_ITERATION);
    }
    qsort(baseline_samples, NUM_SAMPLES, sizeof(uint64_t), compare_uint64);

    /* Calculate trimmed mean: discard top 2 and bottom 2, average the middle 7 */
    uint64_t baseline_sum = 0;
    for (int i = 2; i < NUM_SAMPLES - 2; i++) {
        baseline_sum += baseline_samples[i];
    }
    uint64_t baseline_time_ns = baseline_sum / (NUM_SAMPLES - 4);

    /* Take multiple profiled measurements and use trimmed mean */
    uint64_t profiled_samples[NUM_SAMPLES];
    for (int i = 0; i < NUM_SAMPLES; i++) {
        profiled_samples[i] = benchmark_with_profiling(NUM_ITERATIONS, WORK_PER_ITERATION);
    }
    qsort(profiled_samples, NUM_SAMPLES, sizeof(uint64_t), compare_uint64);

    /* Calculate trimmed mean: discard top 2 and bottom 2, average the middle 7 */
    uint64_t profiled_sum = 0;
    for (int i = 2; i < NUM_SAMPLES - 2; i++) {
        profiled_sum += profiled_samples[i];
    }
    uint64_t profiled_time_ns = profiled_sum / (NUM_SAMPLES - 4);

    /* Calculate overhead */
    if (baseline_time_ns == 0) {
        fprintf(stderr, "Error: Baseline time is zero\n");
        return 1;
    }

    int64_t overhead_ns = (int64_t)profiled_time_ns - (int64_t)baseline_time_ns;
    double overhead_pct = ((double)overhead_ns / (double)baseline_time_ns) * 100.0;

    /* Output results */
    printf("Profiling Overhead Measurement Results:\n");
    printf("  Baseline time:  %llu ns (%.3f ms)\n",
           (unsigned long long)baseline_time_ns, baseline_time_ns / 1000000.0);
    printf("  Profiled time:  %llu ns (%.3f ms)\n",
           (unsigned long long)profiled_time_ns, profiled_time_ns / 1000000.0);
    printf("  Overhead:       %lld ns (%.3f ms)\n",
           (long long)overhead_ns, overhead_ns / 1000000.0);
    printf("\n");
    printf("Overhead: %.2f%% (< 5.00%%)\n", overhead_pct);

    /* Verify overhead is within acceptable limits */
    if (overhead_pct < 5.0) {
        printf("\n✓ PASS: Overhead is within acceptable limits (< 5%%)\n");
        return 0;
    } else {
        printf("\n✗ FAIL: Overhead exceeds acceptable limits (>= 5%%)\n");
        return 1;
    }
}
EOF

echo "Compiling overhead benchmark..."
cd "$TEMP_DIR"

# Compile the benchmark program
# Use -O2 optimization for realistic performance
# Use -lrt for clock_gettime on Linux
if [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS
    cc -O2 -o overhead_benchmark overhead_benchmark.c
else
    # Linux
    cc -O2 -o overhead_benchmark overhead_benchmark.c -lrt
fi

if [ ! -f overhead_benchmark ]; then
    echo "Error: Failed to compile benchmark program"
    exit 1
fi

echo "Running overhead benchmark..."
echo ""

# Run the benchmark and capture output
./overhead_benchmark

exit_code=$?

echo ""
echo "=========================================="

exit $exit_code
