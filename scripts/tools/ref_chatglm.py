#!/usr/bin/env python3
"""Numpy reference forward pass for the tiny synthetic chatglm/glm4 models.

Implements exactly the graph that embodiOS streaming_inference.c executes:
  - RMSNorm (eps from config)
  - chatglm: fused attn_qkv [dim+2*kv_dim, dim] split into Q|K|V rows + bias
  - glm4: separate q/k/v + bias, plus attn_post_norm / ffn_post_norm RMSNorms
    inside the residual branches (before the residual add)
  - partial rotary (n_rot=head_dim/2), interleaved pairs (i, i+1),
    freq_i = theta^(-2i/n_rot)  [llama.cpp NORM rope_type, NOT NeoX]
  - GQA: query head h attends to kv head h // (n_heads/n_kv_heads)
  - SwiGLU seq-split: fused ffn_up [2*n_ff, dim], gate = rows [0, n_ff),
    up = rows [n_ff, 2*n_ff); out = silu(gate) * up
  - logits = output.weight @ rmsnorm(x, output_norm)

Usage:
    python3 tools/ref_chatglm.py weights.npz chatglm 3 17 42
Prints top-8 logits as "RANK id logit" lines for comparison with the kernel
'dbglogits' command output ([DBGLOGITS] #r id=.. logit=..).
"""
import sys

import numpy as np

DIM = 64
N_LAYERS = 2
N_HEADS = 4
N_KV_HEADS = 2
HEAD_DIM = DIM // N_HEADS
KV_DIM = HEAD_DIM * N_KV_HEADS
N_FF = 128
N_ROT = HEAD_DIM // 2
EPS = 1e-5
THETA = 10000.0


def rmsnorm(x, w, eps=EPS):
    return x * (1.0 / np.sqrt(np.mean(x.astype(np.float64) ** 2) + eps)).astype(np.float64) * w


def silu(x):
    return x / (1.0 + np.exp(-x))


def rope_partial(vec, pos, n_heads, n_rot=N_ROT, theta=THETA):
    """vec: [n_heads*head_dim]; rotate pairs (i,i+1) for i in [0,n_rot)."""
    v = vec.reshape(n_heads, HEAD_DIM).astype(np.float64).copy()
    for h in range(n_heads):
        for i in range(0, n_rot, 2):
            freq = theta ** (-i / n_rot)
            c, s = np.cos(pos * freq), np.sin(pos * freq)
            q0, q1 = v[h, i], v[h, i + 1]
            v[h, i] = q0 * c - q1 * s
            v[h, i + 1] = q0 * s + q1 * c
    return v.reshape(-1)


def forward(w, arch, tokens):
    x = w["token_embd.weight"][tokens[0]].astype(np.float64)
    k_cache = {l: [] for l in range(N_LAYERS)}
    v_cache = {l: [] for l in range(N_LAYERS)}

    for pos, tok in enumerate(tokens):
        if pos > 0:
            x = w["token_embd.weight"][tok].astype(np.float64)
        for l in range(N_LAYERS):
            p = f"blk.{l}."
            h = rmsnorm(x, w[p + "attn_norm.weight"])
            if arch == "chatglm":
                qkv = w[p + "attn_qkv.weight"] @ h + w[p + "attn_qkv.bias"]
                q = qkv[:DIM]
                k = qkv[DIM:DIM + KV_DIM]
                v = qkv[DIM + KV_DIM:DIM + 2 * KV_DIM]
            else:
                q = w[p + "attn_q.weight"] @ h + w[p + "attn_q.bias"]
                k = w[p + "attn_k.weight"] @ h + w[p + "attn_k.bias"]
                v = w[p + "attn_v.weight"] @ h + w[p + "attn_v.bias"]

            q = rope_partial(q, pos, N_HEADS)
            k = rope_partial(k, pos, N_KV_HEADS)
            k_cache[l].append(k)
            v_cache[l].append(v)

            kv_mul = N_HEADS // N_KV_HEADS
            attn_out = np.zeros(DIM)
            for hh in range(N_HEADS):
                qh = q[hh * HEAD_DIM:(hh + 1) * HEAD_DIM]
                kh = hh // kv_mul
                scores = np.array([
                    qh @ k_cache[l][t][kh * HEAD_DIM:(kh + 1) * HEAD_DIM] / np.sqrt(HEAD_DIM)
                    for t in range(pos + 1)
                ])
                probs = np.exp(scores - scores.max())
                probs /= probs.sum()
                oh = np.zeros(HEAD_DIM)
                for t in range(pos + 1):
                    oh += probs[t] * v_cache[l][t][kh * HEAD_DIM:(kh + 1) * HEAD_DIM]
                attn_out[hh * HEAD_DIM:(hh + 1) * HEAD_DIM] = oh

            ao = w[p + "attn_output.weight"] @ attn_out
            if arch == "glm4":
                ao = rmsnorm(ao, w[p + "attn_post_norm.weight"])
            x = x + ao

            h = rmsnorm(x, w[p + "ffn_norm.weight"])
            gu = w[p + "ffn_up.weight"] @ h
            gate, up = gu[:N_FF], gu[N_FF:]
            fo = w[p + "ffn_down.weight"] @ (silu(gate) * up)
            if arch == "glm4":
                fo = rmsnorm(fo, w[p + "ffn_post_norm.weight"])
            x = x + fo

    x = rmsnorm(x, w["output_norm.weight"])
    logits = w["output.weight"] @ x
    return logits


def main():
    npz_path = sys.argv[1]
    arch = sys.argv[2] if len(sys.argv) > 2 else "chatglm"
    tokens = [int(t) for t in sys.argv[3:]] or [3, 17, 42]

    w = dict(np.load(npz_path))
    logits = forward(w, arch, tokens)

    order = np.argsort(-logits)
    print(f"REF top-8 logits after {len(tokens)} prompt tokens ({tokens}):")
    for r, idx in enumerate(order[:8]):
        print(f"REF #{r} id={idx} logit={logits[idx]:.6f}")


if __name__ == "__main__":
    main()
