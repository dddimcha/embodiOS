/* EMBODIOS x86_64 AP (Application Processor) Startup Trampoline
 *
 * Real-mode trampoline copied to physical 0x8000 by smp_init() and entered
 * by APs via INIT-SIPI-SIPI (SIPI vector 0x08 => CS:IP = 0x0800:0x0000).
 *
 * Boot path: 16-bit real mode -> 32-bit protected mode -> 64-bit long mode
 * (kernel page tables, identity mapped) -> kernel GDT -> smp_ap_boot_entry.
 *
 * Position-dependence: absolute references are link-time constants based on
 * TRAMP_PHYS (the trampoline is always copied to 0x8000). Data slots at the
 * end of the page are patched by the BSP after copying:
 *   tramp_gdt_desc.base  <- physical address of tramp_gdt (0x8000 + offset)
 *   tramp_cr3            <- kernel CR3 (physical == virtual, identity map)
 *   tramp_stack_slot     <- per-AP stack top (patched before each SIPI)
 *   tramp_entry_slot     <- absolute address of smp_ap_boot_entry
 *
 * References BETWEEN trampoline symbols (RIP-relative or offset deltas) stay
 * valid after the copy; references to OUTSIDE symbols must be absolute
 * (movabs) because the trampoline executes at 0x8000, not its link address.
 */

#define TRAMP_PHYS 0x8000

    .section .text.trampoline
    .align 16
    .code16
    .globl smp_trampoline_start
smp_trampoline_start:
    cli
    cld
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs

    /* Load trampoline GDT (base patched to physical address by BSP) */
    lgdtl (tramp_gdt_desc - smp_trampoline_start + TRAMP_PHYS)

    /* Enable protected mode */
    mov %cr0, %eax
    or  $0x1, %eax
    mov %eax, %cr0

    /* Far jump to 32-bit protected mode (selector 0x08).
     * Encoded by hand: 0x66 (operand-size) + 0xEA (ljmp imm32:imm16). */
    .byte 0x66, 0xea
    .long tramp_pm_entry - smp_trampoline_start + TRAMP_PHYS
    .word 0x08

    .align 16
    .code32
tramp_pm_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs

    /* Enable PAE (required for long mode) */
    mov %cr4, %eax
    or  $(1 << 5), %eax
    mov %eax, %cr4

    /* Load kernel page tables (patched physical CR3) */
    mov (tramp_cr3 - smp_trampoline_start + TRAMP_PHYS), %eax
    mov %eax, %cr3

    /* Enable long mode (EFER.LME) */
    mov $0xC0000080, %ecx
    rdmsr
    or  $(1 << 8), %eax
    wrmsr

    /* Enable paging -> IA-32e compatibility mode.
     * The kernel page tables identity-map [0,1GB), so execution at
     * 0x8000+ continues seamlessly. */
    mov %cr0, %eax
    or  $(1 << 31), %eax
    mov %eax, %cr0

    /* Far jump to the 64-bit code segment (selector 0x18) */
    ljmp $0x18, $(tramp_lm_entry - smp_trampoline_start + TRAMP_PHYS)

    .align 16
    .code64
tramp_lm_entry:
    /* Trampoline 64-bit data segment while still on trampoline GDT */
    mov $0x20, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    /* Switch to the kernel GDT (absolute address, identity mapped) */
    movabs $gdt64_descriptor, %rax
    lgdt (%rax)

    /* Reload data/stack segments with the KERNEL GDT selectors (0x10).
     * The trampoline-GDT selector 0x20 loaded above is beyond the kernel
     * GDT limit; that stayed latent while APs ran with IF=0, but the
     * first interrupt's iretq reloads SS and would #GP(0x20) (WS-A: APs
     * now park with IF=1, so interrupts are taken on APs). */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss

    /* Per-AP stack + entry point (slots inside this page: RIP-relative
     * deltas between trampoline symbols survive the copy to 0x8000) */
    mov tramp_stack_slot(%rip), %rsp
    mov tramp_entry_slot(%rip), %rax

    /* Far transfer to kernel 64-bit CS (0x08): push CS, push RIP, lretq */
    pushq $0x08
    pushq %rax
    lretq

/* Trampoline-private GDT: null / 32-bit code / 32-bit data / 64-bit code /
 * 64-bit data, all base 0 limit 4GB (flat). */
    .align 16
    .globl tramp_gdt
tramp_gdt:
    .quad 0x0000000000000000      /* 0x00 null */
    .quad 0x00CF9A000000FFFF      /* 0x08 32-bit code */
    .quad 0x00CF92000000FFFF      /* 0x10 32-bit data */
    .quad 0x00AF9A000000FFFF      /* 0x18 64-bit code */
    .quad 0x00AF92000000FFFF      /* 0x20 64-bit data */
    .globl tramp_gdt_end
tramp_gdt_end:

    .align 8
    .globl tramp_gdt_desc
tramp_gdt_desc:
    .word tramp_gdt_end - tramp_gdt - 1
    .quad 0                       /* patched: physical address of tramp_gdt */

/* Data slots patched by BSP (offsets computed as symbol - start) */
    .align 8
    .globl tramp_cr3
tramp_cr3:        .quad 0         /* kernel CR3 (physical) */
    .globl tramp_stack_slot
tramp_stack_slot: .quad 0         /* per-AP stack top */
    .globl tramp_entry_slot
tramp_entry_slot: .quad 0         /* smp_ap_boot_entry (absolute) */

    .globl smp_trampoline_end
smp_trampoline_end:

/* ------------------------------------------------------------------------
 * 64-bit AP C-entry trampoline. Lives in the NORMAL kernel .text (not
 * copied to 0x8000): reached via lretq above with kernel CS and the per-AP
 * stack already in RSP. Enables FPU/SSE/AVX for this CPU (APs reset with
 * CR0.EM/TS set), then enters the C AP main loop. Never returns.
 * ------------------------------------------------------------------------ */
    .section .text
    .align 16
    .code64
    .globl smp_ap_boot_entry
smp_ap_boot_entry:
    call enable_cpu_features      /* boot.S: CR0/CR4 FPU/SSE + AVX setup */
    call smp_ap_main              /* smp.c: per-CPU init + work loop */
    /* smp_ap_main never returns; park just in case */
1:  hlt
    jmp 1b

/* Mark stack as non-executable */
    .section .note.GNU-stack,"",@progbits
