# EMBODIOS - Embodied Intelligence Operating System

## Overview

So, I've been working on this interesting project - EMBODIOS. It's basically an operating system, but with a twist. Instead of typing cryptic commands or dealing with complex system calls, you just... talk to it. Well, type to it, actually. The whole thing runs on language models that handle the kernel operations. Pretty neat way to control hardware when you think about it.

Quick heads up: When I say "conversational", I mean text-based conversations. The system reads what you type, not what you say out loud. Though if you're curious about voice stuff, there's a demo in the docs that shows how you could add that.

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
```

## Key Features

- Control things by typing regular sentences
- Language models handle the low-level stuff
- Works like Docker if you're familiar with that
- Runs on pretty much any modern processor
- Responds quickly (usually under 10ms)
- Doesn't need much memory - 512MB is enough to get started

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

## How It Works

The basic flow is simple: you type something, the language model figures out what you want, and then it talks directly to the hardware. No complicated APIs or system calls in between. The model processes your text and converts it into hardware instructions through memory-mapped I/O.

## Contributing

If you want to help out or have ideas, that's awesome. Check the contributing guide for the details.

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

In my testing, it boots in less than a second, processes about 154 tokens per second with TinyLlama, and typically responds in around 6-7 milliseconds. The whole thing (including the model) uses about 1.2GB of memory.

## Community

- [Discord Server](https://discord.gg/xRsYfcdP)

## License

EMBODIOS is open source software licensed under the [MIT License](LICENSE).

## Acknowledgments

This project pulls ideas from various places - Linux kernels, Docker's approach to containers, modern language models, and embedded systems. Just another unconventional approach to OS design that might interest someone out there.

---

**EMBODIOS** - An experimental OS where you can control hardware through everyday language