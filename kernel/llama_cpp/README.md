# llama.cpp Kernel Integration

> **STATUS (build): EXCLUDED FROM THE BUILD.** The files in this directory are an
> unmodified upstream snapshot that is **not compiled** (see `kernel/Makefile`:
> `LLAMA_CPP_SOURCES` and `GGML_C_SOURCES` are empty). Reasons:
> - `ggml/ggml.c` requires host OS headers (`<syscall.h>`, `sys/*.h`) that do not
>   exist in the freestanding kernel environment, and conflicts with
>   `include/compat/*`.
> - No compute backend was ever wired up (`ggml-cpu` is not built), and none of
>   the live code (`ai/streaming_inference.c`) uses the GGML API.
> The directory is kept for reference. To re-enable, fix the compat header
> conflicts first, then populate `GGML_C_SOURCES`/`LLAMA_CPP_SOURCES` again.

This directory contains llama.cpp integrated directly into the EMBODIOS kernel.

## Architecture

Instead of reimplementing transformer inference from scratch, we use llama.cpp directly:

- **ggml/** - Core tensor operations (C)
  - `ggml.c` - Tensor library
  - `ggml-quants.c` - Q4_K dequantization
  - `ggml-alloc.c` - Memory allocation

- **src/** - LLaMA inference engine (C++)
  - `llama.cpp` - Main inference logic

- **include/** - Headers for ggml and llama

- **kernel_stubs.cpp** - Minimal C++ runtime and libc stubs:
  - `operator new/delete` → `kmalloc/kfree`
  - `pthread_*` → no-op (single-threaded)
  - `FILE*` → memory-mapped stubs
  - `malloc/calloc/realloc` → kernel heap

- **llama_kernel.cpp** - Kernel-friendly llama.cpp wrapper

## Benefits

1. **Proven inference engine** - llama.cpp is battle-tested
2. **Optimized operations** - Hand-tuned SIMD kernels
3. **GGUF support** - Native quantized model loading
4. **No reinvention** - Use existing, working code
5. **Easy updates** - Pull upstream llama.cpp changes

## Status

- [x] Copy core llama.cpp files
- [x] Create C++ runtime stubs
- [x] Create libc stubs (FILE*, pthread)
- [ ] Update Makefile to build llama.cpp
- [ ] Implement memory-based GGUF loading
- [ ] Wire into kernel inference command
- [ ] Test with TinyLlama model

## Next Steps

1. Add llama.cpp files to Makefile
2. Create memory-backed FILE* for embedded GGUF
3. Call llama.cpp API from `infer` command
4. Test and verify REAL inference works
