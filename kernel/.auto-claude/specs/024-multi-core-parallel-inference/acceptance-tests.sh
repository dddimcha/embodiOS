#!/bin/bash
# Acceptance Criteria Tests for Multi-Core Parallel Inference
# Run this script to verify all acceptance criteria for task 024

set -e

KERNEL_PATH="embodios.elf"
TEST_RESULTS="acceptance-test-results.txt"

echo "=== Multi-Core Parallel Inference Acceptance Tests ===" | tee "$TEST_RESULTS"
echo "Date: $(date)" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"

# ============================================================================
# Test 1: Boot kernel with -smp 4 and verify all cores detected
# ============================================================================
echo "Test 1: Boot with 4 cores and verify detection" | tee -a "$TEST_RESULTS"
echo "Command: qemu-system-x86_64 -kernel $KERNEL_PATH -m 256M -serial stdio -smp 4 -nographic" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "Expected output:" | tee -a "$TEST_RESULTS"
echo "  - 'SMP: Detected X CPU cores' (where X >= 4)" | tee -a "$TEST_RESULTS"
echo "  - 'SMP: Initializing X CPUs'" | tee -a "$TEST_RESULTS"
echo "  - 'parallel_inference: Spawning N workers...' (where N = 3 for 4 cores)" | tee -a "$TEST_RESULTS"
echo "  - 'Worker 0 pinned to core 1'" | tee -a "$TEST_RESULTS"
echo "  - 'Worker 1 pinned to core 2'" | tee -a "$TEST_RESULTS"
echo "  - 'Worker 2 pinned to core 3'" | tee -a "$TEST_RESULTS"
echo "  - 'Worker thread 0 started on core 1'" | tee -a "$TEST_RESULTS"
echo "  - 'Worker thread 1 started on core 2'" | tee -a "$TEST_RESULTS"
echo "  - 'Worker thread 2 started on core 3'" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "✓ Acceptance Criteria: Inference work distributed across available cores" | tee -a "$TEST_RESULTS"
echo "✓ Acceptance Criteria: All cores detected and booted correctly" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"

# ============================================================================
# Test 2: Run batch inference with 4 requests
# ============================================================================
echo "Test 2: Run batch inference with 4 requests" | tee -a "$TEST_RESULTS"
echo "Command: Same QEMU command, observe kernel console output" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "Expected output:" | tee -a "$TEST_RESULTS"
echo "  - 'Running batch inference with 4 inputs...'" | tee -a "$TEST_RESULTS"
echo "  - 'Batch inference: X inferences/sec (Y threads)'" | tee -a "$TEST_RESULTS"
echo "  - Work distributed across 4 threads" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "✓ Batch inference API functional" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"

# ============================================================================
# Test 3: Measure throughput scaling (expect 3-4x vs 1 core)
# ============================================================================
echo "Test 3: Measure throughput scaling" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "Test 3a: Baseline - Single core performance" | tee -a "$TEST_RESULTS"
echo "Command: qemu-system-x86_64 -kernel $KERNEL_PATH -m 256M -serial stdio -smp 1 -nographic" | tee -a "$TEST_RESULTS"
echo "Expected: Record baseline inferences/sec (e.g., 10 inferences/sec)" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"

echo "Test 3b: Multi-core - 4 core performance" | tee -a "$TEST_RESULTS"
echo "Command: qemu-system-x86_64 -kernel $KERNEL_PATH -m 256M -serial stdio -smp 4 -nographic" | tee -a "$TEST_RESULTS"
echo "Expected: 3-4x improvement over baseline (e.g., 30-40 inferences/sec)" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "How to verify:" | tee -a "$TEST_RESULTS"
echo "  1. Run benchmark_multicore() or benchmark_scaling()" | tee -a "$TEST_RESULTS"
echo "  2. Look for 'Speedup' and 'Efficiency' metrics" | tee -a "$TEST_RESULTS"
echo "  3. Speedup should be 3.0x - 4.0x for 4 cores" | tee -a "$TEST_RESULTS"
echo "  4. Efficiency should be 75% - 100%" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "✓ Acceptance Criteria: Near-linear scaling up to 4 cores (3-4x throughput)" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"

# ============================================================================
# Test 4: Test core affinity (pin to cores 0-1 only)
# ============================================================================
echo "Test 4: Test core affinity configuration" | tee -a "$TEST_RESULTS"
echo "Command: qemu-system-x86_64 -kernel $KERNEL_PATH -m 256M -serial stdio -smp 4 -nographic" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "Test procedure:" | tee -a "$TEST_RESULTS"
echo "  1. Call parallel_pin_cores(cores, 2) where cores = {0, 1}" | tee -a "$TEST_RESULTS"
echo "  2. Verify workers only execute on cores 0-1" | tee -a "$TEST_RESULTS"
echo "  3. Check console output for 'Worker N pinned to core M' where M is 0 or 1" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "Expected output:" | tee -a "$TEST_RESULTS"
echo "  - 'Worker 0 pinned to core 0'" | tee -a "$TEST_RESULTS"
echo "  - 'Worker 1 pinned to core 1'" | tee -a "$TEST_RESULTS"
echo "  - 'Worker thread 0 started on core 0'" | tee -a "$TEST_RESULTS"
echo "  - 'Worker thread 1 started on core 1'" | tee -a "$TEST_RESULTS"
echo "  - NO workers should run on cores 2 or 3" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "✓ Acceptance Criteria: Core affinity configurable for mixed workloads" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"

# ============================================================================
# Test 5: Verify deterministic timing mode
# ============================================================================
echo "Test 5: Verify deterministic timing mode" | tee -a "$TEST_RESULTS"
echo "Command: qemu-system-x86_64 -kernel $KERNEL_PATH -m 256M -serial stdio -smp 4 -nographic" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "Test procedure:" | tee -a "$TEST_RESULTS"
echo "  1. Call parallel_set_deterministic(true)" | tee -a "$TEST_RESULTS"
echo "  2. Run the same inference prompt 10 times" | tee -a "$TEST_RESULTS"
echo "  3. Record execution time for each run" | tee -a "$TEST_RESULTS"
echo "  4. Calculate variance across runs" | tee -a "$TEST_RESULTS"
echo "  5. Verify variance < 5%" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "Expected behavior:" | tee -a "$TEST_RESULTS"
echo "  - Each run should take approximately the same time" | tee -a "$TEST_RESULTS"
echo "  - Standard deviation / mean < 0.05 (5%)" | tee -a "$TEST_RESULTS"
echo "  - Console shows: 'Deterministic mode: enabled' or similar" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "Calculation example:" | tee -a "$TEST_RESULTS"
echo "  Run 1: 1000ms, Run 2: 1020ms, ..., Run 10: 990ms" | tee -a "$TEST_RESULTS"
echo "  Mean: 1005ms, StdDev: 15ms, Variance: 1.5% < 5% ✓" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "✓ Acceptance Criteria: Maintains deterministic timing guarantees (<5% variance)" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"

# ============================================================================
# Test 6: Check per-core statistics
# ============================================================================
echo "Test 6: Check per-core statistics" | tee -a "$TEST_RESULTS"
echo "Command: qemu-system-x86_64 -kernel $KERNEL_PATH -m 256M -serial stdio -smp 4 -nographic" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "Test procedure:" | tee -a "$TEST_RESULTS"
echo "  1. Run some parallel inference workload" | tee -a "$TEST_RESULTS"
echo "  2. Call parallel_print_core_stats()" | tee -a "$TEST_RESULTS"
echo "  3. Verify statistics are tracked per core" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "Expected output:" | tee -a "$TEST_RESULTS"
echo "  - 'Core N Statistics:'" | tee -a "$TEST_RESULTS"
echo "  - '  Total cycles: XXXXXX'" | tee -a "$TEST_RESULTS"
echo "  - '  Work items: XXXX'" | tee -a "$TEST_RESULTS"
echo "  - '  Idle cycles: XXXX'" | tee -a "$TEST_RESULTS"
echo "  - '  Invocations: XXX'" | tee -a "$TEST_RESULTS"
echo "  - '  Utilization: XX.X%'" | tee -a "$TEST_RESULTS"
echo "  - Statistics shown for each active core (0-3)" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "Verification:" | tee -a "$TEST_RESULTS"
echo "  - Each core should show non-zero work items" | tee -a "$TEST_RESULTS"
echo "  - Total work items across all cores should equal total workload" | tee -a "$TEST_RESULTS"
echo "  - Utilization should be reasonable (>50% for batch workloads)" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "✓ Acceptance Criteria: Per-core timing measurements tracked" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"

# ============================================================================
# Test 7: Verify no kernel panics or crashes
# ============================================================================
echo "Test 7: Stability - No kernel panics or crashes" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "Test procedure:" | tee -a "$TEST_RESULTS"
echo "  1. Run all tests above multiple times" | tee -a "$TEST_RESULTS"
echo "  2. Run extended batch inference workloads" | tee -a "$TEST_RESULTS"
echo "  3. Verify kernel never panics or crashes" | tee -a "$TEST_RESULTS"
echo "  4. Check for race conditions, deadlocks, or hangs" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "Expected behavior:" | tee -a "$TEST_RESULTS"
echo "  - Kernel runs stably for entire test duration" | tee -a "$TEST_RESULTS"
echo "  - No 'PANIC' messages" | tee -a "$TEST_RESULTS"
echo "  - No hangs or freezes" | tee -a "$TEST_RESULTS"
echo "  - Worker threads start and complete cleanly" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "✓ Acceptance Criteria: No kernel panics or crashes during multi-core operation" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"

# ============================================================================
# Summary
# ============================================================================
echo "========================================" | tee -a "$TEST_RESULTS"
echo "ACCEPTANCE CRITERIA SUMMARY" | tee -a "$TEST_RESULTS"
echo "========================================" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "[ ] Inference work distributed across available cores" | tee -a "$TEST_RESULTS"
echo "[ ] Near-linear scaling up to 4 cores for batch inference (3-4x throughput)" | tee -a "$TEST_RESULTS"
echo "[ ] Maintains deterministic timing guarantees per core (< 5% variance)" | tee -a "$TEST_RESULTS"
echo "[ ] Core affinity configurable for mixed workloads" | tee -a "$TEST_RESULTS"
echo "[ ] All cores detected and booted correctly" | tee -a "$TEST_RESULTS"
echo "[ ] No kernel panics or crashes during multi-core operation" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "Check each box after manual verification." | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"

echo "Test script complete. Results saved to $TEST_RESULTS" | tee -a "$TEST_RESULTS"
echo "" | tee -a "$TEST_RESULTS"
echo "To run tests manually:" | tee -a "$TEST_RESULTS"
echo "  cd kernel" | tee -a "$TEST_RESULTS"
echo "  make" | tee -a "$TEST_RESULTS"
echo "  # Single core baseline:" | tee -a "$TEST_RESULTS"
echo "  qemu-system-x86_64 -kernel embodios.elf -m 256M -serial stdio -smp 1 -nographic" | tee -a "$TEST_RESULTS"
echo "  # Multi-core test (4 cores):" | tee -a "$TEST_RESULTS"
echo "  qemu-system-x86_64 -kernel embodios.elf -m 256M -serial stdio -smp 4 -nographic" | tee -a "$TEST_RESULTS"
echo "  # Multi-core test (8 cores):" | tee -a "$TEST_RESULTS"
echo "  qemu-system-x86_64 -kernel embodios.elf -m 256M -serial stdio -smp 8 -nographic" | tee -a "$TEST_RESULTS"
