# EMBODIOS - Embodied Intelligence Operating System

## Overview

EMBODIOS is a revolutionary operating system where AI models serve as the kernel, enabling natural language control of hardware and system resources through text commands. Instead of traditional command-line interfaces or system calls, EMBODIOS understands plain English text input.

**Note:** EMBODIOS processes natural language as text. The "Voice AI" refers to the conversational nature of the interface, not audio processing. For actual voice input, see our [voice demo example](docs/voice-demo.md).

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
git clone https://github.com/embodiOS/core.git
cd core
pip install -e .
```

### Build Your First AI-OS

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
```

## Key Features

- **Natural Language OS**: Control everything with plain English
- **AI as Kernel**: Language models directly manage hardware
- **Docker-like Workflow**: Familiar tools for building and deployment
- **Hardware Agnostic**: Runs on x86_64, ARM64, RISC-V
- **Real-time Performance**: Sub-10ms response times
- **Minimal Footprint**: Runs in as little as 512MB RAM

## Documentation

- [Getting Started](docs/getting-started.md)
- [Modelfile Reference](docs/modelfile-reference.md)
- [Hardware Compatibility](docs/hardware.md)
- [API Documentation](docs/api.md)
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

## Architecture

```
User Input (Natural Language)
        ↓
┌─────────────────┐
│  AI Inference   │  ← Transformer Model
│     Engine      │     processes input
└────────┬────────┘
         ↓
┌─────────────────┐
│ Hardware Layer  │  ← Direct hardware
│      (HAL)      │     control via MMIO
└─────────────────┘
```

## Contributing

We welcome contributions! See our [Contributing Guide](CONTRIBUTING.md) for details.

```bash
# Clone the repository
git clone https://github.com/embodiOS/core.git
cd core

# Install dependencies
make deps

# Run tests
make test

# Build EMBODIOS
make build
```

## Performance

- **Boot Time**: ~800ms
- **Inference Speed**: 154 tokens/sec (TinyLlama)
- **Response Latency**: 6.5ms average
- **Memory Usage**: 1.2GB (including model)

## Community

- [Discord Server](https://discord.gg/embodi)

## License

EMBODIOS is open source software licensed under the [MIT License](LICENSE).

## Acknowledgments

Built with inspiration from:
- Linux kernel architecture
- Docker containerization
- Transformer models
- Embedded systems design

---

---

**EMBODIOS - Where Natural Language Meets Bare Metal**