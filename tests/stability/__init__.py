"""
Stability testing suite for EMBODIOS long-running tests.

This package provides infrastructure for testing EMBODIOS stability
over extended periods (hours to days), monitoring for memory leaks,
performance degradation, and crashes.
"""

from .config import StabilityConfig

__all__ = ['StabilityConfig']
