# EMBODIOS - Embodied Intelligence Operating System

## Overview

EMBODIOS is an experimental operating system where AI models run directly on hardware as the OS kernel itself. Instead of traditional system calls and command-line interfaces, you control hardware through natural language commands. The entire OS is powered by language models that handle kernel operations, memory management, and hardware control.

**Key Innovation**: EMBODIOS includes a Python-to-Native compiler that transforms AI models and system code into bare-metal C/Assembly, enabling AI to run directly on hardware without a traditional OS layer.

```bash
> Turn on GPIO pin 17
AI: Executing hardware control...
[HARDWARE] GPIO Pin 17 -> HIGH

> Show system status
AI: System Status Report
[SYSTEM] Memory: 1.2GB allocated
[SYSTEM] Uptime: 2m 34s
[SYSTEM] Hardware: GPIO, UART, Timers active
```

## Quick Start

### Install EMBODIOS CLI

```bash
# Install from source
git clone https://github.com/dddimcha/embodiOS.git
cd embodiOS
pip install -e .
```

### Try It Out

```bash
# Create a Modelfile
cat > Modelfile << EOF
FROM scratch
MODEL huggingface:TinyLlama/TinyLlama-1.1B-Chat-v1.0
QUANTIZE 4bit
MEMORY 2G
HARDWARE gpio:enabled
EOF

# Build EMBODIOS image
embodi build -f Modelfile -t my-ai-os:latest

# Run it!
embodi run my-ai-os:latest

# Or run a model directly (no container)
embodi run test-model.aios --bare-metal

# Create bootable bundle for real hardware
embodi bundle create --model my-ai-os:latest --output embodios.iso --target bare-metal
```

## Key Features

- **Natural Language Control**: Type commands in plain English - "turn on pin 17" instead of `gpio.write(17, HIGH)`
- **AI-Powered Kernel**: Language models handle system operations, memory management, and hardware control
- **Python-to-Native Compiler**: Transforms Python AI code to C/Assembly for bare-metal execution
- **Bare Metal Performance**: Direct hardware access with <2ms response times
- **Minimal Footprint**: Entire OS in ~16MB RAM (vs 100MB+ for traditional deployments)
- **Hardware Abstraction**: Unified interface for GPIO, I2C, SPI, UART through natural language
- **Docker-like Workflow**: Build, run, and deploy AI-OS images with familiar commands
- **Real-time Processing**: 465+ commands/second throughput on commodity hardware

## Documentation

- [Getting Started](docs/getting-started.md)
- [Modelfile Reference](docs/modelfile-reference.md)
- [Hardware Compatibility](docs/hardware.md)
- [API Documentation](docs/api.md)
- [Performance Benchmarks](docs/performance-benchmarks.md)
- [Bare Metal Deployment](docs/bare-metal-deployment.md)
- [Contributing Guide](CONTRIBUTING.md)

## Use Cases

### Embedded Systems
```dockerfile
FROM scratch
MODEL huggingface:microsoft/phi-2
QUANTIZE 4bit
MEMORY 1G
HARDWARE gpio:enabled uart:enabled
```

### Robotics
```yaml
name: robot-embodi
model:
  source: huggingface
  name: microsoft/Phi-3-mini-4k-instruct
capabilities:
  - motion_control
  - sensor_fusion
  - path_planning
```

### Smart Home
```dockerfile
FROM scratch
MODEL huggingface:TinyLlama/TinyLlama-1.1B-Chat-v1.0
CAPABILITY home_automation voice_control
HARDWARE wifi:enabled zigbee:enabled
```

## How It Works

The basic flow is simple: you type something, the language model figures out what you want, and then it talks directly to the hardware. No complicated APIs or system calls in between. The model processes your text and converts it into hardware instructions through memory-mapped I/O.

```
User Input (Natural Language)
        ↓
┌─────────────────┐
│  NL Processor   │  ← Pattern matching &
│                 │     intent extraction
└────────┬────────┘
         ↓
┌─────────────────┐
│  AI Inference   │  ← Transformer Model
│     Engine      │     with hardware tokens
└────────┬────────┘
         ↓
┌─────────────────┐
│ Hardware Layer  │  ← Direct hardware
│      (HAL)      │     control via MMIO
└─────────────────┘
```

### Core Components

- **Natural Language Processor**: Translates commands like "turn on the LED" into hardware operations
- **Inference Engine**: Runs transformer models with special hardware control tokens
- **Hardware Abstraction Layer**: Provides unified interface to GPIO, I2C, SPI, UART
- **Runtime Kernel**: Manages system state, interrupts, and background services

## Why EMBODIOS?

Here's the thing - traditional operating systems have layers upon layers of abstractions. System calls, drivers, APIs, frameworks... it all adds up. EMBODIOS cuts through all that overhead.

**The benefits are pretty straightforward:**

- **Speed**: Direct hardware access means no kernel/userspace context switches. We're talking microseconds, not milliseconds.
- **Resource Efficiency**: No background services, no daemons, no unnecessary processes. Just your model and the hardware. Perfect when every MB counts.
- **Bare Metal Access**: Your commands go straight to the metal. No translation layers, no permission checks, no virtualization overhead.
- **IoT Ready**: Built specifically for embedded devices where traditional OSes are too heavy. Runs great on a Raspberry Pi or even smaller boards.
- **Cloud Cost Savings**: Why pay for cloud compute when your edge device can handle everything locally? No API calls, no bandwidth costs, no latency.

Think about it - a typical Linux distro needs hundreds of MB just for the base system. EMBODIOS? The whole OS *is* the model. That's it. Your 1GB model handles everything from memory management to GPIO control.

For IoT developers tired of stripping down Linux distributions, or anyone who wants their devices to actually understand what they're being asked to do - this might be worth a look.

## Python-to-Native Compiler

EMBODIOS includes a compiler that transforms Python AI code into native C/Assembly for bare-metal execution:

```bash
# Compile Python EMBODIOS to native code
cd src/embodi/compiler
python builder.py model.gguf output_dir/ native

# Generated files:
# - hal_gpio.c, hal_i2c.c, hal_uart.c - Hardware drivers
# - nl_processor.c - Natural language processing
# - kernel.c - Main kernel loop
# - boot.S - Boot assembly
# - weights.S - Model weights in assembly
# - Makefile - Build configuration
```

The compiler:
- Works without external dependencies (no NumPy, TVM, or Cython required)
- Generates bootable kernel images
- Embeds AI model weights directly in assembly
- Creates hardware abstraction layer (HAL) in pure C
- Produces ARM64 and x86-64 compatible code

## Contributing

If you want to help out or have ideas, that's awesome. Check the contributing guide for the details.

```bash
# Clone the repository
git clone https://github.com/dddimcha/embodiOS.git
cd core

# Install dependencies
make deps

# Run tests
make test

# Build EMBODIOS
make build
```

## Performance

Latest benchmark results show significant improvements over traditional deployments:

### Real-World Comparison (2025-07-30)

Using the same TinyLlama 1.1B model:

| Deployment | Response Time | Speed | Notes |
|------------|--------------|-------|--------|
| **EMBODIOS** | 361ms | 165 tokens/sec | Direct model execution |
| **Ollama** | 1,809ms | 133 tokens/sec | Service layer overhead |
| **Improvement** | **5x faster** | 24% higher | Same model, better deployment |

### Performance Metrics

- **Boot Time**: <1 second to fully operational state
- **Memory Usage**: 1.2GB total (vs 2GB+ for Ollama)
- **Response Time**: 361ms (Python prototype) → 20-50ms (projected bare-metal)
- **Throughput**: 165 tokens/second currently → 500+ tokens/sec on bare-metal
- **Direct Hardware Control**: <1ms GPIO/I2C operations

### Projected Bare-Metal Performance

- **Python (current)**: 361ms average response
- **C++ implementation**: ~50ms (7x faster)
- **True bare-metal**: 10-20ms (18-36x faster)
- **Custom silicon**: <5ms (72x faster)

See [full benchmark results](docs/performance-benchmarks.md) for detailed comparisons.

## Community

- [Discord Server](https://discord.gg/xRsYfcdP)

## License

EMBODIOS is open source software licensed under the [MIT License](LICENSE).

## Acknowledgments

This project pulls ideas from various places - Linux kernels, Docker's approach to containers, modern language models, and embedded systems. Just another unconventional approach to OS design that might interest someone out there.

---

**EMBODIOS** - An experimental OS where you can control hardware through everyday language
