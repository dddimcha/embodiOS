# TVM Runtime End-to-End Integration Test Guide

## Overview

This document describes how to test the complete TVM runtime integration, including model loading, inference execution, and performance benchmarking.

## Prerequisites

- QEMU installed (qemu-system-x86_64)
- Built EMBODIOS kernel (embodios.elf)

## Build Verification

1. **Build the kernel:**
   ```bash
   cd kernel
   make
   ```

2. **Verify TVM functions are present:**
   ```bash
   nm embodios.elf | grep tvm
   ```

   Expected output should include:
   - `tvm_create_test_module`
   - `tvm_module_load`
   - `tvm_module_run`
   - `tvm_get_loaded_module`
   - `tvm_run_benchmark`

## Manual Testing Steps

### Step 1: Boot Kernel in QEMU

```bash
qemu-system-x86_64 -kernel embodios.elf -m 256M -serial stdio -nographic
```

**Expected Output:**
```
EMBODIOS Native Kernel v0.1.0-native
Build: Jan 23 2026 01:20:00
...
EMBODIOS Ready (polling mode - no interrupts).
Type 'help' for available commands.

>
```

### Step 2: Check Available Commands

Type at the prompt:
```
help advanced
```

**Expected Output:**
```
Advanced Commands:
==================
...
Testing:
  locktest, quanttest, quantbench, benchgguf, validate

TVM Runtime:
  tvmload, tvmrun, tvmbench
```

### Step 3: Load TVM Module

Type at the prompt:
```
tvmload
```

**Expected Output:**
```
Creating test TVM module...
Loading TVM module (XXXX bytes)...
TVM Runtime: Loaded module 'test_module' (XXXX bytes)
SUCCESS: TVM module loaded
  Name: test_module (or similar)
  Functions: X
  Module size: XXXX bytes
Use 'tvmrun' to execute inference
```

**Success Criteria:**
- ✓ No error messages
- ✓ "SUCCESS: TVM module loaded" appears
- ✓ Module size is displayed
- ✓ Functions count is > 0

### Step 4: Run Inference

Type at the prompt:
```
tvmrun
```

**Expected Output:**
```
Running inference with TVM module...
Executing graph...
SUCCESS: Inference completed
Output (first 10 values): 0.XXX 0.XXX 0.XXX ...
```

**Success Criteria:**
- ✓ No error messages
- ✓ "SUCCESS: Inference completed" appears
- ✓ Output values are displayed (float numbers)
- ✓ Kernel does not panic or crash

### Step 5: Run Performance Benchmark

Type at the prompt:
```
tvmbench
```

**Expected Output:**
```
=== TVM Runtime Performance Benchmark ===
Warmup iterations: 10
Measurement iterations: 100

[TVM] Benchmarking Dense Layer (1x512 x 512x512)...
[TVM] Dense Layer: XXXXX cycles/iter, X.XX GFLOPS
[TVM] Result: PASS

[TVM] Benchmarking ReLU (262144 elements)...
[TVM] ReLU: XXXXX cycles/iter
[TVM] Result: PASS

[TVM] Benchmarking Softmax (1000 elements)...
[TVM] Softmax: XXXXX cycles/iter
[TVM] Result: PASS

[TVM] Benchmarking MLP Inference (512 -> 1024 -> 512)...
[TVM] MLP Inference: XXXXX cycles/iter, X.XX infer/sec
[TVM] Result: PASS

=== Benchmark Summary ===
Dense Layer:   PASS (X.XX GFLOPS)
ReLU:          PASS
Softmax:       PASS
MLP Inference: PASS

Overall: PASS
=====================================
```

**Success Criteria:**
- ✓ All benchmarks execute without errors
- ✓ Performance metrics are displayed
- ✓ At least 3 out of 4 tests show "PASS"
- ✓ GFLOPS and throughput values are reasonable (> 0)

### Step 6: Performance Validation

The performance should be validated against these criteria:

1. **Dense Layer Performance:**
   - Should complete in < 10M cycles per iteration
   - GFLOPS should be > 0.1 GFLOPS

2. **ReLU Performance:**
   - Should complete in < 5M cycles per iteration

3. **Softmax Performance:**
   - Should complete in < 10M cycles per iteration

4. **MLP Inference:**
   - Should complete end-to-end inference
   - Throughput should be > 0 inferences/sec

**Note:** Exact performance numbers will vary based on host system, QEMU configuration, and CPU features.

## Automated Test Script

For convenience, an automated test script is provided:

```bash
cd kernel
./test_tvm_e2e.sh
```

This script will:
1. Build the kernel
2. Boot test the kernel
3. Test tvmload command
4. Test tvmrun command
5. Verify output generation
6. Test tvmbench command
7. Validate performance metrics

## Troubleshooting

### Module Loading Fails

**Error:** "ERROR: Failed to load TVM module"

**Solutions:**
1. Check that TVM runtime is initialized (should happen automatically at boot)
2. Verify sufficient memory is available (check with `mem` command)
3. Check kernel logs for more detailed error messages

### Inference Execution Fails

**Error:** "ERROR: Inference failed with code X"

**Solutions:**
1. Ensure module is loaded first with `tvmload`
2. Check that graph executor is properly initialized
3. Verify tensor allocation succeeds (memory available)

### Benchmark Failures

**Error:** Some benchmarks show "FAIL"

**Solutions:**
1. This may be acceptable if performance is just outside threshold
2. Check specific error messages in benchmark output
3. Verify host system has sufficient resources (QEMU not swapping)

### Kernel Panic

**Error:** Kernel crashes during TVM operations

**Solutions:**
1. Check memory management - may indicate buffer overflow
2. Verify graph executor state is valid
3. Enable debug output by modifying TVM_DEBUG in source

## Success Criteria Summary

The end-to-end integration test passes if:

1. ✅ Kernel builds without errors
2. ✅ Kernel boots successfully in QEMU
3. ✅ `tvmload` command loads test module successfully
4. ✅ `tvmrun` command executes inference and produces output
5. ✅ Output values are generated (float array)
6. ✅ `tvmbench` command runs all benchmarks
7. ✅ Performance is within 10% of standalone TVM (or acceptable for initial integration)

## Additional Verification

### Check TVM Runtime Status

Type at the prompt:
```
tvm
```

This will display TVM runtime statistics including:
- Initialization status
- Workspace allocation
- Loaded module information

### Memory Status

Type at the prompt:
```
mem
```

Verify sufficient memory is available for TVM operations.

## Notes

- The test TVM module is a simple MLP network (512 -> 1024 -> 512)
- Input data is initialized with normalized values (i/512.0)
- Output should be floating-point values representing network output
- Performance benchmarks use optimized SIMD operations where available

## Expected Test Duration

- Manual testing: ~5 minutes
- Automated testing: ~2 minutes

## Reporting Issues

If any test fails, collect the following information:
1. Full kernel boot log
2. Output of failed command
3. Memory status (`mem` command)
4. TVM runtime status (`tvm` command)
5. Host system details (QEMU version, CPU)
