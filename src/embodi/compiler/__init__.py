"""
EMBODIOS Native Compiler - Transforms Python AI to Native OS

This package provides a complete compilation pipeline to transform
Python-based AI code into optimized native C/Assembly code for
bare-metal execution.
"""

from .transpiler import PythonToNativeTranspiler
from .model_compiler import ModelToNativeCompiler

__all__ = ['PythonToNativeTranspiler', 'ModelToNativeCompiler']

__version__ = '0.1.0'