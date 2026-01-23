#!/usr/bin/env python3
"""
Real-time alerting for stability test regression detection.

Provides real-time monitoring and alerting when stability thresholds are
exceeded. Supports multiple alert channels (stdout, webhook) and includes
alert deduplication to prevent spam.
"""

import time
import json
from enum import Enum
from dataclasses import dataclass, field
from typing import List, Dict, Any, Optional, Callable, Set
from pathlib import Path

from tests.stability.analysis import DriftResult, DriftStatus


class AlertSeverity(Enum):
    """Alert severity levels"""
    INFO = "info"
    WARNING = "warning"
    CRITICAL = "critical"


@dataclass
class Alert:
    """
    Alert message for stability regression.

    Attributes:
        severity: Alert severity level
        metric_name: Name of metric that triggered alert
        message: Human-readable alert message
        drift_percentage: Percentage drift from baseline
        threshold_percentage: Threshold that was exceeded
        timestamp: When alert was triggered
        test_name: Name of the test
        metadata: Additional alert metadata
    """

    severity: AlertSeverity
    metric_name: str
    message: str
    drift_percentage: float
    threshold_percentage: float
    timestamp: float = field(default_factory=time.time)
    test_name: str = "stability_test"
    metadata: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        """Convert alert to dictionary"""
        return {
            'severity': self.severity.value,
            'metric_name': self.metric_name,
            'message': self.message,
            'drift_percentage': self.drift_percentage,
            'threshold_percentage': self.threshold_percentage,
            'timestamp': self.timestamp,
            'test_name': self.test_name,
            'metadata': self.metadata,
        }

    def format_stdout(self) -> str:
        """Format alert for stdout display"""
        severity_icons = {
            AlertSeverity.INFO: "ℹ️",
            AlertSeverity.WARNING: "⚠️",
            AlertSeverity.CRITICAL: "🚨",
        }
        icon = severity_icons.get(self.severity, "")

        severity_colors = {
            AlertSeverity.INFO: "\033[94m",  # Blue
            AlertSeverity.WARNING: "\033[93m",  # Yellow
            AlertSeverity.CRITICAL: "\033[91m",  # Red
        }
        reset_color = "\033[0m"
        color = severity_colors.get(self.severity, "")

        return (
            f"\n{color}{'=' * 80}{reset_color}\n"
            f"{color}{icon} STABILITY ALERT [{self.severity.value.upper()}]{reset_color}\n"
            f"{color}Metric: {self.metric_name}{reset_color}\n"
            f"{color}Drift: {self.drift_percentage:.1f}% (threshold: {self.threshold_percentage:.1f}%){reset_color}\n"
            f"{color}Message: {self.message}{reset_color}\n"
            f"{color}{'=' * 80}{reset_color}\n"
        )


class AlertChannel:
    """
    Base class for alert delivery channels.

    Alert channels deliver alerts to different destinations (stdout, webhook,
    email, etc.). Subclass this to implement custom alert delivery.
    """

    def send(self, alert: Alert) -> bool:
        """
        Send alert through this channel.

        Args:
            alert: Alert to send

        Returns:
            True if alert was sent successfully, False otherwise
        """
        raise NotImplementedError("Subclasses must implement send()")


class StdoutAlertChannel(AlertChannel):
    """
    Alert channel that prints to stdout.

    Provides colored, formatted output for alerts during test execution.
    """

    def __init__(self, min_severity: AlertSeverity = AlertSeverity.WARNING):
        """
        Initialize stdout alert channel.

        Args:
            min_severity: Minimum severity to display (default: WARNING)
        """
        self.min_severity = min_severity

    def send(self, alert: Alert) -> bool:
        """Send alert to stdout"""
        severity_levels = {
            AlertSeverity.INFO: 0,
            AlertSeverity.WARNING: 1,
            AlertSeverity.CRITICAL: 2,
        }

        if severity_levels.get(alert.severity, 0) >= severity_levels.get(self.min_severity, 0):
            print(alert.format_stdout(), flush=True)
            return True
        return False


class WebhookAlertChannel(AlertChannel):
    """
    Alert channel that sends to webhook URL.

    Posts alert data as JSON to configured webhook endpoint.
    """

    def __init__(self, webhook_url: str, timeout_seconds: int = 5):
        """
        Initialize webhook alert channel.

        Args:
            webhook_url: URL to post alerts to
            timeout_seconds: Request timeout in seconds
        """
        self.webhook_url = webhook_url
        self.timeout_seconds = timeout_seconds

    def send(self, alert: Alert) -> bool:
        """Send alert to webhook"""
        try:
            import requests

            payload = alert.to_dict()
            response = requests.post(
                self.webhook_url,
                json=payload,
                timeout=self.timeout_seconds,
                headers={'Content-Type': 'application/json'}
            )
            response.raise_for_status()
            return True
        except ImportError:
            print(f"Warning: requests library not available, webhook alert skipped")
            return False
        except Exception as e:
            print(f"Warning: Failed to send webhook alert: {e}")
            return False


class FileAlertChannel(AlertChannel):
    """
    Alert channel that appends to a log file.

    Writes alert data as JSON lines to a log file for post-test analysis.
    """

    def __init__(self, log_path: Path):
        """
        Initialize file alert channel.

        Args:
            log_path: Path to alert log file
        """
        self.log_path = Path(log_path)
        self.log_path.parent.mkdir(parents=True, exist_ok=True)

    def send(self, alert: Alert) -> bool:
        """Send alert to file"""
        try:
            with open(self.log_path, 'a') as f:
                f.write(json.dumps(alert.to_dict()) + '\n')
            return True
        except Exception as e:
            print(f"Warning: Failed to write alert to file: {e}")
            return False


class AlertManager:
    """
    Manages real-time alerting for stability tests.

    Monitors drift analysis results and triggers alerts when thresholds are
    exceeded. Includes alert deduplication to prevent spam and supports
    multiple alert channels.

    Example:
        # Setup alert manager
        manager = AlertManager(test_name='my_test')
        manager.add_channel(StdoutAlertChannel())

        # During test execution
        analyzer = DriftAnalyzer()
        memory_result = analyzer.detect_memory_drift(metrics)
        manager.check_and_alert('memory', memory_result)
    """

    def __init__(
        self,
        test_name: str = 'stability_test',
        dedupe_window_seconds: int = 300
    ):
        """
        Initialize alert manager.

        Args:
            test_name: Name of the test (for alert identification)
            dedupe_window_seconds: Time window for alert deduplication
        """
        self.test_name = test_name
        self.dedupe_window_seconds = dedupe_window_seconds
        self.channels: List[AlertChannel] = []
        self.alert_history: List[Alert] = []
        self._alerted_metrics: Dict[str, float] = {}  # metric -> last alert time

    def add_channel(self, channel: AlertChannel) -> None:
        """
        Add alert delivery channel.

        Args:
            channel: AlertChannel to add
        """
        self.channels.append(channel)

    def check_and_alert(
        self,
        metric_name: str,
        drift_result: DriftResult,
        force: bool = False
    ) -> Optional[Alert]:
        """
        Check drift result and trigger alert if needed.

        Args:
            metric_name: Name of the metric
            drift_result: DriftResult from analysis
            force: If True, skip deduplication

        Returns:
            Alert if one was triggered, None otherwise
        """
        # Determine severity from drift status
        severity_map = {
            DriftStatus.STABLE: None,  # Don't alert on stable
            DriftStatus.WARNING: AlertSeverity.WARNING,
            DriftStatus.CRITICAL: AlertSeverity.CRITICAL,
        }

        severity = severity_map.get(drift_result.status)
        if severity is None:
            return None

        # Check deduplication (skip if recently alerted on this metric)
        if not force and not self._should_alert(metric_name):
            return None

        # Create alert
        alert = Alert(
            severity=severity,
            metric_name=metric_name,
            message=drift_result.message,
            drift_percentage=drift_result.drift_percentage,
            threshold_percentage=drift_result.threshold_percentage,
            test_name=self.test_name,
            metadata={
                'baseline_value': drift_result.baseline_value,
                'current_value': drift_result.current_value,
                'trend_slope': drift_result.trend_slope,
                'confidence': drift_result.confidence,
            }
        )

        # Send through all channels
        self._send_alert(alert)

        # Update alert history and deduplication state
        self.alert_history.append(alert)
        self._alerted_metrics[metric_name] = time.time()

        return alert

    def check_multiple(
        self,
        drift_results: Dict[str, DriftResult]
    ) -> List[Alert]:
        """
        Check multiple drift results and trigger alerts as needed.

        Args:
            drift_results: Dictionary mapping metric names to DriftResults

        Returns:
            List of alerts that were triggered
        """
        alerts = []
        for metric_name, drift_result in drift_results.items():
            alert = self.check_and_alert(metric_name, drift_result)
            if alert:
                alerts.append(alert)
        return alerts

    def alert_info(self, message: str, metric_name: str = 'general') -> Alert:
        """
        Send informational alert.

        Args:
            message: Alert message
            metric_name: Metric name (default: 'general')

        Returns:
            Alert that was sent
        """
        alert = Alert(
            severity=AlertSeverity.INFO,
            metric_name=metric_name,
            message=message,
            drift_percentage=0.0,
            threshold_percentage=0.0,
            test_name=self.test_name,
        )

        self._send_alert(alert)
        self.alert_history.append(alert)
        return alert

    def get_alert_summary(self) -> Dict[str, Any]:
        """
        Get summary of alerts triggered during test.

        Returns:
            Dictionary with alert statistics
        """
        if not self.alert_history:
            return {
                'total_alerts': 0,
                'critical_count': 0,
                'warning_count': 0,
                'info_count': 0,
                'metrics_alerted': [],
            }

        critical_count = sum(1 for a in self.alert_history if a.severity == AlertSeverity.CRITICAL)
        warning_count = sum(1 for a in self.alert_history if a.severity == AlertSeverity.WARNING)
        info_count = sum(1 for a in self.alert_history if a.severity == AlertSeverity.INFO)

        metrics_alerted = list(set(a.metric_name for a in self.alert_history))

        return {
            'total_alerts': len(self.alert_history),
            'critical_count': critical_count,
            'warning_count': warning_count,
            'info_count': info_count,
            'metrics_alerted': metrics_alerted,
            'alerts': [a.to_dict() for a in self.alert_history],
        }

    def _should_alert(self, metric_name: str) -> bool:
        """
        Check if we should alert on this metric (deduplication).

        Args:
            metric_name: Name of the metric

        Returns:
            True if we should alert, False if too soon since last alert
        """
        if metric_name not in self._alerted_metrics:
            return True

        last_alert_time = self._alerted_metrics[metric_name]
        time_since_last = time.time() - last_alert_time

        return time_since_last >= self.dedupe_window_seconds

    def _send_alert(self, alert: Alert) -> None:
        """
        Send alert through all configured channels.

        Args:
            alert: Alert to send
        """
        if not self.channels:
            # Default to stdout if no channels configured
            print(alert.format_stdout(), flush=True)
            return

        for channel in self.channels:
            try:
                channel.send(alert)
            except Exception as e:
                print(f"Warning: Alert channel {channel.__class__.__name__} failed: {e}")
