/* EMBODIOS ARM64 Boot Assembly
 * Minimal boot stub for ARM64 architecture
 * For QEMU virt machine
 */

.section .text.boot
.align 4
.global _start

_start:
    /* Mask all exceptions */
    msr daifset, #0xf

    /* Set up initial stack (16KB) */
    adrp x0, stack_top
    add x0, x0, :lo12:stack_top
    mov sp, x0

    /* Clear BSS section FIRST (before MMU, since page tables are in BSS) */
    adrp x0, __bss_start
    add x0, x0, :lo12:__bss_start
    adrp x1, __bss_end
    add x1, x1, :lo12:__bss_end
bss_clear:
    cmp x0, x1
    b.ge bss_done
    str xzr, [x0], #8
    b bss_clear
bss_done:

    /* Enable MMU FIRST for HVF compatibility (before any device access) */
    bl mmu_init

    /* Now safe to access UART with proper memory attributes */
    bl uart_early_test

    /* Call kernel main - no underscore prefix (ELF convention) */
    bl kernel_main

    /* Hang if kernel returns */
3:
    wfe
    b 3b

.section .data
.align 4
stack_bottom:
    .space 0x4000    /* 16KB stack */
stack_top:

.section .bss
.align 4
.global __bss_start
.global __bss_end
__bss_start:
__bss_end:
