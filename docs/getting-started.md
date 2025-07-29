# Getting Started with EMBODIOS

Welcome to EMBODIOS - Natural Operating System with Voice AI. This guide will help you get started with building and running your first AI-powered operating system.

## Prerequisites

- Python 3.8 or higher
- Docker (optional, for containerized builds)
- 4GB RAM minimum (8GB recommended)
- 10GB free disk space

## Installation

### Quick Install (Recommended)

```bash
curl -fsSL https://get.embodi.ai | bash
```

### Manual Installation

```bash
# Clone the repository
git clone https://github.com/embodi-os/embodi.git
cd embodi

# Install with pip
pip install .

# Or install in development mode
pip install -e .
```

### Verify Installation

```bash
embodi --version
```

## Your First AI-OS

### 1. Initialize a Project

```bash
mkdir my-ai-os
cd my-ai-os
embodi init
```

This creates a basic `Modelfile`:

```dockerfile
FROM scratch

MODEL huggingface:TinyLlama/TinyLlama-1.1B-Chat-v1.0
QUANTIZE 4bit

MEMORY 2G
CPU 2

HARDWARE gpio:enabled
CAPABILITY hardware_control
```

### 2. Build the Image

```bash
embodi build -f Modelfile -t my-first-os:latest
```

You'll see output like:
```
Building EMBODIOS image from Modelfile
Preparing model: TinyLlama/TinyLlama-1.1B-Chat-v1.0
Converting to EMBODIOS format...
Building kernel...
Creating bootable image...
✓ Build successful!
```

### 3. Run Your AI-OS

```bash
embodi run my-first-os:latest
```

You'll be greeted with:
```
EMBODIOS v0.1.0
Model: TinyLlama 1.1B
Ready for natural language input

> _
```

### 4. Interact with Natural Language

Try these commands:

```
> Hello EMBODIOS
AI: Hello! I'm EMBODIOS, your AI-powered operating system. How can I help you?

> Turn on GPIO pin 17
AI: Executing hardware control...
[HARDWARE] GPIO Pin 17 -> HIGH

> Show system status
AI: System Status Report
[SYSTEM] Memory: 1.2GB / 2.0GB
[SYSTEM] CPU: 2 cores active
[SYSTEM] Uptime: 45 seconds

> Calculate 42 * 3.14
AI: The result is 131.88

> Exit
AI: Shutting down EMBODIOS. Goodbye!
```

## Understanding Modelfiles

A Modelfile defines your AI-OS:

```dockerfile
# Base image (usually scratch for bare metal)
FROM scratch

# AI model selection
MODEL huggingface:microsoft/phi-2
QUANTIZE 4bit  # Reduce model size

# System resources
MEMORY 4G
CPU 4

# Hardware interfaces
HARDWARE gpio:enabled
HARDWARE uart:enabled
HARDWARE i2c:enabled

# OS capabilities
CAPABILITY hardware_control
CAPABILITY networking
CAPABILITY process_management

# Environment configuration
ENV EMBODIOS_PROMPT "MyOS> "
ENV EMBODIOS_DEBUG 1
```

## Common Use Cases

### Embedded IoT Device

```dockerfile
FROM scratch
MODEL huggingface:microsoft/phi-2
QUANTIZE 4bit
MEMORY 1G
HARDWARE gpio:enabled i2c:enabled spi:enabled
CAPABILITY sensor_reading data_logging
```

### Robot Controller

```dockerfile
FROM scratch
MODEL huggingface:mistralai/Mistral-7B-Instruct-v0.2
MEMORY 8G
HARDWARE gpio:enabled pwm:enabled can:enabled
CAPABILITY motion_control path_planning obstacle_avoidance
```

### Smart Home Hub

```dockerfile
FROM scratch
MODEL huggingface:TinyLlama/TinyLlama-1.1B-Chat-v1.0
MEMORY 2G
HARDWARE wifi:enabled zigbee:enabled
CAPABILITY home_automation voice_control
```

## Next Steps

- [Modelfile Reference](modelfile-reference.md) - Learn all Modelfile directives
- [Hardware Guide](hardware.md) - Configure hardware interfaces
- [Examples](https://github.com/embodi-os/embodi/tree/main/examples) - More example configurations
- [API Documentation](api.md) - Programmatic usage

## Troubleshooting

### Build Fails

```bash
# Clean build cache
rm -rf ~/.embodi/build-cache

# Build with debug output
embodi build -f Modelfile --debug

# Use specific platform
embodi build -f Modelfile --platform linux/arm64
```

### Model Download Issues

```bash
# Use local model
MODEL local:/path/to/model.gguf

# Specify different source
MODEL custom:https://myserver/model.embodi
```

### Performance Issues

- Reduce model size with `QUANTIZE 4bit`
- Allocate more memory: `MEMORY 4G`
- Use lighter models for embedded devices

## Getting Help

- [Documentation](https://docs.embodi.ai)
- [Discord Community](https://discord.gg/embodi)
- [GitHub Issues](https://github.com/embodi-os/embodi/issues)

Welcome to the future of operating systems!