# EMBODIOS Kernel Makefile

# Architecture detection
ARCH ?= x86_64
ARCH_DIR = arch/$(ARCH)

# Tools
CC ?= gcc
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
        CC ?= $(CROSS_PREFIX)gcc
        AS ?= $(CROSS_PREFIX)as
        LD ?= $(CROSS_PREFIX)ld
        OBJCOPY ?= $(CROSS_PREFIX)objcopy
    endif
endif

# Common flags for all architectures
CFLAGS = -ffreestanding -nostdlib -nostdinc -fno-builtin -fno-stack-protector
CFLAGS += -fno-pic -Wall -Wextra -O2
CFLAGS += -I include -I include/arch/$(ARCH)

# Architecture-specific flags
ifeq ($(ARCH),x86_64)
    CFLAGS += -mno-red-zone -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-3dnow
    # Check if we're on Linux or macOS
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
        ASFLAGS = --64
        LDFLAGS = -m elf_x86_64
    else
        # macOS flags
        ASFLAGS = -arch x86_64
        LDFLAGS = -arch x86_64
    endif
endif

ifeq ($(ARCH),aarch64)
    # ARM64-specific flags
    ifeq ($(BUILD_OS),Darwin)
        # macOS ARM64 flags
        CFLAGS += -target arm64-apple-macos11 -arch arm64
        ASFLAGS = -arch arm64
        LDFLAGS = -arch arm64
    else
        # Linux ARM64 flags
        CFLAGS += -march=armv8-a -mgeneral-regs-only
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
    mm/pmm.c \
    mm/vmm.c \
    mm/slab.c \
    lib/string.c \
    lib/stdlib.c

# Architecture-specific sources
ifeq ($(ARCH),x86_64)
    ARCH_AS_SOURCES = $(ARCH_DIR)/boot.S
    ARCH_C_SOURCES = \
        $(ARCH_DIR)/cpu.c \
        $(ARCH_DIR)/vga.c \
        $(ARCH_DIR)/early_init.c
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
ARCH_AS_OBJS = $(ARCH_AS_SOURCES:.S=.o)
ARCH_C_OBJS = $(ARCH_C_SOURCES:.c=.o)

ALL_OBJS = $(ARCH_AS_OBJS) $(ARCH_C_OBJS) $(KERNEL_OBJS)

# Targets
all: embodios.elf embodios.bin

embodios.elf: $(ALL_OBJS)
	$(LD) $(LDFLAGS) -T $(ARCH_DIR)/kernel.ld -o $@ $(ALL_OBJS)

embodios.bin: embodios.elf
	$(OBJCOPY) -O binary $< $@

# Pattern rules
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(AS) $(ASFLAGS) $< -o $@

# Clean
clean:
	rm -f $(ALL_OBJS) embodios.elf embodios.bin

# ISO for x86_64
ifeq ($(ARCH),x86_64)
iso: embodios.elf
	mkdir -p iso/boot/grub
	cp embodios.elf iso/boot/
	echo 'menuentry "EMBODIOS" { multiboot2 /boot/embodios.elf }' > iso/boot/grub/grub.cfg
	grub-mkrescue -o embodios.iso iso 2>/dev/null || echo "grub-mkrescue not available"
endif

.PHONY: all clean iso