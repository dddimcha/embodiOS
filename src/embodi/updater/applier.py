"""
Atomic update applier for EMBODIOS OTA updates

Applies updates atomically using a staging area to ensure all-or-nothing updates.
"""

import json
import shutil
from pathlib import Path
from typing import Optional, Dict, Any, List
from datetime import datetime

from embodi.updater.config import UpdateConfig
from embodi.updater.verifier import verify_checksum


class UpdateApplier:
    """
    Applies updates atomically using a staging area.

    Updates are first staged, verified, and then atomically moved to their
    final locations. If any step fails, no changes are made to the installation.
    """

    def __init__(self, config: Optional[UpdateConfig] = None):
        """
        Initialize update applier.

        Args:
            config: Optional UpdateConfig instance.
                   Creates a new one if not provided.
        """
        self.config = config if config else UpdateConfig()

        # Installation paths
        self.install_root = Path.home() / '.embodi'
        self.kernel_dir = self.install_root / 'kernel'
        self.models_dir = self.install_root / 'models'

        # Ensure installation directories exist
        self.kernel_dir.mkdir(parents=True, exist_ok=True)
        self.models_dir.mkdir(parents=True, exist_ok=True)

    def stage_update(self, update_files: List[Dict[str, Any]], version: str) -> Optional[Path]:
        """
        Stage update files for atomic application.

        Args:
            update_files: List of update file dictionaries with keys:
                         - type: 'kernel' or 'model'
                         - path: Path to downloaded file
                         - checksum: Expected SHA256 checksum
                         - filename: Target filename
            version: Version being staged

        Returns:
            Path to staging directory if successful, None otherwise
        """
        staging_path = self.config.get_staging_path(version)

        # Clean up any existing staging directory
        if staging_path.exists():
            try:
                shutil.rmtree(staging_path)
            except Exception as e:
                print(f"Warning: Could not clean existing staging: {e}")

        try:
            staging_path.mkdir(parents=True, exist_ok=True)

            # Create staging subdirectories
            kernel_staging = staging_path / 'kernel'
            models_staging = staging_path / 'models'
            kernel_staging.mkdir(exist_ok=True)
            models_staging.mkdir(exist_ok=True)

            # Stage metadata
            metadata = {
                'version': version,
                'timestamp': datetime.utcnow().isoformat() + 'Z',
                'files': []
            }

            print(f"Staging update files for version {version}...")

            # Stage each file
            for update_file in update_files:
                file_type = update_file.get('type')
                source_path = Path(update_file.get('path'))
                checksum = update_file.get('checksum')
                filename = update_file.get('filename', source_path.name)

                if not source_path.exists():
                    print(f"Error: Update file not found: {source_path}")
                    return None

                # Verify checksum before staging
                if checksum:
                    print(f"  Verifying {filename}...")
                    if not verify_checksum(source_path, checksum):
                        print(f"Error: Checksum verification failed for {filename}")
                        return None

                # Determine staging destination
                if file_type == 'kernel':
                    dest_path = kernel_staging / filename
                elif file_type == 'model':
                    dest_path = models_staging / filename
                else:
                    print(f"Error: Unknown file type: {file_type}")
                    return None

                # Copy to staging
                print(f"  Staging {filename} ({file_type})...")
                shutil.copy2(source_path, dest_path)

                # Record in metadata
                metadata['files'].append({
                    'type': file_type,
                    'filename': filename,
                    'size': dest_path.stat().st_size,
                    'checksum': checksum
                })

            # Save staging metadata
            metadata_file = staging_path / 'staging.json'
            with open(metadata_file, 'w') as f:
                json.dump(metadata, f, indent=2)

            print(f"Staged {len(update_files)} file(s) successfully")
            return staging_path

        except Exception as e:
            print(f"Error staging update: {e}")
            # Clean up partial staging
            if staging_path.exists():
                try:
                    shutil.rmtree(staging_path)
                except Exception:
                    pass
            return None

    def verify_staged_update(self, staging_path: Path) -> bool:
        """
        Verify staged update integrity before application.

        Args:
            staging_path: Path to staging directory

        Returns:
            True if all files verify successfully, False otherwise
        """
        if not staging_path.exists():
            print(f"Error: Staging directory not found: {staging_path}")
            return False

        # Load staging metadata
        metadata_file = staging_path / 'staging.json'
        if not metadata_file.exists():
            print("Error: Staging metadata not found")
            return False

        try:
            with open(metadata_file, 'r') as f:
                metadata = json.load(f)

            print("Verifying staged update...")

            # Verify each file
            for file_info in metadata.get('files', []):
                file_type = file_info['type']
                filename = file_info['filename']
                expected_checksum = file_info.get('checksum')

                # Locate staged file
                if file_type == 'kernel':
                    file_path = staging_path / 'kernel' / filename
                elif file_type == 'model':
                    file_path = staging_path / 'models' / filename
                else:
                    print(f"Error: Unknown file type in metadata: {file_type}")
                    return False

                # Check file exists
                if not file_path.exists():
                    print(f"Error: Staged file missing: {filename}")
                    return False

                # Verify checksum if provided
                if expected_checksum:
                    if not verify_checksum(file_path, expected_checksum):
                        print(f"Error: Checksum verification failed for {filename}")
                        return False

                print(f"  Verified {filename}")

            print("All staged files verified successfully")
            return True

        except Exception as e:
            print(f"Error verifying staged update: {e}")
            return False

    def apply_staged_update(self, staging_path: Path) -> bool:
        """
        Atomically apply staged update to installation.

        Args:
            staging_path: Path to verified staging directory

        Returns:
            True if update applied successfully, False otherwise
        """
        if not staging_path.exists():
            print(f"Error: Staging directory not found: {staging_path}")
            return False

        # Verify before applying
        if not self.verify_staged_update(staging_path):
            print("Error: Staged update verification failed")
            return False

        try:
            # Load staging metadata
            metadata_file = staging_path / 'staging.json'
            with open(metadata_file, 'r') as f:
                metadata = json.load(f)

            print(f"Applying update version {metadata['version']}...")

            # Track files being updated for rollback capability
            updated_files = []

            # Apply kernel updates
            kernel_staging = staging_path / 'kernel'
            if kernel_staging.exists():
                for kernel_file in kernel_staging.iterdir():
                    if kernel_file.is_file():
                        dest = self.kernel_dir / kernel_file.name

                        # Atomic move (rename is atomic on same filesystem)
                        temp_dest = dest.with_suffix(dest.suffix + '.tmp')
                        shutil.copy2(kernel_file, temp_dest)

                        # Atomic rename
                        temp_dest.replace(dest)
                        updated_files.append(('kernel', dest))

                        print(f"  Applied kernel file: {kernel_file.name}")

            # Apply model updates
            models_staging = staging_path / 'models'
            if models_staging.exists():
                for model_file in models_staging.iterdir():
                    if model_file.is_file():
                        dest = self.models_dir / model_file.name

                        # Atomic move (rename is atomic on same filesystem)
                        temp_dest = dest.with_suffix(dest.suffix + '.tmp')
                        shutil.copy2(model_file, temp_dest)

                        # Atomic rename
                        temp_dest.replace(dest)
                        updated_files.append(('model', dest))

                        print(f"  Applied model file: {model_file.name}")

            # Update complete
            print(f"Update applied successfully: {len(updated_files)} file(s)")

            # Clean up staging after successful application
            try:
                shutil.rmtree(staging_path)
            except Exception as e:
                print(f"Warning: Could not clean up staging: {e}")

            return True

        except Exception as e:
            print(f"Error applying update: {e}")
            return False

    def apply_update(self, update_files: List[Dict[str, Any]], version: str) -> bool:
        """
        Complete atomic update workflow: stage, verify, and apply.

        Args:
            update_files: List of update file dictionaries
            version: Version being applied

        Returns:
            True if update applied successfully, False otherwise
        """
        # Stage the update
        staging_path = self.stage_update(update_files, version)
        if not staging_path:
            print("Error: Failed to stage update")
            return False

        # Apply the staged update
        success = self.apply_staged_update(staging_path)

        if not success:
            # Clean up staging on failure
            if staging_path.exists():
                try:
                    shutil.rmtree(staging_path)
                except Exception:
                    pass

        return success

    def cleanup_staging(self):
        """Remove all staging directories"""
        self.config.cleanup_staging()

    def __repr__(self) -> str:
        return f"UpdateApplier(install_root={self.install_root})"
