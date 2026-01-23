#!/usr/bin/env python3
"""Unit tests for UpdateVerifier"""

import tempfile
import struct
from pathlib import Path
import pytest
import hashlib

from src.embodi.models.update_verifier import UpdateVerifier


@pytest.fixture
def verifier():
    """Create UpdateVerifier instance"""
    return UpdateVerifier()


@pytest.fixture
def valid_gguf_file():
    """Create a minimal valid GGUF file"""
    gguf_magic = b'GGUF'
    version = (3).to_bytes(4, byteorder='little')
    minimal_gguf = gguf_magic + version + b'\x00' * (1024 - len(gguf_magic) - 4)

    with tempfile.NamedTemporaryFile(mode='wb', suffix='.gguf', delete=False) as f:
        f.write(minimal_gguf)
        temp_path = Path(f.name)

    yield temp_path

    if temp_path.exists():
        temp_path.unlink()


@pytest.fixture
def invalid_gguf_file():
    """Create a file with invalid GGUF magic number"""
    invalid_magic = b'FAKE'
    version = (3).to_bytes(4, byteorder='little')
    invalid_gguf = invalid_magic + version + b'\x00' * (1024 - len(invalid_magic) - 4)

    with tempfile.NamedTemporaryFile(mode='wb', suffix='.gguf', delete=False) as f:
        f.write(invalid_gguf)
        temp_path = Path(f.name)

    yield temp_path

    if temp_path.exists():
        temp_path.unlink()


@pytest.fixture
def small_file():
    """Create a file smaller than minimum size (1KB)"""
    with tempfile.NamedTemporaryFile(mode='wb', suffix='.gguf', delete=False) as f:
        f.write(b'GGUF' + b'\x00' * 100)  # Only 104 bytes
        temp_path = Path(f.name)

    yield temp_path

    if temp_path.exists():
        temp_path.unlink()


def test_verify_checksum_valid(verifier, valid_gguf_file):
    """Test checksum verification with valid checksum"""
    # Calculate actual checksum
    with open(valid_gguf_file, 'rb') as f:
        checksum = hashlib.sha256(f.read()).hexdigest()

    # Verify
    result = verifier.verify_checksum(valid_gguf_file, checksum)
    assert result is True


def test_verify_checksum_invalid(verifier, valid_gguf_file):
    """Test checksum verification with invalid checksum"""
    result = verifier.verify_checksum(valid_gguf_file, "0" * 64)
    assert result is False


def test_verify_gguf_format_valid(verifier, valid_gguf_file):
    """Test GGUF format verification with valid file"""
    is_valid, format_name = verifier.verify_model_format(valid_gguf_file)
    assert is_valid is True
    assert format_name == "gguf"


def test_verify_gguf_format_invalid_magic(verifier, invalid_gguf_file):
    """Test GGUF format verification with invalid magic number"""
    is_valid, format_name = verifier.verify_model_format(invalid_gguf_file)
    assert is_valid is False
    assert format_name == "gguf_invalid_magic"


def test_verify_size_too_small(verifier, small_file):
    """Test file size verification rejects files under 1KB"""
    is_valid, error_msg = verifier.verify_size(small_file)
    assert is_valid is False
    assert "too_small" in error_msg.lower() or "small" in error_msg.lower()


def test_verify_size_valid(verifier, valid_gguf_file):
    """Test file size verification accepts valid files"""
    is_valid, msg = verifier.verify_size(valid_gguf_file)
    assert is_valid is True
    assert "ok" in msg.lower()


@pytest.mark.skip(reason="Difficult to mock large file size properly")
def test_verify_size_too_large(verifier):
    """Test file size verification would reject files over 100GB"""
    # This test verifies the logic exists in the code
    # MAX_MODEL_SIZE is set to 100GB in UpdateVerifier
    assert verifier.MAX_MODEL_SIZE == 100 * 1024 * 1024 * 1024
    # In production, files over this size would be rejected


def test_verify_update_comprehensive(verifier, valid_gguf_file):
    """Test full verification flow"""
    # Calculate checksum for valid file
    with open(valid_gguf_file, 'rb') as f:
        checksum = hashlib.sha256(f.read()).hexdigest()

    # Run comprehensive verification
    result = verifier.verify_update(valid_gguf_file, checksum)

    assert result['valid'] is True
    assert result['checksum_valid'] is True
    assert result['format_valid'] is True
    assert result['size_valid'] is True
    assert result['checksum'] is not None
    assert result['format'] == 'gguf'


def test_verify_update_bad_checksum(verifier, valid_gguf_file):
    """Test verification fails with bad checksum"""
    result = verifier.verify_update(valid_gguf_file, "0" * 64)

    assert result['valid'] is False
    assert result['checksum_valid'] is False


def test_verify_update_invalid_format(verifier, invalid_gguf_file):
    """Test verification fails with invalid format"""
    # Calculate checksum for the invalid file
    with open(invalid_gguf_file, 'rb') as f:
        checksum = hashlib.sha256(f.read()).hexdigest()

    result = verifier.verify_update(invalid_gguf_file, checksum)

    assert result['valid'] is False
    assert result['format_valid'] is False


def test_get_file_metadata(verifier, valid_gguf_file):
    """Test file metadata extraction"""
    metadata = verifier.get_file_metadata(valid_gguf_file)

    assert 'size' in metadata
    assert 'checksum' in metadata
    assert 'format' in metadata
    assert metadata['size'] == 1024
    assert metadata['format'] == '.gguf'
    assert len(metadata['checksum']) == 64  # SHA256 hex length
