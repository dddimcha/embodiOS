/* EMBODIOS x86_64 Kernel Linker Script */
ENTRY(_start)

SECTIONS
{
    /* Physical load address */
    . = 0x100000;
    
    _kernel_start = .;
    
    .multiboot2 ALIGN(4K) :
    {
        KEEP(*(.multiboot2))
    }
    
    .text ALIGN(4K) :
    {
        *(.text.boot)
        *(.text*)
    }
    
    .rodata ALIGN(4K) :
    {
        *(.rodata*)
        
        /* Model weights section */
        _model_weights_start = .;
        *(.model_weights)
        _model_weights_end = .;
    }
    
    .data ALIGN(4K) :
    {
        *(.data*)
    }
    
    .bss ALIGN(4K) :
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