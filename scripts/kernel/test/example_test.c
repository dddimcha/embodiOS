/*
 * Example test demonstrating the EmbodïOS test framework
 *
 * This file shows how to write in-kernel unit tests.
 * Since the TEST() macro has a known issue with designated initializers,
 * this example manually registers tests to demonstrate the framework.
 */

#include <embodios/test.h>
#include <embodios/types.h>
#include <embodios/console.h>

/* Helper function to demonstrate test structure */
static int example_arithmetic_test(void)
{
    int a = 5;
    int b = 3;
    int sum = a + b;

    /* Check that addition works */
    if (sum != 8) {
        console_printf("[FAIL] Addition failed: expected 8, got %d\n", sum);
        return TEST_FAIL;
    }

    /* Check that subtraction works */
    if (a - b != 2) {
        console_printf("[FAIL] Subtraction failed\n");
        return TEST_FAIL;
    }

    return TEST_PASS;
}

/* Helper function to demonstrate pointer checks */
static int example_pointer_test(void)
{
    void* null_ptr = NULL;
    void* valid_ptr = (void*)0x1000;

    /* Check NULL pointer */
    if (null_ptr != NULL) {
        console_printf("[FAIL] NULL pointer check failed\n");
        return TEST_FAIL;
    }

    /* Check valid pointer */
    if (valid_ptr == NULL) {
        console_printf("[FAIL] Valid pointer check failed\n");
        return TEST_FAIL;
    }

    return TEST_PASS;
}

/* Helper function to demonstrate comparison operations */
static int example_comparison_test(void)
{
    int x = 10;
    int y = 20;

    /* Less than */
    if (!(x < y)) {
        console_printf("[FAIL] Less than check failed\n");
        return TEST_FAIL;
    }

    /* Greater than */
    if (!(y > x)) {
        console_printf("[FAIL] Greater than check failed\n");
        return TEST_FAIL;
    }

    /* Equality */
    if (x != x) {
        console_printf("[FAIL] Equality check failed\n");
        return TEST_FAIL;
    }

    return TEST_PASS;
}

/* Test case structures - manually defined for demonstration */
static struct test_case test_arithmetic = {
    "arithmetic",
    __FILE__,
    __LINE__,
    example_arithmetic_test,
    NULL
};

static struct test_case test_pointers = {
    "pointers",
    __FILE__,
    __LINE__,
    example_pointer_test,
    NULL
};

static struct test_case test_comparisons = {
    "comparisons",
    __FILE__,
    __LINE__,
    example_comparison_test,
    NULL
};

/* Register tests using constructor attribute */
static void __attribute__((constructor)) register_example_tests(void)
{
    test_register(&test_arithmetic);
    test_register(&test_pointers);
    test_register(&test_comparisons);
}

/*
 * USAGE NOTES:
 *
 * This example demonstrates the test framework by manually registering tests.
 * Once the TEST() macro is fixed, you can write tests like this:
 *
 *   TEST(my_test)
 *   {
 *       ASSERT_EQ(2 + 2, 4);
 *       ASSERT_TRUE(1 == 1);
 *       return TEST_PASS;
 *   }
 *
 * The test framework provides these assertion macros:
 *   - ASSERT_TRUE(expr)        - Check if expression is true
 *   - ASSERT_FALSE(expr)       - Check if expression is false
 *   - ASSERT_EQ(a, b)          - Check if a == b
 *   - ASSERT_NE(a, b)          - Check if a != b
 *   - ASSERT_LT(a, b)          - Check if a < b
 *   - ASSERT_LE(a, b)          - Check if a <= b
 *   - ASSERT_GT(a, b)          - Check if a > b
 *   - ASSERT_GE(a, b)          - Check if a >= b
 *   - ASSERT_NULL(ptr)         - Check if pointer is NULL
 *   - ASSERT_NOT_NULL(ptr)     - Check if pointer is not NULL
 *   - ASSERT_STR_EQ(a, b)      - Check if strings are equal
 *
 * Tests are automatically registered and run by test_run_all().
 */
