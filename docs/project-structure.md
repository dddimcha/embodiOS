# EMBODIOS Project Structure

## Overview

EMBODIOS is organized into several key components that work together to create an AI-powered operating system.

## Directory Structure

```
embodios/
├── src/embodi/           # Main Python source code
│   ├── core/             # Core OS components
│   ├── builder/          # Container and image builders
│   ├── cli/              # Command-line interface
│   ├── runtime/          # Runtime environment
│   ├── models/           # Model management
│   └── installer/        # Bundle and ISO creation
├── kernel/               # Bare-metal kernel (C/Assembly)
├── docs/                 # Documentation
├── models/               # Model storage
├── scripts/              # Utility scripts
└── examples/             # Example Modelfiles
```

## Key Components

### 1. Core OS (`src/embodi/core/`)

The heart of EMBODIOS:

- `embodi_os.py` - Main OS entry point
- `kernel.py` - Kernel operations
- `runtime_kernel.py` - Runtime management
- `hal.py` - Hardware Abstraction Layer
- `inference.py` - AI inference engine
- `nl_processor.py` - Natural language processing

### 2. CLI (`src/embodi/cli/`)

Docker-like interface for EMBODIOS:

- `main.py` - CLI entry point
- `commands.py` - Command implementations (build, run, bundle)

### 3. Builder (`src/embodi/builder/`)

Container and image management:

- `builder.py` - Image builder
- `modelfile.py` - Modelfile parser
- `converter.py` - Model format conversion

### 4. Runtime (`src/embodi/runtime/`)

Runtime environment management:

- `runtime.py` - Main runtime
- `container.py` - Container management
- `image.py` - Image handling

### 5. Installer (`src/embodi/installer/`)

Deployment tools:

- `bundle/bundler.py` - Bundle creator for bare-metal deployment
- `iso/create_iso.py` - ISO builder
- `hardware/detect.py` - Hardware detection

### 6. Kernel (`kernel/`)

Bare-metal kernel in C/Assembly:

- `core/` - Kernel core (console, panic, interrupts)
- `mm/` - Memory management (PMM, VMM, heap)
- `ai/` - AI inference engine
- `arch/x86_64/` - x86_64 architecture support
- `include/` - Header files

## Documentation

```
docs/
├── getting-started.md      # Quick start guide
├── architecture.md         # System architecture
├── modelfile-reference.md  # Modelfile syntax
├── hardware.md             # Hardware support
├── api.md                  # API reference
├── bare-metal-deployment.md # Deployment guide
└── performance-benchmarks.md # Performance data
```

## Models Directory

```
models/
└── README.md            # Model storage info
```

Models are stored here when downloaded or converted.

## Development Workflow

1. **Python Development**: Work in `src/embodi/`
2. **Build Image**: Use CLI `embodi build`
3. **Test**: Use CLI `embodi run`
4. **Deploy**: Create ISO with `embodi bundle`

## Key Files

- `setup.py` - Package configuration
- `requirements.txt` - Python dependencies
- `CONTRIBUTING.md` - Contribution guidelines
- `CHANGELOG.md` - Version history
- `README.md` - Project overview

## Architecture Notes

EMBODIOS follows a layered architecture:

1. **Hardware Layer**: Direct hardware access via memory-mapped I/O
2. **HAL Layer**: Unified interface for different hardware
3. **Kernel Layer**: Core OS functionality
4. **AI Layer**: Language model inference
5. **Application Layer**: Natural language interface
