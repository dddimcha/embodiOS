# embodiOS

**One binary. Any machine. No OS.**

`embodios.elf` is a bare-metal x86_64 kernel with a quantized LLM compiled
straight into it — one file that *is* the operating system and the model.
Boot it as a kernel, an ISO, or a USB stick, and you land in a chat session
with a transformer running on raw hardware. No Linux. No libc. No userspace.
No dependencies.

![version](https://img.shields.io/badge/version-0.5.0-blue)
![build](https://img.shields.io/badge/build-passing-brightgreen)
![license](https://img.shields.io/badge/license-Apache--2.0-orange)
![platform](https://img.shields.io/badge/platform-x86__64-lightgrey)

---

## 60-second quickstart

```bash
git clone https://github.com/dddimcha/embodiOS.git
cd embodiOS
./embodi pull smollm   # fetch SmolLM-135M-Instruct Q4_K_M (~106 MB, sha256-verified)
./embodi build         # link the model into embodios.elf
./embodi run           # boot in QEMU — you're chatting with an LLM on bare metal
```

No toolchain yet? See [Prerequisites](docs/QUICKSTART.md#prerequisites) —
one package install on macOS, Debian/Ubuntu, or Arch.

## Run it your way

| Format | Command | Status |
|--------|---------|--------|
| QEMU direct kernel (PVH) | `qemu-system-x86_64 -kernel kernel/embodios.elf -nographic` | ✅ Verified |
| QEMU via CLI | `./embodi run` | ✅ Verified |
| Bootable ISO (BIOS, multiboot2) | `./embodi iso && ./embodi run --iso` | ✅ Verified |
| Bootable ISO (UEFI, OVMF) | `./embodi run --iso --uefi` | ✅ Verified |
| USB stick | `sudo dd if=dist/embodios.iso of=/dev/sdX bs=4M status=progress conv=fsync` | ✅ Hybrid image (El Torito + ESP), hardware 📖 |
| Real hardware | Boot the USB, pick it in the boot menu | 📖 Documented |

Prebuilt `elf` + `iso` + a QUICKSTART ship in `dist/` via `./embodi release`.

## What it looks like

```
  ███████╗███╗   ███╗██████╗  ██████╗ ██████╗ ██╗ ██████╗ ███████╗
  ██╔════╝████╗ ████║██╔══██╗██╔═══██╗██╔══██╗██║██╔═══██╗██╔════╝
  █████╗  ██╔████╔██║██████╔╝██║   ██║██║  ██║██║██║   ██║███████╗
  ██╔══╝  ██║╚██╔╝██║██╔══██╗██║   ██║██║  ██║██║██║   ██║╚════██║
  ███████╗██║ ╚═╝ ██║██████╔╝╚██████╔╝██████╔╝██║╚██████╔╝███████║
  ╚══════╝╚═╝     ╚═╝╚═════╝  ╚═════╝ ╚═════╝ ╚═╝ ╚═════╝ ╚══════╝
              One binary. Any machine. No OS.        v0.5.0 Tesla

  [ OK ] CPU features initialized
  [ OK ] Memory: 2048 MB detected, identity-mapped
  [ OK ] TCP/IP stack up
  [....] Loading SmolLM-135M-Instruct Q4_K_M (106 MB) [██████████] 100%
  [ OK ] Model ready — 30 layers, 48900 BPE merges, chatml template

embodios> chat What is the capital of France?
The capital of France is Paris.

embodios> help
  talk       interactive chat        demo       guided tour
  status     model & system status   benchmark  inference benchmark
  temp/topp  sampling controls       exo*       distributed inference
```

Greedy decoding matches the HuggingFace `transformers` reference output
for SmolLM-135M-Instruct **word for word** — the answer above is not staged.

## Features

### LLM inference without an OS

Full transformer inference in ring 0: GGUF parser, BPE tokenizer, RoPE, GQA,
KV cache — all freestanding C. Supported architectures and quantization
formats:

| Architecture | Models | Status |
|--------------|--------|--------|
| `llama` | SmolLM-135M, TinyLlama-1.1B, Qwen2.5 | ✅ Verified |
| `chatglm` | GLM-Edge-1.5B-Chat | ✅ Verified (synthetic + real GGUF forward pass) |
| `glm4` | GLM-4-9B/32B-0414 | ✅ Verified (synthetic, numpy-checked) |

| Quant | Bits/weight | Status |
|-------|-------------|--------|
| F32 / F16 | 32 / 16 | ✅ Verified |
| Q4_0 / Q4_1 / Q5_0 | 4.5–5.5 | ✅ Verified |
| Q8_0 | 8.5 | ✅ Verified (fused matmul) |
| Q2_K / Q3_K / Q4_K / Q5_K / Q6_K | 2.6–6.6 | ✅ Bit-exact port of ggml `ggml-quants.c` |
| IQ4_NL | 4.5 | ✅ Verified E2E |

K-quant dequantization is byte-exact against llama.cpp (max abs error 0.0
on real tensor data, `tools/verify_kquants.py`).

### Tokenizer & chat formats

Merge-order BPE (up to 280k merges / 128k vocab) with autodetected chat
templates: ChatML, llama2, llama3 (`<|start_header_id|>`), GLM. Override at
runtime with `chatformat`.

### Real-time tick & control loops (v0.5.0)

The system tick is a **LAPIC timer @1 kHz** calibrated against HPET (PIT
remains as automatic fallback). On top of it: `rt_timer` IRQ-context periodic
callbacks and the `motor` demo — a 1 kHz PID closed loop driving a simulated
DC motor, with rdtsc jitter accounting (mean/stddev/p99) and an asynchronous
LLM policy hook. See [docs/motor-demo.md](docs/motor-demo.md).

### GPU compute backend (v0.5.0)

Vulkan compute matmul path with hand-assembled SPIR-V shaders (f32, Q8_0),
validated bit-exact on lavapipe host-side; kernel probes PCI for a
Vulkan-capable device and falls back to SIMD when none is present. See
[docs/gpu-backend.md](docs/gpu-backend.md).

### Verified models

| Model | Params | Status |
|-------|--------|--------|
| SmolLM-135M-Instruct Q4_K_M (embedded default) | 135M | ✅ chat verified |
| **Llama-3.2-1B-Instruct Q4_K_M** | 1.24B | ✅ chat verified on bare metal (v0.5.0) |
| GLM-Edge-1.5B-Chat | 1.5B | ✅ forward pass verified |

### Sampling controls

Temperature (`temp 0..2`, 0 = greedy) and nucleus sampling (`topp 0..1`),
live from the shell.

### Distributed inference (exo-style) + OpenAI API

Nodes discover each other over UDP (`:5678`), form a ring, and shard models
layer-wise over a TCP transport. One node can serve an OpenAI-compatible
API straight from bare metal:

```
embodios> exo                    # start node, UDP discovery on :5678
embodios> exoshard even          # split layers across the ring
embodios> exoserve 8080          # OpenAI-compatible HTTP API
```

```bash
# QEMU: -netdev user,id=n0,hostfwd=tcp::18080-:8080 -device virtio-net-pci,netdev=n0
curl -X POST localhost:18080/v1/chat/completions \
  -d '{"model":"smollm","messages":[{"role":"user","content":"What is the capital of France?"}]}'
# → {"choices":[{"message":{"content":"The capital of France is Paris."}}],...}
```

Status: single-node ring fully working; a two-node ring agrees on 15/15
shard splits and starts token exchange. Known limitation: virtio-net RX on
the second guest under QEMU TCG — documented in `kernel/exo/README.md`.

### Beautiful serial UX

ASCII banner, colorized shell, a progress bar while the model loads, and a
`demo` command that walks you through the highlights. All over a plain
serial console — it has no business looking this good.

### Tests & CI

`make test` runs the in-kernel test suite inside QEMU — **6/6 PASS**.
GitHub Actions (`kernel-build` + `smoke-boot`) builds the kernel and boots
it on every push.

## Why embodiOS

- **Zero dependencies.** No Linux, no libc, no runtime. The ELF is the whole
  software stack — kernel, drivers, TCP/IP, tokenizer, model.
- **One file to rule it all.** `embodios.elf` (~106 MB with SmolLM inside)
  boots from QEMU, GRUB ISO, or a USB stick. Copy one file, boot anywhere.
- **Educational value.** A complete, readable path from bootloader to
  transformer logits — paging, memory management, GGUF parsing, BPE,
  attention — nothing hidden behind an OS.
- **A foundation for what's next.** GLM architectures and exo-style
  distributed inference are already landed; the porting playbook is in
  [docs/PORTING_GLM_EXO.md](docs/PORTING_GLM_EXO.md).

## Performance, honestly

Under QEMU **TCG** (pure software CPU emulation, no KVM) expect **~6–7
seconds per token**. That's the emulator, not the kernel: with `-enable-kvm`
or on real hardware the same binary runs orders of magnitude faster. Boot
to shell is under a second either way.

| Model | Size | How it runs | Status |
|-------|------|-------------|--------|
| SmolLM-135M-Instruct Q4_K_M | ~106 MB | Embedded in the ELF | ✅ Verified vs HF reference (word-exact) |
| TinyLlama-1.1B-Chat Q4_K_M | ~669 MB | virtio-blk disk, `loadmodel`, 3 GB RAM | ✅ Verified in QEMU |
| GLM-Edge-1.5B / GLM-4-9B (`chatglm`) | — | Embedded / `loadmodel` | ✅ Synthetic-verified + real GGUF forward pass |
| GLM-4-9B/32B-0414 (`glm4`) | — | Embedded / `loadext` | ✅ Synthetic-verified (needs multi-GB RAM for real GGUF) |
| Qwen2.5 0.5B/1.5B | — | Embedded | ⚠️ Runtime-supported, not yet QEMU-verified |

## Shell commands

| Command | What it does |
|---------|--------------|
| `talk` | Interactive chat session (`exit` to leave) |
| `chat <msg>` | Single-shot message to the model |
| `demo` | Guided tour of the kernel + model |
| `status` / `version` | Model & system status / kernel version |
| `benchmark` / `perf` | Full inference benchmark / last-run timings |
| `validate` | Validate the loaded model (5 checks) |
| `temp [0..2]` / `topp [0..1]` | Sampling temperature / nucleus threshold |
| `chatformat` | Chat template: `auto / off / chatml / llama2 / glm` |
| `loadmodel` | Load a GGUF model from a virtio-blk disk |
| `exo` / `exonodes` / `exoshard` | Distributed node: start, peers, layer shard |
| `exoserve [port]` | OpenAI-compatible API server |
| `mem` / `lspci` / `reboot` | Memory, PCI devices, reboot |
| `uptime` / `power` | Uptime & ticks / power & idle telemetry |
| `shutdown` / `poweroff` | Real ACPI S5 poweroff (PIIX4/ICH9) |
| `cpus` / `smpwork` / `smpbench` | SMP: online cores / AP work demo / parallel matmul bench |
| `fls` / `fsave` / `fload` / `frm` / `df` | embfs persistent files on virtio-blk |
| `color on\|off` | Toggle ANSI colors |
| `help` / `help all` / `help ai` | Command reference |

## Kernel capabilities (the old roadmap — now actually done)

| Capability | Status |
|------------|--------|
| Preemptive multitasking | ✅ LAPIC timer @1 kHz (HPET-calibrated, PIT fallback) drives a priority preemptive scheduler with real context switches (`tasktest` shows interleaved tasks); `-append poll` keeps the old polling mode |
| Real-time control | ✅ 1 kHz closed-loop PID on `rt_timer` IRQ callbacks with jitter accounting + LLM policy hook (`motor` demo) |
| GPU backend | ✅ Vulkan compute matmul (f32/Q8_0 SPIR-V), lavapipe bit-exact host validation, PCI probe + SIMD fallback |
| Interrupt handling (IDT/GDT) | ✅ IDT with full register-dump panics, remapped PIC 8259, LAPIC LVT setup, EOI-before-dispatch |
| Model runtime | ✅ GGUF + 12 quant formats + llama/chatglm/glm4 architectures |
| Command processor | ✅ 30+ command shell with boxed help |
| Network stack | ✅ TCP/IP + virtio-net/e1000e + exo distributed inference + OpenAI API |
| Persistent storage | ✅ virtio-blk write + `embfs` mini-FS (atomic double-buffered commits, CRC32) + persistent config across reboots |
| Multi-core (SMP) | ✅ Real AP boot via INIT-SIPI-SIPI trampoline; parallel inference workers run on real cores (`-smp 4` verified, `cpus` shows per-core work) |
| Power management | ✅ ACPI S5 shutdown, hardened reboot, `hlt` idle (~99% host CPU saved vs busy-poll) |

## Roadmap

- [ ] Direct UEFI boot without GRUB (hybrid BIOS+UEFI ISO already ships)
- [ ] Per-CPU LAPIC timers + IPI wakeups (BSP tick is LAPIC @1 kHz; APs currently poll a mailbox with IF=0)
- [ ] Q4_K/Q6_K SPIR-V shaders (f32 + Q8_0 ship today, host bit-exact)
- [ ] Venus/virtio-gpu command transport to activate the GPU backend under QEMU
- [ ] More verified models (Qwen2.5, larger GLM variants)
- [ ] Multi-node exo inference over a real network (two-node ring works
      under QEMU with a documented TCG caveat)

## Documentation

- [docs/QUICKSTART.md](docs/QUICKSTART.md) — get from zero to chat in minutes
- [docs/PORTING_GLM_EXO.md](docs/PORTING_GLM_EXO.md) — porting GLM & exo playbook
- [kernel/exo/README.md](kernel/exo/README.md) — distributed inference internals
- [models/README.md](models/README.md) — model downloads, manifest, checksums
- [CONTRIBUTING.md](CONTRIBUTING.md) — how to contribute

## Contributing

```bash
git clone https://github.com/YOUR_USERNAME/embodiOS.git
cd embodiOS && git checkout -b feature/my-feature
./embodi build && ./embodi test
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for the full workflow.

## License

Apache License 2.0 — see [LICENSE](LICENSE).

## Links

- [Discord](https://discord.gg/xRsYfcdP)
- [Issues](https://github.com/dddimcha/embodiOS/issues)
- [Wiki](https://github.com/dddimcha/embodiOS/wiki)
