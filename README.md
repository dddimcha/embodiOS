# EMBODIOS - Bare-Metal AI Operating System

Run LLMs directly on hardware without any OS overhead. No Linux. No userspace. Just transformers and bare metal.

## Quick Start

### Prerequisites

**macOS:**
```bash
brew install x86_64-elf-gcc x86_64-elf-binutils x86_64-elf-grub xorriso qemu
```

**Ubuntu/Debian:**
```bash
sudo apt install gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu grub-pc-bin xorriso qemu-system-x86
```

**Arch Linux:**
```bash
sudo pacman -S x86_64-elf-gcc x86_64-elf-binutils grub xorriso qemu
```

### Build and Run

```bash
git clone https://github.com/dddimcha/embodiOS.git
cd embodiOS

./embodi build
./embodi run
```

## CLI Reference

```
Usage: embodi <command> [options]

Commands:
  build             Build the kernel
  iso               Create bootable ISO
  run               Run in QEMU
  clean             Clean build artifacts
  test              Run kernel tests
  help              Show help
```

### Build Kernel

```bash
./embodi build              # Standard build
./embodi build --debug      # Debug build with symbols
```

### Create Bootable ISO

```bash
./embodi iso                              # Without model
./embodi iso --model models/smollm.gguf   # With embedded model
```

### Run in QEMU

```bash
./embodi run                    # Run kernel directly
./embodi run --memory 2G        # With more RAM
./embodi run --iso              # Boot from ISO
```

### Download Models

```bash
mkdir -p models

# SmolLM-135M (469MB, fast)
curl -L -o models/smollm-135m.gguf \
  "https://huggingface.co/HuggingFaceTB/SmolLM-135M-Instruct-GGUF/resolve/main/smollm-135m-instruct-q6_k.gguf"

# TinyLlama-1.1B (638MB, better quality)
curl -L -o models/tinyllama-1.1b.gguf \
  "https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
```

## Run on Real Hardware

### Write to USB

```bash
./embodi iso --model models/smollm-135m.gguf

# Write to USB (replace /dev/sdX with your device)
sudo dd if=build/embodios.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

### Boot

1. Insert USB into target machine
2. Enter BIOS boot menu (F12, F2, or Del)
3. Select USB device
4. EMBODIOS boots to AI shell

## Shell Commands

| Command | Description |
|---------|-------------|
| `talk` | Start interactive AI chat |
| `chat <msg>` | Single message to AI |
| `status` | System and AI status |
| `help` | Show all commands |
| `benchmark` | Run inference benchmark |
| `mem` | Memory usage |
| `perf` | Chat performance stats |

### Example Session

```
embodios> talk
You> Hello!
AI>  Hello! How can I help you today?
You> exit

embodios> status
  AI: Ready (SmolLM-135M)
  Memory: 120MB / 512MB
```

## Supported Models

| Model | Size | Quantization |
|-------|------|--------------|
| SmolLM-135M | 469 MB | Q6_K |
| TinyLlama-1.1B | 638 MB | Q4_K_M |
| Phi-2-2.7B | 1.7 GB | Q4_K_M |
| Mistral-7B | 4.2 GB | Q4_K_M |

Any GGUF model from Ollama/HuggingFace should work.

## Project Structure

```
embodiOS/
├── embodi              # CLI tool
├── kernel/             # Kernel source
│   ├── ai/             # AI runtime (GGUF, tokenizer, inference)
│   ├── core/           # Kernel core (console, scheduler)
│   ├── drivers/        # Hardware drivers (PCI, NVMe, network)
│   ├── mm/             # Memory management
│   └── Makefile
├── models/             # GGUF models (download separately)
├── scripts/            # Build scripts
└── build/              # Output (ISO, etc.)
```

## Performance

| Metric | EMBODIOS | llama.cpp |
|--------|----------|-----------|
| Memory | 120 MB | 160 MB |
| Latency jitter | ±0.5ms | ±5-10ms |
| Boot time | <1 sec | N/A |
| First token | <20ms | ~50ms |

## Documentation

- [Wiki](https://github.com/dddimcha/embodiOS/wiki)
- [Getting Started](https://github.com/dddimcha/embodiOS/wiki/Getting-Started)
- [Console Commands](https://github.com/dddimcha/embodiOS/wiki/Console-Commands)
- [Architecture](https://github.com/dddimcha/embodiOS/wiki/Architecture-Overview)
- [Current State](https://github.com/dddimcha/embodiOS/wiki/Current-State-Analysis)

## Contributing

```bash
git clone https://github.com/YOUR_USERNAME/embodiOS.git
cd embodiOS
git checkout -b feature/my-feature
./embodi build
./embodi test
# Submit PR
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for details.

## License

MIT License - see [LICENSE](LICENSE)

## Links

- [Discord](https://discord.gg/xRsYfcdP)
- [Issues](https://github.com/dddimcha/embodiOS/issues)
- [Wiki](https://github.com/dddimcha/embodiOS/wiki)
