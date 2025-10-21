# EMBODIOS Kernel Makefile

# Architecture detection
ARCH ?= x86_64
ARCH_DIR = arch/$(ARCH)

# Tools
CC ?= gcc
CXX ?= g++
AS ?= as
LD ?= ld
OBJCOPY ?= objcopy

# Detect build OS early
BUILD_OS := $(shell uname -s)

# Cross-compilation prefix for ARM64
ifeq ($(ARCH),aarch64)
    ifeq ($(BUILD_OS),Darwin)
        # macOS ARM64 - use native tools
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

# Common flags for all architectures
CFLAGS = -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
CFLAGS += -fno-pic -Wall -Wextra -O3
CFLAGS += -I include -I include/arch/$(ARCH)
CFLAGS += -I llama_cpp/include -I llama_cpp/ggml
# Allow compiler builtin headers for GGML (needs stdbool.h, stdint.h, etc.)
CFLAGS += -isystem $(shell $(CC) -print-file-name=include)

# ARM NEON optimizations
ifeq ($(ARCH),aarch64)
    CFLAGS += -march=armv8-a+simd -mtune=cortex-a57 -ffast-math
endif

# C++ flags for llama.cpp - needs standard headers but not stdlib
# Start from scratch instead of inheriting CFLAGS to allow standard headers
CXXFLAGS = -ffreestanding -nostdlib -fno-builtin -fno-stack-protector
CXXFLAGS += -fno-pic -Wall -Wextra -O2
CXXFLAGS += -I include -I include/arch/$(ARCH)
CXXFLAGS += -fno-exceptions -fno-rtti -std=c++17
CXXFLAGS += -I llama_cpp/include -I llama_cpp/ggml -I llama_cpp/src

# Architecture-specific flags
ifeq ($(ARCH),x86_64)
    CFLAGS += -mno-red-zone -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-3dnow
    # Check if we're on Linux or macOS
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
        ASFLAGS = -m64
        LDFLAGS = -m elf_x86_64
    else
        # macOS flags
        ASFLAGS = -arch x86_64
        LDFLAGS = -arch x86_64
    endif
endif

ifeq ($(ARCH),aarch64)
    # ARM64-specific flags
    ifeq ($(CC),aarch64-elf-gcc)
        # Cross-compiler flags
        CFLAGS += -march=armv8-a
        CXXFLAGS += -march=armv8-a
        ASFLAGS = -march=armv8-a
        LDFLAGS = -m aarch64elf
    else ifeq ($(BUILD_OS),Darwin)
        # macOS ARM64 flags
        CFLAGS += -target arm64-apple-macos11 -arch arm64
        CXXFLAGS += -target arm64-apple-macos11 -arch arm64
        ASFLAGS = -arch arm64
        LDFLAGS = -arch arm64 -static -e _start
    else
        # Linux ARM64 flags - no FP/SIMD for soft-float compatibility
        CFLAGS += -march=armv8-a -mgeneral-regs-only -mstrict-align
        CXXFLAGS += -march=armv8-a -mgeneral-regs-only -mstrict-align
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
    mm/pmm.c \
    mm/vmm.c \
    mm/slab.c \
    mm/heap.c \
    ai/model_runtime.c \
    ai/tvm_runtime.c \
    ai/embodios_format.c \
    ai/tokenizer.c \
    ai/gguf_integer_loader.c \
    ai/quantized_ops.c \
    ai/quantized_inference.c \
    ai/tinyllama_integer_inference.c \
    ai/tinystories_inference.c \
    ai/simd_ops.c \
    ai/kv_cache.c \
    ai/memory_opt.c \
    ai/quantized_matmul_simd.c \
    lib/string.c \
    lib/stdlib.c

# Temporarily disabled - contain floating-point code or depend on removed modules
# lib/math.c - has floating-point functions (sqrtf, expf, logf, etc.)
# ai/tvm_graph_executor.c \
# ai/tvm_tinyllama.c \
# ai/tensor_ops.c \
# ai/transformer_full.c \
# ai/gguf_loader.c \
# ai/model_loader.c \
# ai/simple_llm.c \
# ai/tinyllama_gguf_inference.c \
# ai/tvm_model_loader.c \
# ai/working_model.c \
# ai/real_inference.c \

# LLaMA.cpp integration - DISABLED for now (too complex for bare-metal)
# Using simple built-in inference instead
LLAMA_CPP_SOURCES =
GGML_C_SOURCES =

# Architecture-specific sources
ifeq ($(ARCH),x86_64)
    ARCH_AS_SOURCES = $(ARCH_DIR)/boot.S $(ARCH_DIR)/interrupt.S
    ARCH_C_SOURCES = \
        $(ARCH_DIR)/cpu.c \
        $(ARCH_DIR)/vga.c \
        $(ARCH_DIR)/early_init.c \
        $(ARCH_DIR)/idt.c \
        $(ARCH_DIR)/paging.c
endif

ifeq ($(ARCH),aarch64)
    ifeq ($(BUILD_OS),Darwin)
        ARCH_AS_SOURCES = $(ARCH_DIR)/boot.S
    else
        ARCH_AS_SOURCES = $(ARCH_DIR)/boot_linux.S
    endif
    ARCH_C_SOURCES = \
        $(ARCH_DIR)/cpu.c \
        $(ARCH_DIR)/uart.c \
        $(ARCH_DIR)/early_init.c
endif

# Object files
KERNEL_OBJS = $(KERNEL_C_SOURCES:.c=.o)
LLAMA_CPP_OBJS = $(LLAMA_CPP_SOURCES:.cpp=.o)
GGML_C_OBJS = $(GGML_C_SOURCES:.c=.o)
ARCH_AS_OBJS = $(ARCH_AS_SOURCES:.S=.o)
ARCH_C_OBJS = $(ARCH_C_SOURCES:.c=.o)

# Model embedding support
# NOTE: Model embedding disabled on macOS (objcopy not available)
# TinyStories model would be embedded here in Linux build
MODEL_OBJS =

ALL_OBJS = $(ARCH_AS_OBJS) $(ARCH_C_OBJS) $(KERNEL_OBJS) $(LLAMA_CPP_OBJS) $(GGML_C_OBJS) $(MODEL_OBJS)

# Targets
all: embodios.elf embodios.bin

embodios.elf: $(ALL_OBJS)
ifeq ($(BUILD_OS),Darwin)
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS)
else
	$(LD) $(LDFLAGS) -T $(ARCH_DIR)/kernel.ld -o $@ $(ALL_OBJS)
endif

embodios.bin: embodios.elf
	$(OBJCOPY) -O binary $< $@

# Pattern rules
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(ASFLAGS) -c $< -o $@

# Clean
clean:
	rm -f $(ALL_OBJS) embodios.elf embodios.bin
	
# Embed TinyStories model as binary object
ai/tinystories_model.o: $(TINYSTORIES_MODEL)
	@echo "Embedding TinyStories-15M model into kernel..."
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		--redefine-sym _binary___models_tinystories_15m_bin_start=_binary_tinystories_15m_bin_start \
		--redefine-sym _binary___models_tinystories_15m_bin_end=_binary_tinystories_15m_bin_end \
		$< $@

# ISO for x86_64
ifeq ($(ARCH),x86_64)
iso: embodios.elf
	mkdir -p iso/boot/grub
	cp embodios.elf iso/boot/
	echo 'menuentry "EMBODIOS" { multiboot2 /boot/embodios.elf }' > iso/boot/grub/grub.cfg
	grub-mkrescue -o embodios.iso iso 2>/dev/null || echo "grub-mkrescue not available"
endif

.PHONY: all clean iso