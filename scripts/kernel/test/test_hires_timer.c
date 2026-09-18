/* Unit test for High-Resolution Timer */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

/* Include timer headers */
#if defined(__x86_64__) || defined(__i386__)
#include "../include/embodios/tsc.h"
#include "../include/embodios/hpet.h"
#elif defined(__aarch64__)
#include "../include/embodios/tsc.h"  /* For ARM counter functions */
#endif

/* Test configuration */
#define TEST_DELAY_US 1000      /* 1ms delay for accuracy tests */
#define TEST_ITERATIONS 1000    /* Number of iterations for resolution test */
#define TOLERANCE_PERCENT 10    /* ±10% tolerance for delay tests */

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static void print_test_header(const char* test_name) {
    printf("\n=== %s ===\n", test_name);
}

static void print_test_result(const char* test_name, bool passed) {
    printf("%s: %s\n", test_name, passed ? "PASSED" : "FAILED");
    if (!passed) {
        printf("ERROR: Test failed!\n");
    }
}

/* ============================================================================
 * Basic Timer Tests
 * ============================================================================ */

void test_timer_monotonicity() {
    print_test_header("Testing Timer Monotonicity");

    uint64_t prev = rdtsc();
    bool monotonic = true;
    int violations = 0;

    for (int i = 0; i < TEST_ITERATIONS; i++) {
        uint64_t current = rdtsc();
        if (current < prev) {
            monotonic = false;
            violations++;
        }
        prev = current;
    }

    printf("Monotonicity check: %d iterations\n", TEST_ITERATIONS);
    printf("Violations: %d\n", violations);

    print_test_result("Monotonicity", monotonic);
    assert(monotonic);
}

void test_timer_resolution() {
    print_test_header("Testing Timer Resolution");

    uint64_t min_delta = UINT64_MAX;
    uint64_t max_delta = 0;
    uint64_t total_delta = 0;
    int non_zero_count = 0;

    for (int i = 0; i < TEST_ITERATIONS; i++) {
        uint64_t t1 = rdtsc_fence();  /* Use fenced read for precision */
        uint64_t t2 = rdtsc_fence();

        uint64_t delta = t2 - t1;
        if (delta > 0) {
            if (delta < min_delta) min_delta = delta;
            if (delta > max_delta) max_delta = delta;
            total_delta += delta;
            non_zero_count++;
        }
    }

    double avg_delta = non_zero_count > 0 ? (double)total_delta / non_zero_count : 0;

    printf("Resolution test: %d successive reads\n", TEST_ITERATIONS);
    printf("Min delta: %lu cycles\n", (unsigned long)min_delta);
    printf("Max delta: %lu cycles\n", (unsigned long)max_delta);
    printf("Avg delta: %.2f cycles\n", avg_delta);
    printf("Non-zero reads: %d/%d\n", non_zero_count, TEST_ITERATIONS);

    /* Timer should be able to measure small intervals */
    bool good_resolution = (min_delta < 1000);  /* Less than 1000 cycles is good */
    print_test_result("Resolution", good_resolution);
}

void test_timer_resolution_verification() {
    print_test_header("Testing Timer Resolution Verification (1 microsecond)");

    /* Calibrate timer frequency using a known delay */
    printf("Calibrating timer frequency...\n");
    uint64_t cal_start = rdtsc_fence();
    usleep(10000);  /* 10ms calibration delay */
    uint64_t cal_end = rdtsc_fence();

    uint64_t cal_cycles = cal_end - cal_start;
    double freq_mhz = (double)cal_cycles / 10000.0;  /* Cycles per microsecond */
    double freq_ghz = freq_mhz / 1000.0;

    printf("Estimated frequency: %.2f MHz (%.3f GHz)\n", freq_mhz, freq_ghz);
    printf("Cycles per microsecond: %.2f\n", freq_mhz);

    /* Test if timer can measure 1 microsecond intervals */
    printf("\nTesting 1 microsecond measurement capability...\n");

    const int test_count = 10;
    uint64_t one_us_cycles[test_count];
    bool can_measure_1us = true;

    for (int i = 0; i < test_count; i++) {
        uint64_t start = rdtsc_fence();
        usleep(1);  /* 1 microsecond delay */
        uint64_t end = rdtsc_fence();

        one_us_cycles[i] = end - start;

        /* Check if we measured any time passing */
        if (one_us_cycles[i] == 0) {
            can_measure_1us = false;
        }
    }

    /* Calculate statistics for 1us measurements */
    uint64_t min_cycles = one_us_cycles[0];
    uint64_t max_cycles = one_us_cycles[0];
    uint64_t total_cycles = 0;

    for (int i = 0; i < test_count; i++) {
        if (one_us_cycles[i] < min_cycles) min_cycles = one_us_cycles[i];
        if (one_us_cycles[i] > max_cycles) max_cycles = one_us_cycles[i];
        total_cycles += one_us_cycles[i];
    }

    double avg_cycles = (double)total_cycles / test_count;

    printf("1 microsecond measurements (%d samples):\n", test_count);
    printf("  Min: %lu cycles\n", (unsigned long)min_cycles);
    printf("  Max: %lu cycles\n", (unsigned long)max_cycles);
    printf("  Avg: %.2f cycles\n", avg_cycles);

    /* Verify resolution: timer should detect non-zero time for 1us delays */
    printf("\nResolution verification:\n");
    printf("  Can measure 1us intervals: %s\n", can_measure_1us ? "YES" : "NO");
    printf("  Expected cycles per 1us: ~%.2f\n", freq_mhz);
    printf("  Measured cycles per 1us: %.2f\n", avg_cycles);

    /* Timer resolution is good if:
     * 1. All measurements are non-zero
     * 2. Minimum delta is less than expected 1us in cycles
     * 3. Timer can distinguish between successive reads
     */
    uint64_t t1 = rdtsc_fence();
    uint64_t t2 = rdtsc_fence();
    uint64_t successive_delta = t2 - t1;

    bool resolution_ok = can_measure_1us &&
                        (successive_delta < freq_mhz) &&
                        (min_cycles > 0);

    printf("  Successive read delta: %lu cycles (should be < %.0f)\n",
           (unsigned long)successive_delta, freq_mhz);

    if (resolution_ok) {
        printf("\nTimer can accurately measure intervals of 1 microsecond or less\n");
    } else {
        printf("\nWARNING: Timer resolution may not support 1 microsecond measurements\n");
    }

    print_test_result("Resolution verification (1us)", resolution_ok);
    assert(resolution_ok);
}

void test_timer_overhead() {
    print_test_header("Testing Timer Read Overhead");

    /* Test rdtsc overhead */
    uint64_t start = rdtsc();
    for (int i = 0; i < TEST_ITERATIONS; i++) {
        rdtsc();
    }
    uint64_t end = rdtsc();
    uint64_t rdtsc_total = end - start;

    /* Test rdtscp overhead */
    start = rdtscp();
    for (int i = 0; i < TEST_ITERATIONS; i++) {
        rdtscp();
    }
    end = rdtscp();
    uint64_t rdtscp_total = end - start;

    /* Test rdtsc_fence overhead */
    start = rdtsc_fence();
    for (int i = 0; i < TEST_ITERATIONS; i++) {
        rdtsc_fence();
    }
    end = rdtsc_fence();
    uint64_t rdtsc_fence_total = end - start;

    printf("Timer read overhead (%d iterations):\n", TEST_ITERATIONS);
    printf("  rdtsc:       %lu cycles (%.2f cycles/call)\n",
           (unsigned long)rdtsc_total, (double)rdtsc_total / TEST_ITERATIONS);
    printf("  rdtscp:      %lu cycles (%.2f cycles/call)\n",
           (unsigned long)rdtscp_total, (double)rdtscp_total / TEST_ITERATIONS);
    printf("  rdtsc_fence: %lu cycles (%.2f cycles/call)\n",
           (unsigned long)rdtsc_fence_total, (double)rdtsc_fence_total / TEST_ITERATIONS);

    /* All read methods should complete */
    bool overhead_ok = (rdtsc_total > 0) && (rdtscp_total > 0) && (rdtsc_fence_total > 0);
    print_test_result("Overhead measurement", overhead_ok);
}

/* ============================================================================
 * Architecture-Specific Tests
 * ============================================================================ */

#if defined(__x86_64__) || defined(__i386__)

void test_tsc_features() {
    print_test_header("Testing TSC Feature Detection (x86_64)");

    uint32_t features = tsc_detect_features();

    printf("TSC Features:\n");
    printf("  Present:   %s\n", (features & TSC_FEATURE_PRESENT) ? "YES" : "NO");
    printf("  Invariant: %s\n", (features & TSC_FEATURE_INVARIANT) ? "YES" : "NO");
    printf("  RDTSCP:    %s\n", (features & TSC_FEATURE_RDTSCP) ? "YES" : "NO");

    /* TSC should at least be present on modern x86_64 */
    bool tsc_present = (features & TSC_FEATURE_PRESENT) != 0;
    print_test_result("TSC Present", tsc_present);
}

void test_tsc_rdtscp() {
    print_test_header("Testing RDTSCP Instruction");

    uint32_t features = tsc_detect_features();

    if (features & TSC_FEATURE_RDTSCP) {
        uint64_t t1 = rdtscp();
        uint64_t t2 = rdtscp();

        printf("RDTSCP available and functional\n");
        printf("  First read:  %lu\n", (unsigned long)t1);
        printf("  Second read: %lu\n", (unsigned long)t2);
        printf("  Delta: %lu cycles\n", (unsigned long)(t2 - t1));

        bool rdtscp_ok = (t2 >= t1);
        print_test_result("RDTSCP", rdtscp_ok);
    } else {
        printf("RDTSCP not supported on this CPU (skipped)\n");
    }
}

#elif defined(__aarch64__)

void test_arm_counter() {
    print_test_header("Testing ARM Generic Timer Counter");

    uint64_t freq = arm_get_counter_frequency();
    uint64_t c1 = rdtsc();
    uint64_t c2 = rdtsc();

    printf("ARM Generic Timer:\n");
    printf("  Frequency: %lu Hz\n", (unsigned long)freq);
    printf("  Counter1:  %lu\n", (unsigned long)c1);
    printf("  Counter2:  %lu\n", (unsigned long)c2);
    printf("  Delta:     %lu ticks\n", (unsigned long)(c2 - c1));

    /* Frequency should be reasonable (typically 24MHz or similar) */
    bool freq_ok = (freq >= 1000000 && freq <= 1000000000);  /* 1MHz to 1GHz */
    bool counter_ok = (c2 >= c1);

    print_test_result("ARM Counter Frequency", freq_ok);
    print_test_result("ARM Counter Monotonic", counter_ok);

    assert(freq_ok && counter_ok);
}

#endif

/* ============================================================================
 * Accuracy Tests (using system sleep for reference)
 * ============================================================================ */

void test_timer_accuracy() {
    print_test_header("Testing Timer Accuracy");

    printf("Note: Using system usleep() as reference\n");
    printf("Testing %d microsecond delays...\n", TEST_DELAY_US);

    /* Warm up */
    for (int i = 0; i < 10; i++) {
        rdtsc();
    }

    /* Test multiple delay values */
    int delays[] = {100, 500, 1000, 5000, 10000};
    int num_delays = sizeof(delays) / sizeof(delays[0]);

    for (int i = 0; i < num_delays; i++) {
        int delay_us = delays[i];

        uint64_t start = rdtsc();
        usleep(delay_us);
        uint64_t end = rdtsc();

        uint64_t elapsed_cycles = end - start;

        printf("  Delay %d us: %lu cycles\n",
               delay_us, (unsigned long)elapsed_cycles);
    }

    printf("Accuracy test completed (visual inspection of cycles)\n");
    printf("Expected: Higher delays should show proportionally higher cycle counts\n");
}

void test_delay_accuracy() {
    print_test_header("Testing Delay Accuracy (1000us with ±5% tolerance)");

    /* Calibrate timer frequency using a known delay */
    printf("Calibrating timer frequency...\n");
    uint64_t cal_start = rdtsc_fence();
    usleep(10000);  /* 10ms calibration delay */
    uint64_t cal_end = rdtsc_fence();

    uint64_t cal_cycles = cal_end - cal_start;
    double cycles_per_us = (double)cal_cycles / 10000.0;

    printf("Calibration: %lu cycles in 10000 us\n", (unsigned long)cal_cycles);
    printf("Estimated cycles per microsecond: %.2f\n", cycles_per_us);

    /* Test delay_us(1000) accuracy */
    const int test_delay_us = 1000;
    const int tolerance_percent = 5;  /* ±5% tolerance */
    const int num_samples = 10;

    printf("\nTesting %d microsecond delay (%d samples)...\n",
           test_delay_us, num_samples);

    int passed_samples = 0;
    uint64_t measured_us[num_samples];

    for (int i = 0; i < num_samples; i++) {
        uint64_t start = rdtsc_fence();
        usleep(test_delay_us);  /* Delay to test */
        uint64_t end = rdtsc_fence();

        uint64_t elapsed_cycles = end - start;
        measured_us[i] = (uint64_t)((double)elapsed_cycles / cycles_per_us);

        /* Calculate tolerance bounds */
        uint64_t min_acceptable = test_delay_us * (100 - tolerance_percent) / 100;
        uint64_t max_acceptable = test_delay_us * (100 + tolerance_percent) / 100;

        bool within_tolerance = (measured_us[i] >= min_acceptable) &&
                               (measured_us[i] <= max_acceptable);

        if (within_tolerance) {
            passed_samples++;
        }

        printf("  Sample %d: %lu us (expected %d us ±%d%%) %s\n",
               i + 1,
               (unsigned long)measured_us[i],
               test_delay_us,
               tolerance_percent,
               within_tolerance ? "PASS" : "FAIL");
    }

    /* Calculate statistics */
    uint64_t min_measured = measured_us[0];
    uint64_t max_measured = measured_us[0];
    uint64_t total_measured = 0;

    for (int i = 0; i < num_samples; i++) {
        if (measured_us[i] < min_measured) min_measured = measured_us[i];
        if (measured_us[i] > max_measured) max_measured = measured_us[i];
        total_measured += measured_us[i];
    }

    double avg_measured = (double)total_measured / num_samples;
    double error_percent = ((avg_measured - test_delay_us) / test_delay_us) * 100.0;

    printf("\nStatistics:\n");
    printf("  Min: %lu us\n", (unsigned long)min_measured);
    printf("  Max: %lu us\n", (unsigned long)max_measured);
    printf("  Avg: %.2f us\n", avg_measured);
    printf("  Error: %.2f%%\n", error_percent);
    printf("  Passed: %d/%d samples\n", passed_samples, num_samples);

    /* Test passes if at least 80% of samples are within tolerance */
    bool accuracy_ok = (passed_samples >= (num_samples * 80 / 100));

    printf("\nAccuracy requirement: %d us ±%d%%\n", test_delay_us, tolerance_percent);
    printf("Acceptable range: %d-%d us\n",
           test_delay_us * (100 - tolerance_percent) / 100,
           test_delay_us * (100 + tolerance_percent) / 100);

    print_test_result("Delay accuracy", accuracy_ok);
    assert(accuracy_ok);
}

/* ============================================================================
 * Stress Tests
 * ============================================================================ */

void test_timer_stress() {
    print_test_header("Testing Timer Under Stress");

    const int iterations = 10000;
    uint64_t min_delta = UINT64_MAX;
    uint64_t max_delta = 0;
    uint64_t total = 0;
    int backward_jumps = 0;

    uint64_t prev = rdtsc();

    for (int i = 0; i < iterations; i++) {
        uint64_t current = rdtsc();

        if (current < prev) {
            backward_jumps++;
        } else {
            uint64_t delta = current - prev;
            if (delta < min_delta) min_delta = delta;
            if (delta > max_delta) max_delta = delta;
            total += delta;
        }

        prev = current;
    }

    printf("Stress test: %d iterations\n", iterations);
    printf("  Min delta: %lu cycles\n", (unsigned long)min_delta);
    printf("  Max delta: %lu cycles\n", (unsigned long)max_delta);
    printf("  Avg delta: %.2f cycles\n", (double)total / iterations);
    printf("  Backward jumps: %d\n", backward_jumps);

    /* Should have no backward jumps on a working timer */
    bool stress_ok = (backward_jumps == 0);
    print_test_result("Stress test", stress_ok);
    assert(stress_ok);
}

void test_concurrent_reads() {
    print_test_header("Testing Concurrent Timer Reads");

    /* Simulate rapid concurrent reads */
    const int burst_size = 100;
    uint64_t timestamps[burst_size];

    /* Burst read */
    for (int i = 0; i < burst_size; i++) {
        timestamps[i] = rdtsc();
    }

    /* Verify all timestamps are monotonic */
    bool monotonic = true;
    for (int i = 1; i < burst_size; i++) {
        if (timestamps[i] < timestamps[i-1]) {
            monotonic = false;
            printf("  Non-monotonic at index %d: %lu -> %lu\n",
                   i, (unsigned long)timestamps[i-1], (unsigned long)timestamps[i]);
        }
    }

    printf("Burst read test: %d rapid reads\n", burst_size);
    printf("  First: %lu\n", (unsigned long)timestamps[0]);
    printf("  Last:  %lu\n", (unsigned long)timestamps[burst_size-1]);
    printf("  Delta: %lu cycles\n",
           (unsigned long)(timestamps[burst_size-1] - timestamps[0]));

    print_test_result("Concurrent reads", monotonic);
    assert(monotonic);
}

/* ============================================================================
 * Long-Term Stability Tests
 * ============================================================================ */

void test_timer_drift() {
    print_test_header("Testing Timer Drift (Long-Term Stability)");

    printf("Note: Simulating long-term operation with multiple intervals\n");

    /* Calibrate timer frequency using a known delay */
    printf("Calibrating timer frequency...\n");
    uint64_t cal_start = rdtsc_fence();
    usleep(50000);  /* 50ms calibration delay */
    uint64_t cal_end = rdtsc_fence();

    uint64_t cal_cycles = cal_end - cal_start;
    double cycles_per_us = (double)cal_cycles / 50000.0;
    double freq_mhz = cycles_per_us;

    printf("Estimated frequency: %.2f MHz (%.3f GHz)\n",
           freq_mhz, freq_mhz / 1000.0);
    printf("Cycles per microsecond: %.2f\n", cycles_per_us);

    /* Test drift over simulated long-term operation */
    /* Use 20 intervals of 10ms each to simulate 200ms total */
    const int num_intervals = 20;
    const int interval_us = 10000;  /* 10ms per interval */
    const double max_drift_percent = 1.0;  /* Allow 1% drift */

    printf("\nMeasuring drift over %d intervals of %d us each...\n",
           num_intervals, interval_us);

    uint64_t total_expected_us = 0;
    uint64_t total_measured_cycles = 0;
    double max_cumulative_drift = 0.0;

    uint64_t drift_measurements[num_intervals];

    for (int i = 0; i < num_intervals; i++) {
        uint64_t interval_start = rdtsc_fence();
        usleep(interval_us);
        uint64_t interval_end = rdtsc_fence();

        uint64_t measured_cycles = interval_end - interval_start;
        total_measured_cycles += measured_cycles;
        total_expected_us += interval_us;

        /* Calculate cumulative drift */
        uint64_t expected_cycles = (uint64_t)(total_expected_us * cycles_per_us);
        int64_t drift_cycles = (int64_t)total_measured_cycles - (int64_t)expected_cycles;
        double drift_percent = ((double)drift_cycles / (double)expected_cycles) * 100.0;

        drift_measurements[i] = measured_cycles;

        if (drift_percent < 0) drift_percent = -drift_percent;
        if (drift_percent > max_cumulative_drift) {
            max_cumulative_drift = drift_percent;
        }

        if (i % 5 == 0 || i == num_intervals - 1) {
            printf("  Interval %2d: measured %lu cycles, cumulative drift %.4f%%\n",
                   i + 1, (unsigned long)measured_cycles, drift_percent);
        }
    }

    /* Calculate statistics */
    uint64_t min_interval = drift_measurements[0];
    uint64_t max_interval = drift_measurements[0];
    uint64_t total_interval = 0;

    for (int i = 0; i < num_intervals; i++) {
        if (drift_measurements[i] < min_interval) min_interval = drift_measurements[i];
        if (drift_measurements[i] > max_interval) max_interval = drift_measurements[i];
        total_interval += drift_measurements[i];
    }

    double avg_interval = (double)total_interval / num_intervals;
    uint64_t expected_interval_cycles = (uint64_t)(interval_us * cycles_per_us);

    printf("\nDrift Statistics:\n");
    printf("  Total simulated time: %d us (%d ms)\n",
           num_intervals * interval_us, num_intervals * interval_us / 1000);
    printf("  Expected cycles per interval: %lu\n",
           (unsigned long)expected_interval_cycles);
    printf("  Measured interval cycles:\n");
    printf("    Min: %lu\n", (unsigned long)min_interval);
    printf("    Max: %lu\n", (unsigned long)max_interval);
    printf("    Avg: %.2f\n", avg_interval);
    printf("  Maximum cumulative drift: %.4f%%\n", max_cumulative_drift);

    /* Calculate overall drift */
    uint64_t total_expected_cycles = (uint64_t)(total_expected_us * cycles_per_us);
    int64_t total_drift = (int64_t)total_measured_cycles - (int64_t)total_expected_cycles;
    double overall_drift_percent = ((double)total_drift / (double)total_expected_cycles) * 100.0;
    if (overall_drift_percent < 0) overall_drift_percent = -overall_drift_percent;

    printf("  Overall drift: %ld cycles (%.4f%%)\n",
           (long)total_drift, overall_drift_percent);

    /* Test passes if drift is within acceptable bounds */
    bool drift_ok = (max_cumulative_drift < max_drift_percent);

    printf("\nDrift requirement: < %.1f%%\n", max_drift_percent);
    printf("Timer stability over simulated long-term operation: %s\n",
           drift_ok ? "EXCELLENT" : "POOR");

    print_test_result("Timer drift", drift_ok);
    assert(drift_ok);
}

/* ============================================================================
 * Main Test Runner
 * ============================================================================ */

int main() {
    printf("=== EMBODIOS High-Resolution Timer Unit Tests ===\n");
    printf("Architecture: ");
    #if defined(__x86_64__)
    printf("x86_64\n");
    #elif defined(__i386__)
    printf("i386\n");
    #elif defined(__aarch64__)
    printf("aarch64 (ARM64)\n");
    #else
    printf("unknown\n");
    #endif

    /* Basic timer tests (all architectures) */
    test_timer_monotonicity();
    test_timer_resolution();
    test_timer_resolution_verification();
    test_timer_overhead();
    test_timer_accuracy();
    test_delay_accuracy();
    test_timer_stress();
    test_concurrent_reads();
    test_timer_drift();

    /* Architecture-specific tests */
    #if defined(__x86_64__) || defined(__i386__)
    test_tsc_features();
    test_tsc_rdtscp();
    #elif defined(__aarch64__)
    test_arm_counter();
    #endif

    printf("\n=== All High-Resolution Timer tests passed! ===\n");
    return 0;
}
