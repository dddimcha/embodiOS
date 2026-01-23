# Checkpoint/Resume Support for Long-Running Stability Tests

This module provides checkpoint/resume functionality for long-running stability tests, enabling tests to be paused and resumed without losing progress.

## Features

- **Graceful Shutdown**: Handles SIGTERM and SIGINT (Ctrl+C) signals to save state before exit
- **State Persistence**: Saves complete test state including metrics, latencies, and timing
- **Resume Support**: Resumes tests from checkpoint with full state restoration
- **Periodic Checkpoints**: Automatically saves checkpoints at configurable intervals
- **Automatic Cleanup**: Keeps only the last N checkpoints to prevent disk bloat
- **Memory Efficient**: Stores last 1000 latencies to balance completeness and size

## Files

- `checkpoint.py`: Core checkpoint manager and data structures
- `test_long_running.py`: Modified test runner with checkpoint support
- `demo_checkpoint.py`: Interactive demonstration script
- `test_checkpoint_manual.py`: Automated verification tests
- `verify_graceful_shutdown.sh`: Shell script for manual SIGTERM testing

## Quick Start

### Running a Test with Checkpoints

```bash
# Start a 60-second test with checkpoints every 10 seconds
PYTHONPATH=. python3 tests/stability/demo_checkpoint.py --duration 60 --checkpoint-interval 10
```

### Interrupting a Test

Press **Ctrl+C** during the test to trigger graceful shutdown. The checkpoint will be saved automatically:

```
^C
[Checkpoint] Received SIGINT, initiating graceful shutdown...
[Shutdown] Graceful shutdown in progress...
[Checkpoint] Saved to checkpoints/demo_stability_test_20260123_020204.json
[Shutdown] Checkpoint saved. Test can be resumed later.
```

### Resuming from Checkpoint

```bash
# Resume the most recent checkpoint
PYTHONPATH=. python3 tests/stability/demo_checkpoint.py --resume
```

Output:
```
=== Attempting to resume from checkpoint ===
[Checkpoint] Loaded from checkpoints/demo_stability_test_20260123_020204.json
[Checkpoint] Test: demo_stability_test, Elapsed: 5.0s, Requests: 150
[Resume] Continuing test from 5.0s
[Resume] Remaining: 55.0s
```

## Programmatic Usage

### Creating a Test with Checkpoint Support

```python
from tests.stability.test_long_running import StabilityTestRunner
from tests.stability.config import StabilityConfig
from tests.stability.checkpoint import CheckpointManager
from pathlib import Path

# Create checkpoint manager
manager = CheckpointManager(checkpoint_dir=Path('./checkpoints'))

# Create configuration
config = StabilityConfig.long_test()  # 72-hour test
config.checkpoint_interval_seconds = 3600  # Checkpoint every hour

# Create runner with checkpoint support
runner = StabilityTestRunner(
    config=config,
    checkpoint_manager=manager,
    test_name='my_72h_test'
)

# Run test
result = runner.run()
```

### Resuming from Checkpoint

```python
# Load latest checkpoint
checkpoint_data = manager.load_latest_checkpoint('my_72h_test')

if checkpoint_data:
    # Create runner with resumed state
    runner = StabilityTestRunner(
        config=config,
        checkpoint_manager=manager,
        resume_from_checkpoint=checkpoint_data,
        test_name='my_72h_test'
    )

    # Continue test
    result = runner.run()
```

## Checkpoint Data Structure

Checkpoints are saved as JSON files containing:

```json
{
  "test_name": "demo_stability_test",
  "config_dict": { /* StabilityConfig parameters */ },
  "start_time": 1769130122.382772,
  "elapsed_time": 4.0,
  "requests_processed": 133,
  "warmup_completed": true,
  "baseline_snapshot_dict": { /* ResourceSnapshot data */ },
  "latencies": [ /* last 1000 latencies */ ],
  "metrics_data": [ /* all MetricPoint data */ ],
  "checkpoint_time": 1769130126.416103,
  "checkpoint_count": 2,
  "resume_count": 0,
  "_checkpoint_metadata": {
    "version": "1.0",
    "saved_at": 1769130126.416103,
    "saved_at_iso": "2026-01-23T02:02:06.416103"
  }
}
```

## Configuration

### Checkpoint Interval

Set in `StabilityConfig`:

```python
config = StabilityConfig.long_test()
config.checkpoint_interval_seconds = 3600  # Checkpoint every hour
```

Set to `0` to disable periodic checkpoints (graceful shutdown still saves):

```python
config.checkpoint_interval_seconds = 0  # Only save on shutdown
```

### Checkpoint Directory

Default: `./checkpoints`

Custom directory:

```python
manager = CheckpointManager(checkpoint_dir=Path('/var/lib/stability/checkpoints'))
```

### Checkpoint Retention

By default, only the last 5 checkpoints are kept:

```python
# Manual cleanup
deleted_count = manager.cleanup_old_checkpoints(
    test_name='my_test',
    keep_count=5  # Keep last 5
)
```

Automatic cleanup happens after each checkpoint save.

## Signal Handling

The checkpoint manager registers handlers for:

- **SIGTERM**: Sent by system shutdown, container stop, or `kill <pid>`
- **SIGINT**: Sent by Ctrl+C in terminal

Both signals trigger graceful shutdown:

1. Set `shutdown_requested` flag
2. Current test loop checks flag
3. Save checkpoint with all state
4. Restore original signal handlers
5. Return partial result with `shutdown_graceful=True`

## Verification

### Automated Tests

Run the comprehensive verification suite:

```bash
PYTHONPATH=. python3 tests/stability/test_checkpoint_manual.py
```

Tests verify:
- Checkpoint save/load functionality
- Resume from checkpoint
- Checkpoint cleanup
- Signal handler setup/teardown

### Manual Verification

Run the shell script to test SIGTERM handling:

```bash
./tests/stability/verify_graceful_shutdown.sh
```

This script:
1. Starts a 20-second test
2. Waits 5 seconds
3. Sends SIGTERM
4. Verifies checkpoint saved
5. Resumes from checkpoint
6. Completes the test

## Result Fields

Test results include checkpoint-related fields:

```python
result = {
    'passed': True,
    'duration': 72.5,
    'requests_processed': 2500,
    # ... standard fields ...
    'checkpoint_count': 72,      # Number of checkpoints saved
    'resume_count': 2,           # Number of times resumed
    'shutdown_graceful': False,  # True if interrupted
    'incomplete': False          # True if test didn't finish
}
```

## Limitations

- **Latency History**: Only last 1000 latencies stored in checkpoint (memory efficiency)
- **Engine State**: MockInferenceEngine state not preserved (inference count, leaked memory)
- **Process State**: Only current process resources tracked, not system-wide
- **File Descriptors**: Platform-dependent (Windows doesn't support `num_fds`)

## Use Cases

### 72-Hour Acceptance Test

```python
config = StabilityConfig.long_test()  # 72 hours
config.checkpoint_interval_seconds = 3600  # Hourly checkpoints

manager = CheckpointManager()
runner = StabilityTestRunner(config, checkpoint_manager=manager, test_name='acceptance_72h')
result = runner.run()
```

If interrupted, resume with:

```python
checkpoint = manager.load_latest_checkpoint('acceptance_72h')
runner = StabilityTestRunner(config, checkpoint_manager=manager,
                            resume_from_checkpoint=checkpoint, test_name='acceptance_72h')
result = runner.run()
```

### Continuous Integration

For CI environments with time limits:

```python
# Run for max CI time (e.g., 2 hours)
config = StabilityConfig.short_test()
config.checkpoint_interval_seconds = 600  # Checkpoint every 10 minutes

# If job is killed, checkpoint is saved
# Next CI run can resume from checkpoint
```

### Development Testing

Quick iterations during development:

```python
# Run for 30 seconds, interrupt, verify checkpoint, resume
config = StabilityConfig.smoke_test(duration_seconds=30)
config.checkpoint_interval_seconds = 5

# Ctrl+C after a few seconds
# Resume to test continuation logic
```

## Troubleshooting

### Checkpoint Not Saving

Check that `checkpoint_interval_seconds > 0` or graceful shutdown is triggered:

```python
print(f"Checkpoint interval: {config.checkpoint_interval_seconds}")
print(f"Checkpoint manager: {runner.checkpoint_manager is not None}")
```

### Resume Not Working

Verify checkpoint file exists and is valid:

```python
checkpoints = manager.list_checkpoints('test_name')
print(f"Found {len(checkpoints)} checkpoint(s)")

if checkpoints:
    try:
        checkpoint = manager.load_checkpoint(checkpoints[0])
        print(f"Loaded: {checkpoint.test_name}")
    except Exception as e:
        print(f"Error loading checkpoint: {e}")
```

### Signal Handler Conflicts

If using other signal handlers, be aware that checkpoint manager overrides SIGTERM/SIGINT.
Call `restore_signal_handlers()` to restore original handlers when done.

## Performance Impact

Checkpoint operations have minimal impact on test performance:

- **Checkpoint Save**: ~10-50ms depending on metrics count
- **Checkpoint Load**: ~5-20ms for typical checkpoint files
- **Memory**: ~1MB per checkpoint file (varies with metrics count)
- **Disk I/O**: Sequential writes, no blocking operations during test loop

Checkpoint saves occur asynchronously relative to inference requests, so they don't affect latency measurements.
