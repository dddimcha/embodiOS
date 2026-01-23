"""
API Middleware - FastAPI middleware components for EMBODIOS API
"""

from .metrics_middleware import MetricsMiddleware

__all__ = ["MetricsMiddleware"]
