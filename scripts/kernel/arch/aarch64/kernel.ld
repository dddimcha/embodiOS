/* EMBODIOS ARM64 Kernel Linker Script
 * For QEMU virt machine (ARM64)
 */

ENTRY(_start)

SECTIONS
{
    /* QEMU virt machine loads kernel at 0x40000000 */
    . = 0x40000000;

    _kernel_start = .;

    .text : {
        *(.text.boot)   /* Boot code first */
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

    /* C++ constructors / init_array for global constructors */
    .init_array : {
        __init_array_start = .;
        KEEP(*(.init_array))
        KEEP(*(.ctors))
        __init_array_end = .;
    }

    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        _bss_start = .;     /* Alias for compatibility */
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
        _bss_end = .;       /* Alias for compatibility */
    }

    /* Model weights section (for embedded models) */
    .model_weights : {
        . = ALIGN(8);
        _model_weights_start = .;
        *(.model_weights)
        _model_weights_end = .;
    }

    _kernel_end = .;

    /* Discard unwanted sections */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}