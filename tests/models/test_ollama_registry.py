#!/usr/bin/env python3
"""Unit tests for OllamaRegistry"""

import pytest

from src.embodi.models.ollama_registry import OllamaRegistry


@pytest.fixture
def registry():
    """Create OllamaRegistry instance"""
    return OllamaRegistry()


def test_resolve_known_model(registry):
    """Test resolving a known Ollama model name"""
    repo_id = registry.resolve("tinyllama")
    assert repo_id == "TinyLlama/TinyLlama-1.1B-Chat-v1.0"


def test_resolve_case_insensitive(registry):
    """Test that resolution is case-insensitive"""
    assert registry.resolve("TinyLlama") == "TinyLlama/TinyLlama-1.1B-Chat-v1.0"
    assert registry.resolve("TINYLLAMA") == "TinyLlama/TinyLlama-1.1B-Chat-v1.0"
    assert registry.resolve("tinyllama") == "TinyLlama/TinyLlama-1.1B-Chat-v1.0"


def test_resolve_with_whitespace(registry):
    """Test that whitespace is stripped during resolution"""
    assert registry.resolve("  tinyllama  ") == "TinyLlama/TinyLlama-1.1B-Chat-v1.0"
    assert registry.resolve("\ttinyllama\n") == "TinyLlama/TinyLlama-1.1B-Chat-v1.0"


def test_resolve_unknown_model(registry):
    """Test resolving an unknown model name returns None"""
    result = registry.resolve("unknown-model")
    assert result is None


def test_resolve_huggingface_repo_id(registry):
    """Test that HuggingFace repo IDs (with slash) pass through"""
    repo_id = "custom-org/custom-model"
    result = registry.resolve(repo_id)
    assert result == repo_id


def test_resolve_huggingface_repo_id_not_in_registry(registry):
    """Test that unknown HuggingFace repo IDs pass through"""
    repo_id = "someorg/somemodel"
    result = registry.resolve(repo_id)
    assert result == repo_id


def test_resolve_phi_variants(registry):
    """Test resolving different Phi model variants"""
    assert registry.resolve("phi") == "microsoft/phi-2"
    assert registry.resolve("phi2") == "microsoft/phi-2"
    assert registry.resolve("phi-2") == "microsoft/phi-2"


def test_resolve_mistral_variants(registry):
    """Test resolving different Mistral model variants"""
    assert registry.resolve("mistral") == "mistralai/Mistral-7B-Instruct-v0.2"
    assert registry.resolve("mistral-7b") == "mistralai/Mistral-7B-Instruct-v0.2"
    assert registry.resolve("mistral-instruct") == "mistralai/Mistral-7B-Instruct-v0.2"


def test_resolve_llama2_variants(registry):
    """Test resolving different Llama 2 model variants"""
    assert registry.resolve("llama2") == "meta-llama/Llama-2-7b-chat-hf"
    assert registry.resolve("llama2-7b") == "meta-llama/Llama-2-7b-chat-hf"
    assert registry.resolve("llama2-13b") == "meta-llama/Llama-2-13b-chat-hf"
    assert registry.resolve("llama2-70b") == "meta-llama/Llama-2-70b-chat-hf"


def test_resolve_codellama_variants(registry):
    """Test resolving different Code Llama model variants"""
    assert registry.resolve("codellama") == "codellama/CodeLlama-7b-hf"
    assert registry.resolve("codellama-7b") == "codellama/CodeLlama-7b-hf"
    assert registry.resolve("codellama-13b") == "codellama/CodeLlama-13b-hf"
    assert registry.resolve("codellama-34b") == "codellama/CodeLlama-34b-hf"
    assert registry.resolve("codellama-python") == "codellama/CodeLlama-7b-Python-hf"


def test_resolve_gemma_variants(registry):
    """Test resolving different Gemma model variants"""
    assert registry.resolve("gemma") == "google/gemma-2b-it"
    assert registry.resolve("gemma-2b") == "google/gemma-2b-it"
    assert registry.resolve("gemma-7b") == "google/gemma-7b-it"


def test_resolve_falcon_variants(registry):
    """Test resolving different Falcon model variants"""
    assert registry.resolve("falcon") == "tiiuae/falcon-7b-instruct"
    assert registry.resolve("falcon-7b") == "tiiuae/falcon-7b-instruct"


def test_resolve_vicuna_variants(registry):
    """Test resolving different Vicuna model variants"""
    assert registry.resolve("vicuna") == "lmsys/vicuna-7b-v1.5"
    assert registry.resolve("vicuna-7b") == "lmsys/vicuna-7b-v1.5"
    assert registry.resolve("vicuna-13b") == "lmsys/vicuna-13b-v1.5"


def test_resolve_orca_variants(registry):
    """Test resolving different Orca model variants"""
    assert registry.resolve("orca") == "microsoft/Orca-2-7b"
    assert registry.resolve("orca-2") == "microsoft/Orca-2-7b"


def test_resolve_zephyr_variants(registry):
    """Test resolving different Zephyr model variants"""
    assert registry.resolve("zephyr") == "HuggingFaceH4/zephyr-7b-beta"
    assert registry.resolve("zephyr-7b") == "HuggingFaceH4/zephyr-7b-beta"


def test_resolve_neural_chat(registry):
    """Test resolving Neural Chat model"""
    assert registry.resolve("neural-chat") == "Intel/neural-chat-7b-v3-1"


def test_resolve_starling_variants(registry):
    """Test resolving different Starling model variants"""
    assert registry.resolve("starling") == "berkeley-nest/Starling-LM-7B-alpha"
    assert registry.resolve("starling-7b") == "berkeley-nest/Starling-LM-7B-alpha"


def test_list_models(registry):
    """Test listing all available models"""
    models = registry.list_models()

    assert isinstance(models, dict)
    assert len(models) > 0
    assert "tinyllama" in models
    assert "mistral" in models
    assert models["tinyllama"] == "TinyLlama/TinyLlama-1.1B-Chat-v1.0"


def test_list_models_returns_copy(registry):
    """Test that list_models returns a copy, not the original"""
    models1 = registry.list_models()
    models2 = registry.list_models()

    # Modify one copy
    models1["test"] = "test/test"

    # Verify the other copy is unchanged
    assert "test" not in models2

    # Verify the registry itself is unchanged
    assert registry.resolve("test") is None


def test_add_model(registry):
    """Test adding a custom model mapping"""
    registry.add_model("custom-model", "custom-org/custom-model-repo")

    result = registry.resolve("custom-model")
    assert result == "custom-org/custom-model-repo"


def test_add_model_case_normalization(registry):
    """Test that add_model normalizes names to lowercase"""
    registry.add_model("CustomModel", "custom-org/custom-model-repo")

    # Should be accessible via lowercase
    result = registry.resolve("custommodel")
    assert result == "custom-org/custom-model-repo"

    # Should also work with original case
    result = registry.resolve("CustomModel")
    assert result == "custom-org/custom-model-repo"


def test_add_model_overwrite_existing(registry):
    """Test that add_model can overwrite existing mappings"""
    original = registry.resolve("tinyllama")
    assert original == "TinyLlama/TinyLlama-1.1B-Chat-v1.0"

    # Override with custom repo
    registry.add_model("tinyllama", "custom-org/custom-tinyllama")

    result = registry.resolve("tinyllama")
    assert result == "custom-org/custom-tinyllama"


def test_add_model_persists_in_list(registry):
    """Test that added models appear in list_models"""
    registry.add_model("new-model", "org/repo")

    models = registry.list_models()
    assert "new-model" in models
    assert models["new-model"] == "org/repo"


def test_resolve_empty_string(registry):
    """Test resolving empty string returns None"""
    result = registry.resolve("")
    assert result is None


def test_resolve_whitespace_only(registry):
    """Test resolving whitespace-only string returns None"""
    result = registry.resolve("   ")
    assert result is None


def test_registry_initialization(registry):
    """Test that registry is properly initialized with expected models"""
    models = registry.list_models()

    # Verify some key models are present
    expected_models = [
        "tinyllama",
        "phi",
        "mistral",
        "llama2",
        "codellama",
        "gemma",
        "falcon",
        "vicuna",
        "orca",
        "zephyr",
        "neural-chat",
        "starling"
    ]

    for model in expected_models:
        assert model in models, f"Expected model '{model}' not found in registry"


def test_multiple_instances_independent(registry):
    """Test that multiple registry instances are independent"""
    registry1 = OllamaRegistry()
    registry2 = OllamaRegistry()

    # Add model to first registry
    registry1.add_model("test-model", "test/repo")

    # Verify second registry is unaffected
    assert registry2.resolve("test-model") is None

    # Verify first registry has the model
    assert registry1.resolve("test-model") == "test/repo"
