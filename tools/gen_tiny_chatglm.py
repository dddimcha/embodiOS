#!/usr/bin/env python3
"""Generate a tiny synthetic chatglm/glm4 GGUF model for embodiOS kernel testing.

The model has known random weights (fixed seed), 2 layers, dim=64, 4 heads
(2 KV heads, GQA 2:1), head_dim=16, partial rotary n_rot=8, n_ff=128,
vocab=1000. All tensors are F32 so the kernel forward pass can be compared
against the numpy reference (tools/ref_chatglm.py) to ~1e-5.

Usage:
    python3 tools/gen_tiny_chatglm.py chatglm out.gguf weights.npz [Q8_0]
    python3 tools/gen_tiny_chatglm.py glm4    out.gguf weights.npz [Q8_0]

Optional 4th argument quantizes 2D weight tensors (default F32). The .npz
then stores the *dequantized* weights so the reference computes what an
ideal kernel would produce from the quantized file.
"""
import sys

import numpy as np
from gguf import GGUFWriter, GGMLQuantizationType
from gguf.quants import quantize as gguf_quantize, dequantize as gguf_dequantize

DIM = 64
N_LAYERS = 2
N_HEADS = 4
N_KV_HEADS = 2
HEAD_DIM = DIM // N_HEADS
KV_DIM = HEAD_DIM * N_KV_HEADS
N_FF = 128
N_ROT = HEAD_DIM // 2
VOCAB = 1000
CTX = 128
EPS = 1e-5
ROPE_THETA = 10000.0

# Special token ids (mirroring the GLM-4 layout at a small scale)
ID_ENDOFTEXT = VOCAB - 4   # eos
ID_USER = VOCAB - 3        # eot / chat stop
ID_ASSISTANT = VOCAB - 2
ID_GMASK = VOCAB - 1       # [gMASK] (also <sop> lives at VOCAB-5)
ID_SOP = VOCAB - 5


def build_vocab():
    toks = [f"t{i}" for i in range(VOCAB - 5)]
    toks += ["<sop>", "<|endoftext|>", "<|user|>", "<|assistant|>", "[gMASK]"]
    assert len(toks) == VOCAB
    return toks


def gen_weights(rng, arch):
    w = {}

    def rnd(*shape):
        # small values keep the forward pass in a sane numeric range
        return rng.standard_normal(shape, dtype=np.float32) * np.float32(0.1)

    def rndn(n):  # norm weights around 1.0
        return (1.0 + 0.1 * rng.standard_normal(n)).astype(np.float32)

    # GGUF dims are reversed vs numpy: numpy (out, in) -> gguf dims [in, out]
    w["token_embd.weight"] = rnd(VOCAB, DIM)
    w["output.weight"] = rnd(VOCAB, DIM)
    w["output_norm.weight"] = rndn(DIM)

    for l in range(N_LAYERS):
        p = f"blk.{l}."
        w[p + "attn_norm.weight"] = rndn(DIM)
        if arch == "chatglm":
            # fused QKV [dim + 2*kv_dim, dim], rows: Q | K | V
            w[p + "attn_qkv.weight"] = rnd(DIM + 2 * KV_DIM, DIM)
            w[p + "attn_qkv.bias"] = rnd(DIM + 2 * KV_DIM)
        else:  # glm4: separate q/k/v with biases
            w[p + "attn_q.weight"] = rnd(DIM, DIM)
            w[p + "attn_k.weight"] = rnd(KV_DIM, DIM)
            w[p + "attn_v.weight"] = rnd(KV_DIM, DIM)
            w[p + "attn_q.bias"] = rnd(DIM)
            w[p + "attn_k.bias"] = rnd(KV_DIM)
            w[p + "attn_v.bias"] = rnd(KV_DIM)
            w[p + "attn_post_norm.weight"] = rndn(DIM)
            w[p + "ffn_post_norm.weight"] = rndn(DIM)
        w[p + "attn_output.weight"] = rnd(DIM, DIM)
        w[p + "ffn_norm.weight"] = rndn(DIM)
        # fused gate|up [2*n_ff, dim]: gate = first n_ff rows, up = last n_ff
        w[p + "ffn_up.weight"] = rnd(2 * N_FF, DIM)
        w[p + "ffn_down.weight"] = rnd(DIM, N_FF)
    return w


def write_gguf(path, arch, w, qtype=None):
    writer = GGUFWriter(path, arch)
    writer.add_context_length(CTX)
    writer.add_embedding_length(DIM)
    writer.add_block_count(N_LAYERS)
    writer.add_feed_forward_length(N_FF)
    writer.add_head_count(N_HEADS)
    writer.add_head_count_kv(N_KV_HEADS)
    writer.add_layer_norm_rms_eps(EPS)
    writer.add_rope_dimension_count(N_ROT)
    writer.add_rope_freq_base(ROPE_THETA)
    writer.add_tokenizer_model("gpt2")
    writer.add_tokenizer_pre("chatglm-bpe" if arch == "chatglm" else "glm4")
    writer.add_token_list(build_vocab())
    writer.add_bos_token_id(ID_ENDOFTEXT)
    writer.add_eos_token_id(ID_ENDOFTEXT)

    ref = {}
    for name, arr in w.items():
        arr = np.ascontiguousarray(arr, dtype=np.float32)
        if qtype is not None and arr.ndim == 2:
            q = gguf_quantize(arr, qtype)
            writer.add_tensor(name, q, raw_dtype=qtype)
            ref[name] = gguf_dequantize(q, qtype)
        else:
            writer.add_tensor(name, arr)
            ref[name] = arr
    writer.write_header_to_file()
    writer.write_kv_data_to_file()
    writer.write_tensors_to_file()
    writer.close()
    return ref


def main():
    arch = sys.argv[1] if len(sys.argv) > 1 else "chatglm"
    assert arch in ("chatglm", "glm4")
    out_gguf = sys.argv[2] if len(sys.argv) > 2 else f"tiny_{arch}.gguf"
    out_npz = sys.argv[3] if len(sys.argv) > 3 else f"tiny_{arch}.npz"
    qtype = None
    if len(sys.argv) > 4:
        qtype = getattr(GGMLQuantizationType, sys.argv[4])

    rng = np.random.default_rng(1337)
    w = gen_weights(rng, arch)
    ref = write_gguf(out_gguf, arch, w, qtype)
    np.savez(out_npz, **ref)
    print(f"wrote {out_gguf} (arch={arch}, quant={qtype}) and {out_npz}")
    print(f"config: dim={DIM} layers={N_LAYERS} heads={N_HEADS} kv={N_KV_HEADS} "
          f"n_ff={N_FF} n_rot={N_ROT} vocab={VOCAB} eps={EPS} theta={ROPE_THETA}")


if __name__ == "__main__":
    main()
