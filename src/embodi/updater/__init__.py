"""
EMBODIOS Updater - OTA update mechanism
"""

from .version import compare_versions, parse_version
from .ota import OTAUpdater

__all__ = ["compare_versions", "parse_version", "OTAUpdater"]
