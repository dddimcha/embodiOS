# Porting GLM (GGUF) and exo to embodiOS

A complete guide: the current state of the embodiOS LLM runtime, a step-by-step
plan for supporting GLM-4 models (GGUF architectures `chatglm` and `glm4`), and a
plan for exo-style distributed inference over a ring of nodes.

Related material:
- `kernel/exo/` — a compilable skeleton of the distributed-inference module
  (this branch, `feature/exo-skeleton`);
- `kernel/exo/README.md` — how to include the module in the build, and the
  integration TODOs;
- the GLM/exo research is folded into §3-4 of this document (sources:
  llama.cpp `src/models/chatglm.cpp`, `src/models/glm4.cpp`, exo
  `exo/orchestration/node.py`, `node_service.proto`, etc.).

---

## 1. Current state of the embodiOS runtime

### 1.1 Main inference chain

```
kernel_loop (core/kernel.c:371)
  → process_command (core/stubs.c:108)          # chat/talk/stream commands
  → gguf_load_model (ai/gguf_loader.c:294)
  → gguf_parser_load (ai/gguf_parser.c:1434)    # GGUF v3, metadata+tensors
  → bpe_tokenizer_init (ai/bpe_tokenizer.c)     # BPE from tokenizer.ggml.tokens
  → streaming_inference_init/generate (ai/streaming_inference.c:2037/2291)
```

### 1.2 What is supported (ai/streaming_inference.c, ~2600 lines)

- **Format**: GGUF v3 (v1/GGML is rejected); tensors are mapped as pointers into
  the model image, weights stay quantized, dequantized on the fly.
- **Quants** (`stream_dequant`): F32, F16, Q4_0, Q4_1, Q5_0, Q8_0,
  Q2_K, Q3_K, Q4_K, Q5_K, Q6_K, IQ4_NL (Q3_K/Q5_K/IQ4_NL were added in
  feat/kquants, ggml-exact ports). Not supported: Q5_1, Q8_1 (used only as the
  activation format), the rest of the IQ family.
- **Matmul**: `matmul_stream` (:1246) — fused Q8_0×Q8_1 and Q4_K×Q8_1 paths
  (SSE2/AVX2/NEON), otherwise dequant+dot. Weights use the GGUF layout
  `[in_features, out_features]` (row = output channel).
- **Architecture**: llama-style — separate `blk.N.attn_q/k/v/output`,
  `ffn_gate/up/down`, RMSNorm pre-norm (`rmsnorm_stream`, :1155), RoPE over all
  head dimensions (`rope`, :1816, **interleaved/GPT-J pairs (i, i+1)**),
  GQA (`kv_mul`), tied embeddings, KV cache for all layers.
- **Sampling**: greedy argmax by default; temperature and top-p are set via
  `streaming_inference_set_temperature/top_p` (commands `temp`/`topp`).
- **Chat template**: present (`ai/chat_template.c`) — ChatML/[INST]/GLM
  auto-detection, `chatformat` command.
- **Attention bias**: absent (`attn_q/k/v.bias` are not read).

### 1.3 Key APIs and structures (extension points)

| Component | File:line | Signature / contents |
|---|---|---|
| GGUF parser | ai/gguf_parser.c | `int gguf_parser_load(const void *data, size_t size)` |
| Metadata | include/embodios/gguf_parser.h | `const struct gguf_model_arch *gguf_parser_get_arch(void)` — fields `general_architecture[64]`, `block_count`, `embedding_length`, `feed_forward_length`, `attention_head_count(_kv)`, `attention_layer_norm_rms_epsilon`, `rope_dimension_count`, `rope_freq_base`, `eos_token_id`, `tokenizer_model[64]`, etc. |
| Tensor lookup | gguf_parser.h | `const struct gguf_tensor_info *gguf_parser_get_tensor_by_name(const char *name)`, `const void *gguf_parser_get_tensor_data_ptr(const struct gguf_tensor_info *info)` |
| Layer mapping | streaming_inference.c:2242–2263 | macro `MAP_LAYER_TENSOR(field, suffix)` → `LayerWeights` (:270–289) |
| Layer | streaming_inference.c:1859 | `static void transformer_forward_stream(int token, int pos, int layer)` — one layer: pre-norm → attn → residual → FFN(SwiGLU) → residual |
| RoPE | streaming_inference.c:1816 | `static void rope(float *q, float *k, int pos, int dim, int head_dim, int kv_dim, float theta)` |
| RMSNorm | streaming_inference.c:1155 | `static void rmsnorm_stream(float *out, const float *x, const void *w_quant, int type, int size, float eps)` |
| Matmul | streaming_inference.c:1246 | `static void matmul_stream(float *out, const void *w_quant, int w_type, const float *x, int rows, int cols)` |
| Tensor name | streaming_inference.c:171 | `static void build_layer_name(char *buf, size_t size, const char *prefix, int layer, const char *suffix)` |
| Tokenizer | ai/bpe_tokenizer.c | `int bpe_tokenizer_encode(const char *text, int *tokens, int max_tokens, bool add_bos, bool add_eos)`, `int bpe_tokenizer_decode(...)`, greedy longest-match (not a full BPE merge) |
| Engine config | streaming_inference.c:190 | `StreamingConfig g_cfg` (dim/hidden_dim/n_layers/n_heads/n_kv_heads/rope_theta/rms_norm_eps/eos/bos/arch_name) |

### 1.4 Model → support status

| Model (GGUF arch) | Status | Notes |
|---|---|---|
| TinyLlama-1.1B (`llama`) | ✓ works | No bias, full RoPE, llama-BPE vocab — the main test path |
| SmolLM-135M/360M (`llama`) | ✓ works | Sandbox test model (Q4_K_M) |
| LLaMA-2/3 (`llama`) | ✓ (RoPE theta/scaling read from metadata) | LLaMA-3: needs a full BPE merge (longest-match is currently an approximation) |
| Qwen2 / Qwen2.5 (`qwen2`) | ✗ → **after step A3** | QKV bias is mandatory (Qwen2 has it) — logits are currently wrong |
| ChatGLM2/3-6B (`chatglm`) | ✓ graph supported (A1–A7 implemented) | + ChatGLM3's SentencePiece tokenizer (not implemented) |
| GLM-4-9B-Chat, GLM-Edge-1.5B/4B (`chatglm`) | ✓ supported (synthetic: logits = numpy reference, F32+Q8_0) | Fused attn_qkv+bias, fused ffn_up (SwiGLU seq), partial rotary (interleaved); the zai-org glm-edge GGUF ships separate q/k/v without bias — the fallback covers it |
| GLM-4-9B/32B-0414 (`glm4`) | ✓ supported (synthetic: logits = numpy reference, F32+Q8_0) | + post-norms; 32B has no bias — the loader tolerates both variants |
| GLM-4.5/Air (`glm4moe`) | out of scope | MoE — a separate large project |
| Mistral/Mixtral (`mistral`) | to verify | Sliding window attention not supported; without SWA it is close to llama |
| Phi-2/3 (`phi`) | to verify | Phi-2: LayerNorm (not RMS) + bias — needs both extensions; Phi-3: llama-like |

---

## 2. Phase A: GLM port (local GGUF inference)

GLM exists in GGUF under three architectures: `chatglm` (ChatGLM2/3-6B,
GLM-4-9B-Chat, GLM-Edge), `glm4` (GLM-4-0414 9B/32B — the ring target),
`glm4moe` (GLM-4.5, out of scope). For 32B-in-a-ring, `glm4` is the relevant one.

### 2.1 Differences from llama-style (summary)

| Feature | `chatglm` (GLM-4-9B-Chat) | `glm4` (GLM-4-0414) |
|---|---|---|
| QKV | **fused** `blk.N.attn_qkv.weight` `[n_embd, n_embd+2·n_embd_gqa]` **+ `.bias`** | separate `attn_q/k/v.weight`, bias per config (9B: yes, 32B: no) |
| FFN | **fused** `blk.N.ffn_up` `[n_embd, 2·n_ff]`, SwiGLU seq-split (first half — gate, second — up); no `ffn_gate` | same |
| RoPE | **partial**: `rope.dimension_count = head_dim·0.5`, **interleaved `(i, i+1)`** pairs (llama.cpp `LLAMA_ROPE_TYPE_NORM` for CHATGLM and GLM4-text; HF GLM4 does the same via `repeat_interleave(2)` — NOT NeoX, a correction to the early research), theta = 10000·rope_ratio | same |
| Norms | pre-RMSNorm only | **+ `attn_post_norm` and `ffn_post_norm`** (RMSNorm inside the residual branch after attention and after the FFN) |
| Tokenizer | GPT-2 BPE, pre-tokenizer `chatglm-bpe` | GPT-2 BPE, pre-tokenizer `glm4` (same regex) |
| eos/eot | `<|endoftext|>`=151329 (eos), `<|user|>`=151336 (eot), `<|assistant|>`=151338 | same + bos=`<|endoftext|>`, add_bos=false |
| Chat format | `[gMASK]<sop><|system|>\n…<|user|>\n…<|assistant|>\n` | same marker family |

### 2.2 Port steps

#### A1 — Metadata (S) — ✅ done (branch feat/glm-arch)

`gguf_parser.c` already reads `<arch>.context_length`, `embedding_length`,
`block_count`, `feed_forward_length`, `attention.head_count(_kv)`,
`attention.layer_norm_rms_eps`, `rope.dimension_count`, `rope.freq_base` —
the keys are built from the `general.architecture` string, so `chatglm`/`glm4`
are picked up automatically by the same keys (`glm4.context_length`, etc.).

To do:
- Make sure `general.architecture` = `chatglm`/`glm4` is not rejected on
  input (check the arch validation branch in `gguf_parser_load`,
  ai/gguf_parser.c:1434+, and in `streaming_inference_init`,
  ai/streaming_inference.c:2037+ — arch_name is stored in
  `g_cfg.arch_name[64]`, but nothing branches on it).
- Add to `StreamingConfig` (streaming_inference.c:190):
  `int rope_n_dims` (from `arch->rope_dimension_count`, 0 = full rotary),
  `bool has_qkv_bias`, `bool fused_qkv`, `bool fused_ffn_up`,
  `bool has_post_norms` — set the flags from `g_cfg.arch_name`.

Touched: `gguf_parser.c` (possibly nothing), `streaming_inference.c`
(`streaming_inference_init`).

#### A2 — Fused attn_qkv and fused ffn_up, arch `chatglm` (S–M) — ✅ done (option 1, virtual mapping; helper `tensor_row_byte_offset`)

`chatglm` tensor names differ from llama-style. Two approaches:

**Option 1 (recommended) — virtual mapping at load time.**
In `streaming_inference_init` (the `MAP_LAYER_TENSOR` block, :2242–2263) add a
fallback branch: if `blk.N.attn_q.weight` is not found, look up
`blk.N.attn_qkv.weight`/`blk.N.attn_qkv.bias` and compute the Q/K/V pointers
as offsets inside the fused tensor:

- a QKV matrix row in the GGUF `[in, out]` layout = an output channel;
  the output-row blocks are contiguous: Q = rows `[0, n_embd)`,
  K = `[n_embd, n_embd+n_kv)`, V = `[n_embd+n_kv, n_embd+2·n_kv)`.
- For quantized tensors, the byte offset of row r =
  `r · (in_dim / block_size) · sizeof(block)` — this arithmetic already exists in
  `matmul_stream` (:1289, `byte_offset` from `row_offset`) and in
  `extract_embedding_*` (:1891). Factor it out into a helper
  `static size_t tensor_row_offset(int type, int in_dim, int row)`.
- The bias is a flat f32 vector with the same layout: `bias_q = bias[0..n_embd)`, etc.
- Extend `LayerWeights` (:270–289) with `attn_q_bias/attn_k_bias/attn_v_bias`.

Likewise for `ffn_up` `[n_embd, 2·n_ff]`: gate = rows `[0, n_ff)`, up =
`[n_ff, 2·n_ff)` — mapped onto the existing `lw->ffn_gate` / `lw->ffn_up`.

Upside of option 1: `transformer_forward_stream` (:1859) barely changes.

Touched: `streaming_inference.c` (`LayerWeights`, the `MAP_LAYER_TENSOR` block,
the offset helper); nothing in `gguf_parser.c` (it returns tensors by name
as-is).

#### A3 — QKV bias in the matmul path (S) — ✅ done earlier (commit 78ce660) + slices of the fused attn_qkv.bias

Currently there is no bias anywhere (breaks Qwen2 — the Makefile's default model).

- After `matmul_stream(g_state.q, lw->attn_q, …)` (calls around :1907) add:
  `static void vec_add_bias(float *y, const float *bias, int n)` —
  a trivial SIMD loop modelled on `elem_add_inplace_simd` (:1549).
- Apply it to q/k/v when `lw->attn_*_bias` is present (f32 tensors, no dequant
  needed — bias in GGUF is always F32).
- Check: Qwen2.5-0.5B-Instruct (Makefile default) — logits must match
  llama.cpp greedy.

Touched: `streaming_inference.c` (`transformer_forward_stream`, new helper).
The same code is reused for `glm4` (bias optional) and `chatglm`.

#### A4 — Partial rotary (S) — ✅ done; IMPORTANT: the style is NOT NeoX but interleaved NORM (verified against llama.cpp `llama_rope_type()`: CHATGLM and GLM4-text → LLAMA_ROPE_TYPE_NORM, and against HF `modeling_glm4.py`: `rotate_half` on `x[0::2]/x[1::2]` + `repeat_interleave(2)`). Frequencies are divided by `n_rot`

The current `rope` (:1816) rotates all `head_dim` components in `(i, i+1)` pairs
(GPT-J interleaved). GLM requires:

1. Rotate only the first `n_rot = rope.dimension_count` components of each
   head (GLM-4: head_dim=128, n_rot=64);
2. NeoX rotate-half order: pairs `(i, i + n_rot/2)`, frequency
   `theta^(-2i/n_rot)` for `i ∈ [0, n_rot/2)`.

Signature change: `rope(float *q, float *k, int pos, int dim,
int head_dim, int kv_dim, float theta, int n_rot, bool neox)` — or add a
`rope_neox_partial()` alongside, keeping the old one for llama/qwen.
Note: frequencies are divided by `n_rot`, not by `head_dim`
(`freq = powf(theta, -2.0f·i/n_rot)`); the rest of the head `[n_rot, head_dim)`
stays unrotated.

theta: `rope.freq_base` is already read (:2077; the converter writes
`10000·rope_ratio` into freq_base — check on glm-4-9b-chat: rope_ratio=1 → 10000).

Touched: `streaming_inference.c` (`rope`, call at :1912).

#### A5 — Arch `glm4`: post-norms (S–M) — ✅ done

For GLM-4-0414 the residual branches in `transformer_forward_stream` (:1859)
change:

```
# attention:
attn_out = wo(attn)              # as now, around :1985
attn_out = rmsnorm(attn_out, blk.N.attn_post_norm.weight)   # NEW
x = x + attn_out
# FFN:
ffn_out = ffn_down(swiglu(...))  # as now
ffn_out = rmsnorm(ffn_out, blk.N.ffn_post_norm.weight)      # NEW
x = x + ffn_out
```

- `LayerWeights` += `attn_post_norm`, `ffn_post_norm` (+ types; F32 tensors).
- Branch on `g_cfg.has_post_norms` (A1) — do not break the llama path.
- `attn_output.bias` is unused in glm4; Q/K/V bias per the A1 flag
  (9B-0414: yes, 32B-0414: no) — the loader must tolerate its absence.
- `blk.N.nextn.*` tensors (MTP/NextN) — ignore (skip).

Touched: `streaming_inference.c` (`LayerWeights`, mapping,
`transformer_forward_stream`).

#### A6 — Tokenizer (M)

`bpe_tokenizer.c` is currently a greedy longest-match over `tokenizer.ggml.tokens`
with score priority; GLM-4 needs:

1. **Pre-tokenizer** `chatglm-bpe`/`glm4` (both = `LLAMA_VOCAB_PRE_TYPE_CHATGLM4`
   in llama.cpp): a GPT-4o-style regex
   `(?:'[sS]|'[tT]|'[rR][eE]|'[vV][eE]|'[mM]|'[lL][lL]|'[dD])|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}{1,3}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+`.
   The kernel has no regex engine — a hand-written scanner (~150 lines):
   - contractions `'s 't 're 've 'm 'll 'd` (any case);
   - letters (`\p{L}+`, Unicode: Cyrillic/CJK — classified by UTF-8
     codepoint ranges) with an optional non-letter prefix;
   - numbers — in groups of 1–3 digits (`\p{N}{1,3}`);
   - punctuation/symbols with an optional leading space and trailing \r\n;
   - whitespace: `\s*[\r\n]+`, `\s+(?!\S)` (trailing whitespace), `\s+`.
   Extension point: `bpe_tokenizer_encode` (ai/bpe_tokenizer.c, signature in
   include/embodios/bpe_tokenizer.h:52) — pre-split the text into chunks before
   longest-match. The selector field is `tokenizer.ggml.pre` (pass it through to
   `gguf_model_arch` / a dedicated getter `gguf_parser_get_pre_tokenizer()`).
2. **BPE merge**: for quality — a real merge loop over
   `tokenizer.ggml.merges` (does the parser read merges? — check; if not,
   add reading `tokenizer.ggml.merges` as a string array in `gguf_parser.c`).
   Longest-match is acceptable as a first approximation.
3. **Special tokens**: `<|endoftext|>` (151329, eos), `<|user|>` (151336, eot),
   `<|assistant|>` (151338) — appear in the prompt text as literals;
   longest-match over the vocab will catch them if the strings are in the vocab
   (in GGUF they are). eos is already taken from metadata (`arch->eos_token_id` → `g_cfg`).

#### A7 — Chat formatter and stop tokens (S) — ✅ done: wrapper `[gMASK]<sop><|user|>\n{q}<|assistant|>`, `<sop>`/`<eop>` are special tokens in BPE, stop = `<|user|>` (metadata EOS `<|endoftext|>` is kept) + an extra stop `<|assistant|>` (streaming_inference_add_stop_token, up to 4 stops)

- GLM-4-Chat format: `[gMASK]<sop><|system|>\n{sys}<|user|>\n{q}<|assistant|>\n`
  (no spaces; system is optional). Implementation — a hand-written formatter in
  the `chat` command (core/stubs.c:445) keyed on the arch flag: build the string →
  `bpe_tokenizer_encode(..., add_bos=false)`.
- Stop: generation stops on any of {151329, 151336, 151338}.
  Currently `streaming_inference_generate` (:2291) checks only
  `g_cfg.eos_token_id` — extend to an array of stop tokens
  (`g_cfg.stop_token_ids[4]`, filled in init for GLM).

#### A8 — Tests (M) — ◐ synthetic ✅, real models pending

1. ✅ **Synthetic tiny models** (tools/gen_tiny_chatglm.py): 2 layers,
   dim 64, heads 4, kv 2, n_rot 8, vocab 1000, F32 and Q8_0, both archs.
   numpy reference (tools/ref_chatglm.py) vs the kernel in QEMU (command
   `dbglogits`, tools/compare_logits.py): **top-8 logits match**,
   max |diff| ≤ 0.0018 (F32) / ≤ 0.026 (Q8_0, includes re-quantizing
   activations to Q8_1) — PASS for chatglm and glm4.
2. ✅ **Regression**: SmolLM-135M chat in QEMU — "The capital of France is
   Paris." (the llama path without flags is not broken).
3. ✅ **Metadata of the real glm-edge-1.5b-chat** (zai-org, hf-mirror,
   Range request for the first 8 MB): arch=`chatglm`, `chatglm.*` keys are read
   by our parser, tensors — separate attn_q/k/v (no bias) + fused
   ffn_up [2048, 12288], rope.dimension_count=128 (=head_dim → full
   rotary, the partial path is not needed for this model), vocab 59264,
   eos=59246, pre=`chatglm-bpe`. Our fallback mapping covers both
   QKV layouts (fused THUDM and separate zai-org).
4. ⏳ **Full glm-edge-1.5b run** in QEMU — blocked by the kernel RAM limit
   (1 GB identity map, kernel.c:232): Q4_K_M ~0.9–1.1 GB + KV cache
   ~235 MB do not fit; Q2_K (~700 MB) — barely. TCG speed is ~seconds
   per token for 1.5B — chat is possible, but slow.
5. ⏳ **glm-4-9b-chat Q4_K_M** (~6 GB) — requires raising the RAM limit.

Phase A overall: **M** (many S steps on top of the existing runtime).

---

## 3. Phase B: exo-style ring of nodes

### 3.1 Architecture (classic exo → embodiOS)

Classic exo (exo-explore/ex-exo): masterless P2P, the model is split by
layers (`Shard{model_id, start_layer, end_layer, n_layers}`), UDP-broadcast
discovery (:5678, JSON every 2.5 s, 30 s timeout), gRPC `NodeService`
(SendPrompt/SendTensor/SendResult/CollectTopology/HealthCheck), ring memory
weighted partitioning, OpenAI-compatible API (:52415, SSE).

Mapping onto embodiOS (implemented in this branch's skeleton, `kernel/exo/`):

| exo (Python) | embodiOS (C) | File |
|---|---|---|
| `udp_discovery.py` | UDP :5678 JSON announcements over `tcpip_send_udp`/SOCK_DGRAM | `exo_discovery.c` |
| `grpc/node_service.proto` | binary TCP protocol: 48-byte `exo_tensor_msg_t` (magic/seq/dtype/dims/payload_len, network byte order) + payload | `exo_transport.c`, `exo.h` |
| `ring_memory_weighted_partitioning_strategy.py` | sort nodes by `ram_free`, layer shares `mem/total`, ring | `exo_shard.c` |
| `orchestration/node.py` | state machine: recv TENSOR → own layers → send next; tail — sample+broadcast | `exo_node.c` (`exo__handle_tensor_msg`, `exo_forward_shard`) |
| `api/chatgpt_api.py` | minimal HTTP: `POST /v1/chat/completions` (JSON/SSE), `/v1/models`, `/healthcheck` | `exo_server.c` |
| `inference/mlx,tinygrad` | the existing `streaming_inference.c` (integration points — §3.3) | — |

### 3.2 Steps and estimates (S/M/L)

| Step | Contents | Size | Status in skeleton |
|---|---|---|---|
| B1 | TCP/IP + socket API | S (stack exists: net/tcpip.c) | ✓ in use |
| B2 | UDP-broadcast discovery (:5678, announcement `{node_id, ctrl_port, memory, memory_free, model}`, peer table, 30 s timeout, health) | M | ✓ `exo_discovery.c` (broadcast TX needs a tcpip.c fix — TODO) |
| B3 | Binary node protocol (PROMPT/TENSOR/RESULT/HEALTH/TOPOLOGY, length-prefixed) | M | ✓ `exo_transport.c` (chunking ≤1400 due to tx_buffer; reassembly/windows are stack limitations) |
| B4 | GGUF shard loader: map only `blk.[start..end)`, embedding on ring[0], lm_head on the tail | M | ✗ TODO (every node holds the whole model; the inference side is ready — layer-range API) |
| B5 | Orchestrator: request_id → recv→layers→send; tail: sample + broadcast RESULT + embed to the first node | M–L | ✓ live: lockstep ring on the layer-range API (embed→forward_layers→TENSOR→…→sample→RESULT barrier); a single node = a full run through the exo path |
| B6 | Ring memory weighted partitioning + rebalance on join/leave | M | ✓ `exo_shard.c` (rebalance is triggered from discovery) |
| B7 | OpenAI HTTP API (SSE) | M | ✓ simplified `exo_server.c` (naive JSON; SSE chunks) |
| B8 | Fault tolerance: timeouts, request_id reset, repartitioning | M | ◐ node timeout exists; request reset — TODO |
| B9 | Validation: 2 nodes × GLM-4-9B vs a single-node run; then 32B Q4_K_M on 2–4 nodes | M | ✗ after B4/B5 |

Phase B overall: **L** (dominated by B5 and hardening the transport).

### 3.3 Integration points with existing code (exact locations)

**net/tcpip.c** (changes as a separate task; the skeleton has weak stubs and TODOs):
1. `tcpip_send_udp` (:712) and `tcp_send_packet` (:817): TX to 255.255.255.255
   must go out as an Ethernet broadcast without ARP — currently `NET_ERR_UNREACHABLE`.
2. `handle_ip` (:~561): accept the subnet broadcast
   `dst == (net_cfg.ip_addr | ~net_cfg.netmask)`.
3. `net_cfg` is static (:16): need `tcpip_get_local_ip()/tcpip_get_local_mac()`
   (exo declares weak stubs — the real implementations replace them at link time).
4. `socket_recvfrom` for UDP (the sender address is currently read via
   `tcpip_get_socket_for_testing()`).
5. Going forward: MSS segmentation on TX, window/rx_buffer >4096, reassembly —
   without these the ring's throughput is limited (but a 12–24 KB/token hidden
   vector gets through anyway, ~9–17 chunks).

**ai/streaming_inference.c** (entry functions factored out, branch feat/exo-live):
1. ✅ Layer-range API: `streaming_inference_embed()` (token_embd lookup),
   `streaming_inference_forward_layers(hidden, pos, start, end)` (the layer body
   moved into `layer_forward_stream()`; the KV cache is indexed by the global
   layer number → a shard touches only its own KV region),
   `streaming_inference_sample_token()` (final norm + lm_head + temp/top-p),
   `streaming_inference_is_stop_token()`. `streaming_inference_generate()`
   now uses the same helpers; regression confirmed in QEMU
   (`chat` → "The capital of France is Paris.").
2. Shard weight mapping: the `MAP_LAYER_TENSOR` loop should map only
   `[start,end)`; `token_embd` — only ring[0], `output/output_norm` —
   only the ring's tail (conditional on `exo_shard_local()`). ✗ TODO (B4) —
   for now every node maps the whole model.
3. ✅ Sampling on the tail: `streaming_inference_sample_token()` outside the
   generation loop; the ring's tail sends RESULT (token+finished) to the orchestrator.
   (:2291) into `int streaming_inference_sample_last(float *logits)`.
4. Separate embedding: `void streaming_inference_embed(int token, float *out)`
   (a wrapper over `extract_embedding_transposed`, :1717) — the first node sends
   the new token's embedding around the ring.

**core/stubs.c / core/kernel.c**: shell commands `exo`, `exonodes`, `exoshard`,
`exoserve` (plan in `kernel/exo/README.md`); `exo_poll()` — in `kernel_loop`
(core/kernel.c:371) next to `schedule()`.

### 3.4 Tensor protocol (on-wire)

```
[exo_tensor_msg_t: 48 bytes, network byte order]
  u32 magic = 0x45584F54 ("EXOT")   u16 version = 1   u16 msg_type
  u32 seq                            u64 request_id
  u32 dtype (0=f32,1=f16,2=bf16,3=i32)  u32 n_dims (<=4)  u32 dims[4]
  u32 payload_len
[payload: payload_len bytes]
```

Messages: `PROMPT` (int32 tokens), `TENSOR` (the token's hidden vector,
f32/f16), `RESULT` (token + is_finished — the ring's tail sends it to everyone),
`HEALTH`, `TOPOLOGY` (gossip). Per token only the hidden vector travels the ring
(32B: 6144·f16 ≈ 12 KB × hop); the KV cache stays on the node that owns the
layers.

---

## 4. Risks and caveats

1. **Hop latency**, not bandwidth: expect ~10–20 tok/s for the 9B class
   on 2–4 nodes over 1G Ethernet (estimate from exo/llama.cpp TCP benchmarks);
   the current stack (one frame per send, rx_buffer 4096, polling) adds
   overhead — measure at B9.
2. **RoPE**: GLM (both archs) — partial rotary with **interleaved pairs**
   (NORM, not NeoX — clarified against llama.cpp/HF, see A4). Implemented via
   `rope_n_dims` from `rope.dimension_count`; verified on synthetic models
   (A8.1).
3. **`attention_bias` differs** between GLM-4-9B-0414 (true) and 32B-0414
   (false) — the loader must tolerate both.
4. **GLM-4-32B-0414: 61 layers** — does not divide evenly; weighted partitioning
   accounts for it, but with equal nodes the ranges will be 21/20/20.
5. **Stop tokens**: the actual dialogue stop is `<|user|>` (151336), not eos;
   handle all three (151329/151336/151338).
6. **Kernel memory**: the 1 GB identity-map limit (kernel.c:232) blocks any
   model >0.5B locally, and the ring's KV cache — raising the limit (audit P1)
   is a prerequisite for phases A8/B9.
7. **Single-connection accept** in tcpip.c (:964, accept returns the same
   fd): concurrent requests to one node are serialized; acceptable for the MVP.
8. **ChatGLM3-6B** uses SentencePiece (`chatglm-spm`), not GPT-2 BPE: if needed,
   the tokenizer is a separate workstream.

## 5. Sources

- llama.cpp: `conversion/chatglm.py`, `conversion/glm.py`,
  `src/models/chatglm.cpp`, `src/models/glm4.cpp`, `src/llama-arch.cpp`,
  `src/llama-vocab.cpp`, `gguf-py/gguf/tensor_mapping.py`; PR #8031.
- HF: `zai-org/GLM-4-9B-0414`, `zai-org/GLM-4-32B-0414` (config.json),
  `bartowski/glm-4-9b-chat-GGUF`, `THUDM/glm-4-9b-chat-GGUF`,
  `ZhipuAI/glm-edge-1.5b-chat-gguf`.
- exo: `github.com/exo-explore/exo` (current MLX/RDMA version),
  `github.com/exo-explore/ex-exo` (classic): `node_service.proto`,
  `exo/orchestration/node.py`, `exo/networking/udp/udp_discovery.py`,
  `exo/topology/ring_memory_weighted_partitioning_strategy.py`,
  `exo/api/chatgpt_api.py`.
- embodiOS codebase audit: `audit.md` (state of the net stack, the engine,
  known P0/P1).
