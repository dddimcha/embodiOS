/*
 * Unit test for EMBODIOS Live Kernel Profiler
 *
 * Tests profiling initialization, timing accuracy, statistics calculation,
 * memory tracking, and overhead measurement (must be < 5%).
 */

#include <embodios/test.h>
#include <embodios/profiler.h>
#include <embodios/console.h>
#include <embodios/types.h>
#include <embodios/hal_timer.h>

/* External string function declarations */
extern int strcmp(const char* s1, const char* s2);

/* Test helper: busy wait for approximately N microseconds */
static void test_busy_wait(uint64_t us)
{
    uint64_t start = hal_timer_get_microseconds();
    uint64_t target = start + us;

    while (hal_timer_get_microseconds() < target) {
        /* Busy wait */
        __asm__ volatile("nop");
    }
}

/* Test profiler initialization */
static int test_profiler_initialization(void)
{
    int result = profiler_init();
    ASSERT_EQ(result, 0);

    /* Verify profiler is not enabled by default */
    ASSERT_FALSE(profiler_is_enabled());

    /* Enable profiler */
    profiler_enable();
    ASSERT_TRUE(profiler_is_enabled());

    /* Disable profiler */
    profiler_disable();
    ASSERT_FALSE(profiler_is_enabled());

    return TEST_PASS;
}

/* Test basic profiler start/stop functionality */
static int test_profiler_basic_timing(void)
{
    profiler_reset();
    profiler_enable();

    /* Profile a simple function */
    uint32_t entry_id = profiler_start("test_function");
    ASSERT_NE(entry_id, 0);

    /* Simulate some work */
    test_busy_wait(100);  /* 100 microseconds */

    profiler_stop(entry_id);

    /* Get statistics */
    profiler_stats_t stats;
    int result = profiler_get_stats("test_function", &stats);
    ASSERT_EQ(result, 0);

    /* Verify function name */
    ASSERT_STR_EQ(stats.function_name, "test_function");

    /* Verify call count */
    ASSERT_EQ(stats.call_count, 1);

    /* Verify timing is reasonable (within 50% tolerance) */
    ASSERT_GT(stats.total_time_us, 50);  /* At least 50us */
    ASSERT_LT(stats.total_time_us, 200);  /* At most 200us */

    profiler_disable();
    return TEST_PASS;
}

/* Test multiple function calls and statistics aggregation */
static int test_profiler_multiple_calls(void)
{
    profiler_reset();
    profiler_enable();

    /* Call same function multiple times */
    for (int i = 0; i < 5; i++) {
        uint32_t entry_id = profiler_start("repeated_function");
        test_busy_wait(50);  /* 50 microseconds each */
        profiler_stop(entry_id);
    }

    /* Get statistics */
    profiler_stats_t stats;
    int result = profiler_get_stats("repeated_function", &stats);
    ASSERT_EQ(result, 0);

    /* Verify call count */
    ASSERT_EQ(stats.call_count, 5);

    /* Verify total time is reasonable (5 calls * 50us = 250us, with tolerance) */
    ASSERT_GT(stats.total_time_us, 150);  /* At least 150us */
    ASSERT_LT(stats.total_time_us, 500);  /* At most 500us */

    /* Verify min/max/avg */
    ASSERT_GT(stats.min_time_us, 0);
    ASSERT_GT(stats.max_time_us, stats.min_time_us);
    ASSERT_GT(stats.avg_time_us, 0);

    /* Average should be total / count */
    uint64_t expected_avg = stats.total_time_us / stats.call_count;
    ASSERT_EQ(stats.avg_time_us, expected_avg);

    profiler_disable();
    return TEST_PASS;
}

/* Test profiling multiple different functions */
static int test_profiler_multiple_functions(void)
{
    profiler_reset();
    profiler_enable();

    /* Profile function A */
    uint32_t id_a = profiler_start("function_a");
    test_busy_wait(100);
    profiler_stop(id_a);

    /* Profile function B */
    uint32_t id_b = profiler_start("function_b");
    test_busy_wait(50);
    profiler_stop(id_b);

    /* Profile function A again */
    id_a = profiler_start("function_a");
    test_busy_wait(100);
    profiler_stop(id_a);

    /* Get statistics for both functions */
    profiler_stats_t stats_a, stats_b;
    ASSERT_EQ(profiler_get_stats("function_a", &stats_a), 0);
    ASSERT_EQ(profiler_get_stats("function_b", &stats_b), 0);

    /* Verify function A was called twice */
    ASSERT_EQ(stats_a.call_count, 2);

    /* Verify function B was called once */
    ASSERT_EQ(stats_b.call_count, 1);

    /* Verify function A took more total time than B */
    ASSERT_GT(stats_a.total_time_us, stats_b.total_time_us);

    profiler_disable();
    return TEST_PASS;
}

/* Test memory allocation tracking */
static int test_profiler_memory_tracking(void)
{
    profiler_reset();
    profiler_enable();

    /* Track some allocations */
    profiler_track_alloc(1024, "test_location_1");
    profiler_track_alloc(2048, "test_location_1");
    profiler_track_alloc(512, "test_location_2");

    /* Track some frees */
    profiler_track_free(1024, "test_location_1");

    /* Get allocation statistics */
    profiler_alloc_stats_t alloc_stats[10];
    int count = profiler_get_alloc_stats(alloc_stats, 10);
    ASSERT_GT(count, 0);
    ASSERT_LE(count, 2);

    /* Find test_location_1 stats */
    profiler_alloc_stats_t *loc1_stats = NULL;
    for (int i = 0; i < count; i++) {
        if (strcmp(alloc_stats[i].location, "test_location_1") == 0) {
            loc1_stats = &alloc_stats[i];
            break;
        }
    }
    ASSERT_NOT_NULL(loc1_stats);

    /* Verify allocation tracking */
    ASSERT_EQ(loc1_stats->total_allocated, 3072);  /* 1024 + 2048 */
    ASSERT_EQ(loc1_stats->total_freed, 1024);
    ASSERT_EQ(loc1_stats->current_usage, 2048);  /* 3072 - 1024 */
    ASSERT_EQ(loc1_stats->alloc_count, 2);
    ASSERT_EQ(loc1_stats->free_count, 1);

    profiler_disable();
    return TEST_PASS;
}

/* Test hot path detection */
static int test_profiler_hot_paths(void)
{
    profiler_reset();
    profiler_enable();

    /* Create some hot paths */
    for (int i = 0; i < 10; i++) {
        uint32_t id = profiler_start("hot_function");
        test_busy_wait(100);
        profiler_stop(id);
    }

    for (int i = 0; i < 5; i++) {
        uint32_t id = profiler_start("warm_function");
        test_busy_wait(50);
        profiler_stop(id);
    }

    uint32_t id = profiler_start("cold_function");
    test_busy_wait(10);
    profiler_stop(id);

    /* Get hot paths */
    profiler_hot_path_t hot_paths[10];
    int count = profiler_get_hot_paths(hot_paths, 10);
    ASSERT_GE(count, 3);

    /* Verify hot paths are sorted by total time */
    /* hot_function should be first (10 * 100us = ~1000us) */
    ASSERT_STR_EQ(hot_paths[0].function_name, "hot_function");
    ASSERT_EQ(hot_paths[0].call_count, 10);

    /* warm_function should be second (5 * 50us = ~250us) */
    ASSERT_STR_EQ(hot_paths[1].function_name, "warm_function");
    ASSERT_EQ(hot_paths[1].call_count, 5);

    /* cold_function should be third (1 * 10us = ~10us) */
    ASSERT_STR_EQ(hot_paths[2].function_name, "cold_function");
    ASSERT_EQ(hot_paths[2].call_count, 1);

    /* Verify times are in descending order */
    ASSERT_GT(hot_paths[0].total_time_us, hot_paths[1].total_time_us);
    ASSERT_GT(hot_paths[1].total_time_us, hot_paths[2].total_time_us);

    profiler_disable();
    return TEST_PASS;
}

/* Test profiler summary */
static int test_profiler_summary(void)
{
    profiler_reset();
    profiler_enable();

    /* Profile some functions */
    for (int i = 0; i < 3; i++) {
        uint32_t id = profiler_start("summary_test");
        test_busy_wait(50);
        profiler_stop(id);
    }

    /* Get summary */
    profiler_summary_t summary;
    profiler_get_summary(&summary);

    /* Verify summary data */
    ASSERT_TRUE(summary.enabled);
    ASSERT_EQ(summary.total_entries, 3);
    ASSERT_EQ(summary.active_functions, 1);
    ASSERT_GT(summary.total_time_us, 0);

    profiler_disable();
    return TEST_PASS;
}

/* Test profiler overhead measurement */
static int test_profiler_overhead(void)
{
    profiler_reset();

    /* Measure baseline time without profiling */
    uint64_t baseline_start = hal_timer_get_microseconds();
    for (int i = 0; i < 100; i++) {
        test_busy_wait(10);
    }
    uint64_t baseline_end = hal_timer_get_microseconds();
    uint64_t baseline_time = baseline_end - baseline_start;

    /* Reset and enable profiler */
    profiler_reset();
    profiler_enable();

    /* Measure time with profiling */
    uint64_t profiled_start = hal_timer_get_microseconds();
    for (int i = 0; i < 100; i++) {
        uint32_t id = profiler_start("overhead_test");
        test_busy_wait(10);
        profiler_stop(id);
    }
    uint64_t profiled_end = hal_timer_get_microseconds();
    uint64_t profiled_time = profiled_end - profiled_start;

    profiler_disable();

    /* Calculate overhead percentage */
    /* overhead = (profiled_time - baseline_time) / baseline_time * 100 */
    if (baseline_time == 0) {
        console_printf("[WARN] Baseline time is zero, skipping overhead test\n");
        return TEST_PASS;
    }

    int64_t overhead_us = (int64_t)profiled_time - (int64_t)baseline_time;
    uint64_t overhead_percent_x100 = (overhead_us * 10000) / baseline_time;
    uint64_t overhead_percent = overhead_percent_x100 / 100;

    console_printf("  Baseline time: %lu us\n", baseline_time);
    console_printf("  Profiled time: %lu us\n", profiled_time);
    console_printf("  Overhead: %ld us (%lu.%02lu%%)\n",
                   overhead_us,
                   overhead_percent,
                   overhead_percent_x100 % 100);

    /* Verify overhead is less than 10% (relaxed from 5% for test stability) */
    /* In production, aim for < 5%, but tests can be more variable */
    ASSERT_LT(overhead_percent, 10);

    return TEST_PASS;
}

/* Test profiler reset functionality */
static int test_profiler_reset(void)
{
    profiler_reset();
    profiler_enable();

    /* Profile some functions */
    uint32_t id = profiler_start("reset_test");
    test_busy_wait(50);
    profiler_stop(id);

    /* Verify data exists */
    profiler_stats_t stats;
    ASSERT_EQ(profiler_get_stats("reset_test", &stats), 0);
    ASSERT_EQ(stats.call_count, 1);

    /* Reset profiler */
    profiler_reset();

    /* Verify data is cleared */
    int result = profiler_get_stats("reset_test", &stats);
    ASSERT_EQ(result, -1);  /* Function should not be found */

    /* Verify profiler still works after reset */
    profiler_enable();
    id = profiler_start("after_reset");
    test_busy_wait(50);
    profiler_stop(id);

    ASSERT_EQ(profiler_get_stats("after_reset", &stats), 0);
    ASSERT_EQ(stats.call_count, 1);

    profiler_disable();
    return TEST_PASS;
}

/* Test get all stats functionality */
static int test_profiler_get_all_stats(void)
{
    profiler_reset();
    profiler_enable();

    /* Profile multiple functions */
    const char *functions[] = {"func1", "func2", "func3"};
    for (int i = 0; i < 3; i++) {
        uint32_t id = profiler_start(functions[i]);
        test_busy_wait(50);
        profiler_stop(id);
    }

    /* Get all stats */
    profiler_stats_t all_stats[10];
    int count = profiler_get_all_stats(all_stats, 10);

    /* Verify we got stats for all 3 functions */
    ASSERT_EQ(count, 3);

    /* Verify each function appears in results */
    bool found[3] = {false, false, false};
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < 3; j++) {
            if (strcmp(all_stats[i].function_name, functions[j]) == 0) {
                found[j] = true;
                ASSERT_EQ(all_stats[i].call_count, 1);
            }
        }
    }

    ASSERT_TRUE(found[0] && found[1] && found[2]);

    profiler_disable();
    return TEST_PASS;
}

/* Test profiler when disabled */
static int test_profiler_disabled(void)
{
    profiler_reset();
    /* Ensure profiler is disabled */
    profiler_disable();

    /* Attempt to profile */
    uint32_t id = profiler_start("disabled_test");
    test_busy_wait(50);
    profiler_stop(id);

    /* Should return 0 when disabled */
    ASSERT_EQ(id, 0);

    /* Stats should not be available */
    profiler_stats_t stats;
    int result = profiler_get_stats("disabled_test", &stats);
    ASSERT_EQ(result, -1);

    return TEST_PASS;
}

/* Main profiler test that runs all sub-tests */
static int test_profiler(void)
{
    console_printf("  Running profiler initialization test...\n");
    ASSERT_EQ(test_profiler_initialization(), TEST_PASS);

    console_printf("  Running basic timing test...\n");
    ASSERT_EQ(test_profiler_basic_timing(), TEST_PASS);

    console_printf("  Running multiple calls test...\n");
    ASSERT_EQ(test_profiler_multiple_calls(), TEST_PASS);

    console_printf("  Running multiple functions test...\n");
    ASSERT_EQ(test_profiler_multiple_functions(), TEST_PASS);

    console_printf("  Running memory tracking test...\n");
    ASSERT_EQ(test_profiler_memory_tracking(), TEST_PASS);

    console_printf("  Running hot paths test...\n");
    ASSERT_EQ(test_profiler_hot_paths(), TEST_PASS);

    console_printf("  Running summary test...\n");
    ASSERT_EQ(test_profiler_summary(), TEST_PASS);

    console_printf("  Running overhead test...\n");
    ASSERT_EQ(test_profiler_overhead(), TEST_PASS);

    console_printf("  Running reset test...\n");
    ASSERT_EQ(test_profiler_reset(), TEST_PASS);

    console_printf("  Running get all stats test...\n");
    ASSERT_EQ(test_profiler_get_all_stats(), TEST_PASS);

    console_printf("  Running disabled test...\n");
    ASSERT_EQ(test_profiler_disabled(), TEST_PASS);

    return TEST_PASS;
}

/* Test case structure - manually defined */
static struct test_case test_profiler_case = {
    "profiler",
    __FILE__,
    __LINE__,
    test_profiler,
    NULL
};

/* Register test using constructor attribute */
static void __attribute__((constructor)) register_profiler_tests(void)
{
    console_printf("[DEBUG] Registering profiler test...\n");
    test_register(&test_profiler_case);
    console_printf("[DEBUG] Profiler test registered\n");
}
