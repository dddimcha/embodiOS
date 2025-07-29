# EMBODIOS Makefile
PROJECT = embodi
VERSION = 0.1.0

# Directories
SRC_DIR = src
BUILD_DIR = build
DIST_DIR = dist
DOC_DIR = docs

# Compiler settings
CC = gcc
CXX = g++
CFLAGS = -Wall -O2 -std=c11
CXXFLAGS = -Wall -O2 -std=c++17

# Python settings
PYTHON = python3
PIP = pip3

# Default target
.PHONY: all
all: build

# Build EMBODIOS
.PHONY: build
build: clean
	@echo "Building EMBODIOS $(VERSION)..."
	@mkdir -p $(BUILD_DIR)
	@$(PYTHON) -m build .
	@echo "Build complete!"

# Install dependencies
.PHONY: deps
deps:
	@echo "Installing dependencies..."
	@$(PIP) install -r requirements.txt
	@$(PIP) install -r requirements-dev.txt
	@echo "Dependencies installed!"

# Run tests
.PHONY: test
test:
	@echo "Running tests..."
	@$(PYTHON) -m pytest tests/ -v --cov=embodi
	@echo "Tests complete!"

# Lint code
.PHONY: lint
lint:
	@echo "Linting code..."
	@$(PYTHON) -m black src/ tests/
	@$(PYTHON) -m flake8 src/ tests/
	@$(PYTHON) -m mypy src/
	@echo "Linting complete!"

# Build documentation
.PHONY: docs
docs:
	@echo "Building documentation..."
	@cd $(DOC_DIR) && mkdocs build
	@echo "Documentation built!"

# Clean build artifacts
.PHONY: clean
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR) $(DIST_DIR) *.egg-info
	@find . -type d -name __pycache__ -exec rm -rf {} +
	@find . -type f -name "*.pyc" -delete
	@echo "Clean complete!"

# Install EMBODIOS locally
.PHONY: install
install: build
	@echo "Installing EMBODIOS..."
	@$(PIP) install -e .
	@echo "EMBODIOS installed!"

# Create distribution packages
.PHONY: dist
dist: clean
	@echo "Creating distribution packages..."
	@mkdir -p $(DIST_DIR)
	@$(PYTHON) -m build --sdist --wheel
	@echo "Distribution packages created!"

# Run EMBODIOS
.PHONY: run
run:
	@embodi --help

# Development server
.PHONY: dev
dev:
	@echo "Starting development environment..."
	@embodi run --dev examples/Modelfile.tinyllama

# Docker build
.PHONY: docker
docker:
	@echo "Building Docker images..."
	@docker build -f docker/Dockerfile -t embodi:$(VERSION) .
	@docker build -f docker/Dockerfile.dev -t embodi:dev .
	@echo "Docker images built!"

# Release
.PHONY: release
release: test lint dist
	@echo "Creating release $(VERSION)..."
	@git tag -a v$(VERSION) -m "Release version $(VERSION)"
	@echo "Release created! Run 'git push origin v$(VERSION)' to publish"

# Show help
.PHONY: help
help:
	@echo "EMBODIOS Makefile targets:"
	@echo "  make build    - Build EMBODIOS"
	@echo "  make deps     - Install dependencies"
	@echo "  make test     - Run tests"
	@echo "  make lint     - Lint code"
	@echo "  make docs     - Build documentation"
	@echo "  make clean    - Clean build artifacts"
	@echo "  make install  - Install EMBODIOS locally"
	@echo "  make dist     - Create distribution packages"
	@echo "  make docker   - Build Docker images"
	@echo "  make release  - Create a new release"
	@echo "  make help     - Show this help"

.DEFAULT_GOAL := help