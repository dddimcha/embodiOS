"""
Rollback manager for EMBODIOS OTA updates

Handles rollback operations to restore previous versions when updates fail.
"""

import json
from pathlib import Path
from typing import Optional, Dict, Any, List
from datetime import datetime

from embodi.updater.backup import BackupManager
from embodi.updater.config import UpdateConfig


class RollbackManager:
    """
    Manages rollback operations for EMBODIOS OTA updates.

    Restores from backups when updates fail and maintains rollback history
    for tracking and debugging.
    """

    def __init__(self, config: Optional[UpdateConfig] = None,
                 backup_manager: Optional[BackupManager] = None):
        """
        Initialize rollback manager.

        Args:
            config: Optional UpdateConfig instance.
                   Creates a new one if not provided.
            backup_manager: Optional BackupManager instance.
                           Creates a new one if not provided.
        """
        self.config = config if config else UpdateConfig()
        self.backup_manager = backup_manager if backup_manager else BackupManager()

        # Rollback history file
        self.history_file = self.config.config_dir / 'rollback_history.json'

        # Load rollback history
        self.rollback_history: List[Dict[str, Any]] = []
        self._load_history()

    def _load_history(self):
        """Load rollback history from file"""
        if self.history_file.exists():
            try:
                with open(self.history_file, 'r') as f:
                    self.rollback_history = json.load(f)
            except Exception as e:
                print(f"Warning: Could not load rollback history: {e}")
                self.rollback_history = []
        else:
            self.rollback_history = []

    def _save_history(self):
        """Save rollback history to file"""
        try:
            with open(self.history_file, 'w') as f:
                json.dump(self.rollback_history, f, indent=2)
        except Exception as e:
            print(f"Warning: Could not save rollback history: {e}")

    def _record_rollback(self, from_version: str, to_version: str,
                        reason: str, success: bool, details: Optional[Dict[str, Any]] = None):
        """
        Record a rollback operation in history.

        Args:
            from_version: Version being rolled back from
            to_version: Version being rolled back to
            reason: Reason for rollback
            success: Whether rollback succeeded
            details: Optional additional details
        """
        record = {
            'timestamp': datetime.utcnow().isoformat() + 'Z',
            'from_version': from_version,
            'to_version': to_version,
            'reason': reason,
            'success': success
        }

        if details:
            record.update(details)

        self.rollback_history.append(record)

        # Limit history size
        if len(self.rollback_history) > 50:
            self.rollback_history = self.rollback_history[-50:]

        self._save_history()

    def rollback_to_backup(self, backup_path: Path, reason: str = "Manual rollback") -> bool:
        """
        Roll back to a specific backup.

        Args:
            backup_path: Path to backup directory to restore from
            reason: Reason for the rollback

        Returns:
            True if rollback successful, False otherwise
        """
        if not backup_path.exists():
            print(f"Error: Backup not found: {backup_path}")
            return False

        try:
            # Load backup metadata to get version
            metadata_file = backup_path / 'backup.json'
            if not metadata_file.exists():
                print("Error: Backup metadata not found")
                return False

            with open(metadata_file, 'r') as f:
                metadata = json.load(f)

            backup_version = metadata.get('version', 'unknown')
            current_version = self.config.current_version

            print(f"Rolling back from version {current_version} to {backup_version}")
            print(f"Reason: {reason}")

            # Perform the restore
            success = self.backup_manager.restore_backup(backup_path, verify=True)

            if success:
                # Update current version in config
                self.config.current_version = backup_version
                self.config.save_state()

                # Record successful rollback
                self._record_rollback(
                    from_version=current_version,
                    to_version=backup_version,
                    reason=reason,
                    success=True,
                    details={'backup_path': str(backup_path)}
                )

                print(f"Rollback successful: Restored to version {backup_version}")
                return True
            else:
                # Record failed rollback
                self._record_rollback(
                    from_version=current_version,
                    to_version=backup_version,
                    reason=reason,
                    success=False,
                    details={'backup_path': str(backup_path), 'error': 'Restore failed'}
                )

                print("Rollback failed: Could not restore from backup")
                return False

        except Exception as e:
            print(f"Error during rollback: {e}")

            # Record failed rollback
            self._record_rollback(
                from_version=self.config.current_version,
                to_version='unknown',
                reason=reason,
                success=False,
                details={'backup_path': str(backup_path), 'error': str(e)}
            )

            return False

    def rollback_to_latest(self, reason: str = "Manual rollback to latest") -> bool:
        """
        Roll back to the most recent backup.

        Args:
            reason: Reason for the rollback

        Returns:
            True if rollback successful, False otherwise
        """
        latest_backup = self.backup_manager.get_latest_backup()

        if not latest_backup:
            print("Error: No backups available for rollback")
            return False

        print(f"Using latest backup: {latest_backup.name}")
        return self.rollback_to_backup(latest_backup, reason)

    def rollback_to_version(self, version: str, reason: str = "Manual rollback to version") -> bool:
        """
        Roll back to a specific version.

        Args:
            version: Version to roll back to
            reason: Reason for the rollback

        Returns:
            True if rollback successful, False otherwise
        """
        # Find backup for the specified version
        backups = self.backup_manager.list_backups()

        target_backup = None
        for backup in backups:
            if backup['version'] == version:
                target_backup = backup['path']
                break

        if not target_backup:
            print(f"Error: No backup found for version {version}")
            print("Available versions:")
            for backup in backups:
                print(f"  - {backup['version']} ({backup['name']})")
            return False

        print(f"Found backup for version {version}: {target_backup.name}")
        return self.rollback_to_backup(target_backup, reason)

    def automatic_rollback(self, failed_version: str, error: str) -> bool:
        """
        Perform automatic rollback after update failure.

        Args:
            failed_version: Version that failed to apply
            error: Error message from failed update

        Returns:
            True if rollback successful, False otherwise
        """
        print(f"\n{'='*60}")
        print("UPDATE FAILED - Initiating automatic rollback")
        print(f"Failed version: {failed_version}")
        print(f"Error: {error}")
        print(f"{'='*60}\n")

        reason = f"Automatic rollback due to update failure: {error}"

        success = self.rollback_to_latest(reason)

        if success:
            # Record the failed update in config
            self.config.record_update(
                version=failed_version,
                success=False,
                details={'error': error, 'auto_rollback': True}
            )

            print("\n" + "="*60)
            print("ROLLBACK COMPLETE - System restored to previous version")
            print(f"Current version: {self.config.current_version}")
            print("="*60 + "\n")
        else:
            print("\n" + "="*60)
            print("CRITICAL: ROLLBACK FAILED")
            print("Manual intervention required!")
            print("="*60 + "\n")

        return success

    def list_rollback_history(self) -> List[Dict[str, Any]]:
        """
        Get rollback history.

        Returns:
            List of rollback records
        """
        return self.rollback_history.copy()

    def get_last_rollback(self) -> Optional[Dict[str, Any]]:
        """
        Get the most recent rollback operation.

        Returns:
            Most recent rollback record, or None if no history
        """
        if not self.rollback_history:
            return None

        return self.rollback_history[-1].copy()

    def can_rollback(self) -> bool:
        """
        Check if rollback is possible (i.e., backups exist).

        Returns:
            True if at least one backup exists, False otherwise
        """
        backups = self.backup_manager.list_backups()
        return len(backups) > 0

    def get_rollback_options(self) -> List[Dict[str, Any]]:
        """
        Get available rollback options (available backups).

        Returns:
            List of available backup versions with metadata
        """
        return self.backup_manager.list_backups()

    def __repr__(self) -> str:
        rollback_count = len(self.rollback_history)
        backup_count = len(self.backup_manager.list_backups())
        return f"RollbackManager(rollbacks={rollback_count}, backups_available={backup_count})"
