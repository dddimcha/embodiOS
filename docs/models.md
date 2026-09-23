# Model Verification Report — v0.6.0 "Volta" (WS-D)

## Qwen2.5-1.5B-Instruct Q4_K_M — verified on bare metal

**Model file**: `qwen2.5-1.5b-instruct-q4_k_m.gguf` (1,117,320,736 bytes, GGUF v3,
from ModelScope `Qwen/Qwen2.5-1.5B-Instruct-GGUF`).

### GGUF metadata facts (host-side parse)

| Key | Value |
|-----|-------|
| `general.architecture` | `qwen2` |
| `qwen2.block_count` | 28 |
| `qwen2.embedding_length` | 1536 |
| `qwen2.attention.head_count` | 12 |
| `qwen2.attention.head_count_kv` | 2 (GQA, kv_dim=256) |
| `qwen2.feed_forward_length` | 8960 |
| `qwen2.rope.freq_base` | 1000000.0 |
| `qwen2.attention.layer_norm_rms_epsilon` | 1e-6 |
| vocab | 151936 tokens, `gpt2` BPE, 151387 merges |
| `tokenizer.ggml.eos_token_id` | 151645 (`<\|im_end\|>`) |
| `tokenizer.chat_template` | present (ChatML) |
| tensors | 339 total; `blk.N.attn_{q,k,v}.bias` present (F32, 84 tensors) |
| `output.weight` | present (Q6_K) — no tied-embedding fallback needed |
| quantization | attn_q/k/out + ffn_gate/up Q4_K, attn_v + ffn_down Q6_K, norms F32 |

### What had to change (and what did not)

The qwen2 architecture was already fully wired in the tree: `gguf_parser.c`
accepts the `qwen2.` metadata prefix, `streaming_inference.c` maps the
standard `blk.N.attn_{q,k,v}.weight` tensors, dequantizes the optional
`.attn_{q,k,v}.bias` vectors per layer and adds them after the Q/K/V matmul
(`g_has_qkv_bias` path), falls back to `token_embd` when `output.weight` is
absent, and `chat_template.c` autodetects ChatML from `<|im_start|>` /
`<|im_end|>` vocab markers.

The actual blocker was boot-time: with a >1 GB model embedded in `.rodata`,
`.bss` (boot page tables, boot/kernel stacks) lands above the 1 GB mark, but
`boot.S` identity-mapped only the first 1 GB — the first stack access after
enabling paging double-faulted before any console output. `boot.S` now
identity-maps `BOOT_IDENTITY_GB = 4` (one PDT of 2 MB pages per GB).
`paging.c:identity_map_2mb_range()` skips already-present entries, so runtime
extension above 4 GB is unaffected, and APs share the kernel CR3 via the SMP
trampoline.

### Verification run (QEMU 7.2.22 TCG, `-m 3072 -smp 1 -nographic`)

```
qemu-system-x86_64 -kernel kernel/embodios.elf -m 3072 -smp 1 -nographic \
    -serial mon:stdio -no-reboot
embodios> chat What is the capital of France?
```

Serial log (ANSI stripped):

```
[OK] GGUF model embedded (1065 MB) — loads on first chat
[GGUF] Geometry from metadata: vocab=151936 embd=1536 layers=28 heads=12 kv=2 ff=8960 align=32
[STREAM] Config: dim=1536 hidden=8960 layers=28 heads=12 kv_heads=2
[STREAM] rope_theta=1000000 rms_eps=10(x1e-7) vocab=151936 seq_len=2048
[STREAM] arch=qwen2 rope_n_dims=0 fused_qkv=0 fused_ffn_up=0 post_norms=0
[STREAM] Q8 buffer: 4748 blocks for max 151936 elements
[STREAM] token_embd STANDARD [1536, 151936] (GGUF dims) type=12
[STREAM] output type=14
[STREAM] QKV biases found (Qwen2/GLM style)
[STREAM] Layer0 types: norm=0 q=12 k=12 v=14 out=12
[STREAM] Layer0 ffn: norm=0 gate=12 up=12 down=14
[CHAT] Detected chat format: chatml
✔ Model ready: qwen2.5-1.5b-instruct (Q4_K_M) · 28 layers · vocab 151936
   loaded in 153895 ms

you> What is the capital of France?
embodios> The capital of France is Paris
 (7 tokens · prompt 333.7s · gen 133.5s · 0.05 tok/s)
```

**Answer: "The capital of France is Paris" — correct, coherent, clean stop on
`<|im_end|>` (7 generated tokens).**

### Timings (QEMU TCG, single vCPU)

| Phase | Time |
|-------|------|
| Boot to shell | < 1 s |
| First-chat lazy load (parse + layer prep + tokenizer) | 153.9 s |
| Prompt processing (ChatML-wrapped prompt) | 333.7 s |
| Generation | 133.5 s for 7 tokens (≈19 s/token, 0.05 tok/s) |
| Total chat round-trip | ≈ 7.8 min |

TCG note: this is pure software CPU emulation of a 1.5B model; the same
binary under KVM or on real hardware runs orders of magnitude faster.
Generation is greedy (temperature 0).

### Regression after the loader/boot changes (SmolLM-135M-Instruct Q4_K_M)

- `make test`: **6/6 PASS**
- chat "What is the capital of France?": `The capital of France is Paris.`
  (8 tokens · prompt 22.1 s · gen 9.7 s · 0.82 tok/s)
- `-smp 4`: 4/4 CPUs online
- `-append poll`: boots (polling mode), interrupt mode boots as before

## GLM-Edge-1.5B-Chat (stretch) — verified on bare metal

GGUF located on ModelScope: `ZhipuAI/glm-edge-1.5b-chat-gguf`,
`ggml-model-Q4_K_M.gguf` (980,470,144 bytes). Host-side metadata parse:

- `general.architecture = chatglm` (runtime-supported arch branch)
- 28 layers, embd 2048, 16 heads / 4 KV heads, ff 6144, rope freq 10000,
  `rope.dimension_count = 128` (= head_dim → full rotary path)
- 227 tensors; **separate** `blk.N.attn_{q,k,v}.weight` (this conversion does
  not use the fused `attn_qkv` layout) and **no** QKV bias tensors
- vocab 59264, `gpt2` BPE (`pre = chatglm-bpe`), eos 59246
- GLM chat template autodetected via `[gMASK]` (59248) / `<sop>` (59250) markers

No code changes were needed: the existing chatglm branch maps the separate
Q/K/V tensors (fused `attn_qkv` fallback simply never triggers) and splits the
fused `ffn_up` gate/up rows.

Verification run (QEMU 7.2.22 TCG, `-m 3072 -smp 1 -nographic`):

```
[CHAT] Detected chat format: glm
✔ Model ready: Glm Edge 1.5b Chat (Q4_K_M) · 28 layers · vocab 59264
   loaded in 182692 ms

you> What is the capital of France?
embodios> The capital of France is Paris.
 (9 tokens · prompt 251.9s · gen 168.2s · 0.05 tok/s)
```

**Answer: "The capital of France is Paris." — correct, coherent, clean stop.**

| Phase | Time |
|-------|------|
| First-chat lazy load | 182.7 s |
| Prompt processing | 251.9 s |
| Generation | 168.2 s for 9 tokens (0.05 tok/s) |
