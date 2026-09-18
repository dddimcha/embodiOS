/* EMBODIOS ARM64 Kernel Linker Script */
ENTRY(_start)

SECTIONS
{
    /* Kernel starts at 2MB for ARM64 */
    . = 0xFFFF000000200000;
    
    _kernel_start = .;
    
    .text.boot : {
        KEEP(*(.text.boot))
    }
    
    .text ALIGN(4K) : {
        *(.text*)
    }
    
    .rodata ALIGN(4K) : {
        *(.rodata*)
        
        /* Model weights section */
        _model_weights_start = .;
        *(.model_weights)
        _model_weights_end = .;
    }
    
    .data ALIGN(4K) : {
        *(.data*)
    }
    
    .bss ALIGN(4K) : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    }
    
    /* Stack */
    . = ALIGN(16K);
    _stack_bottom = .;
    . += 1M;
    _stack_top = .;
    
    _kernel_end = .;
    
    /* Ensure kernel doesn't exceed reasonable size */
    ASSERT((_kernel_end - _kernel_start) <= 64M, "Kernel image too large")
}