#!/usr/bin/env python3
"""Host-side accuracy verification for embodiOS K-quants dequantization.

Generates random Q2_K / Q3_K / Q5_K super-blocks (256 values each, ggml
layout), dequantizes them with a numpy reference (exact port of ggml
dequantize_row_q*_K), and compares against:

  1. the shared float ports in kernel/include/embodios/kquants_dequant.h
     (used verbatim by kernel/ai/streaming_inference.c and gguf_inference.c)
  2. the fixed-point Q16.16 ports in kernel/ai/quantized_ops.c

The C harness (tools/host_test_kquants.c) is compiled with host gcc against
the real kernel sources - no kernel code is duplicated here.

Usage: python3 tools/verify_kquants.py [--blocks N] [--seed S]
Exit code 0 = all checks passed.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QK_K = 256


# ---------------------------------------------------------------------------
# Random block generation (ggml layouts)
# ---------------------------------------------------------------------------

def rand_fp16(rng, lo=0.01, hi=2.0):
    """Random finite fp16 value, returned as raw uint16 bits."""
    v = np.float32(rng.uniform(lo, hi))
    return np.float16(v).view(np.uint16)


def gen_q2k_blocks(rng, n):
    """84 bytes: scales[16], qs[64], d(fp16), dmin(fp16)."""
    blocks = []
    for _ in range(n):
        scales = rng.integers(0, 256, 16, dtype=np.uint8)
        qs = rng.integers(0, 256, 64, dtype=np.uint8)
        d = rand_fp16(rng)
        dmin = rand_fp16(rng, 0.0, 0.5)
        blocks.append(scales.tobytes() + qs.tobytes() +
                      struct.pack('<HH', int(d), int(dmin)))
    return b''.join(blocks)


def gen_q3k_blocks(rng, n):
    """110 bytes: hmask[32], qs[64], scales[12], d(fp16)."""
    blocks = []
    for _ in range(n):
        hmask = rng.integers(0, 256, 32, dtype=np.uint8)
        qs = rng.integers(0, 256, 64, dtype=np.uint8)
        scales = rng.integers(0, 256, 12, dtype=np.uint8)
        d = rand_fp16(rng)
        blocks.append(hmask.tobytes() + qs.tobytes() + scales.tobytes() +
                      struct.pack('<H', int(d)))
    return b''.join(blocks)


def gen_q5k_blocks(rng, n):
    """176 bytes: d(fp16), dmin(fp16), scales[12], qh[32], qs[128]."""
    blocks = []
    for _ in range(n):
        d = rand_fp16(rng)
        dmin = rand_fp16(rng, 0.0, 0.5)
        scales = rng.integers(0, 256, 12, dtype=np.uint8)
        qh = rng.integers(0, 256, 32, dtype=np.uint8)
        qs = rng.integers(0, 256, 128, dtype=np.uint8)
        blocks.append(struct.pack('<HH', int(d), int(dmin)) +
                      scales.tobytes() + qh.tobytes() + qs.tobytes())
    return b''.join(blocks)


# ---------------------------------------------------------------------------
# numpy reference dequantization (exact ports of ggml-quants.c)
# ---------------------------------------------------------------------------

def fp16_bits_to_f32(bits):
    return np.uint16(bits).view(np.float16).astype(np.float32)


def get_scale_min_k4(j, q):
    if j < 4:
        return int(q[j]) & 63, int(q[j + 4]) & 63
    return ((int(q[j + 4]) & 0xF) | ((int(q[j - 4]) >> 6) << 4),
            (int(q[j + 4]) >> 4) | ((int(q[j]) >> 6) << 4))


def ref_dequant_q2k(block):
    scales = np.frombuffer(block[0:16], dtype=np.uint8)
    qs = np.frombuffer(block[16:80], dtype=np.uint8)
    d = fp16_bits_to_f32(struct.unpack('<H', block[80:82])[0])
    dmin = fp16_bits_to_f32(struct.unpack('<H', block[82:84])[0])

    y = np.zeros(QK_K, dtype=np.float64)
    pos = 0
    is_ = 0
    for _ in range(2):  # two 128-element halves
        shift = 0
        qoff = 0 if pos == 0 else 32
        for _j in range(4):
            sc = int(scales[is_]); is_ += 1
            dl = d * (sc & 0xF); ml = dmin * (sc >> 4)
            for l in range(16):
                y[pos] = dl * ((int(qs[qoff + l]) >> shift) & 3) - ml
                pos += 1
            sc = int(scales[is_]); is_ += 1
            dl = d * (sc & 0xF); ml = dmin * (sc >> 4)
            for l in range(16):
                y[pos] = dl * ((int(qs[qoff + 16 + l]) >> shift) & 3) - ml
                pos += 1
            shift += 2
        qoff += 0  # qs pointer advances 32 bytes per 128-half; handled above
        if pos == 128:
            pass
    return y


def ref_dequant_q3k(block):
    hmask = np.frombuffer(block[0:32], dtype=np.uint8)
    qs = np.frombuffer(block[32:96], dtype=np.uint8)
    packed = block[96:108]
    d = fp16_bits_to_f32(struct.unpack('<H', block[108:110])[0])

    kmask1 = np.uint32(0x03030303)
    kmask2 = np.uint32(0x0f0f0f0f)
    aux = np.frombuffer(packed + b'\x00\x00\x00\x00', dtype='<u4').copy()
    tmp = aux[2]
    aux[2] = ((aux[0] >> 4) & kmask2) | (((tmp >> 4) & kmask1) << 4)
    aux[3] = ((aux[1] >> 4) & kmask2) | (((tmp >> 6) & kmask1) << 4)
    aux[0] = (aux[0] & kmask2) | (((tmp >> 0) & kmask1) << 4)
    aux[1] = (aux[1] & kmask2) | (((tmp >> 2) & kmask1) << 4)
    scales = aux.view('<i1')[:16].astype(np.int32)

    y = np.zeros(QK_K, dtype=np.float64)
    pos = 0
    is_ = 0
    m = 1
    for half in range(2):
        shift = 0
        base = half * 32  # qs advances 32 bytes per 128-half; hmask does not
        for _j in range(4):
            dl = d * (scales[is_] - 32); is_ += 1
            for l in range(16):
                q3 = ((int(qs[base + l]) >> shift) & 3) - (0 if (int(hmask[l]) & m) else 4)
                y[pos] = dl * q3
                pos += 1
            dl = d * (scales[is_] - 32); is_ += 1
            for l in range(16):
                q3 = ((int(qs[base + 16 + l]) >> shift) & 3) - (0 if (int(hmask[16 + l]) & m) else 4)
                y[pos] = dl * q3
                pos += 1
            shift += 2
            m <<= 1  # mask bit carries across both 128-halves (1..128)
    return y


def ref_dequant_q5k(block):
    d = fp16_bits_to_f32(struct.unpack('<H', block[0:2])[0])
    dmin = fp16_bits_to_f32(struct.unpack('<H', block[2:4])[0])
    scales = np.frombuffer(block[4:16], dtype=np.uint8)
    qh = np.frombuffer(block[16:48], dtype=np.uint8)
    qs = np.frombuffer(block[48:176], dtype=np.uint8)

    y = np.zeros(QK_K, dtype=np.float64)
    pos = 0
    is_ = 0
    u1, u2 = 1, 2
    for j in range(0, QK_K, 64):
        sc, m = get_scale_min_k4(is_ + 0, scales)
        d1 = d * sc; m1 = dmin * m
        sc, m = get_scale_min_k4(is_ + 1, scales)
        d2 = d * sc; m2 = dmin * m
        base = (j // 64) * 32
        for l in range(32):
            y[pos] = d1 * ((int(qs[base + l]) & 0xF) + (16 if (int(qh[l]) & u1) else 0)) - m1
            pos += 1
        for l in range(32):
            y[pos] = d2 * ((int(qs[base + l]) >> 4) + (16 if (int(qh[l]) & u2) else 0)) - m2
            pos += 1
        is_ += 2
        u1 <<= 2
        u2 <<= 2
    return y


IQ4_NL_KVALUES = [-127, -104, -83, -65, -49, -35, -22, -10,
                  1, 13, 25, 38, 53, 69, 89, 113]


def gen_iq4nl_blocks(rng, n):
    """18 bytes: d(fp16), qs[16] - 32 values per block."""
    blocks = []
    for _ in range(n):
        d = rand_fp16(rng)
        qs = rng.integers(0, 256, 16, dtype=np.uint8)
        blocks.append(struct.pack('<H', int(d)) + qs.tobytes())
    return b''.join(blocks)


def ref_dequant_iq4nl(block):
    d = fp16_bits_to_f32(struct.unpack('<H', block[0:2])[0])
    qs = np.frombuffer(block[2:18], dtype=np.uint8)
    y = np.zeros(32, dtype=np.float64)
    for j in range(16):
        y[j] = d * IQ4_NL_KVALUES[int(qs[j]) & 0xF]
        y[j + 16] = d * IQ4_NL_KVALUES[int(qs[j]) >> 4]
    return y


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

CASES = {
    'q2k': (gen_q2k_blocks, ref_dequant_q2k, 84, 256, True),
    'q3k': (gen_q3k_blocks, ref_dequant_q3k, 110, 256, True),
    'q5k': (gen_q5k_blocks, ref_dequant_q5k, 176, 256, True),
    'iq4nl': (gen_iq4nl_blocks, ref_dequant_iq4nl, 18, 32, False),
}

FLOAT_TOL_REL = 1e-6   # C float32 vs numpy float64 reference
FLOAT_TOL_ABS = 1e-7
FIXED_TOL_REL = 5e-4   # Q16.16 fixed-point quantization (1.5e-5 ulp) + rounding
FIXED_TOL_ABS = 5e-4


def compare(name, ref, got, tol_rel, tol_abs):
    abs_err = np.abs(got.astype(np.float64) - ref)
    denom = np.maximum(np.abs(ref), 1e-30)
    rel_err = abs_err / denom
    ok = np.all(abs_err <= tol_abs + tol_rel * np.abs(ref))
    print('  %-6s max_abs=%.3e max_rel=%.3e  %s' %
          (name, abs_err.max(), rel_err.max(), 'PASS' if ok else 'FAIL'))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--blocks', type=int, default=64)
    ap.add_argument('--seed', type=int, default=1234)
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)
    tmp = tempfile.mkdtemp(prefix='kquants_')
    harness = os.path.join(tmp, 'host_test_kquants')

    compile_cmd = [
        'gcc', '-O2', '-Wall', '-Wextra',
        '-I', os.path.join(REPO, 'kernel', 'include'),
        os.path.join(REPO, 'tools', 'host_test_kquants.c'),
        os.path.join(REPO, 'kernel', 'ai', 'quantized_ops.c'),
        '-o', harness,
    ]
    print('Compiling: %s' % ' '.join(compile_cmd))
    subprocess.run(compile_cmd, check=True)

    all_ok = True
    for mode, (gen, ref_fn, bsz, elems, has_fixed) in CASES.items():
        print('=== %s (%d blocks) ===' % (mode.upper(), args.blocks))
        data = gen(rng, args.blocks)
        in_path = os.path.join(tmp, mode + '.bin')
        with open(in_path, 'wb') as f:
            f.write(data)

        out_prefix = os.path.join(tmp, mode)
        subprocess.run([harness, mode, in_path, str(args.blocks), out_prefix],
                       check=True)

        got_float = np.fromfile(out_prefix + '.float.bin', dtype='<f4')

        ref = np.zeros(args.blocks * elems, dtype=np.float64)
        for b in range(args.blocks):
            block = data[b * bsz:(b + 1) * bsz]
            ref[b * elems:(b + 1) * elems] = ref_fn(block)

        all_ok &= compare('float', ref, got_float, FLOAT_TOL_REL, FLOAT_TOL_ABS)
        if has_fixed:
            got_fixed = np.fromfile(out_prefix + '.fixed.bin', dtype='<f4')
            all_ok &= compare('fixed', ref, got_fixed, FIXED_TOL_REL, FIXED_TOL_ABS)

    print('=== RESULT: %s ===' % ('ALL PASS' if all_ok else 'FAILURES'))
    return 0 if all_ok else 1


if __name__ == '__main__':
    sys.exit(main())
