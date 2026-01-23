#!/usr/bin/env python3
"""Unit tests for HuggingFace enhanced downloader"""

import tempfile
import json
from pathlib import Path
import pytest
import hashlib
import shutil
from unittest.mock import Mock, patch, MagicMock

from src.embodi.models.huggingface import (
    HuggingFaceDownloader,
    ProgressTracker,
    ModelCache
)


@pytest.fixture
def temp_cache_dir():
    """Create temporary cache directory"""
    temp_dir = tempfile.mkdtemp()
    cache_path = Path(temp_dir) / "cache"
    cache_path.mkdir()
    yield cache_path
    shutil.rmtree(temp_dir)


@pytest.fixture
def downloader(temp_cache_dir):
    """Create HuggingFaceDownloader instance with temporary cache"""
    with patch.object(HuggingFaceDownloader, '__init__', lambda self: None):
        dl = HuggingFaceDownloader()
        dl.cache_dir = temp_cache_dir
        return dl


@pytest.fixture
def model_cache(temp_cache_dir):
    """Create ModelCache instance with temporary cache"""
    with patch.object(ModelCache, '__init__', lambda self: None):
        cache = ModelCache()
        cache.cache_dir = temp_cache_dir
        cache.cache_index = temp_cache_dir / 'index.json'
        cache.index = {}
        cache._save_index()
        return cache


@pytest.fixture
def valid_gguf_file():
    """Create a minimal valid GGUF file"""
    gguf_magic = b'GGUF'
    version = (3).to_bytes(4, byteorder='little')
    minimal_gguf = gguf_magic + version + b'\x00' * (2048 - len(gguf_magic) - 4)

    with tempfile.NamedTemporaryFile(mode='wb', suffix='.gguf', delete=False) as f:
        f.write(minimal_gguf)
        temp_path = Path(f.name)

    yield temp_path

    if temp_path.exists():
        temp_path.unlink()


def calculate_checksum(file_path: Path) -> str:
    """Helper to calculate file checksum"""
    sha256_hash = hashlib.sha256()
    with open(file_path, 'rb') as f:
        for byte_block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(byte_block)
    return sha256_hash.hexdigest()


# Test GGUF file detection
def test_extract_quantization_info_q4_k_m(downloader):
    """Test extraction of Q4_K_M quantization"""
    filename = "tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
    quant = downloader._extract_quantization_info(filename)
    assert quant == "Q4_K_M"


def test_extract_quantization_info_q4_0(downloader):
    """Test extraction of Q4_0 quantization"""
    filename = "model.Q4_0.gguf"
    quant = downloader._extract_quantization_info(filename)
    assert quant == "Q4_0"


def test_extract_quantization_info_q8_0(downloader):
    """Test extraction of Q8_0 quantization"""
    filename = "llama-2-7b.Q8_0.gguf"
    quant = downloader._extract_quantization_info(filename)
    assert quant == "Q8_0"


def test_extract_quantization_info_q5_k_s(downloader):
    """Test extraction of Q5_K_S quantization"""
    filename = "model-Q5_K_S.gguf"
    quant = downloader._extract_quantization_info(filename)
    assert quant == "Q5_K_S"


def test_extract_quantization_info_no_quantization(downloader):
    """Test filename with no quantization info"""
    filename = "model.gguf"
    quant = downloader._extract_quantization_info(filename)
    assert quant is None


def test_extract_quantization_info_case_insensitive(downloader):
    """Test case-insensitive quantization extraction"""
    filename = "model.q4_k_m.gguf"
    quant = downloader._extract_quantization_info(filename)
    assert quant == "Q4_K_M"


# Test checksum calculation and verification
def test_calculate_hash(downloader, valid_gguf_file):
    """Test SHA256 hash calculation"""
    hash_result = downloader._calculate_hash(valid_gguf_file)

    # Verify it's a valid SHA256 hash (64 hex chars)
    assert len(hash_result) == 64
    assert all(c in '0123456789abcdef' for c in hash_result)

    # Calculate expected hash
    expected = calculate_checksum(valid_gguf_file)
    assert hash_result == expected


def test_verify_checksum_success(downloader, valid_gguf_file):
    """Test successful checksum verification"""
    expected = calculate_checksum(valid_gguf_file)
    result = downloader._verify_checksum(valid_gguf_file, expected)
    assert result is True


def test_verify_checksum_failure(downloader, valid_gguf_file):
    """Test failed checksum verification"""
    wrong_checksum = "0" * 64
    result = downloader._verify_checksum(valid_gguf_file, wrong_checksum)
    assert result is False


def test_verify_checksum_file_not_exists(downloader):
    """Test checksum verification with non-existent file"""
    non_existent = Path("/nonexistent/file.gguf")
    result = downloader._verify_checksum(non_existent, "abc123")
    assert result is False


def test_verify_checksum_case_insensitive(downloader, valid_gguf_file):
    """Test checksum verification is case-insensitive"""
    expected = calculate_checksum(valid_gguf_file)
    result = downloader._verify_checksum(valid_gguf_file, expected.upper())
    assert result is True


# Test GGUF file detection with mocked HuggingFace API
@patch('src.embodi.models.huggingface.HF_AVAILABLE', True)
@patch('src.embodi.models.huggingface.model_info')
def test_detect_gguf_files_success(mock_model_info, downloader):
    """Test successful GGUF file detection"""
    # Mock model info response
    mock_sibling1 = Mock()
    mock_sibling1.rfilename = "model.Q4_K_M.gguf"
    mock_sibling1.size = 1024 * 1024 * 1024  # 1GB
    mock_sibling1.lfs = {'sha256': 'abc123'}

    mock_sibling2 = Mock()
    mock_sibling2.rfilename = "model.Q8_0.gguf"
    mock_sibling2.size = 2 * 1024 * 1024 * 1024  # 2GB
    mock_sibling2.lfs = {'sha256': 'def456'}

    mock_sibling3 = Mock()
    mock_sibling3.rfilename = "config.json"
    mock_sibling3.size = 1024

    mock_info = Mock()
    mock_info.siblings = [mock_sibling1, mock_sibling2, mock_sibling3]
    mock_model_info.return_value = mock_info

    # Call _detect_gguf_files
    gguf_files = downloader._detect_gguf_files("test/model", quantization=None, auth_token=None)

    # Verify results
    assert len(gguf_files) == 2
    assert gguf_files[0]['filename'] == "model.Q4_K_M.gguf"
    assert gguf_files[0]['quantization'] == "Q4_K_M"
    assert gguf_files[0]['size'] == 1024 * 1024 * 1024
    assert gguf_files[0]['checksum'] == 'abc123'
    assert gguf_files[1]['filename'] == "model.Q8_0.gguf"
    assert gguf_files[1]['quantization'] == "Q8_0"


@patch('src.embodi.models.huggingface.HF_AVAILABLE', True)
@patch('src.embodi.models.huggingface.model_info')
def test_detect_gguf_files_with_quantization_filter(mock_model_info, downloader):
    """Test GGUF detection with quantization filter"""
    # Mock model info response
    mock_sibling1 = Mock()
    mock_sibling1.rfilename = "model.Q4_K_M.gguf"
    mock_sibling1.size = 1024 * 1024 * 1024
    mock_sibling1.lfs = {'sha256': 'abc123'}

    mock_sibling2 = Mock()
    mock_sibling2.rfilename = "model.Q8_0.gguf"
    mock_sibling2.size = 2 * 1024 * 1024 * 1024
    mock_sibling2.lfs = {'sha256': 'def456'}

    mock_info = Mock()
    mock_info.siblings = [mock_sibling1, mock_sibling2]
    mock_model_info.return_value = mock_info

    # Call with quantization=4 (should only return Q4 variants)
    gguf_files = downloader._detect_gguf_files("test/model", quantization=4, auth_token=None)

    # Verify only Q4 file is returned
    assert len(gguf_files) == 1
    assert gguf_files[0]['quantization'] == "Q4_K_M"


@patch('src.embodi.models.huggingface.HF_AVAILABLE', True)
@patch('src.embodi.models.huggingface.model_info')
def test_detect_gguf_files_no_gguf(mock_model_info, downloader):
    """Test GGUF detection when no GGUF files exist"""
    # Mock model info with no GGUF files
    mock_sibling = Mock()
    mock_sibling.rfilename = "model.safetensors"
    mock_sibling.size = 1024 * 1024 * 1024

    mock_info = Mock()
    mock_info.siblings = [mock_sibling]
    mock_model_info.return_value = mock_info

    # Call _detect_gguf_files
    gguf_files = downloader._detect_gguf_files("test/model", quantization=None, auth_token=None)

    # Verify empty list
    assert len(gguf_files) == 0


@patch('src.embodi.models.huggingface.HF_AVAILABLE', False)
def test_detect_gguf_files_hf_not_available(downloader):
    """Test GGUF detection when HuggingFace library not available"""
    gguf_files = downloader._detect_gguf_files("test/model", quantization=None, auth_token=None)
    assert len(gguf_files) == 0


@patch('src.embodi.models.huggingface.HF_AVAILABLE', True)
@patch('src.embodi.models.huggingface.model_info')
def test_detect_gguf_files_api_error(mock_model_info, downloader):
    """Test GGUF detection when API call fails"""
    mock_model_info.side_effect = Exception("API error")

    gguf_files = downloader._detect_gguf_files("test/model", quantization=None, auth_token=None)
    assert len(gguf_files) == 0


# Test checksum from metadata
@patch('src.embodi.models.huggingface.HF_AVAILABLE', True)
@patch('src.embodi.models.huggingface.model_info')
def test_get_file_checksum_lfs(mock_model_info, downloader):
    """Test getting checksum from LFS metadata"""
    mock_sibling = Mock()
    mock_sibling.rfilename = "model.gguf"
    mock_sibling.lfs = {'sha256': 'abc123def456'}

    mock_info = Mock()
    mock_info.siblings = [mock_sibling]
    mock_model_info.return_value = mock_info

    checksum = downloader._get_file_checksum("test/model", "model.gguf", auth_token=None)
    assert checksum == 'abc123def456'


@patch('src.embodi.models.huggingface.HF_AVAILABLE', True)
@patch('src.embodi.models.huggingface.model_info')
def test_get_file_checksum_direct(mock_model_info, downloader):
    """Test getting checksum from direct sha256 attribute"""
    mock_sibling = Mock()
    mock_sibling.rfilename = "model.gguf"
    mock_sibling.lfs = None
    mock_sibling.sha256 = 'direct123'

    mock_info = Mock()
    mock_info.siblings = [mock_sibling]
    mock_model_info.return_value = mock_info

    checksum = downloader._get_file_checksum("test/model", "model.gguf", auth_token=None)
    assert checksum == 'direct123'


@patch('src.embodi.models.huggingface.HF_AVAILABLE', True)
@patch('src.embodi.models.huggingface.model_info')
def test_get_file_checksum_not_found(mock_model_info, downloader):
    """Test getting checksum when file not in metadata"""
    mock_sibling = Mock()
    mock_sibling.rfilename = "other.gguf"
    mock_sibling.lfs = {'sha256': 'abc123'}

    mock_info = Mock()
    mock_info.siblings = [mock_sibling]
    mock_model_info.return_value = mock_info

    checksum = downloader._get_file_checksum("test/model", "model.gguf", auth_token=None)
    assert checksum is None


@patch('src.embodi.models.huggingface.HF_AVAILABLE', True)
@patch('src.embodi.models.huggingface.model_info')
def test_get_file_checksum_api_error(mock_model_info, downloader):
    """Test getting checksum when API fails"""
    mock_model_info.side_effect = Exception("API error")

    checksum = downloader._get_file_checksum("test/model", "model.gguf", auth_token=None)
    assert checksum is None


# Test ProgressTracker
def test_progress_tracker_update():
    """Test progress tracker update with callback"""
    callback_data = []

    def callback(downloaded, total, speed, eta):
        callback_data.append({
            'downloaded': downloaded,
            'total': total,
            'speed': speed,
            'eta': eta
        })

    tracker = ProgressTracker(total=1000, callback=callback)
    tracker.update(100)
    tracker.update(200)

    # Verify callback was called
    assert len(callback_data) == 2
    assert callback_data[0]['downloaded'] == 100
    assert callback_data[1]['downloaded'] == 300


def test_progress_tracker_no_callback():
    """Test progress tracker without callback"""
    tracker = ProgressTracker(total=1000)
    tracker.update(100)
    # Should not raise exception
    assert tracker.bytes_downloaded == 100


def test_progress_tracker_callback_error():
    """Test progress tracker with failing callback"""
    def failing_callback(downloaded, total, speed, eta):
        raise Exception("Callback error")

    tracker = ProgressTracker(total=1000, callback=failing_callback)
    # Should not raise exception - errors are caught
    tracker.update(100)
    assert tracker.bytes_downloaded == 100


def test_progress_tracker_context_manager():
    """Test progress tracker as context manager"""
    with ProgressTracker(total=1000) as tracker:
        tracker.update(100)
        assert tracker.bytes_downloaded == 100


def test_progress_tracker_set_description():
    """Test setting description"""
    tracker = ProgressTracker(desc="Initial")
    assert tracker.desc == "Initial"

    tracker.set_description("Updated")
    assert tracker.desc == "Updated"


# Test ModelCache
def test_model_cache_add_and_get(model_cache, valid_gguf_file):
    """Test adding and retrieving from cache"""
    # Add to cache
    model_cache.add_to_cache(
        "test/model",
        quantization=4,
        path=valid_gguf_file,
        metadata={'source': 'test'}
    )

    # Verify index was saved
    assert model_cache.cache_index.exists()

    # Retrieve from cache
    cached = model_cache.get_cached("test/model", quantization=4)
    assert cached == valid_gguf_file


def test_model_cache_get_non_existent(model_cache):
    """Test getting non-existent model from cache"""
    cached = model_cache.get_cached("nonexistent/model", quantization=4)
    assert cached is None


def test_model_cache_list_cached(model_cache, valid_gguf_file):
    """Test listing cached models"""
    # Add models to cache
    model_cache.add_to_cache(
        "test/model1",
        quantization=4,
        path=valid_gguf_file,
        metadata={'source': 'test'}
    )

    # List cached models
    models = model_cache.list_cached()
    assert len(models) == 1
    assert models[0]['model_id'] == "test/model1"
    assert models[0]['quantization'] == 4


def test_model_cache_different_quantizations(model_cache, valid_gguf_file):
    """Test caching same model with different quantizations"""
    # Add same model with different quantizations
    model_cache.add_to_cache(
        "test/model",
        quantization=4,
        path=valid_gguf_file,
        metadata={'source': 'test'}
    )

    model_cache.add_to_cache(
        "test/model",
        quantization=8,
        path=valid_gguf_file,
        metadata={'source': 'test'}
    )

    # Verify both are cached separately
    cached_4 = model_cache.get_cached("test/model", quantization=4)
    cached_8 = model_cache.get_cached("test/model", quantization=8)

    assert cached_4 == valid_gguf_file
    assert cached_8 == valid_gguf_file


def test_model_cache_clear(model_cache, valid_gguf_file, temp_cache_dir):
    """Test clearing cache"""
    # Add to cache
    cached_file = temp_cache_dir / "cached_model.gguf"
    shutil.copy(valid_gguf_file, cached_file)

    model_cache.add_to_cache(
        "test/model",
        quantization=4,
        path=cached_file,
        metadata={'source': 'test'}
    )

    # Clear cache
    model_cache.clear_cache()

    # Verify cache is empty
    assert len(model_cache.index) == 0
    cached = model_cache.get_cached("test/model", quantization=4)
    assert cached is None


# Test direct GGUF download file selection
def test_download_gguf_direct_selects_q4_k_m(downloader):
    """Test that Q4_K_M is preferred when available"""
    gguf_files = [
        {'filename': 'model.Q8_0.gguf', 'size': 2048, 'quantization': 'Q8_0', 'checksum': 'abc'},
        {'filename': 'model.Q4_K_M.gguf', 'size': 1024, 'quantization': 'Q4_K_M', 'checksum': 'def'},
        {'filename': 'model.Q5_0.gguf', 'size': 1536, 'quantization': 'Q5_0', 'checksum': 'ghi'},
    ]

    with patch('src.embodi.models.huggingface.hf_hub_download') as mock_download, \
         patch.object(downloader, '_get_file_checksum', return_value='def'), \
         patch.object(downloader, '_verify_checksum', return_value=True), \
         patch('shutil.copy2'):

        mock_download.return_value = "/tmp/downloaded.gguf"

        output_path = Path("/tmp/output.gguf")
        result = downloader._download_gguf_direct(
            "test/model",
            gguf_files,
            output_path,
            auth_token=None,
            progress_callback=None
        )

        # Verify Q4_K_M was selected
        assert result is True
        mock_download.assert_called_once()
        call_kwargs = mock_download.call_args[1]
        assert call_kwargs['filename'] == 'model.Q4_K_M.gguf'


def test_download_gguf_direct_falls_back_to_first(downloader):
    """Test fallback to first file when no preferred quantization found"""
    gguf_files = [
        {'filename': 'model.Q2_K.gguf', 'size': 512, 'quantization': 'Q2_K', 'checksum': 'abc'},
        {'filename': 'model.Q3_K_S.gguf', 'size': 768, 'quantization': 'Q3_K_S', 'checksum': 'def'},
    ]

    with patch('src.embodi.models.huggingface.hf_hub_download') as mock_download, \
         patch.object(downloader, '_get_file_checksum', return_value='abc'), \
         patch.object(downloader, '_verify_checksum', return_value=True), \
         patch('shutil.copy2'):

        mock_download.return_value = "/tmp/downloaded.gguf"

        output_path = Path("/tmp/output.gguf")
        result = downloader._download_gguf_direct(
            "test/model",
            gguf_files,
            output_path,
            auth_token=None,
            progress_callback=None
        )

        # Verify first file was selected
        assert result is True
        call_kwargs = mock_download.call_args[1]
        assert call_kwargs['filename'] == 'model.Q2_K.gguf'


def test_download_gguf_direct_empty_list(downloader):
    """Test direct download with empty file list"""
    result = downloader._download_gguf_direct(
        "test/model",
        [],
        Path("/tmp/output.gguf"),
        auth_token=None,
        progress_callback=None
    )

    assert result is False


def test_download_gguf_direct_checksum_failure(downloader):
    """Test direct download with checksum verification failure"""
    gguf_files = [
        {'filename': 'model.Q4_K_M.gguf', 'size': 1024, 'quantization': 'Q4_K_M', 'checksum': 'correct_hash'},
    ]

    with patch('src.embodi.models.huggingface.hf_hub_download') as mock_download, \
         patch.object(downloader, '_get_file_checksum', return_value='correct_hash'), \
         patch.object(downloader, '_verify_checksum', return_value=False):

        mock_download.return_value = "/tmp/downloaded.gguf"

        output_path = Path("/tmp/output.gguf")
        result = downloader._download_gguf_direct(
            "test/model",
            gguf_files,
            output_path,
            auth_token=None,
            progress_callback=None
        )

        # Verify download failed due to checksum
        assert result is False
