#!/usr/bin/env python3
"""
EMBODIOS OTA Updater - Main orchestrator for Over-The-Air updates

Coordinates the complete update flow: check -> download -> verify -> backup -> apply -> rollback on failure
"""

import json
from pathlib import Path
from typing import Optional, Dict, Any, List, Tuple
from datetime import datetime

from embodi.updater.config import UpdateConfig
from embodi.updater.version import compare_versions
from embodi.updater.manifest import fetch_manifest, UpdateManifest
from embodi.updater.downloader import UpdateDownloader
from embodi.updater.verifier import verify_checksum
from embodi.updater.backup import BackupManager
from embodi.updater.applier import UpdateApplier
from embodi.updater.rollback import RollbackManager


class OTAUpdater:
    """
    Main orchestrator for EMBODIOS Over-The-Air updates.

    Coordinates the complete update flow:
    1. Check for available updates
    2. Download update artifacts
    3. Verify checksums
    4. Create backup of current installation
    5. Apply updates atomically
    6. Rollback on failure

    Features:
    - Atomic updates with automatic rollback
    - Secure downloads over HTTPS
    - SHA256 checksum verification
    - Progress tracking and status reporting
    - Update history tracking
    """

    def __init__(self, config: Optional[UpdateConfig] = None):
        """
        Initialize OTA updater with all necessary components.

        Args:
            config: Optional UpdateConfig instance.
                   Creates a new one if not provided.
        """
        # Initialize configuration
        self.config = config if config else UpdateConfig()

        # Initialize components
        self.downloader = UpdateDownloader(
            verify_tls=self.config.verify_tls,
            timeout=self.config.timeout_seconds
        )
        self.backup_manager = BackupManager(backup_dir=self.config.backup_dir)
        self.applier = UpdateApplier(config=self.config)
        self.rollback_manager = RollbackManager(
            config=self.config,
            backup_manager=self.backup_manager
        )

        # Current state
        self.current_manifest: Optional[UpdateManifest] = None
        self.downloaded_files: List[Dict[str, Any]] = []

    def check_for_updates(self, manifest_url: Optional[str] = None) -> Tuple[bool, Optional[UpdateManifest]]:
        """
        Check if updates are available.

        Args:
            manifest_url: Optional custom manifest URL.
                         Uses config default if not provided.

        Returns:
            Tuple of (updates_available, manifest)
            - updates_available: True if newer version is available
            - manifest: UpdateManifest object if fetch succeeded, None otherwise

        Examples:
            >>> updater = OTAUpdater()
            >>> available, manifest = updater.check_for_updates()
            >>> if available:
            ...     print(f"Update to {manifest.version} available")
        """
        print("Checking for updates...")

        try:
            # Fetch latest manifest
            url = manifest_url if manifest_url else self.config.manifest_url
            manifest = fetch_manifest(
                url=url,
                verify_tls=self.config.verify_tls,
                timeout=self.config.timeout_seconds
            )

            self.current_manifest = manifest

            # Update last check time
            self.config.last_check_time = datetime.utcnow().isoformat() + 'Z'
            self.config.save_state()

            # Compare versions
            current_version = self.config.current_version
            manifest_version = manifest.version

            result = compare_versions(current_version, manifest_version)

            if result < 0:
                # Newer version available
                print(f"Update available: {current_version} -> {manifest_version}")
                if manifest.critical:
                    print("⚠️  CRITICAL UPDATE - Installation recommended")
                return True, manifest
            elif result == 0:
                print(f"Already up to date: {current_version}")
                return False, manifest
            else:
                print(f"Current version {current_version} is newer than manifest {manifest_version}")
                return False, manifest

        except Exception as e:
            print(f"Error checking for updates: {e}")
            return False, None

    def download_updates(self, manifest: Optional[UpdateManifest] = None,
                        progress_callback=None) -> bool:
        """
        Download all update artifacts from manifest.

        Args:
            manifest: UpdateManifest to download from.
                     Uses self.current_manifest if not provided.
            progress_callback: Optional callback for progress tracking

        Returns:
            True if all downloads succeeded, False otherwise
        """
        if manifest is None:
            if self.current_manifest is None:
                print("Error: No manifest available. Run check_for_updates() first.")
                return False
            manifest = self.current_manifest

        print(f"Downloading {len(manifest.updates)} update(s)...")

        # Calculate total download size
        total_size = manifest.total_download_size()
        if total_size > 0:
            size_mb = total_size / 1024 / 1024
            print(f"Total download size: {size_mb:.1f} MB")

        # Clear any previous downloads
        self.downloaded_files = []

        try:
            # Download each update
            for i, update in enumerate(manifest.updates, 1):
                print(f"\n[{i}/{len(manifest.updates)}] Downloading {update.type}: {update.url}")

                # Determine output filename
                filename = f"{update.type}-{manifest.version}"
                if update.type == 'kernel':
                    filename += '.bin'
                elif update.type == 'model':
                    filename += '.aios'
                else:
                    # Extract extension from URL if available
                    url_path = update.url.split('?')[0]  # Remove query params
                    ext = Path(url_path).suffix
                    filename += ext if ext else '.bin'

                output_path = self.config.update_dir / filename

                # Download with progress tracking
                def download_progress(downloaded, total):
                    if total > 0:
                        percent = (downloaded / total) * 100
                        print(f"  Progress: {percent:.1f}% ({downloaded}/{total} bytes)", end='\r')
                    if progress_callback:
                        progress_callback(i, len(manifest.updates), downloaded, total)

                success = self.downloader.download(
                    url=update.url,
                    output_path=output_path,
                    expected_checksum=update.checksum,
                    progress_callback=download_progress
                )

                if not success:
                    print(f"\n❌ Download failed for {update.type}")
                    return False

                print(f"\n✓ Downloaded and verified {update.type}")

                # Track downloaded file
                self.downloaded_files.append({
                    'type': update.type,
                    'path': output_path,
                    'checksum': update.checksum,
                    'filename': filename
                })

            print(f"\n✓ All {len(manifest.updates)} update(s) downloaded successfully")
            return True

        except Exception as e:
            print(f"\n❌ Error downloading updates: {e}")
            return False

    def apply_updates(self, manifest: Optional[UpdateManifest] = None,
                     auto_rollback: bool = True) -> bool:
        """
        Apply downloaded updates atomically with automatic rollback on failure.

        This is the critical update phase that:
        1. Creates backup of current installation
        2. Stages updates for atomic application
        3. Verifies staged updates
        4. Applies updates atomically
        5. Rolls back on any failure (if auto_rollback=True)

        Args:
            manifest: UpdateManifest being applied.
                     Uses self.current_manifest if not provided.
            auto_rollback: Whether to automatically rollback on failure (default: True)

        Returns:
            True if update applied successfully, False otherwise
        """
        if manifest is None:
            if self.current_manifest is None:
                print("Error: No manifest available")
                return False
            manifest = self.current_manifest

        if not self.downloaded_files:
            print("Error: No downloaded files available")
            return False

        print(f"\nApplying update to version {manifest.version}...")

        current_version = self.config.current_version

        try:
            # Step 1: Create backup
            print("\n[1/4] Creating backup of current installation...")
            backup_path = self.backup_manager.create_backup(
                version=current_version,
                include_models=True
            )

            if not backup_path:
                print("❌ Failed to create backup")
                return False

            print(f"✓ Backup created: {backup_path.name}")

            # Step 2: Stage updates
            print("\n[2/4] Staging updates...")
            staging_path = self.applier.stage_update(
                update_files=self.downloaded_files,
                version=manifest.version
            )

            if not staging_path:
                print("❌ Failed to stage updates")
                if auto_rollback:
                    print("\nAttempting rollback...")
                    self.rollback_manager.rollback_to_latest(
                        reason="Staging failed",
                        current_version=manifest.version
                    )
                return False

            print(f"✓ Updates staged in: {staging_path}")

            # Step 3: Verify staged updates
            print("\n[3/4] Verifying staged updates...")
            if not self.applier.verify_staged_update(manifest.version):
                print("❌ Staged update verification failed")
                if auto_rollback:
                    print("\nAttempting rollback...")
                    self.rollback_manager.rollback_to_latest(
                        reason="Verification failed",
                        current_version=manifest.version
                    )
                return False

            print("✓ Staged updates verified")

            # Step 4: Apply updates atomically
            print("\n[4/4] Applying updates atomically...")
            if not self.applier.apply_staged_update(manifest.version):
                print("❌ Failed to apply updates")
                if auto_rollback:
                    print("\nAttempting rollback...")
                    self.rollback_manager.rollback_to_latest(
                        reason="Application failed",
                        current_version=manifest.version
                    )
                return False

            print("✓ Updates applied successfully")

            # Update configuration with new version
            self.config.current_version = manifest.version
            self.config.last_update_time = datetime.utcnow().isoformat() + 'Z'
            self.config.last_update_version = manifest.version

            # Add to update history
            self.config.update_history.append({
                'version': manifest.version,
                'timestamp': self.config.last_update_time,
                'from_version': current_version,
                'success': True
            })

            self.config.save_state()

            print(f"\n✅ Successfully updated from {current_version} to {manifest.version}")

            # Clean up old backups
            self.backup_manager.cleanup_old_backups(keep=self.config.keep_backups)

            return True

        except Exception as e:
            print(f"\n❌ Error applying updates: {e}")

            if auto_rollback:
                print("\nAttempting automatic rollback...")
                success = self.rollback_manager.automatic_rollback(
                    failed_version=manifest.version,
                    reason=f"Update failed: {str(e)}"
                )

                if success:
                    print("✓ Rollback successful")
                else:
                    print("❌ Rollback failed - manual intervention required")

            # Record failed update
            self.config.failed_updates.append({
                'version': manifest.version,
                'timestamp': datetime.utcnow().isoformat() + 'Z',
                'from_version': current_version,
                'error': str(e)
            })
            self.config.save_state()

            return False

    def update(self, manifest_url: Optional[str] = None,
               auto_apply: bool = False,
               progress_callback=None) -> bool:
        """
        Complete end-to-end update flow.

        Performs: check -> download -> apply (with backup and rollback)

        Args:
            manifest_url: Optional custom manifest URL
            auto_apply: If True, applies updates without confirmation.
                       If False, downloads but waits for manual apply.
            progress_callback: Optional callback for progress tracking

        Returns:
            True if update completed successfully, False otherwise

        Examples:
            >>> updater = OTAUpdater()
            >>> success = updater.update(auto_apply=True)
            >>> if success:
            ...     print("Update completed successfully")
        """
        # Step 1: Check for updates
        available, manifest = self.check_for_updates(manifest_url)

        if not available:
            return False

        if manifest is None:
            print("Error: Failed to fetch manifest")
            return False

        # Step 2: Download updates
        print("\n" + "="*60)
        if not self.download_updates(manifest, progress_callback):
            print("Error: Failed to download updates")
            return False

        # Step 3: Apply updates (if auto_apply or user confirms)
        print("\n" + "="*60)
        if not auto_apply:
            print("\nUpdates downloaded successfully.")
            print("Run apply_updates() to install, or this update will be skipped.")
            return True

        return self.apply_updates(manifest, auto_rollback=True)

    def get_status(self) -> Dict[str, Any]:
        """
        Get current update status and history.

        Returns:
            Dictionary containing:
            - current_version: Current installed version
            - last_check_time: When updates were last checked
            - last_update_time: When last update was applied
            - update_history: List of previous updates
            - failed_updates: List of failed update attempts
            - available_backups: List of available backup versions

        Examples:
            >>> updater = OTAUpdater()
            >>> status = updater.get_status()
            >>> print(f"Current version: {status['current_version']}")
        """
        # Get available backups
        backups = self.backup_manager.list_backups()
        backup_versions = [b['version'] for b in backups]

        status = {
            'current_version': self.config.current_version,
            'last_check_time': self.config.last_check_time,
            'last_update_time': self.config.last_update_time,
            'last_update_version': self.config.last_update_version,
            'update_count': len(self.config.update_history),
            'failed_update_count': len(self.config.failed_updates),
            'update_history': self.config.update_history[-5:],  # Last 5 updates
            'failed_updates': self.config.failed_updates[-5:],  # Last 5 failures
            'available_backups': backup_versions,
            'backup_count': len(backups),
            'config': {
                'update_server_url': self.config.update_server_url,
                'auto_check': self.config.auto_check,
                'auto_apply': self.config.auto_apply,
                'keep_backups': self.config.keep_backups
            }
        }

        return status

    def rollback(self, to_version: Optional[str] = None) -> bool:
        """
        Manually rollback to a previous version.

        Args:
            to_version: Optional specific version to rollback to.
                       If None, rolls back to the most recent backup.

        Returns:
            True if rollback succeeded, False otherwise

        Examples:
            >>> updater = OTAUpdater()
            >>> updater.rollback()  # Rollback to latest backup
            >>> updater.rollback(to_version='0.2.0')  # Rollback to specific version
        """
        current_version = self.config.current_version

        if to_version:
            print(f"Rolling back from {current_version} to {to_version}...")
            success = self.rollback_manager.rollback_to_version(
                version=to_version,
                reason="Manual rollback",
                current_version=current_version
            )
        else:
            print(f"Rolling back from {current_version} to previous version...")
            success = self.rollback_manager.rollback_to_latest(
                reason="Manual rollback",
                current_version=current_version
            )

        if success:
            # Update config
            backups = self.backup_manager.list_backups()
            if backups:
                latest_backup = backups[0]
                self.config.current_version = latest_backup['version']
                self.config.save_state()

            print("✓ Rollback completed successfully")
        else:
            print("❌ Rollback failed")

        return success
