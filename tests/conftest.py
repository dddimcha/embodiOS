"""Pytest configuration and fixtures."""
import pytest
import tempfile
from pathlib import Path


@pytest.fixture
def temp_dir():
    """Create a temporary directory for tests."""
    with tempfile.TemporaryDirectory() as tmpdir:
        yield Path(tmpdir)


@pytest.fixture
def modelfile_content():
    """Sample Modelfile content."""
    return """FROM scratch
MODEL huggingface:TinyLlama/TinyLlama-1.1B-Chat-v1.0
QUANTIZE 4bit
MEMORY 2G
HARDWARE gpio:enabled uart:enabled
"""


@pytest.fixture
def mock_image_data():
    """Mock image data for testing."""
    return {
        'name': 'test-model',
        'tag': 'latest',
        'size': 1024 * 1024 * 512,  # 512MB
        'created': '2024-01-01T00:00:00Z',
        'model': 'TinyLlama/TinyLlama-1.1B-Chat-v1.0',
        'quantization': '4bit',
        'hardware': ['gpio', 'uart']
    }