"""
Version comparison utilities for EMBODIOS updater
"""

from typing import Tuple


def parse_version(version: str) -> Tuple[int, ...]:
    """
    Parse a semantic version string into a tuple of integers.

    Args:
        version: Version string in format "X.Y.Z" or "X.Y.Z-suffix"

    Returns:
        Tuple of integers representing version components

    Examples:
        >>> parse_version("0.2.0")
        (0, 2, 0)
        >>> parse_version("1.0.0-beta")
        (1, 0, 0)
    """
    # Strip any suffix (e.g., "-beta", "-rc1")
    base_version = version.split("-")[0]

    # Parse numeric components
    try:
        parts = tuple(int(x) for x in base_version.split("."))
        return parts
    except (ValueError, AttributeError) as e:
        raise ValueError(f"Invalid version format: {version}") from e


def compare_versions(version1: str, version2: str) -> int:
    """
    Compare two semantic version strings.

    Args:
        version1: First version string
        version2: Second version string

    Returns:
        -1 if version1 < version2
        0 if version1 == version2
        1 if version1 > version2

    Examples:
        >>> compare_versions("0.2.0", "0.3.0")
        -1
        >>> compare_versions("0.3.0", "0.2.0")
        1
        >>> compare_versions("0.2.0", "0.2.0")
        0
    """
    v1_parts = parse_version(version1)
    v2_parts = parse_version(version2)

    # Compare component by component
    if v1_parts < v2_parts:
        return -1
    elif v1_parts > v2_parts:
        return 1
    else:
        return 0
