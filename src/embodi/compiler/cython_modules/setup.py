"""
Cython build configuration for EMBODIOS performance modules
"""

from setuptools import setup, Extension
from Cython.Build import cythonize
import numpy as np
import platform
from typing import List

# Compiler flags based on platform
extra_compile_args = ['-O3', '-ffast-math']
extra_link_args: List[str] = []

if platform.system() == 'Darwin':  # macOS
    extra_compile_args.extend(['-std=c++11', '-mmacosx-version-min=10.9'])
elif platform.system() == 'Linux':
    extra_compile_args.extend(['-std=c++11', '-march=native'])
    
# Check for CPU features
try:
    import cpuinfo
    info = cpuinfo.get_cpu_info()
    flags = info.get('flags', [])
    
    if 'avx2' in flags:
        extra_compile_args.append('-mavx2')
    if 'avx512f' in flags:
        extra_compile_args.append('-mavx512f')
except:
    pass

# Define extensions
extensions = [
    Extension(
        "inference_ops",
        ["inference_ops.pyx"],
        include_dirs=[np.get_include()],
        extra_compile_args=extra_compile_args,
        extra_link_args=extra_link_args,
        language="c",
    ),
    Extension(
        "matrix_ops",
        ["matrix_ops.pyx"],
        include_dirs=[np.get_include()],
        extra_compile_args=extra_compile_args,
        extra_link_args=extra_link_args,
        language="c",
    )
]

setup(
    name="embodios_cython",
    ext_modules=cythonize(
        extensions,
        compiler_directives={
            'language_level': 3,
            'boundscheck': False,
            'wraparound': False,
            'cdivision': True,
            'profile': False,
        }
    ),
    zip_safe=False,
    install_requires=[
        'numpy',
        'cython>=0.29',
    ],
)