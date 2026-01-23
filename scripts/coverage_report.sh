#!/bin/bash
# Kernel Coverage Report Generation Script
# Generates code coverage reports using gcov/lcov
#
# Usage: ./coverage_report.sh
#
# Prerequisites:
#   - lcov package installed (brew install lcov or apt-get install lcov)
#   - Kernel must be built with COVERAGE=1 flag

set -e

echo "=== Kernel Coverage Report Generator ==="
echo ""

# Configuration
KERNEL_DIR="kernel"
COVERAGE_DIR="coverage"
COVERAGE_INFO="coverage.info"
COVERAGE_HTML_DIR="coverage_html"

# Check for required tools
if ! command -v lcov &> /dev/null; then
    echo "ERROR: lcov not found"
    echo "Install lcov:"
    echo "  macOS:  brew install lcov"
    echo "  Linux:  apt-get install lcov"
    exit 1
fi

if ! command -v genhtml &> /dev/null; then
    echo "ERROR: genhtml not found (should be part of lcov package)"
    exit 1
fi

echo "Tools found:"
echo "  lcov:    $(which lcov)"
echo "  genhtml: $(which genhtml)"
echo ""

# Step 1: Clean old coverage data
echo "=== Step 1: Cleaning old coverage data ==="
if [ -d "$COVERAGE_DIR" ]; then
    echo "Removing old coverage directory..."
    rm -rf "$COVERAGE_DIR"
fi
if [ -d "$COVERAGE_HTML_DIR" ]; then
    echo "Removing old HTML report..."
    rm -rf "$COVERAGE_HTML_DIR"
fi

# Clean .gcda files from previous runs (if any)
echo "Removing old .gcda files..."
find "$KERNEL_DIR" -name "*.gcda" -delete 2>/dev/null || true
echo ""

# Step 2: Build kernel with coverage instrumentation
echo "=== Step 2: Building kernel with coverage ==="
echo "Running: cd $KERNEL_DIR && make clean && make COVERAGE=1"
(cd "$KERNEL_DIR" && make clean && make COVERAGE=1)

if [ ! -f "$KERNEL_DIR/embodios.elf" ]; then
    echo "ERROR: Failed to build kernel with coverage"
    exit 1
fi

echo "Kernel built successfully with coverage instrumentation"
echo ""

# Verify .gcno files were generated
GCNO_COUNT=$(find "$KERNEL_DIR" -name "*.gcno" | wc -l)
if [ "$GCNO_COUNT" -eq 0 ]; then
    echo "ERROR: No .gcno files found. Coverage instrumentation may have failed."
    exit 1
fi

echo "Coverage instrumentation verified: $GCNO_COUNT .gcno files generated"
echo ""

# Step 3: Capture baseline coverage (zero coverage)
echo "=== Step 3: Capturing baseline coverage ==="
mkdir -p "$COVERAGE_DIR"

echo "Running: lcov --capture --initial --directory $KERNEL_DIR --output-file $COVERAGE_DIR/baseline.info"
lcov --capture --initial --directory "$KERNEL_DIR" --output-file "$COVERAGE_DIR/baseline.info" --rc lcov_branch_coverage=1 2>&1 | grep -v "ignoring data for external file" || true

echo "Baseline coverage captured"
echo ""

# Step 4: Run kernel tests in QEMU
echo "=== Step 4: Running kernel tests ==="
echo "Note: .gcda files will be generated during test execution"
echo ""

# Copy kernel binary to parent directory for test script
if [ -f "$KERNEL_DIR/embodios.elf" ]; then
    cp "$KERNEL_DIR/embodios.elf" .
fi

# Run tests (this should generate .gcda files in a real setup)
# Note: With current gcov_stubs.c implementation, .gcda files are not actually written
#       In production, this would require kernel support to export coverage data via
#       virtio disk, serial output, or similar mechanism
echo "Running: bash scripts/run_kernel_tests.sh"
if bash scripts/run_kernel_tests.sh; then
    echo "Tests completed successfully"
else
    echo "WARNING: Tests failed, but continuing with coverage report"
fi
echo ""

# Step 5: Capture test coverage
echo "=== Step 5: Capturing test coverage ==="

# Check if any .gcda files exist
GCDA_COUNT=$(find "$KERNEL_DIR" -name "*.gcda" 2>/dev/null | wc -l)
if [ "$GCDA_COUNT" -eq 0 ]; then
    echo "WARNING: No .gcda files found after test run"
    echo ""
    echo "This is expected with the current gcov_stubs.c implementation."
    echo "The kernel runs in a bare-metal environment without filesystem support"
    echo "to write .gcda files. To enable full coverage reporting, implement one of:"
    echo "  - VirtIO disk output for coverage data"
    echo "  - Serial port coverage data transmission"
    echo "  - Memory-mapped coverage buffer extraction"
    echo ""
    echo "Generating baseline-only coverage report..."
    cp "$COVERAGE_DIR/baseline.info" "$COVERAGE_DIR/$COVERAGE_INFO"
else
    echo "Found $GCDA_COUNT .gcda files, capturing coverage..."
    lcov --capture --directory "$KERNEL_DIR" --output-file "$COVERAGE_DIR/test.info" --rc lcov_branch_coverage=1 2>&1 | grep -v "ignoring data for external file" || true

    # Combine baseline and test coverage
    echo "Combining baseline and test coverage..."
    lcov --add-tracefile "$COVERAGE_DIR/baseline.info" --add-tracefile "$COVERAGE_DIR/test.info" --output-file "$COVERAGE_DIR/$COVERAGE_INFO" --rc lcov_branch_coverage=1 2>&1 | grep -v "ignoring data for external file" || true
fi

echo "Coverage data captured"
echo ""

# Step 6: Filter coverage data (remove external files, test files, etc.)
echo "=== Step 6: Filtering coverage data ==="
echo "Removing external and generated files from coverage..."

# Remove coverage from:
#   - System headers (/usr/*)
#   - LLAMA.cpp library (llama_cpp/*)
#   - Test files (test/*)
#   - Generated model files (ai/*_model.c, ai/*_flag.c)
lcov --remove "$COVERAGE_DIR/$COVERAGE_INFO" \
    '/usr/*' \
    '*/llama_cpp/*' \
    '*/test/*' \
    '*/ai/tinystories_model.c' \
    '*/ai/gguf_model.c' \
    '*/ai/*_flag.c' \
    --output-file "$COVERAGE_DIR/${COVERAGE_INFO}.filtered" \
    --rc lcov_branch_coverage=1 2>&1 | grep -v "ignoring data for external file" || true

mv "$COVERAGE_DIR/${COVERAGE_INFO}.filtered" "$COVERAGE_DIR/$COVERAGE_INFO"

echo "Coverage data filtered"
echo ""

# Step 7: Generate HTML report
echo "=== Step 7: Generating HTML report ==="
echo "Running: genhtml --output-directory $COVERAGE_HTML_DIR $COVERAGE_DIR/$COVERAGE_INFO"

genhtml --output-directory "$COVERAGE_HTML_DIR" \
    "$COVERAGE_DIR/$COVERAGE_INFO" \
    --title "EMBODIOS Kernel Coverage" \
    --show-details \
    --legend \
    --branch-coverage \
    --rc genhtml_branch_coverage=1 2>&1 | grep -E "(Overall|Creating)" || true

echo ""
echo "=== Coverage Report Complete ==="
echo ""
echo "Coverage data: $COVERAGE_DIR/$COVERAGE_INFO"
echo "HTML report:   $COVERAGE_HTML_DIR/index.html"
echo ""

# Display coverage summary
if [ -f "$COVERAGE_DIR/$COVERAGE_INFO" ]; then
    echo "Coverage Summary:"
    lcov --summary "$COVERAGE_DIR/$COVERAGE_INFO" --rc lcov_branch_coverage=1 2>&1 | grep -E "(lines|functions|branches)" || true
    echo ""
fi

# Check if we can open the report
if command -v open &> /dev/null; then
    echo "To view the report, run:"
    echo "  open $COVERAGE_HTML_DIR/index.html"
elif command -v xdg-open &> /dev/null; then
    echo "To view the report, run:"
    echo "  xdg-open $COVERAGE_HTML_DIR/index.html"
else
    echo "Open $COVERAGE_HTML_DIR/index.html in your web browser to view the coverage report"
fi

echo ""
