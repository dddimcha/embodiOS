"""
API Data Models - OpenAI-compatible request/response formats
"""

from typing import Dict, List, Optional, Union
from pydantic import BaseModel, Field
import time


class CompletionRequest(BaseModel):
    """Request format for /v1/completions endpoint (OpenAI compatible)"""

    model: str = Field(
        description="ID of the model to use"
    )
    prompt: Union[str, List[str]] = Field(
        description="The prompt(s) to generate completions for"
    )
    max_tokens: Optional[int] = Field(
        default=16,
        description="Maximum number of tokens to generate"
    )
    temperature: Optional[float] = Field(
        default=1.0,
        ge=0.0,
        le=2.0,
        description="Sampling temperature (0.0 to 2.0)"
    )
    top_p: Optional[float] = Field(
        default=1.0,
        ge=0.0,
        le=1.0,
        description="Nucleus sampling probability"
    )
    n: Optional[int] = Field(
        default=1,
        ge=1,
        description="Number of completions to generate"
    )
    stream: Optional[bool] = Field(
        default=False,
        description="Whether to stream results via SSE"
    )
    logprobs: Optional[int] = Field(
        default=None,
        ge=0,
        le=5,
        description="Include log probabilities on the most likely tokens"
    )
    echo: Optional[bool] = Field(
        default=False,
        description="Echo back the prompt in addition to the completion"
    )
    stop: Optional[Union[str, List[str]]] = Field(
        default=None,
        description="Up to 4 sequences where the API will stop generating"
    )
    presence_penalty: Optional[float] = Field(
        default=0.0,
        ge=-2.0,
        le=2.0,
        description="Penalty for new tokens based on presence in text so far"
    )
    frequency_penalty: Optional[float] = Field(
        default=0.0,
        ge=-2.0,
        le=2.0,
        description="Penalty for new tokens based on frequency in text so far"
    )
    best_of: Optional[int] = Field(
        default=1,
        ge=1,
        description="Generate best_of completions and return the best"
    )
    logit_bias: Optional[Dict[str, float]] = Field(
        default=None,
        description="Modify likelihood of specified tokens appearing"
    )
    user: Optional[str] = Field(
        default=None,
        description="Unique identifier for end-user"
    )


class CompletionUsage(BaseModel):
    """Token usage information"""

    prompt_tokens: int = Field(
        description="Number of tokens in the prompt"
    )
    completion_tokens: int = Field(
        description="Number of tokens in the completion"
    )
    total_tokens: int = Field(
        description="Total tokens used (prompt + completion)"
    )


class CompletionChoice(BaseModel):
    """A single completion choice"""

    text: str = Field(
        description="Generated text completion"
    )
    index: int = Field(
        description="Index of this completion in the list"
    )
    logprobs: Optional[Dict] = Field(
        default=None,
        description="Log probability information"
    )
    finish_reason: Optional[str] = Field(
        default=None,
        description="Reason completion finished: 'stop', 'length', or null"
    )


class CompletionResponse(BaseModel):
    """Response format for /v1/completions endpoint (OpenAI compatible)"""

    id: str = Field(
        description="Unique identifier for this completion"
    )
    object: str = Field(
        default="text_completion",
        description="Object type, always 'text_completion'"
    )
    created: int = Field(
        default_factory=lambda: int(time.time()),
        description="Unix timestamp of when completion was created"
    )
    model: str = Field(
        description="Model used for completion"
    )
    choices: List[CompletionChoice] = Field(
        description="List of completion choices"
    )
    usage: Optional[CompletionUsage] = Field(
        default=None,
        description="Token usage statistics"
    )


class CompletionStreamChunk(BaseModel):
    """Streaming response chunk for SSE (Server-Sent Events)"""

    id: str = Field(
        description="Unique identifier for this completion"
    )
    object: str = Field(
        default="text_completion",
        description="Object type"
    )
    created: int = Field(
        default_factory=lambda: int(time.time()),
        description="Unix timestamp"
    )
    model: str = Field(
        description="Model used for completion"
    )
    choices: List[CompletionChoice] = Field(
        description="List of completion choices with partial text"
    )
