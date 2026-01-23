"""
API Routes - FastAPI endpoints for EMBODIOS inference
"""

from typing import AsyncGenerator, Dict, Optional
from fastapi import APIRouter, Depends, HTTPException, status, Header
from fastapi.responses import StreamingResponse, Response
import json
import time
import uuid
import os
from prometheus_client import generate_latest, CONTENT_TYPE_LATEST

from .models import (
    CompletionRequest,
    CompletionResponse,
    CompletionChoice,
    CompletionUsage,
    CompletionStreamChunk,
    ModelUpdateRequest,
    ModelUpdateResponse,
    ModelStatusResponse,
    ModelInfo,
    ModelSwitchRequest,
    ModelSwitchResponse,
)
from ..core.inference import EMBODIOSInferenceEngine
from .metrics import get_metrics_collector
from .profiling import get_profiling_collector

# Global inference engine instance (will be set by server)
_inference_engine: Optional[EMBODIOSInferenceEngine] = None

router = APIRouter(prefix="/v1", tags=["completions"])
metrics_router = APIRouter(tags=["metrics"])
profiling_router = APIRouter(tags=["profiling"])


def set_inference_engine(engine: EMBODIOSInferenceEngine):
    """Set the global inference engine instance"""
    global _inference_engine
    _inference_engine = engine


def get_inference_engine() -> EMBODIOSInferenceEngine:
    """Dependency to get the inference engine"""
    if _inference_engine is None:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail="Inference engine not initialized"
        )
    return _inference_engine


def verify_auth_token(x_auth_token: Optional[str] = Header(None)) -> str:
    """
    Dependency to verify authentication token

    Checks X-Auth-Token header against EMBODIOS_AUTH_TOKEN environment variable.
    Returns the token if valid, raises 401 if invalid or missing.
    """
    # Check if token was provided
    if not x_auth_token:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Missing authentication token",
            headers={"WWW-Authenticate": "Bearer"}
        )

    # Get expected token from environment
    expected_token = os.environ.get("EMBODIOS_AUTH_TOKEN")

    # If no token is configured, reject all requests
    if not expected_token:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Authentication not configured on server",
            headers={"WWW-Authenticate": "Bearer"}
        )

    # Verify token matches
    if x_auth_token != expected_token:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid authentication token",
            headers={"WWW-Authenticate": "Bearer"}
        )

    return x_auth_token


def _tokenize_prompt(prompt: str) -> list[int]:
    """Convert prompt string to tokens (simplified tokenization)"""
    # Simple character-based tokenization for now
    # In production, this would use a proper tokenizer
    return [ord(c) % 256 for c in prompt]


def _detokenize_output(tokens: list[int]) -> str:
    """Convert output tokens back to string"""
    # Simple detokenization matching the tokenization above
    try:
        return ''.join(chr(t) if t < 256 else '?' for t in tokens)
    except (ValueError, OverflowError):
        return ''.join('?' for _ in tokens)


async def _generate_completion_stream(
    request: CompletionRequest,
    engine: EMBODIOSInferenceEngine
) -> AsyncGenerator[str, None]:
    """Generate streaming completion chunks in SSE format"""
    completion_id = f"cmpl-{uuid.uuid4().hex[:24]}"
    created = int(time.time())

    # Tokenize input
    prompt = request.prompt if isinstance(request.prompt, str) else request.prompt[0]
    input_tokens = _tokenize_prompt(prompt)

    # Run inference
    try:
        output_tokens, hw_operations = engine.inference(input_tokens)
    except Exception as e:
        # Send error event
        error_chunk = {
            "error": {
                "message": f"Inference failed: {str(e)}",
                "type": "inference_error"
            }
        }
        yield f"data: {json.dumps(error_chunk)}\n\n"
        return

    # Convert tokens to text
    completion_text = _detokenize_output(output_tokens)

    # Stream chunks (simulate streaming by chunking the output)
    chunk_size = max(1, len(completion_text) // 10)
    for i in range(0, len(completion_text), chunk_size):
        chunk_text = completion_text[i:i + chunk_size]

        chunk = CompletionStreamChunk(
            id=completion_id,
            object="text_completion",
            created=created,
            model=request.model,
            choices=[
                CompletionChoice(
                    text=chunk_text,
                    index=0,
                    logprobs=None,
                    finish_reason=None
                )
            ]
        )

        yield f"data: {chunk.model_dump_json()}\n\n"

    # Send final chunk with finish_reason
    final_chunk = CompletionStreamChunk(
        id=completion_id,
        object="text_completion",
        created=created,
        model=request.model,
        choices=[
            CompletionChoice(
                text="",
                index=0,
                logprobs=None,
                finish_reason="stop"
            )
        ]
    )
    yield f"data: {final_chunk.model_dump_json()}\n\n"
    yield "data: [DONE]\n\n"


@router.post("/completions")
async def create_completion(
    request: CompletionRequest,
    engine: EMBODIOSInferenceEngine = Depends(get_inference_engine)
):
    """
    Create a completion for the provided prompt

    OpenAI-compatible endpoint that supports both streaming and non-streaming responses.
    """
    # Handle streaming responses
    if request.stream:
        return StreamingResponse(
            _generate_completion_stream(request, engine),
            media_type="text/event-stream"
        )

    # Handle non-streaming responses
    completion_id = f"cmpl-{uuid.uuid4().hex[:24]}"
    created = int(time.time())

    # Tokenize input
    prompt = request.prompt if isinstance(request.prompt, str) else request.prompt[0]
    input_tokens = _tokenize_prompt(prompt)

    # Run inference
    try:
        output_tokens, hw_operations = engine.inference(input_tokens)
    except Exception as e:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"Inference failed: {str(e)}"
        )

    # Convert tokens to text
    completion_text = _detokenize_output(output_tokens)

    # Apply max_tokens limit
    if request.max_tokens and len(output_tokens) >= request.max_tokens:
        output_tokens = output_tokens[:request.max_tokens]
        completion_text = _detokenize_output(output_tokens)
        finish_reason = "length"
    else:
        finish_reason = "stop"

    # Build response
    response = CompletionResponse(
        id=completion_id,
        object="text_completion",
        created=created,
        model=request.model,
        choices=[
            CompletionChoice(
                text=completion_text,
                index=0,
                logprobs=None,
                finish_reason=finish_reason
            )
        ],
        usage=CompletionUsage(
            prompt_tokens=len(input_tokens),
            completion_tokens=len(output_tokens),
            total_tokens=len(input_tokens) + len(output_tokens)
        )
    )

    return response


@router.post("/models/upload")
async def upload_model(
    request: ModelUpdateRequest,
    token: str = Depends(verify_auth_token)
):
    """
    Upload a new model via OTA update

    Requires authentication via X-Auth-Token header.
    Supports both URL-based download and direct base64-encoded upload.
    Performs atomic update with rollback on failure.
    """
    try:
        # Import OTAUpdater (lazy import to avoid circular dependencies)
        from ..models.ota_updater import OTAUpdater

        # Initialize updater
        updater = OTAUpdater()

        # Perform update based on request type
        if request.model_url:
            # Download from URL
            model_id = updater.update_from_url(
                url=request.model_url,
                checksum=request.checksum,
                model_name=request.model_name
            )
        elif request.model_data:
            # Direct upload (base64 encoded)
            import base64
            model_bytes = base64.b64decode(request.model_data)

            # Save to temporary file and update
            import tempfile
            with tempfile.NamedTemporaryFile(delete=False, suffix=".gguf") as tmp_file:
                tmp_file.write(model_bytes)
                tmp_path = tmp_file.name

            try:
                model_id = updater.update_from_file(
                    file_path=tmp_path,
                    checksum=request.checksum,
                    model_name=request.model_name
                )
            finally:
                # Clean up temporary file
                if os.path.exists(tmp_path):
                    os.unlink(tmp_path)
        else:
            raise HTTPException(
                status_code=status.HTTP_400_BAD_REQUEST,
                detail="Either model_url or model_data must be provided"
            )

        # Return success response
        return ModelUpdateResponse(
            status="success",
            model_id=str(model_id),
            message=f"Model uploaded successfully with ID: {model_id}",
            timestamp=int(time.time())
        )

    except ValueError as e:
        # Validation errors (checksum mismatch, invalid format, etc.)
        raise HTTPException(
            status_code=status.HTTP_400_BAD_REQUEST,
            detail=str(e)
        )
    except Exception as e:
        # Internal errors
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"Model update failed: {str(e)}"
        )


@router.get("/models/status")
async def get_model_status(
    engine: EMBODIOSInferenceEngine = Depends(get_inference_engine)
):
    """
    Get status of currently loaded models

    Returns information about loaded models, active model, and memory usage.
    """
    try:
        # Build model info for currently loaded model
        loaded_models = []
        active_model_id = None

        if engine.model_loaded:
            # Extract model information from engine
            model_name = "embodios-model"
            if engine.config and "model_name" in engine.config:
                model_name = engine.config["model_name"]

            # Calculate model size if available
            size_bytes = None
            if engine.weights_data:
                size_bytes = len(engine.weights_data)

            # Get last update timestamp if available
            last_update = None
            if engine.config and "last_update" in engine.config:
                last_update = engine.config["last_update"]

            # Generate model ID
            model_id = "model-primary"
            active_model_id = model_id

            model_info = ModelInfo(
                model_id=model_id,
                model_name=model_name,
                size_bytes=size_bytes,
                loaded=True,
                last_update=last_update
            )
            loaded_models.append(model_info)

        # Calculate memory usage
        memory_usage_mb = None
        if engine.weights_data:
            memory_usage_mb = len(engine.weights_data) / (1024 * 1024)

        # Build registry stats
        registry_stats = {
            "total_models": len(loaded_models),
            "engine_initialized": engine.model_loaded
        }

        # Return status response
        return ModelStatusResponse(
            loaded_models=loaded_models,
            active_model=active_model_id,
            memory_usage_mb=memory_usage_mb,
            registry_stats=registry_stats
        )

    except Exception as e:
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"Failed to get model status: {str(e)}"
        )


@router.post("/models/switch")
async def switch_model(
    request: ModelSwitchRequest,
    token: str = Depends(verify_auth_token),
    engine: EMBODIOSInferenceEngine = Depends(get_inference_engine)
):
    """
    Switch to a different model

    Requires authentication via X-Auth-Token header.
    Switches the active model to the specified model_id.
    """
    try:
        # Import OTAUpdater to check available models
        from ..models.ota_updater import OTAUpdater

        # Initialize updater
        updater = OTAUpdater()

        # Get list of available models
        available_models = updater.list_models()

        # Convert model_id to string for consistency
        model_id_str = str(request.model_id)

        # Check if model exists in registry
        if model_id_str not in available_models and not available_models:
            # No models in registry, but allow switching if engine has a model loaded
            if not engine.model_loaded:
                raise HTTPException(
                    status_code=status.HTTP_404_NOT_FOUND,
                    detail=f"Model not found: {model_id_str}"
                )
            model_id_str = "model-primary"

        elif model_id_str not in available_models:
            # Model not found in registry
            raise HTTPException(
                status_code=status.HTTP_404_NOT_FOUND,
                detail=f"Model not found: {model_id_str}. Available models: {', '.join(available_models.keys())}"
            )

        # Model switching requires kernel integration not yet available
        # Return 501 to be honest about limitation
        raise HTTPException(
            status_code=status.HTTP_501_NOT_IMPLEMENTED,
            detail={
                "error": "Model switching requires kernel model_registry_switch() API integration",
                "status": "not_implemented",
                "planned": "future release",
                "workaround": "Restart service with desired model to change active model"
            }
        )

    except HTTPException:
        # Re-raise HTTP exceptions
        raise
    except Exception as e:
        # Internal errors
        raise HTTPException(
            status_code=status.HTTP_500_INTERNAL_SERVER_ERROR,
            detail=f"Model switch failed: {str(e)}"
        )


@metrics_router.get("/metrics")
async def metrics():
    """
    Prometheus metrics endpoint

    Returns metrics in Prometheus text exposition format for scraping.
    Includes inference request counts, latency histograms, memory usage, and uptime.
    """
    # Update system metrics before generating output
    collector = get_metrics_collector()
    collector.update_system_metrics()

    # Generate Prometheus text format
    metrics_output = generate_latest()

    return Response(content=metrics_output, media_type=CONTENT_TYPE_LATEST)


@profiling_router.get("/api/profiling/live")
async def get_live_profiling():
    """
    Get current live profiling snapshot

    Returns real-time profiling data including function-level timing,
    memory allocation tracking, and hot path information.
    Returns mock data until kernel integration is complete.
    """
    collector = get_profiling_collector()

    # Enable profiling if not already enabled
    if not collector.is_enabled():
        collector.enable()

    # Get current profiling statistics
    stats = collector.get_stats()

    if stats is None:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail="Profiling data not available"
        )

    return stats


@profiling_router.get("/api/profiling/stats")
async def get_profiling_stats():
    """
    Get aggregated profiling statistics

    Returns aggregated profiling data with summary information,
    function statistics, memory tracking, and hot paths.
    Returns mock data until kernel integration is complete.
    """
    collector = get_profiling_collector()

    # Get profiling statistics (returns last snapshot if available)
    stats = collector.get_last_snapshot()

    # If no snapshot exists, get fresh stats
    if stats is None:
        if not collector.is_enabled():
            collector.enable()
        stats = collector.get_stats()

    if stats is None:
        raise HTTPException(
            status_code=status.HTTP_503_SERVICE_UNAVAILABLE,
            detail="Profiling statistics not available"
        )

    return stats
