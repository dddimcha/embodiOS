#!/usr/bin/env python3
"""Compare kernel [DBGLOGITS] output with the numpy reference (ref_chatglm.py).

Usage:
    python3 tools/compare_logits.py qemu.log weights.npz chatglm [tol] 3 17 42
    (tol defaults to 2e-3; use ~0.05 for Q8_0 models: kernel requantizes the
     activations to Q8_1 in the fused matmul path)

Parses "[DBGLOGITS] #r id=N logit=X.YYYYYY" lines from the QEMU serial log,
recomputes the reference top-8, and checks:
  - identical top-8 token id sets (order may swap on near-ties)
  - per-id |kernel - ref| <= tolerance (default 2e-3, F32 SIMD reassociation)
Exit 0 on PASS, 1 on FAIL.
"""
import re
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from ref_chatglm import forward  # noqa: E402


def main():
    log_path, npz_path, arch = sys.argv[1], sys.argv[2], sys.argv[3]
    args = sys.argv[4:]
    tol = 2e-3
    if args and args[0].replace(".", "", 1).isdigit() and len(args) > 1:
        tol = float(args[0])
        args = args[1:]
    tokens = [int(t) for t in args] or [3, 17, 42]

    kernel = {}
    pat = re.compile(r"\[DBGLOGITS\] #(\d+) id=(\d+) logit=(-?\d+)\.(\d+)")
    for line in open(log_path, errors="replace"):
        m = pat.search(line)
        if m:
            val = float(f"{m.group(3)}.{m.group(4)}")
            kernel[int(m.group(2))] = val
    if not kernel:
        print("FAIL: no [DBGLOGITS] lines found in", log_path)
        sys.exit(1)

    w = dict(np.load(npz_path))
    logits = forward(w, arch, tokens)
    order = np.argsort(-logits)
    ref = {int(i): float(logits[i]) for i in order[:8]}

    print(f"{'rank':<5}{'kernel id=logit':<28}{'ref id=logit':<28}diff")
    k_ids = list(kernel)
    r_ids = list(ref)
    max_diff = 0.0
    ok = True
    for r in range(8):
        kid = k_ids[r] if r < len(k_ids) else None
        rid = r_ids[r] if r < len(r_ids) else None
        # compare by id (order may swap on near-ties): match kernel id in ref
        if kid in ref:
            diff = abs(kernel[kid] - ref[kid])
            max_diff = max(max_diff, diff)
            match = f"{diff:.6f}"
            if diff > tol:
                ok = False
                match += " >TOL"
        else:
            ok = False
            match = "id not in ref top-8"
        kcell = f"{kid}={kernel.get(kid, float('nan')):.6f}"
        rcell = f"{rid}={ref.get(rid, float('nan')):.6f}"
        print(f"#{r:<4}{kcell:<28}{rcell:<28}{match}")

    if set(k_ids) != set(r_ids):
        # allow near-tie swaps only if values still match by id
        ok = ok and all(k in ref for k in k_ids)

    print("max |diff| =", f"{max_diff:.6f}", " tolerance =", tol)
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
