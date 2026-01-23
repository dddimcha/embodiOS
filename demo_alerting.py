#!/usr/bin/env python3
"""
Demo script showing real-time alerting during stability tests.

This demonstrates how to use the alerting system to get notified when
stability thresholds are exceeded during long-running tests.

Usage:
    # With memory leak (to trigger alerts)
    PYTHONPATH=. python3 demo_alerting.py --leak

    # Without memory leak (stable test)
    PYTHONPATH=. python3 demo_alerting.py
"""

import argparse
from pathlib import Path

from tests.stability.config import StabilityConfig
from tests.stability.test_long_running import StabilityTestRunner
from tests.stability.alerts import (
    AlertManager,
    StdoutAlertChannel,
    FileAlertChannel,
    AlertSeverity
)


def main():
    parser = argparse.ArgumentParser(description='Demo stability test with alerting')
    parser.add_argument('--leak', action='store_true',
                      help='Simulate memory leak to trigger alerts')
    parser.add_argument('--duration', type=int, default=30,
                      help='Test duration in seconds (default: 30)')
    parser.add_argument('--alert-log', type=str, default='/tmp/stability_alerts.log',
                      help='Path to alert log file')
    args = parser.parse_args()

    print("\n" + "=" * 80)
    print("STABILITY TEST WITH REAL-TIME ALERTING")
    print("=" * 80)

    # Create configuration
    config = StabilityConfig.smoke_test(duration_seconds=args.duration)
    config.monitoring_interval_seconds = 5
    config.memory_drift_threshold_pct = 5.0
    config.latency_degradation_threshold_pct = 10.0

    # Setup alert manager
    alert_manager = AlertManager(
        test_name='demo_test',
        dedupe_window_seconds=15
    )

    # Add stdout channel
    stdout_channel = StdoutAlertChannel(min_severity=AlertSeverity.WARNING)
    alert_manager.add_channel(stdout_channel)

    # Add file channel
    log_path = Path(args.alert_log)
    file_channel = FileAlertChannel(log_path)
    alert_manager.add_channel(file_channel)

    print(f"\n✓ Configuration:")
    print(f"  - Duration: {args.duration}s")
    print(f"  - Memory drift threshold: {config.memory_drift_threshold_pct}%")
    print(f"  - Latency degradation threshold: {config.latency_degradation_threshold_pct}%")
    print(f"  - Memory leak simulation: {'ENABLED' if args.leak else 'DISABLED'}")
    print(f"  - Alert log: {log_path}")

    # Create test runner
    runner = StabilityTestRunner(
        config=config,
        deterministic=True,
        test_name='demo_test',
        alert_manager=alert_manager,
        simulate_memory_leak=args.leak,
        leak_rate_mb=0.5 if args.leak else 0.0
    )

    print(f"\n{'=' * 80}")
    print("STARTING TEST")
    print("=" * 80)
    if args.leak:
        print("⚠️  Memory leak enabled - watch for alerts!")
    print()

    # Run test
    result = runner.run()

    # Display results
    print(f"\n{'=' * 80}")
    print("RESULTS")
    print("=" * 80)
    print(f"Memory Drift: {result['memory_drift_pct']:.2f}%")
    print(f"Latency Degradation: {result['latency_degradation_pct']:.2f}%")
    print(f"Test Passed: {'✓ YES' if result['passed'] else '✗ NO'}")

    # Display alert summary
    alert_summary = result.get('alert_summary')
    if alert_summary:
        print(f"\n{'=' * 80}")
        print("ALERT SUMMARY")
        print("=" * 80)
        print(f"Total Alerts: {alert_summary['total_alerts']}")
        print(f"Critical: {alert_summary['critical_count']}")
        print(f"Warning: {alert_summary['warning_count']}")
        print(f"Info: {alert_summary['info_count']}")

        if alert_summary.get('alerts'):
            print(f"\nAlert Details:")
            for i, alert_data in enumerate(alert_summary['alerts'], 1):
                print(f"\n  #{i}: {alert_data['severity'].upper()}")
                print(f"      Metric: {alert_data['metric_name']}")
                print(f"      Drift: {alert_data['drift_percentage']:.1f}%")
                print(f"      Message: {alert_data['message']}")

        if log_path.exists():
            log_lines = log_path.read_text().strip().split('\n')
            print(f"\n✓ Alert log saved: {log_path} ({len(log_lines)} entries)")

    print(f"\n{'=' * 80}")
    print("DEMO COMPLETE")
    print("=" * 80)

    if args.leak:
        print("\nThe memory leak was detected and alerts were triggered!")
        print("In production, these alerts could be sent to Slack, PagerDuty, etc.")
    else:
        print("\nNo alerts were triggered - metrics remained stable.")
        print("This demonstrates that the system doesn't produce false positives.")


if __name__ == '__main__':
    main()
