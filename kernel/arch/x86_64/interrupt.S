/* x86_64 Interrupt Handlers */
.section .text

.global interrupt_stub_table
.extern interrupt_handler

/* Common interrupt stub - saves context and calls C handler */
.macro ISR_NOERRCODE num
.global isr\num
isr\num:
    cli
    pushq $0                /* Push dummy error code */
    pushq $\num             /* Push interrupt number */
    jmp isr_common_stub
.endm

.macro ISR_ERRCODE num
.global isr\num
isr\num:
    cli
    pushq $\num             /* Push interrupt number */
    jmp isr_common_stub
.endm

/* CPU Exception ISRs (0-31) */
ISR_NOERRCODE 0     /* Divide by zero */
ISR_NOERRCODE 1     /* Debug */
ISR_NOERRCODE 2     /* NMI */
ISR_NOERRCODE 3     /* Breakpoint */
ISR_NOERRCODE 4     /* Overflow */
ISR_NOERRCODE 5     /* Bound range exceeded */
ISR_NOERRCODE 6     /* Invalid opcode */
ISR_NOERRCODE 7     /* Device not available */
ISR_ERRCODE   8     /* Double fault */
ISR_NOERRCODE 9     /* Coprocessor segment overrun */
ISR_ERRCODE   10    /* Invalid TSS */
ISR_ERRCODE   11    /* Segment not present */
ISR_ERRCODE   12    /* Stack segment fault */
ISR_ERRCODE   13    /* General protection fault */
ISR_ERRCODE   14    /* Page fault */
ISR_NOERRCODE 15    /* Reserved */
ISR_NOERRCODE 16    /* x87 floating point exception */
ISR_ERRCODE   17    /* Alignment check */
ISR_NOERRCODE 18    /* Machine check */
ISR_NOERRCODE 19    /* SIMD floating point exception */
ISR_NOERRCODE 20    /* Virtualization exception */
ISR_NOERRCODE 21    /* Reserved */
ISR_NOERRCODE 22    /* Reserved */
ISR_NOERRCODE 23    /* Reserved */
ISR_NOERRCODE 24    /* Reserved */
ISR_NOERRCODE 25    /* Reserved */
ISR_NOERRCODE 26    /* Reserved */
ISR_NOERRCODE 27    /* Reserved */
ISR_NOERRCODE 28    /* Reserved */
ISR_NOERRCODE 29    /* Reserved */
ISR_ERRCODE   30    /* Security exception */
ISR_NOERRCODE 31    /* Reserved */

/* IRQ handlers (32-47) */
ISR_NOERRCODE 32    /* Timer */
ISR_NOERRCODE 33    /* Keyboard */
ISR_NOERRCODE 34
ISR_NOERRCODE 35
ISR_NOERRCODE 36
ISR_NOERRCODE 37
ISR_NOERRCODE 38
ISR_NOERRCODE 39
ISR_NOERRCODE 40
ISR_NOERRCODE 41
ISR_NOERRCODE 42
ISR_NOERRCODE 43
ISR_NOERRCODE 44
ISR_NOERRCODE 45
ISR_NOERRCODE 46
ISR_NOERRCODE 47

/* Common ISR stub - saves all registers and calls C handler */
isr_common_stub:
    /* Save all general purpose registers */
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi

    /* Call C interrupt handler with pointer to registers */
    mov %rsp, %rdi
    call interrupt_handler

    /* Restore registers */
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15

    /* Remove error code and interrupt number */
    add $16, %rsp

    /* Return from interrupt */
    iretq

/* Interrupt stub table for IDT setup */
.section .data
interrupt_stub_table:
    .quad isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    .quad isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    .quad isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .quad isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    .quad isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39
    .quad isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47
