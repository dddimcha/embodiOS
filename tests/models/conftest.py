#!/usr/bin/env python3
"""Pytest configuration and fixtures for model compatibility tests"""

import json
import tempfile
import shutil
from pathlib import Path
import pytest


@pytest.fixture(scope="session")
def ollama_models_data():
    """Load Ollama model metadata from fixture file"""
    fixture_path = Path(__file__).parent / "fixtures" / "ollama_models.json"
    with open(fixture_path, 'r') as f:
        return json.load(f)


@pytest.fixture(scope="session")
def ollama_models_list(ollama_models_data):
    """Get list of Ollama model configurations"""
    return ollama_models_data['models']


@pytest.fixture
def temp_models_dir():
    """Create temporary models directory for testing"""
    temp_dir = tempfile.mkdtemp()
    models_path = Path(temp_dir) / "models"
    models_path.mkdir()
    yield models_path
    shutil.rmtree(temp_dir)


@pytest.fixture
def temp_cache_dir():
    """Create temporary cache directory for model downloads"""
    temp_dir = tempfile.mkdtemp()
    cache_path = Path(temp_dir) / "cache"
    cache_path.mkdir()
    yield cache_path
    shutil.rmtree(temp_dir)


@pytest.fixture
def valid_gguf_file():
    """Create a minimal valid GGUF file for testing"""
    gguf_magic = b'GGUF'
    version = (3).to_bytes(4, byteorder='little')
    minimal_gguf = gguf_magic + version + b'\x00' * (2048 - len(gguf_magic) - 4)

    with tempfile.NamedTemporaryFile(mode='wb', suffix='.gguf', delete=False) as f:
        f.write(minimal_gguf)
        temp_path = Path(f.name)

    yield temp_path

    if temp_path.exists():
        temp_path.unlink()


@pytest.fixture
def invalid_gguf_file():
    """Create a file with invalid GGUF format for testing"""
    invalid_data = b'FAKE' + b'\x00' * 2044

    with tempfile.NamedTemporaryFile(mode='wb', suffix='.gguf', delete=False) as f:
        f.write(invalid_data)
        temp_path = Path(f.name)

    yield temp_path

    if temp_path.exists():
        temp_path.unlink()


@pytest.fixture
def mock_model_file(temp_models_dir):
    """Create a mock model file with valid GGUF structure"""
    gguf_magic = b'GGUF'
    version = (3).to_bytes(4, byteorder='little')
    minimal_gguf = gguf_magic + version + b'\x00' * (2048 - len(gguf_magic) - 4)

    model_path = temp_models_dir / "test_model.gguf"
    with open(model_path, 'wb') as f:
        f.write(minimal_gguf)

    return model_path


@pytest.fixture
def model_manifest(temp_models_dir):
    """Create a test manifest.json file"""
    manifest_path = temp_models_dir / "manifest.json"
    manifest_data = {
        "schema_version": "1.0",
        "models": {}
    }
    with open(manifest_path, 'w') as f:
        json.dump(manifest_data, f)

    return manifest_path


@pytest.fixture
def model_download_cache():
    """Fixture for caching model downloads across tests"""
    cache_dir = Path(tempfile.gettempdir()) / "model_compatibility_cache"
    cache_dir.mkdir(exist_ok=True)
    yield cache_dir
    # Note: We don't cleanup the cache to persist across test runs
