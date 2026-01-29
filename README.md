# EMBODIOS - Bare-Metal AI Operating System

[![Status](https://img.shields.io/badge/Status-75%25%20Complete-green)](https://github.com/dddimcha/embodiOS/wiki/Current-State-Analysis)
[![AI Runtime](https://img.shields.io/badge/AI%20Runtime-90%25-brightgreen)](https://github.com/dddimcha/embodiOS/wiki/Pillar-1:-Ollama-GGUF-Integration)
[![Drivers](https://img.shields.io/badge/Drivers-60%25-yellow)](https://github.com/dddimcha/embodiOS/wiki/Pillar-2:-Linux-Driver-Compatibility)
[![License](https://img.shields.io/badge/License-MIT-blue)](LICENSE)
[![Model Compatibility](https://img.shields.io/badge/Model%20Compatibility-CI-brightgreen)](https://github.com/dddimcha/embodiOS/actions/workflows/model-compatibility-ci.yml)

> **The world's first bare-metal AI operating system** - where the AI model runs directly on hardware as the OS kernel itself. No userspace. No OS overhead. Just transformers and hardware.

## What's New (January 2026)

- **Interactive Chat Mode:** `talk` command for dedicated conversation sessions with performance tracking
- **Performance Stats:** Separate `perf` command to view timing metrics without cluttering chat
- **Console UX:** Polished help system with categories, status display, command suggestions
- **Production ISO Builder:** One-click bootable ISO with GRUB menu
- **Stability Testing Suite:** Automated long-running tests (1h-72h) for memory leak detection
- **Secure Boot:** UEFI Secure Boot support with signed kernel validation
- **Streaming Inference:** Memory-efficient inference engine with parallel workers
- **Industrial Protocols:** Modbus TCP and EtherCAT support for real-time automation

## Current Status

| Component | Status | Completion |
|-----------|--------|------------|
| **Kernel Foundation** | Memory, boot, interrupts, DMA | 85% |
| **AI Runtime** | GGUF, BPE, streaming inference, quantization | 90% |
| **Drivers** | NVMe, VirtIO, e1000e, PCI, TCP/IP | 60% |
| **Performance** | SIMD, fixed-point, parallel inference | 50% |
| **Overall** | Core complete, drivers expanding | **75%** |

## Quick Start

### Clone with Documentation

```bash
# Clone with wiki submodule
git clone --recurse-submodules https://github.com/dddimcha/embodiOS.git
cd embodiOS

# Or initialize submodule after clone
git submodule update --init
```

### Build and Run

```bash
# Build the kernel (requires Linux or Docker)
cd kernel
make

# Run in QEMU
qemu-system-x86_64 -kernel embodios.elf -m 256M -serial stdio
```

### Chat with AI

```bash
> talk                   # Enter interactive chat mode (recommended!)
You> Hello, how are you?
AI>  Hello! I'm doing well. How can I help you today?
You> perf                # Check performance inline
 [Session: 1 msgs, 15 tokens, 127 tok/s avg]
You> exit                # Leave chat mode

> chat Hello world       # Single message (for scripting)
> perf                   # View detailed performance stats
> status                 # Check AI readiness
```

### System Commands

```bash
> help                   # Show available commands
> help ai                # AI-specific commands  
> help all               # All commands
> benchmark              # Full inference benchmark
> mem                    # Show memory stats
> lspci                  # List PCI devices
```

See the **[Console Commands Reference](https://github.com/dddimcha/embodiOS/wiki/Console-Commands)** for all commands.

## Key Features

| Feature | Description |
|---------|-------------|
| **GGUF Model Support** | Load models from Ollama ecosystem directly |
| **BPE Tokenization** | SentencePiece-compatible tokenizer from GGUF |
| **Multi-Model** | Hot-swap between up to 8 loaded models |
| **Integer-Only Math** | No FPU required - runs on any x86_64 |
| **SIMD Acceleration** | SSE2/AVX2 for matrix operations |
| **Zero-Copy DMA** | Identity-mapped memory for direct hardware access |
| **UEFI Secure Boot** | Signed kernel validation for trusted boot chain |
| **<1s Boot Time** | From power-on to AI inference ready |

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                    User Input                        │
└─────────────────────┬───────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────┐
│              BPE Tokenizer (from GGUF)              │
│         SentencePiece-compatible encoding           │
└─────────────────────┬───────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────┐
│            Transformer Engine (Integer)             │
│    Q4_K/Q5_K/Q6_K/Q8_0 quantized weights           │
│         SIMD-accelerated matrix ops                 │
└─────────────────────┬───────────────────────────────┘
                      ▼
┌─────────────────────────────────────────────────────┐
│              Hardware Abstraction                   │
│         PCI • DMA • Memory-Mapped I/O               │
└─────────────────────────────────────────────────────┘
```

## Documentation

Full documentation available on the [EMBODIOS Wiki](https://github.com/dddimcha/embodiOS/wiki).

### Project Status
- [Home](https://github.com/dddimcha/embodiOS/wiki) - Wiki home page
- [Current State Analysis](https://github.com/dddimcha/embodiOS/wiki/Current-State-Analysis) - Project progress (75% complete)
- [Three Strategic Pillars](https://github.com/dddimcha/embodiOS/wiki/Three-Strategic-Pillars) - Implementation roadmap
- [Pillar 1: Ollama GGUF Integration](https://github.com/dddimcha/embodiOS/wiki/Pillar-1:-Ollama-GGUF-Integration) - AI runtime (90% complete)

### Quick Start Guides
- [Getting Started](https://github.com/dddimcha/embodiOS/wiki/Getting-Started) - Installation and first steps
- [Console Commands](https://github.com/dddimcha/embodiOS/wiki/Console-Commands) - **Complete command reference**
- [Modelfile Reference](https://github.com/dddimcha/embodiOS/wiki/Modelfile-Reference) - Model configuration
- [Hardware Requirements](https://github.com/dddimcha/embodiOS/wiki/Hardware-Requirements) - Supported hardware
- [API Reference](https://github.com/dddimcha/embodiOS/wiki/API-Reference) - API documentation
- [Contributing](https://github.com/dddimcha/embodiOS/wiki/Contributing) - How to contribute

### Technical Deep Dives
- [Architecture Overview](https://github.com/dddimcha/embodiOS/wiki/Architecture-Overview) - System architecture
- [Quantized Integer Inference](https://github.com/dddimcha/embodiOS/wiki/Quantized-Integer-Inference) - How integer-only AI works
- [Performance Benchmarks](https://github.com/dddimcha/embodiOS/wiki/Performance-Benchmarks) - Benchmark results
- [Bare Metal Deployment](https://github.com/dddimcha/embodiOS/wiki/Bare-Metal-Deployment) - Real hardware deployment
- [Stability Testing](docs/stability_testing.md) - Long-running stability test suite

## Performance Targets

| Metric | llama.cpp | EMBODIOS v1.0 | Advantage |
|--------|-----------|---------------|-----------|
| **Speed** | 83-86 tok/s | 100-120 tok/s | **20-40% faster** |
| **Memory** | 160 MB | 120 MB | **25% less** |
| **Latency Jitter** | ±5-10ms | ±0.5ms | **10-20x better** |
| **Boot Time** | N/A | <1 sec | **Instant on** |
| **First Token** | ~50ms | <20ms | **2.5x faster** |
| **Context Switch** | ~1-5μs | 0 | **Zero overhead** |

## Verified Models

| Model | Size | Quantization | Status |
|-------|------|--------------|--------|
| TinyLlama-1.1B | 638 MB | Q4_K_M | Tested |
| Phi-2 | 1.7 GB | Q4_K_M | Tested |
| Mistral-7B | 4.2 GB | Q4_K_M | Tested |

## Why Bare-Metal AI?

**Kernel-space AI enables:**
- **Ultra-low latency:** 10x better consistency for real-time applications
- **Minimal footprint:** 25% less memory for edge/embedded devices
- **Direct hardware access:** No syscall overhead, zero-copy DMA
- **Deterministic timing:** Critical for robotics, industrial control

## Contributing

```bash
# Clone the repository
git clone --recurse-submodules https://github.com/dddimcha/embodiOS.git

# Build and test
cd kernel && make
make test
```

**Choose Your Pillar:**
- **Kernel hacker?** → [Pillar 2: Linux Driver Compatibility](https://github.com/dddimcha/embodiOS/wiki/Pillar-2:-Linux-Driver-Compatibility)
- **AI researcher?** → [Pillar 1: Ollama GGUF Integration](https://github.com/dddimcha/embodiOS/wiki/Pillar-1:-Ollama-GGUF-Integration)
- **Performance engineer?** → [Pillar 3: Performance Optimization](https://github.com/dddimcha/embodiOS/wiki/Pillar-3:-Performance-Optimization)

## Community

- [Discord Server](https://discord.gg/xRsYfcdP)
- [GitHub Wiki](https://github.com/dddimcha/embodiOS/wiki)
- [Issues](https://github.com/dddimcha/embodiOS/issues)

## License

EMBODIOS is open source software licensed under the [MIT License](LICENSE).

---

**EMBODIOS** - Bare-metal AI where transformers meet hardware directly.
