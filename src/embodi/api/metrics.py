"""
Metrics Collection Module - Prometheus-compatible metrics for EMBODIOS API
"""

import time
import psutil
from typing import Optional
from prometheus_client import Counter, Histogram, Gauge


class MetricsCollector:
    """
    Centralized metrics collection for EMBODIOS API server.

    Provides Prometheus-compatible metrics for monitoring:
    - Request counts and latency
    - System resource usage
    - Model loading status
    - Server uptime
    """

    def __init__(self):
        """Initialize Prometheus metrics"""
        self._start_time = time.time()

        # Counter: Total inference requests
        self.inference_requests_total = Counter(
            'inference_requests_total',
            'Total number of inference requests',
            ['method', 'endpoint', 'status']
        )

        # Histogram: Request latency distribution
        self.inference_latency_seconds = Histogram(
            'inference_latency_seconds',
            'Inference request latency in seconds',
            ['method', 'endpoint'],
            buckets=(0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0)
        )

        # Gauge: Current in-progress requests
        self.inference_requests_in_progress = Gauge(
            'inference_requests_in_progress',
            'Number of inference requests currently being processed'
        )

        # Gauge: Memory usage in bytes
        self.memory_usage_bytes = Gauge(
            'memory_usage_bytes',
            'Current memory usage in bytes'
        )

        # Gauge: Server uptime in seconds
        self.uptime_seconds = Gauge(
            'uptime_seconds',
            'Server uptime in seconds'
        )

        # Gauge: Model loaded status (0=not loaded, 1=loaded)
        self.model_loaded = Gauge(
            'model_loaded',
            'Whether AI model is loaded (0=no, 1=yes)'
        )

    def update_system_metrics(self):
        """Update system resource metrics (memory, uptime)"""
        # Update memory usage
        process = psutil.Process()
        memory_info = process.memory_info()
        self.memory_usage_bytes.set(memory_info.rss)

        # Update uptime
        uptime = time.time() - self._start_time
        self.uptime_seconds.set(uptime)

    def record_request(self, method: str, endpoint: str, status: int, duration: float):
        """
        Record a completed request.

        Args:
            method: HTTP method (GET, POST, etc.)
            endpoint: Request endpoint path
            status: HTTP status code
            duration: Request duration in seconds
        """
        self.inference_requests_total.labels(
            method=method,
            endpoint=endpoint,
            status=str(status)
        ).inc()

        self.inference_latency_seconds.labels(
            method=method,
            endpoint=endpoint
        ).observe(duration)

    def set_model_status(self, loaded: bool):
        """
        Update model loaded status.

        Args:
            loaded: True if model is loaded, False otherwise
        """
        self.model_loaded.set(1 if loaded else 0)


# Global metrics collector instance
_metrics_collector: Optional[MetricsCollector] = None


def get_metrics_collector() -> MetricsCollector:
    """
    Get or create the global metrics collector instance.

    Returns:
        MetricsCollector instance
    """
    global _metrics_collector
    if _metrics_collector is None:
        _metrics_collector = MetricsCollector()
    return _metrics_collector
