cmake_minimum_required(VERSION 3.16)
project(embodios_kernel LANGUAGES C ASM)

# Detect architecture
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(ARCH "x86_64")
    set(ARCH_DIR "arch/x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(ARCH "arm64")
    set(ARCH_DIR "arch/arm64")
else()
    message(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffreestanding -nostdlib -nostdinc -fno-builtin")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-stack-protector -fno-pic -mno-red-zone")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -O2")

if(ARCH STREQUAL "x86_64")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-3dnow")
    set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -felf64")
elseif(ARCH STREQUAL "arm64")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mgeneral-regs-only")
endif()

# Include directories
include_directories(include)
include_directories(include/arch/${ARCH})

# Source files
set(KERNEL_SOURCES
    core/kernel.c
    core/console.c
    core/panic.c
    mm/pmm.c
    mm/vmm.c
    mm/slab.c
    lib/string.c
    lib/stdlib.c
)

# Architecture-specific sources
if(ARCH STREQUAL "x86_64")
    list(APPEND KERNEL_SOURCES
        ${ARCH_DIR}/boot.S
        ${ARCH_DIR}/cpu.c
        ${ARCH_DIR}/interrupts.c
        ${ARCH_DIR}/apic.c
        ${ARCH_DIR}/gdt.c
        ${ARCH_DIR}/idt.c
    )
elseif(ARCH STREQUAL "arm64")
    list(APPEND KERNEL_SOURCES
        ${ARCH_DIR}/boot.S
        ${ARCH_DIR}/cpu.c
        ${ARCH_DIR}/interrupts.c
        ${ARCH_DIR}/gic.c
        ${ARCH_DIR}/mmu.c
    )
endif()

# Model runtime (from compiler output)
if(EXISTS "${CMAKE_SOURCE_DIR}/../build/model_weights.S")
    list(APPEND KERNEL_SOURCES "${CMAKE_SOURCE_DIR}/../build/model_weights.S")
endif()

# Create kernel executable
add_executable(embodios.elf ${KERNEL_SOURCES})

# Linker script
set(LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/${ARCH_DIR}/kernel.ld")
set_target_properties(embodios.elf PROPERTIES LINK_FLAGS "-T ${LINKER_SCRIPT} -nostdlib")

# Post-build commands
add_custom_command(TARGET embodios.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary embodios.elf embodios.bin
    COMMENT "Creating raw kernel binary"
)

# Create bootable image
if(ARCH STREQUAL "x86_64")
    add_custom_target(iso
        COMMAND mkdir -p iso/boot/grub
        COMMAND cp embodios.elf iso/boot/
        COMMAND echo "menuentry \"EMBODIOS\" { multiboot2 /boot/embodios.elf }" > iso/boot/grub/grub.cfg
        COMMAND grub-mkrescue -o embodios.iso iso
        DEPENDS embodios.elf
        COMMENT "Creating bootable ISO image"
    )
endif()