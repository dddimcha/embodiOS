"""
NOVA - Natural Operating System with Voice AI
"""

__version__ = "0.1.0"
__author__ = "NOVA Contributors"
__license__ = "MIT"

from .core import NovaOS
from .builder import ModelfileParser, NovaBuilder
from .runtime import NovaRuntime

__all__ = ["NovaOS", "ModelfileParser", "NovaBuilder", "NovaRuntime"]