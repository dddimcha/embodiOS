#!/usr/bin/env python3
"""
Update Verifier for EMBODIOS OTA Updates
Validates checksums, formats, and integrity of model updates
"""

import hashlib
import struct
from pathlib import Path
from typing import Optional, Dict, Tuple


class UpdateVerifier:
    """Verifies integrity and validity of model updates"""

    SUPPORTED_FORMATS = {'.gguf', '.safetensors', '.bin'}
    GGUF_MAGIC = 0x46554747  # "GGUF" in little-endian
    MAX_MODEL_SIZE = 100 * 1024 * 1024 * 1024  # 100GB
    MIN_MODEL_SIZE = 1024  # 1KB

    def __init__(self):
        """Initialize update verifier"""
        pass

    def verify_checksum(self, file_path: Path, expected_checksum: str) -> bool:
        """
        Verify SHA256 checksum of a file

        Args:
            file_path: Path to file to verify
            expected_checksum: Expected SHA256 hash (hex string)

        Returns:
            True if checksum matches, False otherwise
        """
        if not file_path.exists():
            return False

        actual_checksum = self._calculate_hash(file_path)
        return actual_checksum.lower() == expected_checksum.lower()

    def verify_model_format(self, file_path: Path) -> Tuple[bool, str]:
        """
        Verify model file format is supported

        Args:
            file_path: Path to model file

        Returns:
            Tuple of (is_valid, format_name)
        """
        if not file_path.exists():
            return False, "file_not_found"

        suffix = file_path.suffix.lower()

        if suffix not in self.SUPPORTED_FORMATS:
            return False, f"unsupported_format_{suffix}"

        # For GGUF files, verify magic number
        if suffix == '.gguf':
            try:
                with open(file_path, 'rb') as f:
                    magic_bytes = f.read(4)
                    if len(magic_bytes) < 4:
                        return False, "gguf_truncated"

                    magic = struct.unpack('<I', magic_bytes)[0]
                    if magic != self.GGUF_MAGIC:
                        return False, "gguf_invalid_magic"

                return True, "gguf"

            except Exception as e:
                return False, f"gguf_read_error_{str(e)}"

        # For other formats, just check extension
        return True, suffix[1:]  # Remove leading dot

    def verify_size(self, file_path: Path) -> Tuple[bool, str]:
        """
        Verify model file size is within acceptable range

        Args:
            file_path: Path to model file

        Returns:
            Tuple of (is_valid, message)
        """
        if not file_path.exists():
            return False, "file_not_found"

        size = file_path.stat().st_size

        if size < self.MIN_MODEL_SIZE:
            return False, f"file_too_small_{size}_bytes"

        if size > self.MAX_MODEL_SIZE:
            size_gb = size / 1024 / 1024 / 1024
            return False, f"file_too_large_{size_gb:.1f}GB"

        return True, f"size_ok_{size}_bytes"

    def verify_update(self, file_path: Path, expected_checksum: Optional[str] = None) -> Dict[str, any]:
        """
        Perform comprehensive verification of model update

        Args:
            file_path: Path to model file to verify
            expected_checksum: Optional expected SHA256 hash

        Returns:
            Dictionary with verification results:
            {
                'valid': bool,
                'checksum_valid': bool,
                'format_valid': bool,
                'size_valid': bool,
                'format': str,
                'size': int,
                'checksum': str,
                'errors': list
            }
        """
        errors = []
        result = {
            'valid': False,
            'checksum_valid': False,
            'format_valid': False,
            'size_valid': False,
            'format': None,
            'size': 0,
            'checksum': None,
            'errors': errors
        }

        # Check file exists
        if not file_path.exists():
            errors.append("file_not_found")
            return result

        # Verify size
        size_valid, size_msg = self.verify_size(file_path)
        result['size_valid'] = size_valid
        result['size'] = file_path.stat().st_size
        if not size_valid:
            errors.append(size_msg)

        # Verify format
        format_valid, format_name = self.verify_model_format(file_path)
        result['format_valid'] = format_valid
        result['format'] = format_name
        if not format_valid:
            errors.append(format_name)

        # Calculate checksum
        checksum = self._calculate_hash(file_path)
        result['checksum'] = checksum

        # Verify checksum if expected value provided
        if expected_checksum:
            checksum_valid = self.verify_checksum(file_path, expected_checksum)
            result['checksum_valid'] = checksum_valid
            if not checksum_valid:
                errors.append("checksum_mismatch")
        else:
            # No expected checksum, mark as valid
            result['checksum_valid'] = True

        # Overall validity
        result['valid'] = (
            size_valid and
            format_valid and
            result['checksum_valid']
        )

        return result

    def _calculate_hash(self, file_path: Path, algorithm: str = 'sha256') -> str:
        """
        Calculate hash of a file

        Args:
            file_path: Path to file
            algorithm: Hash algorithm (default: sha256)

        Returns:
            Hex string of file hash
        """
        hash_obj = hashlib.new(algorithm)

        with open(file_path, 'rb') as f:
            # Read in chunks for memory efficiency
            for chunk in iter(lambda: f.read(8192), b''):
                hash_obj.update(chunk)

        return hash_obj.hexdigest()

    def get_file_metadata(self, file_path: Path) -> Dict[str, any]:
        """
        Get metadata about a model file

        Args:
            file_path: Path to model file

        Returns:
            Dictionary with file metadata
        """
        if not file_path.exists():
            return {
                'exists': False,
                'path': str(file_path)
            }

        stat = file_path.stat()

        return {
            'exists': True,
            'path': str(file_path),
            'name': file_path.name,
            'size': stat.st_size,
            'size_mb': stat.st_size / 1024 / 1024,
            'size_gb': stat.st_size / 1024 / 1024 / 1024,
            'modified': stat.st_mtime,
            'checksum': self._calculate_hash(file_path),
            'format': file_path.suffix.lower()
        }
