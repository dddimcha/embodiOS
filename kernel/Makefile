# EMBODIOS Kernel Makefile

# Architecture detection
ARCH ?= x86_64
ARCH_DIR = arch/$(ARCH)

# Tools - defaults
CC ?= gcc
CXX ?= g++
AS ?= as
LD ?= ld
OBJCOPY ?= objcopy

# Detect build OS early
BUILD_OS := $(shell uname -s)

# Cross-compilation for x86_64 (on macOS or Linux cross-compile)
ifeq ($(ARCH),x86_64)
    # Check for x86_64-elf cross-compiler (preferred for bare-metal ELF)
    X86_64_ELF_GCC := $(shell which x86_64-elf-gcc 2>/dev/null)
    ifneq ($(X86_64_ELF_GCC),)
        # Use x86_64-elf-gcc cross-compiler (produces ELF)
        CC := x86_64-elf-gcc
        CXX := x86_64-elf-g++
        AS := x86_64-elf-as
        LD := x86_64-elf-ld
        OBJCOPY := x86_64-elf-objcopy
    endif
endif

# Cross-compilation for ARM64
ifeq ($(ARCH),aarch64)
    # Check for aarch64-elf cross-compiler (preferred for bare-metal ELF)
    AARCH64_ELF_GCC := $(shell which aarch64-elf-gcc 2>/dev/null)
    ifneq ($(AARCH64_ELF_GCC),)
        # Use aarch64-elf-gcc cross-compiler (produces ELF)
        CC := aarch64-elf-gcc
        CXX := aarch64-elf-g++
        AS := aarch64-elf-as
        LD := aarch64-elf-ld
        OBJCOPY := aarch64-elf-objcopy
    else ifeq ($(BUILD_OS),Darwin)
        # macOS ARM64 - use native tools (produces Mach-O, for testing only)
        CC := clang
        AS := as
        LD := ld
        OBJCOPY := objcopy
    else
        # Linux - use cross-compiler
        CROSS_PREFIX ?= aarch64-linux-gnu-
        CC := $(CROSS_PREFIX)gcc
        CXX := $(CROSS_PREFIX)g++
        AS := $(CROSS_PREFIX)as
        LD := $(CROSS_PREFIX)ld
        OBJCOPY := $(CROSS_PREFIX)objcopy
    endif
endif

# Vulkan shader compiler detection (shaderc)
# Prefer glslc (from shaderc) as it's the official Vulkan shader compiler
# Fall back to glslangValidator (from glslang) if glslc is not available
GLSLC := $(shell which glslc 2>/dev/null)
GLSLANG := $(shell which glslangValidator 2>/dev/null)
ifneq ($(GLSLC),)
    # Use glslc (shaderc) - produces SPIR-V directly
    SHADER_COMPILER := $(GLSLC)
    SHADER_FLAGS := -fshader-stage=comp -o
    $(info Vulkan shader compiler: glslc (shaderc))
else ifneq ($(GLSLANG),)
    # Use glslangValidator (glslang) - produces SPIR-V
    SHADER_COMPILER := $(GLSLANG)
    SHADER_FLAGS := -V -o
    $(info Vulkan shader compiler: glslangValidator (glslang))
else
    # No shader compiler found - GPU acceleration will be limited
    SHADER_COMPILER :=
    $(info Vulkan shader compiler not found - install shaderc or glslang for GPU acceleration)
endif

# Common flags for all architectures
CFLAGS = -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
CFLAGS += -fno-pic -Wall -Wextra -O2
# IMPORTANT: compat headers MUST come first to override system headers for llama.cpp
CFLAGS += -I include/compat -I include -I include/arch/$(ARCH)
CFLAGS += -I llama_cpp/include -I llama_cpp/ggml
# Vulkan SDK headers for GPU acceleration
CFLAGS += -I include/vulkan
# Allow compiler builtin headers for GGML (needs stdbool.h, stdint.h, etc.)
CFLAGS += -isystem $(shell $(CC) -print-file-name=include)

# C++ flags for llama.cpp - needs standard headers but not stdlib
# Start from scratch instead of inheriting CFLAGS to allow standard headers
CXXFLAGS = -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
CXXFLAGS += -fno-pic -Wall -Wextra -O2
# IMPORTANT: compat headers MUST come first to override system headers for llama.cpp
CXXFLAGS += -I include/compat -I include -I include/arch/$(ARCH)
CXXFLAGS += -fno-exceptions -fno-rtti -std=c++17
CXXFLAGS += -I llama_cpp/include -I llama_cpp/ggml -I llama_cpp/src
# Vulkan SDK headers for GPU acceleration
CXXFLAGS += -I include/vulkan

# Vulkan GPU acceleration support
# Enable with: make GGML_VULKAN=1
# Requires Vulkan SDK headers and ggml-vulkan.cpp backend implementation
ifeq ($(GGML_VULKAN),1)
    $(info GGML Vulkan backend enabled)
    CFLAGS += -DGGML_USE_VULKAN
    CXXFLAGS += -DGGML_USE_VULKAN
endif

# Coverage instrumentation support (gcov/lcov)
# Enable with: make COVERAGE=1
# Note: Uses lib/gcov_stubs.c for bare-metal gcov runtime support
ifeq ($(COVERAGE),1)
    $(info Coverage instrumentation enabled)
    CFLAGS += -fprofile-arcs -ftest-coverage
    CXXFLAGS += -fprofile-arcs -ftest-coverage
endif

# Architecture-specific flags
ifeq ($(ARCH),x86_64)
    CFLAGS += -mno-red-zone -mcmodel=kernel
    # NOTE: SSE2 enabled for AI inference SIMD and math functions (required)
    CFLAGS += -msse2 -mfpmath=sse
    # AVX2 support - optional, requires real hardware or KVM (QEMU TCG doesn't support AVX)
    # Enable with: make AVX2=1
    ifeq ($(AVX2),1)
        $(info AVX2 SIMD enabled - requires real hardware or KVM acceleration)
        CFLAGS += -mavx2 -mfma
        CXXFLAGS += -msse2 -mfpmath=sse -mavx2 -mfma
    else
        # C++ needs at least SSE2 for x86_64
        CXXFLAGS += -msse2 -mfpmath=sse
    endif
    # Check if we're using cross-compiler or native tools
    ifneq (,$(findstring x86_64-elf,$(CC)))
        # Cross-compiler (x86_64-elf-gcc)
        ASFLAGS = -c
        LDFLAGS = -m elf_x86_64 -z noexecstack
    else
        # Check if we're on Linux or macOS
        UNAME_S := $(shell uname -s)
        ifeq ($(UNAME_S),Linux)
            ASFLAGS = -c
            LDFLAGS = -m elf_x86_64 -z noexecstack
        else
            # macOS native flags (Clang)
            ASFLAGS = -arch x86_64
            LDFLAGS = -arch x86_64
        endif
    endif
endif

ifeq ($(ARCH),aarch64)
    # ARM64-specific flags
    ifneq ($(AARCH64_ELF_GCC),)
        # aarch64-elf cross-compiler flags (produces ELF for bare-metal)
        # -mno-outline-atomics: inline atomic ops instead of calling libgcc helpers
        CFLAGS += -march=armv8-a+simd -mtune=cortex-a57 -mno-outline-atomics
        CXXFLAGS += -march=armv8-a+simd -mtune=cortex-a57 -mno-outline-atomics
        ASFLAGS = -march=armv8-a
        LDFLAGS = -m aarch64elf
    else ifeq ($(BUILD_OS),Darwin)
        # macOS ARM64 flags with SIMD support
        CFLAGS += -target arm64-apple-macos11 -arch arm64 -march=armv8-a+simd
        CXXFLAGS += -target arm64-apple-macos11 -arch arm64 -march=armv8-a+simd
        ASFLAGS = -arch arm64
        LDFLAGS = -arch arm64 -static -e _start
    else
        # Linux ARM64 cross-compile flags with SIMD support
        CFLAGS += -march=armv8-a+simd -mtune=cortex-a57
        CXXFLAGS += -march=armv8-a+simd -mtune=cortex-a57
        ASFLAGS = -march=armv8-a
        LDFLAGS = -m aarch64elf
    endif
endif

# Source files
KERNEL_C_SOURCES = \
    core/kernel.c \
    core/console.c \
    core/panic.c \
    core/stubs.c \
    core/interrupt.c \
    core/task.c \
    core/percpu.c \
    core/gdb_stub.c \
    core/dma.c \
    core/dma_test.c \
    core/lock_test.c \
    timer.c \
    core/hal_cpu.c \
    core/hal_timer.c \
    core/profiler.c \
    core/ui.c \
    core/cmd_storage.c \
    core/cmd_smp.c \
    core/cmd_parbench.c \
    core/cmd_power.c \
    core/rt_timer.c \
    core/cmd_motor.c \
    fs/embfs.c \
    drivers/pci/pci.c \
    drivers/block/virtio_blk.c \
    drivers/block/virtio_mmio.c \
    drivers/nvme/nvme.c \
    drivers/net/virtio_net.c \
    drivers/net/e1000e.c \
    drivers/can/can.c \
    net/tcpip.c \
    net/modbus.c \
    net/ethercat.c \
    exo/exo_node.c \
    exo/exo_discovery.c \
    exo/exo_shard.c \
    exo/exo_transport.c \
    exo/exo_server.c \
    mm/pmm.c \
    mm/vmm.c \
    mm/slab.c \
    mm/heap.c \
    ai/model_runtime.c \
    ai/tvm_runtime.c \
    ai/tvm_graph_executor.c \
    ai/tvm_model_loader.c \
    ai/tokenizer.c \
    ai/gguf_integer_loader.c \
    ai/gguf_parser.c \
    ai/gguf_loader.c \
    ai/gguf_inference.c \
    ai/quantized_ops.c \
    ai/quantized_test.c \
    ai/gpu_backend.c \
    ai/vulkan_test.c \
    ai/e2e_gpu_test.c \
    ai/quantized_inference.c \
    ai/tinyllama_integer_inference.c \
    ai/tinystories_inference.c \
    ai/simd_ops.c \
    ai/simd_kernels_scalar.c \
    ai/simd_dispatch.c \
    ai/memory_opt.c \
    ai/quantized_matmul_simd.c \
    ai/embeddings.c \
    ai/kv_cache_enhanced.c \
    ai/kv_cache_benchmark.c \
    ai/model_loader.c \
    ai/model_registry.c \
    ai/embedded_model_stubs.c \
    ai/bpe_tokenizer.c \
    ai/chat_template.c \
    ai/transformer_full.c \
    ai/transformer_inference.c \
    ai/inference_test.c \
    ai/tensor_ops.c \
    ai/streaming_inference.c \
    ai/parallel_inference.c \
    ai/fixed_point.c \
    ai/benchmark.c \
    ai/tvm_benchmark.c \
    ai/tinyllama_gguf_inference.c \
    test/test_framework.c \
    test/example_test.c \
    test/test_pmm.c \
    test/test_profiler.c \
    lib/string.c \
    lib/stdlib.c \
    lib/math.c \
    lib/compat_stubs.c

# GPU-DRVR block (WS-C, Volta v0.6.0, branch volta-vktransport):
# virtio-gpu control-plane driver + Venus protocol layer.
# Own-workstream objects only; appended here to keep the main list intact.
KERNEL_C_SOURCES += \
    drivers/gpu/virtio_gpu.c \
    drivers/gpu/venus.c

# Add gcov runtime stubs when coverage is enabled
ifeq ($(COVERAGE),1)
    KERNEL_C_SOURCES += lib/gcov_stubs.c
endif

# LLaMA.cpp integration - ENABLED with full GGML support
# The compat headers in kernel/include/compat/ provide bare-metal stubs for stdio.h, etc.
# GGML sources synced from: https://github.com/ggerganov/llama.cpp/tree/master/ggml
#
# kernel_stubs.cpp provides:
# - C++ runtime (new/delete operators)
# - pthread stubs for single-threaded kernel

# NOTE: llama.cpp/GGML sources are present in llama_cpp/ but are NOT built.
# They are an unmodified upstream snapshot that does not compile with the
# freestanding compat headers (needs host <syscall.h>, sys/*.h) and provides
# no compute backend (ggml-cpu is not built). The live inference engine is
# ai/streaming_inference.c, which does not use the GGML API at all.
# kernel_stubs.cpp (operator new/delete, pthread stubs) is only needed when
# C++ sources are built; with no C++ in the build it is excluded too.
# See kernel/llama_cpp/README.md.
LLAMA_CPP_SOURCES =

# GGML Vulkan backend (C++) - compiled when GGML_VULKAN=1
# Provides GPU-accelerated matrix operations via Vulkan compute shaders
ifeq ($(GGML_VULKAN),1)
    LLAMA_CPP_SOURCES += llama_cpp/ggml/ggml-vulkan.cpp
endif

# GGML core library - DISABLED (see note above)
# To resurrect, fix compat header conflicts first, then set:
#   GGML_C_SOURCES = llama_cpp/ggml/ggml.c ... etc
GGML_C_SOURCES =

# Architecture-specific sources
ifeq ($(ARCH),x86_64)
    ARCH_AS_SOURCES = $(ARCH_DIR)/boot.S $(ARCH_DIR)/interrupt.S $(ARCH_DIR)/smp_trampoline.S
    ARCH_C_SOURCES = \
        $(ARCH_DIR)/cpu.c \
        $(ARCH_DIR)/vga.c \
        $(ARCH_DIR)/early_init.c \
        $(ARCH_DIR)/idt.c \
        $(ARCH_DIR)/pic.c \
        $(ARCH_DIR)/paging.c \
        $(ARCH_DIR)/keyboard.c \
        $(ARCH_DIR)/serial.c \
        $(ARCH_DIR)/tsc.c \
        $(ARCH_DIR)/hpet.c \
        $(ARCH_DIR)/hal_timer.c \
        $(ARCH_DIR)/lapic_timer.c \
        $(ARCH_DIR)/smp.c
    # WS-A (volta-ipi): per-CPU LAPIC timers + IPI wakeups
    ARCH_C_SOURCES += $(ARCH_DIR)/ipi.c
endif

ifeq ($(ARCH),aarch64)
    ifeq ($(BUILD_OS),Darwin)
        # Use assembly UART for HVF compatibility (avoids post-indexed stores)
        ARCH_AS_SOURCES = $(ARCH_DIR)/boot.S $(ARCH_DIR)/mmu.S $(ARCH_DIR)/uart_asm.S
    else
        ARCH_AS_SOURCES = $(ARCH_DIR)/boot_linux.S $(ARCH_DIR)/mmu.S $(ARCH_DIR)/uart_asm.S
    endif
    ARCH_C_SOURCES = \
        $(ARCH_DIR)/cpu.c \
        $(ARCH_DIR)/early_init.c \
        $(ARCH_DIR)/hal_timer.c
endif

# Object files
KERNEL_OBJS = $(KERNEL_C_SOURCES:.c=.o)
LLAMA_CPP_OBJS = $(LLAMA_CPP_SOURCES:.cpp=.o)
GGML_C_OBJS = $(GGML_C_SOURCES:.c=.o)
ARCH_AS_OBJS = $(ARCH_AS_SOURCES:.S=.o)
ARCH_C_OBJS = $(ARCH_C_SOURCES:.c=.o)

# AVX2 SIMD kernels: built ONLY on x86_64 and ONLY with -mavx2 for this one
# translation unit (runtime dispatch in ai/simd_dispatch.c guards execution;
# the rest of the kernel remains baseline SSE2 for the fallback path).
ifeq ($(ARCH),x86_64)
    SIMD_AVX2_OBJS = ai/simd_kernels_avx2.o
else
    SIMD_AVX2_OBJS =
endif

# Model embedding support
# TinyStories model path (relative to kernel directory)
TINYSTORIES_MODEL = ../models/tinystories-15m.bin
TINYSTORIES_TOKENIZER = ../models/tokenizer.bin
# GGUF model for embedded inference (smallest available: tinystories-656k.gguf ~732KB)
# GGUF Model - can be overridden: make GGUF_MODEL=../models/smollm-135m-instruct-q4_k_m.gguf
GGUF_MODEL ?= ../models/smollm-135m-instruct-q4_k_m.gguf

# Enable model embedding for x86_64 builds if model files exist
# Works on both Linux and macOS with cross-compilation
# The kernel boots fine without embedded models - AI will just be unavailable
ifeq ($(ARCH),x86_64)
    MODEL_OBJS =
    # Check if model files exist before trying to embed them
    ifneq ($(wildcard $(TINYSTORIES_MODEL)),)
        MODEL_OBJS += ai/tinystories_model.o ai/tinystories_model_flag.o
        $(info Model embedding enabled: $(TINYSTORIES_MODEL) found)
    else
        $(info Model embedding disabled: $(TINYSTORIES_MODEL) not found)
    endif
    # Always embed tokenizer if available (small file, 433KB)
    ifneq ($(wildcard $(TINYSTORIES_TOKENIZER)),)
        MODEL_OBJS += ai/tinystories_tokenizer.o ai/tokenizer_flag.o
        $(info Tokenizer embedding enabled: $(TINYSTORIES_TOKENIZER) found)
    endif
    # Embed GGUF model if available
    ifneq ($(wildcard $(GGUF_MODEL)),)
        MODEL_OBJS += ai/gguf_model.o ai/gguf_model_flag.o
        $(info GGUF embedding enabled: $(GGUF_MODEL) found)
    endif
else ifeq ($(ARCH),aarch64)
    MODEL_OBJS =
    # Embed GGUF model for ARM64 if available (used for speed testing)
    ifneq ($(wildcard $(GGUF_MODEL)),)
        MODEL_OBJS += ai/gguf_model.o ai/gguf_model_flag.o
        $(info GGUF embedding enabled for ARM64: $(GGUF_MODEL) found)
    else
        $(info GGUF embedding disabled for ARM64: $(GGUF_MODEL) not found)
    endif
else
    MODEL_OBJS =
endif

ALL_OBJS = $(ARCH_AS_OBJS) $(ARCH_C_OBJS) $(KERNEL_OBJS) $(LLAMA_CPP_OBJS) $(GGML_C_OBJS) $(SIMD_AVX2_OBJS) $(MODEL_OBJS)

# Targets
all: embodios.elf embodios.bin

# ----------------------------------------------------------------------------
# GPU section (WS-GPU, v0.5.0 "Tesla") — Vulkan compute backend
# ----------------------------------------------------------------------------
# vk_device.c is the kernel-side Vulkan compute device layer (PCI probe +
# driver skeleton; see kernel/include/embodios/vk_device.h). The compute
# shaders are hand-assembled by tools/spirv_gen.py into ai/vk_shaders.h
# (no GLSL compiler exists in the build environment) and validated
# end-to-end on lavapipe by tools/host_test_vulkan.c.
GPU_OBJS = ai/vk_device.o

# Generate the embedded SPIR-V shaders. Regenerate manually with:
#     python3 ../tools/spirv_gen.py
ai/vk_shaders.h: ../tools/spirv_gen.py
	python3 ../tools/spirv_gen.py $@

ai/vk_device.o: ai/vk_device.c ai/vk_shaders.h
	$(CC) $(CFLAGS) -c $< -o $@

ai/vulkan_test.o: ai/vulkan_test.c ai/vk_shaders.h
	$(CC) $(CFLAGS) -c $< -o $@

ALL_OBJS += $(GPU_OBJS)

embodios.elf: $(ALL_OBJS) $(ARCH_DIR)/kernel.ld
ifeq ($(ARCH),aarch64)
ifneq ($(AARCH64_ELF_GCC),)
	$(LD) $(LDFLAGS) -T $(ARCH_DIR)/kernel.ld -o $@ $(ALL_OBJS)
else
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS)
endif
else
ifneq (,$(findstring x86_64-elf,$(LD)))
	$(LD) $(LDFLAGS) -T $(ARCH_DIR)/kernel.ld -o $@ $(ALL_OBJS)
else ifeq ($(BUILD_OS),Darwin)
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS)
else
	$(LD) $(LDFLAGS) -T $(ARCH_DIR)/kernel.ld -o $@ $(ALL_OBJS)
endif
endif

embodios.bin: embodios.elf
	$(OBJCOPY) -O binary $< $@

# Sign kernel for UEFI Secure Boot
sign: embodios.elf
	@echo "Signing kernel for Secure Boot..."
	./scripts/sign_kernel.sh embodios.elf

# Verify kernel signature
sign-check: embodios.elf.signed
	@echo "Verifying kernel signature..."
	sbverify --cert secure-boot/DB.crt embodios.elf.signed

# Pattern rules
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# AVX2-only translation unit (explicit rule wins over the pattern rule)
ai/simd_kernels_avx2.o: ai/simd_kernels_avx2.c
	$(CC) $(CFLAGS) -mavx2 -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(ASFLAGS) -c $< -o $@

# Vulkan shader compilation (GLSL -> SPIR-V)
# Compile compute shaders (.comp) to SPIR-V (.spv)
ifneq ($(SHADER_COMPILER),)
%.spv: %.comp
	@echo "Compiling shader: $< -> $@"
	$(SHADER_COMPILER) $(SHADER_FLAGS) $@ $<

%.spv: %.vert
	@echo "Compiling vertex shader: $< -> $@"
	$(SHADER_COMPILER) $(SHADER_FLAGS) $@ $<

%.spv: %.frag
	@echo "Compiling fragment shader: $< -> $@"
	$(SHADER_COMPILER) $(SHADER_FLAGS) $@ $<
endif

# Clean
clean:
	rm -f $(ALL_OBJS) embodios.elf embodios.bin embodios.iso embodios.elf.signed
	rm -f ai/tinystories_model_flag.o ai/tokenizer_flag.o ai/gguf_model_flag.o ai/gguf_model.o

# Test target - run kernel unit tests in QEMU
# Usage: make test           - runs all tests
#        make test TESTNAME=foo - runs single test named "foo"
test: embodios.elf
	@echo "Running kernel unit tests in QEMU..."
	@cd .. && bash scripts/run_kernel_tests.sh $(TESTNAME)

# Coverage target - build with coverage, run tests, generate report
# Usage: make coverage - generates HTML coverage report
coverage:
	@echo "Generating kernel coverage report..."
	@cd .. && bash scripts/coverage_report.sh

# Embed TinyStories model as binary object
ai/tinystories_model.o: $(TINYSTORIES_MODEL)
	@echo "Embedding TinyStories-15M model into kernel..."
ifeq ($(ARCH),x86_64)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		--redefine-sym _binary____models_tinystories_15m_bin_start=_binary_tinystories_15m_bin_start \
		--redefine-sym _binary____models_tinystories_15m_bin_end=_binary_tinystories_15m_bin_end \
		--redefine-sym _binary____models_tinystories_15m_bin_size=_binary_tinystories_15m_bin_size \
		$< $@
else
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		--redefine-sym _binary____models_tinystories_15m_bin_start=_binary_tinystories_15m_bin_start \
		--redefine-sym _binary____models_tinystories_15m_bin_end=_binary_tinystories_15m_bin_end \
		--redefine-sym _binary____models_tinystories_15m_bin_size=_binary_tinystories_15m_bin_size \
		$< $@
endif

# Presence flag object - separate rule to avoid -jN race
ai/tinystories_model_flag.o: ai/tinystories_model.o
	@echo "int _tinystories_model_present = 1;" | $(CC) $(CFLAGS) -x c -c - -o $@

# Embed tokenizer as binary object
ai/tinystories_tokenizer.o: $(TINYSTORIES_TOKENIZER)
	@echo "Embedding TinyStories tokenizer into kernel..."
ifeq ($(ARCH),x86_64)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		--redefine-sym _binary____models_tokenizer_bin_start=_binary_tokenizer_bin_start \
		--redefine-sym _binary____models_tokenizer_bin_end=_binary_tokenizer_bin_end \
		--redefine-sym _binary____models_tokenizer_bin_size=_binary_tokenizer_bin_size \
		$< $@
else
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		--redefine-sym _binary____models_tokenizer_bin_start=_binary_tokenizer_bin_start \
		--redefine-sym _binary____models_tokenizer_bin_end=_binary_tokenizer_bin_end \
		--redefine-sym _binary____models_tokenizer_bin_size=_binary_tokenizer_bin_size \
		$< $@
endif

# Presence flag object - separate rule to avoid -jN race
ai/tokenizer_flag.o: ai/tinystories_tokenizer.o
	@echo "int _tokenizer_present = 1;" | $(CC) $(CFLAGS) -x c -c - -o $@

# Embed GGUF model as binary object
# We copy to a fixed temp name to get predictable symbol names
GGUF_TEMP = ai/embedded_model.gguf
ai/gguf_model.o: $(GGUF_MODEL)
	@echo "Embedding GGUF model into kernel: $(GGUF_MODEL)"
	@cp $(GGUF_MODEL) $(GGUF_TEMP)
ifeq ($(ARCH),x86_64)
	$(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		--redefine-sym _binary_ai_embedded_model_gguf_start=_binary_gguf_model_start \
		--redefine-sym _binary_ai_embedded_model_gguf_end=_binary_gguf_model_end \
		--redefine-sym _binary_ai_embedded_model_gguf_size=_binary_gguf_model_size \
		$(GGUF_TEMP) $@
	@rm -f $(GGUF_TEMP)
else ifeq ($(ARCH),aarch64)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		--redefine-sym _binary_ai_embedded_model_gguf_start=_binary_gguf_model_start \
		--redefine-sym _binary_ai_embedded_model_gguf_end=_binary_gguf_model_end \
		--redefine-sym _binary_ai_embedded_model_gguf_size=_binary_gguf_model_size \
		$(GGUF_TEMP) $@
	@rm -f $(GGUF_TEMP)
endif

# Presence flag object - separate rule to avoid -jN race
ai/gguf_model_flag.o: ai/gguf_model.o
	@echo "int _gguf_model_present = 1;" | $(CC) $(CFLAGS) -x c -c - -o $@

# ISO for x86_64
ifeq ($(ARCH),x86_64)
iso: embodios.elf
	mkdir -p iso/boot/grub
	cp embodios.elf iso/boot/
	printf '%s\n' \
	    'set timeout=3' \
	    'set default=0' \
	    'serial --unit=0 --speed=115200' \
	    'terminal_input console serial' \
	    'terminal_output console serial' \
	    'menuentry "EMBODIOS - Bare Metal AI OS" {' \
	    '    multiboot2 /boot/embodios.elf' \
	    '    boot' \
	    '}' > iso/boot/grub/grub.cfg
	x86_64-elf-grub-mkrescue -o embodios.iso iso 2>&1 || grub-mkrescue -o embodios.iso iso 2>&1 || echo "grub-mkrescue not available"
	@ls -lh embodios.iso 2>/dev/null || true

# Production ISO with manifest
iso-prod: embodios.elf
	@echo "Building production ISO..."
	./scripts/make_iso.sh

# Production ISO with models included
iso-full: embodios.elf
	@echo "Building full ISO with models..."
	./scripts/make_iso.sh --models

# Quick ISO using existing GRUB setup
iso-quick: embodios.elf
	mkdir -p grub_iso/boot
	cp embodios.elf grub_iso/boot/
	grub-mkrescue -o embodios.iso grub_iso 2>/dev/null || echo "grub-mkrescue not available"
endif

# Unit tests (compiled with host compiler for testing)
test-modbus: test/test_modbus.c
	@echo "Building and running Modbus unit tests..."
	gcc -o test/test_modbus test/test_modbus.c -I include -Wall -Wextra
	./test/test_modbus
	@rm -f test/test_modbus

test-pmm: test/test_pmm.c
	@echo "Building and running PMM unit tests..."
	gcc -o test/test_pmm test/test_pmm.c -I include -Wall -Wextra
	./test/test_pmm
	@rm -f test/test_pmm

test-slab: test/test_slab.c
	@echo "Building and running Slab unit tests..."
	gcc -o test/test_slab test/test_slab.c -I include -Wall -Wextra
	./test/test_slab
	@rm -f test/test_slab

test-ethercat: test/test_ethercat.c
	@echo "Building and running EtherCAT unit tests..."
	gcc -o test/test_ethercat test/test_ethercat.c -I include -Wall -Wextra
	./test/test_ethercat
	@rm -f test/test_ethercat

.PHONY: all clean iso iso-prod iso-full iso-quick test coverage sign sign-check test-modbus test-pmm test-slab test-ethercat

# ----------------------------------------------------------------------------
# UEFI direct boot section (WS-E, v0.6.0 "Volta") — BOOTX64.EFI + esp.img
#
# Builds a GRUB-free UEFI boot path: the loader is compiled freestanding
# (ms_abi UEFI calls, no PIC, large code model so every absolute reference
# is R_X86_64_64), linked flat at ImageBase 0x400000 with relocations
# preserved, converted to PE32+ by ../tools/mkuefi.py (the sandbox objcopy
# has no efi-app-x86_64 target), and packed into a FAT16 ESP image by
# ../tools/mkesp.py (no mtools/mkfs.vfat in the sandbox).
# ----------------------------------------------------------------------------
ifeq ($(ARCH),x86_64)
UEFI_LOADER = $(ARCH_DIR)/uefi_loader
UEFI_CFLAGS = -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
    -fshort-wchar -mno-red-zone -maccumulate-outgoing-args -fno-pic \
    -mcmodel=large -O2 -Wall -Wextra

$(UEFI_LOADER).o: $(UEFI_LOADER).c
	$(CC) $(UEFI_CFLAGS) -c $< -o $@

$(UEFI_LOADER).elf: $(UEFI_LOADER).o $(ARCH_DIR)/uefi_loader.ld
	$(LD) -m elf_x86_64 -nostdlib --emit-relocs -T $(ARCH_DIR)/uefi_loader.ld -o $@ $(UEFI_LOADER).o

BOOTX64.EFI: $(UEFI_LOADER).elf ../tools/mkuefi.py
	python3 ../tools/mkuefi.py $< $@

# FAT ESP image: EFI/BOOT/BOOTX64.EFI (firmware fallback path) + embodios.elf
esp.img: BOOTX64.EFI embodios.elf ../tools/mkesp.py
	python3 ../tools/mkesp.py $@ BOOTX64.EFI:EFI/BOOT/BOOTX64.EFI embodios.elf:embodios.elf

uefi: esp.img
	@ls -lh esp.img BOOTX64.EFI

.PHONY: uefi
endif
