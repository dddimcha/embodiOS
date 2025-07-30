# EMBODIOS Python-to-Native Compiler

This module transforms EMBODIOS Python code into native C/Assembly for bare-metal execution.

## Overview

The compiler enables EMBODIOS to run directly on hardware without a Python interpreter, achieving:
- Direct hardware control with <2ms latency
- Minimal memory footprint (~16MB total)
- Bootable kernel generation
- AI model embedding in assembly

## Components

### Core Files

- `embodios_transpiler.py` - Transpiles EMBODIOS-specific Python to C
- `model_compiler.py` - Compiles AI models (GGUF/SafeTensors) to assembly
- `tvm_compiler.py` - Optional TVM integration for optimized model compilation
- `builder.py` - Orchestrates the compilation process

### Native Components

- `native/boot.S` - Multiboot2-compliant bootloader
- `native/memory.c` - Custom memory allocator
- `native/embodios.h` - System headers

### Cython Modules (Optional)

- `cython_modules/inference_ops.pyx` - Optimized inference operations
- `cython_modules/matrix_ops.pyx` - Matrix operations

## Usage

### Basic Compilation

```bash
# Compile a model to native code
python builder.py model.gguf output_dir/ native

# This generates:
# - C source files (HAL, kernel, NL processor)
# - Assembly files (boot, weights)
# - Build files (Makefile, CMake, linker script)
```

### Programmatic Usage

```python
from embodi.compiler import HybridCompiler

# Create compiler
compiler = HybridCompiler("build/")

# Configure build
config = {
    'model_path': 'model.gguf',
    'target_arch': 'native',
    'optimization_level': 3
}

# Compile
result = compiler.compile_embodios(config)
```

### Generated Output

The compiler produces a complete bare-metal OS:

```
output_dir/
├── hal_gpio.c         # GPIO hardware control
├── hal_i2c.c         # I2C communication
├── hal_uart.c        # UART serial interface
├── hal.h             # Hardware abstraction header
├── tokens.h          # Hardware control tokens
├── nl_processor.c    # Natural language processor
├── kernel.c          # Main kernel loop
├── boot.S            # Boot assembly (ARM64/x86-64)
├── weights.S         # Model weights in assembly
├── Makefile          # GNU Make build file
├── CMakeLists.txt    # CMake configuration
└── link.ld           # Linker script
```

## Building the Kernel

After compilation:

```bash
cd output_dir/
make                    # Build kernel
make iso               # Create bootable ISO
make run               # Run in QEMU
```

## Architecture Support

The compiler generates code for:
- **x86-64**: Intel/AMD processors
- **ARM64**: Raspberry Pi 4, Apple Silicon
- **Generic C**: Fallback for other architectures

## Dependencies

The compiler works WITHOUT external dependencies:
- ✅ No NumPy required
- ✅ No TVM required (optional optimization)
- ✅ No Cython required (optional performance)
- ✅ Pure Python implementation

## How It Works

1. **Parse Python Code**: Extract EMBODIOS components
2. **Transpile to C**: Convert Python → C with pattern matching
3. **Compile Models**: Transform weights → assembly data
4. **Generate HAL**: Create hardware drivers
5. **Build System**: Generate Makefile/CMake
6. **Link**: Produce bootable kernel

## Architecture

```
Python Code → Hybrid Compiler → Native Binary
                ├── Model Compiler (GGUF/SafeTensors)
                ├── EMBODIOS Transpiler (HAL/Tokens)
                ├── Cython (Optional Hot Paths)
                └── Native C (Boot/Memory)
```

## Model Support

### Supported Formats
- **GGUF**: Native support with custom parser
- **SafeTensors**: Via custom parser
- **ONNX**: Via TVM (when available)

### Model Compilation
Models are compiled to assembly with:
- Weights embedded as binary data
- Metadata stored in structured format
- Direct memory mapping for inference

## Optimizations

- Hardware tokens for fast command dispatch
- Model weights embedded in binary
- Zero-copy memory operations
- Direct MMIO for hardware control
- Minimal runtime overhead

## Testing

```bash
# Run compiler tests
python -m pytest src/embodi/compiler/tests/

# Verify generated code
python test_compiler_verification.py
```

## Performance

The compiler achieves:
- **10-100x speedup** over Python for inference
- **<1 second boot time** to AI-ready state
- **~16MB kernel size** (excluding model)
- **Direct hardware access** with minimal overhead

## Extending

To add new functionality:

1. **New Model Format**: Add parser in `model_compiler.py`
2. **New Hardware**: Add HAL in `embodios_transpiler.py`
3. **New Optimization**: Add in `cython_modules/`
4. **New Architecture**: Update boot assembly and HAL

## Examples

### Compile TinyLlama Model

```python
from embodi.compiler.builder import build_embodios

# Compile model
success = build_embodios(
    model_path="tinyllama-1.1b.gguf",
    output_dir="build/tinyllama",
    target="native",
    verbose=True
)
```

### Custom Transpilation

```python
from embodi.compiler.embodios_transpiler import EMBODIOSTranspiler

transpiler = EMBODIOSTranspiler()

# Generate hardware tokens
tokens = {
    "<GPIO_WRITE>": 32001,
    "<GPIO_HIGH>": 32002,
}
token_code = transpiler.transpile_hardware_tokens(tokens)

# Generate HAL
hal_files = transpiler.transpile_hal_operations("")
```

## Limitations

- Models must be in GGUF or SafeTensors format
- Maximum model size limited by available RAM
- Some Python features not supported in transpilation
- Hardware support varies by platform

## Future Enhancements

- [ ] Support for more model formats
- [ ] Advanced optimization passes
- [ ] Real-time scheduling
- [ ] Multi-core support
- [ ] GPU acceleration

## License

Part of the EMBODIOS project. See main LICENSE file.