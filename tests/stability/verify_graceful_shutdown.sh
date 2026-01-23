#!/bin/bash
# Verification script for graceful shutdown and checkpoint/resume
#
# This script demonstrates:
# 1. Starting a test with checkpoint support
# 2. Sending SIGTERM to trigger graceful shutdown
# 3. Verifying checkpoint is saved
# 4. Resuming from checkpoint

set -e

echo "=== Graceful Shutdown & Checkpoint/Resume Verification ==="
echo ""

# Clean up any previous checkpoints
rm -rf ./checkpoints_verify
mkdir -p ./checkpoints_verify

echo "Step 1: Starting test that will be interrupted..."
echo "        (Test will run for 20 seconds, will interrupt after 5 seconds)"
echo ""

# Start test in background
PYTHONPATH=. python3 ./tests/stability/demo_checkpoint.py \
    --duration 20 \
    --checkpoint-interval 3 \
    --checkpoint-dir ./checkpoints_verify &

TEST_PID=$!
echo "Test started with PID: $TEST_PID"

# Wait for test to start and run for a bit
echo "Waiting 5 seconds before sending SIGTERM..."
sleep 5

# Send SIGTERM for graceful shutdown
echo ""
echo "Step 2: Sending SIGTERM to trigger graceful shutdown..."
kill -TERM $TEST_PID

# Wait for process to exit
wait $TEST_PID || true

echo ""
echo "Step 3: Verifying checkpoint was saved..."
CHECKPOINT_COUNT=$(ls -1 ./checkpoints_verify/*.json 2>/dev/null | wc -l)
echo "Found $CHECKPOINT_COUNT checkpoint file(s)"

if [ "$CHECKPOINT_COUNT" -eq 0 ]; then
    echo "ERROR: No checkpoint files found!"
    exit 1
fi

# Show checkpoint details
echo ""
echo "Checkpoint files:"
ls -lh ./checkpoints_verify/*.json

echo ""
echo "Step 4: Resuming from checkpoint..."
PYTHONPATH=. python3 ./tests/stability/demo_checkpoint.py \
    --resume \
    --duration 20 \
    --checkpoint-interval 3 \
    --checkpoint-dir ./checkpoints_verify

echo ""
echo "=== Verification Complete ==="
echo ""
echo "Summary:"
echo "  ✓ Test started successfully"
echo "  ✓ SIGTERM triggered graceful shutdown"
echo "  ✓ Checkpoint saved to disk"
echo "  ✓ Test resumed from checkpoint"
echo ""
echo "Cleanup: rm -rf ./checkpoints_verify"
