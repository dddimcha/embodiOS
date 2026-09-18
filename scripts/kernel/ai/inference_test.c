/* EMBODIOS Inference Engine Tests
 * Comprehensive tests for transformer inference components
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/inference.h>

/* ============================================================================
 * Test Utilities
 * ============================================================================ */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (cond) { \
        tests_passed++; \
        console_printf("  PASS: %s\n", msg); \
    } else { \
        tests_failed++; \
        console_printf("  FAIL: %s\n", msg); \
    } \
} while(0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    if ((a) == (b)) { \
        tests_passed++; \
        console_printf("  PASS: %s\n", msg); \
    } else { \
        tests_failed++; \
        console_printf("  FAIL: %s (expected %d, got %d)\n", msg, (int)(b), (int)(a)); \
    } \
} while(0)

#define TEST_ASSERT_NEAR(a, b, tol, msg) do { \
    fixed_t _diff = (a) - (b); \
    if (_diff < 0) _diff = -_diff; \
    if (_diff <= (tol)) { \
        tests_passed++; \
        console_printf("  PASS: %s\n", msg); \
    } else { \
        tests_failed++; \
        console_printf("  FAIL: %s (expected %d, got %d, diff %d)\n", \
                       msg, (int)(b), (int)(a), (int)_diff); \
    } \
} while(0)

/* ============================================================================
 * Test: Fixed-Point Math
 * ============================================================================ */

static void test_fixed_point_math(void)
{
    console_printf("\n[Test] Fixed-point math\n");

    /* Test fxmul */
    fixed_t a = F2FX(2.5f);
    fixed_t b = F2FX(4.0f);
    fixed_t result = fxmul(a, b);
    fixed_t expected = F2FX(10.0f);
    TEST_ASSERT_NEAR(result, expected, F2FX(0.01f), "fxmul(2.5, 4.0) = 10.0");

    /* Test fxdiv */
    a = F2FX(10.0f);
    b = F2FX(4.0f);
    result = fxdiv(a, b);
    expected = F2FX(2.5f);
    TEST_ASSERT_NEAR(result, expected, F2FX(0.01f), "fxdiv(10.0, 4.0) = 2.5");

    /* Test division by zero */
    result = fxdiv(F2FX(1.0f), 0);
    TEST_ASSERT_EQ(result, 0, "fxdiv(x, 0) = 0 (safe)");

    /* Test negative numbers */
    a = F2FX(-3.0f);
    b = F2FX(2.0f);
    result = fxmul(a, b);
    expected = F2FX(-6.0f);
    TEST_ASSERT_NEAR(result, expected, F2FX(0.01f), "fxmul(-3.0, 2.0) = -6.0");

    /* Test overflow safety */
    a = F2FX(100.0f);
    b = F2FX(100.0f);
    result = fxmul(a, b);
    expected = F2FX(10000.0f);
    TEST_ASSERT_NEAR(result, expected, F2FX(1.0f), "fxmul(100, 100) = 10000");
}

/* ============================================================================
 * Test: RMSNorm
 * ============================================================================ */

static void test_rms_norm(void)
{
    console_printf("\n[Test] RMSNorm\n");

    /* Test with simple vector */
    fixed_t x[4] = {F2FX(1.0f), F2FX(2.0f), F2FX(3.0f), F2FX(4.0f)};
    fixed_t weight[4] = {FIXED_ONE, FIXED_ONE, FIXED_ONE, FIXED_ONE};

    int err = rms_norm_fx(x, weight, 4, F2FX(1e-5f));
    TEST_ASSERT_EQ(err, INFERENCE_OK, "RMSNorm returns OK");

    /* After normalization, check variance is approximately 1 */
    int64_t sum_sq = 0;
    for (int i = 0; i < 4; i++) {
        sum_sq += ((int64_t)x[i] * x[i]) >> FIXED_SHIFT;
    }
    fixed_t mean_sq = (fixed_t)(sum_sq / 4);
    TEST_ASSERT_NEAR(mean_sq, FIXED_ONE, F2FX(0.3f), "RMSNorm: mean(x^2) ~ 1.0");

    /* Test NULL input */
    err = rms_norm_fx(NULL, weight, 4, F2FX(1e-5f));
    TEST_ASSERT_EQ(err, INFERENCE_ERR_NULL, "RMSNorm: NULL input returns error");

    /* Test invalid size */
    fixed_t y[4] = {FIXED_ONE, FIXED_ONE, FIXED_ONE, FIXED_ONE};
    err = rms_norm_fx(y, NULL, 0, F2FX(1e-5f));
    TEST_ASSERT_EQ(err, INFERENCE_ERR_BOUNDS, "RMSNorm: size=0 returns error");

    /* Test with zero vector */
    fixed_t zero[4] = {0, 0, 0, 0};
    err = rms_norm_fx(zero, NULL, 4, F2FX(1e-5f));
    TEST_ASSERT_EQ(err, INFERENCE_OK, "RMSNorm: zero vector OK");
    TEST_ASSERT_EQ(zero[0], 0, "RMSNorm: zero stays zero");
}

/* ============================================================================
 * Test: RoPE
 * ============================================================================ */

static void test_rope(void)
{
    console_printf("\n[Test] RoPE (Rotary Position Embeddings)\n");

    /* Test that RoPE preserves vector magnitude */
    fixed_t q[64];
    fixed_t k[64];

    for (int i = 0; i < 64; i++) {
        q[i] = F2FX(0.1f);
        k[i] = F2FX(0.1f);
    }

    /* Magnitude before */
    int64_t mag_before = 0;
    for (int i = 0; i < 64; i++) {
        mag_before += ((int64_t)q[i] * q[i]) >> FIXED_SHIFT;
    }

    /* Apply RoPE */
    int err = rope_apply(q, k, 5, 64, 1, 1);
    TEST_ASSERT_EQ(err, INFERENCE_OK, "RoPE returns OK");

    /* Magnitude after */
    int64_t mag_after = 0;
    for (int i = 0; i < 64; i++) {
        mag_after += ((int64_t)q[i] * q[i]) >> FIXED_SHIFT;
    }

    int64_t diff = (mag_after > mag_before) ?
                   (mag_after - mag_before) : (mag_before - mag_after);
    TEST_ASSERT(diff < mag_before / 2, "RoPE: preserves magnitude approximately");

    /* Test NULL input */
    err = rope_apply(NULL, k, 5, 64, 1, 1);
    TEST_ASSERT_EQ(err, INFERENCE_ERR_NULL, "RoPE: NULL q returns error");

    /* Test invalid parameters */
    fixed_t q2[64], k2[64];
    for (int i = 0; i < 64; i++) q2[i] = k2[i] = FIXED_ONE;
    err = rope_apply(q2, k2, 5, 0, 1, 1);
    TEST_ASSERT_EQ(err, INFERENCE_ERR_BOUNDS, "RoPE: head_dim=0 returns error");

    /* Test different positions give different results */
    fixed_t q3[64], q4[64], k_dummy[64];
    for (int i = 0; i < 64; i++) {
        q3[i] = F2FX(0.5f);
        q4[i] = F2FX(0.5f);
        k_dummy[i] = 0;
    }

    rope_apply(q3, k_dummy, 0, 64, 1, 1);
    rope_apply(q4, k_dummy, 10, 64, 1, 1);

    bool different = false;
    for (int i = 0; i < 64; i++) {
        if (q3[i] != q4[i]) {
            different = true;
            break;
        }
    }
    TEST_ASSERT(different, "RoPE: different positions give different rotations");
}

/* ============================================================================
 * Test: Inference Initialization
 * ============================================================================ */

static void test_inference_init(void)
{
    console_printf("\n[Test] Inference initialization\n");

    /* First cleanup any previous state */
    inference_cleanup();

    /* Test valid initialization */
    int result = inference_init(
        1000,   /* n_vocab */
        256,    /* n_embd */
        2,      /* n_layer */
        8,      /* n_heads */
        4,      /* n_kv_heads */
        512,    /* n_ff */
        64      /* max_seq_len */
    );
    TEST_ASSERT_EQ(result, INFERENCE_OK, "inference_init succeeds");
    TEST_ASSERT_EQ(inference_get_position(), 0, "Initial position is 0");

    /* Test double initialization */
    result = inference_init(1000, 256, 2, 8, 4, 512, 64);
    TEST_ASSERT_EQ(result, INFERENCE_ERR_ALREADY_INIT, "Double init returns error");

    /* Reset should work */
    inference_reset();
    TEST_ASSERT_EQ(inference_get_position(), 0, "Reset position to 0");

    /* Cleanup for next tests */
    inference_cleanup();

    /* Test invalid parameters */
    result = inference_init(0, 256, 2, 8, 4, 512, 64);
    TEST_ASSERT_EQ(result, INFERENCE_ERR_BOUNDS, "n_vocab=0 returns error");

    result = inference_init(1000, 255, 2, 8, 4, 512, 64);  /* Not divisible by n_heads */
    TEST_ASSERT_EQ(result, INFERENCE_ERR_INVALID, "n_embd not divisible by n_heads");

    result = inference_init(1000, 256, 2, 0, 4, 512, 64);
    TEST_ASSERT_EQ(result, INFERENCE_ERR_BOUNDS, "n_heads=0 returns error");
}

/* ============================================================================
 * Test: Forward Pass
 * ============================================================================ */

static void test_forward_pass(void)
{
    console_printf("\n[Test] Forward pass (demo mode)\n");

    /* Initialize for this test */
    inference_cleanup();
    int init_result = inference_init(1000, 256, 2, 8, 4, 512, 64);
    if (init_result != INFERENCE_OK) {
        console_printf("  SKIP: Could not initialize inference engine\n");
        return;
    }

    /* Allocate logits buffer */
    fixed_t logits[1000];

    /* Forward pass with token 42 */
    int result = inference_forward(42, logits);
    TEST_ASSERT_EQ(result, INFERENCE_OK, "Forward pass succeeds");
    TEST_ASSERT_EQ(inference_get_position(), 1, "Position incremented to 1");

    /* Logits should have variation */
    bool has_variation = false;
    for (int i = 1; i < 256; i++) {
        if (logits[i] != logits[0]) {
            has_variation = true;
            break;
        }
    }
    TEST_ASSERT(has_variation, "Logits have variation");

    /* Test NULL logits */
    result = inference_forward(100, NULL);
    TEST_ASSERT_EQ(result, INFERENCE_ERR_NULL, "NULL logits returns error");

    /* Second forward pass */
    result = inference_forward(100, logits);
    TEST_ASSERT_EQ(result, INFERENCE_OK, "Second forward pass succeeds");
    TEST_ASSERT_EQ(inference_get_position(), 2, "Position incremented to 2");

    inference_cleanup();
}

/* ============================================================================
 * Test: Sampling
 * ============================================================================ */

static void test_sampling(void)
{
    console_printf("\n[Test] Sampling\n");

    /* Create test logits with clear maximum */
    fixed_t logits[100];
    for (int i = 0; i < 100; i++) {
        logits[i] = F2FX(-1.0f);
    }
    logits[42] = F2FX(5.0f);

    /* Greedy sampling should return argmax */
    int sampled = inference_sample(logits, 100, FIXED_ONE, FIXED_ONE);
    TEST_ASSERT_EQ(sampled, 42, "Greedy sampling returns argmax");

    /* Reset and test with different max */
    for (int i = 0; i < 100; i++) {
        logits[i] = F2FX(-1.0f);
    }
    logits[50] = F2FX(5.0f);

    /* Low temperature should also pick max */
    sampled = inference_sample(logits, 100, F2FX(0.1f), FIXED_ONE);
    TEST_ASSERT_EQ(sampled, 50, "Low temperature sampling picks max");

    /* Test edge cases */
    sampled = inference_sample(NULL, 100, FIXED_ONE, FIXED_ONE);
    TEST_ASSERT_EQ(sampled, 0, "NULL logits returns 0");

    sampled = inference_sample(logits, 0, FIXED_ONE, FIXED_ONE);
    TEST_ASSERT_EQ(sampled, 0, "vocab_size=0 returns 0");
}

/* ============================================================================
 * Test: Token Generation (10+ tokens)
 * ============================================================================ */

static void test_token_generation(void)
{
    console_printf("\n[Test] Token generation (10+ tokens)\n");

    /* Initialize */
    inference_cleanup();
    int init_result = inference_init(1000, 256, 2, 8, 4, 512, 64);
    if (init_result != INFERENCE_OK) {
        console_printf("  SKIP: Could not initialize inference engine\n");
        tests_failed++;
        return;
    }

    inference_reset();

    fixed_t logits[1000];
    int tokens_generated[20];
    int num_tokens = 0;

    /* Generate 15 tokens */
    int initial_token = 1;

    for (int i = 0; i < 15 && num_tokens < 20; i++) {
        int input_token = (i == 0) ? initial_token : tokens_generated[i - 1];

        int result = inference_forward(input_token, logits);
        if (result != INFERENCE_OK) {
            console_printf("  Forward pass failed at token %d (err=%d)\n", i, result);
            break;
        }

        int next_token = inference_sample(logits, 256, F2FX(0.8f), FIXED_ONE);
        tokens_generated[num_tokens++] = next_token;
    }

    TEST_ASSERT(num_tokens >= 10, "Generated 10+ tokens");

    console_printf("  Generated %d tokens: ", num_tokens);
    for (int i = 0; i < num_tokens && i < 8; i++) {
        console_printf("%d ", tokens_generated[i]);
    }
    if (num_tokens > 8) console_printf("...");
    console_printf("\n");

    /* Verify all tokens are valid */
    bool all_valid = true;
    for (int i = 0; i < num_tokens; i++) {
        if (tokens_generated[i] < 0 || tokens_generated[i] >= 256) {
            all_valid = false;
            break;
        }
    }
    TEST_ASSERT(all_valid, "All generated tokens are valid");

    /* Position should equal number of forward passes */
    TEST_ASSERT_EQ(inference_get_position(), num_tokens,
                   "KV cache position matches generation count");

    inference_cleanup();
}

/* ============================================================================
 * Test: Coherence Check
 * ============================================================================ */

static void test_coherence(void)
{
    console_printf("\n[Test] Coherence check\n");

    inference_cleanup();
    int init_result = inference_init(1000, 256, 2, 8, 4, 512, 64);
    if (init_result != INFERENCE_OK) {
        console_printf("  SKIP: Could not initialize inference engine\n");
        tests_failed++;
        return;
    }

    inference_reset();

    fixed_t logits[1000];
    int tokens[32];
    int num_tokens = 0;

    int prompt_token = 100;

    for (int i = 0; i < 20; i++) {
        int input = (i == 0) ? prompt_token : tokens[i - 1];
        int err = inference_forward(input, logits);
        if (err != INFERENCE_OK) break;
        tokens[num_tokens++] = inference_sample(logits, 256, F2FX(0.7f), FIXED_ONE);
    }

    /* Check for diversity */
    int unique_tokens = 0;
    bool seen[256] = {0};
    for (int i = 0; i < num_tokens; i++) {
        if (tokens[i] >= 0 && tokens[i] < 256 && !seen[tokens[i]]) {
            seen[tokens[i]] = true;
            unique_tokens++;
        }
    }

    TEST_ASSERT(unique_tokens >= 2, "Output has diversity (2+ unique tokens)");
    console_printf("  Unique tokens: %d/%d\n", unique_tokens, num_tokens);

    inference_cleanup();
}

/* ============================================================================
 * Test: Bounds Checking
 * ============================================================================ */

static void test_bounds_checking(void)
{
    console_printf("\n[Test] Bounds checking\n");

    inference_cleanup();

    /* Test max_seq_len enforcement */
    int result = inference_init(1000, 256, 2, 8, 4, 512, 4);  /* Very short seq */
    TEST_ASSERT_EQ(result, INFERENCE_OK, "Init with short max_seq_len");

    fixed_t logits[1000];

    /* Generate up to limit */
    for (int i = 0; i < 4; i++) {
        result = inference_forward(i, logits);
        if (result != INFERENCE_OK) break;
    }

    /* Next should fail due to bounds */
    result = inference_forward(99, logits);
    TEST_ASSERT_EQ(result, INFERENCE_ERR_BOUNDS, "Position overflow detected");

    inference_cleanup();
}

/* ============================================================================
 * Run All Tests
 * ============================================================================ */

void inference_run_tests(void)
{
    console_printf("\n");
    console_printf("========================================\n");
    console_printf("EMBODIOS Inference Engine Tests\n");
    console_printf("========================================\n");

    tests_passed = 0;
    tests_failed = 0;

    /* Ensure clean state */
    inference_cleanup();

    /* Run test suites */
    test_fixed_point_math();
    test_rms_norm();
    test_rope();
    test_inference_init();
    test_forward_pass();
    test_sampling();
    test_token_generation();
    test_coherence();
    test_bounds_checking();

    /* Final cleanup */
    inference_cleanup();

    /* Summary */
    console_printf("\n========================================\n");
    console_printf("Test Results: %d passed, %d failed\n",
                   tests_passed, tests_failed);
    console_printf("========================================\n\n");

    if (tests_failed == 0) {
        console_printf("SUCCESS: All tests PASSED!\n");
    } else {
        console_printf("FAILURE: Some tests failed.\n");
    }
}
