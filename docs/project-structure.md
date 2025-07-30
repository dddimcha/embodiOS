# EMBODIOS Project Structure

## Overview

EMBODIOS is organized into several key components that work together to create an AI-powered operating system.

## Directory Structure

```
embodios/
├── src/embodi/             # Main source code
│   ├── compiler/          # Python-to-Native compiler
│   ├── core/             # Core OS components
│   ├── builder/          # Container and image builders
│   ├── cli/              # Command-line interface
│   ├── runtime/          # Runtime environment
│   ├── models/           # Model management
│   └── demos/            # Demo applications
├── docs/                  # Documentation
├── tests/                 # Test suite
├── models/               # Model storage
├── scripts/              # Utility scripts
└── embodi-installer/     # Installation tools
```

## Key Components

### 1. Compiler (`src/embodi/compiler/`)

The Python-to-Native compiler transforms EMBODIOS Python code into bare-metal C/Assembly:

- `embodios_transpiler.py` - Main transpiler for EMBODIOS-specific code
- `model_compiler.py` - Compiles AI models to native code
- `tvm_compiler.py` - TVM integration (with fallback)
- `builder.py` - Orchestrates the compilation process
- `native/` - Pre-written C components (boot.S, memory.c)
- `cython_modules/` - Performance-critical operations

### 2. Core OS (`src/embodi/core/`)

The heart of EMBODIOS:

- `embodi_os.py` - Main OS entry point
- `kernel.py` - Kernel operations
- `runtime_kernel.py` - Runtime management
- `hal.py` - Hardware Abstraction Layer
- `inference.py` - AI inference engine
- `nl_processor.py` - Natural language processing

### 3. CLI (`src/embodi/cli/`)

Docker-like interface for EMBODIOS:

- `main.py` - CLI entry point
- `commands.py` - Command implementations (build, run, bundle)

### 4. Builder (`src/embodi/builder/`)

Container and image management:

- `builder.py` - Image builder
- `modelfile.py` - Modelfile parser
- `converter.py` - Model format conversion

### 5. Runtime (`src/embodi/runtime/`)

Runtime environment management:

- `runtime.py` - Main runtime
- `container.py` - Container management
- `image.py` - Image handling

## Build Artifacts

When you compile EMBODIOS, the following files are generated:

```
output_dir/
├── hal_gpio.c         # GPIO driver
├── hal_i2c.c         # I2C driver
├── hal_uart.c        # UART driver
├── hal.h             # HAL header
├── tokens.h          # Hardware tokens
├── nl_processor.c    # NL processing
├── kernel.c          # Main kernel
├── boot.S            # Boot assembly
├── weights.S         # Model weights
├── Makefile          # Build config
├── CMakeLists.txt    # CMake config
└── link.ld           # Linker script
```

## Testing

Tests are organized by component:

```
tests/
├── test_builder.py    # Builder tests
├── test_cli.py       # CLI tests
├── test_core.py      # Core OS tests
├── test_runtime.py   # Runtime tests
├── integration/      # Integration tests
└── benchmarks/       # Performance tests
```

## Documentation

```
docs/
├── getting-started.md      # Quick start guide
├── architecture.md         # System architecture
├── modelfile-reference.md  # Modelfile syntax
├── hardware.md            # Hardware support
├── api.md                 # API reference
├── bare-metal-deployment.md # Deployment guide
└── performance-benchmarks.md # Performance data
```

## Installation Tools

```
embodi-installer/
├── bundle/bundler.py      # Bundle creator
├── iso/create_iso.py     # ISO builder
└── hardware/detect.py    # Hardware detection
```

## Models Directory

```
models/
└── README.md            # Model storage info
```

Models are stored here when downloaded or converted.

## Development Workflow

1. **Python Development**: Work in `src/embodi/core/`
2. **Compile to Native**: Use `src/embodi/compiler/`
3. **Test**: Run tests in `tests/`
4. **Build Image**: Use CLI `embodi build`
5. **Deploy**: Create ISO with `embodi bundle`

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

The compiler transforms Python implementations into native code at each layer, enabling bare-metal execution without Python runtime.