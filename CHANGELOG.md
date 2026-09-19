# Changelog

All notable changes to EMBODIOS will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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