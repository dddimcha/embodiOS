#!/usr/bin/env python3
"""Unit tests for OTAUpdater"""

import tempfile
import json
from pathlib import Path
import pytest
import hashlib
import shutil

from src.embodi.models.ota_updater import OTAUpdater


@pytest.fixture
def temp_models_dir():
    """Create temporary models directory"""
    temp_dir = tempfile.mkdtemp()
    models_path = Path(temp_dir) / "models"
    models_path.mkdir()
    yield models_path
    shutil.rmtree(temp_dir)


@pytest.fixture
def updater(temp_models_dir):
    """Create OTAUpdater instance with temporary directory"""
    manifest_path = temp_models_dir / "manifest.json"
    # Create initial manifest
    manifest_data = {
        "schema_version": "1.0",
        "models": {}
    }
    with open(manifest_path, 'w') as f:
        json.dump(manifest_data, f)

    return OTAUpdater(models_dir=str(temp_models_dir))


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


@pytest.fixture
def invalid_gguf_file():
    """Create a file with invalid GGUF format"""
    invalid_data = b'FAKE' + b'\x00' * 2044

    with tempfile.NamedTemporaryFile(mode='wb', suffix='.gguf', delete=False) as f:
        f.write(invalid_data)
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


def test_update_model_success(updater, valid_gguf_file, temp_models_dir):
    """Test successful update flow"""
    checksum = calculate_checksum(valid_gguf_file)

    success, message = updater.update_model(
        valid_gguf_file,
        "test_model",
        expected_checksum=checksum,
        metadata={"name": "Test Model"}
    )

    assert success is True
    assert "updated successfully" in message

    # Verify manifest was updated
    manifest_path = temp_models_dir / "manifest.json"
    with open(manifest_path, 'r') as f:
        manifest = json.load(f)

    assert "test_model" in manifest['models']
    assert manifest['models']["test_model"]['update_source'] == "OTA"
    assert 'last_update_time' in manifest['models']["test_model"]


def test_update_model_checksum_failure(updater, valid_gguf_file, temp_models_dir):
    """Test rollback on bad checksum"""
    # Use wrong checksum
    wrong_checksum = "0" * 64

    success, message = updater.update_model(
        valid_gguf_file,
        "test_model",
        expected_checksum=wrong_checksum,
        metadata=None
    )

    assert success is False
    assert 'checksum' in message.lower() or 'verification failed' in message.lower()

    # Verify manifest unchanged (no test_model entry)
    manifest_path = temp_models_dir / "manifest.json"
    with open(manifest_path, 'r') as f:
        manifest = json.load(f)

    assert "test_model" not in manifest['models']


def test_update_model_file_not_found(updater):
    """Test handling of missing file"""
    non_existent = Path("/nonexistent/file.gguf")

    success, message = updater.update_model(
        non_existent,
        "test",
        expected_checksum="abc123",
        metadata=None
    )

    assert success is False
    assert 'not found' in message.lower()


def test_backup_file(updater, valid_gguf_file, temp_models_dir):
    """Test backup creation"""
    # First, copy file to models directory
    model_path = temp_models_dir / "existing_model.gguf"
    shutil.copy(valid_gguf_file, model_path)

    # Create backup
    backup_path = updater._backup_file(model_path, "existing_model")

    assert backup_path is not None
    assert backup_path.exists()
    assert backup_path.parent == updater.backup_dir
    assert "existing_model" in backup_path.name


def test_rollback(updater, valid_gguf_file, temp_models_dir):
    """Test rollback restores previous state"""
    # Setup: create original model and manifest
    original_model_path = temp_models_dir / "original.gguf"
    shutil.copy(valid_gguf_file, original_model_path)

    manifest_path = temp_models_dir / "manifest.json"
    original_manifest = {
        "schema_version": "1.0",
        "models": {
            "original": {
                "name": "original",
                "files": {"gguf": {"filename": "original.gguf"}}
            }
        }
    }
    with open(manifest_path, 'w') as f:
        json.dump(original_manifest, f)

    # Create backups
    backup_path = updater._backup_file(original_model_path, "original")
    backup_manifest_path = updater._backup_manifest()

    # Corrupt the model
    with open(original_model_path, 'w') as f:
        f.write("corrupted")

    # Corrupt manifest
    with open(manifest_path, 'w') as f:
        f.write("corrupted")

    # Rollback
    updater._rollback(original_model_path, backup_path, backup_manifest_path)

    # Verify content restored
    assert original_model_path.exists()
    with open(original_model_path, 'rb') as f:
        content = f.read()
    with open(backup_path, 'rb') as f:
        backup_content = f.read()
    assert content == backup_content

    # Verify manifest restored
    with open(manifest_path, 'r') as f:
        restored_manifest = json.load(f)
    assert restored_manifest == original_manifest


def test_update_manifest(updater, valid_gguf_file, temp_models_dir):
    """Test manifest update with OTA metadata"""
    # Create verification result (simulating what UpdateVerifier returns)
    verification = {
        'valid': True,
        'format': 'gguf',
        'size': 2048,
        'checksum': 'abc123' * 10 + 'abcd',  # 64 char hex
    }

    metadata = {
        'name': 'OTA Test Model',
        'version': '1.0'
    }

    updater._update_manifest(
        "ota_test",
        valid_gguf_file,
        verification,
        metadata
    )

    # Read manifest
    manifest_path = temp_models_dir / "manifest.json"
    with open(manifest_path, 'r') as f:
        manifest = json.load(f)

    assert "ota_test" in manifest['models']
    model = manifest['models']["ota_test"]
    assert model['name'] == "OTA Test Model"
    assert model['update_source'] == "OTA"
    assert 'last_update_time' in model


def test_cleanup_backups(updater, temp_models_dir):
    """Test removal of old backups"""
    # Create several backup files in the backup directory
    backup_dir = updater.backup_dir
    for i in range(5):
        backup_file = backup_dir / f"model_{i:08d}.gguf"
        backup_file.write_text("backup")

    # Cleanup keeping only 2
    updater.cleanup_backups(keep_latest=2)

    # Count remaining backups
    backups = list(backup_dir.glob("model_*.gguf"))
    assert len(backups) <= 2


def test_update_from_file(updater, valid_gguf_file):
    """Test update from local file path"""
    checksum = calculate_checksum(valid_gguf_file)

    model_id = updater.update_from_file(
        str(valid_gguf_file),
        checksum=checksum,
        model_name="file_test"
    )

    assert model_id is not None
    assert model_id == "file_test"


def test_update_from_file_invalid_checksum(updater, valid_gguf_file):
    """Test update from file with invalid checksum fails"""
    wrong_checksum = "0" * 64

    with pytest.raises(ValueError) as exc_info:
        updater.update_from_file(
            str(valid_gguf_file),
            checksum=wrong_checksum,
            model_name="file_test"
        )

    assert "checksum" in str(exc_info.value).lower() or "verification" in str(exc_info.value).lower()


def test_update_from_url(updater, monkeypatch):
    """Test update from URL (mocked)"""
    # Mock the URL download to avoid actual network calls
    def mock_urlretrieve(url, path):
        # Create a fake GGUF file
        gguf_magic = b'GGUF'
        version = (3).to_bytes(4, byteorder='little')
        minimal_gguf = gguf_magic + version + b'\x00' * 2044
        Path(path).write_bytes(minimal_gguf)

    import urllib.request
    monkeypatch.setattr(urllib.request, "urlretrieve", mock_urlretrieve)

    model_id = updater.update_from_url(
        "https://example.com/model.gguf",
        model_name="url_test"
    )

    assert model_id is not None
    assert model_id == "url_test"


def test_invalid_format_rejected(updater, invalid_gguf_file):
    """Test that invalid GGUF format is rejected"""
    checksum = calculate_checksum(invalid_gguf_file)

    success, message = updater.update_model(
        invalid_gguf_file,
        "invalid_test",
        expected_checksum=checksum,
        metadata=None
    )

    assert success is False
    assert 'verification failed' in message.lower()


def test_get_model_info(updater, valid_gguf_file):
    """Test retrieving model info from manifest"""
    checksum = calculate_checksum(valid_gguf_file)

    # Add a model
    updater.update_from_file(
        str(valid_gguf_file),
        checksum=checksum,
        model_name="info_test"
    )

    # Get model info
    info = updater.get_model_info("info_test")

    assert info is not None
    assert info['name'] == "info_test"
    assert info['update_source'] == "OTA"


def test_list_models(updater, valid_gguf_file):
    """Test listing all models"""
    checksum = calculate_checksum(valid_gguf_file)

    # Add two models
    updater.update_from_file(
        str(valid_gguf_file),
        checksum=checksum,
        model_name="model1"
    )
    updater.update_from_file(
        str(valid_gguf_file),
        checksum=checksum,
        model_name="model2"
    )

    # List models
    models = updater.list_models()

    assert len(models) == 2
    assert "model1" in models
    assert "model2" in models
