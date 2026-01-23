#!/usr/bin/env python3
"""
Resource monitoring module for stability tests.

Provides real-time monitoring of system resources (CPU, memory, threads)
for long-running stability tests to detect memory leaks, CPU spikes,
and resource exhaustion.
"""

import time
import psutil
from dataclasses import dataclass
from typing import Optional, List


@dataclass
class ResourceSnapshot:
    """
    Snapshot of system resource usage at a point in time.

    Attributes:
        timestamp: Time of snapshot (Unix timestamp)
        cpu_percent: CPU usage percentage (0-100)
        memory_rss_bytes: Resident Set Size in bytes
        memory_vms_bytes: Virtual Memory Size in bytes
        memory_percent: Memory usage percentage (0-100)
        num_threads: Number of threads
        num_fds: Number of open file descriptors (Unix only)
    """

    timestamp: float
    cpu_percent: float
    memory_rss_bytes: int
    memory_vms_bytes: int
    memory_percent: float
    num_threads: int
    num_fds: Optional[int] = None

    def memory_rss_mb(self) -> float:
        """Get RSS memory in MB"""
        return self.memory_rss_bytes / (1024 * 1024)

    def memory_vms_mb(self) -> float:
        """Get VMS memory in MB"""
        return self.memory_vms_bytes / (1024 * 1024)


class ResourceMonitor:
    """
    Real-time resource monitor for stability testing.

    Monitors CPU, memory, threads, and file descriptors for the current
    process. Provides snapshots for tracking resource usage over time
    and detecting resource leaks or degradation.

    Example:
        monitor = ResourceMonitor()
        snapshot = monitor.capture()
        print(f"Memory: {snapshot.memory_rss_mb():.2f} MB")
    """

    def __init__(self, process: Optional[psutil.Process] = None):
        """
        Initialize resource monitor.

        Args:
            process: psutil.Process to monitor (default: current process)
        """
        self._process = process or psutil.Process()
        self._baseline: Optional[ResourceSnapshot] = None
        self._snapshots: List[ResourceSnapshot] = []

        # Warm up CPU measurement (first call returns 0.0)
        try:
            self._process.cpu_percent(interval=0.1)
        except Exception:
            pass

    def capture(self) -> ResourceSnapshot:
        """
        Capture current resource usage snapshot.

        Returns:
            ResourceSnapshot with current resource usage
        """
        try:
            # Get memory info
            memory_info = self._process.memory_info()
            memory_percent = self._process.memory_percent()

            # Get CPU usage (non-blocking)
            cpu_percent = self._process.cpu_percent(interval=0)

            # Get thread count
            num_threads = self._process.num_threads()

            # Get file descriptor count (Unix only)
            num_fds = None
            try:
                num_fds = self._process.num_fds()
            except (AttributeError, NotImplementedError):
                # Windows doesn't support num_fds
                pass

            snapshot = ResourceSnapshot(
                timestamp=time.time(),
                cpu_percent=cpu_percent,
                memory_rss_bytes=memory_info.rss,
                memory_vms_bytes=memory_info.vms,
                memory_percent=memory_percent,
                num_threads=num_threads,
                num_fds=num_fds,
            )

            self._snapshots.append(snapshot)
            return snapshot

        except (psutil.NoSuchProcess, psutil.AccessDenied) as e:
            raise RuntimeError(f"Failed to capture resource snapshot: {e}")

    def set_baseline(self, snapshot: Optional[ResourceSnapshot] = None) -> ResourceSnapshot:
        """
        Set baseline resource usage for comparison.

        Args:
            snapshot: Snapshot to use as baseline (default: capture new one)

        Returns:
            The baseline snapshot
        """
        if snapshot is None:
            snapshot = self.capture()

        self._baseline = snapshot
        return snapshot

    def get_baseline(self) -> Optional[ResourceSnapshot]:
        """
        Get the current baseline snapshot.

        Returns:
            Baseline snapshot or None if not set
        """
        return self._baseline

    def calculate_memory_drift(self, current: Optional[ResourceSnapshot] = None) -> Optional[float]:
        """
        Calculate memory drift percentage from baseline.

        Args:
            current: Current snapshot (default: capture new one)

        Returns:
            Percentage drift from baseline (positive = growth, negative = shrink)
            Returns None if no baseline is set
        """
        if self._baseline is None:
            return None

        if current is None:
            current = self.capture()

        baseline_rss = self._baseline.memory_rss_bytes
        current_rss = current.memory_rss_bytes

        if baseline_rss == 0:
            return 0.0

        drift = ((current_rss - baseline_rss) / baseline_rss) * 100.0
        return drift

    def get_snapshots(self) -> List[ResourceSnapshot]:
        """
        Get all captured snapshots.

        Returns:
            List of ResourceSnapshot objects in chronological order
        """
        return self._snapshots.copy()

    def clear_snapshots(self) -> None:
        """Clear all captured snapshots (keeps baseline)"""
        self._snapshots.clear()

    def get_peak_memory(self) -> Optional[ResourceSnapshot]:
        """
        Get snapshot with highest RSS memory usage.

        Returns:
            Snapshot with peak memory or None if no snapshots captured
        """
        if not self._snapshots:
            return None

        return max(self._snapshots, key=lambda s: s.memory_rss_bytes)

    def get_average_cpu(self) -> Optional[float]:
        """
        Calculate average CPU usage across all snapshots.

        Returns:
            Average CPU percentage or None if no snapshots captured
        """
        if not self._snapshots:
            return None

        total_cpu = sum(s.cpu_percent for s in self._snapshots)
        return total_cpu / len(self._snapshots)
