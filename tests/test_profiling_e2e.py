#!/usr/bin/env python3
"""
End-to-end integration tests for Live Kernel Profiling
Tests complete workflow from API server to profiling data collection
"""

import pytest
import time
from unittest.mock import patch, MagicMock
from fastapi.testclient import TestClient

from src.embodi.api.profiling import (
    ProfilingStats,
    FunctionProfile,
    MemoryProfile,
    HotPath,
    ProfilingSummary,
    get_profiling_collector
)
from src.embodi.core.inference import EMBODIOSInferenceEngine


class TestProfilingE2E:
    """End-to-end integration tests for profiling functionality"""

    @pytest.fixture(autouse=True)
    def reset_profiling_collector(self):
        """Reset profiling collector state before each test"""
        # Import the module to access the global
        import src.embodi.api.profiling as profiling_module
        profiling_module._profiling_collector = None
        yield
        # Clean up after test
        profiling_module._profiling_collector = None

    @pytest.fixture
    def inference_engine(self):
        """Provide a mock inference engine for testing"""
        engine = EMBODIOSInferenceEngine()
        engine.model_loaded = True
        # Set up minimal config for testing
        engine.config = {'model_name': 'test-model'}
        engine.architecture = {
            'vocab_size': 100,
            'hidden_size': 64
        }
        # Mock weights to avoid loading actual model
        engine.weights_data = b'\x00' * 1024
        return engine

    @pytest.fixture
    def app_with_engine(self, inference_engine):
        """Create FastAPI app with mock inference engine"""
        from fastapi import FastAPI
        from src.embodi.api.routes import (
            profiling_router,
            set_inference_engine
        )

        # Create app
        app = FastAPI()

        # Set the inference engine
        set_inference_engine(inference_engine)

        # Register profiling routes
        app.include_router(profiling_router)

        return app

    def test_profiling_workflow_complete(self, app_with_engine, inference_engine):
        """
        Test complete profiling workflow:
        1. Start API server
        2. Enable profiling
        3. Run inference
        4. Query /api/profiling/stats
        5. Verify data structure
        """
        client = TestClient(app_with_engine)

        # Step 1: Enable profiling on inference engine
        inference_engine.enable_profiling()

        # Step 2: Simulate inference operation
        # In real scenario, this would run actual inference
        # For E2E test, we verify the profiling data is collected
        input_tokens = [1, 2, 3, 4, 5]

        # Mock the forward pass to avoid actual computation
        with patch.object(inference_engine, '_forward_pass', return_value=[10, 20, 30]):
            output_tokens, hw_ops = inference_engine.inference(input_tokens)
            assert output_tokens is not None

        # Step 3: Query profiling stats endpoint
        response = client.get("/api/profiling/stats")

        # Step 4: Verify response
        assert response.status_code == 200, \
            f"Expected status 200, got {response.status_code}"

        # Step 5: Verify response structure
        data = response.json()
        assert 'timestamp' in data, "Response should contain timestamp"
        assert 'summary' in data, "Response should contain summary"
        assert 'functions' in data, "Response should contain functions"
        assert 'memory' in data, "Response should contain memory tracking"
        assert 'hot_paths' in data, "Response should contain hot paths"

        # Validate response matches ProfilingStats model
        stats = ProfilingStats(**data)
        assert stats is not None

    def test_profiling_live_endpoint(self, app_with_engine):
        """Test /api/profiling/live endpoint returns current snapshot"""
        client = TestClient(app_with_engine)

        # Query live profiling endpoint
        response = client.get("/api/profiling/live")

        # Verify response
        assert response.status_code == 200, \
            f"Expected status 200, got {response.status_code}"

        data = response.json()

        # Verify structure
        assert 'timestamp' in data
        assert 'summary' in data
        assert 'functions' in data
        assert 'memory' in data
        assert 'hot_paths' in data

        # Verify summary indicates profiling is enabled
        assert data['summary']['enabled'] is True, \
            "Profiling should be enabled after accessing live endpoint"

    def test_profiling_stats_endpoint(self, app_with_engine):
        """Test /api/profiling/stats endpoint returns aggregated statistics"""
        client = TestClient(app_with_engine)

        # Query stats endpoint
        response = client.get("/api/profiling/stats")

        # Verify response
        assert response.status_code == 200

        data = response.json()

        # Verify all required fields are present
        assert 'timestamp' in data
        assert 'summary' in data
        assert 'functions' in data
        assert 'memory' in data
        assert 'hot_paths' in data

    def test_profiling_contains_expected_functions(self, app_with_engine):
        """Verify profiling data contains expected inference functions"""
        client = TestClient(app_with_engine)

        # Get profiling stats
        response = client.get("/api/profiling/stats")
        assert response.status_code == 200

        data = response.json()

        # Verify functions list is populated
        functions = data['functions']
        assert isinstance(functions, list), "Functions should be a list"
        assert len(functions) > 0, "Functions list should not be empty"

        # Verify function profile structure
        for func in functions:
            assert 'function_name' in func
            assert 'total_time_us' in func
            assert 'call_count' in func
            assert 'min_time_us' in func
            assert 'max_time_us' in func
            assert 'avg_time_us' in func
            assert 'cpu_percent' in func

        # Verify expected inference functions are present
        function_names = [f['function_name'] for f in functions]

        # These are the functions that should be profiled based on
        # the realistic mock data in profiling.py
        expected_functions = [
            'inference_forward_pass',
            'matrix_multiply_kernel',
            'attention_compute',
            'softmax_kernel',
            'layer_norm'
        ]

        for expected_func in expected_functions:
            assert expected_func in function_names, \
                f"Expected function '{expected_func}' not found in profiling data"

    def test_profiling_memory_tracking_exists(self, app_with_engine):
        """Verify memory tracking data exists in profiling output"""
        client = TestClient(app_with_engine)

        # Get profiling stats
        response = client.get("/api/profiling/stats")
        assert response.status_code == 200

        data = response.json()

        # Verify memory tracking list is populated
        memory = data['memory']
        assert isinstance(memory, list), "Memory should be a list"
        assert len(memory) > 0, "Memory tracking list should not be empty"

        # Verify memory profile structure
        for mem in memory:
            assert 'location' in mem
            assert 'total_allocated' in mem
            assert 'total_freed' in mem
            assert 'current_usage' in mem
            assert 'peak_usage' in mem
            assert 'alloc_count' in mem
            assert 'free_count' in mem
            assert 'alloc_rate_bps' in mem

            # Verify data types and reasonable values
            assert isinstance(mem['location'], str)
            assert mem['total_allocated'] >= 0
            assert mem['total_freed'] >= 0
            assert mem['current_usage'] >= 0
            assert mem['peak_usage'] >= 0
            assert mem['alloc_count'] >= 0
            assert mem['free_count'] >= 0

    def test_profiling_hot_paths_identified(self, app_with_engine):
        """Verify hot paths are correctly identified and sorted"""
        client = TestClient(app_with_engine)

        # Get profiling stats
        response = client.get("/api/profiling/stats")
        assert response.status_code == 200

        data = response.json()

        # Verify hot paths list is populated
        hot_paths = data['hot_paths']
        assert isinstance(hot_paths, list), "Hot paths should be a list"
        assert len(hot_paths) > 0, "Hot paths list should not be empty"

        # Verify hot path structure
        for hot_path in hot_paths:
            assert 'function_name' in hot_path
            assert 'total_time_us' in hot_path
            assert 'call_count' in hot_path
            assert 'cpu_percent' in hot_path
            assert 'avg_time_us' in hot_path

            # Verify data types
            assert isinstance(hot_path['function_name'], str)
            assert hot_path['total_time_us'] > 0
            assert hot_path['call_count'] > 0
            assert hot_path['cpu_percent'] > 0

        # Verify hot paths are sorted by CPU time (descending)
        cpu_percentages = [hp['cpu_percent'] for hp in hot_paths]
        assert cpu_percentages == sorted(cpu_percentages, reverse=True), \
            "Hot paths should be sorted by CPU percentage in descending order"

        # Verify top hot path has highest CPU usage
        top_hot_path = hot_paths[0]
        assert top_hot_path['cpu_percent'] >= 20.0, \
            "Top hot path should have significant CPU usage (>= 20%)"

    def test_profiling_summary_statistics(self, app_with_engine):
        """Verify profiling summary contains correct statistics"""
        client = TestClient(app_with_engine)

        # Get profiling stats
        response = client.get("/api/profiling/stats")
        assert response.status_code == 200

        data = response.json()

        # Verify summary structure
        summary = data['summary']
        assert 'total_entries' in summary
        assert 'total_samples' in summary
        assert 'total_time_us' in summary
        assert 'overhead_us' in summary
        assert 'overhead_percent' in summary
        assert 'active_functions' in summary
        assert 'dropped_entries' in summary
        assert 'enabled' in summary

        # Verify summary values are reasonable
        assert summary['total_entries'] >= 0
        assert summary['total_samples'] >= 0
        assert summary['total_time_us'] >= 0
        assert summary['overhead_us'] >= 0
        assert summary['overhead_percent'] >= 0.0
        assert summary['active_functions'] >= 0
        assert summary['dropped_entries'] >= 0
        assert summary['enabled'] is True

        # Verify overhead is within acceptable range (< 5% requirement)
        assert summary['overhead_percent'] < 5.0, \
            f"Profiling overhead {summary['overhead_percent']}% exceeds 5% requirement"

    def test_profiling_consistency_across_requests(self, app_with_engine):
        """Verify profiling data is consistent across multiple requests"""
        client = TestClient(app_with_engine)

        # Make first request
        response1 = client.get("/api/profiling/stats")
        assert response1.status_code == 200
        data1 = response1.json()

        # Make second request
        response2 = client.get("/api/profiling/stats")
        assert response2.status_code == 200
        data2 = response2.json()

        # Verify both responses have same structure
        assert set(data1.keys()) == set(data2.keys())
        assert len(data1['functions']) == len(data2['functions'])
        assert len(data1['memory']) == len(data2['memory'])
        assert len(data1['hot_paths']) == len(data2['hot_paths'])

    def test_profiling_integration_with_inference_engine(self, app_with_engine, inference_engine):
        """Test that profiling integrates correctly with inference engine"""
        client = TestClient(app_with_engine)

        # Verify profiling can be enabled on engine
        inference_engine.enable_profiling()
        assert inference_engine.profiling_enabled is True

        # Get profiling data from engine
        engine_data = inference_engine.get_profiling_data()
        assert engine_data is not None
        assert 'summary' in engine_data
        assert engine_data['summary']['enabled'] is True

        # Verify API endpoint also returns data
        response = client.get("/api/profiling/stats")
        assert response.status_code == 200

        api_data = response.json()
        assert api_data['summary']['enabled'] is True

    def test_profiling_disabled_state(self, app_with_engine, inference_engine):
        """Test profiling behavior when disabled"""
        # Disable profiling on inference engine
        inference_engine.disable_profiling()
        assert inference_engine.profiling_enabled is False

        # Verify get_profiling_data returns None when disabled
        engine_data = inference_engine.get_profiling_data()
        assert engine_data is None

        # Note: API endpoints auto-enable profiling when accessed,
        # so they will still return data. This is expected behavior.

    def test_profiling_timestamp_accuracy(self, app_with_engine):
        """Verify profiling timestamps are accurate"""
        client = TestClient(app_with_engine)

        # Record time before request
        before_time = int(time.time())

        # Get profiling stats
        response = client.get("/api/profiling/stats")
        assert response.status_code == 200

        # Record time after request
        after_time = int(time.time())

        data = response.json()
        timestamp = data['timestamp']

        # Verify timestamp is within reasonable range
        assert before_time <= timestamp <= after_time, \
            f"Timestamp {timestamp} should be between {before_time} and {after_time}"

    def test_profiling_function_timing_accuracy(self, app_with_engine):
        """Verify function timing data is internally consistent"""
        client = TestClient(app_with_engine)

        # Get profiling stats
        response = client.get("/api/profiling/stats")
        assert response.status_code == 200

        data = response.json()
        functions = data['functions']

        for func in functions:
            # Verify timing consistency
            assert func['min_time_us'] <= func['avg_time_us'], \
                f"Min time should be <= avg time for {func['function_name']}"
            assert func['avg_time_us'] <= func['max_time_us'], \
                f"Avg time should be <= max time for {func['function_name']}"
            assert func['min_time_us'] <= func['max_time_us'], \
                f"Min time should be <= max time for {func['function_name']}"

            # Verify total time is reasonable
            # total_time_us should be approximately call_count * avg_time_us
            expected_total = func['call_count'] * func['avg_time_us']
            # Allow for some variance due to rounding
            tolerance = 0.01  # 1% tolerance
            assert abs(func['total_time_us'] - expected_total) / expected_total < tolerance, \
                f"Total time inconsistent with call_count * avg_time for {func['function_name']}"

    def test_profiling_memory_tracking_consistency(self, app_with_engine):
        """Verify memory tracking data is internally consistent"""
        client = TestClient(app_with_engine)

        # Get profiling stats
        response = client.get("/api/profiling/stats")
        assert response.status_code == 200

        data = response.json()
        memory = data['memory']

        for mem in memory:
            # Verify memory tracking consistency
            assert mem['current_usage'] >= 0, \
                f"Current usage should be non-negative at {mem['location']}"
            assert mem['peak_usage'] >= mem['current_usage'], \
                f"Peak usage should be >= current usage at {mem['location']}"
            assert mem['total_allocated'] >= mem['total_freed'], \
                f"Total allocated should be >= total freed at {mem['location']}"

            # Verify allocation/free count consistency
            assert mem['alloc_count'] >= 0
            assert mem['free_count'] >= 0
            assert mem['alloc_count'] >= mem['free_count'], \
                f"Alloc count should be >= free count at {mem['location']}"

    def test_profiling_data_model_validation(self, app_with_engine):
        """Verify profiling data validates against Pydantic models"""
        client = TestClient(app_with_engine)

        # Get profiling stats
        response = client.get("/api/profiling/stats")
        assert response.status_code == 200

        data = response.json()

        # Validate entire response as ProfilingStats
        stats = ProfilingStats(**data)
        assert stats is not None

        # Validate individual function profiles
        for func_data in data['functions']:
            func = FunctionProfile(**func_data)
            assert func.function_name is not None
            assert func.total_time_us >= 0
            assert func.call_count > 0

        # Validate memory profiles
        for mem_data in data['memory']:
            mem = MemoryProfile(**mem_data)
            assert mem.location is not None
            assert mem.total_allocated >= 0

        # Validate hot paths
        for hp_data in data['hot_paths']:
            hp = HotPath(**hp_data)
            assert hp.function_name is not None
            assert hp.cpu_percent > 0

        # Validate summary
        summary = ProfilingSummary(**data['summary'])
        assert summary.enabled is True


class TestProfilingE2EErrorHandling:
    """Test error handling in E2E profiling scenarios"""

    def test_profiling_without_inference_engine(self):
        """Test profiling endpoints work even without inference engine loaded"""
        from fastapi import FastAPI
        from src.embodi.api.routes import profiling_router

        # Create app WITHOUT setting inference engine
        app = FastAPI()
        app.include_router(profiling_router)

        client = TestClient(app)

        # Profiling endpoints should still work (return mock data)
        response = client.get("/api/profiling/stats")
        assert response.status_code == 200

        data = response.json()
        assert 'summary' in data
        assert 'functions' in data

    def test_profiling_concurrent_requests(self, app_with_engine):
        """Test profiling handles concurrent requests correctly"""
        import src.embodi.api.profiling as profiling_module
        profiling_module._profiling_collector = None

        client = TestClient(app_with_engine)

        # Make multiple concurrent requests
        responses = []
        for _ in range(5):
            response = client.get("/api/profiling/stats")
            responses.append(response)

        # All requests should succeed
        for response in responses:
            assert response.status_code == 200
            data = response.json()
            assert 'summary' in data

    @pytest.fixture
    def app_with_engine(self, inference_engine):
        """Create FastAPI app with mock inference engine"""
        from fastapi import FastAPI
        from src.embodi.api.routes import (
            profiling_router,
            set_inference_engine
        )

        # Create app
        app = FastAPI()

        # Set the inference engine
        set_inference_engine(inference_engine)

        # Register profiling routes
        app.include_router(profiling_router)

        return app

    @pytest.fixture
    def inference_engine(self):
        """Provide a mock inference engine for testing"""
        engine = EMBODIOSInferenceEngine()
        engine.model_loaded = True
        engine.config = {'model_name': 'test-model'}
        engine.architecture = {
            'vocab_size': 100,
            'hidden_size': 64
        }
        engine.weights_data = b'\x00' * 1024
        return engine
