/* x86_64 boot code for EMBODIOS */
.section .text
.global _start
.extern kernel_main

_start:
    /* Disable interrupts */
    cli

    /* Set up stack */
    movq $stack_top, %rsp

    /* Zero out BSS section */
    movq $__bss_start, %rdi
    movq $__bss_end, %rcx
    subq %rdi, %rcx
    xorb %al, %al
    rep stosb

    /* Call kernel main */
    call kernel_main

    /* Halt if kernel returns */
halt:
    hlt
    jmp halt

/* Stack space */
.section .bss
.align 16
stack_bottom:
    .skip 16384  /* 16KB stack */
stack_top:
