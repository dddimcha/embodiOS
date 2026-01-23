#!/usr/bin/env python3
"""
Demonstration script for checkpoint/resume functionality.

This script demonstrates graceful shutdown and checkpoint/resume:
1. Starts a stability test with checkpoint support
2. Can be interrupted with Ctrl+C (SIGINT) or SIGTERM
3. Saves checkpoint on shutdown
4. Can be resumed from checkpoint

Usage:
    # Start new test
    python tests/stability/demo_checkpoint.py --duration 60

    # Resume from checkpoint
    python tests/stability/demo_checkpoint.py --resume

    # Interrupt with Ctrl+C to test graceful shutdown
"""

import argparse
import sys
from pathlib import Path

from tests.stability.test_long_running import StabilityTestRunner
from tests.stability.config import StabilityConfig
from tests.stability.checkpoint import CheckpointManager


def main():
    parser = argparse.ArgumentParser(description='Checkpoint/Resume Demo')
    parser.add_argument('--duration', type=int, default=60,
                      help='Test duration in seconds (default: 60)')
    parser.add_argument('--resume', action='store_true',
                      help='Resume from latest checkpoint')
    parser.add_argument('--checkpoint-dir', type=str, default='./checkpoints',
                      help='Checkpoint directory (default: ./checkpoints)')
    parser.add_argument('--checkpoint-interval', type=int, default=10,
                      help='Checkpoint interval in seconds (default: 10)')
    args = parser.parse_args()

    checkpoint_dir = Path(args.checkpoint_dir)
    test_name = 'demo_stability_test'

    # Initialize checkpoint manager
    manager = CheckpointManager(checkpoint_dir=checkpoint_dir)

    # Check if resuming
    checkpoint_data = None
    if args.resume:
        print("\n=== Attempting to resume from checkpoint ===")
        checkpoint_data = manager.load_latest_checkpoint(test_name)

        if checkpoint_data is None:
            print(f"No checkpoint found for '{test_name}' in {checkpoint_dir}")
            print("Starting new test instead...")
        else:
            print(f"Found checkpoint from {checkpoint_data.checkpoint_time}")
            print(f"Will resume from {checkpoint_data.elapsed_time:.1f}s with {checkpoint_data.requests_processed} requests")

    # Create config
    config = StabilityConfig.smoke_test(duration_seconds=args.duration)
    config.checkpoint_interval_seconds = args.checkpoint_interval

    print("\n=== Test Configuration ===")
    print(f"Duration: {config.duration_seconds}s")
    print(f"Checkpoint interval: {config.checkpoint_interval_seconds}s")
    print(f"Checkpoint directory: {checkpoint_dir}")
    print(f"Memory drift threshold: {config.memory_drift_threshold_pct}%")
    print(f"Latency degradation threshold: {config.latency_degradation_threshold_pct}%")

    # Create runner
    runner = StabilityTestRunner(
        config=config,
        deterministic=True,
        checkpoint_manager=manager,
        resume_from_checkpoint=checkpoint_data,
        test_name=test_name
    )

    print("\n=== Starting Test ===")
    print("Press Ctrl+C to trigger graceful shutdown and save checkpoint")
    print("-" * 70)

    try:
        # Run test
        result = runner.run()

        # Print results
        print("\n" + "=" * 70)
        print("TEST COMPLETED")
        print("=" * 70)
        print(f"Status:              {'PASSED' if result['passed'] else 'FAILED'}")
        print(f"Duration:            {result['duration']:.1f}s")
        print(f"Requests Processed:  {result['requests_processed']}")
        print(f"Memory Drift:        {result['memory_drift_pct']:.2f}% (threshold: {config.memory_drift_threshold_pct}%)")
        print(f"Latency Degradation: {result['latency_degradation_pct']:.2f}% (threshold: {config.latency_degradation_threshold_pct}%)")
        print(f"Peak Memory:         {result['peak_memory_mb']:.2f} MB")
        print(f"Avg CPU:             {result['avg_cpu_percent']:.1f}%")
        print(f"Checkpoints Saved:   {result['checkpoint_count']}")
        print(f"Resume Count:        {result['resume_count']}")
        print(f"Graceful Shutdown:   {result.get('shutdown_graceful', False)}")

        if result.get('incomplete', False):
            print("\nNote: Test was interrupted. Run with --resume to continue.")

        if result['failures']:
            print("\nFailures:")
            for failure in result['failures']:
                print(f"  - {failure}")

        print("=" * 70)

        # List available checkpoints
        checkpoints = manager.list_checkpoints(test_name)
        if checkpoints:
            print(f"\nAvailable checkpoints ({len(checkpoints)}):")
            for cp in checkpoints:
                print(f"  - {cp.name}")

        return 0 if result['passed'] else 1

    except KeyboardInterrupt:
        print("\n\n[Manual Interrupt] Ctrl+C detected - checkpoint should have been saved")
        return 2
    except Exception as e:
        print(f"\n\nError: {e}")
        import traceback
        traceback.print_exc()
        return 3


if __name__ == '__main__':
    sys.exit(main())
