/* EMBODIOS ARM64 MMU Setup
 * Minimal setup for HVF (macOS Hypervisor) compatibility
 * Maps device memory with correct attributes to avoid ISV assertion
 */

.section .bss
.align 12   /* 4KB aligned for page tables */

/* Page tables - with T0SZ=25 (39-bit VA), walk starts at L1 */
l1_table:   .space 4096
l2_table:   .space 4096

.section .data
.align 4
.global mmu_enabled
mmu_enabled:
    .byte 0

.section .text

.global mmu_init

/*
 * Minimal identity map for HVF (macOS Hypervisor):
 *   0x00000000-0x3FFFFFFF: Device memory (UART, GIC)
 *   0x40000000-0xFFFFFFFF: Normal memory (kernel, RAM)
 *
 * IMPORTANT: No UART access before MMU enable (causes HVF assertion)
 */
mmu_init:
    stp x29, x30, [sp, #-16]!
    mov x29, sp

    /* Load page table addresses (L0 unused with T0SZ=25) */
    adrp x11, l1_table
    add x11, x11, :lo12:l1_table
    adrp x12, l2_table
    add x12, x12, :lo12:l2_table

    /* Zero L1 table */
    mov x0, x11
    mov x1, #512
2:  str xzr, [x0], #8
    subs x1, x1, #1
    b.ne 2b

    /* Zero L2 table */
    mov x0, x12
    mov x1, #512
3:  str xzr, [x0], #8
    subs x1, x1, #1
    b.ne 3b

    /* L1[0] -> L2 table for 0x00000000-0x3FFFFFFF (device memory) */
    mov x0, x12
    orr x0, x0, #3          /* Valid | Table */
    str x0, [x11]

    /* L1[1]: 1GB block at 0x40000000 - Normal memory
     * Bits: Valid=1, Block=0, AttrIdx=1, AF=1, SH=Inner
     *   [1:0] = 01 (block)
     *   [4:2] = 001 (AttrIdx 1 = normal)
     *   [9:8] = 11 (SH inner)
     *   [10]  = 1 (AF)
     */
    mov x0, #0x40000000
    mov x1, #0x0705         /* Block | AttrIdx=1 | SH=inner | AF */
    orr x0, x0, x1
    str x0, [x11, #8]

    /* L1[2]: 1GB block at 0x80000000 - Normal memory */
    mov x0, #0x80000000
    mov x1, #0x0705
    orr x0, x0, x1
    str x0, [x11, #16]

    /* L1[3]: 1GB block at 0xC0000000 - Normal memory */
    mov x0, #0xC0000000
    mov x1, #0x0705
    orr x0, x0, x1
    str x0, [x11, #24]

    /* Fill L2 table: 512 x 2MB blocks for first 1GB
     * All as device memory (UART at 0x09000000)
     * Device: AttrIdx=0, AF=1, UXN, PXN
     */
    mov x0, #0              /* Start address */
    mov x2, x12             /* L2 table pointer */
    mov x3, #512            /* All 512 entries */

4:  mov x1, x0
    mov x4, #0x0401         /* Block | AttrIdx=0 | AF */
    orr x1, x1, x4
    /* Set UXN (bit 54) and PXN (bit 53) */
    mov x4, #3
    lsl x4, x4, #52         /* Bits 53-54 */
    orr x1, x1, x4
    str x1, [x2], #8
    add x0, x0, #0x200000   /* +2MB */
    subs x3, x3, #1
    b.ne 4b

    /* Set MAIR_EL1:
     * Attr0 = 0x00 (Device-nGnRnE)
     * Attr1 = 0xFF (Normal, Write-Back)
     */
    mov x0, #0xFF00
    msr mair_el1, x0

    /* Set TCR_EL1:
     * T0SZ = 25 (39-bit VA, 512GB)
     * IRGN0 = 1 (WB-WA)
     * ORGN0 = 1 (WB-WA)
     * SH0 = 3 (Inner shareable)
     * TG0 = 0 (4KB granule)
     * IPS = 2 (40-bit PA)
     * EPD1 = 1 (Disable TTBR1)
     */
    mov x0, #25             /* T0SZ */
    orr x0, x0, #(1 << 8)   /* IRGN0 */
    orr x0, x0, #(1 << 10)  /* ORGN0 */
    orr x0, x0, #(3 << 12)  /* SH0 */
    orr x0, x0, #(1 << 23)  /* EPD1 */
    mov x1, #2
    lsl x1, x1, #32         /* IPS at bits [34:32] */
    orr x0, x0, x1
    msr tcr_el1, x0

    /* Set TTBR0_EL1 to L1 table (with T0SZ=25, we start at L1) */
    adrp x0, l1_table
    add x0, x0, :lo12:l1_table
    msr ttbr0_el1, x0

    /* Invalidate TLB and ensure visibility */
    tlbi vmalle1
    dsb sy
    isb

    /* Enable MMU with caches */
    mrs x0, sctlr_el1
    orr x0, x0, #1          /* M - MMU enable */
    orr x0, x0, #(1 << 2)   /* C - Data cache */
    orr x0, x0, #(1 << 12)  /* I - Instruction cache */
    msr sctlr_el1, x0
    isb

    /* Mark as enabled */
    adrp x0, mmu_enabled
    add x0, x0, :lo12:mmu_enabled
    mov w1, #1
    strb w1, [x0]

    ldp x29, x30, [sp], #16
    ret
