/* x86_64 interrupt handlers */
.section .text

.global isr_common_stub
.extern interrupt_handler

/* Common ISR stub */
isr_common_stub:
    /* Save all registers */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Call C interrupt handler */
    movq %rsp, %rdi
    call interrupt_handler

    /* Restore registers */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    /* Clean up error code and interrupt number */
    addq $16, %rsp

    /* Return from interrupt */
    iretq

/* ISR macros */
.macro ISR_NOERRCODE num
.global isr\num
isr\num:
    pushq $0            /* Dummy error code */
    pushq $\num         /* Interrupt number */
    jmp isr_common_stub
.endm

.macro ISR_ERRCODE num
.global isr\num
isr\num:
    pushq $\num         /* Interrupt number */
    jmp isr_common_stub
.endm

/* CPU exceptions */
ISR_NOERRCODE 0    /* Divide by zero */
ISR_NOERRCODE 1    /* Debug */
ISR_NOERRCODE 2    /* NMI */
ISR_NOERRCODE 3    /* Breakpoint */
ISR_NOERRCODE 4    /* Overflow */
ISR_NOERRCODE 5    /* Bound range exceeded */
ISR_NOERRCODE 6    /* Invalid opcode */
ISR_NOERRCODE 7    /* Device not available */
ISR_ERRCODE   8    /* Double fault */
ISR_NOERRCODE 9    /* Coprocessor segment overrun */
ISR_ERRCODE   10   /* Invalid TSS */
ISR_ERRCODE   11   /* Segment not present */
ISR_ERRCODE   12   /* Stack segment fault */
ISR_ERRCODE   13   /* General protection fault */
ISR_ERRCODE   14   /* Page fault */
ISR_NOERRCODE 15   /* Reserved */
ISR_NOERRCODE 16   /* x87 FPU error */
ISR_ERRCODE   17   /* Alignment check */
ISR_NOERRCODE 18   /* Machine check */
ISR_NOERRCODE 19   /* SIMD floating point */
ISR_NOERRCODE 20   /* Virtualization */
ISR_NOERRCODE 21   /* Reserved */
ISR_NOERRCODE 22   /* Reserved */
ISR_NOERRCODE 23   /* Reserved */
ISR_NOERRCODE 24   /* Reserved */
ISR_NOERRCODE 25   /* Reserved */
ISR_NOERRCODE 26   /* Reserved */
ISR_NOERRCODE 27   /* Reserved */
ISR_NOERRCODE 28   /* Reserved */
ISR_NOERRCODE 29   /* Reserved */
ISR_NOERRCODE 30   /* Reserved */
ISR_NOERRCODE 31   /* Reserved */

/* IRQs */
ISR_NOERRCODE 32   /* IRQ0 - Timer */
ISR_NOERRCODE 33   /* IRQ1 - Keyboard */
ISR_NOERRCODE 34   /* IRQ2 */
ISR_NOERRCODE 35   /* IRQ3 */
ISR_NOERRCODE 36   /* IRQ4 */
ISR_NOERRCODE 37   /* IRQ5 */
ISR_NOERRCODE 38   /* IRQ6 */
ISR_NOERRCODE 39   /* IRQ7 */
ISR_NOERRCODE 40   /* IRQ8 */
ISR_NOERRCODE 41   /* IRQ9 */
ISR_NOERRCODE 42   /* IRQ10 */
ISR_NOERRCODE 43   /* IRQ11 */
ISR_NOERRCODE 44   /* IRQ12 */
ISR_NOERRCODE 45   /* IRQ13 */
ISR_NOERRCODE 46   /* IRQ14 */
ISR_NOERRCODE 47   /* IRQ15 */
