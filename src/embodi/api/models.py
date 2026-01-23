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


class ModelUpdateRequest(BaseModel):
    """Request format for OTA model update endpoint"""

    model_config = {"protected_namespaces": ()}

    model_url: Optional[str] = Field(
        default=None,
        description="URL to download model file from"
    )
    model_data: Optional[str] = Field(
        default=None,
        description="Base64-encoded model file data for direct upload"
    )
    checksum: str = Field(
        description="SHA256 checksum of the model file for verification"
    )
    model_name: Optional[str] = Field(
        default=None,
        description="Optional name for the model"
    )


class ModelUpdateResponse(BaseModel):
    """Response format for OTA model update endpoint"""

    model_config = {"protected_namespaces": ()}

    status: str = Field(
        description="Update status: 'success', 'failed', or 'in_progress'"
    )
    model_id: Optional[str] = Field(
        default=None,
        description="Model ID assigned to the updated model (on success)"
    )
    message: str = Field(
        description="Human-readable message about the update operation"
    )
    timestamp: int = Field(
        default_factory=lambda: int(time.time()),
        description="Unix timestamp of the update operation"
    )


class ModelInfo(BaseModel):
    """Information about a loaded model"""

    model_config = {"protected_namespaces": ()}

    model_id: str = Field(
        description="Unique identifier for the model"
    )
    model_name: str = Field(
        description="Human-readable model name"
    )
    size_bytes: Optional[int] = Field(
        default=None,
        description="Model file size in bytes"
    )
    loaded: bool = Field(
        description="Whether the model is currently loaded in memory"
    )
    last_update: Optional[int] = Field(
        default=None,
        description="Unix timestamp of last update (for OTA models)"
    )


class ModelStatusResponse(BaseModel):
    """Response format for model status endpoint"""

    loaded_models: List[ModelInfo] = Field(
        description="List of models loaded in the registry"
    )
    active_model: Optional[str] = Field(
        default=None,
        description="ID of the currently active model"
    )
    memory_usage_mb: Optional[float] = Field(
        default=None,
        description="Total memory usage by models in megabytes"
    )
    registry_stats: Optional[Dict] = Field(
        default=None,
        description="Additional registry statistics"
    )


class ModelSwitchRequest(BaseModel):
    """Request format for model switch endpoint"""

    model_config = {"protected_namespaces": ()}

    model_id: Union[int, str] = Field(
        description="Model ID (integer index or string identifier) to switch to"
    )


class ModelSwitchResponse(BaseModel):
    """Response format for model switch endpoint"""

    model_config = {"protected_namespaces": ()}

    status: str = Field(
        description="Switch status: 'success' or 'failed'"
    )
    active_model: str = Field(
        description="ID of the currently active model after switch"
    )
    message: str = Field(
        description="Human-readable message about the switch operation"
    )
    timestamp: int = Field(
        default_factory=lambda: int(time.time()),
        description="Unix timestamp of the switch operation"
    )
