/* EMBODIOS Native Boot Code - Multiboot2 Compliant */
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

.section .text
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
    
    /* Set up stack */
    mov $stack_top, %esp
    
    /* Clear direction flag */
    cld
    
    /* Check if we're on x86_64 capable CPU */
    call check_cpuid
    call check_long_mode
    
    /* Set up paging for 64-bit mode */
    call setup_page_tables
    call enable_paging
    
    /* Load GDT */
    lgdt gdt_descriptor
    
    /* Jump to 64-bit code */
    ljmp $0x08, $start64

.code64
start64:
    /* Reload segment registers */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    
    /* Set up 64-bit stack */
    mov $stack_top, %rsp
    
    /* Clear BSS section */
    xor %rax, %rax
    mov $__bss_start, %rdi
    mov $__bss_end, %rcx
    sub %rdi, %rcx
    rep stosb
    
    /* Enable CPU features for AI workloads */
    call enable_cpu_features
    
    /* Initialize EMBODIOS kernel */
    call kernel_main
    
    /* Halt if kernel returns */
    cli
1:  hlt
    jmp 1b

/* Check for CPUID support */
.code32
check_cpuid:
    pushfl
    pop %eax
    mov %eax, %ecx
    xor $0x200000, %eax  /* Toggle ID bit */
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
    call print_string_32
    hlt

/* Check for long mode support */
check_long_mode:
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb no_long_mode
    
    mov $0x80000001, %eax
    cpuid
    test $(1 << 29), %edx  /* Check LM bit */
    jz no_long_mode
    ret

no_long_mode:
    mov $no_long_mode_msg, %esi
    call print_string_32
    hlt

/* Set up identity-mapped page tables */
setup_page_tables:
    /* Clear page tables */
    mov $page_table_l4, %edi
    mov %edi, %cr3
    xor %eax, %eax
    mov $4096*3, %ecx
    rep stosl
    mov %cr3, %edi
    
    /* PML4[0] -> PDPT */
    mov $page_table_l3, %eax
    or $0x03, %eax  /* Present + Writable */
    mov %eax, (%edi)
    
    /* PDPT[0] -> PDT */
    mov $page_table_l2, %eax
    or $0x03, %eax
    mov %eax, page_table_l3
    
    /* PDT[0] -> 2MB page (identity map first 2MB) */
    mov $0x83, %eax  /* Present + Writable + Huge */
    mov %eax, page_table_l2
    
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

/* Print string in 32-bit mode (for debugging) */
print_string_32:
    pusha
    mov $0xb8000, %edi  /* VGA buffer */
.loop:
    lodsb
    test %al, %al
    jz .done
    mov $0x0f, %ah      /* White on black */
    stosw
    jmp .loop
.done:
    popa
    ret

/* Enable CPU features for AI workloads */
.code64
enable_cpu_features:
    /* Enable SSE */
    mov %cr0, %rax
    and $~0x04, %rax    /* Clear EM */
    or $0x02, %rax      /* Set MP */
    mov %rax, %cr0
    
    mov %cr4, %rax
    or $0x600, %rax     /* Set OSFXSR and OSXMMEXCPT */
    mov %rax, %cr4
    
    /* Check and enable AVX if available */
    mov $1, %eax
    cpuid
    test $(1 << 28), %ecx  /* Check AVX bit */
    jz .no_avx
    
    /* Enable AVX */
    xor %rcx, %rcx
    xgetbv
    or $0x07, %rax      /* Enable SSE, AVX, and x87 */
    xsetbv
    
.no_avx:
    /* Check and enable AVX-512 if available */
    mov $7, %eax
    xor %ecx, %ecx
    cpuid
    test $(1 << 16), %ebx  /* Check AVX512F bit */
    jz .no_avx512
    
    /* Enable AVX-512 */
    xor %rcx, %rcx
    xgetbv
    or $0xe0, %rax      /* Enable AVX-512 */
    xsetbv
    
.no_avx512:
    ret

/* GDT for 64-bit mode */
.section .rodata
.align 16
gdt:
    .quad 0                     /* Null descriptor */
    .quad 0x00AF9A000000FFFF   /* 64-bit code segment */
    .quad 0x00AF92000000FFFF   /* 64-bit data segment */
gdt_end:

gdt_descriptor:
    .word gdt_end - gdt - 1
    .quad gdt

/* Error messages */
no_cpuid_msg:       .asciz "ERROR: CPUID not supported"
no_long_mode_msg:   .asciz "ERROR: 64-bit mode not supported"

/* BSS section */
.section .bss
.align 16
stack_bottom:
    .space 1048576  /* 1MB stack */
stack_top:

.align 4096
page_table_l4:
    .space 4096
page_table_l3:
    .space 4096
page_table_l2:
    .space 4096

/* Multiboot info storage */
.section .data
.align 8
multiboot_magic:    .long 0
multiboot_info:     .long 0