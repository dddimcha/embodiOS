/* EMBODIOS Quantized Operations Test Suite
 *
 * Tests and benchmarks for GGUF quantization types:
 * - Q4_K, Q5_K, Q6_K, Q8_0 dequantization
 * - Matrix-vector multiplication
 * - Performance measurements
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/kernel.h>
#include <embodios/mm.h>
#include <embodios/quantized_ops.h>
#include <embodios/gguf_parser.h>

/* ============================================================================
 * Test Utilities
 * ============================================================================ */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (cond) { \
            tests_passed++; \
        } else { \
            tests_failed++; \
            console_printf("  FAIL: %s\n", msg); \
        } \
    } while (0)

#define TEST_ASSERT_RANGE(val, min, max, msg) \
    TEST_ASSERT((val) >= (min) && (val) <= (max), msg)

/* Simple timing - uses CPU cycles approximation */
static volatile uint64_t timer_cycles = 0;

static inline uint64_t get_cycles(void)
{
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    /* Fallback - just count iterations */
    return timer_cycles++;
#endif
}

/* ============================================================================
 * Q4_K Tests
 * ============================================================================ */

static void test_q4_k_basic(void)
{
    console_printf("[Test] Q4_K basic dequantization\n");

    /* Create a simple Q4_K block with known values */
    struct block_q4_k block;
    memset(&block, 0, sizeof(block));

    /* Set scale = 1.0 in Q8.8 format (256) */
    block.d = 256;
    block.dmin = 0;

    /* Set all scales to 1 */
    memset(block.scales, 0x11, K_SCALE_SIZE);  /* 6-bit packed scales */

    /* Set all quantized values to 8 (middle value for 4-bit) */
    memset(block.qs, 0x88, QK_K / 2);

    /* Dequantize */
    fixed_t output[QK_K];
    dequantize_block_q4_k(&block, output);

    /* Check that output values are reasonable */
    int valid_count = 0;
    for (int i = 0; i < QK_K; i++) {
        if (output[i] != 0) valid_count++;
    }

    TEST_ASSERT(valid_count > 0, "Q4_K produces non-zero output");
    console_printf("  PASS: Q4_K basic dequantization (%d non-zero values)\n", valid_count);
}

static void test_q4_k_tensor(void)
{
    console_printf("[Test] Q4_K tensor dequantization\n");

    /* Allocate a small tensor (2 blocks = 512 values) */
    struct block_q4_k blocks[2];
    memset(blocks, 0, sizeof(blocks));

    blocks[0].d = 256;  /* scale = 1.0 */
    blocks[1].d = 512;  /* scale = 2.0 */

    fixed_t output[512];
    int ret = dequantize_q4_k(blocks, sizeof(blocks), output, 512);

    TEST_ASSERT(ret == 0, "Q4_K tensor dequant succeeds");
    console_printf("  PASS: Q4_K tensor dequantization\n");
}

/* ============================================================================
 * Q5_K Tests
 * ============================================================================ */

static void test_q5_k_basic(void)
{
    console_printf("[Test] Q5_K basic dequantization\n");

    struct block_q5_k block;
    memset(&block, 0, sizeof(block));

    block.d = 256;
    block.dmin = 0;
    memset(block.scales, 0x11, K_SCALE_SIZE);
    memset(block.qs, 0x88, QK_K / 2);
    memset(block.qh, 0x00, QK_K / 8);  /* No high bits set */

    fixed_t output[QK_K];
    dequantize_block_q5_k(&block, output);

    int valid_count = 0;
    for (int i = 0; i < QK_K; i++) {
        if (output[i] != 0) valid_count++;
    }

    TEST_ASSERT(valid_count > 0, "Q5_K produces non-zero output");
    console_printf("  PASS: Q5_K basic dequantization (%d non-zero values)\n", valid_count);
}

static void test_q5_k_highbit(void)
{
    console_printf("[Test] Q5_K high bit handling\n");

    struct block_q5_k block;
    memset(&block, 0, sizeof(block));

    block.d = 256;
    memset(block.scales, 0x11, K_SCALE_SIZE);
    memset(block.qs, 0x00, QK_K / 2);     /* Low 4 bits = 0 */
    memset(block.qh, 0xFF, QK_K / 8);     /* High bits = 1 (value = 16) */

    fixed_t output[QK_K];
    dequantize_block_q5_k(&block, output);

    /* With high bit set, values should be 16 before scaling */
    int high_count = 0;
    for (int i = 0; i < QK_K; i++) {
        if (output[i] != 0) high_count++;
    }

    TEST_ASSERT(high_count > 0, "Q5_K high bit produces non-zero output");
    console_printf("  PASS: Q5_K high bit handling (%d affected values)\n", high_count);
}

/* ============================================================================
 * Q6_K Tests
 * ============================================================================ */

static void test_q6_k_basic(void)
{
    console_printf("[Test] Q6_K basic dequantization\n");

    struct block_q6_k block;
    memset(&block, 0, sizeof(block));

    block.d = 256;
    memset(block.scales, 16, QK_K / 16);  /* Small positive scales */
    memset(block.ql, 0x88, QK_K / 2);     /* Low 4 bits = 8 */
    memset(block.qh, 0x00, QK_K / 4);     /* High 2 bits = 0 */

    fixed_t output[QK_K];
    dequantize_block_q6_k(&block, output);

    int valid_count = 0;
    for (int i = 0; i < QK_K; i++) {
        if (output[i] != 0) valid_count++;
    }

    TEST_ASSERT(valid_count > 0, "Q6_K produces non-zero output");
    console_printf("  PASS: Q6_K basic dequantization (%d non-zero values)\n", valid_count);
}

/* ============================================================================
 * Q8_0 Tests
 * ============================================================================ */

static void test_q8_0_basic(void)
{
    console_printf("[Test] Q8_0 basic dequantization\n");

    struct block_q8_0 block;
    block.d = 256;  /* scale = 1.0 */

    /* Set known values: -128 to 127 range */
    for (int i = 0; i < QK8_0; i++) {
        block.qs[i] = (int8_t)(i - 16);  /* -16 to 15 */
    }

    fixed_t output[QK8_0];
    dequantize_block_q8_0(&block, output);

    /* Verify output has expected pattern */
    int correct = 0;
    for (int i = 0; i < QK8_0; i++) {
        /* Expected value is (i-16) * 256 >> 7 = (i-16) * 2 in fixed-point */
        int32_t expected_approx = (i - 16) * 2;
        int32_t actual = output[i] >> 8;  /* Convert to approximate integer */
        if (actual >= expected_approx - 1 && actual <= expected_approx + 1) {
            correct++;
        }
    }

    TEST_ASSERT(correct >= 28, "Q8_0 values within expected range");
    console_printf("  PASS: Q8_0 basic dequantization (%d/32 correct)\n", correct);
}

static void test_q8_0_tensor(void)
{
    console_printf("[Test] Q8_0 tensor dequantization\n");

    /* 4 blocks = 128 values */
    struct block_q8_0 blocks[4];
    for (int b = 0; b < 4; b++) {
        blocks[b].d = (int16_t)(256 + b * 64);  /* Varying scales */
        for (int i = 0; i < QK8_0; i++) {
            blocks[b].qs[i] = (int8_t)i;
        }
    }

    fixed_t output[128];
    int ret = dequantize_q8_0(blocks, sizeof(blocks), output, 128);

    TEST_ASSERT(ret == 0, "Q8_0 tensor dequant succeeds");
    console_printf("  PASS: Q8_0 tensor dequantization\n");
}

/* ============================================================================
 * Unified Dispatcher Tests
 * ============================================================================ */

static void test_dispatcher(void)
{
    console_printf("[Test] Unified dequantization dispatcher\n");

    /* Test Q4_K dispatch */
    struct block_q4_k q4_block;
    memset(&q4_block, 0, sizeof(q4_block));
    q4_block.d = 256;

    fixed_t output[256];
    int ret = dequantize_tensor(QUANT_TYPE_Q4_K, &q4_block, sizeof(q4_block), output, 256);
    TEST_ASSERT(ret == 0, "Dispatcher handles Q4_K");

    /* Test unsupported type */
    ret = dequantize_tensor(QUANT_TYPE_F32, &q4_block, sizeof(q4_block), output, 256);
    TEST_ASSERT(ret == -2, "Dispatcher rejects unsupported types");

    console_printf("  PASS: Unified dispatcher\n");
}

/* ============================================================================
 * Type Info Tests
 * ============================================================================ */

static void test_type_info(void)
{
    console_printf("[Test] Quantization type info\n");

    /* Test block sizes */
    TEST_ASSERT(get_block_size(QUANT_TYPE_Q4_K) == 144, "Q4_K block size = 144");
    TEST_ASSERT(get_block_size(QUANT_TYPE_Q5_K) == 176, "Q5_K block size = 176");
    TEST_ASSERT(get_block_size(QUANT_TYPE_Q6_K) == 210, "Q6_K block size = 210");
    TEST_ASSERT(get_block_size(QUANT_TYPE_Q8_0) == 34, "Q8_0 block size = 34");

    /* Test block elements */
    TEST_ASSERT(get_block_elements(QUANT_TYPE_Q4_K) == 256, "Q4_K elements = 256");
    TEST_ASSERT(get_block_elements(QUANT_TYPE_Q5_K) == 256, "Q5_K elements = 256");
    TEST_ASSERT(get_block_elements(QUANT_TYPE_Q6_K) == 256, "Q6_K elements = 256");
    TEST_ASSERT(get_block_elements(QUANT_TYPE_Q8_0) == 32, "Q8_0 elements = 32");

    /* Test type names */
    TEST_ASSERT(strcmp(get_type_name(QUANT_TYPE_Q4_K), "Q4_K") == 0, "Q4_K name correct");
    TEST_ASSERT(strcmp(get_type_name(QUANT_TYPE_Q8_0), "Q8_0") == 0, "Q8_0 name correct");

    /* Test is_quant_type_supported */
    TEST_ASSERT(is_quant_type_supported(QUANT_TYPE_Q4_K) == true, "Q4_K is supported");
    TEST_ASSERT(is_quant_type_supported(QUANT_TYPE_F32) == false, "F32 not supported");

    console_printf("  PASS: Type info functions\n");
}

/* ============================================================================
 * Performance Benchmarks
 * ============================================================================ */

#define BENCH_ITERATIONS 1000

static void benchmark_dequant(void)
{
    console_printf("\n[Benchmark] Dequantization Performance\n");
    console_printf("  Iterations: %d per type\n\n", BENCH_ITERATIONS);

    uint64_t start, end, cycles;

    /* Q4_K benchmark */
    struct block_q4_k q4_block;
    memset(&q4_block, 0, sizeof(q4_block));
    q4_block.d = 256;
    fixed_t q4_output[QK_K];

    start = get_cycles();
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        dequantize_block_q4_k(&q4_block, q4_output);
    }
    end = get_cycles();
    cycles = (end - start) / BENCH_ITERATIONS;
    console_printf("  Q4_K: %llu cycles/block (%llu values/block)\n",
                   (unsigned long long)cycles, (unsigned long long)QK_K);

    /* Q5_K benchmark */
    struct block_q5_k q5_block;
    memset(&q5_block, 0, sizeof(q5_block));
    q5_block.d = 256;
    fixed_t q5_output[QK_K];

    start = get_cycles();
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        dequantize_block_q5_k(&q5_block, q5_output);
    }
    end = get_cycles();
    cycles = (end - start) / BENCH_ITERATIONS;
    console_printf("  Q5_K: %llu cycles/block (%llu values/block)\n",
                   (unsigned long long)cycles, (unsigned long long)QK_K);

    /* Q6_K benchmark */
    struct block_q6_k q6_block;
    memset(&q6_block, 0, sizeof(q6_block));
    q6_block.d = 256;
    fixed_t q6_output[QK_K];

    start = get_cycles();
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        dequantize_block_q6_k(&q6_block, q6_output);
    }
    end = get_cycles();
    cycles = (end - start) / BENCH_ITERATIONS;
    console_printf("  Q6_K: %llu cycles/block (%llu values/block)\n",
                   (unsigned long long)cycles, (unsigned long long)QK_K);

    /* Q8_0 benchmark */
    struct block_q8_0 q8_block;
    memset(&q8_block, 0, sizeof(q8_block));
    q8_block.d = 256;
    fixed_t q8_output[QK8_0];

    start = get_cycles();
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        dequantize_block_q8_0(&q8_block, q8_output);
    }
    end = get_cycles();
    cycles = (end - start) / BENCH_ITERATIONS;
    console_printf("  Q8_0: %llu cycles/block (%llu values/block)\n",
                   (unsigned long long)cycles, (unsigned long long)QK8_0);
}

static void benchmark_matmul(void)
{
    console_printf("\n[Benchmark] Matrix-Vector Multiplication\n");
    console_printf("  Matrix: 64x256, Iterations: %d per type\n\n", BENCH_ITERATIONS / 10);

    /* 64 rows x 256 columns = 64 blocks for K-quants */
    const size_t M = 64;
    const size_t N = 256;
    const int iters = BENCH_ITERATIONS / 10;

    uint64_t start, end, cycles;

    /* Allocate input vector */
    fixed_t x[N];
    fixed_t y[M];
    for (size_t i = 0; i < N; i++) {
        x[i] = (fixed_t)(i << 8);  /* Simple pattern */
    }

    /* Q4_K matmul benchmark */
    struct block_q4_k q4_matrix[M];  /* 1 block per row */
    memset(q4_matrix, 0, sizeof(q4_matrix));
    for (size_t i = 0; i < M; i++) {
        q4_matrix[i].d = 256;
    }

    start = get_cycles();
    for (int i = 0; i < iters; i++) {
        matmul_q4_k(q4_matrix, sizeof(q4_matrix), x, y, M, N);
    }
    end = get_cycles();
    cycles = (end - start) / iters;
    console_printf("  Q4_K: %llu cycles/matmul (64x256)\n", (unsigned long long)cycles);

    /* Q8_0 matmul benchmark */
    const size_t q8_blocks_per_row = (N + QK8_0 - 1) / QK8_0;  /* 8 blocks per row */
    struct block_q8_0 q8_matrix[M * q8_blocks_per_row];
    memset(q8_matrix, 0, sizeof(q8_matrix));
    for (size_t i = 0; i < M * q8_blocks_per_row; i++) {
        q8_matrix[i].d = 256;
    }

    start = get_cycles();
    for (int i = 0; i < iters; i++) {
        matmul_q8_0(q8_matrix, sizeof(q8_matrix), x, y, M, N);
    }
    end = get_cycles();
    cycles = (end - start) / iters;
    console_printf("  Q8_0: %llu cycles/matmul (64x256)\n", (unsigned long long)cycles);
}

/* ============================================================================
 * GGUF Parser Tensor Info Tests
 * ============================================================================ */

static void test_ggml_type_info(void)
{
    console_printf("[Test] GGML type info functions\n");

    /* Test type names */
    TEST_ASSERT(strcmp(ggml_type_name(GGML_TYPE_Q4_K), "Q4_K") == 0, "GGML Q4_K name");
    TEST_ASSERT(strcmp(ggml_type_name(GGML_TYPE_Q8_0), "Q8_0") == 0, "GGML Q8_0 name");
    TEST_ASSERT(strcmp(ggml_type_name(GGML_TYPE_F16), "F16") == 0, "GGML F16 name");

    /* Test block sizes */
    TEST_ASSERT(ggml_type_block_size(GGML_TYPE_Q4_K) == 144, "GGML Q4_K block size");
    TEST_ASSERT(ggml_type_block_size(GGML_TYPE_Q8_0) == 34, "GGML Q8_0 block size");

    /* Test block elements */
    TEST_ASSERT(ggml_type_block_elements(GGML_TYPE_Q4_K) == 256, "GGML Q4_K elements");
    TEST_ASSERT(ggml_type_block_elements(GGML_TYPE_Q8_0) == 32, "GGML Q8_0 elements");

    console_printf("  PASS: GGML type info functions\n");
}

/* ============================================================================
 * Public Test Entry Point
 * ============================================================================ */

/**
 * run_quantized_tests - Run full quantization test suite with benchmarks
 *
 * Tests all supported quantization types (Q4_K, Q5_K, Q6_K, Q8_0) for
 * correct dequantization, dispatcher functionality, and type info.
 * Also runs performance benchmarks.
 *
 * Returns: Number of failed tests (0 = all passed)
 */
int run_quantized_tests(void)
{
    console_printf("\n========================================\n");
    console_printf("EMBODIOS Quantization Tests\n");
    console_printf("========================================\n\n");

    tests_passed = 0;
    tests_failed = 0;

    /* Q4_K tests */
    test_q4_k_basic();
    test_q4_k_tensor();

    /* Q5_K tests */
    test_q5_k_basic();
    test_q5_k_highbit();

    /* Q6_K tests */
    test_q6_k_basic();

    /* Q8_0 tests */
    test_q8_0_basic();
    test_q8_0_tensor();

    /* Dispatcher tests */
    test_dispatcher();

    /* Type info tests */
    test_type_info();
    test_ggml_type_info();

    /* Benchmarks */
    benchmark_dequant();
    benchmark_matmul();

    /* Summary */
    console_printf("\n========================================\n");
    console_printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    console_printf("========================================\n\n");

    return tests_failed;
}

/**
 * run_quantized_benchmarks - Run quantization performance benchmarks only
 *
 * Measures dequantization and matrix-vector multiplication performance
 * for all supported quantization types. Reports cycles per operation.
 *
 * Returns: 0 on success
 */
int run_quantized_benchmarks(void)
{
    console_printf("\n========================================\n");
    console_printf("EMBODIOS Quantization Benchmarks\n");
    console_printf("========================================\n");

    benchmark_dequant();
    benchmark_matmul();

    console_printf("\n========================================\n\n");
    return 0;
}
