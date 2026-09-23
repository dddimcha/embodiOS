# GPU Backend — Vulkan Compute (v0.6.0 "Volta", WS-GPU + WS-B)

embodiOS ships a real Vulkan compute matmul path: hand-assembled SPIR-V
shaders, a host-side validation harness that runs them end-to-end on the
lavapipe software Vulkan driver, and a kernel-side device layer with an
honest runtime probe and zero-regression CPU fallback.

## What is validated

**Host, end-to-end on lavapipe** (`make -C tools vktest`,
binary `tools/host_test_vulkan`):

- The exact SPIR-V words embedded in the kernel (`kernel/ai/vk_shaders.h`)
  are accepted by a real Vulkan driver (`vkCreateShaderModule` +
  `vkCreateComputePipelines`) and pass `spirv-val --target-env vulkan1.1`.
- All four matmul formats produce **bit-exact** results against the CPU
  reference on every tested shape, including odd sizes that exercise the
  workgroup bounds check (measured `max_abs_err = 0`, `max_rel_err = 0`):

```
device: llvmpipe (LLVM 15.0.6, 256 bits) (vendorID 0x10005, api 1.3)
pipeline matmul_f32: created OK (431 words)
pipeline matmul_q8_0: created OK (820 words)
pipeline matmul_q4_k_q8_0: created OK (1599 words)
pipeline matmul_q6_k_q8_0: created OK (1759 words)
matmul_f32   37x129x23  : max_abs_err=0 max_rel_err=0 -> PASS
matmul_f32   64x64 x64  : max_abs_err=0 max_rel_err=0 -> PASS
matmul_f32    1x1  x1   : max_abs_err=0 max_rel_err=0 -> PASS
matmul_f32  128x256x96  : max_abs_err=0 max_rel_err=0 -> PASS
matmul_q8_0  33x128x17  : max_abs_err=0 max_rel_err=0 -> PASS
matmul_q8_0  64x256x64  : max_abs_err=0 max_rel_err=0 -> PASS
matmul_q8_0   1x32 x1   : max_abs_err=0 max_rel_err=0 -> PASS
matmul_q8_0 100x96 x100 : max_abs_err=0 max_rel_err=0 -> PASS
matmul_q4_k_q8_0   3x256x5   : max_abs_err=0 max_rel_err=0 -> PASS
matmul_q4_k_q8_0   1x256x1   : max_abs_err=0 max_rel_err=0 -> PASS
matmul_q4_k_q8_0  16x512x9   : max_abs_err=0 max_rel_err=0 -> PASS
matmul_q4_k_q8_0  33x768x7   : max_abs_err=0 max_rel_err=0 -> PASS
matmul_q6_k_q8_0   5x256x3   : max_abs_err=0 max_rel_err=0 -> PASS
matmul_q6_k_q8_0   1x256x1   : max_abs_err=0 max_rel_err=0 -> PASS
matmul_q6_k_q8_0   9x512x16  : max_abs_err=0 max_rel_err=0 -> PASS
matmul_q6_k_q8_0  17x1280x6  : max_abs_err=0 max_rel_err=0 -> PASS
ALL TESTS PASSED
```

  Bit-exactness is expected, not accidental: the shaders accumulate in the
  same order as the CPU reference (k ascending; per-block partial sum scaled
  by the Q8_0 block scale, matching the kernel's `vec_dot_q8_0` structure;
  for the K-quants: exact int32 group sums, then the same float op order as
  `vec_dot_q4_k_q8_1_scalar` / `vec_dot_q6_k_q8_1_scalar` in
  `kernel/ai/simd_kernels_scalar.c`), and lavapipe executes fp32 IEEE
  semantics. The K-quant test matrices use deterministic PRNG superblocks
  with full-range random scale/quant bytes plus periodic zero and fp16-
  denormal superblock scales, exercising every branch of the in-shader
  fp16→fp32 converter.

  One lavapipe codegen quirk matters for bit-exactness: it canonicalizes
  `acc + (a - b)` into `(acc + a) - b`. The Q4_K shader emits the canonical
  form directly (and the CPU reference matches), see the comment in
  `build_matmul_q4_k_q8_0()` in `tools/spirv_gen.py`.

**Kernel, under QEMU TCG** (no GPU present):

- Builds with `GPU_OBJS` and boots with zero regressions (`make test` 6/6,
  chat inference intact).
- The probe reports honestly, e.g. on the default QEMU machine:
  `GPU: display adapter 1234:1111 at 00:02.0 (not Vulkan-capable)` followed by
  `GPU: none (no Vulkan-capable PCI device)`.
- `vulkantest` console command: 29/29 pass, including the probe report and
  the dispatch fallback contract (`gpu_matmul_q8_0` returns `< 0` without a
  GPU, so callers fall back to SIMD).

## What is NOT validated (and why)

- **Real GPU hardware.** No physical GPU is available in the development
  environment; the passthrough activation path is documented but untested.
- **Vulkan from inside the kernel.** Under QEMU TCG there is no
  guest-reachable Vulkan driver: no KVM, and this QEMU build has no
  Venus/virtio-gpu-vulkan support. `vk_device_init()` therefore stops at
  `VK_DEV_NO_DRIVER` after PCI discovery, and every matmul entry point
  reports failure so the caller uses the SIMD CPU path. Nothing in the
  kernel pretends a GPU exists.

## Architecture

```
tools/spirv_gen.py          hand-assembled SPIR-V 1.3 generator (no GLSL
                            compiler exists in this environment); emits
kernel/ai/vk_shaders.h      static const uint32_t shader words (committed)
tools/host_test_vulkan.c    host harness: lavapipe, CPU reference from the
                            kernel's own dequant math, self-contained
                            minimal Vulkan declarations (no vulkan-dev pkg)
kernel/ai/vk_device.c       kernel device layer: PCI probe, Vulkan object
                            model, dispatch entry points, activation TODOs
kernel/ai/gpu_backend.c     policy: cached probe, backend state, fallback
kernel/ai/vulkan_test.c     console test suite (probe report + contracts)
```

### Shaders

All four compute shaders use `local_size_x = 64`, one invocation per element
of C, push constants `{ u32 m, u32 k, u32 n }`, and three SSBOs (set 0,
bindings 0/1/2 = A/B/C, std430).

- **matmul_f32** — `C[m*n] = A[m*k] * B[k*n]`, all fp32 row-major.
- **matmul_q8_0** — A is ggml Q8_0 blocks (34 bytes per 32 values: u16 fp16
  scale + 32×int8). The shader views A as u32 words, extracts the fp16
  scale with a branch-free bit-manipulation converter (denormals exact via
  `mant × 2⁻²⁴`; inf/nan pass through), sign-extends each int8 lane, and
  accumulates per block: `acc += d * Σ qⱼ·B[...]`. `k` must be a multiple
  of 32 (ggml row invariant).
- **matmul_q4_k_q8_0** (v0.6.0) — A is ggml Q4_K superblocks (144 bytes per
  256 values: `d`/`dmin` fp16 + 12 bytes of packed 6-bit scales/mins +
  128 bytes of nibbles, 8 groups of 32), B is Q8_0-quantized activations.
  Per group: exact int32 sums `isum = Σ q·yq`, `iysum = Σ yq`, then
  `sumi += sc·yd·isum`, `summs += yd·(mn·iysum)`, and per superblock
  `acc = (acc + d·sumi) − dmin·summs`. `k` must be a multiple of 256.
- **matmul_q6_k_q8_0** (v0.6.0) — A is ggml Q6_K superblocks (210 bytes per
  256 values: `ql[128]` + `qh[64]` + `scales[16]` int8 + `d` fp16, 2 halves
  × 4 groups of 32), B as above. Per group the 32 elements split into two
  exact int32 sums scaled by `sc[2g]`/`sc[2g+1]`; `acc += d·sumi` per
  superblock. `k` must be a multiple of 256.

  Q8_0 activations carry no fp16 block sum (that is Q8_1), so the Q4_K mins
  term uses `yd × Σ yq` (exact int32 quant sum times the dequantized
  activation scale) instead of the Q8_1 `s` field. This matches the
  deployment dot `vec_dot_q4_k_q8_1` structurally; the GPU result is
  bit-exact against the Q8_0-based CPU reference in
  `tools/host_test_vulkan.c`.

**B-as-Q8_0 layout:** for `matmul_q4_k_q8_0` / `matmul_q6_k_q8_0`, B holds
one Q8_0 block chain per column: block `b` of column `j` sits at byte
offset `(j·(k/32) + b) · 34`. The caller quantizes each fp32 activation
column with the ggml row quantizer (in deployment `n = 1`, so this is one
chain per matvec).

**Buffer contract:** any quantized-format buffer viewed as u32 words must be
padded to a 4-byte multiple (≤ 3 tail bytes): A for `matmul_q8_0` and
`matmul_q6_k_q8_0` (34- and 210-byte blocks are not word-aligned), B for
both K-quant shaders. The shader reads whole u32 words, and the last word
of the final block would otherwise straddle the buffer end (found during
lavapipe validation with a single-block matrix).

### Runtime dispatch

`matmul_q8_0_fused()` in `kernel/ai/streaming_inference.c` first calls
`gpu_backend_probe()` (cached — one branch on GPU-less systems) and only
then `gpu_matmul_q8_0()`. Any negative result falls through to the existing
SIMD path. There is no behavioral change when no GPU is present.

v0.6.0 adds `gpu_matmul_q4_k_q8_0()` / `gpu_matmul_q6_k_q8_0()` (wrapping
`vk_matmul_q4_k_q8_0()` / `vk_matmul_q6_k_q8_0()`) behind the same cached
probe; they reject `k % 256 != 0` and return `< 0` until a Vulkan transport
exists, so under QEMU TCG (`GPU: none`) nothing changes. Wiring them into
`matmul_q4_k_fused()` / `matmul_q6_k_fused()` is part of the transport
activation milestone (the caller must quantize activations to Q8_0 block
chains instead of Q8_1 for the GPU path).

### Weight-format coverage (SmolLM-135M Q4_K_M)

Weight-element shares of the deployed Q4_K_M model, and their dispatch
status:

| format | share of weight elements | GPU shader |
|--------|--------------------------|------------|
| Q5_0   | 58.0% | not yet (CPU fused path) |
| Q8_0   | 22.2% | `matmul_q8_0` (fp32 B) |
| Q4_K   | 10.5% | `matmul_q4_k_q8_0` (v0.6.0) |
| Q6_K   |  9.2% | `matmul_q6_k_q8_0` (v0.6.0) |
| F32    |  0.03% (norms) | `matmul_f32` |

The GPU path now covers both K-quant superblock formats (~20% of this
model's weights; Q4_K+Q6_K dominate larger Q4_K_M models — e.g. Llama-3.2
Q4_K_M is ~90% Q4_K/Q6_K). The remaining gap for SmolLM-135M is Q5_0
(58%): no `matmul_q5_0` shader yet — the CPU fused Q5_0×Q8_1 path handles
it with zero regression.

## Activation path for real hardware

1. **virtio-gpu with Venus (planned primary path).** Run QEMU with
   `-device virtio-gpu-gl,venus=on` (requires a QEMU build with Venus and a
   host Vulkan driver). The kernel then:
   - detects `1af4:1010/1050` in `vk_device_probe()` (already implemented);
   - negotiates `VIRTIO_GPU_F_VENUS` (TODO in `vk_device_init()`);
   - creates a Venus context (`VIRTIO_GPU_CMD_CTX_INIT`) and marshals
     `vkCreateInstance`/`vkCreateDevice`/descriptor/pipeline/dispatch calls
     per `VK_EXT_command_stream` over the control virtqueue.
   The dispatch sequence mirrors `vk_run_matmul()` in
   `tools/host_test_vulkan.c` 1:1.
2. **PCI passthrough of a real GPU** (AMD 0x1002 / NVIDIA 0x10DE / Intel
   0x8086 display-class devices are already recognized as candidates).
   Requires vendor KMD territory: BAR mapping, firmware upload, doorbells.
   Documented as TODO; out of scope for a generic layer.

## Building and testing

```bash
# host validation (needs libvulkan runtime + lavapipe ICD; no -dev package)
make -C tools vktest

# regenerate the embedded shaders after editing tools/spirv_gen.py
python3 tools/spirv_gen.py          # writes kernel/ai/vk_shaders.h

# kernel (probe + fallback are always compiled in; no special flag needed)
cd kernel && make -j4 GGUF_MODEL=/path/to/model.gguf

# in the embodiOS console
gpu          # one-line probe report
vulkantest   # full suite (29 checks)
```

## Known limitations

- Naive shader schedule (one thread per C element, no tiling/shared memory).
  Correct, lavapipe-validated; performance tuning belongs to the hardware
  activation milestone.
- `matmul_q8_0` requires `k % 32 == 0`; the K-quant shaders require
  `k % 256 == 0` (ggml superblock invariants; caller-side partial rows would
  need a dequant tail path).
- No `matmul_q5_0` shader yet — Q5_0 is 58% of the deployed SmolLM-135M
  Q4_K_M weights and stays on the CPU fused path (see the coverage table).
- The K-quant shaders take Q8_0-quantized activations, while the CPU fused
  path quantizes to Q8_1; the two are numerically equivalent up to the Q4_K
  mins term (`yd·Σyq` vs the fp16 `s` block sum), which is why bit-exactness
  is claimed against the Q8_0-based CPU reference, not against
  `vec_dot_q4_k_q8_1` on the same floats.
- No multi-dispatch pipelining or persistent buffers yet; the dispatch TODOs
  in `vk_device.c` stage buffers per call, mirroring the host harness.

---

## virtio-gpu driver + Venus transport (v0.6.0 "Volta", WS-C)

New in this release: a real virtio-gpu control-plane driver
(`kernel/drivers/gpu/virtio_gpu.c`) plus a Venus protocol layer
(`kernel/drivers/gpu/venus.c`). The two parts have very different
validation status — read the split below before trusting either.

### Validated under QEMU 7.2 (`-device virtio-gpu-pci`)

The driver probes both virtio-gpu PCI ids (transitional `1af4:1010` and
modern-only `1af4:1050`). QEMU's `virtio-gpu-pci` is modern-only, so the
driver implements the **modern virtio PCI transport**: vendor-capability
discovery (common/notify/device-config regions), MMIO register access
through `arch_identity_map_mmio()` (the PCI MMIO window, e.g. 0xFE000000,
is outside the kernel identity map — `vmm_map()` does not work for it
because the boot page tables use 2 MB huge pages), 64-bit feature
negotiation with the FEATURES_OK handshake, and split-ring virtqueue
programming. A legacy I/O-port path is kept for transitional devices on
other hypervisors.

Boot log with the device attached:

```
[VirtIO-GPU] Probing device 1AF4:1050 at 00:04.0
[VirtIO-GPU] modern transport, common_cfg=0xFE000000 notify=0xFE003000 devcfg=0xFE002000
[VirtIO-GPU] host features: 0x0000010130000002, negotiated: 0x0000000100000002
[VirtIO-GPU] controlq: 64 descriptors (modern)
[VirtIO-GPU] cursorq: 16 descriptors (modern)
[VirtIO-GPU] scanouts: 1, capsets: 0
[VirtIO-GPU] display info: 1 scanout(s) enabled
[VirtIO-GPU]   scanout 0: 1280x800+0+0
GPU: virtio-gpu without Venus capset (transport unavailable)
```

Validated pieces: PCI probe, capability parsing, feature negotiation
(VIRGL deliberately not requested), controlq/cursorq setup,
`VIRTIO_GPU_CMD_GET_DISPLAY_INFO`, capset enumeration
(`GET_CAPSET_INFO`/`GET_CAPSET`), the `gpuinfo` console command, and the
honest no-Venus path. `make test` 6/6, plain boot (no GPU device), and
the 135M chat regression all pass with the driver compiled in.

Note on the device config ABI: `struct virtio_gpu_config` uses **32-bit**
`events_read`/`events_clear` (Linux uapi / QEMU reality), so
`num_scanouts`/`num_capsets` live at offsets 0x08/0x0c, not 0x04/0x08 as
some spec drafts imply. Verified against QEMU 7.2 at runtime.

### Spec-complete, NOT validated here (no Venus device in this QEMU)

`venus.c` is written against the virtio-gpu Venus specification
(virtio spec 1.2 §5.7, virglrenderer `virgl_renderer_capset_venus`, Mesa
`vn_ring`/`vkCreateRingMESA`). It implements, behind clear
`TODO(venus-host)` markers:

- `VIRTIO_GPU_CAPSET_VENUS` (capset id **4**) detection and capset payload
  fetch (wire format / vk.xml / VK_EXT_command_stream versions);
- Venus context creation (`VIRTIO_GPU_CMD_CTX_CREATE` with
  `context_init = CAPSET_ID(VENUS)`, requires `VIRTIO_GPU_F_CONTEXT_INIT`);
- command-ring allocation and registration via an encoded
  `vkCreateRingMESA` record, and vn_protocol command submission through
  `VIRTIO_GPU_CMD_SUBMIT_3D` with fences (`venus_vk_execute()`).

None of that path can run in this environment (QEMU 7.2 here has no
`venus=on` device), so the byte-level details — the vkCreateRingMESA
record layout, blob-resource backing of the ring, ring wrap/wait-space
semantics — must be validated against a live Venus host before use.
When the capset is absent the layer is inert by construction: it logs
`GPU: virtio-gpu without Venus capset (transport unavailable)` and
`venus_transport_probe()` returns NULL. Integration with
`gpu_backend.c`/`vk_device.c` (WS-B) is via the exported
`venus_transport_probe()` / `venus_vk_execute()` symbols.
