#!/bin/bash
# Kernel Unit Test Runner Script
# Runs kernel unit tests in QEMU and reports results
#
# Usage: ./run_kernel_tests.sh [test_name]
#        If test_name is provided, runs only that test
#        Otherwise, runs all tests

set -e

TEST_NAME="${1:-}"

echo "=== Kernel Unit Test Runner ==="
echo ""

# Check if kernel binary exists
if [ ! -f "embodios.elf" ]; then
    echo "ERROR: embodios.elf not found"
    echo "Building kernel..."
    (cd kernel && make)
    # Copy binary to working directory
    if [ -f "kernel/embodios.elf" ]; then
        cp kernel/embodios.elf .
    else
        echo "ERROR: Failed to build kernel"
        exit 1
    fi
fi

echo "Kernel binary: embodios.elf"
echo ""

# Check if QEMU is available
if ! command -v qemu-system-x86_64 &> /dev/null; then
    echo "ERROR: qemu-system-x86_64 not found"
    echo "Install QEMU: brew install qemu"
    exit 1
fi

# Setup kernel command line for test mode
if [ -n "$TEST_NAME" ]; then
    KERNEL_CMDLINE="runtest=$TEST_NAME"
    echo "Running single test: $TEST_NAME"
else
    KERNEL_CMDLINE="test"
    echo "Running all tests"
fi

echo ""
echo "=== Starting QEMU Test Environment ==="
echo ""
echo "Expected behavior:"
echo "  1. Kernel boots in test mode"
echo "  2. Test framework initializes"
echo "  3. Tests execute with results printed to serial"
echo "  4. Kernel shuts down after tests complete"
echo ""
echo "----------------------------------------"
echo ""

# Create temporary file for test output
TEST_OUTPUT=$(mktemp /tmp/kernel_test_output.XXXXXX)

# Trap to cleanup temp file on exit
trap "rm -f $TEST_OUTPUT" EXIT

# Launch QEMU with test mode enabled
# -append: Pass kernel command line parameter
# -serial stdio: Redirect serial output to stdout/stdin
# -display none: No graphical display
# -no-reboot: Exit instead of rebooting on triple fault
# -no-shutdown: Keep QEMU running after guest shutdown for clean exit
qemu-system-x86_64 \
    -kernel embodios.elf \
    -m 2G \
    -append "$KERNEL_CMDLINE" \
    -serial stdio \
    -display none \
    -no-reboot \
    -no-shutdown 2>&1 | tee "$TEST_OUTPUT"

echo ""
echo "=== Parsing Test Results ==="
echo ""

# Parse test results from output
# Look for patterns like:
#   "Tests passed: X/Y"
#   "Tests failed: Z"
#   "All tests passed"

if grep -q "All tests passed" "$TEST_OUTPUT" || grep -q "Tests passed:.*0 failed" "$TEST_OUTPUT"; then
    echo "✓ All tests passed"
    exit 0
elif grep -q "Tests failed:" "$TEST_OUTPUT"; then
    FAILED_COUNT=$(grep "Tests failed:" "$TEST_OUTPUT" | tail -1 | sed 's/.*Tests failed: \([0-9]*\).*/\1/')
    echo "✗ Tests failed: $FAILED_COUNT"
    exit 1
else
    # Check if output contains test results at all
    if grep -q "TEST:" "$TEST_OUTPUT" || grep -q "PASS:" "$TEST_OUTPUT" || grep -q "FAIL:" "$TEST_OUTPUT"; then
        # Tests ran but no summary found - check for failures
        if grep -q "FAIL:" "$TEST_OUTPUT"; then
            echo "✗ Some tests failed (see output above)"
            exit 1
        else
            echo "✓ Tests completed (verify output above)"
            exit 0
        fi
    else
        echo "⚠ Unable to determine test results"
        echo "Check output above for details"
        exit 2
    fi
fi
