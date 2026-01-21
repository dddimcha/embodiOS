#!/usr/bin/env python3
"""
EMBODIOS Update Verifier - Checksum verification for downloaded artifacts
"""

import hashlib
from pathlib import Path
from typing import Union


def calculate_checksum(file_path: Union[str, Path], algorithm: str = 'sha256') -> str:
    """
    Calculate checksum of a file.

    Args:
        file_path: Path to the file
        algorithm: Hash algorithm to use (default: sha256)

    Returns:
        Hexadecimal checksum string

    Raises:
        FileNotFoundError: If file does not exist
        ValueError: If algorithm is not supported
    """
    file_path = Path(file_path)

    if not file_path.exists():
        raise FileNotFoundError(f"File not found: {file_path}")

    # Get hash algorithm
    try:
        hasher = hashlib.new(algorithm)
    except ValueError:
        raise ValueError(f"Unsupported algorithm: {algorithm}")

    # Read file in chunks and update hash
    with open(file_path, 'rb') as f:
        for chunk in iter(lambda: f.read(8192), b''):
            hasher.update(chunk)

    return hasher.hexdigest()


def verify_checksum(file_path: Union[str, Path], expected_checksum: str,
                   algorithm: str = 'sha256') -> bool:
    """
    Verify file checksum against expected value.

    Args:
        file_path: Path to the file to verify
        expected_checksum: Expected checksum value (hexadecimal string)
        algorithm: Hash algorithm to use (default: sha256)

    Returns:
        True if checksum matches, False otherwise
    """
    try:
        actual_checksum = calculate_checksum(file_path, algorithm)
        return actual_checksum.lower() == expected_checksum.lower()
    except (FileNotFoundError, ValueError):
        return False


def verify_file_integrity(file_path: Union[str, Path], checksums: dict) -> tuple:
    """
    Verify file integrity using multiple checksums.

    Args:
        file_path: Path to the file to verify
        checksums: Dictionary of algorithm: expected_checksum pairs
                  e.g., {'sha256': '...', 'md5': '...'}

    Returns:
        Tuple of (success: bool, results: dict)
        results contains algorithm: match status for each checksum
    """
    file_path = Path(file_path)

    if not file_path.exists():
        return False, {'error': 'File not found'}

    results = {}
    all_passed = True

    for algorithm, expected in checksums.items():
        try:
            actual = calculate_checksum(file_path, algorithm)
            matches = actual.lower() == expected.lower()
            results[algorithm] = {
                'expected': expected,
                'actual': actual,
                'match': matches
            }
            if not matches:
                all_passed = False
        except ValueError:
            results[algorithm] = {
                'error': f'Unsupported algorithm: {algorithm}'
            }
            all_passed = False

    return all_passed, results
