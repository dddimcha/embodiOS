# GPU Backend — Vulkan Compute (v0.5.0 "Tesla", WS-GPU)

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
- `matmul_f32` and `matmul_q8_0` produce **bit-exact** results against the
  CPU reference on every tested shape, including odd sizes that exercise the
  workgroup bounds check (measured `max_abs_err = 0`, `max_rel_err = 0`):

```
device: llvmpipe (LLVM 15.0.6, 256 bits) (vendorID 0x10005, api 1.3)
pipeline matmul_f32: created OK (431 words)
pipeline matmul_q8_0: created OK (820 words)
matmul_f32   37x129x23  : max_abs_err=0 max_rel_err=0 -> PASS
matmul_f32   64x64 x64  : max_abs_err=0 max_rel_err=0 -> PASS
matmul_f32    1x1  x1   : max_abs_err=0 max_rel_err=0 -> PASS
matmul_f32  128x256x96  : max_abs_err=0 max_rel_err=0 -> PASS
matmul_q8_0  33x128x17  : max_abs_err=0 max_rel_err=0 -> PASS
matmul_q8_0  64x256x64  : max_abs_err=0 max_rel_err=0 -> PASS
matmul_q8_0   1x32 x1   : max_abs_err=0 max_rel_err=0 -> PASS
matmul_q8_0 100x96 x100 : max_abs_err=0 max_rel_err=0 -> PASS
ALL TESTS PASSED
```

  Bit-exactness is expected, not accidental: the shaders accumulate in the
  same order as the CPU reference (k ascending; per-block partial sum scaled
  by the Q8_0 block scale, matching the kernel's `vec_dot_q8_0` structure),
  and lavapipe executes fp32 IEEE semantics.

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

Both compute shaders use `local_size_x = 64`, one invocation per element of
C, push constants `{ u32 m, u32 k, u32 n }`, and three SSBOs (set 0,
bindings 0/1/2 = A/B/C, std430).

- **matmul_f32** — `C[m*n] = A[m*k] * B[k*n]`, all fp32 row-major.
- **matmul_q8_0** — A is ggml Q8_0 blocks (34 bytes per 32 values: u16 fp16
  scale + 32×int8). The shader views A as u32 words, extracts the fp16
  scale with a branch-free bit-manipulation converter (denormals exact via
  `mant × 2⁻²⁴`; inf/nan pass through), sign-extends each int8 lane, and
  accumulates per block: `acc += d * Σ qⱼ·B[...]`. `k` must be a multiple
  of 32 (ggml row invariant).

**Buffer contract:** the A buffer for `matmul_q8_0` must be padded to a
4-byte multiple (≤ 3 tail bytes). The shader reads whole u32 words, and the
last word of the final 34-byte block would otherwise straddle the buffer
end (found during lavapipe validation with a single-block matrix).

### Runtime dispatch

`matmul_q8_0_fused()` in `kernel/ai/streaming_inference.c` first calls
`gpu_backend_probe()` (cached — one branch on GPU-less systems) and only
then `gpu_matmul_q8_0()`. Any negative result falls through to the existing
SIMD path. There is no behavioral change when no GPU is present.

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
- `matmul_q8_0` requires `k % 32 == 0` (ggml invariant; caller-side partial
  rows would need a dequant tail path).
- Stretch shader `matmul_q4_k` is not implemented in v0.5.0 (the deployed
  135M model is Q4_K_M, but Q4_K dequant on GPU is follow-up work; the CPU
  fused path handles it today).
- No multi-dispatch pipelining or persistent buffers yet; the dispatch TODOs
  in `vk_device.c` stage buffers per call, mirroring the host harness.
