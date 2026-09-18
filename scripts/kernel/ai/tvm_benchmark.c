/* SPDX-License-Identifier: GPL-2.0 */
/**
 * EMBODIOS TVM Runtime Benchmark
 *
 * Benchmarks TVM runtime performance including:
 * - Individual operator performance (dense, relu, softmax)
 * - Graph execution overhead
 * - End-to-end MLP inference throughput
 *
 * Target: Match standalone TVM performance within 10%
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/tvm.h>

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

#define BENCH_WARMUP_ITERS      10
#define BENCH_MEASURE_ITERS     100

/* Dense layer sizes */
#define DENSE_BATCH_SIZE        1
#define DENSE_INPUT_DIM         512
#define DENSE_OUTPUT_DIM        512

/* ReLU sizes */
#define RELU_SIZE               (512 * 512)

/* Softmax sizes */
#define SOFTMAX_SIZE            1000

/* MLP configuration */
#define MLP_INPUT_DIM           512
#define MLP_HIDDEN_DIM          1024
#define MLP_OUTPUT_DIM          512

/* ============================================================================
 * Benchmark Results
 * ============================================================================ */

typedef struct {
    const char* operation;
    uint64_t cycles;
    float cycles_per_iter;
    float gflops;
    bool passed;
} tvm_op_benchmark_t;

typedef struct {
    tvm_op_benchmark_t dense_result;
    tvm_op_benchmark_t relu_result;
    tvm_op_benchmark_t softmax_result;
    tvm_op_benchmark_t mlp_result;
    bool overall_passed;
} tvm_benchmark_results_t;

/* ============================================================================
 * Tensor Helper Functions
 * ============================================================================ */

/**
 * Create a test tensor and fill with random-ish data
 */
static TVMTensor* create_test_tensor(int64_t* shape, int ndim, int dtype)
{
    TVMTensor* tensor = tvm_tensor_create(shape, ndim, dtype);
    if (!tensor) return NULL;

    /* Calculate total elements */
    int64_t size = 1;
    for (int i = 0; i < ndim; i++) {
        size *= shape[i];
    }

    /* Fill with test data (simple pattern) */
    float* data = (float*)tensor->data;
    for (int64_t i = 0; i < size; i++) {
        data[i] = (float)(i % 100) * 0.01f;
    }

    return tensor;
}

/* ============================================================================
 * Operation Benchmark Functions
 * ============================================================================ */

/* Forward declarations */
void tensor_dense_forward(TVMTensor* input, TVMTensor* weight,
                         TVMTensor* bias, TVMTensor* output);
void tensor_relu_forward(TVMTensor* input, TVMTensor* output);
void tensor_softmax_forward(TVMTensor* input, TVMTensor* output);

/**
 * Benchmark dense (matrix multiplication) operation
 */
static tvm_op_benchmark_t benchmark_tvm_dense(void)
{
    tvm_op_benchmark_t result = {
        .operation = "Dense Layer",
        .cycles = 0,
        .cycles_per_iter = 0.0f,
        .gflops = 0.0f,
        .passed = false
    };

    console_printf("  [TVM] Benchmarking Dense Layer (%dx%d x %dx%d)...\n",
                   DENSE_BATCH_SIZE, DENSE_INPUT_DIM,
                   DENSE_INPUT_DIM, DENSE_OUTPUT_DIM);

    /* Create tensors */
    int64_t input_shape[] = {DENSE_BATCH_SIZE, DENSE_INPUT_DIM};
    int64_t weight_shape[] = {DENSE_OUTPUT_DIM, DENSE_INPUT_DIM};
    int64_t bias_shape[] = {DENSE_OUTPUT_DIM};
    int64_t output_shape[] = {DENSE_BATCH_SIZE, DENSE_OUTPUT_DIM};

    TVMTensor* input = create_test_tensor(input_shape, 2, TVM_DTYPE_FLOAT32);
    TVMTensor* weight = create_test_tensor(weight_shape, 2, TVM_DTYPE_FLOAT32);
    TVMTensor* bias = create_test_tensor(bias_shape, 1, TVM_DTYPE_FLOAT32);
    TVMTensor* output = create_test_tensor(output_shape, 2, TVM_DTYPE_FLOAT32);

    if (!input || !weight || !bias || !output) {
        console_printf("    ERROR: Failed to allocate tensors\n");
        if (input) tvm_tensor_free(input);
        if (weight) tvm_tensor_free(weight);
        if (bias) tvm_tensor_free(bias);
        if (output) tvm_tensor_free(output);
        return result;
    }

    /* Warmup */
    for (int i = 0; i < BENCH_WARMUP_ITERS; i++) {
        tensor_dense_forward(input, weight, bias, output);
    }

    /* Measure */
    uint64_t start = read_cycles();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        tensor_dense_forward(input, weight, bias, output);
    }
    uint64_t end = read_cycles();

    /* Calculate results */
    result.cycles = end - start;
    result.cycles_per_iter = (float)result.cycles / BENCH_MEASURE_ITERS;

    /* FLOPS = 2 * M * N * K (multiply-add) */
    int64_t flops_per_iter = 2 * DENSE_BATCH_SIZE * DENSE_OUTPUT_DIM * DENSE_INPUT_DIM;
    float total_gflops = (float)flops_per_iter * BENCH_MEASURE_ITERS / (result.cycles / 2.4e9f) / 1e9f;
    result.gflops = total_gflops / BENCH_MEASURE_ITERS;

    result.passed = (result.cycles_per_iter < 10000000);  /* Reasonable threshold */

    console_printf("    Cycles: %llu total, %.2f per iteration\n",
                   result.cycles, result.cycles_per_iter);
    console_printf("    Performance: %.2f GFLOPS\n", result.gflops);

    /* Cleanup */
    tvm_tensor_free(input);
    tvm_tensor_free(weight);
    tvm_tensor_free(bias);
    tvm_tensor_free(output);

    return result;
}

/**
 * Benchmark ReLU activation
 */
static tvm_op_benchmark_t benchmark_tvm_relu(void)
{
    tvm_op_benchmark_t result = {
        .operation = "ReLU Activation",
        .cycles = 0,
        .cycles_per_iter = 0.0f,
        .gflops = 0.0f,
        .passed = false
    };

    console_printf("  [TVM] Benchmarking ReLU (%d elements)...\n", RELU_SIZE);

    /* Create tensors */
    int64_t shape[] = {RELU_SIZE};
    TVMTensor* input = create_test_tensor(shape, 1, TVM_DTYPE_FLOAT32);
    TVMTensor* output = create_test_tensor(shape, 1, TVM_DTYPE_FLOAT32);

    if (!input || !output) {
        console_printf("    ERROR: Failed to allocate tensors\n");
        if (input) tvm_tensor_free(input);
        if (output) tvm_tensor_free(output);
        return result;
    }

    /* Warmup */
    for (int i = 0; i < BENCH_WARMUP_ITERS; i++) {
        tensor_relu_forward(input, output);
    }

    /* Measure */
    uint64_t start = read_cycles();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        tensor_relu_forward(input, output);
    }
    uint64_t end = read_cycles();

    /* Calculate results */
    result.cycles = end - start;
    result.cycles_per_iter = (float)result.cycles / BENCH_MEASURE_ITERS;
    result.passed = (result.cycles_per_iter < 5000000);

    console_printf("    Cycles: %llu total, %.2f per iteration\n",
                   result.cycles, result.cycles_per_iter);

    /* Cleanup */
    tvm_tensor_free(input);
    tvm_tensor_free(output);

    return result;
}

/**
 * Benchmark Softmax activation
 */
static tvm_op_benchmark_t benchmark_tvm_softmax(void)
{
    tvm_op_benchmark_t result = {
        .operation = "Softmax Activation",
        .cycles = 0,
        .cycles_per_iter = 0.0f,
        .gflops = 0.0f,
        .passed = false
    };

    console_printf("  [TVM] Benchmarking Softmax (%d elements)...\n", SOFTMAX_SIZE);

    /* Create tensors */
    int64_t shape[] = {1, SOFTMAX_SIZE};
    TVMTensor* input = create_test_tensor(shape, 2, TVM_DTYPE_FLOAT32);
    TVMTensor* output = create_test_tensor(shape, 2, TVM_DTYPE_FLOAT32);

    if (!input || !output) {
        console_printf("    ERROR: Failed to allocate tensors\n");
        if (input) tvm_tensor_free(input);
        if (output) tvm_tensor_free(output);
        return result;
    }

    /* Warmup */
    for (int i = 0; i < BENCH_WARMUP_ITERS; i++) {
        tensor_softmax_forward(input, output);
    }

    /* Measure */
    uint64_t start = read_cycles();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        tensor_softmax_forward(input, output);
    }
    uint64_t end = read_cycles();

    /* Calculate results */
    result.cycles = end - start;
    result.cycles_per_iter = (float)result.cycles / BENCH_MEASURE_ITERS;
    result.passed = (result.cycles_per_iter < 10000000);

    console_printf("    Cycles: %llu total, %.2f per iteration\n",
                   result.cycles, result.cycles_per_iter);

    /* Cleanup */
    tvm_tensor_free(input);
    tvm_tensor_free(output);

    return result;
}

/**
 * Benchmark end-to-end MLP inference
 */
static tvm_op_benchmark_t benchmark_tvm_mlp_inference(void)
{
    tvm_op_benchmark_t result = {
        .operation = "MLP Inference",
        .cycles = 0,
        .cycles_per_iter = 0.0f,
        .gflops = 0.0f,
        .passed = false
    };

    console_printf("  [TVM] Benchmarking MLP Inference (%d -> %d -> %d)...\n",
                   MLP_INPUT_DIM, MLP_HIDDEN_DIM, MLP_OUTPUT_DIM);

    /* Create MLP graph */
    tvm_graph_executor_t* executor = tvm_create_mlp_graph(
        MLP_INPUT_DIM, MLP_HIDDEN_DIM, MLP_OUTPUT_DIM);

    if (!executor) {
        console_printf("    ERROR: Failed to create MLP graph\n");
        return result;
    }

    /* Warmup */
    for (int i = 0; i < BENCH_WARMUP_ITERS; i++) {
        tvm_graph_execute(executor);
    }

    /* Measure */
    uint64_t start = read_cycles();
    for (int i = 0; i < BENCH_MEASURE_ITERS; i++) {
        tvm_graph_execute(executor);
    }
    uint64_t end = read_cycles();

    /* Calculate results */
    result.cycles = end - start;
    result.cycles_per_iter = (float)result.cycles / BENCH_MEASURE_ITERS;
    result.passed = (result.cycles_per_iter < 50000000);

    /* Calculate throughput */
    float inferences_per_second = 2.4e9f / result.cycles_per_iter;  /* Assume 2.4 GHz */

    console_printf("    Cycles: %llu total, %.2f per iteration\n",
                   result.cycles, result.cycles_per_iter);
    console_printf("    Throughput: %.2f inferences/sec\n", inferences_per_second);

    /* Cleanup */
    tvm_graph_executor_free(executor);

    return result;
}

/* ============================================================================
 * Main Benchmark Function
 * ============================================================================ */

/**
 * Run all TVM benchmarks and report results
 */
void tvm_run_benchmark(void)
{
    console_printf("\n=== TVM Runtime Performance Benchmark ===\n");
    console_printf("Warmup iterations: %d\n", BENCH_WARMUP_ITERS);
    console_printf("Measurement iterations: %d\n\n", BENCH_MEASURE_ITERS);

    tvm_benchmark_results_t results;

    /* Run individual operation benchmarks */
    results.dense_result = benchmark_tvm_dense();
    results.relu_result = benchmark_tvm_relu();
    results.softmax_result = benchmark_tvm_softmax();

    console_printf("\n");

    /* Run end-to-end benchmark */
    results.mlp_result = benchmark_tvm_mlp_inference();

    /* Overall pass/fail */
    results.overall_passed = (
        results.dense_result.passed &&
        results.relu_result.passed &&
        results.softmax_result.passed &&
        results.mlp_result.passed
    );

    /* Print summary */
    console_printf("\n=== Benchmark Summary ===\n");
    console_printf("Dense Layer:   %s (%.2f GFLOPS)\n",
                   results.dense_result.passed ? "PASS" : "FAIL",
                   results.dense_result.gflops);
    console_printf("ReLU:          %s\n",
                   results.relu_result.passed ? "PASS" : "FAIL");
    console_printf("Softmax:       %s\n",
                   results.softmax_result.passed ? "PASS" : "FAIL");
    console_printf("MLP Inference: %s\n",
                   results.mlp_result.passed ? "PASS" : "FAIL");
    console_printf("\nOverall: %s\n",
                   results.overall_passed ? "PASS" : "FAIL");
    console_printf("=====================================\n\n");
}
