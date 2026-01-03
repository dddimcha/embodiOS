# EMBODIOS - Bare-Metal AI Operating System

[![Status](https://img.shields.io/badge/Status-65%25%20Complete-yellow)](https://github.com/dddimcha/embodiOS/wiki/Current-State-Analysis)
[![AI Runtime](https://img.shields.io/badge/AI%20Runtime-85%25-green)](https://github.com/dddimcha/embodiOS/wiki/Pillar-1:-Ollama-GGUF-Integration)
[![License](https://img.shields.io/badge/License-MIT-blue)](LICENSE)

> **The world's first bare-metal AI operating system** - where the AI model runs directly on hardware as the OS kernel itself. No userspace. No OS overhead. Just transformers and hardware.

## What's New (January 2026)

- **GGUF Parser:** Full support for TinyLlama-1.1B, Phi-2, Mistral-7B
- **BPE Tokenizer:** Proper tokenization loaded directly from GGUF vocabulary
- **Multi-Model Registry:** Load, switch, unload up to 8 models at runtime
- **Integer-Only Inference:** Transformer forward pass without FPU dependencies
- **All Quantization Types:** Q4_K, Q5_K, Q6_K, Q8_0 support

## Current Status

| Component | Status | Completion |
|-----------|--------|------------|
| **Kernel Foundation** | Memory management, boot process, I/O | 80% |
| **AI Runtime** | GGUF parser, BPE tokenizer, transformer, model registry | 85% |
| **Drivers** | PCI enumeration done (needs VirtIO, NVMe) | 20% |
| **Performance** | SIMD ops implemented, needs benchmarking | 40% |
| **Overall** | Foundation complete, AI runtime working | **65%** |

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

### Shell Commands

```bash
EMBODIOS> help           # Show available commands
EMBODIOS> models         # List loaded AI models
EMBODIOS> model load tinystories   # Load TinyStories model
EMBODIOS> ai Hello       # Generate text with AI
EMBODIOS> bpeinit        # Initialize BPE tokenizer from GGUF
EMBODIOS> mem            # Show memory stats
EMBODIOS> lspci          # List PCI devices
```

## Key Features

| Feature | Description |
|---------|-------------|
| **GGUF Model Support** | Load models from Ollama ecosystem directly |
| **BPE Tokenization** | SentencePiece-compatible tokenizer from GGUF |
| **Multi-Model** | Hot-swap between up to 8 loaded models |
| **Integer-Only Math** | No FPU required - runs on any x86_64 |
| **SIMD Acceleration** | SSE2/AVX2 for matrix operations |
| **Zero-Copy DMA** | Identity-mapped memory for direct hardware access |
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

The `docs/` folder is a **git submodule** pointing to the [EMBODIOS Wiki](https://github.com/dddimcha/embodiOS/wiki).

### Project Status
- [Home](docs/Home.md) - Wiki home page
- [Current State Analysis](docs/Current-State-Analysis.md) - Project progress (65% complete)
- [Three Strategic Pillars](docs/Three-Strategic-Pillars.md) - Implementation roadmap
- [Pillar 1: Ollama GGUF Integration](docs/Pillar-1:-Ollama-GGUF-Integration.md) - AI runtime (85% complete)

### Quick Start Guides
- [Getting Started](docs/Getting-Started.md) - Installation and first steps
- [Modelfile Reference](docs/Modelfile-Reference.md) - Model configuration
- [Hardware Requirements](docs/Hardware-Requirements.md) - Supported hardware
- [API Reference](docs/API-Reference.md) - API documentation

### Technical Deep Dives
- [Architecture Overview](docs/Architecture-Overview.md) - System architecture
- [Quantized Integer Inference](docs/Quantized-Integer-Inference.md) - How integer-only AI works
- [Performance Benchmarks](docs/Performance-Benchmarks.md) - Benchmark results
- [Bare Metal Deployment](docs/Bare-Metal-Deployment.md) - Real hardware deployment

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
- **Kernel hacker?** → [Pillar 2: Linux Driver Compatibility](docs/Pillar-2:-Linux-Driver-Compatibility.md)
- **AI researcher?** → [Pillar 1: Ollama GGUF Integration](docs/Pillar-1:-Ollama-GGUF-Integration.md)
- **Performance engineer?** → [Pillar 3: Performance Optimization](docs/Pillar-3:-Performance-Optimization.md)

## Community

- [Discord Server](https://discord.gg/xRsYfcdP)
- [GitHub Wiki](https://github.com/dddimcha/embodiOS/wiki)
- [Issues](https://github.com/dddimcha/embodiOS/issues)

## License

EMBODIOS is open source software licensed under the [MIT License](LICENSE).

---

**EMBODIOS** - Bare-metal AI where transformers meet hardware directly.
