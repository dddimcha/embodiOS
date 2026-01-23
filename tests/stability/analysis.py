#!/usr/bin/env python3
"""
Drift detection and statistical analysis for stability tests.

Provides tools for detecting memory drift, latency degradation, and throughput
decline using statistical methods like moving averages and trend detection.
"""

import statistics
from dataclasses import dataclass
from typing import List, Optional, Dict, Any, Tuple
from enum import Enum

from tests.stability.storage import MetricPoint


class DriftStatus(Enum):
    """Status of drift detection"""
    STABLE = "stable"
    WARNING = "warning"
    CRITICAL = "critical"


@dataclass
class DriftResult:
    """
    Result of drift detection analysis.

    Attributes:
        metric_name: Name of the metric analyzed
        status: Drift status (stable/warning/critical)
        drift_percentage: Percentage drift from baseline
        threshold_percentage: Threshold that was checked against
        baseline_value: Baseline value for comparison
        current_value: Current/final value
        trend_slope: Slope of linear trend (positive = increasing)
        confidence: Confidence score 0-1 based on data quality
        message: Human-readable explanation
    """

    metric_name: str
    status: DriftStatus
    drift_percentage: float
    threshold_percentage: float
    baseline_value: float
    current_value: float
    trend_slope: float
    confidence: float
    message: str


class DriftAnalyzer:
    """
    Statistical analyzer for detecting performance drift in stability tests.

    Analyzes time-series metrics to detect memory leaks, latency degradation,
    and throughput decline using moving averages and trend detection.

    Example:
        analyzer = DriftAnalyzer()
        metrics = storage.get_metrics('memory_rss_mb')
        result = analyzer.detect_memory_drift(metrics, threshold_pct=5.0)
        if result.status == DriftStatus.CRITICAL:
            print(f"Memory leak detected: {result.message}")
    """

    def __init__(self, moving_avg_window: int = 10):
        """
        Initialize drift analyzer.

        Args:
            moving_avg_window: Window size for moving average calculations
        """
        self.moving_avg_window = moving_avg_window

    def detect_memory_drift(
        self,
        metrics: List[MetricPoint],
        threshold_pct: float = 5.0,
        baseline_value: Optional[float] = None
    ) -> DriftResult:
        """
        Detect memory drift using trend analysis.

        Analyzes memory usage over time to detect sustained growth indicating
        memory leaks. Uses linear regression to calculate trend slope.

        Args:
            metrics: List of memory metric points (e.g., 'memory_rss_mb')
            threshold_pct: Drift threshold percentage
            baseline_value: Baseline memory value (default: first metric)

        Returns:
            DriftResult with drift detection outcome
        """
        if not metrics:
            return self._create_insufficient_data_result(
                "memory", threshold_pct, "No metrics provided"
            )

        if len(metrics) < 3:
            return self._create_insufficient_data_result(
                "memory", threshold_pct, f"Insufficient data points ({len(metrics)} < 3)"
            )

        # Extract values
        values = [m.value for m in metrics]

        # Calculate baseline (use provided or first value)
        baseline = baseline_value if baseline_value is not None else values[0]

        # Calculate current value (moving average of last N points)
        window = min(self.moving_avg_window, len(values))
        current = statistics.mean(values[-window:])

        # Calculate drift percentage
        drift_pct = ((current - baseline) / baseline) * 100.0 if baseline > 0 else 0.0

        # Calculate trend slope using linear regression
        trend_slope = self._calculate_trend_slope(values)

        # Determine status
        if abs(drift_pct) > threshold_pct:
            status = DriftStatus.CRITICAL
            message = f"Memory drift {drift_pct:.1f}% exceeds threshold {threshold_pct:.1f}%"
        elif abs(drift_pct) > threshold_pct * 0.7:
            status = DriftStatus.WARNING
            message = f"Memory drift {drift_pct:.1f}% approaching threshold {threshold_pct:.1f}%"
        else:
            status = DriftStatus.STABLE
            message = f"Memory stable: {drift_pct:.1f}% drift (threshold {threshold_pct:.1f}%)"

        # Calculate confidence based on data quality
        confidence = self._calculate_confidence(values)

        return DriftResult(
            metric_name="memory",
            status=status,
            drift_percentage=drift_pct,
            threshold_percentage=threshold_pct,
            baseline_value=baseline,
            current_value=current,
            trend_slope=trend_slope,
            confidence=confidence,
            message=message
        )

    def detect_latency_degradation(
        self,
        metrics: List[MetricPoint],
        threshold_pct: float = 10.0,
        baseline_value: Optional[float] = None
    ) -> DriftResult:
        """
        Detect latency degradation using trend analysis.

        Analyzes latency metrics (e.g., P99) over time to detect performance
        degradation. Uses moving averages to smooth out variance.

        Args:
            metrics: List of latency metric points (e.g., 'latency_p99_ms')
            threshold_pct: Degradation threshold percentage
            baseline_value: Baseline latency value (default: first metric)

        Returns:
            DriftResult with degradation detection outcome
        """
        if not metrics:
            return self._create_insufficient_data_result(
                "latency", threshold_pct, "No metrics provided"
            )

        if len(metrics) < 3:
            return self._create_insufficient_data_result(
                "latency", threshold_pct, f"Insufficient data points ({len(metrics)} < 3)"
            )

        # Extract values
        values = [m.value for m in metrics]

        # Calculate baseline (use provided or first value)
        baseline = baseline_value if baseline_value is not None else values[0]

        # Calculate current value (moving average of last N points)
        window = min(self.moving_avg_window, len(values))
        current = statistics.mean(values[-window:])

        # Calculate degradation percentage
        degradation_pct = ((current - baseline) / baseline) * 100.0 if baseline > 0 else 0.0

        # Calculate trend slope
        trend_slope = self._calculate_trend_slope(values)

        # Determine status (degradation is positive drift)
        if degradation_pct > threshold_pct:
            status = DriftStatus.CRITICAL
            message = f"Latency degradation {degradation_pct:.1f}% exceeds threshold {threshold_pct:.1f}%"
        elif degradation_pct > threshold_pct * 0.7:
            status = DriftStatus.WARNING
            message = f"Latency degradation {degradation_pct:.1f}% approaching threshold {threshold_pct:.1f}%"
        else:
            status = DriftStatus.STABLE
            message = f"Latency stable: {degradation_pct:.1f}% change (threshold {threshold_pct:.1f}%)"

        # Calculate confidence
        confidence = self._calculate_confidence(values)

        return DriftResult(
            metric_name="latency",
            status=status,
            drift_percentage=degradation_pct,
            threshold_percentage=threshold_pct,
            baseline_value=baseline,
            current_value=current,
            trend_slope=trend_slope,
            confidence=confidence,
            message=message
        )

    def detect_throughput_decline(
        self,
        metrics: List[MetricPoint],
        threshold_pct: float = 10.0,
        baseline_value: Optional[float] = None
    ) -> DriftResult:
        """
        Detect throughput decline using trend analysis.

        Analyzes throughput metrics (requests/sec) over time to detect
        performance degradation. Declining throughput indicates system slowdown.

        Args:
            metrics: List of throughput metric points (e.g., 'requests_per_sec')
            threshold_pct: Decline threshold percentage
            baseline_value: Baseline throughput value (default: first metric)

        Returns:
            DriftResult with decline detection outcome
        """
        if not metrics:
            return self._create_insufficient_data_result(
                "throughput", threshold_pct, "No metrics provided"
            )

        if len(metrics) < 3:
            return self._create_insufficient_data_result(
                "throughput", threshold_pct, f"Insufficient data points ({len(metrics)} < 3)"
            )

        # Extract values
        values = [m.value for m in metrics]

        # Calculate baseline (use provided or first value)
        baseline = baseline_value if baseline_value is not None else values[0]

        # Calculate current value (moving average of last N points)
        window = min(self.moving_avg_window, len(values))
        current = statistics.mean(values[-window:])

        # Calculate decline percentage (negative = decline)
        decline_pct = ((current - baseline) / baseline) * 100.0 if baseline > 0 else 0.0

        # Calculate trend slope
        trend_slope = self._calculate_trend_slope(values)

        # Determine status (decline is negative drift)
        if decline_pct < -threshold_pct:
            status = DriftStatus.CRITICAL
            message = f"Throughput decline {abs(decline_pct):.1f}% exceeds threshold {threshold_pct:.1f}%"
        elif decline_pct < -threshold_pct * 0.7:
            status = DriftStatus.WARNING
            message = f"Throughput decline {abs(decline_pct):.1f}% approaching threshold {threshold_pct:.1f}%"
        else:
            status = DriftStatus.STABLE
            message = f"Throughput stable: {decline_pct:.1f}% change (threshold {threshold_pct:.1f}%)"

        # Calculate confidence
        confidence = self._calculate_confidence(values)

        return DriftResult(
            metric_name="throughput",
            status=status,
            drift_percentage=decline_pct,
            threshold_percentage=threshold_pct,
            baseline_value=baseline,
            current_value=current,
            trend_slope=trend_slope,
            confidence=confidence,
            message=message
        )

    def calculate_moving_average(self, values: List[float], window: Optional[int] = None) -> List[float]:
        """
        Calculate moving average of values.

        Args:
            values: List of numeric values
            window: Window size (default: use instance window)

        Returns:
            List of moving average values (same length as input)
        """
        if not values:
            return []

        window = window or self.moving_avg_window
        window = max(1, min(window, len(values)))

        moving_avg = []
        for i in range(len(values)):
            start_idx = max(0, i - window + 1)
            window_values = values[start_idx:i + 1]
            moving_avg.append(statistics.mean(window_values))

        return moving_avg

    def _calculate_trend_slope(self, values: List[float]) -> float:
        """
        Calculate trend slope using simple linear regression.

        Args:
            values: List of numeric values

        Returns:
            Slope of linear trend (positive = increasing, negative = decreasing)
        """
        if len(values) < 2:
            return 0.0

        n = len(values)
        x = list(range(n))
        y = values

        # Calculate means
        x_mean = statistics.mean(x)
        y_mean = statistics.mean(y)

        # Calculate slope using least squares
        numerator = sum((x[i] - x_mean) * (y[i] - y_mean) for i in range(n))
        denominator = sum((x[i] - x_mean) ** 2 for i in range(n))

        if denominator == 0:
            return 0.0

        slope = numerator / denominator
        return slope

    def _calculate_confidence(self, values: List[float]) -> float:
        """
        Calculate confidence score based on data quality.

        Higher confidence when:
        - More data points available
        - Lower variance in data
        - Consistent trend

        Args:
            values: List of numeric values

        Returns:
            Confidence score 0.0-1.0
        """
        if not values:
            return 0.0

        # Data point confidence (more points = higher confidence)
        data_points_confidence = min(1.0, len(values) / 20.0)

        # Variance confidence (lower variance = higher confidence)
        if len(values) > 1:
            try:
                stdev = statistics.stdev(values)
                mean = statistics.mean(values)
                cv = stdev / mean if mean > 0 else 1.0  # Coefficient of variation
                variance_confidence = max(0.0, 1.0 - min(1.0, cv))
            except statistics.StatisticsError:
                variance_confidence = 0.5
        else:
            variance_confidence = 0.5

        # Combine confidences (weighted average)
        confidence = (data_points_confidence * 0.6) + (variance_confidence * 0.4)

        return confidence

    def _create_insufficient_data_result(
        self,
        metric_name: str,
        threshold_pct: float,
        reason: str
    ) -> DriftResult:
        """
        Create result for insufficient data scenarios.

        Args:
            metric_name: Name of the metric
            threshold_pct: Threshold percentage
            reason: Reason for insufficient data

        Returns:
            DriftResult with warning status
        """
        return DriftResult(
            metric_name=metric_name,
            status=DriftStatus.WARNING,
            drift_percentage=0.0,
            threshold_percentage=threshold_pct,
            baseline_value=0.0,
            current_value=0.0,
            trend_slope=0.0,
            confidence=0.0,
            message=f"Insufficient data for analysis: {reason}"
        )

    def analyze_all(
        self,
        memory_metrics: List[MetricPoint],
        latency_metrics: List[MetricPoint],
        memory_threshold: float = 5.0,
        latency_threshold: float = 10.0
    ) -> Dict[str, DriftResult]:
        """
        Run all drift detection analyses.

        Args:
            memory_metrics: Memory usage metrics
            latency_metrics: Latency metrics
            memory_threshold: Memory drift threshold percentage
            latency_threshold: Latency degradation threshold percentage

        Returns:
            Dictionary with analysis results for each metric type
        """
        results = {}

        # Memory drift analysis
        if memory_metrics:
            results['memory'] = self.detect_memory_drift(
                memory_metrics,
                threshold_pct=memory_threshold
            )

        # Latency degradation analysis
        if latency_metrics:
            results['latency'] = self.detect_latency_degradation(
                latency_metrics,
                threshold_pct=latency_threshold
            )

        return results

    def get_summary(self, results: Dict[str, DriftResult]) -> Dict[str, Any]:
        """
        Get summary of analysis results.

        Args:
            results: Dictionary of DriftResult objects

        Returns:
            Summary dictionary with overall status and details
        """
        if not results:
            return {
                'overall_status': 'unknown',
                'passed': False,
                'failures': [],
                'warnings': [],
                'message': 'No analysis results available'
            }

        # Count status types
        critical_count = sum(1 for r in results.values() if r.status == DriftStatus.CRITICAL)
        warning_count = sum(1 for r in results.values() if r.status == DriftStatus.WARNING)

        # Determine overall status
        if critical_count > 0:
            overall_status = 'critical'
            passed = False
        elif warning_count > 0:
            overall_status = 'warning'
            passed = True
        else:
            overall_status = 'stable'
            passed = True

        # Collect failures and warnings
        failures = [r.message for r in results.values() if r.status == DriftStatus.CRITICAL]
        warnings = [r.message for r in results.values() if r.status == DriftStatus.WARNING]

        # Build summary message
        if critical_count > 0:
            message = f"{critical_count} critical issue(s) detected"
        elif warning_count > 0:
            message = f"{warning_count} warning(s) detected"
        else:
            message = "All metrics stable"

        return {
            'overall_status': overall_status,
            'passed': passed,
            'critical_count': critical_count,
            'warning_count': warning_count,
            'failures': failures,
            'warnings': warnings,
            'message': message,
            'results': {name: {
                'status': result.status.value,
                'drift_pct': result.drift_percentage,
                'threshold_pct': result.threshold_percentage,
                'confidence': result.confidence,
                'message': result.message
            } for name, result in results.items()}
        }
