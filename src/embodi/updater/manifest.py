"""
Update manifest parser and validator for EMBODIOS OTA updates
"""

import json
from typing import Dict, List, Optional, Any
from pathlib import Path
from datetime import datetime


class UpdateItem:
    """Represents a single update item in a manifest"""

    def __init__(self, data: Dict[str, Any]):
        """
        Initialize an update item from manifest data.

        Args:
            data: Dictionary containing update item fields

        Raises:
            ValueError: If required fields are missing or invalid
        """
        self.type = data.get('type')
        self.url = data.get('url')
        self.checksum = data.get('checksum')
        self.size = data.get('size')

        # Validate required fields
        if not self.type:
            raise ValueError("Update item missing required field: type")
        if not self.url:
            raise ValueError("Update item missing required field: url")
        if not self.checksum:
            raise ValueError("Update item missing required field: checksum")

        # Validate type
        valid_types = ['kernel', 'model', 'runtime', 'firmware']
        if self.type not in valid_types:
            raise ValueError(f"Invalid update type: {self.type}. Must be one of: {valid_types}")

        # Validate URL format
        if not self.url.startswith(('http://', 'https://')):
            raise ValueError(f"Invalid URL format: {self.url}")

        # Validate checksum format (SHA256 = 64 hex chars)
        if not isinstance(self.checksum, str) or len(self.checksum) != 64:
            raise ValueError(f"Invalid checksum format: {self.checksum}. Expected 64-character SHA256 hex string")

        # Validate size if provided
        if self.size is not None:
            if not isinstance(self.size, int) or self.size < 0:
                raise ValueError(f"Invalid size: {self.size}. Must be a positive integer")

    def __repr__(self) -> str:
        return f"UpdateItem(type={self.type}, url={self.url})"


class UpdateManifest:
    """
    Parser and validator for EMBODIOS update manifests.

    Manifest format:
    {
        "version": "0.3.0",
        "release_date": "2024-01-15T12:00:00Z",
        "updates": [
            {
                "type": "kernel",
                "url": "https://updates.embodi.ai/kernel-0.3.0.bin",
                "checksum": "abc123...",
                "size": 1048576
            }
        ]
    }
    """

    def __init__(self, data: Dict[str, Any]):
        """
        Initialize an update manifest from parsed JSON data.

        Args:
            data: Dictionary containing manifest fields

        Raises:
            ValueError: If manifest is invalid or missing required fields

        Examples:
            >>> manifest_data = {
            ...     'version': '0.3.0',
            ...     'updates': [{
            ...         'type': 'kernel',
            ...         'url': 'https://example.com/kernel',
            ...         'checksum': 'a' * 64
            ...     }]
            ... }
            >>> manifest = UpdateManifest(manifest_data)
            >>> manifest.version
            '0.3.0'
        """
        if not isinstance(data, dict):
            raise ValueError("Manifest data must be a dictionary")

        # Required fields
        self.version = data.get('version')
        if not self.version:
            raise ValueError("Manifest missing required field: version")

        # Validate version format
        if not isinstance(self.version, str):
            raise ValueError(f"Invalid version type: {type(self.version)}. Must be a string")

        # Parse version to ensure it's valid
        from .version import parse_version
        try:
            parse_version(self.version)
        except ValueError as e:
            raise ValueError(f"Invalid version format in manifest: {e}")

        # Parse updates list
        updates_data = data.get('updates', [])
        if not isinstance(updates_data, list):
            raise ValueError("Manifest 'updates' field must be a list")

        if not updates_data:
            raise ValueError("Manifest must contain at least one update")

        # Parse and validate each update item
        self.updates: List[UpdateItem] = []
        for i, item_data in enumerate(updates_data):
            try:
                update_item = UpdateItem(item_data)
                self.updates.append(update_item)
            except ValueError as e:
                raise ValueError(f"Invalid update item at index {i}: {e}")

        # Optional fields
        self.release_date = data.get('release_date')
        self.description = data.get('description')
        self.changelog = data.get('changelog')
        self.min_version = data.get('min_version')
        self.critical = data.get('critical', False)

        # Validate release_date if provided
        if self.release_date:
            try:
                datetime.fromisoformat(self.release_date.replace('Z', '+00:00'))
            except (ValueError, AttributeError):
                raise ValueError(f"Invalid release_date format: {self.release_date}")

    def get_updates_by_type(self, update_type: str) -> List[UpdateItem]:
        """
        Get all updates of a specific type.

        Args:
            update_type: Type of updates to retrieve (e.g., 'kernel', 'model')

        Returns:
            List of UpdateItem objects matching the type
        """
        return [update for update in self.updates if update.type == update_type]

    def total_download_size(self) -> int:
        """
        Calculate total download size for all updates.

        Returns:
            Total size in bytes, or 0 if sizes not provided
        """
        total = 0
        for update in self.updates:
            if update.size is not None:
                total += update.size
        return total

    def to_dict(self) -> Dict[str, Any]:
        """
        Convert manifest back to dictionary format.

        Returns:
            Dictionary representation of the manifest
        """
        result = {
            'version': self.version,
            'updates': [
                {
                    'type': update.type,
                    'url': update.url,
                    'checksum': update.checksum,
                    'size': update.size
                }
                for update in self.updates
            ]
        }

        # Include optional fields if present
        if self.release_date:
            result['release_date'] = self.release_date
        if self.description:
            result['description'] = self.description
        if self.changelog:
            result['changelog'] = self.changelog
        if self.min_version:
            result['min_version'] = self.min_version
        if self.critical:
            result['critical'] = self.critical

        return result

    def __repr__(self) -> str:
        return f"UpdateManifest(version={self.version}, updates={len(self.updates)})"


def load_manifest(path: Path) -> UpdateManifest:
    """
    Load and parse an update manifest from a file.

    Args:
        path: Path to the manifest JSON file

    Returns:
        Parsed UpdateManifest object

    Raises:
        FileNotFoundError: If manifest file doesn't exist
        ValueError: If manifest is invalid
        json.JSONDecodeError: If JSON is malformed
    """
    if not path.exists():
        raise FileNotFoundError(f"Manifest file not found: {path}")

    with open(path, 'r') as f:
        data = json.load(f)

    return UpdateManifest(data)


def save_manifest(manifest: UpdateManifest, path: Path) -> None:
    """
    Save an update manifest to a file.

    Args:
        manifest: UpdateManifest object to save
        path: Path where manifest should be saved
    """
    path.parent.mkdir(parents=True, exist_ok=True)

    with open(path, 'w') as f:
        json.dump(manifest.to_dict(), f, indent=2)
