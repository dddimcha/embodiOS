"""
EMBODIOS - Natural Operating System with Voice AI
Setup configuration
"""

from setuptools import setup, find_packages

with open("README.md", "r", encoding="utf-8") as fh:
    long_description = fh.read()

with open("requirements.txt", "r", encoding="utf-8") as fh:
    requirements = [line.strip() for line in fh if line.strip() and not line.startswith("#")]

setup(
    name="embodi-os",
    version="0.1.0",
    author="EMBODIOS Contributors",
    author_email="contact@embodi.ai",
    description="AI-powered operating system with natural language interface",
    long_description=long_description,
    long_description_content_type="text/markdown",
    url="https://github.com/embodi-os/embodi",
    project_urls={
        "Bug Tracker": "https://github.com/embodi-os/embodi/issues",
        "Documentation": "https://docs.embodi.ai",
        "Source Code": "https://github.com/embodi-os/embodi",
    },
    classifiers=[
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Developers",
        "Topic :: System :: Operating System",
        "License :: OSI Approved :: MIT License",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
    ],
    package_dir={"": "src"},
    packages=find_packages(where="src"),
    python_requires=">=3.8",
    install_requires=requirements,
    entry_points={
        "console_scripts": [
            "embodi=embodi.cli.main:main",
        ],
    },
    include_package_data=True,
    package_data={
        "embodi": [
            "templates/*",
            "data/*",
        ],
    },
)