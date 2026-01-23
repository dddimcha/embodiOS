#!/usr/bin/env python3
"""
Unit tests for EMBODIOS Benchmarking Module
Tests latency measurement, statistical analysis, and comparison utilities
"""

import time
import json
import subprocess
import statistics
from unittest.mock import patch, MagicMock, mock_open, call
import pytest

from src.embodi.core.benchmarking import (
    LatencyMeasurement,
    BenchmarkResult,
    LatencyBenchmark,
    EMBODIOSBenchmarkRunner,
    ComparisonResult,
    BenchmarkComparison,
    LlamaCppBenchmarkRunner,
    calculate_percentiles,
    parse_llamacpp_output,
    benchmark_function,
    format_benchmark_table,
    format_comparison_table,
    PSUTIL_AVAILABLE,
    RICH_AVAILABLE
)


@pytest.fixture
def sample_measurements():
    """Provide sample latency measurements for testing"""
    return [
        LatencyMeasurement(
            start_time=1000.0,
            end_time=1000.1,
            duration_ms=100.0,
            memory_used_mb=10.0,
            metadata={"label": "test_1"}
        ),
        LatencyMeasurement(
            start_time=1001.0,
            end_time=1001.15,
            duration_ms=150.0,
            memory_used_mb=12.0,
            metadata={"label": "test_2"}
        ),
        LatencyMeasurement(
            start_time=1002.0,
            end_time=1002.2,
            duration_ms=200.0,
            memory_used_mb=15.0,
            metadata={"label": "test_3"}
        ),
    ]


@pytest.fixture
def sample_benchmark_result(sample_measurements):
    """Provide a sample BenchmarkResult for testing"""
    durations = [m.duration_ms for m in sample_measurements]
    return BenchmarkResult(
        total_measurements=len(sample_measurements),
        total_duration_ms=sum(durations),
        measurements=sample_measurements,
        mean_ms=statistics.mean(durations),
        median_ms=statistics.median(durations),
        min_ms=min(durations),
        max_ms=max(durations),
        stdev_ms=statistics.stdev(durations),
        percentiles={"p50": 150.0, "p95": 200.0, "p99": 200.0, "p99.9": 200.0},
        throughput_per_sec=6.666,
        peak_memory_mb=15.0,
        avg_memory_mb=12.333,
        metadata={"test": "value"}
    )


class TestLatencyMeasurement:
    """Test suite for LatencyMeasurement dataclass"""

    def test_init_creates_measurement(self):
        """Test that LatencyMeasurement initializes with correct values"""
        measurement = LatencyMeasurement(
            start_time=1000.0,
            end_time=1000.5,
            duration_ms=500.0,
            memory_used_mb=20.0,
            metadata={"test": "data"}
        )

        assert measurement.start_time == 1000.0, "Should set start_time"
        assert measurement.end_time == 1000.5, "Should set end_time"
        assert measurement.duration_ms == 500.0, "Should set duration_ms"
        assert measurement.memory_used_mb == 20.0, "Should set memory_used_mb"
        assert measurement.metadata == {"test": "data"}, "Should set metadata"

    def test_init_with_defaults(self):
        """Test that LatencyMeasurement uses default values"""
        measurement = LatencyMeasurement(
            start_time=1000.0,
            end_time=1000.5,
            duration_ms=500.0
        )

        assert measurement.memory_used_mb == 0.0, "Should default memory to 0.0"
        assert measurement.metadata == {}, "Should default metadata to empty dict"

    def test_to_dict(self):
        """Test that to_dict converts measurement to dictionary"""
        measurement = LatencyMeasurement(
            start_time=1000.0,
            end_time=1000.5,
            duration_ms=500.0,
            memory_used_mb=20.0,
            metadata={"test": "data"}
        )

        result = measurement.to_dict()

        assert isinstance(result, dict), "Should return dictionary"
        assert result["start_time"] == 1000.0, "Should include start_time"
        assert result["end_time"] == 1000.5, "Should include end_time"
        assert result["duration_ms"] == 500.0, "Should include duration_ms"
        assert result["memory_used_mb"] == 20.0, "Should include memory_used_mb"
        assert result["metadata"] == {"test": "data"}, "Should include metadata"


class TestBenchmarkResult:
    """Test suite for BenchmarkResult dataclass"""

    def test_init_creates_result(self, sample_measurements):
        """Test that BenchmarkResult initializes with correct values"""
        result = BenchmarkResult(
            total_measurements=3,
            total_duration_ms=450.0,
            measurements=sample_measurements,
            mean_ms=150.0,
            median_ms=150.0,
            min_ms=100.0,
            max_ms=200.0,
            stdev_ms=50.0,
            percentiles={"p50": 150.0, "p95": 195.0, "p99": 199.0, "p99.9": 200.0},
            throughput_per_sec=6.666,
            peak_memory_mb=15.0,
            avg_memory_mb=12.333
        )

        assert result.total_measurements == 3, "Should set total_measurements"
        assert result.total_duration_ms == 450.0, "Should set total_duration_ms"
        assert len(result.measurements) == 3, "Should set measurements list"
        assert result.mean_ms == 150.0, "Should set mean_ms"
        assert result.throughput_per_sec == 6.666, "Should set throughput_per_sec"

    def test_to_dict_converts_measurements(self, sample_benchmark_result):
        """Test that to_dict converts nested measurements to dicts"""
        result = sample_benchmark_result.to_dict()

        assert isinstance(result, dict), "Should return dictionary"
        assert isinstance(result["measurements"], list), "Should include measurements list"
        assert isinstance(result["measurements"][0], dict), "Should convert measurements to dicts"
        assert "duration_ms" in result["measurements"][0], "Should include measurement fields"


class TestCalculatePercentiles:
    """Test suite for calculate_percentiles function"""

    def test_calculates_percentiles_for_simple_list(self):
        """Test that calculate_percentiles works with simple list"""
        values = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0]
        percentiles = calculate_percentiles(values)

        assert "p50" in percentiles, "Should calculate P50"
        assert "p95" in percentiles, "Should calculate P95"
        assert "p99" in percentiles, "Should calculate P99"
        assert "p99.9" in percentiles, "Should calculate P99.9"

        # P50 should be around the middle (index-based calculation)
        assert percentiles["p50"] == 6.0, "P50 should be at 50% index"

    def test_calculates_percentiles_for_large_dataset(self):
        """Test percentile calculation on large dataset"""
        values = list(range(1, 10001))  # 10000 values
        percentiles = calculate_percentiles(values)

        # For 10000 values, P95 should be around 9500
        assert percentiles["p95"] >= 9000, "P95 should be in top 5%"
        assert percentiles["p99"] >= 9800, "P99 should be in top 1%"
        assert percentiles["p99.9"] >= 9900, "P99.9 should be in top 0.1%"

    def test_raises_error_for_empty_list(self):
        """Test that calculate_percentiles raises ValueError for empty list"""
        with pytest.raises(ValueError, match="Cannot calculate percentiles for empty list"):
            calculate_percentiles([])

    def test_handles_single_value(self):
        """Test percentile calculation with single value"""
        values = [42.0]
        percentiles = calculate_percentiles(values)

        # All percentiles should equal the single value
        assert percentiles["p50"] == 42.0, "P50 should be the single value"
        assert percentiles["p95"] == 42.0, "P95 should be the single value"
        assert percentiles["p99"] == 42.0, "P99 should be the single value"

    def test_p999_uses_last_value_for_small_datasets(self):
        """Test that P99.9 uses last value for datasets < 1000 items"""
        values = list(range(1, 101))  # 100 values
        percentiles = calculate_percentiles(values)

        # P99.9 should be the last value for datasets < 1000
        assert percentiles["p99.9"] == 100, "P99.9 should be last value for small datasets"


class TestParseLlamacppOutput:
    """Test suite for parse_llamacpp_output function"""

    def test_parses_classic_format(self):
        """Test parsing classic llama_print_timings format"""
        output = """
llama_print_timings: load time = 134.84 ms
llama_print_timings: prompt eval time = 925.78 ms / 24 tokens ( 38.57 ms per token)
llama_print_timings: eval time = 4178.32 ms / 31 runs ( 134.78 ms per token)
        """

        metrics = parse_llamacpp_output(output)

        assert metrics["load_time_ms"] == 134.84, "Should parse load time"
        assert metrics["prompt_eval_time_ms"] == 925.78, "Should parse prompt eval time"
        assert metrics["prompt_tokens"] == 24, "Should parse prompt token count"
        assert metrics["prompt_ms_per_token"] == 38.57, "Should parse prompt ms per token"
        assert metrics["eval_time_ms"] == 4178.32, "Should parse eval time"
        assert metrics["eval_tokens"] == 31, "Should parse eval token count"
        assert metrics["eval_ms_per_token"] == 134.78, "Should parse eval ms per token"

    def test_parses_newer_format(self):
        """Test parsing newer llama_perf format with tokens per second"""
        output = """
llama_perf_context_print: load time = 318.81 ms
llama_perf_context_print: prompt eval time = 59.26 ms / 5 tokens ( 11.85 ms per token, 84.38 tokens per second)
llama_perf_context_print: eval time = 17797.98 ms / 379 runs ( 46.96 ms per token, 21.29 tokens per second)
        """

        metrics = parse_llamacpp_output(output)

        assert metrics["load_time_ms"] == 318.81, "Should parse load time"
        assert metrics["prompt_eval_time_ms"] == 59.26, "Should parse prompt eval time"
        assert metrics["prompt_tokens"] == 5, "Should parse prompt tokens"
        assert metrics["prompt_tokens_per_second"] == 84.38, "Should parse prompt tokens per second"
        assert metrics["eval_time_ms"] == 17797.98, "Should parse eval time"
        assert metrics["eval_tokens"] == 379, "Should parse eval tokens"
        assert metrics["eval_tokens_per_second"] == 21.29, "Should parse eval tokens per second"

    def test_calculates_derived_metrics(self):
        """Test that missing metrics are calculated from available data"""
        output = "llama_print_timings: prompt eval time = 100.0 ms / 10 tokens"

        metrics = parse_llamacpp_output(output)

        # Should calculate ms per token and tokens per second
        assert "prompt_ms_per_token" in metrics, "Should calculate ms per token"
        assert metrics["prompt_ms_per_token"] == 10.0, "Should calculate correct ms per token"
        assert "prompt_tokens_per_second" in metrics, "Should calculate tokens per second"
        assert metrics["prompt_tokens_per_second"] == 100.0, "Should calculate correct tokens per second"

    def test_accepts_list_of_lines(self):
        """Test that parse_llamacpp_output accepts list of strings"""
        lines = [
            "llama_print_timings: load time = 100.0 ms",
            "llama_print_timings: eval time = 500.0 ms / 10 runs"
        ]

        metrics = parse_llamacpp_output(lines)

        assert metrics["load_time_ms"] == 100.0, "Should parse from list of lines"
        assert metrics["eval_time_ms"] == 500.0, "Should parse multiple lines"

    def test_handles_empty_output(self):
        """Test that empty output returns empty metrics dict"""
        metrics = parse_llamacpp_output("")

        assert isinstance(metrics, dict), "Should return dictionary"
        assert len(metrics) == 0, "Should return empty dict for empty output"

    def test_calculates_total_tokens(self):
        """Test that total tokens is calculated from prompt + eval tokens"""
        output = """
llama_print_timings: prompt eval time = 100.0 ms / 10 tokens
llama_print_timings: eval time = 500.0 ms / 20 runs
        """

        metrics = parse_llamacpp_output(output)

        assert metrics["total_tokens"] == 30, "Should sum prompt and eval tokens"


class TestLatencyBenchmark:
    """Test suite for LatencyBenchmark class"""

    def test_init_creates_empty_benchmark(self):
        """Test that __init__ creates benchmark with empty measurements"""
        benchmark = LatencyBenchmark()

        assert len(benchmark.get_measurements()) == 0, "Should start with no measurements"
        assert benchmark._current_start is None, "Should have no active measurement"

    def test_start_measurement_records_time(self):
        """Test that start_measurement records start time"""
        benchmark = LatencyBenchmark()

        with patch('time.time', return_value=1234.567):
            benchmark.start_measurement()

        assert benchmark._current_start == 1234.567, "Should record start time"

    def test_end_measurement_calculates_duration(self):
        """Test that end_measurement calculates duration correctly"""
        benchmark = LatencyBenchmark()

        with patch('time.time', return_value=1000.0):
            benchmark.start_measurement()

        with patch('time.time', return_value=1000.5):
            measurement = benchmark.end_measurement()

        assert measurement.duration_ms == 500.0, "Should calculate duration in ms"
        assert measurement.start_time == 1000.0, "Should record start time"
        assert measurement.end_time == 1000.5, "Should record end time"

    def test_end_measurement_raises_error_if_not_started(self):
        """Test that end_measurement raises error if no measurement started"""
        benchmark = LatencyBenchmark()

        with pytest.raises(RuntimeError, match="No measurement in progress"):
            benchmark.end_measurement()

    def test_end_measurement_stores_measurement(self):
        """Test that end_measurement stores measurement in list"""
        benchmark = LatencyBenchmark()

        benchmark.start_measurement()
        benchmark.end_measurement()

        assert len(benchmark.get_measurements()) == 1, "Should store measurement"

    def test_end_measurement_with_metadata(self):
        """Test that end_measurement attaches metadata"""
        benchmark = LatencyBenchmark()

        benchmark.start_measurement()
        measurement = benchmark.end_measurement(metadata={"test": "value"})

        assert measurement.metadata == {"test": "value"}, "Should attach metadata"

    def test_measure_context_manager(self):
        """Test that measure() context manager works correctly"""
        benchmark = LatencyBenchmark()

        with patch('time.time', side_effect=[1000.0, 1000.1]):
            with benchmark.measure("test_label"):
                pass

        measurements = benchmark.get_measurements()
        assert len(measurements) == 1, "Should record one measurement"
        assert measurements[0].metadata.get("label") == "test_label", "Should attach label"
        assert abs(measurements[0].duration_ms - 100.0) < 0.01, "Should calculate duration"

    def test_measure_context_manager_without_label(self):
        """Test measure() without label"""
        benchmark = LatencyBenchmark()

        with benchmark.measure():
            pass

        measurements = benchmark.get_measurements()
        assert len(measurements) == 1, "Should record measurement"
        assert measurements[0].metadata == {}, "Should have empty metadata"

    def test_clear_measurements(self):
        """Test that clear_measurements removes all measurements"""
        benchmark = LatencyBenchmark()

        with benchmark.measure():
            pass

        assert len(benchmark.get_measurements()) > 0, "Should have measurements"

        benchmark.clear_measurements()

        assert len(benchmark.get_measurements()) == 0, "Should clear all measurements"
        assert benchmark._current_start is None, "Should reset current start"

    def test_get_result_calculates_statistics(self):
        """Test that get_result calculates correct statistics"""
        benchmark = LatencyBenchmark()

        # Add known measurements
        benchmark._measurements = [
            LatencyMeasurement(1000.0, 1000.1, 100.0, 0.0, {}),
            LatencyMeasurement(1001.0, 1001.15, 150.0, 0.0, {}),
            LatencyMeasurement(1002.0, 1002.2, 200.0, 0.0, {}),
        ]

        result = benchmark.get_result()

        assert result.total_measurements == 3, "Should count measurements"
        assert result.total_duration_ms == 450.0, "Should sum durations"
        assert result.mean_ms == 150.0, "Should calculate mean"
        assert result.median_ms == 150.0, "Should calculate median"
        assert result.min_ms == 100.0, "Should find minimum"
        assert result.max_ms == 200.0, "Should find maximum"

    def test_get_result_raises_error_if_no_measurements(self):
        """Test that get_result raises error if no measurements recorded"""
        benchmark = LatencyBenchmark()

        with pytest.raises(ValueError, match="No measurements recorded"):
            benchmark.get_result()

    def test_get_result_calculates_throughput(self):
        """Test that get_result calculates throughput correctly"""
        benchmark = LatencyBenchmark()

        # 10 measurements of 100ms each = 1000ms total
        # Throughput = 10 * 1000 / 1000 = 10 items/sec
        for _ in range(10):
            benchmark._measurements.append(
                LatencyMeasurement(0.0, 0.0, 100.0, 0.0, {})
            )

        result = benchmark.get_result()

        assert result.throughput_per_sec == 10.0, "Should calculate throughput"

    @patch('src.embodi.core.benchmarking.PSUTIL_AVAILABLE', True)
    def test_get_memory_usage_with_psutil(self):
        """Test that _get_memory_usage_mb uses psutil when available"""
        mock_process = MagicMock()
        mock_memory_info = MagicMock()
        mock_memory_info.rss = 104857600  # 100 MB in bytes
        mock_process.memory_info.return_value = mock_memory_info

        with patch('src.embodi.core.benchmarking.psutil.Process', return_value=mock_process):
            memory_mb = LatencyBenchmark._get_memory_usage_mb()

        assert memory_mb == 100.0, "Should convert bytes to MB"

    @patch('src.embodi.core.benchmarking.PSUTIL_AVAILABLE', False)
    def test_get_memory_usage_without_psutil(self):
        """Test that _get_memory_usage_mb returns 0 when psutil unavailable"""
        memory_mb = LatencyBenchmark._get_memory_usage_mb()

        assert memory_mb == 0.0, "Should return 0 when psutil unavailable"

    def test_print_summary_without_measurements(self, capsys):
        """Test that print_summary handles no measurements gracefully"""
        benchmark = LatencyBenchmark()

        benchmark.print_summary()

        captured = capsys.readouterr()
        assert "Cannot print summary" in captured.out, "Should show error message"

    def test_print_summary_with_measurements(self, capsys):
        """Test that print_summary displays results"""
        benchmark = LatencyBenchmark()
        benchmark._measurements = [
            LatencyMeasurement(1000.0, 1000.1, 100.0, 0.0, {}),
        ]

        benchmark.print_summary()

        captured = capsys.readouterr()
        assert "LATENCY BENCHMARK RESULTS" in captured.out, "Should show title"
        assert "100.000 ms" in captured.out, "Should show duration"


class TestBenchmarkFunction:
    """Test suite for benchmark_function helper"""

    def test_benchmarks_simple_function(self):
        """Test benchmarking a simple function"""
        def test_func():
            time.sleep(0.001)  # 1ms sleep

        result = benchmark_function(test_func, num_iterations=10, warmup_iterations=2)

        assert result.total_measurements == 10, "Should run specified iterations"
        assert result.mean_ms >= 1.0, "Should measure at least 1ms per call"

    def test_runs_warmup_iterations(self):
        """Test that warmup iterations are not measured"""
        call_count = []

        def test_func():
            call_count.append(1)

        result = benchmark_function(test_func, num_iterations=5, warmup_iterations=3)

        assert len(call_count) == 8, "Should call func warmup + measurement times"
        assert result.total_measurements == 5, "Should only measure non-warmup iterations"

    def test_attaches_iteration_labels(self):
        """Test that iteration labels are attached to measurements"""
        def test_func():
            pass

        result = benchmark_function(test_func, num_iterations=3, warmup_iterations=0)

        assert result.measurements[0].metadata["label"] == "iteration_0", "Should label first iteration"
        assert result.measurements[2].metadata["label"] == "iteration_2", "Should label last iteration"


class TestEMBODIOSBenchmarkRunner:
    """Test suite for EMBODIOSBenchmarkRunner class"""

    def test_init_without_engine(self):
        """Test initialization without inference engine"""
        runner = EMBODIOSBenchmarkRunner()

        assert runner.inference_engine is None, "Should allow None engine"
        assert isinstance(runner.benchmark, LatencyBenchmark), "Should create benchmark"

    def test_init_with_engine(self):
        """Test initialization with inference engine"""
        mock_engine = MagicMock()
        runner = EMBODIOSBenchmarkRunner(mock_engine)

        assert runner.inference_engine is mock_engine, "Should store engine"

    def test_run_inference_benchmark_with_mock_engine(self):
        """Test running benchmark with mock inference"""
        runner = EMBODIOSBenchmarkRunner()

        result = runner.run_inference_benchmark(num_requests=10, warmup_requests=2)

        assert result.total_measurements == 10, "Should run specified requests"
        assert runner.last_result is result, "Should store last result"

    def test_run_inference_benchmark_calls_engine(self):
        """Test that benchmark calls inference engine"""
        mock_engine = MagicMock()
        runner = EMBODIOSBenchmarkRunner(mock_engine)

        runner.run_inference_benchmark(
            input_tokens=[1, 2, 3],
            num_requests=5,
            warmup_requests=1
        )

        # Should call inference 6 times total (1 warmup + 5 measured)
        assert mock_engine.inference.call_count == 6, "Should call engine for each request"

    def test_run_inference_benchmark_with_metadata(self):
        """Test that metadata is attached to result"""
        runner = EMBODIOSBenchmarkRunner()

        result = runner.run_inference_benchmark(
            num_requests=5,
            warmup_requests=0,
            metadata={"model": "test-model", "version": "1.0"}
        )

        assert result.metadata["model"] == "test-model", "Should attach metadata"
        assert result.metadata["version"] == "1.0", "Should include all metadata"

    def test_run_inference_benchmark_clears_previous(self):
        """Test that running benchmark clears previous measurements"""
        runner = EMBODIOSBenchmarkRunner()

        runner.run_inference_benchmark(num_requests=5, warmup_requests=0)
        first_result = runner.last_result

        runner.run_inference_benchmark(num_requests=3, warmup_requests=0)
        second_result = runner.last_result

        assert second_result.total_measurements == 3, "Should have new measurement count"
        assert second_result is not first_result, "Should be new result object"

    def test_print_results_without_result(self, capsys):
        """Test print_results when no benchmark has run"""
        runner = EMBODIOSBenchmarkRunner()

        runner.print_results()

        captured = capsys.readouterr()
        assert "No benchmark results available" in captured.out, "Should show error"

    def test_print_results_with_last_result(self, capsys):
        """Test print_results uses last result by default"""
        runner = EMBODIOSBenchmarkRunner()
        runner.run_inference_benchmark(num_requests=5, warmup_requests=0)

        runner.print_results()

        captured = capsys.readouterr()
        assert "EMBODIOS INFERENCE BENCHMARK RESULTS" in captured.out, "Should show results"

    def test_compare_configurations(self):
        """Test comparing two configurations"""
        runner = EMBODIOSBenchmarkRunner()

        def config_a():
            time.sleep(0.001)  # Fast config

        def config_b():
            time.sleep(0.002)  # Slower config

        comparison = runner.compare_configurations(
            config_a,
            config_b,
            num_requests=10,
            warmup_requests=2,
            config_a_label="Fast",
            config_b_label="Slow"
        )

        assert "Fast" in comparison, "Should include first config"
        assert "Slow" in comparison, "Should include second config"
        assert "mean_overhead_pct" in comparison, "Should calculate overhead"
        assert comparison["mean_overhead_pct"] > 0, "Slower config should have positive overhead"

    def test_print_comparison(self, capsys):
        """Test printing comparison results"""
        runner = EMBODIOSBenchmarkRunner()

        def config_a():
            pass

        def config_b():
            pass

        comparison = runner.compare_configurations(
            config_a,
            config_b,
            num_requests=5,
            warmup_requests=0
        )

        runner.print_comparison(comparison)

        captured = capsys.readouterr()
        assert "BENCHMARK COMPARISON" in captured.out, "Should show comparison"


class TestComparisonResult:
    """Test suite for ComparisonResult dataclass"""

    def test_init_creates_comparison_result(self, sample_benchmark_result):
        """Test ComparisonResult initialization"""
        baseline = sample_benchmark_result
        test = sample_benchmark_result

        result = ComparisonResult(
            baseline_result=baseline,
            test_result=test,
            mean_overhead_ms=10.0,
            mean_overhead_pct=5.0,
            median_overhead_ms=8.0,
            median_overhead_pct=4.0,
            p95_overhead_ms=12.0,
            p95_overhead_pct=6.0,
            p99_overhead_ms=15.0,
            p99_overhead_pct=7.0,
            passes_threshold=True,
            threshold_pct=10.0,
            metadata={"test": "comparison"}
        )

        assert result.mean_overhead_pct == 5.0, "Should set overhead percentage"
        assert result.passes_threshold is True, "Should set threshold status"

    def test_to_dict_converts_nested_results(self, sample_benchmark_result):
        """Test that to_dict converts nested BenchmarkResults"""
        result = ComparisonResult(
            baseline_result=sample_benchmark_result,
            test_result=sample_benchmark_result,
            mean_overhead_ms=0.0,
            mean_overhead_pct=0.0,
            median_overhead_ms=0.0,
            median_overhead_pct=0.0,
            p95_overhead_ms=0.0,
            p95_overhead_pct=0.0,
            p99_overhead_ms=0.0,
            p99_overhead_pct=0.0,
            passes_threshold=True,
            threshold_pct=1.0
        )

        result_dict = result.to_dict()

        assert isinstance(result_dict["baseline_result"], dict), "Should convert baseline to dict"
        assert isinstance(result_dict["test_result"], dict), "Should convert test to dict"


class TestBenchmarkComparison:
    """Test suite for BenchmarkComparison class"""

    def test_init_stores_parameters(self, sample_benchmark_result):
        """Test that __init__ stores comparison parameters"""
        comparison = BenchmarkComparison(
            sample_benchmark_result,
            sample_benchmark_result,
            threshold_pct=2.0,
            baseline_label="Base",
            test_label="Test"
        )

        assert comparison.threshold_pct == 2.0, "Should store threshold"
        assert comparison.baseline_label == "Base", "Should store baseline label"
        assert comparison.test_label == "Test", "Should store test label"

    def test_compare_calculates_overhead(self):
        """Test that compare() calculates overhead correctly"""
        baseline = BenchmarkResult(
            total_measurements=10,
            total_duration_ms=1000.0,
            mean_ms=100.0,
            median_ms=95.0,
            min_ms=90.0,
            max_ms=110.0,
            stdev_ms=5.0,
            percentiles={"p50": 95.0, "p95": 108.0, "p99": 110.0, "p99.9": 110.0},
            throughput_per_sec=10.0
        )

        test = BenchmarkResult(
            total_measurements=10,
            total_duration_ms=1100.0,
            mean_ms=110.0,
            median_ms=105.0,
            min_ms=100.0,
            max_ms=120.0,
            stdev_ms=6.0,
            percentiles={"p50": 105.0, "p95": 118.0, "p99": 120.0, "p99.9": 120.0},
            throughput_per_sec=9.09
        )

        comparison = BenchmarkComparison(baseline, test, threshold_pct=15.0)
        result = comparison.compare()

        assert result.mean_overhead_ms == 10.0, "Should calculate mean overhead in ms"
        assert result.mean_overhead_pct == 10.0, "Should calculate mean overhead percentage"
        assert result.median_overhead_ms == 10.0, "Should calculate median overhead"
        assert result.p95_overhead_pct > 0, "Should calculate P95 overhead"

    def test_compare_determines_threshold_pass(self):
        """Test that compare() correctly determines threshold pass/fail"""
        baseline = BenchmarkResult(
            total_measurements=10,
            total_duration_ms=1000.0,
            mean_ms=100.0,
            median_ms=100.0,
            min_ms=100.0,
            max_ms=100.0,
            stdev_ms=0.0,
            percentiles={"p50": 100.0, "p95": 100.0, "p99": 100.0, "p99.9": 100.0},
            throughput_per_sec=10.0
        )

        # Test with 5% overhead (should pass 10% threshold)
        test_pass = BenchmarkResult(
            total_measurements=10,
            total_duration_ms=1050.0,
            mean_ms=105.0,
            median_ms=105.0,
            min_ms=105.0,
            max_ms=105.0,
            stdev_ms=0.0,
            percentiles={"p50": 105.0, "p95": 105.0, "p99": 105.0, "p99.9": 105.0},
            throughput_per_sec=9.52
        )

        comparison_pass = BenchmarkComparison(baseline, test_pass, threshold_pct=10.0)
        result_pass = comparison_pass.compare()

        assert result_pass.passes_threshold is True, "Should pass with 5% overhead vs 10% threshold"

        # Test with 15% overhead (should fail 10% threshold)
        test_fail = BenchmarkResult(
            total_measurements=10,
            total_duration_ms=1150.0,
            mean_ms=115.0,
            median_ms=115.0,
            min_ms=115.0,
            max_ms=115.0,
            stdev_ms=0.0,
            percentiles={"p50": 115.0, "p95": 115.0, "p99": 115.0, "p99.9": 115.0},
            throughput_per_sec=8.7
        )

        comparison_fail = BenchmarkComparison(baseline, test_fail, threshold_pct=10.0)
        result_fail = comparison_fail.compare()

        assert result_fail.passes_threshold is False, "Should fail with 15% overhead vs 10% threshold"

    def test_print_comparison_displays_results(self, capsys, sample_benchmark_result):
        """Test that print_comparison displays comparison results"""
        comparison = BenchmarkComparison(
            sample_benchmark_result,
            sample_benchmark_result,
            baseline_label="Baseline",
            test_label="Test"
        )

        comparison.print_comparison()

        captured = capsys.readouterr()
        assert "BENCHMARK COMPARISON RESULTS" in captured.out, "Should show title"
        assert "Baseline" in captured.out, "Should show baseline label"
        assert "Test" in captured.out, "Should show test label"

    def test_to_json_returns_valid_json(self, sample_benchmark_result):
        """Test that to_json returns valid JSON string"""
        comparison = BenchmarkComparison(
            sample_benchmark_result,
            sample_benchmark_result
        )

        json_str = comparison.to_json()

        # Should be valid JSON
        parsed = json.loads(json_str)
        assert isinstance(parsed, dict), "Should parse as dictionary"
        assert "baseline_result" in parsed, "Should include baseline"
        assert "test_result" in parsed, "Should include test"
        assert "mean_overhead_pct" in parsed, "Should include overhead"

    def test_save_json_writes_file(self, sample_benchmark_result, tmp_path):
        """Test that save_json writes JSON file"""
        comparison = BenchmarkComparison(
            sample_benchmark_result,
            sample_benchmark_result
        )

        filepath = tmp_path / "comparison.json"
        comparison.save_json(str(filepath))

        assert filepath.exists(), "Should create file"

        # Verify file contents
        with open(filepath, 'r') as f:
            data = json.load(f)

        assert "baseline_result" in data, "Should save baseline"
        assert "test_result" in data, "Should save test"

    def test_json_output_format(self, sample_benchmark_result):
        """Test that JSON output format matches specification"""
        # Create comparison with metadata
        baseline = BenchmarkResult(
            total_measurements=1000,
            total_duration_ms=102261.45,
            measurements=[],
            mean_ms=102.261,
            median_ms=102.225,
            min_ms=100.012,
            max_ms=105.892,
            stdev_ms=1.471,
            percentiles={"p50": 102.225, "p95": 102.626, "p99": 103.308, "p99.9": 104.156},
            throughput_per_sec=9.779,
            peak_memory_mb=1024.5,
            avg_memory_mb=1018.3,
            metadata={"configuration": "llama.cpp baseline", "model": "test.gguf"}
        )

        test = BenchmarkResult(
            total_measurements=1000,
            total_duration_ms=102773.82,
            measurements=[],
            mean_ms=102.773,
            median_ms=102.696,
            min_ms=100.234,
            max_ms=107.421,
            stdev_ms=0.866,
            percentiles={"p50": 102.696, "p95": 103.289, "p99": 105.330, "p99.9": 106.234},
            throughput_per_sec=9.730,
            peak_memory_mb=1026.8,
            avg_memory_mb=1019.1,
            metadata={"configuration": "EMBODIOS", "model": "test.gguf"}
        )

        comparison = BenchmarkComparison(
            baseline,
            test,
            threshold_pct=1.0,
            baseline_label="llama.cpp",
            test_label="EMBODIOS"
        )

        # Get JSON output
        json_str = comparison.to_json()
        data = json.loads(json_str)

        # Verify top-level structure
        assert isinstance(data, dict), "Should be a dictionary"

        # Verify required top-level fields
        required_fields = [
            "baseline_result", "test_result",
            "mean_overhead_ms", "mean_overhead_pct",
            "median_overhead_ms", "median_overhead_pct",
            "p95_overhead_ms", "p95_overhead_pct",
            "p99_overhead_ms", "p99_overhead_pct",
            "passes_threshold", "threshold_pct",
            "metadata"
        ]
        for field in required_fields:
            assert field in data, f"Should include {field} in JSON output"

        # Verify baseline_result structure
        baseline_result = data["baseline_result"]
        assert isinstance(baseline_result, dict), "baseline_result should be a dictionary"

        # Verify BenchmarkResult required fields
        result_fields = [
            "total_measurements", "total_duration_ms", "measurements",
            "mean_ms", "median_ms", "min_ms", "max_ms", "stdev_ms",
            "percentiles", "throughput_per_sec",
            "peak_memory_mb", "avg_memory_mb", "metadata"
        ]
        for field in result_fields:
            assert field in baseline_result, f"baseline_result should include {field}"

        # Verify test_result structure
        test_result = data["test_result"]
        assert isinstance(test_result, dict), "test_result should be a dictionary"
        for field in result_fields:
            assert field in test_result, f"test_result should include {field}"

        # Verify percentiles structure
        assert isinstance(baseline_result["percentiles"], dict), "percentiles should be a dictionary"
        percentile_keys = ["p50", "p95", "p99", "p99.9"]
        for key in percentile_keys:
            assert key in baseline_result["percentiles"], f"percentiles should include {key}"
            assert isinstance(baseline_result["percentiles"][key], (int, float)), f"{key} should be numeric"

        # Verify data types at top level
        assert isinstance(data["mean_overhead_ms"], (int, float)), "mean_overhead_ms should be numeric"
        assert isinstance(data["mean_overhead_pct"], (int, float)), "mean_overhead_pct should be numeric"
        assert isinstance(data["passes_threshold"], bool), "passes_threshold should be boolean"
        assert isinstance(data["metadata"], dict), "metadata should be dictionary"

        # Verify data types in BenchmarkResults
        assert isinstance(baseline_result["total_measurements"], int), "total_measurements should be int"
        assert isinstance(baseline_result["mean_ms"], (int, float)), "mean_ms should be numeric"
        assert isinstance(test_result["total_measurements"], int), "total_measurements should be int"
        assert isinstance(test_result["mean_ms"], (int, float)), "mean_ms should be numeric"

        # Verify overhead calculations are present and reasonable
        assert data["mean_overhead_ms"] >= 0, "mean_overhead_ms should be non-negative"
        assert data["median_overhead_ms"] >= 0, "median_overhead_ms should be non-negative"

        # Verify metadata structure
        assert "baseline_label" in data["metadata"], "metadata should include baseline_label"
        assert "test_label" in data["metadata"], "metadata should include test_label"
        assert data["metadata"]["baseline_label"] == "llama.cpp", "Should preserve baseline label"
        assert data["metadata"]["test_label"] == "EMBODIOS", "Should preserve test label"

        # Verify measurements list is present (even if empty)
        assert isinstance(baseline_result["measurements"], list), "measurements should be a list"
        assert isinstance(test_result["measurements"], list), "measurements should be a list"

        # Verify JSON is re-parseable (no serialization issues)
        json_str_2 = json.dumps(data)
        data_2 = json.loads(json_str_2)
        assert data == data_2, "JSON should be re-parseable without data loss"


class TestLlamaCppBenchmarkRunner:
    """Test suite for LlamaCppBenchmarkRunner class"""

    def test_init_stores_parameters(self):
        """Test that __init__ stores runner parameters"""
        runner = LlamaCppBenchmarkRunner(
            binary_path="/usr/bin/llama-cli",
            model_path="/models/model.gguf",
            timeout_seconds=600
        )

        assert runner.binary_path == "/usr/bin/llama-cli", "Should store binary path"
        assert runner.model_path == "/models/model.gguf", "Should store model path"
        assert runner.timeout_seconds == 600, "Should store timeout"

    def test_run_benchmark_builds_correct_command(self):
        """Test that run_benchmark builds correct command"""
        runner = LlamaCppBenchmarkRunner(
            binary_path="/usr/bin/llama-cli",
            model_path="/models/model.gguf"
        )

        mock_process = MagicMock()
        mock_process.stdout = iter([])  # Empty stdout
        mock_process.wait.return_value = 0
        mock_process.stderr.read.return_value = ""

        with patch('subprocess.Popen', return_value=mock_process) as mock_popen:
            runner.run_benchmark(
                prompt="Test prompt",
                n_predict=64,
                n_threads=4
            )

            # Verify command arguments
            call_args = mock_popen.call_args[0][0]
            assert "/usr/bin/llama-cli" in call_args, "Should include binary path"
            assert "-m" in call_args, "Should include model flag"
            assert "/models/model.gguf" in call_args, "Should include model path"
            assert "-p" in call_args, "Should include prompt flag"
            assert "Test prompt" in call_args, "Should include prompt"
            assert "-n" in call_args, "Should include n_predict flag"
            assert "64" in call_args, "Should include n_predict value"
            assert "-t" in call_args, "Should include threads flag"
            assert "4" in call_args, "Should include threads value"

    def test_run_benchmark_captures_output(self):
        """Test that run_benchmark captures stdout and stderr"""
        runner = LlamaCppBenchmarkRunner(
            binary_path="/usr/bin/llama-cli",
            model_path="/models/model.gguf"
        )

        mock_process = MagicMock()
        mock_process.stdout = iter(["line 1", "line 2"])
        mock_process.wait.return_value = 0
        mock_process.stderr.read.return_value = "error output"

        with patch('subprocess.Popen', return_value=mock_process):
            result = runner.run_benchmark()

        assert "line 1" in result["stdout"], "Should capture stdout"
        assert "line 2" in result["stdout"], "Should capture all lines"
        assert result["stderr"] == "error output", "Should capture stderr"

    def test_run_benchmark_parses_metrics(self):
        """Test that run_benchmark parses llama.cpp metrics"""
        runner = LlamaCppBenchmarkRunner(
            binary_path="/usr/bin/llama-cli",
            model_path="/models/model.gguf"
        )

        mock_process = MagicMock()
        mock_process.stdout = iter([
            "llama_print_timings: load time = 100.0 ms",
            "llama_print_timings: eval time = 500.0 ms / 10 runs"
        ])
        mock_process.wait.return_value = 0
        mock_process.stderr.read.return_value = ""

        with patch('subprocess.Popen', return_value=mock_process):
            result = runner.run_benchmark()

        assert result["load_time_ms"] == 100.0, "Should parse load time"
        assert result["eval_time_ms"] == 500.0, "Should parse eval time"

    def test_run_benchmark_handles_timeout(self):
        """Test that run_benchmark handles timeout"""
        runner = LlamaCppBenchmarkRunner(
            binary_path="/usr/bin/llama-cli",
            model_path="/models/model.gguf",
            timeout_seconds=1
        )

        mock_process = MagicMock()
        mock_process.stdout = iter([])
        mock_process.wait.side_effect = subprocess.TimeoutExpired("cmd", 1)

        with patch('subprocess.Popen', return_value=mock_process):
            with pytest.raises(subprocess.TimeoutExpired):
                runner.run_benchmark()

    def test_run_benchmark_handles_file_not_found(self):
        """Test that run_benchmark raises FileNotFoundError for missing binary"""
        runner = LlamaCppBenchmarkRunner(
            binary_path="/nonexistent/llama-cli",
            model_path="/models/model.gguf"
        )

        with patch('subprocess.Popen', side_effect=FileNotFoundError()):
            with pytest.raises(FileNotFoundError, match="llama.cpp binary not found"):
                runner.run_benchmark()

    def test_is_running_returns_correct_status(self):
        """Test that is_running returns correct process status"""
        runner = LlamaCppBenchmarkRunner(
            binary_path="/usr/bin/llama-cli",
            model_path="/models/model.gguf"
        )

        assert runner.is_running() is False, "Should return False when no process"

        mock_process = MagicMock()
        mock_process.poll.return_value = None  # Still running
        runner._process = mock_process

        assert runner.is_running() is True, "Should return True when running"

        mock_process.poll.return_value = 0  # Finished

        assert runner.is_running() is False, "Should return False when finished"

    def test_stop_terminates_process(self):
        """Test that stop() terminates running process"""
        runner = LlamaCppBenchmarkRunner(
            binary_path="/usr/bin/llama-cli",
            model_path="/models/model.gguf"
        )

        mock_process = MagicMock()
        runner._process = mock_process

        runner.stop()

        mock_process.terminate.assert_called_once(), "Should call terminate"

    def test_get_last_output_returns_captured_lines(self):
        """Test that get_last_output returns captured stdout"""
        runner = LlamaCppBenchmarkRunner(
            binary_path="/usr/bin/llama-cli",
            model_path="/models/model.gguf"
        )

        runner._output_lines = ["line 1", "line 2", "line 3"]
        output = runner.get_last_output()

        assert output == ["line 1", "line 2", "line 3"], "Should return output lines"
        assert output is not runner._output_lines, "Should return copy"

    def test_get_last_errors_returns_captured_stderr(self):
        """Test that get_last_errors returns captured stderr"""
        runner = LlamaCppBenchmarkRunner(
            binary_path="/usr/bin/llama-cli",
            model_path="/models/model.gguf"
        )

        runner._error_lines = ["error 1", "error 2"]
        errors = runner.get_last_errors()

        assert errors == ["error 1", "error 2"], "Should return error lines"


class TestFormatBenchmarkTable:
    """Test suite for format_benchmark_table function"""

    @patch('src.embodi.core.benchmarking.RICH_AVAILABLE', True)
    def test_returns_table_when_rich_available(self, sample_benchmark_result):
        """Test that format_benchmark_table returns Table when rich available"""
        with patch('src.embodi.core.benchmarking.Table') as mock_table:
            mock_table_instance = MagicMock()
            mock_table.return_value = mock_table_instance

            result = format_benchmark_table(sample_benchmark_result)

            assert result is not None, "Should return table"

    @patch('src.embodi.core.benchmarking.RICH_AVAILABLE', False)
    def test_returns_none_when_rich_unavailable(self, sample_benchmark_result):
        """Test that format_benchmark_table returns None when rich unavailable"""
        result = format_benchmark_table(sample_benchmark_result)

        assert result is None, "Should return None without rich"


class TestFormatComparisonTable:
    """Test suite for format_comparison_table function"""

    @patch('src.embodi.core.benchmarking.RICH_AVAILABLE', True)
    def test_returns_table_when_rich_available(self, sample_benchmark_result):
        """Test that format_comparison_table returns Table when rich available"""
        comparison_result = ComparisonResult(
            baseline_result=sample_benchmark_result,
            test_result=sample_benchmark_result,
            mean_overhead_ms=0.0,
            mean_overhead_pct=0.0,
            median_overhead_ms=0.0,
            median_overhead_pct=0.0,
            p95_overhead_ms=0.0,
            p95_overhead_pct=0.0,
            p99_overhead_ms=0.0,
            p99_overhead_pct=0.0,
            passes_threshold=True,
            threshold_pct=1.0,
            metadata={"baseline_label": "Base", "test_label": "Test"}
        )

        with patch('src.embodi.core.benchmarking.Table') as mock_table:
            mock_table_instance = MagicMock()
            mock_table.return_value = mock_table_instance

            result = format_comparison_table(comparison_result)

            assert result is not None, "Should return table"

    @patch('src.embodi.core.benchmarking.RICH_AVAILABLE', False)
    def test_returns_none_when_rich_unavailable(self, sample_benchmark_result):
        """Test that format_comparison_table returns None when rich unavailable"""
        comparison_result = ComparisonResult(
            baseline_result=sample_benchmark_result,
            test_result=sample_benchmark_result,
            mean_overhead_ms=0.0,
            mean_overhead_pct=0.0,
            median_overhead_ms=0.0,
            median_overhead_pct=0.0,
            p95_overhead_ms=0.0,
            p95_overhead_pct=0.0,
            p99_overhead_ms=0.0,
            p99_overhead_pct=0.0,
            passes_threshold=True,
            threshold_pct=1.0
        )

        result = format_comparison_table(comparison_result)

        assert result is None, "Should return None without rich"


def test_benchmark_cli_e2e(tmp_path, sample_benchmark_result):
    """Test end-to-end CLI execution with mock data"""
    from click.testing import CliRunner
    from src.embodi.cli.benchmark import benchmark

    # Create a temporary model file for testing
    model_file = tmp_path / "test-model.aios"
    model_file.write_text("mock model content")

    # Create output file path
    output_file = tmp_path / "results.json"

    # Mock the EMBODIOSBenchmarkRunner
    with patch('src.embodi.cli.benchmark.EMBODIOSBenchmarkRunner') as mock_runner_class:
        # Setup mock runner instance
        mock_runner = MagicMock()
        mock_runner.run_inference_benchmark.return_value = sample_benchmark_result
        mock_runner_class.return_value = mock_runner

        # Mock print_results_rich to avoid rendering issues
        mock_runner.print_results_rich = MagicMock()

        # Run CLI command
        runner = CliRunner()
        result = runner.invoke(benchmark, [
            str(model_file),
            '--prompts', '10',
            '--warmup', '2',
            '--output', str(output_file),
            '--format', 'json'
        ])

        # Verify CLI executed successfully
        assert result.exit_code == 0, f"CLI should exit successfully, got: {result.output}"

        # Verify runner was initialized
        mock_runner_class.assert_called_once(), "Should initialize benchmark runner"

        # Verify benchmark was run with correct parameters
        mock_runner.run_inference_benchmark.assert_called_once()
        call_kwargs = mock_runner.run_inference_benchmark.call_args[1]
        assert call_kwargs['num_requests'] == 10, "Should pass correct number of requests"
        assert call_kwargs['warmup_requests'] == 2, "Should pass correct warmup count"
        assert 'metadata' in call_kwargs, "Should include metadata"

        # Verify output file was created
        assert output_file.exists(), "Should create output JSON file"

        # Verify output file contains expected data
        with open(output_file, 'r') as f:
            output_data = json.load(f)

        assert 'embodios_result' in output_data, "Should include EMBODIOS result"
        assert 'config' in output_data, "Should include configuration"
        assert output_data['config']['prompts'] == 10, "Should save prompts config"
        assert output_data['config']['warmup'] == 2, "Should save warmup config"
        assert output_data['embodios_result']['total_measurements'] == 3, \
            "Should include result measurements"
