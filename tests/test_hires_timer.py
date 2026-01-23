#!/usr/bin/env python3
"""
Integration tests for High-Resolution Timer
Tests kernel timer functionality via compiled test binary
"""

import subprocess
import os
from pathlib import Path
import pytest
import re


class TestHighResolutionTimer:
    """Test suite for high-resolution timer integration"""

    @pytest.fixture
    def kernel_dir(self):
        """Get the kernel directory path"""
        # Tests are in project_root/tests, kernel is in project_root/kernel
        test_dir = Path(__file__).parent
        kernel_dir = test_dir.parent / "kernel"
        return kernel_dir

    @pytest.fixture
    def test_binary_path(self, kernel_dir):
        """Get path to the compiled test binary"""
        return kernel_dir / "test" / "test_hires_timer"

    def test_timer_binary_exists(self, test_binary_path):
        """Test that the timer test binary exists"""
        assert test_binary_path.exists(), \
            f"Timer test binary not found at {test_binary_path}. " \
            "Run 'cd kernel/test && make test_hires_timer' to build it."

    def test_timer_binary_is_executable(self, test_binary_path):
        """Test that the timer test binary is executable"""
        if not test_binary_path.exists():
            pytest.skip("Timer test binary not found")

        assert os.access(test_binary_path, os.X_OK), \
            f"Timer test binary at {test_binary_path} is not executable"

    def test_timer_monotonicity(self, test_binary_path):
        """Test that timer is monotonically increasing"""
        if not test_binary_path.exists():
            pytest.skip("Timer test binary not found")

        result = subprocess.run(
            [str(test_binary_path)],
            capture_output=True,
            text=True,
            timeout=10
        )

        # Should exit successfully
        assert result.returncode == 0, \
            f"Timer test failed with return code {result.returncode}\n" \
            f"stdout: {result.stdout}\nstderr: {result.stderr}"

        # Check for monotonicity test pass
        assert "Monotonicity: PASSED" in result.stdout, \
            "Monotonicity test did not pass"

    def test_timer_resolution(self, test_binary_path):
        """Test that timer has sufficient resolution"""
        if not test_binary_path.exists():
            pytest.skip("Timer test binary not found")

        result = subprocess.run(
            [str(test_binary_path)],
            capture_output=True,
            text=True,
            timeout=10
        )

        assert result.returncode == 0, \
            f"Timer test failed with return code {result.returncode}"

        # Check for resolution test pass
        assert "Resolution: PASSED" in result.stdout, \
            "Resolution test did not pass"

        # Verify resolution is reported
        assert "Min delta:" in result.stdout, \
            "Resolution metrics not reported"

    def test_timer_accuracy(self, test_binary_path):
        """Test that timer measurements are accurate"""
        if not test_binary_path.exists():
            pytest.skip("Timer test binary not found")

        result = subprocess.run(
            [str(test_binary_path)],
            capture_output=True,
            text=True,
            timeout=15
        )

        assert result.returncode == 0, \
            f"Timer test failed with return code {result.returncode}"

        # Check for accuracy test pass
        output = result.stdout
        assert "Accuracy: PASSED" in output or \
               "Timer Accuracy: PASSED" in output, \
            "Accuracy test did not pass"

    def test_timer_all_tests_pass(self, test_binary_path):
        """Test that all timer tests complete successfully"""
        if not test_binary_path.exists():
            pytest.skip("Timer test binary not found")

        result = subprocess.run(
            [str(test_binary_path)],
            capture_output=True,
            text=True,
            timeout=15
        )

        # Should exit successfully (return code 0)
        assert result.returncode == 0, \
            f"Timer test suite failed with return code {result.returncode}\n" \
            f"Output:\n{result.stdout}\nErrors:\n{result.stderr}"

        # Count PASSED vs FAILED
        passed_count = result.stdout.count("PASSED")
        failed_count = result.stdout.count("FAILED")

        assert passed_count > 0, \
            "No tests reported as PASSED"
        assert failed_count == 0, \
            f"Some tests FAILED. Passed: {passed_count}, Failed: {failed_count}\n" \
            f"Output:\n{result.stdout}"

    def test_timer_overhead_measurement(self, test_binary_path):
        """Test that timer overhead is measured and reported"""
        if not test_binary_path.exists():
            pytest.skip("Timer test binary not found")

        result = subprocess.run(
            [str(test_binary_path)],
            capture_output=True,
            text=True,
            timeout=10
        )

        assert result.returncode == 0, \
            f"Timer test failed with return code {result.returncode}"

        # Check for overhead measurements
        output = result.stdout
        assert "overhead" in output.lower() or \
               "Timer Read Overhead" in output, \
            "Timer overhead not measured"

    def test_timer_stress_test(self, test_binary_path):
        """Test that timer handles stress testing"""
        if not test_binary_path.exists():
            pytest.skip("Timer test binary not found")

        result = subprocess.run(
            [str(test_binary_path)],
            capture_output=True,
            text=True,
            timeout=20
        )

        assert result.returncode == 0, \
            f"Timer test failed with return code {result.returncode}"

        # Check for stress test
        output = result.stdout
        assert "Stress" in output or "stress" in output, \
            "Stress test not found in output"

    def test_timer_no_errors_in_output(self, test_binary_path):
        """Test that timer test produces no error messages"""
        if not test_binary_path.exists():
            pytest.skip("Timer test binary not found")

        result = subprocess.run(
            [str(test_binary_path)],
            capture_output=True,
            text=True,
            timeout=15
        )

        # Check stderr is empty or minimal
        assert len(result.stderr) < 100, \
            f"Unexpected errors in stderr: {result.stderr}"

        # Check for error keywords in stdout
        error_keywords = ["error", "ERROR", "failed to", "FAIL:"]
        for keyword in error_keywords:
            # Allow "FAILED" in assertion messages but not actual errors
            if keyword.lower() != "failed":
                assert keyword not in result.stdout, \
                    f"Error keyword '{keyword}' found in output: {result.stdout}"

    def test_timer_architecture_detection(self, test_binary_path):
        """Test that timer detects and reports architecture correctly"""
        if not test_binary_path.exists():
            pytest.skip("Timer test binary not found")

        result = subprocess.run(
            [str(test_binary_path)],
            capture_output=True,
            text=True,
            timeout=10
        )

        assert result.returncode == 0, \
            f"Timer test failed with return code {result.returncode}"

        # Check for architecture reporting
        output = result.stdout
        assert "Architecture:" in output, \
            "Architecture not reported in test output"

        # Should detect one of the supported architectures
        arch_detected = any(arch in output for arch in ["x86_64", "i386", "aarch64", "arm64"])
        assert arch_detected, \
            "No supported architecture detected in output"

    def test_timer_microsecond_resolution_capability(self, test_binary_path):
        """Test that timer can measure microsecond intervals"""
        if not test_binary_path.exists():
            pytest.skip("Timer test binary not found")

        result = subprocess.run(
            [str(test_binary_path)],
            capture_output=True,
            text=True,
            timeout=15
        )

        assert result.returncode == 0, \
            f"Timer test failed with return code {result.returncode}"

        # Check for microsecond resolution verification
        output = result.stdout
        assert "microsecond" in output.lower() or \
               "1 microsecond" in output or \
               "Timer Resolution Verification" in output, \
            "Microsecond resolution capability not verified"


class TestTimerBuild:
    """Test suite for timer build system"""

    @pytest.fixture
    def kernel_dir(self):
        """Get the kernel directory path"""
        test_dir = Path(__file__).parent
        kernel_dir = test_dir.parent / "kernel"
        return kernel_dir

    def test_timer_source_exists(self, kernel_dir):
        """Test that timer test source file exists"""
        test_source = kernel_dir / "test" / "test_hires_timer.c"
        assert test_source.exists(), \
            f"Timer test source not found at {test_source}"

    def test_timer_can_be_built(self, kernel_dir):
        """Test that timer test binary can be compiled"""
        test_dir = kernel_dir / "test"
        test_source = test_dir / "test_hires_timer.c"

        if not test_source.exists():
            pytest.skip("Timer test source not found")

        # Try to compile the test
        result = subprocess.run(
            ["gcc", "-o", "/dev/null", str(test_source), "-lm"],
            capture_output=True,
            cwd=str(test_dir),
            timeout=30
        )

        # Compilation should succeed or we should skip if gcc not available
        if result.returncode != 0 and "gcc: not found" in result.stderr:
            pytest.skip("gcc not available")

        assert result.returncode == 0, \
            f"Timer test compilation failed:\n{result.stderr.decode()}"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
