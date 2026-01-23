/* EMBODIOS x86_64 Boot Code - Extended from compiler version */
.section .multiboot2
    .align 8
multiboot2_header:
    .long 0xe85250d6                /* Magic number */
    .long 0                         /* Architecture (i386) */
    .long multiboot2_header_end - multiboot2_header
    .long -(0xe85250d6 + 0 + (multiboot2_header_end - multiboot2_header))

    /* Information request tag */
    .align 8
    .short 1                        /* Type */
    .short 0                        /* Flags */
    .long 8                         /* Size */

    /* End tag */
    .align 8
    .short 0                        /* Type */
    .short 0                        /* Flags */
    .long 8                         /* Size */
multiboot2_header_end:

/* PVH ELF Note - Required by modern QEMU */
.section .note.Xen, "a"
    .align 4
    .long 4                         /* Name size */
    .long 4                         /* Descriptor size */
    .long 18                        /* Type = XEN_ELFNOTE_PHYS32_ENTRY */
    .asciz "Xen"                    /* Name */
    .align 4
    .long _start                    /* Entry point */

.section .text.boot
.global _start
.extern kernel_main

_start:
    /* We're in 32-bit mode initially */
    .code32

    /* Disable interrupts */
    cli

    /* Initialize serial port for early debug output */
    mov $0x3F8+1, %dx
    mov $0x00, %al
    outb %al, %dx          /* Disable interrupts */
    mov $0x3F8+3, %dx
    mov $0x80, %al
    outb %al, %dx          /* Enable DLAB */
    mov $0x3F8+0, %dx
    mov $0x03, %al
    outb %al, %dx          /* 38400 baud lo */
    mov $0x3F8+1, %dx
    mov $0x00, %al
    outb %al, %dx          /* 38400 baud hi */
    mov $0x3F8+3, %dx
    mov $0x03, %al
    outb %al, %dx          /* 8N1 */
    mov $0x3F8+2, %dx
    mov $0xC7, %al
    outb %al, %dx          /* FIFO */
    mov $0x3F8+4, %dx
    mov $0x0B, %al
    outb %al, %dx          /* RTS/DSR set */

    /* Save multiboot info */
    mov %eax, multiboot_magic
    mov %ebx, multiboot_info

    /* Set up temporary stack */
    mov $boot_stack_top, %esp

    /* Clear direction flag */
    cld

    /* Check CPU features */
    call check_cpuid
    call check_long_mode

    /* Set up paging for 64-bit mode */
    call setup_page_tables
    call enable_paging

    /* Load GDT */
    lgdt gdt64_descriptor

    /* Jump to 64-bit code */
    ljmp $0x08, $start64

.code64
.global start64
start64:
    /* Reload segment registers */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Set up kernel stack */
    mov $kernel_stack_top, %rsp

    /* Clear frame pointer */
    xor %rbp, %rbp

    /* Enable CPU features for AI workloads */
    call enable_cpu_features

    /* Call kernel main (we're identity-mapped, no higher half needed) */
    call kernel_main

    /* Should not return */
    cli
1:  hlt
    jmp 1b

/* Check for CPUID support */
.section .text.boot
.code32
check_cpuid:
    pushfl
    pop %eax
    mov %eax, %ecx
    xor $0x200000, %eax
    push %eax
    popfl
    pushfl
    pop %eax
    push %ecx
    popfl
    cmp %ecx, %eax
    je no_cpuid
    ret

no_cpuid:
    mov $no_cpuid_msg, %esi
    call print_error_32
    hlt

/* Check for long mode support */
check_long_mode:
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb no_long_mode
    
    mov $0x80000001, %eax
    cpuid
    test $(1 << 29), %edx
    jz no_long_mode
    ret

no_long_mode:
    mov $no_long_mode_msg, %esi
    call print_error_32
    hlt

/* Set up 4-level page tables */
setup_page_tables:
    /* Clear page tables */
    mov $pml4_table, %edi
    xor %eax, %eax
    mov $1024, %ecx  /* Clear 4KB (1024 dwords) for each table */
    rep stosl

    mov $pdpt_table, %edi
    mov $1024, %ecx
    rep stosl

    mov $pdt_table, %edi
    mov $1024, %ecx
    rep stosl

    mov $pml4_table, %edi

    /* PML4[0] -> PDPT (identity map) */
    mov $pdpt_table, %eax
    or $0x03, %eax
    mov %eax, (%edi)

    /* PDPT[0] -> PDT */
    mov $pdt_table, %eax
    or $0x03, %eax
    mov %eax, pdpt_table

    /* Map first 1GB using 2MB pages */
    mov $pdt_table, %edi
    mov $0x83, %eax  /* Present + Writable + Huge */
    mov $512, %ecx
.map_pages:
    mov %eax, (%edi)
    add $0x200000, %eax
    add $8, %edi
    loop .map_pages

    /* Load page table address into CR3 */
    mov $pml4_table, %eax
    mov %eax, %cr3

    ret

/* Enable paging and long mode */
enable_paging:
    /* Enable PAE */
    mov %cr4, %eax
    or $(1 << 5), %eax
    mov %eax, %cr4
    
    /* Set long mode bit in EFER */
    mov $0xC0000080, %ecx
    rdmsr
    or $(1 << 8), %eax
    wrmsr
    
    /* Enable paging */
    mov %cr0, %eax
    or $(1 << 31), %eax
    mov %eax, %cr0
    
    ret

/* Print error in 32-bit mode */
print_error_32:
    pusha
    mov $0xb8000, %edi
.loop:
    lodsb
    test %al, %al
    jz .done
    mov $0x4f, %ah  /* Red on black */
    stosw
    jmp .loop
.done:
    popa
    ret

/* Enable CPU features */
.code64
enable_cpu_features:
    /* Enable SSE */
    mov %cr0, %rax
    and $~0x04, %rax      /* Clear EM (bit 2) */
    or $0x02, %rax        /* Set MP (bit 1) */
    mov %rax, %cr0

    mov %cr4, %rax
    or $0x600, %rax       /* Set OSFXSR (bit 9) and OSXMMEXCPT (bit 10) */
    mov %rax, %cr4

    /* Check for OSXSAVE support before trying AVX */
    mov $1, %eax
    cpuid
    test $(1 << 27), %ecx  /* Check OSXSAVE support (bit 27) */
    jz .no_avx
    test $(1 << 28), %ecx  /* Check AVX support (bit 28) */
    jz .no_avx

    /* Enable OSXSAVE in CR4 first - required before xgetbv/xsetbv */
    mov %cr4, %rax
    or $0x40000, %rax      /* Set OSXSAVE (bit 18) */
    mov %rax, %cr4

    /* Now we can use xgetbv/xsetbv to enable AVX */
    xor %rcx, %rcx
    xgetbv
    or $0x07, %rax         /* Enable XMM and YMM state */
    xsetbv

.no_avx:
    ret

/* Global Descriptor Table */
.section .rodata
.align 16
gdt64:
    .quad 0                         /* Null descriptor */
    .quad 0x00AF9A000000FFFF       /* Code segment */
    .quad 0x00AF92000000FFFF       /* Data segment */
gdt64_end:

gdt64_descriptor:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

/* Error messages */
no_cpuid_msg:       .asciz "ERROR: CPUID not supported"
no_long_mode_msg:   .asciz "ERROR: 64-bit mode not supported"

/* BSS section */
.section .bss
.align 4096
pml4_table:     .space 4096
pdpt_table:     .space 4096
pdt_table:      .space 4096
pt_table:       .space 4096

.align 16
boot_stack_bottom:
    .space 16384
boot_stack_top:

.align 16
.global kernel_stack_bottom
kernel_stack_bottom:
    .space 65536
.global kernel_stack_top
kernel_stack_top:

/* Data section */
.section .data
.align 8
.global multiboot_magic
.global multiboot_info
multiboot_magic:    .long 0
multiboot_info:     .long 0
