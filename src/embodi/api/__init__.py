"""
EMBODIOS API - REST API for inference requests
"""

from .models import (
    CompletionRequest,
    CompletionResponse,
    CompletionChoice,
    CompletionUsage,
)

__all__ = [
    "CompletionRequest",
    "CompletionResponse",
    "CompletionChoice",
    "CompletionUsage",
]
