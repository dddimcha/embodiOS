"""
Metrics Middleware - Automatic request tracking for Prometheus metrics
"""

import time
from typing import Callable
from fastapi import Request, Response
from starlette.middleware.base import BaseHTTPMiddleware
from starlette.types import ASGIApp

from ..metrics import get_metrics_collector


class MetricsMiddleware(BaseHTTPMiddleware):
    """
    FastAPI middleware for automatic request metrics collection.

    Tracks request duration, counts, and in-progress requests with minimal
    overhead (< 1ms per request). Integrates with MetricsCollector to expose
    Prometheus-compatible metrics.

    Usage:
        app = FastAPI()
        app.add_middleware(MetricsMiddleware)
    """

    def __init__(self, app: ASGIApp):
        """
        Initialize metrics middleware.

        Args:
            app: ASGI application instance
        """
        super().__init__(app)
        self._metrics = get_metrics_collector()

    async def dispatch(self, request: Request, call_next: Callable) -> Response:
        """
        Process request and record metrics.

        Args:
            request: Incoming HTTP request
            call_next: Next middleware/route handler

        Returns:
            HTTP response from downstream handlers
        """
        # Skip metrics collection for /metrics endpoint to avoid recursion
        if request.url.path == "/metrics":
            return await call_next(request)

        # Record start time
        start_time = time.time()

        # Increment in-progress counter
        self._metrics.inference_requests_in_progress.inc()

        try:
            # Process request
            response = await call_next(request)

            # Calculate duration
            duration = time.time() - start_time

            # Record metrics
            self._metrics.record_request(
                method=request.method,
                endpoint=request.url.path,
                status=response.status_code,
                duration=duration
            )

            return response

        except Exception as e:
            # Calculate duration even on error
            duration = time.time() - start_time

            # Record error metrics with 500 status
            self._metrics.record_request(
                method=request.method,
                endpoint=request.url.path,
                status=500,
                duration=duration
            )

            # Re-raise exception to maintain normal error handling
            raise

        finally:
            # Always decrement in-progress counter
            self._metrics.inference_requests_in_progress.dec()
