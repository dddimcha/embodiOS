"""
API Routes - FastAPI endpoints for EMBODIOS inference
"""

from typing import AsyncGenerator, Dict, Optional
from fastapi import APIRouter, Depends, HTTPException, status
from fastapi.responses import StreamingResponse
import json
import time
import uuid

from .models import (
    CompletionRequest,
    CompletionResponse,
    CompletionChoice,
    CompletionUsage,
    CompletionStreamChunk,
)
from ..core.inference import EMBODIOSInferenceEngine

# Global inference engine instance (will be set by server)
_inference_engine: Optional[EMBODIOSInferenceEngine] = None

router = APIRouter(prefix="/v1", tags=["completions"])


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
