/* EMBODIOS ARM64 Boot Assembly
 * Minimal boot stub for ARM64 architecture
 */

.text
.align 4
.global _start

_start:
    /* Set up initial stack */
    ldr x0, =stack_top
    mov sp, x0
    
    /* Early UART test */
    bl _uart_early_test
    
    /* Clear BSS section */
    ldr x0, =_bss_start
    ldr x1, =_bss_end
1:
    cmp x0, x1
    b.ge 2f
    str xzr, [x0], #8
    b 1b
2:
    
    /* Call kernel main */
    bl _kernel_main
    
    /* Hang if kernel returns */
3:
    wfe
    b 3b

.data
.align 4
stack_bottom:
    .space 0x4000    /* 16KB stack */
stack_top:

/* Provide BSS symbols for macOS build */
.global _bss_start
.global _bss_end
_bss_start:
    .quad 0
_bss_end:
    .quad 0
