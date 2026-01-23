#!/usr/bin/env python3
"""
OTA Updater for EMBODIOS
Handles over-the-air model updates with atomic operations and rollback
"""

import os
import json
import shutil
import tempfile
import time
from pathlib import Path
from typing import Optional, Dict, Tuple
from datetime import datetime

from .update_verifier import UpdateVerifier


class OTAUpdater:
    """Manages over-the-air model updates with atomic operations"""

    def __init__(self, models_dir: Optional[Path] = None):
        """
        Initialize OTA updater

        Args:
            models_dir: Directory containing models (default: ./models)
        """
        if models_dir is None:
            # Default to models directory in project root
            self.models_dir = Path('models')
        else:
            self.models_dir = Path(models_dir)

        self.models_dir.mkdir(parents=True, exist_ok=True)
        self.manifest_path = self.models_dir / 'manifest.json'
        self.verifier = UpdateVerifier()

        # Backup directory for rollback
        self.backup_dir = self.models_dir / '.backup'
        self.backup_dir.mkdir(parents=True, exist_ok=True)

    def update_model(self, model_file: Path, model_id: str,
                    expected_checksum: Optional[str] = None,
                    metadata: Optional[Dict] = None) -> Tuple[bool, str]:
        """
        Perform atomic model update with rollback on failure

        Args:
            model_file: Path to model file to install
            model_id: Unique identifier for the model
            expected_checksum: Optional SHA256 checksum for verification
            metadata: Optional metadata about the model

        Returns:
            Tuple of (success: bool, message: str)
        """
        # Step 1: Create temporary directory for atomic operations
        with tempfile.TemporaryDirectory(dir=self.models_dir) as temp_dir:
            temp_path = Path(temp_dir)

            try:
                # Step 2: Copy model to temp directory
                temp_model_path = temp_path / model_file.name

                if not model_file.exists():
                    return False, f"Model file not found: {model_file}"

                shutil.copy2(model_file, temp_model_path)

                # Step 3: Verify the model
                verification = self.verifier.verify_update(
                    temp_model_path,
                    expected_checksum
                )

                if not verification['valid']:
                    errors = ', '.join(verification['errors'])
                    return False, f"Model verification failed: {errors}"

                # Step 4: Backup existing model if it exists
                final_model_path = self.models_dir / model_file.name
                backup_path = None

                if final_model_path.exists():
                    backup_path = self._backup_file(final_model_path, model_id)

                # Step 5: Backup current manifest
                manifest_backup = None
                if self.manifest_path.exists():
                    manifest_backup = self._backup_manifest()

                try:
                    # Step 6: Atomically move model to final location
                    shutil.move(str(temp_model_path), str(final_model_path))

                    # Step 7: Update manifest
                    self._update_manifest(
                        model_id,
                        final_model_path,
                        verification,
                        metadata
                    )

                    # Step 8: Cleanup old backup if update succeeded
                    if backup_path and backup_path.exists():
                        backup_path.unlink()

                    if manifest_backup and manifest_backup.exists():
                        manifest_backup.unlink()

                    return True, f"Model '{model_id}' updated successfully"

                except Exception as e:
                    # Rollback on failure
                    self._rollback(
                        final_model_path,
                        backup_path,
                        manifest_backup
                    )
                    return False, f"Update failed, rolled back: {str(e)}"

            except Exception as e:
                return False, f"Update failed during preparation: {str(e)}"

    def _backup_file(self, file_path: Path, model_id: str) -> Path:
        """
        Create backup of existing file

        Args:
            file_path: File to backup
            model_id: Model identifier for backup naming

        Returns:
            Path to backup file
        """
        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        backup_name = f"{model_id}_{timestamp}{file_path.suffix}"
        backup_path = self.backup_dir / backup_name

        shutil.copy2(file_path, backup_path)
        return backup_path

    def _backup_manifest(self) -> Path:
        """
        Create backup of manifest file

        Returns:
            Path to backup manifest
        """
        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        backup_path = self.backup_dir / f"manifest_{timestamp}.json"

        shutil.copy2(self.manifest_path, backup_path)
        return backup_path

    def _rollback(self, model_path: Path, backup_path: Optional[Path],
                  manifest_backup: Optional[Path]):
        """
        Rollback failed update

        Args:
            model_path: Path to newly installed model (to remove)
            backup_path: Path to backup of previous model (to restore)
            manifest_backup: Path to backup manifest (to restore)
        """
        # Remove failed model if it exists
        if model_path.exists():
            try:
                model_path.unlink()
            except Exception as e:
                print(f"Warning: Failed to remove failed model: {e}")

        # Restore previous model if backup exists
        if backup_path and backup_path.exists():
            try:
                shutil.copy2(backup_path, model_path)
            except Exception as e:
                print(f"Warning: Failed to restore backup model: {e}")

        # Restore previous manifest if backup exists
        if manifest_backup and manifest_backup.exists():
            try:
                shutil.copy2(manifest_backup, self.manifest_path)
            except Exception as e:
                print(f"Warning: Failed to restore backup manifest: {e}")

    def _update_manifest(self, model_id: str, model_path: Path,
                        verification: Dict, metadata: Optional[Dict]):
        """
        Update manifest.json with new model information

        Args:
            model_id: Model identifier
            model_path: Path to model file
            verification: Verification results from UpdateVerifier
            metadata: Optional additional metadata
        """
        # Load existing manifest or create new one
        if self.manifest_path.exists():
            with open(self.manifest_path, 'r') as f:
                manifest = json.load(f)
        else:
            manifest = {
                'models': {},
                'schema_version': '1.0'
            }

        # Store previous version for rollback tracking
        previous_version = None
        if model_id in manifest['models']:
            previous_version = manifest['models'][model_id].get('version')

        # Prepare model entry
        model_entry = {
            'name': metadata.get('name', model_id) if metadata else model_id,
            'version': metadata.get('version', 'ota-update') if metadata else 'ota-update',
            'files': {
                verification['format']: {
                    'filename': model_path.name,
                    'size': verification['size'],
                    'sha256': verification['checksum']
                }
            },
            'source': metadata.get('source', 'ota-update') if metadata else 'ota-update',
            'quantization': metadata.get('quantization', 'unknown') if metadata else 'unknown',
            'last_updated': datetime.now().isoformat(),
            'update_source': 'OTA',
            'last_update_time': datetime.now().isoformat(),
            'previous_version': previous_version
        }

        # Add optional metadata fields
        if metadata:
            if 'license' in metadata:
                model_entry['license'] = metadata['license']
            if 'revision' in metadata:
                model_entry['revision'] = metadata['revision']
            if 'url' in metadata:
                model_entry['files'][verification['format']]['url'] = metadata['url']

        # Update manifest
        manifest['models'][model_id] = model_entry

        # Write manifest atomically (write to temp file, then rename)
        temp_manifest = self.manifest_path.with_suffix('.tmp')
        with open(temp_manifest, 'w') as f:
            json.dump(manifest, f, indent=2)

        # Atomic rename
        temp_manifest.replace(self.manifest_path)

    def get_model_info(self, model_id: str) -> Optional[Dict]:
        """
        Get information about a model from manifest

        Args:
            model_id: Model identifier

        Returns:
            Model information dict or None if not found
        """
        if not self.manifest_path.exists():
            return None

        with open(self.manifest_path, 'r') as f:
            manifest = json.load(f)

        return manifest.get('models', {}).get(model_id)

    def list_models(self) -> Dict[str, Dict]:
        """
        List all models in manifest

        Returns:
            Dictionary of model_id -> model_info
        """
        if not self.manifest_path.exists():
            return {}

        with open(self.manifest_path, 'r') as f:
            manifest = json.load(f)

        return manifest.get('models', {})

    def update_from_file(self, file_path: str, checksum: Optional[str] = None,
                        model_name: Optional[str] = None) -> str:
        """
        Update model from a local file path

        Args:
            file_path: Path to model file
            checksum: Optional SHA256 checksum for verification
            model_name: Optional name for the model

        Returns:
            Model ID of the updated model
        """
        file_path_obj = Path(file_path)

        # Generate model ID from filename or use provided name
        if model_name:
            model_id = model_name.lower().replace(' ', '_')
        else:
            model_id = file_path_obj.stem.lower().replace(' ', '_')

        # Prepare metadata
        metadata = {
            'name': model_name or file_path_obj.stem,
        }

        # Perform update
        success, message = self.update_model(
            file_path_obj,
            model_id,
            expected_checksum=checksum,
            metadata=metadata
        )

        if not success:
            raise ValueError(message)

        return model_id

    def update_from_url(self, url: str, checksum: Optional[str] = None,
                       model_name: Optional[str] = None) -> str:
        """
        Update model from a URL

        Args:
            url: URL to download model from
            checksum: Optional SHA256 checksum for verification
            model_name: Optional name for the model

        Returns:
            Model ID of the updated model
        """
        import urllib.request
        import socket

        # Download to temporary file
        filename = url.split('/')[-1]
        if not filename.endswith('.gguf'):
            filename += '.gguf'

        with tempfile.NamedTemporaryFile(delete=False, suffix='.gguf') as tmp_file:
            tmp_path = Path(tmp_file.name)

        try:
            # Download file with timeout
            old_timeout = socket.getdefaulttimeout()
            try:
                socket.setdefaulttimeout(30)  # 30 second timeout
                urllib.request.urlretrieve(url, tmp_path)
            finally:
                socket.setdefaulttimeout(old_timeout)

            # Update from downloaded file
            model_id = self.update_from_file(
                str(tmp_path),
                checksum=checksum,
                model_name=model_name
            )

            return model_id
        finally:
            # Clean up temp file
            if tmp_path.exists():
                tmp_path.unlink()

    def cleanup_backups(self, keep_latest: int = 5):
        """
        Clean up old backup files, keeping only the most recent

        Args:
            keep_latest: Number of latest backups to keep per model
        """
        if not self.backup_dir.exists():
            return

        # Group backups by model
        backups = {}
        for backup_file in self.backup_dir.glob('*'):
            if backup_file.is_file():
                # Extract model ID from filename (format: modelid_timestamp.ext)
                parts = backup_file.stem.split('_')
                if len(parts) >= 2:
                    model_id = parts[0]
                    if model_id not in backups:
                        backups[model_id] = []
                    backups[model_id].append(backup_file)

        # Sort and remove old backups
        for model_id, files in backups.items():
            # Sort by modification time (newest first)
            files.sort(key=lambda p: p.stat().st_mtime, reverse=True)

            # Remove old backups
            for old_backup in files[keep_latest:]:
                try:
                    old_backup.unlink()
                except Exception as e:
                    print(f"Warning: Failed to remove old backup {old_backup}: {e}")
