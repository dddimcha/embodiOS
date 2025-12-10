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

    /* Output 'B' for Boot started */
    mov $0x3F8+5, %dx
.wait_serial1:
    inb %dx, %al
    test $0x20, %al
    jz .wait_serial1
    mov $0x3F8, %dx
    mov $'B', %al
    outb %al, %dx

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

    /* Output '1' for checks done */
    mov $0x3F8+5, %dx
.wait_serial2:
    inb %dx, %al
    test $0x20, %al
    jz .wait_serial2
    mov $0x3F8, %dx
    mov $'1', %al
    outb %al, %dx

    /* Set up paging for 64-bit mode */
    call setup_page_tables
    call enable_paging

    /* Output '2' for paging done */
    mov $0x3F8+5, %dx
.wait_serial3:
    inb %dx, %al
    test $0x20, %al
    jz .wait_serial3
    mov $0x3F8, %dx
    mov $'2', %al
    outb %al, %dx

    /* Load GDT */
    lgdt gdt64_descriptor

    /* Jump to 64-bit code */
    ljmp $0x08, $start64

.code64
.global start64
start64:
    /* Output '3' on serial immediately - we reached 64-bit! */
    mov $0x3F8+5, %dx
.wait_serial64_1:
    inb %dx, %al
    test $0x20, %al
    jz .wait_serial64_1
    mov $0x3F8, %dx
    mov $'3', %al
    outb %al, %dx

    /* Print '64' to show we reached 64-bit mode */
    mov $0xb8000, %rdi
    movw $0x0f36, (%rdi)      /* '6' */
    movw $0x0f34, 2(%rdi)     /* '4' */

    /* Reload segment registers */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    /* Print 'S' for segments loaded */
    movw $0x0f53, 4(%rdi)

    /* Set up kernel stack */
    mov $kernel_stack_top, %rsp

    /* Output '4' - stack setup */
    mov $0x3F8+5, %dx
.wait_serial64_2:
    inb %dx, %al
    test $0x20, %al
    jz .wait_serial64_2
    mov $0x3F8, %dx
    mov $'4', %al
    outb %al, %dx

    /* Clear frame pointer */
    xor %rbp, %rbp

    /* Output '5' - before CPU features */
    mov $0x3F8+5, %dx
.wait_serial64_3:
    inb %dx, %al
    test $0x20, %al
    jz .wait_serial64_3
    mov $0x3F8, %dx
    mov $'5', %al
    outb %al, %dx

    /* Enable CPU features for AI workloads */
    call enable_cpu_features

    /* Output '6' - before kernel_main */
    mov $0x3F8+5, %dx
.wait_serial64_4:
    inb %dx, %al
    test $0x20, %al
    jz .wait_serial64_4
    mov $0x3F8, %dx
    mov $'6', %al
    outb %al, %dx

    /* Call kernel main (we're identity-mapped, no higher half needed) */
    call kernel_main

    /* Output 'X' - kernel_main returned (shouldn't happen) */
    mov $0x3F8+5, %dx
.wait_serial64_5:
    inb %dx, %al
    test $0x20, %al
    jz .wait_serial64_5
    mov $0x3F8, %dx
    mov $'X', %al
    outb %al, %dx

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
    and $~0x04, %rax
    or $0x02, %rax
    mov %rax, %cr0
    
    mov %cr4, %rax
    or $0x600, %rax
    mov %rax, %cr4
    
    /* Check and enable AVX */
    mov $1, %eax
    cpuid
    test $(1 << 28), %ecx
    jz .no_avx
    
    xor %rcx, %rcx
    xgetbv
    or $0x07, %rax
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
multiboot_magic:    .long 0
multiboot_info:     .long 0
