/* EMBODIOS x86_64 Kernel Linker Script */
ENTRY(_start)

SECTIONS
{
    /* Kernel starts at 1MB */
    . = 0xFFFFFFFF80100000;
    
    _kernel_start = .;
    
    .multiboot2 ALIGN(4K) : AT(ADDR(.multiboot2) - 0xFFFFFFFF80000000)
    {
        KEEP(*(.multiboot2))
    }
    
    .text ALIGN(4K) : AT(ADDR(.text) - 0xFFFFFFFF80000000)
    {
        *(.text.boot)
        *(.text*)
    }
    
    .rodata ALIGN(4K) : AT(ADDR(.rodata) - 0xFFFFFFFF80000000)
    {
        *(.rodata*)
        
        /* Model weights section */
        _model_weights_start = .;
        *(.model_weights)
        _model_weights_end = .;
    }
    
    .data ALIGN(4K) : AT(ADDR(.data) - 0xFFFFFFFF80000000)
    {
        *(.data*)
    }
    
    .bss ALIGN(4K) : AT(ADDR(.bss) - 0xFFFFFFFF80000000)
    {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    }
    
    _kernel_end = .;
    
    /* Ensure kernel doesn't exceed reasonable size */
    ASSERT((_kernel_end - _kernel_start) <= 64M, "Kernel image too large")
}