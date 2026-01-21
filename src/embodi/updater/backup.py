"""
Backup manager for EMBODIOS OTA updates

Handles backing up and restoring EMBODIOS kernel and models before updates.
"""

import json
import shutil
import hashlib
from pathlib import Path
from typing import Optional, Dict, Any, List
from datetime import datetime


class BackupManager:
    """
    Manages backups of EMBODIOS installations for OTA updates.

    Creates backups of kernel and model files before updates,
    allowing rollback if updates fail.
    """

    def __init__(self, backup_dir: Optional[Path] = None):
        """
        Initialize backup manager.

        Args:
            backup_dir: Optional custom backup directory.
                       Defaults to ~/.embodi/updater/backups/
        """
        if backup_dir is None:
            self.backup_dir = Path.home() / '.embodi' / 'updater' / 'backups'
        else:
            self.backup_dir = backup_dir

        self.backup_dir.mkdir(parents=True, exist_ok=True)

        # Track installation paths
        self.install_root = Path.home() / '.embodi'
        self.kernel_dir = self.install_root / 'kernel'
        self.models_dir = self.install_root / 'models'

    def create_backup(self, version: str, include_models: bool = True) -> Optional[Path]:
        """
        Create a backup of the current installation.

        Args:
            version: Version identifier for this backup
            include_models: Whether to include model files in backup

        Returns:
            Path to backup directory if successful, None otherwise
        """
        timestamp = datetime.utcnow().strftime('%Y%m%d_%H%M%S')
        backup_name = f'backup-{version}-{timestamp}'
        backup_path = self.backup_dir / backup_name

        print(f"Creating backup: {backup_name}")

        try:
            backup_path.mkdir(parents=True, exist_ok=True)

            # Backup metadata
            metadata = {
                'version': version,
                'timestamp': datetime.utcnow().isoformat() + 'Z',
                'include_models': include_models,
                'files': []
            }

            # Backup kernel files
            if self.kernel_dir.exists():
                kernel_backup = backup_path / 'kernel'
                kernel_backup.mkdir(exist_ok=True)

                for kernel_file in self.kernel_dir.iterdir():
                    if kernel_file.is_file():
                        dest = kernel_backup / kernel_file.name
                        shutil.copy2(kernel_file, dest)

                        metadata['files'].append({
                            'type': 'kernel',
                            'name': kernel_file.name,
                            'size': kernel_file.stat().st_size,
                            'checksum': self._calculate_checksum(kernel_file)
                        })

                print(f"  Backed up kernel files")

            # Backup models if requested
            if include_models and self.models_dir.exists():
                models_backup = backup_path / 'models'
                models_backup.mkdir(exist_ok=True)

                model_count = 0
                for model_file in self.models_dir.iterdir():
                    if model_file.is_file() and model_file.suffix in ['.aios', '.gguf', '.bin']:
                        dest = models_backup / model_file.name
                        shutil.copy2(model_file, dest)

                        metadata['files'].append({
                            'type': 'model',
                            'name': model_file.name,
                            'size': model_file.stat().st_size,
                            'checksum': self._calculate_checksum(model_file)
                        })
                        model_count += 1

                if model_count > 0:
                    print(f"  Backed up {model_count} model file(s)")

            # Save metadata
            metadata_file = backup_path / 'backup.json'
            with open(metadata_file, 'w') as f:
                json.dump(metadata, f, indent=2)

            print(f"Backup created: {backup_path}")
            return backup_path

        except Exception as e:
            print(f"Error creating backup: {e}")
            # Clean up partial backup
            if backup_path.exists():
                try:
                    shutil.rmtree(backup_path)
                except Exception:
                    pass
            return None

    def restore_backup(self, backup_path: Path, verify: bool = True) -> bool:
        """
        Restore from a backup.

        Args:
            backup_path: Path to backup directory
            verify: Whether to verify checksums before restoring

        Returns:
            True if restore successful, False otherwise
        """
        if not backup_path.exists():
            print(f"Error: Backup not found: {backup_path}")
            return False

        print(f"Restoring from backup: {backup_path.name}")

        try:
            # Load metadata
            metadata_file = backup_path / 'backup.json'
            if not metadata_file.exists():
                print("Error: Backup metadata not found")
                return False

            with open(metadata_file, 'r') as f:
                metadata = json.load(f)

            # Verify checksums if requested
            if verify:
                print("  Verifying backup integrity...")
                if not self._verify_backup(backup_path, metadata):
                    print("Error: Backup verification failed")
                    return False

            # Restore kernel files
            kernel_backup = backup_path / 'kernel'
            if kernel_backup.exists():
                self.kernel_dir.mkdir(parents=True, exist_ok=True)

                for kernel_file in kernel_backup.iterdir():
                    if kernel_file.is_file():
                        dest = self.kernel_dir / kernel_file.name
                        shutil.copy2(kernel_file, dest)

                print("  Restored kernel files")

            # Restore models if they were backed up
            models_backup = backup_path / 'models'
            if models_backup.exists():
                self.models_dir.mkdir(parents=True, exist_ok=True)

                model_count = 0
                for model_file in models_backup.iterdir():
                    if model_file.is_file():
                        dest = self.models_dir / model_file.name
                        shutil.copy2(model_file, dest)
                        model_count += 1

                if model_count > 0:
                    print(f"  Restored {model_count} model file(s)")

            print(f"Restore complete from {backup_path.name}")
            return True

        except Exception as e:
            print(f"Error restoring backup: {e}")
            return False

    def list_backups(self) -> List[Dict[str, Any]]:
        """
        List all available backups.

        Returns:
            List of backup information dictionaries
        """
        backups = []

        if not self.backup_dir.exists():
            return backups

        for backup_path in sorted(self.backup_dir.iterdir(), reverse=True):
            if not backup_path.is_dir():
                continue

            metadata_file = backup_path / 'backup.json'
            if not metadata_file.exists():
                continue

            try:
                with open(metadata_file, 'r') as f:
                    metadata = json.load(f)

                # Calculate total size
                total_size = sum(f['size'] for f in metadata.get('files', []))

                backups.append({
                    'path': backup_path,
                    'name': backup_path.name,
                    'version': metadata.get('version'),
                    'timestamp': metadata.get('timestamp'),
                    'include_models': metadata.get('include_models', True),
                    'file_count': len(metadata.get('files', [])),
                    'total_size': total_size
                })
            except Exception:
                continue

        return backups

    def get_latest_backup(self) -> Optional[Path]:
        """
        Get the most recent backup.

        Returns:
            Path to latest backup, or None if no backups exist
        """
        backups = self.list_backups()
        if not backups:
            return None

        return backups[0]['path']

    def delete_backup(self, backup_path: Path) -> bool:
        """
        Delete a backup.

        Args:
            backup_path: Path to backup directory to delete

        Returns:
            True if deletion successful, False otherwise
        """
        if not backup_path.exists():
            print(f"Error: Backup not found: {backup_path}")
            return False

        try:
            shutil.rmtree(backup_path)
            print(f"Deleted backup: {backup_path.name}")
            return True
        except Exception as e:
            print(f"Error deleting backup: {e}")
            return False

    def cleanup_old_backups(self, keep: int = 3):
        """
        Remove old backups beyond the retention limit.

        Args:
            keep: Number of backups to keep
        """
        backups = self.list_backups()

        if len(backups) <= keep:
            return

        print(f"Cleaning up old backups (keeping {keep} most recent)")

        for backup in backups[keep:]:
            self.delete_backup(backup['path'])

    def _calculate_checksum(self, file_path: Path) -> str:
        """
        Calculate SHA256 checksum of a file.

        Args:
            file_path: Path to file

        Returns:
            Hex digest of file checksum
        """
        sha256 = hashlib.sha256()

        with open(file_path, 'rb') as f:
            while True:
                chunk = f.read(8192)
                if not chunk:
                    break
                sha256.update(chunk)

        return sha256.hexdigest()

    def _verify_backup(self, backup_path: Path, metadata: Dict[str, Any]) -> bool:
        """
        Verify backup integrity using checksums.

        Args:
            backup_path: Path to backup directory
            metadata: Backup metadata with checksums

        Returns:
            True if all checksums match, False otherwise
        """
        for file_info in metadata.get('files', []):
            file_type = file_info['type']
            file_name = file_info['name']
            expected_checksum = file_info['checksum']

            if file_type == 'kernel':
                file_path = backup_path / 'kernel' / file_name
            elif file_type == 'model':
                file_path = backup_path / 'models' / file_name
            else:
                continue

            if not file_path.exists():
                print(f"  Missing file: {file_name}")
                return False

            actual_checksum = self._calculate_checksum(file_path)
            if actual_checksum != expected_checksum:
                print(f"  Checksum mismatch: {file_name}")
                return False

        return True

    def __repr__(self) -> str:
        backup_count = len(self.list_backups())
        return f"BackupManager(backups={backup_count}, dir={self.backup_dir})"
