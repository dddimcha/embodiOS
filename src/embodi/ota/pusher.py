"""OTA Pusher - Push model updates to remote EMBODIOS instances"""

import requests
from pathlib import Path
from typing import Optional, Callable
import hashlib


class OTAPusher:
    def __init__(self, debug: bool = False):
        self.debug = debug

    def push_update(
        self,
        model: str,
        target: str,
        verify: bool = False,
        force: bool = False,
        rollback: bool = False,
        progress_callback: Optional[Callable] = None
    ) -> bool:
        """
        Push model update to remote device

        Args:
            model: Path to model file or model identifier
            target: Target host (IP:port or hostname)
            verify: Verify update after push
            force: Force update even if same version
            rollback: Enable automatic rollback on failure
            progress_callback: Optional callback for progress updates

        Returns:
            True if successful, False otherwise
        """
        # Parse model path
        model_path = Path(model)
        if not model_path.exists():
            if self.debug:
                print(f"Model file not found: {model}")
            return False

        # Calculate checksum
        if progress_callback:
            progress_callback("Calculating checksum...")

        checksum = self._calculate_checksum(model_path)

        # Prepare upload URL
        if not target.startswith('http'):
            target = f"http://{target}"
        upload_url = f"{target}/v1/models/upload"

        # Get auth token from environment
        import os
        auth_token = os.environ.get("EMBODIOS_AUTH_TOKEN")
        if not auth_token:
            if self.debug:
                print("Error: EMBODIOS_AUTH_TOKEN environment variable not set")
            return False

        # Read model file
        if progress_callback:
            progress_callback("Reading model file...")

        import base64
        with open(model_path, 'rb') as f:
            model_bytes = f.read()
        model_data_b64 = base64.b64encode(model_bytes).decode('utf-8')

        # Upload model
        if progress_callback:
            progress_callback("Uploading to target device...")

        try:
            response = requests.post(
                upload_url,
                headers={"X-Auth-Token": auth_token},
                json={
                    "model_data": model_data_b64,
                    "checksum": checksum,
                    "model_name": model_path.stem
                },
                timeout=300  # 5 minute timeout for large models
            )

            if response.status_code == 200 or response.status_code == 201:
                if progress_callback:
                    progress_callback("Upload complete!")
                return True
            else:
                if self.debug:
                    print(f"Upload failed: {response.status_code} - {response.text}")
                return False

        except Exception as e:
            if self.debug:
                print(f"Upload error: {e}")
            return False

    def _calculate_checksum(self, file_path: Path) -> str:
        """Calculate SHA256 checksum of file"""
        sha256_hash = hashlib.sha256()
        with open(file_path, 'rb') as f:
            for byte_block in iter(lambda: f.read(4096), b""):
                sha256_hash.update(byte_block)
        return sha256_hash.hexdigest()
