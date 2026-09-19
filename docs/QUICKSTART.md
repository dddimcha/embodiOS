# Quickstart

From a clean checkout to chatting with an LLM on bare metal in a few minutes.

## Prerequisites

You need an x86_64 cross-compiler, GRUB tools (for the ISO), xorriso, and QEMU.

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

## Build and run

```bash
git clone https://github.com/dddimcha/embodiOS.git
cd embodiOS

./embodi pull smollm   # download SmolLM-135M-Instruct Q4_K_M (~106 MB, sha256-verified)
./embodi build         # build the kernel; the model is linked into embodios.elf
./embodi run           # boot in QEMU (direct kernel, PVH)
```

You land in the embodiOS shell. Try:

```
embodios> demo                              # guided tour
embodios> chat What is the capital of France?
The capital of France is Paris.
embodios> help                              # all commands
```

Exit QEMU with `Ctrl-A X`.

## Other ways to boot

**Bootable ISO (BIOS + UEFI hybrid):**

```bash
./embodi iso                 # → dist/embodios.iso (hybrid BIOS+UEFI)
./embodi run --iso           # boot the ISO in QEMU (BIOS)
./embodi run --iso --uefi    # boot the ISO under UEFI (OVMF)
```

**USB stick / real hardware:**

```bash
sudo dd if=dist/embodios.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

Boot the target machine from the USB stick (BIOS boot menu: F12/F2/Del).

**Release bundle** (`elf` + `iso` + QUICKSTART in `dist/`):

```bash
./embodi release
```

## Useful CLI commands

```
./embodi pull <smollm|tinyllama|all>   # download models (manifest-verified)
./embodi build [--debug]               # build the kernel
./embodi iso [--model <gguf>]          # create bootable ISO
./embodi run [--iso] [--uefi] [--memory 2G]   # run in QEMU
./embodi test                          # run the kernel test suite (6/6 PASS)
./embodi clean                         # clean build artifacts
```

## Notes

- **Speed:** under plain QEMU (TCG, no KVM) inference runs at ~6–7 s/token —
  that's software CPU emulation. Use `-enable-kvm` on Linux or real hardware
  for realistic speeds.
- **Bigger models:** TinyLlama-1.1B runs from a virtio-blk disk image
  (`./embodi pull tinyllama`, then `loadmodel` in the shell with ≥3 GB RAM).
- **Troubleshooting:** see the [Wiki](https://github.com/dddimcha/embodiOS/wiki)
  or open an [issue](https://github.com/dddimcha/embodiOS/issues).
