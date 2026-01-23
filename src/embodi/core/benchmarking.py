#!/usr/bin/env python3
"""
EMBODIOS Core Benchmarking Module

Provides comprehensive latency measurement and performance benchmarking utilities
for EMBODIOS inference engine. Supports detailed latency tracking, throughput
measurement, and comparative analysis against baseline implementations.

This module follows patterns from:
- benchmark_metrics.py: Statistical analysis and timing patterns
- kernel/ai/benchmark.c: TSC-style timing and result structures
"""

import re
import time
import statistics
import subprocess
import threading
from typing import List, Dict, Optional, Any, Callable, Union
from dataclasses import dataclass, field, asdict
from contextlib import contextmanager

try:
    import psutil
    PSUTIL_AVAILABLE = True
except ImportError:
    PSUTIL_AVAILABLE = False

try:
    from rich.console import Console
    from rich.table import Table
    RICH_AVAILABLE = True
except ImportError:
    RICH_AVAILABLE = False


@dataclass
class LatencyMeasurement:
    """
    Single latency measurement result.

    Attributes:
        start_time: Unix timestamp when measurement started
        end_time: Unix timestamp when measurement ended
        duration_ms: Duration in milliseconds
        memory_used_mb: Memory used during measurement in MB (if available)
        metadata: Optional metadata about the measurement
    """
    start_time: float
    end_time: float
    duration_ms: float
    memory_used_mb: float = 0.0
    metadata: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        """Convert measurement to dictionary"""
        return asdict(self)


@dataclass
class BenchmarkResult:
    """
    Aggregated benchmark results with statistical analysis.

    Attributes:
        total_measurements: Total number of measurements taken
        total_duration_ms: Total duration across all measurements
        measurements: List of individual latency measurements
        mean_ms: Mean latency in milliseconds
        median_ms: Median latency in milliseconds
        min_ms: Minimum latency in milliseconds
        max_ms: Maximum latency in milliseconds
        stdev_ms: Standard deviation in milliseconds
        percentiles: Dictionary of percentile values (p50, p95, p99, p99.9)
        throughput_per_sec: Throughput in items per second
        peak_memory_mb: Peak memory usage in MB (if available)
        avg_memory_mb: Average memory usage in MB (if available)
        metadata: Optional metadata about the benchmark run
    """
    total_measurements: int
    total_duration_ms: float
    measurements: List[LatencyMeasurement] = field(default_factory=list)
    mean_ms: float = 0.0
    median_ms: float = 0.0
    min_ms: float = 0.0
    max_ms: float = 0.0
    stdev_ms: float = 0.0
    percentiles: Dict[str, float] = field(default_factory=dict)
    throughput_per_sec: float = 0.0
    peak_memory_mb: float = 0.0
    avg_memory_mb: float = 0.0
    metadata: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        """Convert result to dictionary"""
        result = asdict(self)
        # Convert measurements to dicts for JSON serialization
        result['measurements'] = [m.to_dict() for m in self.measurements]
        return result


def calculate_percentiles(values: List[float]) -> Dict[str, float]:
    """
    Calculate latency percentiles (P50, P95, P99, P99.9) from a list of values.

    Follows the statistical pattern from benchmark_metrics.py calculate_stats().
    Percentiles are calculated using index-based approach on sorted values.

    Args:
        values: List of numerical values (e.g., latencies in milliseconds)

    Returns:
        Dictionary with percentile values:
            - p50: 50th percentile (median)
            - p95: 95th percentile
            - p99: 99th percentile
            - p99.9: 99.9th percentile

    Raises:
        ValueError: If values list is empty

    Example:
        >>> latencies = [1.0, 2.0, 3.0, 4.0, 5.0]
        >>> percentiles = calculate_percentiles(latencies)
        >>> print(f"P50: {percentiles['p50']:.2f} ms")
    """
    if not values:
        raise ValueError("Cannot calculate percentiles for empty list")

    # Sort values once for efficient percentile calculation
    sorted_values = sorted(values)
    n = len(sorted_values)

    # Calculate percentile indices (following benchmark_metrics.py pattern)
    # Using int() truncation matches the reference implementation
    percentiles = {
        "p50": sorted_values[int(n * 0.50)],
        "p95": sorted_values[int(n * 0.95)],
        "p99": sorted_values[int(n * 0.99)],
        "p99.9": sorted_values[int(n * 0.999)] if n >= 1000 else sorted_values[-1],
    }

    return percentiles


def parse_llamacpp_output(output: Union[str, List[str]]) -> Dict[str, Any]:
    """
    Parse llama.cpp output to extract performance metrics.

    Supports both classic llama_print_timings format and newer llama_perf format.
    Extracts timing information, token counts, and calculates derived metrics
    like tokens per second and ms per token.

    Classic format example:
        llama_print_timings: load time = 134.84 ms
        llama_print_timings: prompt eval time = 925.78 ms / 24 tokens ( 38.57 ms per token)
        llama_print_timings: eval time = 4178.32 ms / 31 runs ( 134.78 ms per token)

    Newer format example:
        llama_perf_context_print: load time = 318.81 ms
        llama_perf_context_print: prompt eval time = 59.26 ms / 5 tokens ( 11.85 ms per token, 84.38 tokens per second)
        llama_perf_context_print: eval time = 17797.98 ms / 379 runs ( 46.96 ms per token, 21.29 tokens per second)

    Args:
        output: Either a single string or list of strings containing llama.cpp output

    Returns:
        Dictionary containing extracted metrics:
            - load_time_ms: Model load time in milliseconds
            - prompt_eval_time_ms: Prompt processing time in milliseconds
            - prompt_tokens: Number of prompt tokens processed
            - prompt_ms_per_token: Milliseconds per prompt token
            - prompt_tokens_per_second: Prompt tokens per second
            - eval_time_ms: Generation/eval time in milliseconds
            - eval_tokens: Number of tokens generated
            - eval_ms_per_token: Milliseconds per generated token
            - eval_tokens_per_second: Generated tokens per second
            - sample_time_ms: Sampling time in milliseconds (newer format)
            - sample_tokens: Number of sampling runs (newer format)
            - total_time_ms: Total execution time in milliseconds
            - total_tokens: Total number of tokens processed

    Example:
        >>> output = "llama_print_timings: eval time = 4178.32 ms / 31 runs"
        >>> metrics = parse_llamacpp_output(output)
        >>> print(f"Eval time: {metrics.get('eval_time_ms', 0):.2f} ms")
    """
    # Convert single string to list of lines
    if isinstance(output, str):
        lines = output.splitlines()
    else:
        lines = output

    metrics: Dict[str, Any] = {}

    # Regular expression patterns for parsing metrics
    # Pattern for "time = X.XX ms"
    time_pattern = re.compile(r'time\s*=\s*([\d.]+)\s*ms')
    # Pattern for "X.XX ms / Y tokens" or "X.XX ms / Y runs"
    tokens_pattern = re.compile(r'([\d.]+)\s*ms\s*/\s*(\d+)\s*(tokens|runs)')
    # Pattern for "(X.XX ms per token)"
    per_token_pattern = re.compile(r'\(\s*([\d.]+)\s*ms\s*per\s*token')
    # Pattern for "X.XX tokens per second"
    tps_pattern = re.compile(r'([\d.]+)\s*tokens\s*per\s*second')

    for line in lines:
        line_lower = line.lower()

        # Parse load time
        if 'load time' in line_lower:
            match = time_pattern.search(line)
            if match:
                metrics['load_time_ms'] = float(match.group(1))

        # Parse prompt eval time (prompt processing)
        elif 'prompt eval time' in line_lower or 'prompt_eval_time' in line_lower:
            # Extract time value
            match = time_pattern.search(line)
            if match:
                metrics['prompt_eval_time_ms'] = float(match.group(1))

            # Extract token count
            tokens_match = tokens_pattern.search(line)
            if tokens_match:
                metrics['prompt_tokens'] = int(tokens_match.group(2))

            # Extract ms per token
            per_token_match = per_token_pattern.search(line)
            if per_token_match:
                metrics['prompt_ms_per_token'] = float(per_token_match.group(1))

            # Extract tokens per second (newer format)
            tps_match = tps_pattern.search(line)
            if tps_match:
                metrics['prompt_tokens_per_second'] = float(tps_match.group(1))

        # Parse eval time (generation/decode)
        elif ('eval time' in line_lower or 'eval_time' in line_lower) and 'prompt' not in line_lower:
            # Extract time value
            match = time_pattern.search(line)
            if match:
                metrics['eval_time_ms'] = float(match.group(1))

            # Extract token/run count
            tokens_match = tokens_pattern.search(line)
            if tokens_match:
                metrics['eval_tokens'] = int(tokens_match.group(2))

            # Extract ms per token
            per_token_match = per_token_pattern.search(line)
            if per_token_match:
                metrics['eval_ms_per_token'] = float(per_token_match.group(1))

            # Extract tokens per second (newer format)
            tps_match = tps_pattern.search(line)
            if tps_match:
                metrics['eval_tokens_per_second'] = float(tps_match.group(1))

        # Parse sample time (newer format)
        elif 'sample time' in line_lower or 'sampling time' in line_lower:
            # Extract time value
            match = time_pattern.search(line)
            if match:
                metrics['sample_time_ms'] = float(match.group(1))

            # Extract run/token count
            tokens_match = tokens_pattern.search(line)
            if tokens_match:
                metrics['sample_tokens'] = int(tokens_match.group(2))

        # Parse total time
        elif 'total time' in line_lower:
            # Extract time value
            match = time_pattern.search(line)
            if match:
                metrics['total_time_ms'] = float(match.group(1))

            # Extract total token count if present
            tokens_match = re.search(r'/\s*(\d+)\s*tokens', line)
            if tokens_match:
                metrics['total_tokens'] = int(tokens_match.group(1))

    # Calculate derived metrics if not already present
    if 'prompt_eval_time_ms' in metrics and 'prompt_tokens' in metrics:
        if metrics['prompt_tokens'] > 0:
            if 'prompt_ms_per_token' not in metrics:
                metrics['prompt_ms_per_token'] = metrics['prompt_eval_time_ms'] / metrics['prompt_tokens']
            if 'prompt_tokens_per_second' not in metrics:
                metrics['prompt_tokens_per_second'] = (metrics['prompt_tokens'] * 1000.0) / metrics['prompt_eval_time_ms']

    if 'eval_time_ms' in metrics and 'eval_tokens' in metrics:
        if metrics['eval_tokens'] > 0:
            if 'eval_ms_per_token' not in metrics:
                metrics['eval_ms_per_token'] = metrics['eval_time_ms'] / metrics['eval_tokens']
            if 'eval_tokens_per_second' not in metrics:
                metrics['eval_tokens_per_second'] = (metrics['eval_tokens'] * 1000.0) / metrics['eval_time_ms']

    # Calculate total tokens if not present
    if 'total_tokens' not in metrics:
        prompt_tokens = metrics.get('prompt_tokens', 0)
        eval_tokens = metrics.get('eval_tokens', 0)
        if prompt_tokens > 0 or eval_tokens > 0:
            metrics['total_tokens'] = prompt_tokens + eval_tokens

    return metrics


class LatencyBenchmark:
    """
    Core latency benchmarking utility for measuring inference performance.

    Provides high-precision timing measurement and statistical analysis
    following patterns from benchmark_metrics.py and kernel/ai/benchmark.c.

    Example:
        >>> benchmark = LatencyBenchmark()
        >>> with benchmark.measure("inference"):
        ...     # Run inference
        ...     pass
        >>> result = benchmark.get_result()
        >>> print(f"Mean latency: {result.mean_ms:.3f} ms")
    """

    def __init__(self):
        """Initialize benchmark with empty measurement list"""
        self._measurements: List[LatencyMeasurement] = []
        self._current_start: Optional[float] = None
        self._start_memory_mb: Optional[float] = None

    @staticmethod
    def _get_memory_usage_mb() -> float:
        """
        Get current process memory usage in MB.

        Returns:
            Memory usage in MB, or 0.0 if psutil is not available

        Pattern follows kernel/ai/benchmark.c memory tracking approach.
        """
        if not PSUTIL_AVAILABLE:
            return 0.0

        try:
            process = psutil.Process()
            # Use RSS (Resident Set Size) for actual physical memory usage
            return process.memory_info().rss / (1024 * 1024)  # Convert bytes to MB
        except Exception:
            return 0.0

    def start_measurement(self) -> None:
        """
        Start a latency measurement.

        Uses time.time() for high-precision timing following benchmark_metrics.py pattern.
        Captures initial memory state if psutil is available.
        """
        self._current_start = time.time()
        self._start_memory_mb = self._get_memory_usage_mb()

    def end_measurement(self, metadata: Optional[Dict[str, Any]] = None) -> LatencyMeasurement:
        """
        End current measurement and record result.

        Args:
            metadata: Optional metadata to attach to measurement

        Returns:
            LatencyMeasurement object with timing and memory data

        Raises:
            RuntimeError: If no measurement was started
        """
        if self._current_start is None:
            raise RuntimeError("No measurement in progress. Call start_measurement() first.")

        end_time = time.time()
        duration_ms = (end_time - self._current_start) * 1000.0  # Convert to milliseconds

        # Calculate memory usage (follows kernel/ai/benchmark.c pattern)
        end_memory_mb = self._get_memory_usage_mb()
        memory_used_mb = 0.0
        if self._start_memory_mb is not None and end_memory_mb > 0:
            memory_used_mb = end_memory_mb - self._start_memory_mb

        measurement = LatencyMeasurement(
            start_time=self._current_start,
            end_time=end_time,
            duration_ms=duration_ms,
            memory_used_mb=memory_used_mb,
            metadata=metadata or {}
        )

        self._measurements.append(measurement)
        self._current_start = None
        self._start_memory_mb = None

        return measurement

    @contextmanager
    def measure(self, label: Optional[str] = None):
        """
        Context manager for measuring latency of a code block.

        Args:
            label: Optional label to attach to measurement metadata

        Example:
            >>> benchmark = LatencyBenchmark()
            >>> with benchmark.measure("test_operation"):
            ...     time.sleep(0.1)
            >>> result = benchmark.get_result()
        """
        metadata = {"label": label} if label else {}
        self.start_measurement()
        try:
            yield
        finally:
            self.end_measurement(metadata)

    def get_measurements(self) -> List[LatencyMeasurement]:
        """Get all recorded measurements"""
        return self._measurements.copy()

    def clear_measurements(self) -> None:
        """Clear all recorded measurements"""
        self._measurements.clear()
        self._current_start = None
        self._start_memory_mb = None

    def get_result(self) -> BenchmarkResult:
        """
        Calculate aggregated benchmark results with statistical analysis.

        Follows statistical patterns from benchmark_metrics.py calculate_stats()
        and throughput calculation from kernel/ai/benchmark.c.

        Returns:
            BenchmarkResult with aggregated statistics including throughput and memory

        Raises:
            ValueError: If no measurements have been recorded
        """
        if not self._measurements:
            raise ValueError("No measurements recorded. Use start_measurement()/end_measurement() or measure() context manager.")

        durations = [m.duration_ms for m in self._measurements]
        total_duration = sum(durations)

        # Calculate basic statistics
        mean = statistics.mean(durations)
        median = statistics.median(durations)
        min_val = min(durations)
        max_val = max(durations)
        stdev = statistics.stdev(durations) if len(durations) > 1 else 0.0

        # Calculate percentiles using dedicated function
        percentiles = calculate_percentiles(durations)

        # Calculate throughput (items per second)
        # Following pattern from benchmark_metrics.py and kernel/ai/benchmark.c
        # throughput = (total_items * 1000) / total_duration_ms
        throughput_per_sec = 0.0
        if total_duration > 0:
            throughput_per_sec = (len(self._measurements) * 1000.0) / total_duration

        # Calculate memory statistics (following kernel/ai/benchmark.c pattern)
        peak_memory_mb = 0.0
        avg_memory_mb = 0.0
        if PSUTIL_AVAILABLE:
            memory_values = [m.memory_used_mb for m in self._measurements]
            # Filter out zero values (measurements without memory tracking)
            non_zero_memory = [m for m in memory_values if m != 0.0]
            if non_zero_memory:
                peak_memory_mb = max(non_zero_memory)
                avg_memory_mb = statistics.mean(non_zero_memory)

        return BenchmarkResult(
            total_measurements=len(self._measurements),
            total_duration_ms=total_duration,
            measurements=self._measurements.copy(),
            mean_ms=mean,
            median_ms=median,
            min_ms=min_val,
            max_ms=max_val,
            stdev_ms=stdev,
            percentiles=percentiles,
            throughput_per_sec=throughput_per_sec,
            peak_memory_mb=peak_memory_mb,
            avg_memory_mb=avg_memory_mb,
            metadata={}
        )

    def print_summary(self) -> None:
        """
        Print human-readable summary of benchmark results.

        Follows reporting pattern from benchmark_metrics.py and kernel/ai/benchmark.c.
        """
        try:
            result = self.get_result()
        except ValueError as e:
            print(f"Cannot print summary: {e}")
            return

        print("=" * 70)
        print("LATENCY BENCHMARK RESULTS")
        print("=" * 70)
        print(f"Total measurements: {result.total_measurements}")
        print(f"Total duration:     {result.total_duration_ms:.3f} ms")
        print()
        print("Latency Statistics:")
        print(f"  Mean:   {result.mean_ms:.3f} ms")
        print(f"  Median: {result.median_ms:.3f} ms")
        print(f"  Min:    {result.min_ms:.3f} ms")
        print(f"  Max:    {result.max_ms:.3f} ms")
        print(f"  Stdev:  {result.stdev_ms:.3f} ms")
        print()
        print("Percentiles:")
        print(f"  P50:    {result.percentiles['p50']:.3f} ms")
        print(f"  P95:    {result.percentiles['p95']:.3f} ms")
        print(f"  P99:    {result.percentiles['p99']:.3f} ms")
        print(f"  P99.9:  {result.percentiles['p99.9']:.3f} ms")
        print()
        print("Throughput:")
        print(f"  {result.throughput_per_sec:.2f} items/sec")
        if PSUTIL_AVAILABLE and result.peak_memory_mb > 0:
            print()
            print("Memory Usage:")
            print(f"  Peak:    {result.peak_memory_mb:.2f} MB")
            print(f"  Average: {result.avg_memory_mb:.2f} MB")
        print("=" * 70)

    def print_summary_rich(self, console: Optional[Any] = None) -> None:
        """
        Print human-readable summary using rich tables.

        Follows pattern from cli/main.py for rich table formatting.
        Falls back to plain text if rich is not available.

        Args:
            console: Optional rich Console instance. If None, creates new console.

        Example:
            >>> benchmark = LatencyBenchmark()
            >>> with benchmark.measure():
            ...     time.sleep(0.1)
            >>> benchmark.print_summary_rich()
        """
        if not RICH_AVAILABLE:
            # Fallback to plain text
            self.print_summary()
            return

        try:
            result = self.get_result()
        except ValueError as e:
            print(f"Cannot print summary: {e}")
            return

        if console is None:
            console = Console()

        # Create and print table
        table = format_benchmark_table(result, title="Latency Benchmark Results")
        if table:
            console.print(table)


def format_benchmark_table(result: BenchmarkResult, title: str = "Benchmark Results") -> Optional[Any]:
    """
    Create a rich table from benchmark results.

    Follows pattern from cli/main.py for creating formatted tables with
    multiple columns and styled output.

    Args:
        result: BenchmarkResult to format
        title: Table title (default: "Benchmark Results")

    Returns:
        Rich Table object if rich is available, None otherwise

    Example:
        >>> result = benchmark_function(my_func, num_iterations=100)
        >>> table = format_benchmark_table(result, title="My Benchmark")
        >>> if table:
        ...     console = Console()
        ...     console.print(table)
    """
    if not RICH_AVAILABLE:
        return None

    table = Table(title=title)
    table.add_column("Metric", style="cyan", no_wrap=True)
    table.add_column("Value", style="green")
    table.add_column("Unit", style="blue")

    # Add measurement summary
    table.add_row("Total Measurements", str(result.total_measurements), "count")
    table.add_row("Total Duration", f"{result.total_duration_ms:.3f}", "ms")
    table.add_row("", "", "")  # Spacer

    # Add latency statistics
    table.add_row("Mean Latency", f"{result.mean_ms:.3f}", "ms")
    table.add_row("Median Latency", f"{result.median_ms:.3f}", "ms")
    table.add_row("Min Latency", f"{result.min_ms:.3f}", "ms")
    table.add_row("Max Latency", f"{result.max_ms:.3f}", "ms")
    table.add_row("Std Deviation", f"{result.stdev_ms:.3f}", "ms")
    table.add_row("", "", "")  # Spacer

    # Add percentiles
    table.add_row("P50 (Median)", f"{result.percentiles.get('p50', 0.0):.3f}", "ms")
    table.add_row("P95", f"{result.percentiles.get('p95', 0.0):.3f}", "ms")
    table.add_row("P99", f"{result.percentiles.get('p99', 0.0):.3f}", "ms")
    table.add_row("P99.9", f"{result.percentiles.get('p99.9', 0.0):.3f}", "ms")
    table.add_row("", "", "")  # Spacer

    # Add throughput
    table.add_row("Throughput", f"{result.throughput_per_sec:.2f}", "items/sec")

    # Add memory if available
    if PSUTIL_AVAILABLE and result.peak_memory_mb > 0:
        table.add_row("", "", "")  # Spacer
        table.add_row("Peak Memory", f"{result.peak_memory_mb:.2f}", "MB")
        table.add_row("Avg Memory", f"{result.avg_memory_mb:.2f}", "MB")

    return table


def format_comparison_table(comparison_result: 'ComparisonResult') -> Optional[Any]:
    """
    Create a rich table for benchmark comparison results.

    Displays baseline vs test results side-by-side with overhead analysis.
    Follows pattern from cli/main.py for multi-column tables.

    Args:
        comparison_result: ComparisonResult to format

    Returns:
        Rich Table object if rich is available, None otherwise

    Example:
        >>> baseline = benchmark_function(baseline_func, num_iterations=100)
        >>> test = benchmark_function(test_func, num_iterations=100)
        >>> comparison = BenchmarkComparison(baseline, test)
        >>> result = comparison.compare()
        >>> table = format_comparison_table(result)
        >>> if table:
        ...     console = Console()
        ...     console.print(table)
    """
    if not RICH_AVAILABLE:
        return None

    baseline_label = comparison_result.metadata.get("baseline_label", "Baseline")
    test_label = comparison_result.metadata.get("test_label", "Test")

    table = Table(title="Benchmark Comparison")
    table.add_column("Metric", style="cyan", no_wrap=True)
    table.add_column(baseline_label, style="green")
    table.add_column(test_label, style="yellow")
    table.add_column("Overhead", style="magenta")

    # Latency metrics
    table.add_row(
        "Mean Latency",
        f"{comparison_result.baseline_result.mean_ms:.3f} ms",
        f"{comparison_result.test_result.mean_ms:.3f} ms",
        f"{comparison_result.mean_overhead_pct:+.2f}%"
    )
    table.add_row(
        "Median Latency",
        f"{comparison_result.baseline_result.median_ms:.3f} ms",
        f"{comparison_result.test_result.median_ms:.3f} ms",
        f"{comparison_result.median_overhead_pct:+.2f}%"
    )
    table.add_row(
        "P95 Latency",
        f"{comparison_result.baseline_result.percentiles.get('p95', 0.0):.3f} ms",
        f"{comparison_result.test_result.percentiles.get('p95', 0.0):.3f} ms",
        f"{comparison_result.p95_overhead_pct:+.2f}%"
    )
    table.add_row(
        "P99 Latency",
        f"{comparison_result.baseline_result.percentiles.get('p99', 0.0):.3f} ms",
        f"{comparison_result.test_result.percentiles.get('p99', 0.0):.3f} ms",
        f"{comparison_result.p99_overhead_pct:+.2f}%"
    )
    table.add_row(
        "Std Deviation",
        f"{comparison_result.baseline_result.stdev_ms:.3f} ms",
        f"{comparison_result.test_result.stdev_ms:.3f} ms",
        "-"
    )

    # Add threshold and status
    table.add_row("", "", "", "")  # Spacer
    status = "✓ PASS" if comparison_result.passes_threshold else "✗ FAIL"
    status_style = "green" if comparison_result.passes_threshold else "red"
    table.add_row(
        "Threshold",
        f"{comparison_result.threshold_pct:.2f}%",
        "",
        f"[{status_style}]{status}[/{status_style}]"
    )

    return table


def benchmark_function(func, num_iterations: int = 1000, warmup_iterations: int = 10) -> BenchmarkResult:
    """
    Benchmark a function by running it multiple times and measuring latency.

    Follows pattern from benchmark_metrics.py benchmark_app().

    Args:
        func: Callable to benchmark
        num_iterations: Number of times to run the function (default: 1000)
        warmup_iterations: Number of warmup runs before measurement (default: 10)

    Returns:
        BenchmarkResult with aggregated statistics

    Example:
        >>> def my_function():
        ...     time.sleep(0.001)
        >>> result = benchmark_function(my_function, num_iterations=100)
        >>> print(f"Mean: {result.mean_ms:.3f} ms")
    """
    benchmark = LatencyBenchmark()

    # Warmup phase (not measured)
    for _ in range(warmup_iterations):
        func()

    # Measurement phase
    for i in range(num_iterations):
        with benchmark.measure(f"iteration_{i}"):
            func()

    return benchmark.get_result()


class EMBODIOSBenchmarkRunner:
    """
    High-level benchmark runner for EMBODIOS inference engine.

    Provides comprehensive benchmarking capabilities for measuring inference
    latency, throughput, and comparing different configurations. Follows
    patterns from benchmark_metrics.py for creating and running benchmarks.

    Example:
        >>> from embodi.core.inference import EMBODIOSInferenceEngine
        >>> engine = EMBODIOSInferenceEngine()
        >>> runner = EMBODIOSBenchmarkRunner(engine)
        >>> result = runner.run_inference_benchmark(num_requests=100)
        >>> runner.print_results()
    """

    def __init__(self, inference_engine=None):
        """
        Initialize benchmark runner.

        Args:
            inference_engine: Optional EMBODIOSInferenceEngine instance.
                            If not provided, benchmarks will use mock inference.
        """
        self.inference_engine = inference_engine
        self.benchmark = LatencyBenchmark()
        self.last_result: Optional[BenchmarkResult] = None

    def run_inference_benchmark(
        self,
        input_tokens: Optional[List[int]] = None,
        num_requests: int = 1000,
        warmup_requests: int = 10,
        metadata: Optional[Dict[str, Any]] = None
    ) -> BenchmarkResult:
        """
        Run inference benchmark with specified parameters.

        Follows pattern from benchmark_metrics.py benchmark_app().

        Args:
            input_tokens: Input tokens for inference. If None, uses default test tokens.
            num_requests: Number of inference requests to make (default: 1000)
            warmup_requests: Number of warmup requests before measurement (default: 10)
            metadata: Optional metadata to attach to benchmark result

        Returns:
            BenchmarkResult with aggregated statistics
        """
        # Clear previous measurements
        self.benchmark.clear_measurements()

        # Use default test tokens if none provided
        if input_tokens is None:
            input_tokens = [1, 2, 3, 4, 5]  # Simple test sequence

        # Warmup phase (not measured)
        for _ in range(warmup_requests):
            self._run_single_inference(input_tokens)

        # Measurement phase
        for i in range(num_requests):
            request_metadata = {"request_id": i}
            if metadata:
                request_metadata.update(metadata)

            with self.benchmark.measure():
                self._run_single_inference(input_tokens)

        # Get results and store
        result = self.benchmark.get_result()
        if metadata:
            result.metadata.update(metadata)

        self.last_result = result
        return result

    def _run_single_inference(self, input_tokens: List[int]) -> None:
        """
        Run a single inference request.

        Args:
            input_tokens: Input tokens for inference
        """
        if self.inference_engine is not None:
            # Use actual inference engine if provided
            try:
                self.inference_engine.inference(input_tokens)
            except Exception:
                # For testing without loaded model, do nothing
                pass
        else:
            # Mock inference for testing
            # Simulate minimal processing time
            time.sleep(0.0001)  # 0.1ms mock inference

    def compare_configurations(
        self,
        config_a_func,
        config_b_func,
        num_requests: int = 1000,
        warmup_requests: int = 10,
        config_a_label: str = "Configuration A",
        config_b_label: str = "Configuration B"
    ) -> Dict[str, Any]:
        """
        Compare two different benchmark configurations.

        Follows pattern from benchmark_metrics.py run_benchmark() for comparing
        baseline vs metrics-enabled configurations.

        Args:
            config_a_func: Callable for first configuration
            config_b_func: Callable for second configuration
            num_requests: Number of requests per configuration
            warmup_requests: Number of warmup requests
            config_a_label: Label for first configuration
            config_b_label: Label for second configuration

        Returns:
            Dictionary with comparison results including overhead percentage
        """
        # Benchmark configuration A
        self.benchmark.clear_measurements()
        for _ in range(warmup_requests):
            config_a_func()

        for _ in range(num_requests):
            with self.benchmark.measure():
                config_a_func()

        result_a = self.benchmark.get_result()
        result_a.metadata["label"] = config_a_label

        # Benchmark configuration B
        self.benchmark.clear_measurements()
        for _ in range(warmup_requests):
            config_b_func()

        for _ in range(num_requests):
            with self.benchmark.measure():
                config_b_func()

        result_b = self.benchmark.get_result()
        result_b.metadata["label"] = config_b_label

        # Calculate overhead (following benchmark_metrics.py pattern)
        mean_overhead_pct = ((result_b.mean_ms - result_a.mean_ms) / result_a.mean_ms) * 100
        median_overhead_pct = ((result_b.median_ms - result_a.median_ms) / result_a.median_ms) * 100

        return {
            config_a_label: result_a,
            config_b_label: result_b,
            "mean_overhead_pct": mean_overhead_pct,
            "median_overhead_pct": median_overhead_pct,
        }

    def print_results(self, result: Optional[BenchmarkResult] = None) -> None:
        """
        Print human-readable benchmark results.

        Follows reporting pattern from benchmark_metrics.py and kernel/ai/benchmark.c.

        Args:
            result: Optional BenchmarkResult to print. If None, uses last result.
        """
        if result is None:
            result = self.last_result

        if result is None:
            print("No benchmark results available. Run a benchmark first.")
            return

        print("=" * 70)
        print("EMBODIOS INFERENCE BENCHMARK RESULTS")
        print("=" * 70)
        print(f"Total requests:     {result.total_measurements}")
        print(f"Total duration:     {result.total_duration_ms:.3f} ms")
        print()
        print("Latency Statistics:")
        print(f"  Mean:   {result.mean_ms:.3f} ms")
        print(f"  Median: {result.median_ms:.3f} ms")
        print(f"  Min:    {result.min_ms:.3f} ms")
        print(f"  Max:    {result.max_ms:.3f} ms")
        print(f"  Stdev:  {result.stdev_ms:.3f} ms")
        print()
        print("Percentiles:")
        print(f"  P50:    {result.percentiles['p50']:.3f} ms")
        print(f"  P95:    {result.percentiles['p95']:.3f} ms")
        print(f"  P99:    {result.percentiles['p99']:.3f} ms")
        print(f"  P99.9:  {result.percentiles['p99.9']:.3f} ms")
        print()
        print("Throughput:")
        print(f"  {result.throughput_per_sec:.2f} requests/sec")
        if PSUTIL_AVAILABLE and result.peak_memory_mb > 0:
            print()
            print("Memory Usage:")
            print(f"  Peak:    {result.peak_memory_mb:.2f} MB")
            print(f"  Average: {result.avg_memory_mb:.2f} MB")
        print("=" * 70)

        if result.metadata:
            print("\nMetadata:")
            for key, value in result.metadata.items():
                print(f"  {key}: {value}")
            print("=" * 70)

    def print_results_rich(self, result: Optional[BenchmarkResult] = None, console: Optional[Any] = None) -> None:
        """
        Print human-readable benchmark results using rich tables.

        Follows pattern from cli/main.py for rich table formatting.
        Falls back to plain text if rich is not available.

        Args:
            result: Optional BenchmarkResult to print. If None, uses last result.
            console: Optional rich Console instance. If None, creates new console.

        Example:
            >>> runner = EMBODIOSBenchmarkRunner()
            >>> result = runner.run_inference_benchmark(num_requests=100)
            >>> runner.print_results_rich()
        """
        if not RICH_AVAILABLE:
            # Fallback to plain text
            self.print_results(result)
            return

        if result is None:
            result = self.last_result

        if result is None:
            print("No benchmark results available. Run a benchmark first.")
            return

        if console is None:
            console = Console()

        # Create and print main results table
        table = format_benchmark_table(result, title="EMBODIOS Inference Benchmark Results")
        if table:
            console.print(table)

        # Print metadata if present
        if result.metadata:
            console.print("\n[bold]Metadata:[/bold]")
            for key, value in result.metadata.items():
                console.print(f"  [cyan]{key}:[/cyan] {value}")

    def print_comparison(self, comparison: Dict[str, Any]) -> None:
        """
        Print comparison results between two configurations.

        Follows reporting pattern from benchmark_metrics.py.

        Args:
            comparison: Comparison dictionary from compare_configurations()
        """
        # Extract results
        config_a_label = None
        config_b_label = None
        result_a = None
        result_b = None

        for key, value in comparison.items():
            if isinstance(value, BenchmarkResult):
                if result_a is None:
                    result_a = value
                    config_a_label = key
                else:
                    result_b = value
                    config_b_label = key

        if result_a is None or result_b is None:
            print("Invalid comparison results")
            return

        mean_overhead = comparison.get("mean_overhead_pct", 0.0)
        median_overhead = comparison.get("median_overhead_pct", 0.0)

        print("=" * 70)
        print("BENCHMARK COMPARISON")
        print("=" * 70)
        print()
        print(f"{config_a_label}:")
        print(f"  Mean latency:   {result_a.mean_ms:.3f} ms")
        print(f"  Median latency: {result_a.median_ms:.3f} ms")
        print(f"  P95 latency:    {result_a.percentiles['p95']:.3f} ms")
        print(f"  P99 latency:    {result_a.percentiles['p99']:.3f} ms")
        print()
        print(f"{config_b_label}:")
        print(f"  Mean latency:   {result_b.mean_ms:.3f} ms")
        print(f"  Median latency: {result_b.median_ms:.3f} ms")
        print(f"  P95 latency:    {result_b.percentiles['p95']:.3f} ms")
        print(f"  P99 latency:    {result_b.percentiles['p99']:.3f} ms")
        print()
        print("Overhead Analysis:")
        print(f"  Mean overhead:   {mean_overhead:+.2f}%")
        print(f"  Median overhead: {median_overhead:+.2f}%")
        print("=" * 70)


@dataclass
class ComparisonResult:
    """
    Results from comparing two benchmark runs (baseline vs test).

    Attributes:
        baseline_result: Baseline benchmark result
        test_result: Test benchmark result
        mean_overhead_ms: Absolute overhead in mean latency (ms)
        mean_overhead_pct: Percentage overhead in mean latency
        median_overhead_ms: Absolute overhead in median latency (ms)
        median_overhead_pct: Percentage overhead in median latency
        p95_overhead_ms: Absolute overhead in P95 latency (ms)
        p95_overhead_pct: Percentage overhead in P95 latency
        p99_overhead_ms: Absolute overhead in P99 latency (ms)
        p99_overhead_pct: Percentage overhead in P99 latency
        passes_threshold: Whether overhead is within acceptable threshold
        threshold_pct: Threshold percentage used for comparison
        metadata: Optional metadata about the comparison
    """
    baseline_result: BenchmarkResult
    test_result: BenchmarkResult
    mean_overhead_ms: float
    mean_overhead_pct: float
    median_overhead_ms: float
    median_overhead_pct: float
    p95_overhead_ms: float
    p95_overhead_pct: float
    p99_overhead_ms: float
    p99_overhead_pct: float
    passes_threshold: bool
    threshold_pct: float
    metadata: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        """Convert comparison result to dictionary"""
        result = asdict(self)
        # Convert nested BenchmarkResults to dicts
        result['baseline_result'] = self.baseline_result.to_dict()
        result['test_result'] = self.test_result.to_dict()
        return result


class BenchmarkComparison:
    """
    Benchmark comparison engine for analyzing performance differences.

    Compares baseline and test benchmark results to calculate overhead
    and determine if performance meets acceptance criteria. Follows
    patterns from BENCHMARK_RESULTS.md for overhead analysis.

    Example:
        >>> baseline = benchmark_function(baseline_func, num_iterations=1000)
        >>> test = benchmark_function(test_func, num_iterations=1000)
        >>> comparison = BenchmarkComparison(baseline, test, threshold_pct=1.0)
        >>> result = comparison.compare()
        >>> print(f"Mean overhead: {result.mean_overhead_pct:.2f}%")
        >>> print(f"Passes threshold: {result.passes_threshold}")
    """

    def __init__(
        self,
        baseline_result: BenchmarkResult,
        test_result: BenchmarkResult,
        threshold_pct: float = 1.0,
        baseline_label: str = "Baseline",
        test_label: str = "Test"
    ):
        """
        Initialize benchmark comparison.

        Args:
            baseline_result: Baseline benchmark result (control)
            test_result: Test benchmark result (to compare against baseline)
            threshold_pct: Acceptable overhead threshold percentage (default: 1.0%)
            baseline_label: Label for baseline configuration
            test_label: Label for test configuration
        """
        self.baseline_result = baseline_result
        self.test_result = test_result
        self.threshold_pct = threshold_pct
        self.baseline_label = baseline_label
        self.test_label = test_label

    def compare(self) -> ComparisonResult:
        """
        Compare baseline and test results to calculate overhead metrics.

        Calculates absolute and percentage overhead for all key metrics
        (mean, median, P95, P99). Determines if test meets threshold criteria
        based on mean overhead percentage.

        Returns:
            ComparisonResult with detailed comparison metrics

        Example:
            >>> comparison = BenchmarkComparison(baseline, test, threshold_pct=1.0)
            >>> result = comparison.compare()
            >>> if result.passes_threshold:
            ...     print("Performance meets requirements!")
        """
        # Calculate mean overhead
        mean_overhead_ms = self.test_result.mean_ms - self.baseline_result.mean_ms
        mean_overhead_pct = (mean_overhead_ms / self.baseline_result.mean_ms) * 100

        # Calculate median overhead
        median_overhead_ms = self.test_result.median_ms - self.baseline_result.median_ms
        median_overhead_pct = (median_overhead_ms / self.baseline_result.median_ms) * 100

        # Calculate P95 overhead
        baseline_p95 = self.baseline_result.percentiles.get('p95', 0.0)
        test_p95 = self.test_result.percentiles.get('p95', 0.0)
        p95_overhead_ms = test_p95 - baseline_p95
        p95_overhead_pct = (p95_overhead_ms / baseline_p95) * 100 if baseline_p95 > 0 else 0.0

        # Calculate P99 overhead
        baseline_p99 = self.baseline_result.percentiles.get('p99', 0.0)
        test_p99 = self.test_result.percentiles.get('p99', 0.0)
        p99_overhead_ms = test_p99 - baseline_p99
        p99_overhead_pct = (p99_overhead_ms / baseline_p99) * 100 if baseline_p99 > 0 else 0.0

        # Determine if test passes threshold (based on mean overhead)
        passes_threshold = mean_overhead_pct <= self.threshold_pct

        return ComparisonResult(
            baseline_result=self.baseline_result,
            test_result=self.test_result,
            mean_overhead_ms=mean_overhead_ms,
            mean_overhead_pct=mean_overhead_pct,
            median_overhead_ms=median_overhead_ms,
            median_overhead_pct=median_overhead_pct,
            p95_overhead_ms=p95_overhead_ms,
            p95_overhead_pct=p95_overhead_pct,
            p99_overhead_ms=p99_overhead_ms,
            p99_overhead_pct=p99_overhead_pct,
            passes_threshold=passes_threshold,
            threshold_pct=self.threshold_pct,
            metadata={
                "baseline_label": self.baseline_label,
                "test_label": self.test_label,
            }
        )

    def print_comparison(self, result: Optional[ComparisonResult] = None) -> None:
        """
        Print human-readable comparison report.

        Follows reporting pattern from BENCHMARK_RESULTS.md, showing
        detailed statistics for both baseline and test, plus overhead analysis.

        Args:
            result: Optional ComparisonResult to print. If None, runs comparison first.

        Example:
            >>> comparison = BenchmarkComparison(baseline, test)
            >>> comparison.print_comparison()
        """
        if result is None:
            result = self.compare()

        print("=" * 70)
        print("BENCHMARK COMPARISON RESULTS")
        print("=" * 70)
        print()
        print(f"{self.baseline_label} (Baseline):")
        print(f"  Mean:   {result.baseline_result.mean_ms:.3f} ms")
        print(f"  Median: {result.baseline_result.median_ms:.3f} ms")
        print(f"  P95:    {result.baseline_result.percentiles.get('p95', 0.0):.3f} ms")
        print(f"  P99:    {result.baseline_result.percentiles.get('p99', 0.0):.3f} ms")
        print(f"  Stdev:  {result.baseline_result.stdev_ms:.3f} ms")
        print()
        print(f"{self.test_label} (Test):")
        print(f"  Mean:   {result.test_result.mean_ms:.3f} ms")
        print(f"  Median: {result.test_result.median_ms:.3f} ms")
        print(f"  P95:    {result.test_result.percentiles.get('p95', 0.0):.3f} ms")
        print(f"  P99:    {result.test_result.percentiles.get('p99', 0.0):.3f} ms")
        print(f"  Stdev:  {result.test_result.stdev_ms:.3f} ms")
        print()
        print("Overhead Analysis:")
        print(f"  Mean overhead:   {result.mean_overhead_ms:+.3f} ms ({result.mean_overhead_pct:+.2f}%)")
        print(f"  Median overhead: {result.median_overhead_ms:+.3f} ms ({result.median_overhead_pct:+.2f}%)")
        print(f"  P95 overhead:    {result.p95_overhead_ms:+.3f} ms ({result.p95_overhead_pct:+.2f}%)")
        print(f"  P99 overhead:    {result.p99_overhead_ms:+.3f} ms ({result.p99_overhead_pct:+.2f}%)")
        print()
        print(f"Threshold: {result.threshold_pct:.2f}%")
        print(f"Status: {'✓ PASS' if result.passes_threshold else '✗ FAIL'}")
        print("=" * 70)

    def print_comparison_rich(self, result: Optional[ComparisonResult] = None, console: Optional[Any] = None) -> None:
        """
        Print human-readable comparison report using rich tables.

        Follows pattern from cli/main.py for rich table formatting.
        Falls back to plain text if rich is not available.

        Args:
            result: Optional ComparisonResult to print. If None, runs comparison first.
            console: Optional rich Console instance. If None, creates new console.

        Example:
            >>> comparison = BenchmarkComparison(baseline, test)
            >>> comparison.print_comparison_rich()
        """
        if not RICH_AVAILABLE:
            # Fallback to plain text
            self.print_comparison(result)
            return

        if result is None:
            result = self.compare()

        if console is None:
            console = Console()

        # Create and print comparison table
        table = format_comparison_table(result)
        if table:
            console.print(table)

    def to_json(
        self,
        result: Optional[ComparisonResult] = None,
        indent: int = 2
    ) -> str:
        """
        Convert comparison results to JSON string.

        Generates machine-readable JSON output for benchmark comparison results,
        suitable for storage, analysis, or integration with other tools.

        Args:
            result: Optional ComparisonResult to serialize. If None, runs comparison first.
            indent: Number of spaces for JSON indentation (default: 2)

        Returns:
            JSON string containing complete comparison results

        Example:
            >>> comparison = BenchmarkComparison(baseline, test)
            >>> json_output = comparison.to_json()
            >>> print(json_output)
            {
              "baseline_result": {...},
              "test_result": {...},
              "mean_overhead_ms": 0.512,
              ...
            }
        """
        import json

        if result is None:
            result = self.compare()

        return json.dumps(result.to_dict(), indent=indent)

    def save_json(
        self,
        filepath: str,
        result: Optional[ComparisonResult] = None,
        indent: int = 2
    ) -> None:
        """
        Save comparison results to JSON file.

        Args:
            filepath: Path to output JSON file
            result: Optional ComparisonResult to save. If None, runs comparison first.
            indent: Number of spaces for JSON indentation (default: 2)

        Example:
            >>> comparison = BenchmarkComparison(baseline, test)
            >>> comparison.save_json("benchmark_results.json")
        """
        import json

        if result is None:
            result = self.compare()

        with open(filepath, 'w') as f:
            json.dump(result.to_dict(), f, indent=indent)


class LlamaCppBenchmarkRunner:
    """
    Subprocess-based benchmark runner for llama.cpp model inference.

    Provides process management for running llama.cpp benchmarks, capturing
    output, and parsing results. Follows subprocess patterns for reliable
    process lifecycle management with timeouts and error handling.

    Example:
        >>> runner = LlamaCppBenchmarkRunner(
        ...     binary_path="/usr/local/bin/llama-cli",
        ...     model_path="/models/llama-7b.gguf"
        ... )
        >>> result = runner.run_benchmark(
        ...     prompt="Hello, world!",
        ...     n_predict=128
        ... )
        >>> print(f"Inference time: {result['inference_time_ms']:.2f} ms")
    """

    def __init__(
        self,
        binary_path: str,
        model_path: str,
        timeout_seconds: int = 300
    ):
        """
        Initialize llama.cpp benchmark runner.

        Args:
            binary_path: Path to llama.cpp executable (e.g., llama-cli or llama-bench)
            model_path: Path to GGUF model file
            timeout_seconds: Maximum time to wait for subprocess (default: 300s)
        """
        self.binary_path = binary_path
        self.model_path = model_path
        self.timeout_seconds = timeout_seconds
        self._process: Optional[subprocess.Popen] = None
        self._output_lines: List[str] = []
        self._error_lines: List[str] = []

    def run_benchmark(
        self,
        prompt: str = "Hello, world!",
        n_predict: int = 128,
        n_threads: Optional[int] = None,
        extra_args: Optional[List[str]] = None,
        output_callback: Optional[Callable[[str], None]] = None
    ) -> Dict[str, Any]:
        """
        Run llama.cpp benchmark with specified parameters.

        Executes llama.cpp as subprocess, captures output, and parses
        benchmark metrics. Follows subprocess management best practices
        with timeout handling and clean shutdown.

        Args:
            prompt: Input prompt for inference
            n_predict: Number of tokens to generate (default: 128)
            n_threads: Number of threads (default: uses llama.cpp default)
            extra_args: Additional command-line arguments
            output_callback: Optional callback for streaming output lines

        Returns:
            Dictionary containing benchmark results:
                - command: Command that was run
                - exit_code: Process exit code
                - stdout: Captured stdout
                - stderr: Captured stderr
                - execution_time_ms: Total execution time in milliseconds
                - success: Whether benchmark completed successfully

        Raises:
            FileNotFoundError: If binary_path or model_path doesn't exist
            subprocess.TimeoutExpired: If process exceeds timeout_seconds
            RuntimeError: If process fails to start or exits with error
        """
        # Build command arguments
        cmd = [
            self.binary_path,
            "-m", self.model_path,
            "-p", prompt,
            "-n", str(n_predict),
        ]

        if n_threads is not None:
            cmd.extend(["-t", str(n_threads)])

        if extra_args:
            cmd.extend(extra_args)

        # Clear previous output
        self._output_lines.clear()
        self._error_lines.clear()

        # Start timing
        start_time = time.time()

        try:
            # Run subprocess with output capture
            self._process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,  # Line buffered
                universal_newlines=True
            )

            # Capture output in real-time
            stdout_lines = []
            stderr_lines = []

            # Read stdout
            if self._process.stdout:
                for line in self._process.stdout:
                    line = line.rstrip()
                    stdout_lines.append(line)
                    self._output_lines.append(line)
                    if output_callback:
                        output_callback(line)

            # Wait for process to complete
            exit_code = self._process.wait(timeout=self.timeout_seconds)

            # Read stderr after process completes
            if self._process.stderr:
                stderr_output = self._process.stderr.read()
                stderr_lines = stderr_output.splitlines()
                self._error_lines.extend(stderr_lines)

        except subprocess.TimeoutExpired:
            # Kill process if timeout exceeded
            self._terminate_process()
            raise subprocess.TimeoutExpired(
                cmd=cmd,
                timeout=self.timeout_seconds
            )
        except FileNotFoundError:
            raise FileNotFoundError(
                f"llama.cpp binary not found at: {self.binary_path}"
            )
        except Exception as e:
            # Clean up on any error
            self._terminate_process()
            raise RuntimeError(f"Failed to run llama.cpp benchmark: {e}")
        finally:
            # Calculate execution time
            execution_time_ms = (time.time() - start_time) * 1000.0

        # Build result dictionary
        result = {
            "command": " ".join(cmd),
            "exit_code": exit_code,
            "stdout": "\n".join(stdout_lines),
            "stderr": "\n".join(stderr_lines),
            "execution_time_ms": execution_time_ms,
            "success": exit_code == 0,
        }

        # Parse llama.cpp output for benchmark metrics
        parsed_metrics = self._parse_output(stdout_lines)
        result.update(parsed_metrics)

        return result

    def run_benchmark_async(
        self,
        prompt: str = "Hello, world!",
        n_predict: int = 128,
        n_threads: Optional[int] = None,
        extra_args: Optional[List[str]] = None,
        output_callback: Optional[Callable[[str], None]] = None,
        completion_callback: Optional[Callable[[Dict[str, Any]], None]] = None
    ) -> threading.Thread:
        """
        Run llama.cpp benchmark asynchronously in background thread.

        Args:
            prompt: Input prompt for inference
            n_predict: Number of tokens to generate
            n_threads: Number of threads
            extra_args: Additional command-line arguments
            output_callback: Optional callback for streaming output
            completion_callback: Optional callback when benchmark completes

        Returns:
            Thread object for the running benchmark
        """
        def _async_wrapper():
            result = self.run_benchmark(
                prompt=prompt,
                n_predict=n_predict,
                n_threads=n_threads,
                extra_args=extra_args,
                output_callback=output_callback
            )
            if completion_callback:
                completion_callback(result)

        thread = threading.Thread(target=_async_wrapper, daemon=True)
        thread.start()
        return thread

    def _terminate_process(self) -> None:
        """
        Gracefully terminate running subprocess.

        Attempts SIGTERM first, then SIGKILL if process doesn't respond.
        Follows subprocess cleanup best practices.
        """
        if self._process is None:
            return

        try:
            # Try graceful termination first
            self._process.terminate()
            try:
                self._process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                # Force kill if still running
                self._process.kill()
                self._process.wait()
        except Exception:
            # Process may already be terminated
            pass
        finally:
            self._process = None

    def _parse_output(self, output_lines: List[str]) -> Dict[str, Any]:
        """
        Parse llama.cpp output for benchmark metrics.

        Extracts timing information, token counts, and performance metrics
        from llama.cpp stdout. Handles different output formats gracefully.

        Args:
            output_lines: List of stdout lines from llama.cpp

        Returns:
            Dictionary with parsed metrics (empty if parsing fails)
        """
        # Use the standalone parse_llamacpp_output function for consistency
        return parse_llamacpp_output(output_lines)

    def get_last_output(self) -> List[str]:
        """
        Get captured stdout lines from last benchmark run.

        Returns:
            List of stdout lines
        """
        return self._output_lines.copy()

    def get_last_errors(self) -> List[str]:
        """
        Get captured stderr lines from last benchmark run.

        Returns:
            List of stderr lines
        """
        return self._error_lines.copy()

    def is_running(self) -> bool:
        """
        Check if subprocess is currently running.

        Returns:
            True if process is running, False otherwise
        """
        if self._process is None:
            return False
        return self._process.poll() is None

    def stop(self) -> None:
        """
        Stop currently running benchmark subprocess.

        Gracefully terminates the process if running.
        """
        self._terminate_process()
