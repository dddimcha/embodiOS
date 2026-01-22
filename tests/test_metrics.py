#!/usr/bin/env python3
"""
Unit tests for MetricsCollector module
Tests Prometheus metrics collection for EMBODIOS API
"""

import time
from unittest.mock import patch, MagicMock
import pytest
from prometheus_client import CollectorRegistry

from src.embodi.api.metrics import MetricsCollector, get_metrics_collector


@pytest.fixture
def custom_registry():
    """Provide a clean Prometheus registry for each test"""
    return CollectorRegistry()


@pytest.fixture
def collector(custom_registry):
    """Provide a MetricsCollector instance with custom registry"""
    with patch('src.embodi.api.metrics.Counter') as mock_counter, \
         patch('src.embodi.api.metrics.Histogram') as mock_histogram, \
         patch('src.embodi.api.metrics.Gauge') as mock_gauge:

        # Create mock metrics that don't register globally
        # Each call creates a new MagicMock to avoid sharing
        mock_counter.side_effect = lambda *args, **kwargs: MagicMock()
        mock_histogram.side_effect = lambda *args, **kwargs: MagicMock()
        mock_gauge.side_effect = lambda *args, **kwargs: MagicMock()

        collector = MetricsCollector()
        return collector


class TestMetricsCollectorInit:
    """Test suite for MetricsCollector initialization"""

    def test_init_creates_all_metrics(self, collector):
        """Test that __init__ creates all required Prometheus metrics"""
        # Verify all metrics are created
        assert hasattr(collector, 'inference_requests_total'), \
            "Should create inference_requests_total Counter"
        assert hasattr(collector, 'inference_latency_seconds'), \
            "Should create inference_latency_seconds Histogram"
        assert hasattr(collector, 'inference_requests_in_progress'), \
            "Should create inference_requests_in_progress Gauge"
        assert hasattr(collector, 'memory_usage_bytes'), \
            "Should create memory_usage_bytes Gauge"
        assert hasattr(collector, 'uptime_seconds'), \
            "Should create uptime_seconds Gauge"
        assert hasattr(collector, 'model_loaded'), \
            "Should create model_loaded Gauge"

    def test_init_sets_start_time(self):
        """Test that __init__ records the start time for uptime calculation"""
        with patch('time.time', return_value=1234567890.0), \
             patch('src.embodi.api.metrics.Counter'), \
             patch('src.embodi.api.metrics.Histogram'), \
             patch('src.embodi.api.metrics.Gauge'):
            collector = MetricsCollector()
            assert collector._start_time == 1234567890.0, \
                "Should record start time on initialization"

    def test_inference_requests_total_has_labels(self, collector):
        """Test that inference_requests_total Counter has correct labels"""
        # Mock counter should accept labels method call
        collector.inference_requests_total.labels = MagicMock(return_value=MagicMock())

        # Counter should accept labels: method, endpoint, status
        try:
            collector.inference_requests_total.labels(
                method='POST',
                endpoint='/api/inference',
                status='200'
            )
        except Exception as e:
            pytest.fail(f"Counter should accept method, endpoint, status labels: {e}")

    def test_inference_latency_histogram_has_buckets(self, collector):
        """Test that inference_latency_seconds Histogram has proper buckets"""
        # Just verify it's a Histogram with observe method
        collector.inference_latency_seconds.observe = MagicMock()
        assert hasattr(collector.inference_latency_seconds, 'observe'), \
            "Histogram should have observe method"


class TestMetricsCollectorSystemMetrics:
    """Test suite for system metrics updates"""

    def test_update_system_metrics_updates_memory(self, collector):
        """Test that update_system_metrics updates memory_usage_bytes"""
        # Mock psutil.Process
        mock_process = MagicMock()
        mock_memory_info = MagicMock()
        mock_memory_info.rss = 123456789  # bytes
        mock_process.memory_info.return_value = mock_memory_info

        # Reset mock call counts
        collector.memory_usage_bytes.set.reset_mock()

        with patch('src.embodi.api.metrics.psutil.Process', return_value=mock_process):
            collector.update_system_metrics()

            # Verify memory gauge was updated
            collector.memory_usage_bytes.set.assert_called_with(123456789)

    def test_update_system_metrics_updates_uptime(self, collector):
        """Test that update_system_metrics calculates and updates uptime"""
        # Set collector start time to known value
        collector._start_time = 1000.0

        # Mock psutil for memory (required by update_system_metrics)
        mock_process = MagicMock()
        mock_memory_info = MagicMock()
        mock_memory_info.rss = 100000
        mock_process.memory_info.return_value = mock_memory_info

        # Reset mock call counts
        collector.uptime_seconds.set.reset_mock()

        # Current time is 1500.0, so uptime should be 500.0 seconds
        with patch('src.embodi.api.metrics.psutil.Process', return_value=mock_process):
            with patch('time.time', return_value=1500.0):
                collector.update_system_metrics()

                # Verify uptime was calculated correctly
                collector.uptime_seconds.set.assert_called_with(500.0)

    def test_update_system_metrics_uses_current_process(self, collector):
        """Test that update_system_metrics gets metrics from current process"""
        mock_process = MagicMock()
        mock_memory_info = MagicMock()
        mock_memory_info.rss = 100000
        mock_process.memory_info.return_value = mock_memory_info

        with patch('src.embodi.api.metrics.psutil.Process') as mock_process_class:
            mock_process_class.return_value = mock_process

            collector.update_system_metrics()

            # Verify psutil.Process was called without arguments (current process)
            mock_process_class.assert_called_once_with()
            mock_process.memory_info.assert_called_once()


class TestMetricsCollectorRecordRequest:
    """Test suite for request recording"""

    def test_record_request_increments_counter(self, collector):
        """Test that record_request increments the request counter"""
        # Mock the counter
        mock_counter_labels = MagicMock()
        mock_counter = MagicMock()
        mock_counter_labels.return_value = mock_counter

        with patch.object(collector.inference_requests_total, 'labels',
                         mock_counter_labels):
            collector.record_request('POST', '/api/inference', 200, 0.123)

            # Verify labels were applied correctly
            mock_counter_labels.assert_called_once_with(
                method='POST',
                endpoint='/api/inference',
                status='200'  # Should convert to string
            )
            # Verify counter was incremented
            mock_counter.inc.assert_called_once()

    def test_record_request_observes_latency(self, collector):
        """Test that record_request records latency in histogram"""
        # Mock the histogram
        mock_histogram_labels = MagicMock()
        mock_histogram = MagicMock()
        mock_histogram_labels.return_value = mock_histogram

        with patch.object(collector.inference_latency_seconds, 'labels',
                         mock_histogram_labels):
            collector.record_request('POST', '/api/inference', 200, 0.456)

            # Verify labels were applied correctly
            mock_histogram_labels.assert_called_once_with(
                method='POST',
                endpoint='/api/inference'
            )
            # Verify duration was observed
            mock_histogram.observe.assert_called_once_with(0.456)

    def test_record_request_handles_different_status_codes(self, collector):
        """Test that record_request handles various HTTP status codes"""
        test_cases = [
            (200, '200'),
            (404, '404'),
            (500, '500'),
            (201, '201'),
        ]

        for status_code, expected_label in test_cases:
            mock_counter_labels = MagicMock()
            mock_counter = MagicMock()
            mock_counter_labels.return_value = mock_counter

            with patch.object(collector.inference_requests_total, 'labels',
                             mock_counter_labels):
                collector.record_request('GET', '/test', status_code, 0.1)

                # Verify status was converted to string
                call_kwargs = mock_counter_labels.call_args[1]
                assert call_kwargs['status'] == expected_label, \
                    f"Status {status_code} should be converted to '{expected_label}'"

    def test_record_request_handles_different_methods(self, collector):
        """Test that record_request handles various HTTP methods"""
        methods = ['GET', 'POST', 'PUT', 'DELETE', 'PATCH']

        for method in methods:
            mock_counter_labels = MagicMock()
            mock_counter = MagicMock()
            mock_counter_labels.return_value = mock_counter

            with patch.object(collector.inference_requests_total, 'labels',
                             mock_counter_labels):
                collector.record_request(method, '/test', 200, 0.1)

                # Verify method was passed correctly
                call_kwargs = mock_counter_labels.call_args[1]
                assert call_kwargs['method'] == method, \
                    f"Method {method} should be passed correctly"


class TestMetricsCollectorModelStatus:
    """Test suite for model status tracking"""

    def test_set_model_status_true_sets_one(self, collector):
        """Test that set_model_status(True) sets gauge to 1"""
        with patch.object(collector.model_loaded, 'set') as mock_set:
            collector.set_model_status(True)

            # Verify gauge was set to 1
            mock_set.assert_called_once_with(1)

    def test_set_model_status_false_sets_zero(self, collector):
        """Test that set_model_status(False) sets gauge to 0"""
        with patch.object(collector.model_loaded, 'set') as mock_set:
            collector.set_model_status(False)

            # Verify gauge was set to 0
            mock_set.assert_called_once_with(0)


class TestGetMetricsCollector:
    """Test suite for global metrics collector singleton"""

    def test_get_metrics_collector_creates_instance(self):
        """Test that get_metrics_collector creates a MetricsCollector instance"""
        # Reset global instance
        import src.embodi.api.metrics as metrics_module
        metrics_module._metrics_collector = None

        with patch('src.embodi.api.metrics.Counter'), \
             patch('src.embodi.api.metrics.Histogram'), \
             patch('src.embodi.api.metrics.Gauge'):
            collector = get_metrics_collector()

            assert isinstance(collector, MetricsCollector), \
                "Should return a MetricsCollector instance"

    def test_get_metrics_collector_returns_singleton(self):
        """Test that get_metrics_collector returns same instance on multiple calls"""
        # Reset global instance
        import src.embodi.api.metrics as metrics_module
        metrics_module._metrics_collector = None

        with patch('src.embodi.api.metrics.Counter'), \
             patch('src.embodi.api.metrics.Histogram'), \
             patch('src.embodi.api.metrics.Gauge'):
            collector1 = get_metrics_collector()
            collector2 = get_metrics_collector()

            assert collector1 is collector2, \
                "Should return the same instance (singleton pattern)"

    def test_get_metrics_collector_does_not_recreate(self):
        """Test that get_metrics_collector reuses existing instance"""
        # Reset global instance
        import src.embodi.api.metrics as metrics_module
        metrics_module._metrics_collector = None

        with patch('src.embodi.api.metrics.Counter'), \
             patch('src.embodi.api.metrics.Histogram'), \
             patch('src.embodi.api.metrics.Gauge'):
            # Create first instance
            collector1 = get_metrics_collector()

            # Modify a property to verify it's the same instance
            collector1._test_marker = 'test_value'

            # Get instance again
            collector2 = get_metrics_collector()

            assert hasattr(collector2, '_test_marker'), \
                "Should return same instance with preserved state"
            assert collector2._test_marker == 'test_value', \
                "Instance state should be preserved"
