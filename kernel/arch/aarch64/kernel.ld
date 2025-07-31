/* EMBODIOS ARM64 Kernel Linker Script */

ENTRY(_start)

SECTIONS
{
    . = 0x40000000;  /* Typical ARM64 kernel load address */
    _kernel_start = .;
    
    .text : {
        *(.text.boot)
        *(.text)
        *(.text.*)
    }
    
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }
    
    .data : {
        *(.data)
        *(.data.*)
    }
    
    .bss : {
        . = ALIGN(16);
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(16);
        _bss_end = .;
    }
    
    /* Model weights section */
    .model_weights : {
        _model_weights_start = .;
        *(.model_weights)
        _model_weights_end = .;
    }
    
    /* End of kernel */
    _kernel_end = .;
}