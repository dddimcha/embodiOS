# EMBODIOS - Bare-Metal AI Operating System

[![Status](https://img.shields.io/badge/Status-95%25%20Complete-brightgreen)](https://github.com/dddimcha/embodiOS/wiki/Current-State-Analysis)
[![AI Runtime](https://img.shields.io/badge/AI%20Runtime-100%25-brightgreen)](https://github.com/dddimcha/embodiOS/wiki/Pillar-1:-Ollama-GGUF-Integration)
[![License](https://img.shields.io/badge/License-MIT-blue)](LICENSE)

> **The world's first bare-metal AI operating system** - Run LLMs directly on hardware without any OS overhead. No Linux. No userspace. Just transformers and bare metal.

---

## 🚀 Quick Start

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

### Build & Run (3 Commands)

```bash
# 1. Clone the repo
git clone https://github.com/dddimcha/embodiOS.git
cd embodiOS

# 2. Build kernel (without model - for quick testing)
cd kernel && make

# 3. Run in QEMU
qemu-system-x86_64 -kernel embodios.elf -m 512M -nographic
```

You should see the EMBODIOS boot banner and shell prompt!

---

## 📀 Build Bootable ISO (with AI Model)

### Download a Model

```bash
# Create models directory
mkdir -p models

# Download SmolLM-135M (recommended for testing, 469MB)
curl -L -o models/smollm-135m.gguf \
  "https://huggingface.co/HuggingFaceTB/SmolLM-135M-Instruct-GGUF/resolve/main/smollm-135m-instruct-q6_k.gguf"

# Or TinyLlama 1.1B (better quality, 638MB)
curl -L -o models/tinyllama-1.1b.gguf \
  "https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
```

### Build the ISO

```bash
# Build ISO with embedded model
./scripts/create_iso.sh -m models/smollm-135m.gguf

# Output: build/embodios.iso (~500MB with SmolLM)
```

### Run in QEMU

```bash
# Note: Large kernels (>100MB) may not boot in QEMU due to SeaBIOS limitations
# For testing, use the kernel directly:
qemu-system-x86_64 -kernel kernel/embodios.elf -m 1024M -nographic

# Or write ISO to USB for real hardware (see below)
```

---

## 💾 Run on Real Hardware

### Write to USB

```bash
# Find your USB device (BE CAREFUL - this will erase the drive!)
# macOS:
diskutil list

# Linux:
lsblk

# Write the ISO (replace /dev/sdX with your USB device)
sudo dd if=build/embodios.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

### Boot from USB

1. Insert USB into target machine
2. Enter BIOS/UEFI boot menu (usually F12, F2, or Del)
3. Select USB device
4. EMBODIOS boots directly to AI shell

---

## 💬 Using the AI Shell

Once booted, you'll see the EMBODIOS shell:

```
╔══════════════════════════════════════════════════════════════╗
║   EMBODIOS - Bare Metal AI Operating System                  ║
║   Type 'help' for commands, 'talk' to chat with AI           ║
╚══════════════════════════════════════════════════════════════╝

embodios>
```

### Essential Commands

| Command | Description |
|---------|-------------|
| `talk` | **Start interactive AI chat** (recommended) |
| `chat <message>` | Send single message to AI |
| `status` | Show system and AI status |
| `help` | Show all commands |
| `help ai` | Show AI-specific commands |
| `benchmark` | Run inference benchmark |
| `mem` | Show memory usage |
| `perf` | Show last chat performance stats |

### Interactive Chat Example

```
embodios> talk

╔═══════════════════════════════════════════════╗
║  Interactive Chat Mode                        ║
║  Type 'exit' to leave, '/perf' for stats      ║
╚═══════════════════════════════════════════════╝

You> Hello! What can you do?
AI>  Hello! I'm an AI assistant running directly on bare metal hardware...

You> /perf
 [Session: 1 msgs, 42 tokens, 127.3 tok/s avg]

You> exit
 Session ended: 2 messages, 89 tokens, 12.4s total

embodios>
```

---

## 📊 Performance

| Metric | EMBODIOS | llama.cpp (Linux) | Advantage |
|--------|----------|-------------------|-----------|
| Memory Usage | 120 MB | 160 MB | **25% less** |
| Latency Jitter | ±0.5ms | ±5-10ms | **10x better** |
| Boot Time | <1 sec | N/A | **Instant** |
| First Token | <20ms | ~50ms | **2.5x faster** |

---

## 🔧 Supported Models

| Model | Size | Quantization | Status |
|-------|------|--------------|--------|
| SmolLM-135M | 469 MB | Q6_K | ✅ Verified |
| TinyLlama-1.1B | 638 MB | Q4_K_M | ✅ Verified |
| Phi-2-2.7B | 1.7 GB | Q4_K_M | ✅ Verified |
| Mistral-7B | 4.2 GB | Q4_K_M | ✅ Verified |

Any GGUF model from Ollama/HuggingFace should work.

---

## 📁 Project Structure

```
embodiOS/
├── kernel/                 # Kernel source code
│   ├── ai/                 # AI runtime (GGUF, tokenizer, inference)
│   ├── core/               # Kernel core (console, scheduler, memory)
│   ├── drivers/            # Hardware drivers (PCI, NVMe, network)
│   ├── mm/                 # Memory management (PMM, VMM, heap)
│   └── Makefile            # Build system
├── models/                 # Place GGUF models here
├── scripts/
│   ├── create_iso.sh       # Build bootable ISO
│   └── benchmark_vs_llamacpp.sh
├── build/                  # Build output (ISO, etc.)
└── docs/                   # Documentation
```

---

## 🛠️ Development

### Build Options

```bash
cd kernel

# Debug build (with symbols)
make DEBUG=1

# Clean build
make clean && make

# Build without model (faster iteration)
make

# Run tests
make test
```

### QEMU Development Workflow

```bash
# Quick iteration (kernel only, no ISO)
make && qemu-system-x86_64 -kernel embodios.elf -m 512M -nographic

# With serial logging
make && qemu-system-x86_64 -kernel embodios.elf -m 512M -serial stdio

# With GDB debugging
make DEBUG=1
qemu-system-x86_64 -kernel embodios.elf -m 512M -s -S &
gdb embodios.elf -ex "target remote :1234"
```

---

## 📖 Documentation

- **[Wiki Home](https://github.com/dddimcha/embodiOS/wiki)** - Full documentation
- **[Getting Started](https://github.com/dddimcha/embodiOS/wiki/Getting-Started)** - Detailed setup guide
- **[Console Commands](https://github.com/dddimcha/embodiOS/wiki/Console-Commands)** - Complete command reference
- **[Current State](https://github.com/dddimcha/embodiOS/wiki/Current-State-Analysis)** - Project status (95% complete)
- **[Architecture](https://github.com/dddimcha/embodiOS/wiki/Architecture-Overview)** - System design

---

## 🤝 Contributing

```bash
# Fork and clone
git clone https://github.com/YOUR_USERNAME/embodiOS.git
cd embodiOS

# Create feature branch
git checkout -b feature/my-feature

# Make changes, build, test
cd kernel && make && make test

# Submit PR
```

See [Contributing Guide](https://github.com/dddimcha/embodiOS/wiki/Contributing) for details.

---

## 🌟 Why Bare-Metal AI?

- **Zero OS overhead** - No syscalls, no context switches, no kernel/userspace boundary
- **Deterministic latency** - Critical for robotics, industrial control, real-time systems
- **Minimal footprint** - Runs on embedded devices with limited resources
- **Direct hardware access** - Zero-copy DMA, direct MMIO, no abstraction layers

---

## 📜 License

MIT License - see [LICENSE](LICENSE)

---

## 🔗 Links

- [Discord Community](https://discord.gg/xRsYfcdP)
- [GitHub Issues](https://github.com/dddimcha/embodiOS/issues)
- [Wiki Documentation](https://github.com/dddimcha/embodiOS/wiki)

---

**EMBODIOS** - Where transformers meet bare metal. 🤖⚡
