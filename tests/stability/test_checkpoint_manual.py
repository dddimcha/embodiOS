#!/usr/bin/env python3
"""
Manual verification test for checkpoint/resume functionality.

Run this test to verify:
1. Checkpoints are saved correctly
2. Checkpoint data is persisted
3. Tests can resume from checkpoint
4. Graceful shutdown saves state

Usage:
    python3 tests/stability/test_checkpoint_manual.py
"""

import os
import tempfile
import time
from pathlib import Path

from tests.stability.test_long_running import StabilityTestRunner
from tests.stability.config import StabilityConfig
from tests.stability.checkpoint import CheckpointManager


def test_checkpoint_save_load():
    """Test checkpoint save and load"""
    print("\n=== Test 1: Checkpoint Save/Load ===")

    tmpdir = Path(tempfile.mkdtemp())
    print(f"Using temp directory: {tmpdir}")

    # Create config with checkpointing
    config = StabilityConfig.smoke_test(duration_seconds=10)
    config.checkpoint_interval_seconds = 3

    # Run test with checkpoints
    manager = CheckpointManager(checkpoint_dir=tmpdir)
    runner = StabilityTestRunner(
        config,
        deterministic=True,
        checkpoint_manager=manager,
        test_name='test_save_load'
    )

    print("Running test...")
    result = runner.run()

    print(f"✓ Test completed: {result['duration']:.1f}s")
    print(f"✓ Requests: {result['requests_processed']}")
    print(f"✓ Checkpoints saved: {result['checkpoint_count']}")

    # Verify checkpoints exist
    checkpoints = manager.list_checkpoints('test_save_load')
    assert len(checkpoints) > 0, "No checkpoints found"
    print(f"✓ Found {len(checkpoints)} checkpoint(s)")

    # Load checkpoint
    checkpoint_data = manager.load_checkpoint(checkpoints[0])
    print(f"✓ Loaded checkpoint: {checkpoint_data.test_name}")
    print(f"  - Elapsed: {checkpoint_data.elapsed_time:.1f}s")
    print(f"  - Requests: {checkpoint_data.requests_processed}")
    print(f"  - Warmup completed: {checkpoint_data.warmup_completed}")

    # Cleanup
    for cp in checkpoints:
        cp.unlink()
    tmpdir.rmdir()

    print("✓ Test 1 PASSED\n")


def test_checkpoint_resume():
    """Test resuming from checkpoint"""
    print("\n=== Test 2: Checkpoint Resume ===")

    tmpdir = Path(tempfile.mkdtemp())
    print(f"Using temp directory: {tmpdir}")

    # Run initial test
    config1 = StabilityConfig.smoke_test(duration_seconds=8)
    config1.checkpoint_interval_seconds = 2

    manager1 = CheckpointManager(checkpoint_dir=tmpdir)
    runner1 = StabilityTestRunner(
        config1,
        deterministic=True,
        checkpoint_manager=manager1,
        test_name='test_resume'
    )

    print("Running initial test...")
    result1 = runner1.run()
    print(f"✓ Initial test: {result1['requests_processed']} requests")

    # Load checkpoint
    checkpoint_data = manager1.load_latest_checkpoint('test_resume')
    assert checkpoint_data is not None, "No checkpoint to resume from"
    print(f"✓ Checkpoint loaded: {checkpoint_data.requests_processed} requests")

    # Resume from checkpoint
    config2 = StabilityConfig.smoke_test(duration_seconds=15)  # Longer duration
    manager2 = CheckpointManager(checkpoint_dir=tmpdir)
    runner2 = StabilityTestRunner(
        config2,
        deterministic=True,
        checkpoint_manager=manager2,
        resume_from_checkpoint=checkpoint_data,
        test_name='test_resume'
    )

    print("Resuming from checkpoint...")
    result2 = runner2.run()
    print(f"✓ Resumed test: {result2['requests_processed']} requests")
    print(f"✓ Resume count: {result2['resume_count']}")
    print(f"✓ Total duration: {result2['duration']:.1f}s")

    assert result2['resume_count'] == 1, f"Expected resume_count=1, got {result2['resume_count']}"

    # Cleanup
    for cp in manager2.list_checkpoints('test_resume'):
        cp.unlink()
    tmpdir.rmdir()

    print("✓ Test 2 PASSED\n")


def test_checkpoint_cleanup():
    """Test checkpoint cleanup functionality"""
    print("\n=== Test 3: Checkpoint Cleanup ===")

    tmpdir = Path(tempfile.mkdtemp())
    print(f"Using temp directory: {tmpdir}")

    manager = CheckpointManager(checkpoint_dir=tmpdir)

    # Create multiple checkpoints by running short tests
    for i in range(8):
        config = StabilityConfig.smoke_test(duration_seconds=6)
        config.checkpoint_interval_seconds = 2  # Enable checkpointing
        runner = StabilityTestRunner(
            config,
            deterministic=True,
            checkpoint_manager=manager,
            test_name='test_cleanup'
        )
        runner.run()
        time.sleep(0.1)  # Small delay to ensure different timestamps

    checkpoints = manager.list_checkpoints('test_cleanup')
    initial_count = len(checkpoints)
    print(f"✓ Created {initial_count} checkpoints")

    # Cleanup old checkpoints (keep 5)
    deleted = manager.cleanup_old_checkpoints('test_cleanup', keep_count=5)
    print(f"✓ Deleted {deleted} old checkpoint(s)")

    remaining = manager.list_checkpoints('test_cleanup')
    print(f"✓ Remaining checkpoints: {len(remaining)}")

    assert len(remaining) == 5, f"Expected 5 checkpoints, found {len(remaining)}"

    # Cleanup all
    for cp in remaining:
        cp.unlink()
    tmpdir.rmdir()

    print("✓ Test 3 PASSED\n")


def test_signal_handler_setup():
    """Test signal handler setup and teardown"""
    print("\n=== Test 4: Signal Handler Setup ===")

    tmpdir = Path(tempfile.mkdtemp())
    manager = CheckpointManager(checkpoint_dir=tmpdir)

    print("Setting up signal handlers...")
    manager.setup_signal_handlers()
    print("✓ Signal handlers registered")

    # Verify shutdown_requested flag
    assert manager.shutdown_requested == False, "shutdown_requested should be False initially"
    print("✓ shutdown_requested flag is False")

    # Restore handlers
    manager.restore_signal_handlers()
    print("✓ Signal handlers restored")

    tmpdir.rmdir()
    print("✓ Test 4 PASSED\n")


def main():
    """Run all manual verification tests"""
    print("=" * 70)
    print("CHECKPOINT/RESUME MANUAL VERIFICATION")
    print("=" * 70)

    try:
        test_checkpoint_save_load()
        test_checkpoint_resume()
        test_checkpoint_cleanup()
        test_signal_handler_setup()

        print("=" * 70)
        print("ALL TESTS PASSED ✓")
        print("=" * 70)
        print("\nManual verification steps:")
        print("1. Run: PYTHONPATH=. python3 tests/stability/demo_checkpoint.py --duration 60")
        print("2. Press Ctrl+C after a few seconds to trigger graceful shutdown")
        print("3. Verify checkpoint saved with message: '[Checkpoint] Saved to...'")
        print("4. Run: PYTHONPATH=. python3 tests/stability/demo_checkpoint.py --resume")
        print("5. Verify test resumes from checkpoint and completes")
        print("")
        return 0

    except Exception as e:
        print(f"\n✗ TEST FAILED: {e}")
        import traceback
        traceback.print_exc()
        return 1


if __name__ == '__main__':
    import sys
    sys.exit(main())
