#!/usr/bin/env python3
"""
Stability suite runner script for EMBODIOS.

Runs long-running stability tests with continuous resource monitoring, drift
detection, and HTML report generation. Supports checkpoint/resume for extended
test durations.

Usage:
    # Run 60-second smoke test
    python scripts/run_stability_suite.py --duration 60 --report /tmp/stability_report.html

    # Run 1-hour test with checkpoints
    python scripts/run_stability_suite.py --duration 3600 --report ./stability_1h.html

    # Run 72-hour acceptance test with checkpoints
    python scripts/run_stability_suite.py --duration 259200 --report ./stability_72h.html
"""

import argparse
import sys
import time
from pathlib import Path

# Add src to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent / "src"))
sys.path.insert(0, str(Path(__file__).parent.parent))

from tests.stability.config import StabilityConfig
from tests.stability.test_long_running import StabilityTestRunner
from tests.stability.report import StabilityReport, ReportConfig
from tests.stability.checkpoint import CheckpointManager
from tests.stability.alerts import AlertManager, StdoutAlertChannel
from tests.stability.analysis import DriftAnalyzer


def parse_args():
    """Parse command-line arguments"""
    parser = argparse.ArgumentParser(
        description='Run EMBODIOS long-running stability tests',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Quick smoke test (60 seconds)
  python scripts/run_stability_suite.py --duration 60 --report /tmp/report.html

  # 1-hour test
  python scripts/run_stability_suite.py --duration 3600 --report ./stability_1h.html

  # 12-hour test with checkpoints
  python scripts/run_stability_suite.py --duration 43200 --report ./stability_12h.html

  # 72-hour acceptance test
  python scripts/run_stability_suite.py --duration 259200 --report ./stability_72h.html
        """
    )

    parser.add_argument(
        '--duration',
        type=int,
        required=True,
        help='Test duration in seconds (e.g., 60 for 1 minute, 3600 for 1 hour)'
    )

    parser.add_argument(
        '--report',
        type=str,
        required=True,
        help='Path to save HTML test report (e.g., /tmp/stability_report.html)'
    )

    parser.add_argument(
        '--checkpoint-dir',
        type=str,
        default=None,
        help='Directory for checkpoint storage (enables checkpoint/resume support)'
    )

    parser.add_argument(
        '--checkpoint-interval',
        type=int,
        default=None,
        help='Checkpoint interval in seconds (overrides automatic calculation)'
    )

    parser.add_argument(
        '--resume',
        action='store_true',
        help='Resume from latest checkpoint if available'
    )

    parser.add_argument(
        '--memory-threshold',
        type=float,
        default=5.0,
        help='Memory drift threshold percentage (default: 5.0)'
    )

    parser.add_argument(
        '--latency-threshold',
        type=float,
        default=10.0,
        help='Latency degradation threshold percentage (default: 10.0)'
    )

    parser.add_argument(
        '--enable-alerts',
        action='store_true',
        help='Enable real-time alerting for drift detection'
    )

    parser.add_argument(
        '--deterministic',
        action='store_true',
        help='Use deterministic latency for stable test results'
    )

    return parser.parse_args()


def create_config(args) -> StabilityConfig:
    """
    Create StabilityConfig from command-line arguments.

    Args:
        args: Parsed command-line arguments

    Returns:
        StabilityConfig with appropriate settings
    """
    # Determine monitoring interval based on duration
    if args.duration <= 300:  # 5 minutes or less
        monitoring_interval = 5
    elif args.duration <= 3600:  # 1 hour or less
        monitoring_interval = 10
    elif args.duration <= 43200:  # 12 hours or less
        monitoring_interval = 30
    else:  # Longer tests
        monitoring_interval = 60

    # Determine checkpoint interval based on duration
    checkpoint_interval = 0  # Disabled by default
    if args.checkpoint_interval is not None:
        # Use explicit checkpoint interval if provided
        checkpoint_interval = args.checkpoint_interval
    elif args.checkpoint_dir:
        if args.duration <= 3600:  # 1 hour or less
            checkpoint_interval = 600  # Every 10 minutes
        elif args.duration <= 43200:  # 12 hours or less
            checkpoint_interval = 1800  # Every 30 minutes
        else:  # Longer tests
            checkpoint_interval = 3600  # Every hour

    # Determine minimum requests based on duration
    if args.duration <= 60:
        min_requests = 10
    elif args.duration <= 300:
        min_requests = 50
    elif args.duration <= 3600:
        min_requests = 100
    elif args.duration <= 43200:
        min_requests = 1000
    else:
        min_requests = 10000  # Acceptance criteria for 72-hour test

    return StabilityConfig(
        duration_seconds=args.duration,
        monitoring_interval_seconds=monitoring_interval,
        memory_drift_threshold_pct=args.memory_threshold,
        latency_degradation_threshold_pct=args.latency_threshold,
        min_requests=min_requests,
        warmup_requests=max(10, min_requests // 10),
        checkpoint_interval_seconds=checkpoint_interval,
    )


def format_duration(seconds: float) -> str:
    """Format duration in human-readable format"""
    if seconds < 60:
        return f"{seconds:.1f}s"
    elif seconds < 3600:
        minutes = seconds / 60
        return f"{minutes:.1f}m ({seconds:.0f}s)"
    else:
        hours = seconds / 3600
        return f"{hours:.2f}h ({seconds:.0f}s)"


def main():
    """Main entry point for stability suite runner"""
    args = parse_args()

    # Create configuration
    config = create_config(args)

    # Print test configuration
    print("=" * 70)
    print("EMBODIOS STABILITY TEST SUITE")
    print("=" * 70)
    print(f"Test Duration:         {format_duration(args.duration)}")
    print(f"Memory Threshold:      {args.memory_threshold}%")
    print(f"Latency Threshold:     {args.latency_threshold}%")
    print(f"Monitoring Interval:   {config.monitoring_interval_seconds}s")
    print(f"Minimum Requests:      {config.min_requests}")
    print(f"Report Output:         {args.report}")

    # Setup checkpoint manager if enabled
    checkpoint_manager = None
    resume_checkpoint = None

    if args.checkpoint_dir:
        checkpoint_dir = Path(args.checkpoint_dir)
        checkpoint_manager = CheckpointManager(checkpoint_dir)
        print(f"Checkpoint Directory:  {checkpoint_dir}")
        print(f"Checkpoint Interval:   {config.checkpoint_interval_seconds}s")

        # Resume from checkpoint if requested
        if args.resume:
            test_name = 'stability_test'
            resume_checkpoint = checkpoint_manager.load_latest_checkpoint(test_name)
            if resume_checkpoint:
                print(f"Resuming from checkpoint (elapsed: {resume_checkpoint.elapsed_time:.1f}s)")
            else:
                print("No checkpoint found - starting fresh test")

    # Setup alerting if enabled
    alert_manager = None
    if args.enable_alerts:
        alert_manager = AlertManager()
        alert_manager.add_channel(StdoutAlertChannel())
        print(f"Real-time Alerts:      Enabled")

    print("=" * 70)
    print()

    # Validate configuration
    try:
        config.validate()
    except ValueError as e:
        print(f"Error: Invalid configuration - {e}")
        return 1

    # Create and run stability test
    start_time = time.time()
    try:
        runner = StabilityTestRunner(
            config=config,
            deterministic=args.deterministic,
            checkpoint_manager=checkpoint_manager,
            resume_from_checkpoint=resume_checkpoint,
            test_name='stability_test',
            alert_manager=alert_manager,
        )

        result = runner.run()

    except KeyboardInterrupt:
        print("\n\n[Interrupted] Test was interrupted by user")
        print("If checkpoint support is enabled, test can be resumed later.")
        return 130  # Standard exit code for SIGINT

    except Exception as e:
        print(f"\n\nError: Test failed with exception: {e}")
        import traceback
        traceback.print_exc()
        return 1

    # Calculate actual duration
    actual_duration = time.time() - start_time

    # Print test summary
    print("\n" + "=" * 70)
    print("TEST SUMMARY")
    print("=" * 70)
    print(f"Status:              {'PASS ✓' if result['passed'] else 'FAIL ✗'}")
    print(f"Duration:            {format_duration(result['duration'])}")
    print(f"Requests Processed:  {result['requests_processed']}")
    print(f"Memory Drift:        {result['memory_drift_pct']:.2f}% (threshold: {args.memory_threshold}%)")
    print(f"Latency Degradation: {result['latency_degradation_pct']:.2f}% (threshold: {args.latency_threshold}%)")
    print(f"Peak Memory:         {result['peak_memory_mb']:.2f} MB")
    print(f"Avg CPU:             {result['avg_cpu_percent']:.1f}%")
    print(f"Baseline P99:        {result['baseline_p99_ms']:.2f} ms")
    print(f"Final P99:           {result['final_p99_ms']:.2f} ms")

    if result.get('checkpoint_count', 0) > 0:
        print(f"Checkpoints Saved:   {result['checkpoint_count']}")
    if result.get('resume_count', 0) > 0:
        print(f"Resume Count:        {result['resume_count']}")

    print("=" * 70)

    # Print failures if any
    if result['failures']:
        print("\nFAILURES:")
        for failure in result['failures']:
            print(f"  ✗ {failure}")
        print()

    # Print alert summary if available
    if result.get('alert_summary'):
        alert_summary = result['alert_summary']
        print("\nALERT SUMMARY:")
        print(f"  Total Alerts:    {alert_summary['total_alerts']}")
        print(f"  Critical:        {alert_summary['by_severity'].get('critical', 0)}")
        print(f"  Warning:         {alert_summary['by_severity'].get('warning', 0)}")
        print(f"  Info:            {alert_summary['by_severity'].get('info', 0)}")
        print()

    # Generate HTML report
    print(f"Generating HTML report: {args.report}")
    try:
        report = StabilityReport(
            config=ReportConfig(
                title=f"EMBODIOS Stability Test - {format_duration(args.duration)}",
                format='html',
                include_charts=True,
            )
        )

        # Prepare test summary for report
        test_summary = {
            'passed': result['passed'],
            'duration_seconds': result['duration'],
            'start_time': start_time,
            'end_time': time.time(),
            'requests_processed': result['requests_processed'],
            'memory_drift_pct': result['memory_drift_pct'],
            'latency_degradation_pct': result['latency_degradation_pct'],
            'peak_memory_mb': result['peak_memory_mb'],
            'avg_cpu_percent': result['avg_cpu_percent'],
            'failures': result['failures'],
        }

        # Add drift analysis results if we have metrics
        if hasattr(runner, 'storage'):
            analyzer = DriftAnalyzer()

            # Analyze memory drift
            memory_metrics = runner.storage.get_metrics('memory_rss_mb')
            if len(memory_metrics) >= 2:
                baseline_memory = memory_metrics[0].value
                memory_drift_result = analyzer.detect_memory_drift(
                    memory_metrics,
                    threshold_pct=args.memory_threshold,
                    baseline_value=baseline_memory
                )
                report.add_drift_result('Memory RSS (MB)', memory_drift_result)

            # Analyze latency degradation
            latency_metrics = runner.storage.get_metrics('latency_p99_ms')
            if len(latency_metrics) >= 2:
                baseline_latency = latency_metrics[0].value
                latency_drift_result = analyzer.detect_latency_degradation(
                    latency_metrics,
                    threshold_pct=args.latency_threshold,
                    baseline_value=baseline_latency
                )
                report.add_drift_result('Latency P99 (ms)', latency_drift_result)

            # Generate HTML report
            report_path = report.generate_html(
                storage=runner.storage,
                output_path=args.report,
                test_summary=test_summary
            )

            print(f"Report saved: {report_path}")
        else:
            print("Warning: No metrics storage available - skipping detailed report")
            # Create minimal report
            report_path = Path(args.report)
            report_path.parent.mkdir(parents=True, exist_ok=True)
            minimal_html = f"""<!DOCTYPE html>
<html>
<head><title>Stability Test Report</title></head>
<body>
<h1>Stability Test Report</h1>
<p>Status: {'PASS' if result['passed'] else 'FAIL'}</p>
<p>Duration: {format_duration(result['duration'])}</p>
<p>Requests: {result['requests_processed']}</p>
</body>
</html>"""
            report_path.write_text(minimal_html)
            print(f"Minimal report saved: {report_path}")

    except Exception as e:
        print(f"Warning: Failed to generate report: {e}")
        import traceback
        traceback.print_exc()

    print("\n" + "=" * 70)
    print(f"Test completed: {'PASS ✓' if result['passed'] else 'FAIL ✗'}")
    print("=" * 70)

    # Exit with appropriate code
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
