/* Minimal test without macros */
#include <embodios/test.h>
#include <embodios/types.h>

static int test_simple_test(void)
{
    return TEST_PASS;
}

static struct test_case test_case_simple_test = {
    .name = "simple_test",
    .file = __FILE__,
    .line = __LINE__,
    .func = test_simple_test,
    .next = NULL
};

static void __attribute__((constructor)) test_register_simple_test(void) {
    test_register(&test_case_simple_test);
}
