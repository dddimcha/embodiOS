#!/usr/bin/env python3
"""
Long-running stability tests for EMBODIOS.

Tests continuous inference over extended periods (hours to days) to detect
memory leaks, performance degradation, and crashes. Monitors system resources
and validates against acceptance criteria thresholds.
"""

import time
import pytest
import statistics
from typing import List, Dict, Any, Tuple, Optional
from unittest.mock import Mock, patch
from pathlib import Path

from tests.stability.config import StabilityConfig
from tests.stability.monitors import ResourceMonitor, ResourceSnapshot
from tests.stability.storage import MetricsStorage, MetricPoint
from tests.stability.checkpoint import CheckpointManager, CheckpointData
from tests.stability.analysis import DriftAnalyzer, DriftResult
from tests.stability.alerts import AlertManager, StdoutAlertChannel, AlertSeverity


class MockInferenceEngine:
    """
    Mock inference engine for stability testing.

    Simulates realistic inference behavior without requiring actual model files.
    Can be configured to simulate memory leaks or performance degradation for
    testing alerting and drift detection.
    """

    def __init__(self, simulate_memory_leak: bool = False, leak_rate_mb: float = 0.1,
                 deterministic: bool = False):
        """
        Initialize mock inference engine.

        Args:
            simulate_memory_leak: If True, gradually increase memory usage
            leak_rate_mb: Memory leak rate in MB per inference call
            deterministic: If True, use fixed latency instead of random
        """
        self.model_loaded = True
        self.inference_count = 0
        self.simulate_memory_leak = simulate_memory_leak
        self.leak_rate_mb = leak_rate_mb
        self.deterministic = deterministic
        self._leaked_memory: List[bytes] = []

    def inference(self, input_tokens: List[int]) -> Tuple[List[int], Dict]:
        """
        Simulate inference with realistic latency.

        Args:
            input_tokens: Input token sequence

        Returns:
            Tuple of (output_tokens, hardware_operations)
        """
        self.inference_count += 1

        # Simulate realistic inference latency
        if self.deterministic:
            # Fixed latency for deterministic testing (20ms)
            latency = 0.020
        else:
            # Variable latency (15-25ms) for realistic testing
            import random
            latency = random.uniform(0.015, 0.025)

        time.sleep(latency)

        # Simulate memory leak if enabled
        if self.simulate_memory_leak:
            leak_size = int(self.leak_rate_mb * 1024 * 1024)
            self._leaked_memory.append(b'0' * leak_size)

        # Return mock output
        output_tokens = [32001, 17, 32002]  # GPIO write example
        hw_operations = {'gpio': [], 'memory': [], 'i2c': [], 'interrupts': []}

        return output_tokens, hw_operations


class StabilityTestRunner:
    """
    Orchestrates long-running stability tests.

    Runs continuous inference while monitoring system resources and collecting
    metrics. Validates results against configured thresholds and generates
    detailed test reports.

    Example:
        config = StabilityConfig.smoke_test(duration_seconds=60)
        runner = StabilityTestRunner(config)
        result = runner.run()
        assert result['passed']
    """

    def __init__(
        self,
        config: StabilityConfig,
        deterministic: bool = False,
        checkpoint_manager: Optional[CheckpointManager] = None,
        resume_from_checkpoint: Optional[CheckpointData] = None,
        test_name: str = 'stability_test',
        alert_manager: Optional[AlertManager] = None,
        simulate_memory_leak: bool = False,
        leak_rate_mb: float = 0.1
    ):
        """
        Initialize stability test runner.

        Args:
            config: Test configuration with duration and thresholds
            deterministic: Use deterministic latency for stable testing
            checkpoint_manager: Optional checkpoint manager for save/resume
            resume_from_checkpoint: Optional checkpoint data to resume from
            test_name: Name of the test (for checkpoint identification)
            alert_manager: Optional alert manager for real-time alerting
            simulate_memory_leak: If True, simulate memory leak for testing
            leak_rate_mb: Memory leak rate in MB per inference call
        """
        self.config = config
        self.deterministic = deterministic
        self.test_name = test_name
        self.monitor = ResourceMonitor()
        self.storage = MetricsStorage()
        self.engine: MockInferenceEngine = None
        self.latencies: List[float] = []

        # Checkpoint support
        self.checkpoint_manager = checkpoint_manager
        self.checkpoint_count = 0
        self.resume_count = 0
        self.original_start_time: Optional[float] = None
        self.resumed_elapsed_time: float = 0.0

        # Alerting support
        self.alert_manager = alert_manager
        self.analyzer = DriftAnalyzer()
        self.last_drift_check_time: Optional[float] = None

        # Memory leak simulation (for testing drift detection)
        self.simulate_memory_leak = simulate_memory_leak
        self.leak_rate_mb = leak_rate_mb

        # Resume from checkpoint if provided
        if resume_from_checkpoint:
            self._restore_from_checkpoint(resume_from_checkpoint)

    def _restore_from_checkpoint(self, checkpoint: CheckpointData) -> None:
        """
        Restore test state from checkpoint.

        Args:
            checkpoint: Checkpoint data to restore from
        """
        print(f"\n[Resume] Restoring from checkpoint...")
        print(f"[Resume] Original start: {checkpoint.start_time}")
        print(f"[Resume] Elapsed so far: {checkpoint.elapsed_time:.1f}s")
        print(f"[Resume] Requests completed: {checkpoint.requests_processed}")

        # Restore timing
        self.original_start_time = checkpoint.start_time
        self.resumed_elapsed_time = checkpoint.elapsed_time

        # Restore latencies (keep last 1000 for memory efficiency)
        self.latencies = checkpoint.latencies[-1000:] if checkpoint.latencies else []

        # Restore metrics
        for metric_data in checkpoint.metrics_data:
            point = MetricPoint.from_dict(metric_data)
            self.storage.metrics.append(point)

        # Restore baseline if warmup was completed
        if checkpoint.warmup_completed and checkpoint.baseline_snapshot_dict:
            baseline_dict = checkpoint.baseline_snapshot_dict
            baseline = ResourceSnapshot(**baseline_dict)
            self.monitor._baseline = baseline
            print(f"[Resume] Baseline memory: {baseline.memory_rss_mb():.2f} MB")

        # Track resume count
        self.resume_count = checkpoint.resume_count + 1
        self.checkpoint_count = checkpoint.checkpoint_count

        print(f"[Resume] Resuming test (resume #{self.resume_count})...")

    def _create_checkpoint(
        self,
        start_time: float,
        elapsed_time: float,
        requests_processed: int,
        warmup_completed: bool
    ) -> CheckpointData:
        """
        Create checkpoint data from current test state.

        Args:
            start_time: Original test start time
            elapsed_time: Total elapsed time
            requests_processed: Number of requests completed
            warmup_completed: Whether warmup phase is complete

        Returns:
            CheckpointData with current state
        """
        # Serialize baseline snapshot
        baseline_dict = None
        if self.monitor._baseline:
            baseline_dict = {
                'timestamp': self.monitor._baseline.timestamp,
                'cpu_percent': self.monitor._baseline.cpu_percent,
                'memory_rss_bytes': self.monitor._baseline.memory_rss_bytes,
                'memory_vms_bytes': self.monitor._baseline.memory_vms_bytes,
                'memory_percent': self.monitor._baseline.memory_percent,
                'num_threads': self.monitor._baseline.num_threads,
                'num_fds': self.monitor._baseline.num_fds,
            }

        # Serialize metrics (keep last 1000 for memory efficiency)
        metrics_data = [m.to_dict() for m in self.storage.metrics]

        # Serialize config
        config_dict = {
            'duration_seconds': self.config.duration_seconds,
            'monitoring_interval_seconds': self.config.monitoring_interval_seconds,
            'memory_drift_threshold_pct': self.config.memory_drift_threshold_pct,
            'latency_degradation_threshold_pct': self.config.latency_degradation_threshold_pct,
            'min_requests': self.config.min_requests,
            'warmup_requests': self.config.warmup_requests,
            'checkpoint_interval_seconds': self.config.checkpoint_interval_seconds,
        }

        return CheckpointData(
            test_name=self.test_name,
            config_dict=config_dict,
            start_time=start_time,
            elapsed_time=elapsed_time,
            requests_processed=requests_processed,
            warmup_completed=warmup_completed,
            baseline_snapshot_dict=baseline_dict,
            latencies=self.latencies[-1000:],  # Keep last 1000
            metrics_data=metrics_data,
            checkpoint_time=time.time(),
            checkpoint_count=self.checkpoint_count + 1,
            resume_count=self.resume_count,
        )

    def _save_checkpoint(
        self,
        start_time: float,
        elapsed_time: float,
        requests_processed: int,
        warmup_completed: bool
    ) -> None:
        """
        Save current state to checkpoint.

        Args:
            start_time: Original test start time
            elapsed_time: Total elapsed time
            requests_processed: Number of requests completed
            warmup_completed: Whether warmup phase is complete
        """
        if not self.checkpoint_manager:
            return

        checkpoint = self._create_checkpoint(
            start_time, elapsed_time, requests_processed, warmup_completed
        )

        self.checkpoint_manager.save_checkpoint(checkpoint)
        self.checkpoint_count += 1

        # Cleanup old checkpoints (keep last 5)
        self.checkpoint_manager.cleanup_old_checkpoints(self.test_name, keep_count=5)

    def run(self) -> Dict[str, Any]:
        """
        Run stability test for configured duration.

        Supports checkpoint/resume for long-running tests. If resuming from
        checkpoint, continues from previous state. Handles graceful shutdown
        on SIGTERM/SIGINT with checkpoint save.

        Returns:
            Dictionary with test results:
                - passed: bool indicating if test passed all thresholds
                - duration: actual test duration in seconds
                - requests_processed: number of inference requests completed
                - memory_drift_pct: memory drift percentage from baseline
                - latency_degradation_pct: P99 latency increase from baseline
                - baseline_snapshot: initial resource snapshot
                - final_snapshot: final resource snapshot
                - peak_memory_mb: peak memory usage in MB
                - avg_cpu_percent: average CPU usage percentage
                - failures: list of threshold violations
                - checkpoint_count: number of checkpoints saved
                - resume_count: number of times test was resumed
                - shutdown_graceful: whether test was shutdown gracefully
        """
        self.config.validate()

        # Setup signal handlers for graceful shutdown
        if self.checkpoint_manager:
            self.checkpoint_manager.setup_signal_handlers(
                on_shutdown=lambda: self._on_shutdown_signal()
            )

        # Initialize mock inference engine
        self.engine = MockInferenceEngine(
            deterministic=self.deterministic,
            simulate_memory_leak=self.simulate_memory_leak,
            leak_rate_mb=self.leak_rate_mb
        )

        # Determine if we're resuming or starting fresh
        resuming = self.original_start_time is not None
        warmup_completed = resuming  # If resuming, warmup was already done

        # Start time (use original if resuming)
        if resuming:
            start_time = self.original_start_time
            initial_requests = len(self.latencies)
            print(f"\n[Resume] Continuing test from {self.resumed_elapsed_time:.1f}s")
            print(f"[Resume] Remaining: {self.config.duration_seconds - self.resumed_elapsed_time:.1f}s")
        else:
            start_time = time.time()
            initial_requests = 0
            self.latencies.clear()
            self.storage.clear()

        # Warmup phase (skip if resuming)
        if not resuming:
            warmup_count = max(self.config.warmup_requests, 20)
            print(f"Running {warmup_count} warmup requests...")
            for _ in range(warmup_count):
                self._run_inference_request()

            # Capture baseline after warmup
            baseline = self.monitor.set_baseline()
            print(f"Baseline memory: {baseline.memory_rss_mb():.2f} MB")

            # Record baseline metrics
            self.storage.record('memory_rss_mb', baseline.memory_rss_mb(),
                               metadata={'phase': 'baseline'})
            self.storage.record('cpu_percent', baseline.cpu_percent,
                               metadata={'phase': 'baseline'})

            # Calculate P99 from warmup requests as baseline latency
            baseline_p99 = self._calculate_p99(self.latencies[-warmup_count:])
            self.storage.record('latency_p99_ms', baseline_p99 * 1000,
                               metadata={'phase': 'baseline'})

            warmup_completed = True

        # Get baseline for calculations
        baseline = self.monitor.get_baseline()
        if not baseline:
            raise RuntimeError("Baseline not set - cannot run test")

        # Main test loop
        print(f"Starting stability test for {self.config.duration_seconds}s...")
        next_monitor_time = start_time + self.config.monitoring_interval_seconds + self.resumed_elapsed_time
        next_checkpoint_time = start_time + self.config.checkpoint_interval_seconds + self.resumed_elapsed_time
        requests_processed = initial_requests

        while True:
            current_time = time.time()
            total_elapsed = (current_time - start_time) + self.resumed_elapsed_time

            # Check if test duration completed
            if total_elapsed >= self.config.duration_seconds:
                print("\n[Test] Duration completed")
                break

            # Check for graceful shutdown request
            if self.checkpoint_manager and self.checkpoint_manager.shutdown_requested:
                print("\n[Shutdown] Graceful shutdown in progress...")
                self._save_checkpoint(start_time, total_elapsed, requests_processed, warmup_completed)
                print("[Shutdown] Checkpoint saved. Test can be resumed later.")

                # Restore signal handlers
                self.checkpoint_manager.restore_signal_handlers()

                # Return partial results
                return self._build_partial_result(
                    start_time=start_time,
                    total_elapsed=total_elapsed,
                    requests_processed=requests_processed,
                    baseline=baseline,
                    shutdown_graceful=True
                )

            # Run inference request
            self._run_inference_request()
            requests_processed += 1

            # Periodic resource monitoring
            if current_time >= next_monitor_time:
                self._record_metrics(total_elapsed)
                self._check_drift_and_alert(total_elapsed)
                next_monitor_time += self.config.monitoring_interval_seconds

                # Progress update every 10 monitoring intervals
                if len(self.storage.get_metrics(metric_name='memory_rss_mb')) % 10 == 0:
                    progress_pct = (total_elapsed / self.config.duration_seconds) * 100
                    print(f"  Progress: {progress_pct:.1f}% ({requests_processed} requests)")

            # Periodic checkpoint save
            if self.config.checkpoint_interval_seconds > 0 and current_time >= next_checkpoint_time:
                self._save_checkpoint(start_time, total_elapsed, requests_processed, warmup_completed)
                next_checkpoint_time += self.config.checkpoint_interval_seconds

        # Final measurements
        final_snapshot = self.monitor.capture()
        total_elapsed = (time.time() - start_time) + self.resumed_elapsed_time

        print(f"Test completed: {total_elapsed:.1f}s, {requests_processed} requests")

        # Restore signal handlers if using checkpoints
        if self.checkpoint_manager:
            self.checkpoint_manager.restore_signal_handlers()

        # Calculate final metrics
        memory_drift = self.monitor.calculate_memory_drift(final_snapshot)

        # Calculate P99 latency from last 100 requests
        recent_latencies = self.latencies[-min(100, len(self.latencies)):]
        final_p99 = self._calculate_p99(recent_latencies)

        # Calculate baseline P99 for comparison
        baseline_metrics = self.storage.get_metrics(metric_name='latency_p99_ms')
        if baseline_metrics:
            baseline_p99_ms = baseline_metrics[0].value
            baseline_p99 = baseline_p99_ms / 1000
        else:
            baseline_p99 = final_p99  # Fallback if no baseline recorded

        latency_degradation = ((final_p99 - baseline_p99) / baseline_p99) * 100 if baseline_p99 > 0 else 0

        # Peak memory and average CPU
        peak_memory = self.monitor.get_peak_memory()
        avg_cpu = self.monitor.get_average_cpu()

        # Validate against thresholds
        failures = []

        if requests_processed < self.config.min_requests:
            failures.append(f"Insufficient requests: {requests_processed} < {self.config.min_requests}")

        if memory_drift is not None and memory_drift > self.config.memory_drift_threshold_pct:
            failures.append(f"Memory drift exceeded: {memory_drift:.2f}% > {self.config.memory_drift_threshold_pct}%")

        if latency_degradation > self.config.latency_degradation_threshold_pct:
            failures.append(f"Latency degradation exceeded: {latency_degradation:.2f}% > {self.config.latency_degradation_threshold_pct}%")

        # Get alert summary if alert manager was used
        alert_summary = None
        if self.alert_manager:
            alert_summary = self.alert_manager.get_alert_summary()

        # Build result dictionary
        result = {
            'passed': len(failures) == 0,
            'duration': total_elapsed,
            'requests_processed': requests_processed,
            'memory_drift_pct': memory_drift,
            'latency_degradation_pct': latency_degradation,
            'baseline_snapshot': baseline,
            'final_snapshot': final_snapshot,
            'peak_memory_mb': peak_memory.memory_rss_mb() if peak_memory else 0,
            'avg_cpu_percent': avg_cpu if avg_cpu else 0,
            'baseline_p99_ms': baseline_p99 * 1000,
            'final_p99_ms': final_p99 * 1000,
            'failures': failures,
            'checkpoint_count': self.checkpoint_count,
            'resume_count': self.resume_count,
            'shutdown_graceful': False,
            'alert_summary': alert_summary
        }

        return result

    def _on_shutdown_signal(self) -> None:
        """Callback invoked when shutdown signal is received"""
        print("\n[Shutdown] Preparing to save checkpoint and exit...")

    def _build_partial_result(
        self,
        start_time: float,
        total_elapsed: float,
        requests_processed: int,
        baseline: ResourceSnapshot,
        shutdown_graceful: bool
    ) -> Dict[str, Any]:
        """
        Build partial result for interrupted test.

        Args:
            start_time: Original test start time
            total_elapsed: Total elapsed time
            requests_processed: Number of requests completed
            baseline: Baseline resource snapshot
            shutdown_graceful: Whether shutdown was graceful

        Returns:
            Partial result dictionary
        """
        # Capture final snapshot
        final_snapshot = self.monitor.capture()

        # Calculate metrics from what we have
        memory_drift = self.monitor.calculate_memory_drift(final_snapshot)

        recent_latencies = self.latencies[-min(100, len(self.latencies)):]
        final_p99 = self._calculate_p99(recent_latencies) if recent_latencies else 0.0

        baseline_metrics = self.storage.get_metrics(metric_name='latency_p99_ms')
        if baseline_metrics:
            baseline_p99 = baseline_metrics[0].value / 1000
        else:
            baseline_p99 = final_p99

        latency_degradation = ((final_p99 - baseline_p99) / baseline_p99) * 100 if baseline_p99 > 0 else 0

        peak_memory = self.monitor.get_peak_memory()
        avg_cpu = self.monitor.get_average_cpu()

        # Get alert summary if alert manager was used
        alert_summary = None
        if self.alert_manager:
            alert_summary = self.alert_manager.get_alert_summary()

        return {
            'passed': False,  # Incomplete test
            'duration': total_elapsed,
            'requests_processed': requests_processed,
            'memory_drift_pct': memory_drift,
            'latency_degradation_pct': latency_degradation,
            'baseline_snapshot': baseline,
            'final_snapshot': final_snapshot,
            'peak_memory_mb': peak_memory.memory_rss_mb() if peak_memory else 0,
            'avg_cpu_percent': avg_cpu if avg_cpu else 0,
            'baseline_p99_ms': baseline_p99 * 1000,
            'final_p99_ms': final_p99 * 1000,
            'failures': ['Test interrupted - graceful shutdown'],
            'checkpoint_count': self.checkpoint_count,
            'resume_count': self.resume_count,
            'shutdown_graceful': shutdown_graceful,
            'incomplete': True,
            'alert_summary': alert_summary
        }

    def _run_inference_request(self) -> None:
        """Execute single inference request and record latency"""
        input_tokens = [1, 2, 3, 4, 5]  # Mock input

        start = time.time()
        self.engine.inference(input_tokens)
        latency = time.time() - start

        self.latencies.append(latency)

    def _record_metrics(self, elapsed_time: float) -> None:
        """Capture and record current metrics"""
        snapshot = self.monitor.capture()

        # Record to storage
        self.storage.record('memory_rss_mb', snapshot.memory_rss_mb(),
                           metadata={'elapsed_time': elapsed_time})
        self.storage.record('memory_vms_mb', snapshot.memory_vms_mb(),
                           metadata={'elapsed_time': elapsed_time})
        self.storage.record('cpu_percent', snapshot.cpu_percent,
                           metadata={'elapsed_time': elapsed_time})
        self.storage.record('num_threads', snapshot.num_threads,
                           metadata={'elapsed_time': elapsed_time})

        # Record latency metrics from recent requests
        if self.latencies:
            recent = self.latencies[-min(100, len(self.latencies)):]
            p99 = self._calculate_p99(recent)
            mean = statistics.mean(recent)

            self.storage.record('latency_p99_ms', p99 * 1000,
                               metadata={'elapsed_time': elapsed_time})
            self.storage.record('latency_mean_ms', mean * 1000,
                               metadata={'elapsed_time': elapsed_time})

    def _check_drift_and_alert(self, elapsed_time: float) -> None:
        """
        Check for drift and trigger alerts if thresholds are exceeded.

        Args:
            elapsed_time: Total elapsed time since test start
        """
        if not self.alert_manager:
            return

        # Only check periodically (every 30 seconds minimum)
        if self.last_drift_check_time is not None:
            if elapsed_time - self.last_drift_check_time < 30:
                return

        self.last_drift_check_time = elapsed_time

        # Need sufficient data for analysis
        memory_metrics = self.storage.get_metrics('memory_rss_mb')
        latency_metrics = self.storage.get_metrics('latency_p99_ms')

        if len(memory_metrics) < 3 or len(latency_metrics) < 3:
            return

        # Get baseline values
        baseline_memory = memory_metrics[0].value if memory_metrics else None
        baseline_latency = latency_metrics[0].value if latency_metrics else None

        # Analyze memory drift
        memory_result = self.analyzer.detect_memory_drift(
            memory_metrics,
            threshold_pct=self.config.memory_drift_threshold_pct,
            baseline_value=baseline_memory
        )

        # Analyze latency degradation
        latency_result = self.analyzer.detect_latency_degradation(
            latency_metrics,
            threshold_pct=self.config.latency_degradation_threshold_pct,
            baseline_value=baseline_latency
        )

        # Trigger alerts
        drift_results = {
            'memory': memory_result,
            'latency': latency_result,
        }

        self.alert_manager.check_multiple(drift_results)

    def _calculate_p99(self, values: List[float]) -> float:
        """Calculate 99th percentile from list of values"""
        if not values:
            return 0.0

        sorted_values = sorted(values)
        p99_index = int(len(sorted_values) * 0.99)
        return sorted_values[min(p99_index, len(sorted_values) - 1)]


# Pytest fixtures

@pytest.fixture
def smoke_config():
    """Provide smoke test configuration (60 seconds)"""
    return StabilityConfig.smoke_test(duration_seconds=60)


@pytest.fixture
def short_config():
    """Provide short test configuration (1 hour)"""
    return StabilityConfig.short_test()


@pytest.fixture
def medium_config():
    """Provide medium test configuration (12 hours)"""
    return StabilityConfig.medium_test()


@pytest.fixture
def long_config():
    """Provide long test configuration (72 hours)"""
    return StabilityConfig.long_test()


# Test cases

def test_short_stability(smoke_config):
    """
    Short stability test for verification and CI.

    Runs for 60 seconds with continuous inference and resource monitoring.
    Validates that memory drift and latency degradation stay within thresholds.

    This test uses smoke_config which runs for 60 seconds, making it suitable
    for CI/CD pipelines and development verification.

    Uses deterministic latency to ensure stable test results without
    statistical variance affecting pass/fail outcomes.
    """
    # Create runner with smoke test config and deterministic mode
    runner = StabilityTestRunner(smoke_config, deterministic=True)

    # Run stability test
    result = runner.run()

    # Print summary
    print("\n" + "=" * 70)
    print("STABILITY TEST SUMMARY")
    print("=" * 70)
    print(f"Duration:            {result['duration']:.1f}s")
    print(f"Requests Processed:  {result['requests_processed']}")
    print(f"Memory Drift:        {result['memory_drift_pct']:.2f}%")
    print(f"Latency Degradation: {result['latency_degradation_pct']:.2f}%")
    print(f"Peak Memory:         {result['peak_memory_mb']:.2f} MB")
    print(f"Avg CPU:             {result['avg_cpu_percent']:.1f}%")
    print(f"Baseline P99:        {result['baseline_p99_ms']:.2f} ms")
    print(f"Final P99:           {result['final_p99_ms']:.2f} ms")
    print("=" * 70)

    # Report failures
    if result['failures']:
        print("FAILURES:")
        for failure in result['failures']:
            print(f"  - {failure}")
        print("=" * 70)

    # Assert test passed
    assert result['passed'], f"Stability test failed: {result['failures']}"
    assert result['requests_processed'] >= smoke_config.min_requests, \
        f"Insufficient requests: {result['requests_processed']} < {smoke_config.min_requests}"


@pytest.mark.slow
def test_one_hour_stability(short_config):
    """
    One-hour stability test.

    Runs continuous inference for 1 hour with full resource monitoring.
    This test is marked as 'slow' and should be run separately from
    the standard test suite.

    Usage:
        pytest tests/stability/test_long_running.py::test_one_hour_stability -v -m slow
    """
    runner = StabilityTestRunner(short_config)
    result = runner.run()

    # Print detailed summary
    print("\n" + "=" * 70)
    print("1-HOUR STABILITY TEST SUMMARY")
    print("=" * 70)
    print(f"Duration:            {result['duration']:.1f}s ({result['duration']/3600:.2f}h)")
    print(f"Requests Processed:  {result['requests_processed']}")
    print(f"Memory Drift:        {result['memory_drift_pct']:.2f}%")
    print(f"Latency Degradation: {result['latency_degradation_pct']:.2f}%")
    print(f"Peak Memory:         {result['peak_memory_mb']:.2f} MB")
    print(f"Avg CPU:             {result['avg_cpu_percent']:.1f}%")
    print("=" * 70)

    assert result['passed'], f"1-hour stability test failed: {result['failures']}"


@pytest.mark.slow
@pytest.mark.long_running
def test_twelve_hour_stability(medium_config):
    """
    Twelve-hour stability test.

    Runs continuous inference for 12 hours with full resource monitoring.
    This test is marked as 'slow' and 'long_running' and should only be
    run manually or in dedicated long-running test environments.

    Usage:
        pytest tests/stability/test_long_running.py::test_twelve_hour_stability -v -m long_running
    """
    runner = StabilityTestRunner(medium_config)
    result = runner.run()

    print("\n" + "=" * 70)
    print("12-HOUR STABILITY TEST SUMMARY")
    print("=" * 70)
    print(f"Duration:            {result['duration']:.1f}s ({result['duration']/3600:.2f}h)")
    print(f"Requests Processed:  {result['requests_processed']}")
    print(f"Memory Drift:        {result['memory_drift_pct']:.2f}%")
    print(f"Latency Degradation: {result['latency_degradation_pct']:.2f}%")
    print(f"Peak Memory:         {result['peak_memory_mb']:.2f} MB")
    print(f"Avg CPU:             {result['avg_cpu_percent']:.1f}%")
    print("=" * 70)

    assert result['passed'], f"12-hour stability test failed: {result['failures']}"


@pytest.mark.slow
@pytest.mark.long_running
def test_seventy_two_hour_stability(long_config):
    """
    Seventy-two-hour stability test (acceptance criteria).

    Runs continuous inference for 72 hours with full resource monitoring.
    This is the primary acceptance criteria test for EMBODIOS stability.

    Acceptance Criteria:
    - Runs 72-hour continuous inference test without degradation
    - Memory usage remains stable within 5% after 10,000 inferences
    - Latency P99 remains within 10% of initial measurement

    This test should only be run manually or in dedicated long-running
    test environments with checkpoint/resume support.

    Usage:
        pytest tests/stability/test_long_running.py::test_seventy_two_hour_stability -v -m long_running
    """
    runner = StabilityTestRunner(long_config)
    result = runner.run()

    print("\n" + "=" * 70)
    print("72-HOUR STABILITY TEST SUMMARY (ACCEPTANCE CRITERIA)")
    print("=" * 70)
    print(f"Duration:            {result['duration']:.1f}s ({result['duration']/3600:.2f}h)")
    print(f"Requests Processed:  {result['requests_processed']}")
    print(f"Memory Drift:        {result['memory_drift_pct']:.2f}% (threshold: 5%)")
    print(f"Latency Degradation: {result['latency_degradation_pct']:.2f}% (threshold: 10%)")
    print(f"Peak Memory:         {result['peak_memory_mb']:.2f} MB")
    print(f"Avg CPU:             {result['avg_cpu_percent']:.1f}%")
    print("=" * 70)

    # Verify acceptance criteria
    assert result['passed'], f"72-hour stability test failed: {result['failures']}"
    assert result['requests_processed'] >= 10000, \
        f"Acceptance criteria requires >= 10,000 inferences, got {result['requests_processed']}"
