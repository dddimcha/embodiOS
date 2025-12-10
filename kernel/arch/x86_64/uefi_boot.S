/* UEFI Boot Stub for EMBODIOS */
.section .text
.code64

.global _start
_start:
    /* UEFI entry point */
    /* RCX = EFI_HANDLE ImageHandle */
    /* RDX = EFI_SYSTEM_TABLE *SystemTable */
    
    /* Save UEFI parameters */
    mov %rcx, %r14      /* Save ImageHandle */
    mov %rdx, %r15      /* Save SystemTable */
    
    /* Setup stack */
    lea stack_top(%rip), %rsp
    
    /* Clear direction flag */
    cld
    
    /* Jump to C entry point */
    call uefi_kernel_main
    
    /* Should never return, but just in case */
1:
    hlt
    jmp 1b

.section .bss
.align 16
stack_bottom:
    .skip 0x100000      /* 1MB stack */
stack_top:
