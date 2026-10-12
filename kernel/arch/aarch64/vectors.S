/* EMBODIOS ARM64 Exception Vectors
 *
 * 16-entry vector table for EL1 (installed via VBAR_EL1 in early_init.c):
 *   group 0: current EL, SP0   (sync/irq/fiq/serr)
 *   group 1: current EL, SPx   (sync/irq/fiq/serr)
 *   group 2: lower EL, AArch64 (sync/irq/fiq/serr)
 *   group 3: lower EL, AArch32 (sync/irq/fiq/serr)
 *
 * Each vector slot must fit in 0x80 bytes, so slots hold a single branch
 * to an out-of-line entry stub. IRQ entries save the full GP register
 * frame and call the C dispatcher (irq_dispatch in gic.c). Synchronous/
 * SError/FIQ entries dump the frame via the PL011 UART and halt
 * (no recovery in v0.8.0).
 *
 * For QEMU virt (GIC-400, EL1 with SPx).
 */

.section .text.vectors

/* Register frame: 272 bytes
 *   [sp+  0.. 15] x0/x1
 *   ...
 *   [sp+224..239] x28/x29
 *   [sp+240]      x30
 *   [sp+248]      elr_el1
 *   [sp+256]      spsr_el1
 *   [sp+264]      esr_el1
 */
.macro SAVE_REGS
    sub  sp, sp, #272
    stp  x0,  x1,  [sp, #0]
    stp  x2,  x3,  [sp, #16]
    stp  x4,  x5,  [sp, #32]
    stp  x6,  x7,  [sp, #48]
    stp  x8,  x9,  [sp, #64]
    stp  x10, x11, [sp, #80]
    stp  x12, x13, [sp, #96]
    stp  x14, x15, [sp, #112]
    stp  x16, x17, [sp, #128]
    stp  x18, x19, [sp, #144]
    stp  x20, x21, [sp, #160]
    stp  x22, x23, [sp, #176]
    stp  x24, x25, [sp, #192]
    stp  x26, x27, [sp, #208]
    stp  x28, x29, [sp, #224]
    str  x30, [sp, #240]
    mrs  x9,  elr_el1
    mrs  x10, spsr_el1
    mrs  x11, esr_el1
    stp  x9,  x10, [sp, #248]
    str  x11, [sp, #264]
.endm

.macro RESTORE_REGS
    ldp  x9,  x10, [sp, #248]
    msr  elr_el1, x9
    msr  spsr_el1, x10
    ldp  x0,  x1,  [sp, #0]
    ldp  x2,  x3,  [sp, #16]
    ldp  x4,  x5,  [sp, #32]
    ldp  x6,  x7,  [sp, #48]
    ldp  x8,  x9,  [sp, #64]
    ldp  x10, x11, [sp, #80]
    ldp  x12, x13, [sp, #96]
    ldp  x14, x15, [sp, #112]
    ldp  x16, x17, [sp, #128]
    ldp  x18, x19, [sp, #144]
    ldp  x20, x21, [sp, #160]
    ldp  x22, x23, [sp, #176]
    ldp  x24, x25, [sp, #192]
    ldp  x26, x27, [sp, #208]
    ldp  x28, x29, [sp, #224]
    ldr  x30, [sp, #240]
    add  sp, sp, #272
.endm

/* Handlers: C functions in gic.c
 *   void irq_dispatch(uint64_t *frame)
 *   void sync_exception_handler(uint64_t *frame, uint64_t which)  [noreturn]
 *   void fiq_handler(uint64_t *frame)                             [noreturn]
 *   void serr_handler(uint64_t *frame)                            [noreturn]
 */

.balign 2048          /* VBAR_EL1 requires 2KB alignment */
.global exception_vectors
exception_vectors:

/* ---- Current EL, SP0 ---- */
.balign 128
    b vec_cur_sp0_sync
.balign 128
    b vec_cur_sp0_irq
.balign 128
    b vec_cur_sp0_fiq
.balign 128
    b vec_cur_sp0_serr

/* ---- Current EL, SPx ---- */
.balign 128
    b vec_cur_spx_sync
.balign 128
    b vec_cur_spx_irq
.balign 128
    b vec_cur_spx_fiq
.balign 128
    b vec_cur_spx_serr

/* ---- Lower EL, AArch64 ---- */
.balign 128
    b vec_low64_sync
.balign 128
    b vec_low64_irq
.balign 128
    b vec_low64_fiq
.balign 128
    b vec_low64_serr

/* ---- Lower EL, AArch32 ---- */
.balign 128
    b vec_low32_sync
.balign 128
    b vec_low32_irq
.balign 128
    b vec_low32_fiq
.balign 128
    b vec_low32_serr

/* ---- Out-of-line entry stubs ---- */

vec_cur_sp0_sync:
    SAVE_REGS
    mov  x0, sp
    mov  x1, #0
    bl   sync_exception_handler
    b    .

vec_cur_sp0_irq:
    SAVE_REGS
    mov  x0, sp
    bl   irq_dispatch
    RESTORE_REGS
    eret

vec_cur_sp0_fiq:
    SAVE_REGS
    mov  x0, sp
    bl   fiq_handler
    b    .

vec_cur_sp0_serr:
    SAVE_REGS
    mov  x0, sp
    bl   serr_handler
    b    .

vec_cur_spx_sync:
    SAVE_REGS
    mov  x0, sp
    mov  x1, #1
    bl   sync_exception_handler
    b    .

vec_cur_spx_irq:
    SAVE_REGS
    mov  x0, sp
    bl   irq_dispatch
    RESTORE_REGS
    eret

vec_cur_spx_fiq:
    SAVE_REGS
    mov  x0, sp
    bl   fiq_handler
    b    .

vec_cur_spx_serr:
    SAVE_REGS
    mov  x0, sp
    bl   serr_handler
    b    .

vec_low64_sync:
    SAVE_REGS
    mov  x0, sp
    mov  x1, #2
    bl   sync_exception_handler
    b    .

vec_low64_irq:
    SAVE_REGS
    mov  x0, sp
    bl   irq_dispatch
    RESTORE_REGS
    eret

vec_low64_fiq:
    SAVE_REGS
    mov  x0, sp
    bl   fiq_handler
    b    .

vec_low64_serr:
    SAVE_REGS
    mov  x0, sp
    bl   serr_handler
    b    .

vec_low32_sync:
    SAVE_REGS
    mov  x0, sp
    mov  x1, #3
    bl   sync_exception_handler
    b    .

vec_low32_irq:
    SAVE_REGS
    mov  x0, sp
    bl   irq_dispatch
    RESTORE_REGS
    eret

vec_low32_fiq:
    SAVE_REGS
    mov  x0, sp
    bl   fiq_handler
    b    .

vec_low32_serr:
    SAVE_REGS
    mov  x0, sp
    bl   serr_handler
    b    .

.section .note.GNU-stack,"",@progbits
