#ifndef EMBODIOS_TEST_H
#define EMBODIOS_TEST_H

#include <embodios/types.h>
#include <embodios/console.h>

/* Test framework for in-kernel unit testing */

/* Test result codes */
#define TEST_PASS 0
#define TEST_FAIL 1

/* Test function type */
typedef int (*test_func_t)(void);

/* Test hook function type - setup/teardown */
typedef void (*test_hook_t)(void);

/* Test registration structure */
struct test_case {
    const char* name;
    const char* file;
    int line;
    test_func_t func;
    struct test_case* next;
};

/* Test statistics */
struct test_stats {
    int total;
    int passed;
    int failed;
};

/* Test framework API */
void test_register(struct test_case* test);
int test_run_all(void);
int test_run_single(const char* name);
void test_get_stats(struct test_stats* stats);

/* Setup/teardown hook API */
void test_set_setup_hook(test_hook_t setup);
void test_set_teardown_hook(test_hook_t teardown);

/* Test definition macro */
#define TEST(name) \
    static int test_##name(void); \
    static struct test_case test_case_##name = { \
        .name = #name, \
        .file = __FILE__, \
        .line = __LINE__, \
        .func = test_##name, \
        .next = NULL \
    }; \
    static void __attribute__((constructor)) test_register_##name(void) { \
        test_register(&test_case_##name); \
    } \
    static int test_##name(void)

/* Setup hook definition macro */
#define TEST_SETUP() \
    static void test_setup_hook(void); \
    static void __attribute__((constructor)) test_register_setup(void) { \
        test_set_setup_hook(test_setup_hook); \
    } \
    static void test_setup_hook(void)

/* Teardown hook definition macro */
#define TEST_TEARDOWN() \
    static void test_teardown_hook(void); \
    static void __attribute__((constructor)) test_register_teardown(void) { \
        test_set_teardown_hook(test_teardown_hook); \
    } \
    static void test_teardown_hook(void)

/* Assertion macros */
#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            console_printf("[FAIL] %s:%d: ASSERT_TRUE(%s) failed\n", \
                __FILE__, __LINE__, #expr); \
            return TEST_FAIL; \
        } \
    } while (0)

#define ASSERT_FALSE(expr) \
    do { \
        if (expr) { \
            console_printf("[FAIL] %s:%d: ASSERT_FALSE(%s) failed\n", \
                __FILE__, __LINE__, #expr); \
            return TEST_FAIL; \
        } \
    } while (0)

#define ASSERT_EQ(a, b) \
    do { \
        __typeof__(a) _a = (a); \
        __typeof__(b) _b = (b); \
        if (_a != _b) { \
            console_printf("[FAIL] %s:%d: ASSERT_EQ(%s, %s) failed: ", \
                __FILE__, __LINE__, #a, #b); \
            console_printf("expected %lld, got %lld\n", \
                (long long)_b, (long long)_a); \
            return TEST_FAIL; \
        } \
    } while (0)

#define ASSERT_NE(a, b) \
    do { \
        __typeof__(a) _a = (a); \
        __typeof__(b) _b = (b); \
        if (_a == _b) { \
            console_printf("[FAIL] %s:%d: ASSERT_NE(%s, %s) failed: ", \
                __FILE__, __LINE__, #a, #b); \
            console_printf("both values are %lld\n", (long long)_a); \
            return TEST_FAIL; \
        } \
    } while (0)

#define ASSERT_LT(a, b) \
    do { \
        __typeof__(a) _a = (a); \
        __typeof__(b) _b = (b); \
        if (_a >= _b) { \
            console_printf("[FAIL] %s:%d: ASSERT_LT(%s, %s) failed: ", \
                __FILE__, __LINE__, #a, #b); \
            console_printf("%lld >= %lld\n", \
                (long long)_a, (long long)_b); \
            return TEST_FAIL; \
        } \
    } while (0)

#define ASSERT_LE(a, b) \
    do { \
        __typeof__(a) _a = (a); \
        __typeof__(b) _b = (b); \
        if (_a > _b) { \
            console_printf("[FAIL] %s:%d: ASSERT_LE(%s, %s) failed: ", \
                __FILE__, __LINE__, #a, #b); \
            console_printf("%lld > %lld\n", \
                (long long)_a, (long long)_b); \
            return TEST_FAIL; \
        } \
    } while (0)

#define ASSERT_GT(a, b) \
    do { \
        __typeof__(a) _a = (a); \
        __typeof__(b) _b = (b); \
        if (_a <= _b) { \
            console_printf("[FAIL] %s:%d: ASSERT_GT(%s, %s) failed: ", \
                __FILE__, __LINE__, #a, #b); \
            console_printf("%lld <= %lld\n", \
                (long long)_a, (long long)_b); \
            return TEST_FAIL; \
        } \
    } while (0)

#define ASSERT_GE(a, b) \
    do { \
        __typeof__(a) _a = (a); \
        __typeof__(b) _b = (b); \
        if (_a < _b) { \
            console_printf("[FAIL] %s:%d: ASSERT_GE(%s, %s) failed: ", \
                __FILE__, __LINE__, #a, #b); \
            console_printf("%lld < %lld\n", \
                (long long)_a, (long long)_b); \
            return TEST_FAIL; \
        } \
    } while (0)

#define ASSERT_NULL(ptr) \
    do { \
        if ((ptr) != NULL) { \
            console_printf("[FAIL] %s:%d: ASSERT_NULL(%s) failed: ", \
                __FILE__, __LINE__, #ptr); \
            console_printf("pointer is %p\n", (void*)(ptr)); \
            return TEST_FAIL; \
        } \
    } while (0)

#define ASSERT_NOT_NULL(ptr) \
    do { \
        if ((ptr) == NULL) { \
            console_printf("[FAIL] %s:%d: ASSERT_NOT_NULL(%s) failed\n", \
                __FILE__, __LINE__, #ptr); \
            return TEST_FAIL; \
        } \
    } while (0)

#define ASSERT_STR_EQ(a, b) \
    do { \
        const char* _a = (a); \
        const char* _b = (b); \
        extern int strcmp(const char*, const char*); \
        if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) { \
            console_printf("[FAIL] %s:%d: ASSERT_STR_EQ(%s, %s) failed: ", \
                __FILE__, __LINE__, #a, #b); \
            console_printf("expected \"%s\", got \"%s\"\n", \
                _b ? _b : "(null)", _a ? _a : "(null)"); \
            return TEST_FAIL; \
        } \
    } while (0)

#endif /* EMBODIOS_TEST_H */
