"""
Profiling Data Models - Live kernel profiling statistics and collection

Provides Python models for real-time profiling data from the EMBODIOS kernel.
Models correspond to C structures in kernel/include/embodios/profiler.h
"""

from typing import List, Optional
from pydantic import BaseModel, Field
import time


class FunctionProfile(BaseModel):
    """
    Per-function profiling statistics.

    Corresponds to profiler_stats_t in kernel profiler.
    """

    function_name: str = Field(
        description="Function identifier"
    )
    total_time_us: int = Field(
        description="Total time spent in function (microseconds)"
    )
    call_count: int = Field(
        description="Number of calls to this function"
    )
    min_time_us: int = Field(
        description="Minimum call duration (microseconds)"
    )
    max_time_us: int = Field(
        description="Maximum call duration (microseconds)"
    )
    avg_time_us: int = Field(
        description="Average call duration (microseconds)"
    )
    cpu_percent: float = Field(
        description="Percentage of total CPU time"
    )


class MemoryProfile(BaseModel):
    """
    Memory allocation tracking statistics.

    Corresponds to profiler_alloc_stats_t in kernel profiler.
    """

    location: str = Field(
        description="Allocation site identifier (file:line or hash)"
    )
    total_allocated: int = Field(
        description="Total bytes allocated at this site"
    )
    total_freed: int = Field(
        description="Total bytes freed at this site"
    )
    current_usage: int = Field(
        description="Current allocated bytes"
    )
    peak_usage: int = Field(
        description="Peak allocated bytes"
    )
    alloc_count: int = Field(
        description="Number of allocations"
    )
    free_count: int = Field(
        description="Number of frees"
    )
    alloc_rate_bps: float = Field(
        description="Allocation rate (bytes per second)"
    )


class HotPath(BaseModel):
    """
    Hot path entry - functions consuming most CPU time.

    Corresponds to profiler_hot_path_t in kernel profiler.
    """

    function_name: str = Field(
        description="Function identifier"
    )
    total_time_us: int = Field(
        description="Total time in this function (microseconds)"
    )
    call_count: int = Field(
        description="Number of calls"
    )
    cpu_percent: float = Field(
        description="Percentage of total CPU time"
    )
    avg_time_us: int = Field(
        description="Average time per call (microseconds)"
    )


class ProfilingSummary(BaseModel):
    """
    Overall profiler state and summary.

    Corresponds to profiler_summary_t in kernel profiler.
    """

    total_entries: int = Field(
        description="Total profiling entries recorded"
    )
    total_samples: int = Field(
        description="Total samples collected"
    )
    total_time_us: int = Field(
        description="Total profiling time (microseconds)"
    )
    overhead_us: int = Field(
        description="Profiler overhead time (microseconds)"
    )
    overhead_percent: float = Field(
        description="Overhead as percentage"
    )
    active_functions: int = Field(
        description="Number of tracked functions"
    )
    dropped_entries: int = Field(
        description="Entries dropped due to buffer full"
    )
    enabled: bool = Field(
        description="Whether profiler is active"
    )


class ProfilingStats(BaseModel):
    """
    Complete profiling statistics snapshot.

    Contains all profiling data: function stats, memory tracking,
    hot paths, and overall summary.
    """

    timestamp: int = Field(
        default_factory=lambda: int(time.time()),
        description="Unix timestamp when snapshot was taken"
    )
    summary: ProfilingSummary = Field(
        description="Overall profiler summary"
    )
    functions: List[FunctionProfile] = Field(
        default_factory=list,
        description="Per-function profiling statistics"
    )
    memory: List[MemoryProfile] = Field(
        default_factory=list,
        description="Memory allocation tracking data"
    )
    hot_paths: List[HotPath] = Field(
        default_factory=list,
        description="Hot paths sorted by CPU time"
    )


class ProfilingCollector:
    """
    Centralized profiling data collection for EMBODIOS API server.

    Provides methods for collecting and aggregating profiling data
    from the kernel profiler system. Acts as a bridge between the
    kernel C profiler and Python API endpoints.
    """

    def __init__(self):
        """Initialize profiling collector"""
        self._enabled = False
        self._last_snapshot: Optional[ProfilingStats] = None

    def enable(self):
        """Enable profiling data collection"""
        self._enabled = True

    def disable(self):
        """Disable profiling data collection"""
        self._enabled = False

    def is_enabled(self) -> bool:
        """
        Check if profiling is enabled.

        Returns:
            True if profiling is active, False otherwise
        """
        return self._enabled

    def get_stats(self) -> Optional[ProfilingStats]:
        """
        Get current profiling statistics.

        Returns realistic profiling data simulating kernel profiler output.
        Will be replaced with real kernel data once integration is complete.

        Returns:
            ProfilingStats object with current profiling data,
            or None if profiling is disabled
        """
        if not self._enabled:
            return None

        # Generate realistic profiling data
        # This simulates what kernel profiler would provide

        # Sample function profiling data
        functions = [
            FunctionProfile(
                function_name="inference_forward_pass",
                total_time_us=45230,
                call_count=127,
                min_time_us=245,
                max_time_us=892,
                avg_time_us=356,
                cpu_percent=32.5
            ),
            FunctionProfile(
                function_name="matrix_multiply_kernel",
                total_time_us=38100,
                call_count=512,
                min_time_us=42,
                max_time_us=156,
                avg_time_us=74,
                cpu_percent=27.4
            ),
            FunctionProfile(
                function_name="attention_compute",
                total_time_us=22450,
                call_count=256,
                min_time_us=65,
                max_time_us=145,
                avg_time_us=87,
                cpu_percent=16.1
            ),
            FunctionProfile(
                function_name="softmax_kernel",
                total_time_us=15670,
                call_count=256,
                min_time_us=48,
                max_time_us=89,
                avg_time_us=61,
                cpu_percent=11.3
            ),
            FunctionProfile(
                function_name="layer_norm",
                total_time_us=12340,
                call_count=384,
                min_time_us=28,
                max_time_us=52,
                avg_time_us=32,
                cpu_percent=8.9
            )
        ]

        # Sample memory profiling data
        memory = [
            MemoryProfile(
                location="inference.c:145",
                total_allocated=4194304,
                total_freed=3145728,
                current_usage=1048576,
                peak_usage=2097152,
                alloc_count=42,
                free_count=31,
                alloc_rate_bps=524288.0
            ),
            MemoryProfile(
                location="attention.c:89",
                total_allocated=2097152,
                total_freed=1572864,
                current_usage=524288,
                peak_usage=786432,
                alloc_count=128,
                free_count=96,
                alloc_rate_bps=262144.0
            ),
            MemoryProfile(
                location="matmul.c:234",
                total_allocated=8388608,
                total_freed=7340032,
                current_usage=1048576,
                peak_usage=4194304,
                alloc_count=256,
                free_count=224,
                alloc_rate_bps=1048576.0
            )
        ]

        # Sample hot paths (sorted by CPU time)
        hot_paths = [
            HotPath(
                function_name="inference_forward_pass",
                total_time_us=45230,
                call_count=127,
                cpu_percent=32.5,
                avg_time_us=356
            ),
            HotPath(
                function_name="matrix_multiply_kernel",
                total_time_us=38100,
                call_count=512,
                cpu_percent=27.4,
                avg_time_us=74
            ),
            HotPath(
                function_name="attention_compute",
                total_time_us=22450,
                call_count=256,
                cpu_percent=16.1,
                avg_time_us=87
            )
        ]

        # Generate summary statistics
        total_time = sum(f.total_time_us for f in functions)
        summary = ProfilingSummary(
            total_entries=1535,
            total_samples=1535,
            total_time_us=total_time,
            overhead_us=1892,
            overhead_percent=1.4,
            active_functions=len(functions),
            dropped_entries=0,
            enabled=self._enabled
        )

        stats = ProfilingStats(
            summary=summary,
            functions=functions,
            memory=memory,
            hot_paths=hot_paths
        )

        self._last_snapshot = stats
        return stats

    def get_last_snapshot(self) -> Optional[ProfilingStats]:
        """
        Get the last recorded profiling snapshot.

        Returns:
            Last ProfilingStats snapshot, or None if no snapshot exists
        """
        return self._last_snapshot

    def reset(self):
        """Reset profiling statistics and clear cached data"""
        self._last_snapshot = None


# Global profiling collector instance
_profiling_collector: Optional[ProfilingCollector] = None


def get_profiling_collector() -> ProfilingCollector:
    """
    Get or create the global profiling collector instance.

    Returns:
        ProfilingCollector instance
    """
    global _profiling_collector
    if _profiling_collector is None:
        _profiling_collector = ProfilingCollector()
    return _profiling_collector
