/* EMBODIOS x86_64 Boot Code - Extended from compiler version */
.section __TEXT,__multiboot2
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

.section __TEXT,__text_boot
.global _start
.extern kernel_main

_start:
    /* We're in 32-bit mode initially */
    .code32
    
    /* Disable interrupts */
    cli
    
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
    
    /* Jump to higher half */
    mov $higher_half_start, %rax
    jmp *%rax

.section __TEXT,__text
higher_half_start:
    /* Reload stack pointer to higher half */
    mov $kernel_stack_top, %rsp
    /* Stack is already set up at physical address */
    
    /* Call kernel main */
    call kernel_main
    
    /* Should not return */
    cli
1:  hlt
    jmp 1b

/* Check for CPUID support */
.section __TEXT,__text_boot
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
    mov %edi, %cr3
    xor %eax, %eax
    mov $4096*5, %ecx
    rep stosl
    mov %cr3, %edi
    
    /* PML4[0] -> PDPT (identity map) */
    mov $pdpt_table, %eax
    or $0x03, %eax
    mov %eax, (%edi)
    
    /* PML4[511] -> PDPT (higher half) */
    mov $pdpt_table, %eax
    or $0x03, %eax
    mov %eax, 511*8(%edi)
    
    /* PDPT[0] -> PDT */
    mov $pdt_table, %eax
    or $0x03, %eax
    mov %eax, pdpt_table
    
    /* PDPT[510] -> PDT (for higher half) */
    mov $pdt_table, %eax
    or $0x03, %eax
    mov %eax, pdpt_table + 510*8
    
    /* Map first 1GB using 2MB pages */
    mov $pdt_table, %edi
    mov $0x83, %eax  /* Present + Writable + Huge */
    mov $512, %ecx
.map_pages:
    mov %eax, (%edi)
    add $0x200000, %eax
    add $8, %edi
    loop .map_pages
    
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
.section __TEXT,__rodata
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
.section __DATA,__bss
.align 12
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
.section __DATA,__data
.align 8
multiboot_magic:    .long 0
multiboot_info:     .long 0
