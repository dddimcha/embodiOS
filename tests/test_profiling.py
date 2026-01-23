#!/usr/bin/env python3
"""
Unit tests for Profiling module
Tests profiling data models and ProfilingCollector for EMBODIOS API
"""

import time
from unittest.mock import patch, MagicMock
import pytest

from src.embodi.api.profiling import (
    FunctionProfile,
    MemoryProfile,
    HotPath,
    ProfilingSummary,
    ProfilingStats,
    ProfilingCollector,
    get_profiling_collector
)


@pytest.fixture
def collector():
    """Provide a fresh ProfilingCollector instance for each test"""
    return ProfilingCollector()


@pytest.fixture
def sample_function_profile():
    """Provide a sample FunctionProfile for testing"""
    return FunctionProfile(
        function_name="test_function",
        total_time_us=1000000,
        call_count=100,
        min_time_us=5000,
        max_time_us=20000,
        avg_time_us=10000,
        cpu_percent=15.5
    )


@pytest.fixture
def sample_memory_profile():
    """Provide a sample MemoryProfile for testing"""
    return MemoryProfile(
        location="test.c:42",
        total_allocated=1048576,
        total_freed=524288,
        current_usage=524288,
        peak_usage=1048576,
        alloc_count=100,
        free_count=50,
        alloc_rate_bps=1024.5
    )


@pytest.fixture
def sample_hot_path():
    """Provide a sample HotPath for testing"""
    return HotPath(
        function_name="hot_function",
        total_time_us=5000000,
        call_count=500,
        cpu_percent=45.2,
        avg_time_us=10000
    )


@pytest.fixture
def sample_profiling_summary():
    """Provide a sample ProfilingSummary for testing"""
    return ProfilingSummary(
        total_entries=1000,
        total_samples=5000,
        total_time_us=10000000,
        overhead_us=50000,
        overhead_percent=0.5,
        active_functions=25,
        dropped_entries=0,
        enabled=True
    )


class TestFunctionProfile:
    """Test suite for FunctionProfile data model"""

    def test_function_profile_creation(self, sample_function_profile):
        """Test that FunctionProfile can be created with valid data"""
        assert sample_function_profile.function_name == "test_function"
        assert sample_function_profile.total_time_us == 1000000
        assert sample_function_profile.call_count == 100
        assert sample_function_profile.min_time_us == 5000
        assert sample_function_profile.max_time_us == 20000
        assert sample_function_profile.avg_time_us == 10000
        assert sample_function_profile.cpu_percent == 15.5

    def test_function_profile_required_fields(self):
        """Test that FunctionProfile requires all fields"""
        with pytest.raises(Exception):
            FunctionProfile(function_name="test")

    def test_function_profile_serialization(self, sample_function_profile):
        """Test that FunctionProfile can be serialized to dict"""
        data = sample_function_profile.model_dump()
        assert isinstance(data, dict)
        assert data['function_name'] == "test_function"
        assert data['total_time_us'] == 1000000
        assert data['cpu_percent'] == 15.5

    def test_function_profile_json_serialization(self, sample_function_profile):
        """Test that FunctionProfile can be serialized to JSON"""
        json_str = sample_function_profile.model_dump_json()
        assert isinstance(json_str, str)
        assert "test_function" in json_str
        assert "1000000" in json_str


class TestMemoryProfile:
    """Test suite for MemoryProfile data model"""

    def test_memory_profile_creation(self, sample_memory_profile):
        """Test that MemoryProfile can be created with valid data"""
        assert sample_memory_profile.location == "test.c:42"
        assert sample_memory_profile.total_allocated == 1048576
        assert sample_memory_profile.total_freed == 524288
        assert sample_memory_profile.current_usage == 524288
        assert sample_memory_profile.peak_usage == 1048576
        assert sample_memory_profile.alloc_count == 100
        assert sample_memory_profile.free_count == 50
        assert sample_memory_profile.alloc_rate_bps == 1024.5

    def test_memory_profile_required_fields(self):
        """Test that MemoryProfile requires all fields"""
        with pytest.raises(Exception):
            MemoryProfile(location="test.c:1")

    def test_memory_profile_serialization(self, sample_memory_profile):
        """Test that MemoryProfile can be serialized to dict"""
        data = sample_memory_profile.model_dump()
        assert isinstance(data, dict)
        assert data['location'] == "test.c:42"
        assert data['total_allocated'] == 1048576
        assert data['alloc_rate_bps'] == 1024.5

    def test_memory_profile_tracks_current_usage(self):
        """Test that MemoryProfile correctly tracks current memory usage"""
        profile = MemoryProfile(
            location="test.c:100",
            total_allocated=2000,
            total_freed=500,
            current_usage=1500,
            peak_usage=2000,
            alloc_count=10,
            free_count=5,
            alloc_rate_bps=100.0
        )
        assert profile.current_usage == 1500
        assert profile.current_usage == profile.total_allocated - profile.total_freed


class TestHotPath:
    """Test suite for HotPath data model"""

    def test_hot_path_creation(self, sample_hot_path):
        """Test that HotPath can be created with valid data"""
        assert sample_hot_path.function_name == "hot_function"
        assert sample_hot_path.total_time_us == 5000000
        assert sample_hot_path.call_count == 500
        assert sample_hot_path.cpu_percent == 45.2
        assert sample_hot_path.avg_time_us == 10000

    def test_hot_path_required_fields(self):
        """Test that HotPath requires all fields"""
        with pytest.raises(Exception):
            HotPath(function_name="test")

    def test_hot_path_serialization(self, sample_hot_path):
        """Test that HotPath can be serialized to dict"""
        data = sample_hot_path.model_dump()
        assert isinstance(data, dict)
        assert data['function_name'] == "hot_function"
        assert data['cpu_percent'] == 45.2

    def test_hot_path_high_cpu_percentage(self):
        """Test that HotPath can represent high CPU usage functions"""
        hot_path = HotPath(
            function_name="expensive_function",
            total_time_us=9000000,
            call_count=1000,
            cpu_percent=90.0,
            avg_time_us=9000
        )
        assert hot_path.cpu_percent == 90.0
        assert hot_path.cpu_percent > 50.0


class TestProfilingSummary:
    """Test suite for ProfilingSummary data model"""

    def test_profiling_summary_creation(self, sample_profiling_summary):
        """Test that ProfilingSummary can be created with valid data"""
        assert sample_profiling_summary.total_entries == 1000
        assert sample_profiling_summary.total_samples == 5000
        assert sample_profiling_summary.total_time_us == 10000000
        assert sample_profiling_summary.overhead_us == 50000
        assert sample_profiling_summary.overhead_percent == 0.5
        assert sample_profiling_summary.active_functions == 25
        assert sample_profiling_summary.dropped_entries == 0
        assert sample_profiling_summary.enabled is True

    def test_profiling_summary_required_fields(self):
        """Test that ProfilingSummary requires all fields"""
        with pytest.raises(Exception):
            ProfilingSummary(total_entries=100)

    def test_profiling_summary_serialization(self, sample_profiling_summary):
        """Test that ProfilingSummary can be serialized to dict"""
        data = sample_profiling_summary.model_dump()
        assert isinstance(data, dict)
        assert data['total_entries'] == 1000
        assert data['enabled'] is True

    def test_profiling_summary_tracks_overhead(self):
        """Test that ProfilingSummary correctly tracks profiler overhead"""
        summary = ProfilingSummary(
            total_entries=1000,
            total_samples=5000,
            total_time_us=1000000,
            overhead_us=30000,
            overhead_percent=3.0,
            active_functions=10,
            dropped_entries=0,
            enabled=True
        )
        assert summary.overhead_percent == 3.0
        # Overhead should be less than 5% per spec
        assert summary.overhead_percent < 5.0

    def test_profiling_summary_disabled_state(self):
        """Test that ProfilingSummary can represent disabled profiler"""
        summary = ProfilingSummary(
            total_entries=0,
            total_samples=0,
            total_time_us=0,
            overhead_us=0,
            overhead_percent=0.0,
            active_functions=0,
            dropped_entries=0,
            enabled=False
        )
        assert summary.enabled is False
        assert summary.total_entries == 0


class TestProfilingStats:
    """Test suite for ProfilingStats data model"""

    def test_profiling_stats_creation(self, sample_profiling_summary,
                                     sample_function_profile,
                                     sample_memory_profile,
                                     sample_hot_path):
        """Test that ProfilingStats can be created with valid data"""
        stats = ProfilingStats(
            summary=sample_profiling_summary,
            functions=[sample_function_profile],
            memory=[sample_memory_profile],
            hot_paths=[sample_hot_path]
        )
        assert stats.summary == sample_profiling_summary
        assert len(stats.functions) == 1
        assert len(stats.memory) == 1
        assert len(stats.hot_paths) == 1

    def test_profiling_stats_timestamp_auto_generated(self, sample_profiling_summary):
        """Test that ProfilingStats generates timestamp automatically"""
        with patch('time.time', return_value=1234567890.0):
            stats = ProfilingStats(
                summary=sample_profiling_summary
            )
            assert stats.timestamp == 1234567890

    def test_profiling_stats_empty_lists_default(self, sample_profiling_summary):
        """Test that ProfilingStats has empty lists by default"""
        stats = ProfilingStats(summary=sample_profiling_summary)
        assert stats.functions == []
        assert stats.memory == []
        assert stats.hot_paths == []

    def test_profiling_stats_serialization(self, sample_profiling_summary,
                                          sample_function_profile):
        """Test that ProfilingStats can be serialized to dict"""
        stats = ProfilingStats(
            summary=sample_profiling_summary,
            functions=[sample_function_profile]
        )
        data = stats.model_dump()
        assert isinstance(data, dict)
        assert 'timestamp' in data
        assert 'summary' in data
        assert 'functions' in data
        assert len(data['functions']) == 1

    def test_profiling_stats_multiple_entries(self, sample_profiling_summary):
        """Test that ProfilingStats can hold multiple profiling entries"""
        functions = [
            FunctionProfile(
                function_name=f"func_{i}",
                total_time_us=1000 * i,
                call_count=10 * i,
                min_time_us=50,
                max_time_us=200,
                avg_time_us=100,
                cpu_percent=1.0 * i
            )
            for i in range(10)
        ]
        stats = ProfilingStats(
            summary=sample_profiling_summary,
            functions=functions
        )
        assert len(stats.functions) == 10
        assert stats.functions[0].function_name == "func_0"
        assert stats.functions[9].function_name == "func_9"


class TestProfilingCollectorInit:
    """Test suite for ProfilingCollector initialization"""

    def test_init_starts_disabled(self, collector):
        """Test that ProfilingCollector starts with profiling disabled"""
        assert collector.is_enabled() is False
        assert collector._enabled is False

    def test_init_no_snapshot(self, collector):
        """Test that ProfilingCollector starts with no snapshot"""
        assert collector.get_last_snapshot() is None
        assert collector._last_snapshot is None


class TestProfilingCollectorEnableDisable:
    """Test suite for ProfilingCollector enable/disable functionality"""

    def test_enable_sets_flag(self, collector):
        """Test that enable() activates profiling"""
        collector.enable()
        assert collector.is_enabled() is True
        assert collector._enabled is True

    def test_disable_clears_flag(self, collector):
        """Test that disable() deactivates profiling"""
        collector.enable()
        collector.disable()
        assert collector.is_enabled() is False
        assert collector._enabled is False

    def test_is_enabled_returns_correct_state(self, collector):
        """Test that is_enabled() reflects current profiling state"""
        # Initially disabled
        assert collector.is_enabled() is False

        # After enable
        collector.enable()
        assert collector.is_enabled() is True

        # After disable
        collector.disable()
        assert collector.is_enabled() is False

    def test_multiple_enable_calls_safe(self, collector):
        """Test that calling enable() multiple times is safe"""
        collector.enable()
        collector.enable()
        collector.enable()
        assert collector.is_enabled() is True

    def test_multiple_disable_calls_safe(self, collector):
        """Test that calling disable() multiple times is safe"""
        collector.enable()
        collector.disable()
        collector.disable()
        collector.disable()
        assert collector.is_enabled() is False


class TestProfilingCollectorGetStats:
    """Test suite for ProfilingCollector get_stats functionality"""

    def test_get_stats_returns_none_when_disabled(self, collector):
        """Test that get_stats() returns None when profiling is disabled"""
        stats = collector.get_stats()
        assert stats is None

    def test_get_stats_returns_stats_when_enabled(self, collector):
        """Test that get_stats() returns ProfilingStats when enabled"""
        collector.enable()
        stats = collector.get_stats()
        assert stats is not None
        assert isinstance(stats, ProfilingStats)

    def test_get_stats_creates_valid_summary(self, collector):
        """Test that get_stats() creates ProfilingStats with valid summary"""
        collector.enable()
        stats = collector.get_stats()
        assert stats.summary is not None
        assert isinstance(stats.summary, ProfilingSummary)
        assert stats.summary.enabled is True

    def test_get_stats_mock_data_structure(self, collector):
        """Test that get_stats() returns expected mock data structure"""
        collector.enable()
        stats = collector.get_stats()

        # Verify summary has expected mock values
        assert stats.summary.total_entries == 0
        assert stats.summary.total_samples == 0
        assert stats.summary.total_time_us == 0
        assert stats.summary.overhead_us == 0
        assert stats.summary.overhead_percent == 0.0
        assert stats.summary.active_functions == 0
        assert stats.summary.dropped_entries == 0
        assert stats.summary.enabled is True

        # Verify empty lists
        assert stats.functions == []
        assert stats.memory == []
        assert stats.hot_paths == []

    def test_get_stats_has_timestamp(self, collector):
        """Test that get_stats() includes a timestamp"""
        collector.enable()
        before = int(time.time())
        stats = collector.get_stats()
        after = int(time.time())

        assert stats.timestamp >= before
        assert stats.timestamp <= after

    def test_get_stats_updates_last_snapshot(self, collector):
        """Test that get_stats() updates the last snapshot"""
        collector.enable()
        assert collector._last_snapshot is None

        stats = collector.get_stats()
        assert collector._last_snapshot is not None
        assert collector._last_snapshot == stats

    def test_get_stats_after_disable_returns_none(self, collector):
        """Test that get_stats() returns None after disabling"""
        collector.enable()
        collector.get_stats()  # Get initial stats
        collector.disable()

        stats = collector.get_stats()
        assert stats is None


class TestProfilingCollectorSnapshot:
    """Test suite for ProfilingCollector snapshot functionality"""

    def test_get_last_snapshot_initially_none(self, collector):
        """Test that get_last_snapshot() returns None initially"""
        snapshot = collector.get_last_snapshot()
        assert snapshot is None

    def test_get_last_snapshot_after_get_stats(self, collector):
        """Test that get_last_snapshot() returns stats after get_stats()"""
        collector.enable()
        stats = collector.get_stats()
        snapshot = collector.get_last_snapshot()

        assert snapshot is not None
        assert snapshot == stats

    def test_get_last_snapshot_persists_after_disable(self, collector):
        """Test that last snapshot persists after disabling profiler"""
        collector.enable()
        stats = collector.get_stats()
        collector.disable()

        snapshot = collector.get_last_snapshot()
        assert snapshot is not None
        assert snapshot == stats

    def test_get_last_snapshot_multiple_calls(self, collector):
        """Test that get_last_snapshot() returns the most recent stats"""
        collector.enable()

        with patch('time.time', return_value=1000.0):
            stats1 = collector.get_stats()

        with patch('time.time', return_value=2000.0):
            stats2 = collector.get_stats()

        snapshot = collector.get_last_snapshot()
        assert snapshot == stats2
        assert snapshot.timestamp != stats1.timestamp


class TestProfilingCollectorReset:
    """Test suite for ProfilingCollector reset functionality"""

    def test_reset_clears_snapshot(self, collector):
        """Test that reset() clears the last snapshot"""
        collector.enable()
        collector.get_stats()
        assert collector._last_snapshot is not None

        collector.reset()
        assert collector._last_snapshot is None

    def test_reset_preserves_enabled_state(self, collector):
        """Test that reset() does not change enabled state"""
        collector.enable()
        collector.get_stats()

        collector.reset()
        assert collector.is_enabled() is True

    def test_reset_when_disabled(self, collector):
        """Test that reset() works when profiling is disabled"""
        collector.reset()
        assert collector._last_snapshot is None
        assert collector.is_enabled() is False

    def test_reset_clears_all_cached_data(self, collector):
        """Test that reset() clears all cached profiling data"""
        collector.enable()
        collector.get_stats()
        assert collector.get_last_snapshot() is not None

        collector.reset()
        assert collector.get_last_snapshot() is None


class TestGetProfilingCollector:
    """Test suite for get_profiling_collector singleton function"""

    def test_get_profiling_collector_returns_instance(self):
        """Test that get_profiling_collector() returns ProfilingCollector"""
        collector = get_profiling_collector()
        assert collector is not None
        assert isinstance(collector, ProfilingCollector)

    def test_get_profiling_collector_singleton_pattern(self):
        """Test that get_profiling_collector() returns same instance"""
        collector1 = get_profiling_collector()
        collector2 = get_profiling_collector()
        assert collector1 is collector2

    def test_get_profiling_collector_state_persists(self):
        """Test that state persists across get_profiling_collector calls"""
        collector1 = get_profiling_collector()
        collector1.enable()

        collector2 = get_profiling_collector()
        assert collector2.is_enabled() is True

    def test_get_profiling_collector_reset_affects_singleton(self):
        """Test that reset() affects the singleton instance"""
        collector1 = get_profiling_collector()
        collector1.enable()
        collector1.get_stats()
        assert collector1.get_last_snapshot() is not None

        collector1.reset()

        collector2 = get_profiling_collector()
        assert collector2.get_last_snapshot() is None


class TestProfilingCollectorIntegration:
    """Integration tests for ProfilingCollector full workflows"""

    def test_typical_profiling_workflow(self, collector):
        """Test a typical profiling session workflow"""
        # Start disabled
        assert collector.is_enabled() is False
        assert collector.get_stats() is None

        # Enable and get stats
        collector.enable()
        stats1 = collector.get_stats()
        assert stats1 is not None
        assert collector.get_last_snapshot() == stats1

        # Get more stats
        stats2 = collector.get_stats()
        assert stats2 is not None
        assert collector.get_last_snapshot() == stats2

        # Disable
        collector.disable()
        assert collector.get_stats() is None
        # Snapshot still available
        assert collector.get_last_snapshot() == stats2

        # Reset
        collector.reset()
        assert collector.get_last_snapshot() is None

    def test_multiple_enable_disable_cycles(self, collector):
        """Test multiple enable/disable cycles"""
        for i in range(3):
            collector.enable()
            stats = collector.get_stats()
            assert stats is not None

            collector.disable()
            assert collector.get_stats() is None

            # Each cycle should still work
            assert collector.get_last_snapshot() is not None

    def test_stats_collection_while_enabled(self, collector):
        """Test collecting multiple stats snapshots while enabled"""
        collector.enable()

        snapshots = []
        for i in range(5):
            stats = collector.get_stats()
            assert stats is not None
            snapshots.append(stats)

        # All snapshots should be valid
        assert len(snapshots) == 5
        for snapshot in snapshots:
            assert isinstance(snapshot, ProfilingStats)

        # Last one should be current
        assert collector.get_last_snapshot() == snapshots[-1]
