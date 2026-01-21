"""
Update configuration and state management for EMBODIOS OTA updates
"""

import json
from pathlib import Path
from typing import Optional, Dict, Any, List
from datetime import datetime


class UpdateConfig:
    """
    Manages update configuration and state for EMBODIOS OTA updates.

    Stores configuration in ~/.embodi/updater/ directory and tracks
    update state including last check time, current version, and update history.
    """

    def __init__(self, config_dir: Optional[Path] = None):
        """
        Initialize update configuration.

        Args:
            config_dir: Optional custom config directory.
                       Defaults to ~/.embodi/updater/
        """
        # Set up directories
        if config_dir is None:
            self.config_dir = Path.home() / '.embodi' / 'updater'
        else:
            self.config_dir = config_dir

        self.config_dir.mkdir(parents=True, exist_ok=True)

        # Core directories
        self.update_dir = self.config_dir / 'downloads'
        self.backup_dir = self.config_dir / 'backups'
        self.staging_dir = self.config_dir / 'staging'

        # Create directories
        self.update_dir.mkdir(parents=True, exist_ok=True)
        self.backup_dir.mkdir(parents=True, exist_ok=True)
        self.staging_dir.mkdir(parents=True, exist_ok=True)

        # Config and state files
        self.config_file = self.config_dir / 'config.json'
        self.state_file = self.config_dir / 'state.json'

        # Load configuration
        self._load_config()
        self._load_state()

    def _load_config(self):
        """Load configuration from file or set defaults"""
        if self.config_file.exists():
            with open(self.config_file, 'r') as f:
                config = json.load(f)
        else:
            config = {}

        # Configuration settings with defaults
        self.update_server_url = config.get(
            'update_server_url',
            'https://updates.embodi.ai'
        )
        self.manifest_url = config.get(
            'manifest_url',
            f'{self.update_server_url}/manifest.json'
        )
        self.auto_check = config.get('auto_check', False)
        self.auto_apply = config.get('auto_apply', False)
        self.check_interval_hours = config.get('check_interval_hours', 24)
        self.keep_backups = config.get('keep_backups', 3)
        self.verify_tls = config.get('verify_tls', True)
        self.timeout_seconds = config.get('timeout_seconds', 300)

    def _load_state(self):
        """Load update state from file or initialize defaults"""
        if self.state_file.exists():
            with open(self.state_file, 'r') as f:
                state = json.load(f)
        else:
            state = {}

        # State tracking
        self.current_version = state.get('current_version', '0.1.0')
        self.last_check_time = state.get('last_check_time')
        self.last_update_time = state.get('last_update_time')
        self.last_update_version = state.get('last_update_version')
        self.update_history: List[Dict[str, Any]] = state.get('update_history', [])
        self.failed_updates: List[Dict[str, Any]] = state.get('failed_updates', [])

    def save_config(self):
        """Save configuration to file"""
        config = {
            'update_server_url': self.update_server_url,
            'manifest_url': self.manifest_url,
            'auto_check': self.auto_check,
            'auto_apply': self.auto_apply,
            'check_interval_hours': self.check_interval_hours,
            'keep_backups': self.keep_backups,
            'verify_tls': self.verify_tls,
            'timeout_seconds': self.timeout_seconds
        }

        with open(self.config_file, 'w') as f:
            json.dump(config, f, indent=2)

    def save_state(self):
        """Save update state to file"""
        state = {
            'current_version': self.current_version,
            'last_check_time': self.last_check_time,
            'last_update_time': self.last_update_time,
            'last_update_version': self.last_update_version,
            'update_history': self.update_history,
            'failed_updates': self.failed_updates
        }

        with open(self.state_file, 'w') as f:
            json.dump(state, f, indent=2)

    def record_check(self):
        """Record that an update check was performed"""
        self.last_check_time = datetime.utcnow().isoformat() + 'Z'
        self.save_state()

    def record_update(self, version: str, success: bool, details: Optional[Dict[str, Any]] = None):
        """
        Record an update attempt.

        Args:
            version: Version that was updated to
            success: Whether the update succeeded
            details: Optional additional details about the update
        """
        timestamp = datetime.utcnow().isoformat() + 'Z'

        record = {
            'version': version,
            'timestamp': timestamp,
            'success': success
        }

        if details:
            record.update(details)

        if success:
            self.current_version = version
            self.last_update_time = timestamp
            self.last_update_version = version
            self.update_history.append(record)

            # Limit history size
            if len(self.update_history) > 50:
                self.update_history = self.update_history[-50:]
        else:
            self.failed_updates.append(record)

            # Limit failed updates size
            if len(self.failed_updates) > 20:
                self.failed_updates = self.failed_updates[-20:]

        self.save_state()

    def should_check_for_updates(self) -> bool:
        """
        Determine if it's time to check for updates based on interval.

        Returns:
            True if check is needed, False otherwise
        """
        if not self.auto_check:
            return False

        if not self.last_check_time:
            return True

        try:
            last_check = datetime.fromisoformat(
                self.last_check_time.replace('Z', '+00:00')
            )
            now = datetime.utcnow()
            hours_since_check = (now - last_check.replace(tzinfo=None)).total_seconds() / 3600

            return hours_since_check >= self.check_interval_hours
        except (ValueError, AttributeError):
            return True

    def get_download_path(self, filename: str) -> Path:
        """
        Get path for a downloaded update file.

        Args:
            filename: Name of the file to download

        Returns:
            Path where file should be saved
        """
        return self.update_dir / filename

    def get_backup_path(self, version: str) -> Path:
        """
        Get path for a backup directory.

        Args:
            version: Version being backed up

        Returns:
            Path where backup should be stored
        """
        return self.backup_dir / f'backup-{version}'

    def get_staging_path(self, version: str) -> Path:
        """
        Get path for staging a new version.

        Args:
            version: Version being staged

        Returns:
            Path where staged files should be placed
        """
        return self.staging_dir / f'staging-{version}'

    def cleanup_old_backups(self):
        """Remove old backups beyond the retention limit"""
        if not self.backup_dir.exists():
            return

        # Get all backup directories sorted by modification time
        backups = sorted(
            [d for d in self.backup_dir.iterdir() if d.is_dir()],
            key=lambda x: x.stat().st_mtime,
            reverse=True
        )

        # Remove backups beyond retention limit
        for backup in backups[self.keep_backups:]:
            try:
                import shutil
                shutil.rmtree(backup)
            except Exception as e:
                pass

    def cleanup_downloads(self):
        """Remove downloaded update files"""
        if not self.update_dir.exists():
            return

        for file in self.update_dir.iterdir():
            if file.is_file():
                try:
                    file.unlink()
                except Exception:
                    pass

    def cleanup_staging(self):
        """Remove staging directories"""
        if not self.staging_dir.exists():
            return

        for staging in self.staging_dir.iterdir():
            if staging.is_dir():
                try:
                    import shutil
                    shutil.rmtree(staging)
                except Exception:
                    pass

    def __repr__(self) -> str:
        return f"UpdateConfig(version={self.current_version}, server={self.update_server_url})"
