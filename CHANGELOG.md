# Changelog

All notable changes to EMBODIOS will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.5.0] - 2026-09-20 — codename Tesla

Product-level hardening: real-time tick, closed-loop control demo, GPU compute
backend, and 1B-class model support.

### Added
- **LAPIC timer @1 kHz** (HPET-calibrated, PIT fallback): `lapic_timer_probe()`
  + `lapic_timer_init(1000)` at `hal_timer_enable()` switches the system tick
  from PIT IRQ0 @100 Hz to a calibrated LAPIC LVT periodic interrupt on the same
  legacy vector (0x20); PIT is masked off. Boot log: `TICK: LAPIC timer @1000 Hz
  (calibrated vs HPET)` or `TICK: PIT @100 Hz (fallback)`. Calibration: 10 ms
  window against the HPET main counter (fallback: PIT channel 2 one-shot);
  implausible values fail over to the legacy PIT path. The legacy 100 Hz chain
  (scheduler quantum, uptime, hal_timer semantics) is decimated from the 1 kHz
  tick, so preemption behavior is unchanged. EOI is routed to the LAPIC when
  active. `-append poll` and SMP boot are unaffected.
- **rt_timer**: periodic IRQ-context callbacks (8 slots) driven by the tick;
  `fxsave/fxrstor` around every callback so float users (PID) cannot corrupt
  in-flight inference.
- **`motor` closed-loop control demo** (`cmd_motor.c`): float PID with
  anti-windup and 8-bit saturation, 2nd-order DC motor plant (dc=speed /
  servo=position), actuator byte to IO port 0xE9 (QEMU isa-debugcon) every
  tick, rdtsc jitter ring (8192 samples) with count/mean/stddev/min/max/p99
  report, and an asynchronous LLM policy hook that adjusts the setpoint from
  task context while the IRQ loop keeps firing. Commands: `motor run <ms>
  [hz]`, `motor jitter`, `motor llm on|off`, `motor plant dc|servo`,
  `motor pid <kp> <ki> <kd>`. Docs: `docs/motor-demo.md`.
- **Vulkan compute backend** (`ai/vk_device.c`, `ai/vk_shaders.h`,
  `tools/spirv_gen.py`, `tools/host_test_vulkan.c`): hand-assembled SPIR-V
  matmul shaders (f32, Q8_0 with in-shader dequant) — no shader compiler
  exists in-tree, so `spirv_gen.py` emits the words directly. Kernel-side
  Vulkan device layer with PCI probe; inference dispatches Q8_0 matmul to GPU
  only when `gpu_backend_probe() > 0`, otherwise silently falls back to the
  SIMD path (zero regression without GPU). Under TCG: `GPU: none (no
  Vulkan-capable PCI device)`. Host validation on lavapipe: 8/8 shape tests
  **bit-exact** vs CPU reference (max_abs_err = 0) for both shaders; the
  embedded words are the same ones compiled into the kernel. Docs:
  `docs/gpu-backend.md` (validated vs not-validated, Venus/passthrough
  activation paths, 4-byte SSBO tail-padding contract).
- **Llama-3 chat template** (`CHAT_FORMAT_LLAMA3`, `chatformat llama3`):
  auto-detected from `<|start_header_id|>`/`<|eot_id|>` vocab markers; stop on
  `<|eot_id|>`.
- **1B-class model support verified**: Llama-3.2-1B-Instruct Q4_K_M (GGUF v3,
  147 tensors, vocab 128256, GQA 32/8 heads, rope theta 500000, tied
  embeddings) boots and chats on bare metal — answer "The capital of France
  is Paris." verbatim with clean `<|eot_id|>` stop. QEMU TCG (2-core host,
  single vCPU): model load 80 s, prompt 312.6 s, gen 136.9 s (8 tokens).

### Notes
- SMP sizing: under TCG, more vCPUs than host cores hurts (same 1B model:
  prompt 759.4 s @ `-smp 4` vs 312.6 s @ `-smp 1` on a 2-core host). Size
  `-smp` to the host.
- TCG time base: rdtsc drifts ~2.3x vs LAPIC/HPET wall time under TCG, so
  motor-demo jitter absolutes (~1.3 ms mean lateness, p99 ≈ 1.5x mean, zero
  lost ticks) are emulation-bound; see `docs/motor-demo.md` for the honest
  analysis and the qualitative PREEMPT_RT comparison.

## [0.4.1] - 2026-09-19 — codename Figaro

### Added
- SIMD runtime dispatch for quantized inference kernels ("Figaro" release,
  branch `simd`): boot-time probe (CPUID + live xgetbv XCR0 verification in
  `arch/x86_64/cpu.c` — AVX2 is only advertised when the OS actually enabled
  XMM+YMM state) selects AVX2 vs SSE2/scalar/NEON per quantized vec_dot
  format; boot log prints "SIMD: AVX2 enabled" or "SIMD: scalar fallback".
  AVX2 kernels (Q8_0, Q4_K, Q5_0, Q6_K) live in a dedicated `-mavx2`
  translation unit (`ai/simd_kernels_avx2.c`) and are bit-identical to the
  scalar references (exact integer reductions, identical FP accumulation
  order) — greedy argmax cannot diverge between dispatch paths.
- Fused Q5_0 and Q6_K matmul paths in `streaming_inference.c` (both used
  dequantize+float-dot on v0.4.0). Q5_0 is the dominant format of the
  SmolLM-135M Q4_K_M model (58% of weight elements; Q8_0 22%, Q4_K 10.5%,
  Q6_K 9%).
- Host-side correctness + microbenchmark harness
  `tools/host_test_simd_kernels.c`: 1000 random trials per format —
  scalar-vs-AVX2 bit-identical, vs double-precision dequant reference
  max rel err ≤ 1.0e-4 (float32 accumulation noise only).

### Performance
- Kernel `benchmark` (20 tokens, QEMU TCG): 161.1 s → 60.5 s (2.66x) from
  the fused scalar Q5_0/Q6_K paths alone; chat output unchanged
  ("The capital of France is Paris."), `make test` 6/6 PASS.
- Host microbench (2M iters, 1536-wide row, scalar vs AVX2): Q8_0 1.5x,
  Q4_K 1.9x, Q5_0 4.6x, Q6_K 5.8x — expected additional speedup on real
  hardware/KVM where the AVX2 dispatch activates (TCG does not emulate AVX).

### Added
- exo live distributed inference (feat/exo-live): `exo_forward_shard` runs
  the node's real layer range — the passthrough stub is gone. Ring protocol
  PROMPT→TENSOR→RESULT works end-to-end: the orchestrator (ring head)
  embeds and forwards the hidden vector, the ring tail applies final norm +
  lm_head and samples (temperature/top-p honored); RESULT doubles as the
  lockstep barrier. Single node = full inference through the exo path.
- Layer-range API in `streaming_inference.c`: `streaming_inference_embed`,
  `streaming_inference_forward_layers` (per-shard local KV cache), 
  `streaming_inference_sample_token`, `streaming_inference_is_stop_token`;
  `streaming_inference_generate` reuses the new helpers (regression-tested:
  `chat` → "The capital of France is Paris.")
- OpenAI API (`exoserve`) returns real model output: chat template applied,
  `max_tokens` honored (default 64), BPE-decoded text, stop token stripped;
  fixed POST /v1/chat/completions routing (25-char prefix was compared
  with length 26 and never matched → 404)
- Static peers for broadcast-less transports: `exopeer` shell command +
  `exo_discovery_add_peer` (pinned peers never expire); `setip` command;
  `exoshard <model> <n> even` for deterministic splits. Two-node QEMU test
  topology: user-net hostfwd mesh (10.0.2.2:<hostport> reaches the peer)

### Added (previous waves)
- Full K-quants coverage: Q2_K, Q3_K and Q5_K dequantization ported
  byte-exactly from llama.cpp ggml-quants.c (pure C, no SIMD intrinsics),
  shared via `kernel/include/embodios/kquants_dequant.h` and wired into
  every type switch (streaming inference, gguf inference, integer loader)
- Integer-only Q16.16 fixed-point dequant for Q2_K/Q3_K/Q5_K in
  `quantized_ops.c` (+ Q5_K rewritten to the exact ggml layout); new
  `dequantize_q2_k/q3_k`, `matmul_q2_k/q3_k` and dispatcher entries
- IQ4_NL (type 20) dequantization — required by modern llama.cpp K-quant
  mixes (real SmolLM-135M Q3_K_S files use it for attention/FFN tensors)
- `tools/verify_kquants.py` + `tools/host_test_kquants.c`: host-side
  accuracy verification compiling the real kernel code against a numpy
  reference; float paths are bit-exact (max abs error 0.0 over 256 random
  blocks per format) and cross-validated against the official `gguf`
  Python package on real tensor data
- E2E: SmolLM-135M Q3_K_S (QuantFactory) boots and chats in QEMU with
  Q3_K + IQ4_NL tensors

## [0.2.0] - 2026-09-18

First release where the kernel builds cleanly, boots in QEMU, and runs a
real embedded GGUF LLM (SmolLM-135M-Instruct Q4_K_M) end-to-end with output
verified against the HuggingFace reference pipeline.

### Added
- Working LLM chat in QEMU with embedded SmolLM-135M-Instruct Q4_K_M
  (greedy output cross-checked with the HF reference)
- Chat template autodetection (ChatML / llama2 / GLM) with `chatformat`
  command and proper stop-token handling
- Temperature + top-p (nucleus) sampling in the streaming inference engine
  (xorshift64 PRNG seeded from rdtsc, nucleus capped at 128 candidates);
  new `temp [0..2]` and `topp [0..1]` shell commands, shown in `status`;
  `temp 0` (default) stays bit-compatible greedy argmax
- Optional QKV bias support (`blk.N.attn_q/k/v.bias`) in both inference
  engines — required for Qwen2/Qwen2.5 and GLM-4 GGUF models
- exo-style distributed inference skeleton (`kernel/exo/`: discovery, node,
  shard, transport, server) plus GLM-4/exo porting guide
  (`docs/PORTING_GLM_EXO.md`)
- `scripts/download-models.sh` + real `embodi pull <model>` CLI command
  with size + sha256 verification (hf-mirror.com primary, huggingface.co
  fallback); `models/manifest.json` now covers smollm (default) and
  tinyllama with verified hashes
- GLM architecture support (`chatglm` + `glm4` GGUF archs) in the
  streaming inference engine: fused `blk.N.attn_qkv.weight/.bias` carving
  (chatglm), fused `blk.N.ffn_up` SwiGLU seq-split (both), partial rotary
  via `rope.dimension_count` (interleaved NORM pairing, matching llama.cpp
  CHATGLM/GLM4 rope type), `glm4` post-attention/post-MLP RMSNorms inside
  residual branches, multi stop-token support (`<|user|>`, `<|assistant|>`
  besides EOS), GLM chat template fixed to `[gMASK]<sop><|user|>..`; new
  `dbglogits` shell command dumps top-k logits for verification.
  Verified in QEMU against a numpy reference forward on synthetic
  2-layer models (tools/gen_tiny_chatglm.py + tools/ref_chatglm.py):
  top-8 logits match for chatglm and glm4, F32 and Q8_0
- True merge-order BPE tokenizer driven by `tokenizer.ggml.merges`
  (~49k rules for SmolLM), replacing greedy longest-match when merges are
  present — tokenization now matches the HuggingFace tokenizers output
  (e.g. "assistant" -> [ass, istant], "Hello world" -> [Hello, Ġworld]);
  greedy longest-match remains as fallback for models without merges
- Restored `ai/benchmark.c` (benchmark/benchgguf/validate/timingtest
  commands work again; `validate` passes 5/5 on the embedded model)

### Fixed
- Build: separate flag-object rules (parallel `make -jN` race), restored
  missing benchmark.c, dropped broken GGML snapshot from the build, fixed
  the `_Bool`/`bool` typedef conflict, default GGUF model is now the
  actually-downloadable SmolLM Q4_K_M
- Boot: `create_iso.sh` now emits `multiboot2` (matching the kernel header),
  BSS is zeroed on boot, model init chain wired in `kernel_main`
- Tokenizer: special tokens (`<|im_start|>`, `[INST]`, ...) are split out
  of raw text before BPE matching; GPT-2 preprocessing maps `\n` to `Ċ`
  with no stray `Ġ` prefix after newlines
- Inference: GGUF geometry/alignment read from metadata (TinyLlama
  hardcode removed); parallel inference thread count clamped to online
  CPUs (deadlock on 1-vCPU QEMU); RoPE kept at the verified interleaved
  GPT-J pairing (a half-split variant was tried and reverted after failing
  the HF reference check)

### Known issues / TODO
- RAM is limited to 1 GB (boot page tables), so models >~0.5 GB cannot be
  embedded yet
- `benchgguf`/`benchmark` targets (85 tok/s) are not met under QEMU TCG
  (~0.1 tok/s is expected there)

## [0.1.0] - 2025-07-29

Initial release

### Added
- Core EMBODIOS kernel with AI-powered hardware control
- Hardware Abstraction Layer (HAL) supporting GPIO, I2C, SPI, UART
- Natural Language Processor for converting text commands to hardware operations
- AI Inference Engine for running language models on bare metal
- Docker-like CLI for building and running AI-OS images
- Support for multiple AI models (TinyLlama, Mistral, custom models)
- Modelfile format for defining AI-OS configurations
- Real-time hardware control with <2ms response times
- Memory-efficient operation (~16MB RAM for core OS)
- Comprehensive test suite with 34 tests
- Performance benchmarks showing 465+ commands/second throughput
- Example Modelfiles for different use cases
- Documentation for getting started, API reference, and hardware setup

### Features
- **Natural Language Control**: Control hardware using plain English commands
- **Bare Metal Support**: Direct hardware access without traditional OS overhead
- **Container-like Workflow**: Build, run, and deploy AI-OS images
- **Multi-Architecture**: Support for x86_64 and ARM platforms
- **Hardware Tokens**: Special tokens for efficient hardware control
- **Interrupt Handling**: Real-time response to hardware events
- **Memory Management**: Efficient memory-mapped model weights

### Supported Hardware
- Raspberry Pi 3/4/5
- x86_64 systems (with limited GPIO)
- Generic ARM boards
- I2C devices (sensors, displays)
- SPI devices
- UART communication

### Known Limitations
- Alpha release - API may change
- Voice input requires additional setup (text-based by default)
- Limited to models that fit in available RAM
- Some hardware features platform-dependent

[0.2.0]: https://github.com/dddimcha/embodiOS/releases/tag/v0.2.0
[0.1.0]: https://github.com/dddimcha/embodiOS/releases/tag/v0.1.0