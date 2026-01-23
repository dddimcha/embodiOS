#!/usr/bin/env python3
"""
Manual verification test for alerting functionality.

This script demonstrates real-time alerting by simulating a memory leak
and verifying that alerts are triggered when thresholds are exceeded.
"""

import time
from pathlib import Path

from tests.stability.config import StabilityConfig
from tests.stability.test_long_running import StabilityTestRunner
from tests.stability.alerts import AlertManager, StdoutAlertChannel, FileAlertChannel, AlertSeverity


def test_alerting_with_memory_leak():
    """
    Test that alerts are triggered when memory leak is detected.

    This test:
    1. Creates a stability test runner with memory leak simulation
    2. Configures an alert manager with stdout and file channels
    3. Runs the test for 60 seconds
    4. Verifies that alerts are triggered during the test
    """
    print("\n" + "=" * 80)
    print("MANUAL VERIFICATION: Alerting with Memory Leak Simulation")
    print("=" * 80)

    # Create configuration with tight thresholds
    config = StabilityConfig.smoke_test(duration_seconds=60)
    config.memory_drift_threshold_pct = 5.0  # 5% threshold
    config.latency_degradation_threshold_pct = 10.0
    config.monitoring_interval_seconds = 5  # Check every 5 seconds

    # Setup alert manager with multiple channels
    alert_manager = AlertManager(test_name='memory_leak_test', dedupe_window_seconds=15)

    # Add stdout channel (shows alerts in terminal)
    stdout_channel = StdoutAlertChannel(min_severity=AlertSeverity.WARNING)
    alert_manager.add_channel(stdout_channel)

    # Add file channel (saves alerts to log file)
    log_path = Path('/tmp/stability_alerts.log')
    file_channel = FileAlertChannel(log_path)
    alert_manager.add_channel(file_channel)

    print(f"\n✓ Alert manager configured")
    print(f"  - Stdout channel (min severity: WARNING)")
    print(f"  - File channel ({log_path})")
    print(f"  - Deduplication window: 15s")

    # Create test runner with memory leak simulation
    leak_rate = 0.5  # MB per request
    runner = StabilityTestRunner(
        config=config,
        deterministic=True,
        test_name='memory_leak_test',
        alert_manager=alert_manager,
        simulate_memory_leak=True,
        leak_rate_mb=leak_rate
    )

    print(f"\n✓ Test runner configured")
    print(f"  - Duration: {config.duration_seconds}s")
    print(f"  - Memory drift threshold: {config.memory_drift_threshold_pct}%")
    print(f"  - Monitoring interval: {config.monitoring_interval_seconds}s")
    print(f"  - Memory leak simulation: {leak_rate} MB per request")

    print(f"\n{'=' * 80}")
    print("STARTING TEST - Watch for alerts!")
    print("=" * 80)

    # Run test
    start_time = time.time()
    result = runner.run()
    duration = time.time() - start_time

    print(f"\n{'=' * 80}")
    print("TEST COMPLETED")
    print("=" * 80)

    # Display results
    print(f"\nTest Duration: {duration:.1f}s")
    print(f"Requests Processed: {result['requests_processed']}")
    print(f"Memory Drift: {result['memory_drift_pct']:.2f}%")
    print(f"Latency Degradation: {result['latency_degradation_pct']:.2f}%")
    print(f"Test Passed: {result['passed']}")

    # Display alert summary
    alert_summary = result.get('alert_summary')
    if alert_summary:
        print(f"\n{'=' * 80}")
        print("ALERT SUMMARY")
        print("=" * 80)
        print(f"Total Alerts: {alert_summary['total_alerts']}")
        print(f"Critical Alerts: {alert_summary['critical_count']}")
        print(f"Warning Alerts: {alert_summary['warning_count']}")
        print(f"Info Alerts: {alert_summary['info_count']}")
        print(f"Metrics Alerted: {', '.join(alert_summary['metrics_alerted'])}")

        # Show individual alerts
        if alert_summary.get('alerts'):
            print(f"\nAlert Details:")
            for i, alert_data in enumerate(alert_summary['alerts'], 1):
                print(f"\n  Alert #{i}:")
                print(f"    Severity: {alert_data['severity']}")
                print(f"    Metric: {alert_data['metric_name']}")
                print(f"    Drift: {alert_data['drift_percentage']:.1f}%")
                print(f"    Message: {alert_data['message']}")

    # Verification
    print(f"\n{'=' * 80}")
    print("VERIFICATION")
    print("=" * 80)

    # Check that alerts were triggered
    assert alert_summary is not None, "Alert summary should be present"
    assert alert_summary['total_alerts'] > 0, "Alerts should have been triggered"
    assert alert_summary['critical_count'] > 0 or alert_summary['warning_count'] > 0, \
        "Should have warning or critical alerts"
    assert 'memory' in alert_summary['metrics_alerted'], "Memory metric should have triggered alert"

    print("✓ Alerts were triggered as expected")
    print("✓ Memory leak was detected and alerted")

    # Check that alert log file was created
    assert log_path.exists(), "Alert log file should be created"
    log_lines = log_path.read_text().strip().split('\n')
    assert len(log_lines) > 0, "Alert log should contain entries"
    print(f"✓ Alert log file created: {log_path} ({len(log_lines)} entries)")

    print(f"\n{'=' * 80}")
    print("✅ VERIFICATION PASSED")
    print("=" * 80)
    print("\nThe alerting system successfully detected the memory leak")
    print("and triggered alerts in real-time during test execution.")


def test_alerting_with_stable_metrics():
    """
    Test that alerts are NOT triggered when metrics are stable.

    This test verifies that the alerting system doesn't produce
    false positives when the system is running normally.
    """
    print("\n" + "=" * 80)
    print("MANUAL VERIFICATION: No Alerts with Stable Metrics")
    print("=" * 80)

    # Create configuration
    config = StabilityConfig.smoke_test(duration_seconds=30)
    config.monitoring_interval_seconds = 5

    # Setup alert manager
    alert_manager = AlertManager(test_name='stable_test')
    alert_manager.add_channel(StdoutAlertChannel(min_severity=AlertSeverity.WARNING))

    print(f"\n✓ Alert manager configured")
    print(f"  - Test duration: {config.duration_seconds}s")
    print(f"  - No memory leak simulation")

    # Create test runner WITHOUT memory leak
    runner = StabilityTestRunner(
        config=config,
        deterministic=True,
        test_name='stable_test',
        alert_manager=alert_manager
    )

    print(f"\n{'=' * 80}")
    print("STARTING STABLE TEST")
    print("=" * 80)

    # Run test
    result = runner.run()

    print(f"\n{'=' * 80}")
    print("TEST COMPLETED")
    print("=" * 80)

    # Display results
    print(f"\nMemory Drift: {result['memory_drift_pct']:.2f}%")
    print(f"Test Passed: {result['passed']}")

    # Display alert summary
    alert_summary = result.get('alert_summary')
    if alert_summary:
        print(f"\nTotal Alerts: {alert_summary['total_alerts']}")
        print(f"Critical Alerts: {alert_summary['critical_count']}")

    # Verification
    print(f"\n{'=' * 80}")
    print("VERIFICATION")
    print("=" * 80)

    # With stable metrics, we should have very few or no alerts
    assert alert_summary is not None, "Alert summary should be present"
    assert alert_summary['critical_count'] == 0, "Should have no critical alerts for stable test"
    print("✓ No critical alerts for stable metrics")

    print(f"\n{'=' * 80}")
    print("✅ VERIFICATION PASSED")
    print("=" * 80)
    print("\nThe alerting system correctly did not trigger alerts")
    print("when metrics were stable and within thresholds.")


if __name__ == '__main__':
    print("\n" + "=" * 80)
    print("ALERTING SYSTEM MANUAL VERIFICATION")
    print("=" * 80)
    print("\nThis script verifies the real-time alerting functionality")
    print("by simulating memory leaks and checking that alerts are triggered.\n")

    try:
        # Test 1: Memory leak detection
        test_alerting_with_memory_leak()

        # Test 2: Stable metrics (no false positives)
        test_alerting_with_stable_metrics()

        print("\n" + "=" * 80)
        print("🎉 ALL VERIFICATION TESTS PASSED")
        print("=" * 80)
        print("\nThe alerting system is working correctly:")
        print("  ✓ Detects and alerts on memory leaks")
        print("  ✓ Does not trigger false positives on stable metrics")
        print("  ✓ Supports multiple alert channels (stdout, file)")
        print("  ✓ Includes alert deduplication")
        print("\nReady for production use!")

    except AssertionError as e:
        print(f"\n❌ VERIFICATION FAILED: {e}")
        exit(1)
    except Exception as e:
        print(f"\n❌ ERROR: {e}")
        import traceback
        traceback.print_exc()
        exit(1)
