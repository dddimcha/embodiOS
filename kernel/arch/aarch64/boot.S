/* EMBODIOS ARM64 Boot Assembly
 *
 * Entry paths:
 *   - QEMU virt -kernel        : EL1 (or EL2 with -machine virtualization=on)
 *   - QEMU raspi3b -kernel     : EL1 (QEMU emulates the Pi armstub; x0 = DTB)
 *   - Real Pi firmware (start*.elf, arm_64bit=1): EL2, x0 = DTB pointer,
 *     kernel8.img loaded at 0x80000
 *
 * All paths converge on EL1h before the stack is set up. If we land at EL2
 * (real hardware) we configure HCR_EL2/CPTR_EL2/HSTR_EL2/CNTHCTL_EL2/
 * SCTLR_EL1/SPSR_EL2/ELR_EL2 and eret down to EL1h. The EL2 setup is never
 * exercised on QEMU raspi3b (QEMU enters at EL1 already) but is required on
 * real Pi 3/4/5 hardware.
 */

.section .text.boot
.align 4
.global _start

_start:
    /* Mask all exceptions */
    msr daifset, #0xf

    /* Save the DTB pointer (x0) before anything clobbers it.
     * g_dtb_ptr lives in .data so the later BSS clear does not wipe it. */
    adrp x9, g_dtb_ptr
    add  x9, x9, :lo12:g_dtb_ptr
    str  x0, [x9]

    /* Check the current exception level */
    mrs  x9, CurrentEL
    lsr  x9, x9, #2
    cmp  x9, #2
    b.ne 1f                 /* EL1 (QEMU) - nothing to drop */

    /* ---- EL2 -> EL1 transition (real Pi hardware) ---- */

    /* HCR_EL2.RW = 1: EL1 execution state is AArch64 */
    mov  x9, #(1 << 31)
    msr  hcr_el2, x9

    /* CPTR_EL2 = 0: do not trap FP/SIMD/ACLE accesses to EL2 */
    msr  cptr_el2, xzr

    /* HSTR_EL2 = 0: do not trap system register accesses to EL2 */
    msr  hstr_el2, xzr

    /* CNTHCTL_EL2: EL1 physical timer/counter access enable (no traps) */
    mov  x9, #3             /* EL1PCEN | EL1PCTEN */
    msr  cnthctl_el2, x9
    msr  cntvoff_el2, xzr

    /* SCTLR_EL1: architectural RES1 value, MMU/caches off at EL1 entry
     * (0x30C50830: RES1 bits, EE=0 little-endian, SPAN, no WXN/M/C/I) */
    mov  x9, #0x0830
    movk x9, #0x30C5, lsl #16
    msr  sctlr_el1, x9

    /* SPSR_EL2: return to EL1h (M[3:0]=0101) with DAIF masked (0x3C0) */
    mov  x9, #0x3C5
    msr  spsr_el2, x9

    /* ELR_EL2 = address of the EL1 continuation (physical; MMU off) */
    adr  x9, 1f
    msr  elr_el2, x9
    eret

1:  /* ---- Now at EL1h (SP_EL1) on every entry path ---- */

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
