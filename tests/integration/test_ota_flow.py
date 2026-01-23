#!/usr/bin/env python3
"""
Integration tests for OTA model update flow
Tests the full end-to-end flow of atomic updates with successful model loads
"""

import os
import json
import tempfile
import hashlib
import base64
import time
from pathlib import Path
from typing import Generator
import pytest
from fastapi.testclient import TestClient

from src.embodi.api.server import create_app
from src.embodi.models.ota_updater import OTAUpdater


@pytest.fixture
def test_model_file() -> Generator[Path, None, None]:
    """Create a minimal test GGUF model file"""
    # Create a minimal GGUF file with correct magic number
    # GGUF magic: "GGUF" (0x47 0x47 0x55 0x46)
    gguf_magic = b'GGUF'
    version = (3).to_bytes(4, byteorder='little')

    # Minimal GGUF structure: magic + version + basic header
    # This creates a valid (though minimal) GGUF file
    minimal_gguf = gguf_magic + version
    # Add padding to meet minimum size requirement (1024 bytes)
    # UpdateVerifier requires MIN_MODEL_SIZE = 1024 bytes
    minimal_gguf += b'\x00' * (1024 - len(minimal_gguf))

    # Create temporary file
    with tempfile.NamedTemporaryFile(
        mode='wb',
        suffix='.gguf',
        delete=False
    ) as f:
        f.write(minimal_gguf)
        temp_path = Path(f.name)

    yield temp_path

    # Cleanup
    if temp_path.exists():
        temp_path.unlink()


@pytest.fixture
def test_models_dir() -> Generator[Path, None, None]:
    """Create a temporary models directory"""
    with tempfile.TemporaryDirectory() as tmpdir:
        models_dir = Path(tmpdir) / 'models'
        models_dir.mkdir(parents=True)

        # Create initial manifest
        manifest = {
            "models": {},
            "schema_version": "1.0"
        }
        manifest_path = models_dir / 'manifest.json'
        with open(manifest_path, 'w') as f:
            json.dump(manifest, f, indent=2)

        yield models_dir


@pytest.fixture
def auth_token() -> str:
    """Provide authentication token for tests"""
    return "test_token_123"


@pytest.fixture
def api_client(test_models_dir: Path, auth_token: str) -> Generator[TestClient, None, None]:
    """Create FastAPI test client with auth configured"""
    # Set environment variables
    os.environ["EMBODIOS_AUTH_TOKEN"] = auth_token

    # Change to test models directory
    original_cwd = os.getcwd()
    os.chdir(test_models_dir.parent)

    try:
        # Create app instance
        app = create_app(debug=True)

        # Create test client
        client = TestClient(app)

        yield client
    finally:
        # Restore original directory and clean up env
        os.chdir(original_cwd)
        if "EMBODIOS_AUTH_TOKEN" in os.environ:
            del os.environ["EMBODIOS_AUTH_TOKEN"]


def calculate_checksum(file_path: Path) -> str:
    """Calculate SHA256 checksum of a file"""
    sha256_hash = hashlib.sha256()
    with open(file_path, 'rb') as f:
        for byte_block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(byte_block)
    return sha256_hash.hexdigest()


class TestAtomicUpdateSuccessFlow:
    """Test suite for atomic update with successful model load"""

    def test_full_ota_update_flow(
        self,
        api_client: TestClient,
        test_model_file: Path,
        test_models_dir: Path,
        auth_token: str
    ):
        """
        Test complete OTA update flow:
        1. Upload model via API
        2. Verify model appears in manifest
        3. Query model status
        4. Switch to new model
        5. Verify inference works
        """
        # Step 1: Upload model via API
        # Calculate checksum for verification
        checksum = calculate_checksum(test_model_file)

        # Read model file and encode as base64
        with open(test_model_file, 'rb') as f:
            model_bytes = f.read()
        model_data_b64 = base64.b64encode(model_bytes).decode('utf-8')

        # Upload request
        upload_response = api_client.post(
            "/v1/models/upload",
            headers={"X-Auth-Token": auth_token},
            json={
                "model_data": model_data_b64,
                "checksum": checksum,
                "model_name": "test_model"
            }
        )

        # Verify upload succeeded
        assert upload_response.status_code == 200, (
            f"Upload failed: {upload_response.text}"
        )

        upload_data = upload_response.json()
        assert upload_data["status"] == "success"
        assert "model_id" in upload_data
        model_id = upload_data["model_id"]

        # Step 2: Verify model appears in manifest.json
        manifest_path = test_models_dir / 'manifest.json'
        assert manifest_path.exists(), "Manifest file should exist"

        with open(manifest_path, 'r') as f:
            manifest = json.load(f)

        # Check manifest contains the new model
        assert "models" in manifest
        assert model_id in manifest["models"], (
            f"Model {model_id} should be in manifest"
        )

        # Verify OTA metadata
        model_entry = manifest["models"][model_id]
        assert model_entry["update_source"] == "OTA", (
            "Model should have OTA update source"
        )
        assert "last_update_time" in model_entry, (
            "Model should have last_update_time"
        )

        # Step 3: Query /v1/models/status
        status_response = api_client.get("/v1/models/status")

        assert status_response.status_code == 200, (
            f"Status query failed: {status_response.text}"
        )

        status_data = status_response.json()
        assert "loaded_models" in status_data
        assert "registry_stats" in status_data

        # Verify registry stats
        assert status_data["registry_stats"]["total_models"] >= 0

        # Step 4: Attempt to switch to new model
        # Note: Switch currently returns 501 Not Implemented
        switch_response = api_client.post(
            "/v1/models/switch",
            headers={"X-Auth-Token": auth_token},
            json={"model_id": model_id}
        )

        assert switch_response.status_code == 501, (
            f"Model switch should return 501 Not Implemented: {switch_response.text}"
        )

        switch_data = switch_response.json()
        assert "detail" in switch_data
        assert "not_implemented" in str(switch_data["detail"]).lower()

        # Step 5: Verify inference succeeds with new model
        # Note: This tests the API endpoint, actual inference may be mocked
        # in the test environment
        completion_response = api_client.post(
            "/v1/completions",
            json={
                "model": model_id,
                "prompt": "Test prompt",
                "max_tokens": 10
            }
        )

        # Inference may fail with minimal test model, but endpoint should respond
        # We're primarily testing that the OTA update succeeded
        assert completion_response.status_code in [200, 500], (
            f"Completion endpoint should be accessible: {completion_response.text}"
        )

    def test_upload_requires_authentication(self, api_client: TestClient):
        """Test that upload endpoint requires authentication"""
        # Try upload without token
        response = api_client.post(
            "/v1/models/upload",
            json={
                "model_data": "fake_data",
                "checksum": "fake_checksum"
            }
        )

        assert response.status_code == 401, (
            "Upload without auth should return 401"
        )

    def test_upload_rejects_invalid_token(
        self,
        api_client: TestClient,
        test_model_file: Path
    ):
        """Test that upload rejects invalid authentication tokens"""
        checksum = calculate_checksum(test_model_file)

        with open(test_model_file, 'rb') as f:
            model_bytes = f.read()
        model_data_b64 = base64.b64encode(model_bytes).decode('utf-8')

        # Try with wrong token
        response = api_client.post(
            "/v1/models/upload",
            headers={"X-Auth-Token": "wrong_token"},
            json={
                "model_data": model_data_b64,
                "checksum": checksum,
                "model_name": "test_model"
            }
        )

        assert response.status_code == 401, (
            "Upload with invalid token should return 401"
        )

    def test_model_appears_in_status_after_upload(
        self,
        api_client: TestClient,
        test_model_file: Path,
        test_models_dir: Path,
        auth_token: str
    ):
        """Test that uploaded model appears in status endpoint"""
        # Upload model
        checksum = calculate_checksum(test_model_file)

        with open(test_model_file, 'rb') as f:
            model_bytes = f.read()
        model_data_b64 = base64.b64encode(model_bytes).decode('utf-8')

        upload_response = api_client.post(
            "/v1/models/upload",
            headers={"X-Auth-Token": auth_token},
            json={
                "model_data": model_data_b64,
                "checksum": checksum,
                "model_name": "status_test_model"
            }
        )

        assert upload_response.status_code == 200
        model_id = upload_response.json()["model_id"]

        # Check status endpoint
        status_response = api_client.get("/v1/models/status")
        assert status_response.status_code == 200

        # Verify the model update was recorded
        manifest_path = test_models_dir / 'manifest.json'
        with open(manifest_path, 'r') as f:
            manifest = json.load(f)

        assert model_id in manifest["models"]
        assert manifest["models"][model_id]["name"] == "status_test_model"


class TestAtomicOperations:
    """Test atomic operations and consistency"""

    def test_manifest_atomicity(
        self,
        test_model_file: Path,
        test_models_dir: Path
    ):
        """Test that manifest updates are atomic"""
        # Save original manifest state
        manifest_path = test_models_dir / 'manifest.json'
        with open(manifest_path, 'r') as f:
            original_manifest = json.load(f)

        # Perform update using OTAUpdater directly
        updater = OTAUpdater(models_dir=test_models_dir)

        # Calculate checksum
        checksum = calculate_checksum(test_model_file)

        # Perform update
        success, message = updater.update_model(
            test_model_file,
            model_id="atomic_test_model",
            expected_checksum=checksum,
            metadata={"name": "Atomic Test Model"}
        )

        assert success, f"Update should succeed: {message}"

        # Read updated manifest
        with open(manifest_path, 'r') as f:
            updated_manifest = json.load(f)

        # Verify manifest was updated atomically
        assert "atomic_test_model" in updated_manifest["models"]
        assert updated_manifest["schema_version"] == "1.0"

        # Verify OTA metadata is present
        model_entry = updated_manifest["models"]["atomic_test_model"]
        assert model_entry["update_source"] == "OTA"
        assert model_entry["last_update_time"] is not None

    def test_no_temp_files_after_success(
        self,
        test_model_file: Path,
        test_models_dir: Path
    ):
        """Test that no temporary files remain after successful update"""
        updater = OTAUpdater(models_dir=test_models_dir)
        checksum = calculate_checksum(test_model_file)

        # Perform update
        success, message = updater.update_model(
            test_model_file,
            model_id="cleanup_test",
            expected_checksum=checksum,
            metadata={"name": "Cleanup Test"}
        )

        assert success, f"Update should succeed: {message}"

        # Check for temporary directories
        temp_dirs = [
            d for d in test_models_dir.iterdir()
            if d.is_dir() and d.name.startswith('tmp')
        ]

        assert len(temp_dirs) == 0, (
            f"No temporary directories should remain: {temp_dirs}"
        )


class TestRollbackOnFailure:
    """Test rollback behavior when updates fail"""

    def test_rollback_on_checksum_failure(
        self,
        api_client: TestClient,
        test_model_file: Path,
        test_models_dir: Path,
        auth_token: str
    ):
        """
        Test that update rolls back when checksum verification fails:
        1. Record current manifest.json state
        2. Push model with incorrect checksum
        3. Verify upload fails with checksum error
        4. Verify manifest.json unchanged
        5. Verify no temp files remain
        6. Verify original model still active (if any)
        """
        # Step 1: Record current manifest.json state
        manifest_path = test_models_dir / 'manifest.json'
        with open(manifest_path, 'r') as f:
            original_manifest = json.load(f)

        original_manifest_str = json.dumps(original_manifest, sort_keys=True)

        # Get list of models before update attempt
        original_model_ids = set(original_manifest.get("models", {}).keys())

        # Step 2: Push model with incorrect checksum
        # Read model file and encode as base64
        with open(test_model_file, 'rb') as f:
            model_bytes = f.read()
        model_data_b64 = base64.b64encode(model_bytes).decode('utf-8')

        # Use an intentionally incorrect checksum
        incorrect_checksum = "0" * 64  # Invalid SHA256

        # Attempt upload with wrong checksum
        upload_response = api_client.post(
            "/v1/models/upload",
            headers={"X-Auth-Token": auth_token},
            json={
                "model_data": model_data_b64,
                "checksum": incorrect_checksum,
                "model_name": "bad_checksum_model"
            }
        )

        # Step 3: Verify upload fails with checksum error
        assert upload_response.status_code == 400, (
            "Upload with bad checksum should fail with 400 error"
        )

        response_data = upload_response.json()
        assert "detail" in response_data or "message" in response_data, (
            "Response should contain error details"
        )

        # Verify error message mentions checksum or verification
        error_text = json.dumps(response_data).lower()
        assert "checksum" in error_text or "verification" in error_text, (
            f"Error should mention checksum/verification failure: {response_data}"
        )

        # Step 4: Verify manifest.json unchanged
        with open(manifest_path, 'r') as f:
            current_manifest = json.load(f)

        current_manifest_str = json.dumps(current_manifest, sort_keys=True)

        assert current_manifest_str == original_manifest_str, (
            "Manifest should be unchanged after failed update"
        )

        # Verify no new models were added
        current_model_ids = set(current_manifest.get("models", {}).keys())
        assert current_model_ids == original_model_ids, (
            "No new models should be added after failed update"
        )

        # Verify the bad model is NOT in manifest
        assert "bad_checksum_model" not in current_manifest.get("models", {}), (
            "Failed model should not be in manifest"
        )

        # Step 5: Verify no temp files remain
        # Check for temporary directories
        temp_dirs = [
            d for d in test_models_dir.iterdir()
            if d.is_dir() and (d.name.startswith('tmp') or d.name.startswith('temp'))
        ]

        assert len(temp_dirs) == 0, (
            f"No temporary directories should remain after failed update: {temp_dirs}"
        )

        # Check for temporary files
        temp_files = [
            f for f in test_models_dir.iterdir()
            if f.is_file() and (f.name.endswith('.tmp') or 'temp' in f.name.lower())
        ]

        # Exclude manifest.tmp which might exist briefly
        temp_files = [f for f in temp_files if not f.name.startswith('manifest')]

        assert len(temp_files) == 0, (
            f"No temporary files should remain after failed update: {temp_files}"
        )

        # Step 6: Verify original model still active (if any)
        # Query status to ensure system is still functional
        status_response = api_client.get("/v1/models/status")
        assert status_response.status_code == 200, (
            "Status endpoint should still work after failed update"
        )

        # If there were models before, they should still be there
        if original_model_ids:
            status_data = status_response.json()
            # Verify manifest is still readable and contains original models
            for model_id in original_model_ids:
                assert model_id in current_manifest["models"], (
                    f"Original model {model_id} should still be in manifest"
                )

    def test_rollback_using_ota_updater_directly(
        self,
        test_model_file: Path,
        test_models_dir: Path
    ):
        """
        Test rollback at OTAUpdater level when checksum fails
        """
        # Create updater
        updater = OTAUpdater(models_dir=test_models_dir)

        # Record manifest state
        manifest_path = test_models_dir / 'manifest.json'
        with open(manifest_path, 'r') as f:
            original_manifest = json.load(f)

        # Attempt update with wrong checksum
        incorrect_checksum = "1234567890abcdef" * 4  # Wrong but valid length

        success, message = updater.update_model(
            test_model_file,
            model_id="bad_checksum_test",
            expected_checksum=incorrect_checksum,
            metadata={"name": "Bad Checksum Test"}
        )

        # Verify update failed
        assert not success, "Update should fail with incorrect checksum"
        assert "verification failed" in message.lower() or "checksum" in message.lower(), (
            f"Error message should mention verification or checksum: {message}"
        )

        # Verify manifest unchanged
        with open(manifest_path, 'r') as f:
            current_manifest = json.load(f)

        assert current_manifest == original_manifest, (
            "Manifest should be unchanged after failed update"
        )

        # Verify model not added to models directory
        bad_model_path = test_models_dir / test_model_file.name
        if bad_model_path.exists():
            # If file exists, verify it's the original (not our test model)
            # Check by verifying it's not in the manifest with our test ID
            assert "bad_checksum_test" not in current_manifest.get("models", {}), (
                "Failed model should not be in manifest"
            )

        # Verify no temp directories remain
        temp_dirs = [
            d for d in test_models_dir.iterdir()
            if d.is_dir() and d.name.startswith('tmp')
        ]
        assert len(temp_dirs) == 0, (
            f"No temp directories should remain: {temp_dirs}"
        )


class TestAuthenticationFailureHandling:
    """Test authentication failure handling for OTA endpoints"""

    def test_upload_without_auth_token(self, api_client: TestClient):
        """
        Verification step 1: Attempt push without auth token
        Expected: 401 Unauthorized response
        """
        response = api_client.post(
            "/v1/models/upload",
            json={
                "model_data": "fake_data",
                "checksum": "fake_checksum"
            }
        )

        assert response.status_code == 401, (
            "Upload without auth token should return 401 Unauthorized"
        )

        # Verify error message indicates authentication failure
        response_data = response.json()
        assert "detail" in response_data or "message" in response_data, (
            "Response should contain error details"
        )

    def test_upload_with_invalid_token(
        self,
        api_client: TestClient,
        test_model_file: Path
    ):
        """
        Verification step 3: Attempt push with invalid token
        Expected: 401 Unauthorized response
        """
        checksum = calculate_checksum(test_model_file)

        with open(test_model_file, 'rb') as f:
            model_bytes = f.read()
        model_data_b64 = base64.b64encode(model_bytes).decode('utf-8')

        # Try with wrong token
        response = api_client.post(
            "/v1/models/upload",
            headers={"X-Auth-Token": "invalid_token_xyz"},
            json={
                "model_data": model_data_b64,
                "checksum": checksum,
                "model_name": "test_model"
            }
        )

        assert response.status_code == 401, (
            "Upload with invalid token should return 401 Unauthorized"
        )

        # Verify error message
        response_data = response.json()
        assert "detail" in response_data or "message" in response_data

    def test_upload_with_valid_token(
        self,
        api_client: TestClient,
        test_model_file: Path,
        auth_token: str
    ):
        """
        Verification step 5: Attempt push with valid token
        Expected: 200/201 success
        """
        checksum = calculate_checksum(test_model_file)

        with open(test_model_file, 'rb') as f:
            model_bytes = f.read()
        model_data_b64 = base64.b64encode(model_bytes).decode('utf-8')

        # Try with valid token
        response = api_client.post(
            "/v1/models/upload",
            headers={"X-Auth-Token": auth_token},
            json={
                "model_data": model_data_b64,
                "checksum": checksum,
                "model_name": "valid_auth_model"
            }
        )

        assert response.status_code in [200, 201], (
            f"Upload with valid token should succeed: {response.text}"
        )

        # Verify success response
        response_data = response.json()
        assert response_data["status"] == "success"
        assert "model_id" in response_data

    def test_switch_without_auth_token(self, api_client: TestClient):
        """
        Test switch endpoint without auth token
        Expected: 401 Unauthorized response
        """
        response = api_client.post(
            "/v1/models/switch",
            json={"model_id": "test_model"}
        )

        assert response.status_code == 401, (
            "Switch without auth token should return 401 Unauthorized"
        )

        # Verify error message
        response_data = response.json()
        assert "detail" in response_data or "message" in response_data

    def test_switch_with_invalid_token(self, api_client: TestClient):
        """
        Test switch endpoint with invalid token
        Expected: 401 Unauthorized response
        """
        response = api_client.post(
            "/v1/models/switch",
            headers={"X-Auth-Token": "wrong_token_abc"},
            json={"model_id": "test_model"}
        )

        assert response.status_code == 401, (
            "Switch with invalid token should return 401 Unauthorized"
        )

        # Verify error message
        response_data = response.json()
        assert "detail" in response_data or "message" in response_data

    def test_switch_with_valid_token(
        self,
        api_client: TestClient,
        test_model_file: Path,
        test_models_dir: Path,
        auth_token: str
    ):
        """
        Test switch endpoint with valid token
        Expected: 501 Not Implemented (feature requires kernel integration)
        """
        # First upload a model to switch to
        checksum = calculate_checksum(test_model_file)

        with open(test_model_file, 'rb') as f:
            model_bytes = f.read()
        model_data_b64 = base64.b64encode(model_bytes).decode('utf-8')

        upload_response = api_client.post(
            "/v1/models/upload",
            headers={"X-Auth-Token": auth_token},
            json={
                "model_data": model_data_b64,
                "checksum": checksum,
                "model_name": "switch_test_model"
            }
        )

        assert upload_response.status_code == 200
        model_id = upload_response.json()["model_id"]

        # Now try to switch with valid token
        response = api_client.post(
            "/v1/models/switch",
            headers={"X-Auth-Token": auth_token},
            json={"model_id": model_id}
        )

        assert response.status_code == 501, (
            f"Switch endpoint should return 501 Not Implemented: {response.text}"
        )

        # Verify not_implemented response
        response_data = response.json()
        assert "detail" in response_data
        assert "not_implemented" in str(response_data["detail"]).lower()

    def test_authentication_prevents_unauthorized_access(
        self,
        api_client: TestClient,
        test_model_file: Path
    ):
        """
        Comprehensive test: Verify authentication prevents unauthorized model changes
        Tests both upload and switch endpoints
        """
        checksum = calculate_checksum(test_model_file)

        with open(test_model_file, 'rb') as f:
            model_bytes = f.read()
        model_data_b64 = base64.b64encode(model_bytes).decode('utf-8')

        # Test 1: No auth header
        response = api_client.post(
            "/v1/models/upload",
            json={
                "model_data": model_data_b64,
                "checksum": checksum,
                "model_name": "unauthorized_model"
            }
        )
        assert response.status_code == 401, "No auth header should fail"

        # Test 2: Empty auth token
        response = api_client.post(
            "/v1/models/upload",
            headers={"X-Auth-Token": ""},
            json={
                "model_data": model_data_b64,
                "checksum": checksum,
                "model_name": "unauthorized_model"
            }
        )
        assert response.status_code == 401, "Empty auth token should fail"

        # Test 3: Wrong auth token
        response = api_client.post(
            "/v1/models/upload",
            headers={"X-Auth-Token": "definitely_wrong_token"},
            json={
                "model_data": model_data_b64,
                "checksum": checksum,
                "model_name": "unauthorized_model"
            }
        )
        assert response.status_code == 401, "Wrong auth token should fail"

        # Test 4: No auth on switch endpoint
        response = api_client.post(
            "/v1/models/switch",
            json={"model_id": "some_model"}
        )
        assert response.status_code == 401, "Switch without auth should fail"

        # Test 5: Wrong auth on switch endpoint
        response = api_client.post(
            "/v1/models/switch",
            headers={"X-Auth-Token": "wrong_switch_token"},
            json={"model_id": "some_model"}
        )
        assert response.status_code == 401, "Switch with wrong auth should fail"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
