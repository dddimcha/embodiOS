/* EMBODIOS ARM64 MMU Setup
 *
 * Identity map, EL1, 4KB granule, T0SZ=25 (39-bit VA, 512GB), IPS=40-bit.
 * Walk starts at L1 (each L1 entry = 1GB block or pointer to an L2 table of
 * 512 x 2MB blocks).
 *
 * Per-board device-memory windows (AttrIdx0 = Device-nGnRnE, XN):
 *
 *   virt: L1[0] -> L2, all 512 entries device (0x00000000-0x3FFFFFFF:
 *         UART 0x09000000, GIC 0x08000000). L1[1..3] normal 1GB blocks.
 *         Byte-identical behavior to v0.8.0.
 *
 *   pi3 : kernel runs at 0x80000, so RAM must be normal, not device.
 *         L1[0] -> L2a: 0x00000000-0x3EFFFFFF normal (504 x 2MB),
 *                       0x3F000000-0x3FFFFFFF device (8 x 2MB: peripherals,
 *                       UART0 0x3F201000, mailbox 0x3F00B880).
 *         L1[1] -> L2b: 0x40000000-0x401FFFFF device (GIC-400 0x40001000/
 *                       0x40002000), 0x40200000-0x7FFFFFFF normal.
 *         L1[2..3] normal 1GB blocks.
 *
 *   pi4 : L1[0..2] normal 1GB blocks (kernel at 0x80000).
 *         L1[3] -> L2: 0xC0000000-0xFBFFFFFF normal (480 x 2MB),
 *                      0xFC000000-0xFFFFFFFF device (32 x 2MB: peripherals
 *                      0xFE000000, GIC-400 0xFF840000, local 0xFF800000).
 *
 *   pi5 : L1[0..3] normal 1GB blocks (kernel at 0x80000, RAM 0-4GB).
 *         L1[65] -> L2: 0x107C000000-0x107FFFFFFF device (32 x 2MB = 64MB
 *                       soc/RP1 window: UART0 0x107D001000, GIC-400
 *                       0x107FFF9000, mailbox 0x107C013880), rest invalid.
 */

#include "board.h"

.section .bss
.align 12   /* 4KB aligned for page tables */

/* Page tables - with T0SZ=25 (39-bit VA), walk starts at L1 */
l1_table:   .space 4096
l2_table:   .space 4096
#if defined(BOARD_PI3)
l2_table2:  .space 4096
#endif

.section .data
.align 4
.global mmu_enabled
mmu_enabled:
    .byte 0

.section .text

.global mmu_init

/* Descriptor attribute templates (ORed with the block base address):
 *   DEVICE: Block(01) | AttrIdx0 (nGnRnE) | AF | UXN | PXN
 *   NORMAL: Block(01) | AttrIdx1 (WB)     | AF | SH-inner
 * The existing code uses 0x0401|bits[53:54] and 0x0705; keep identical. */
#define DESC_DEVICE_FLAGS   0x0401          /* Block | AttrIdx=0 | AF */
#define DESC_NORMAL_FLAGS   0x0705          /* Block | AttrIdx=1 | SH | AF */
#define DESC_XN             (3 << 52)       /* UXN | PXN */

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

#if defined(BOARD_PI3)
    /* ---- pi3 ---- */

    adrp x13, l2_table2
    add x13, x13, :lo12:l2_table2

    /* Zero L2b */
    mov x0, x13
    mov x1, #512
3:  str xzr, [x0], #8
    subs x1, x1, #1
    b.ne 3b

    /* L1[0] -> L2a */
    mov x0, x12
    orr x0, x0, #3          /* Valid | Table */
    str x0, [x11]

    /* L1[1] -> L2b */
    mov x0, x13
    orr x0, x0, #3
    str x0, [x11, #8]

    /* L1[2],[3]: normal 1GB blocks */
    mov x0, #0x80000000
    mov x1, #DESC_NORMAL_FLAGS
    orr x0, x0, x1
    str x0, [x11, #16]
    mov x0, #0xC0000000
    mov x1, #DESC_NORMAL_FLAGS
    orr x0, x0, x1
    str x0, [x11, #24]

    /* L2a entries 0..503: 0x00000000-0x3EFFFFFF normal RAM (kernel @0x80000) */
    mov x0, #0
    mov x2, x12
    mov x3, #504
4:  mov x1, x0
    mov x4, #DESC_NORMAL_FLAGS
    orr x1, x1, x4
    str x1, [x2], #8
    add x0, x0, #0x200000
    subs x3, x3, #1
    b.ne 4b

    /* L2a entries 504..511: 0x3F000000-0x3FFFFFFF device (peripherals) */
    mov x3, #8
5:  mov x1, x0
    mov x4, #DESC_DEVICE_FLAGS
    orr x1, x1, x4
    mov x4, #DESC_XN
    orr x1, x1, x4
    str x1, [x2], #8
    add x0, x0, #0x200000
    subs x3, x3, #1
    b.ne 5b

    /* L2b entry 0: 0x40000000-0x401FFFFF device (GIC-400) */
    mov x1, #0x40000000
    mov x4, #DESC_DEVICE_FLAGS
    orr x1, x1, x4
    mov x4, #DESC_XN
    orr x1, x1, x4
    str x1, [x13], #8

    /* L2b entries 1..511: 0x40200000-0x7FFFFFFF normal */
    mov x0, #0x40200000
    mov x3, #511
6:  mov x1, x0
    mov x4, #DESC_NORMAL_FLAGS
    orr x1, x1, x4
    str x1, [x13], #8
    add x0, x0, #0x200000
    subs x3, x3, #1
    b.ne 6b

#elif defined(BOARD_PI4)
    /* ---- pi4 ---- */

    /* L1[0..2]: normal 1GB blocks (RAM incl. kernel @0x80000) */
    mov x0, #0x0
    mov x1, #DESC_NORMAL_FLAGS
    orr x0, x0, x1
    str x0, [x11]
    mov x0, #0x40000000
    mov x1, #DESC_NORMAL_FLAGS
    orr x0, x0, x1
    str x0, [x11, #8]
    mov x0, #0x80000000
    mov x1, #DESC_NORMAL_FLAGS
    orr x0, x0, x1
    str x0, [x11, #16]

    /* L1[3] -> L2 */
    mov x0, x12
    orr x0, x0, #3
    str x0, [x11, #24]

    /* L2 entries 0..479: 0xC0000000-0xFBFFFFFF normal */
    mov x0, #0xC0000000
    mov x2, x12
    mov x3, #480
4:  mov x1, x0
    mov x4, #DESC_NORMAL_FLAGS
    orr x1, x1, x4
    str x1, [x2], #8
    add x0, x0, #0x200000
    subs x3, x3, #1
    b.ne 4b

    /* L2 entries 480..511: 0xFC000000-0xFFFFFFFF device
     * (peripherals 0xFE000000, GIC-400 0xFF840000, local 0xFF800000) */
    mov x3, #32
5:  mov x1, x0
    mov x4, #DESC_DEVICE_FLAGS
    orr x1, x1, x4
    mov x4, #DESC_XN
    orr x1, x1, x4
    str x1, [x2], #8
    add x0, x0, #0x200000
    subs x3, x3, #1
    b.ne 5b

#elif defined(BOARD_PI5)
    /* ---- pi5 ---- */

    /* L1[0..3]: normal 1GB blocks (RAM 0-4GB, kernel @0x80000) */
    mov x0, #0x0
    mov x1, #DESC_NORMAL_FLAGS
    orr x0, x0, x1
    str x0, [x11]
    mov x0, #0x40000000
    mov x1, #DESC_NORMAL_FLAGS
    orr x0, x0, x1
    str x0, [x11, #8]
    mov x0, #0x80000000
    mov x1, #DESC_NORMAL_FLAGS
    orr x0, x0, x1
    str x0, [x11, #16]
    mov x0, #0xC0000000
    mov x1, #DESC_NORMAL_FLAGS
    orr x0, x0, x1
    str x0, [x11, #24]

    /* L1[65] -> L2 (covers 0x1040000000-0x107FFFFFFF) */
    mov x0, x12
    orr x0, x0, #3
    str x0, [x11, #(65 * 8)]

    /* L2 entries 480..511: 0x107C000000-0x107FFFFFFF device (64MB soc/RP1
     * window: UART0 0x107D001000, GIC 0x107FFF9000, mailbox 0x107C013880).
     * Entries 0..479 stay invalid (unused DRAM alias space). */
    mov x0, #0x7C000000
    mov x1, #(0x10 << 28)   /* 0x100000000 */
    orr x0, x0, x1          /* 0x107C000000 */
    mov x2, x12
    add x2, x2, #(480 * 8)
    mov x3, #32
4:  mov x1, x0
    mov x4, #DESC_DEVICE_FLAGS
    orr x1, x1, x4
    mov x4, #DESC_XN
    orr x1, x1, x4
    str x1, [x2], #8
    add x0, x0, #0x200000
    subs x3, x3, #1
    b.ne 4b

#else
    /* ---- virt (default; byte-identical to v0.8.0) ---- */

    /* L1[0] -> L2 table for 0x00000000-0x3FFFFFFF (device memory) */
    mov x0, x12
    orr x0, x0, #3          /* Valid | Table */
    str x0, [x11]

    /* L1[1]: 1GB block at 0x40000000 - Normal memory */
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
#endif

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
