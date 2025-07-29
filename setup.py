"""
NOVA - Natural Operating System with Voice AI
Setup configuration
"""

from setuptools import setup, find_packages

with open("README.md", "r", encoding="utf-8") as fh:
    long_description = fh.read()

with open("requirements.txt", "r", encoding="utf-8") as fh:
    requirements = [line.strip() for line in fh if line.strip() and not line.startswith("#")]

setup(
    name="nova-os",
    version="0.1.0",
    author="NOVA Contributors",
    author_email="contact@nova.ai",
    description="AI-powered operating system with natural language interface",
    long_description=long_description,
    long_description_content_type="text/markdown",
    url="https://github.com/nova-os/nova",
    project_urls={
        "Bug Tracker": "https://github.com/nova-os/nova/issues",
        "Documentation": "https://docs.nova.ai",
        "Source Code": "https://github.com/nova-os/nova",
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
            "nova=nova.cli.main:main",
        ],
    },
    include_package_data=True,
    package_data={
        "nova": [
            "templates/*",
            "data/*",
        ],
    },
    scripts=[
        "nova-installer/iso/create_iso.py",
        "nova-installer/bundle/bundler.py",
    ],
)