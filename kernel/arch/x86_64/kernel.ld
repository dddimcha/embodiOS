/* EMBODIOS x86_64 Kernel Linker Script */
ENTRY(_start)

SECTIONS
{
    /* Physical load address */
    . = 0x100000;

    _kernel_start = .;

    /* Multiboot2 header MUST be in first 32KB and in a LOAD segment */
    .boot :
    {
        KEEP(*(.multiboot2))
        *(.text.boot)
    }

    .text :
    {
        *(.text*)
    }

    .rodata :
    {
        *(.rodata*)

        /* Model weights section */
        _model_weights_start = .;
        *(.model_weights)
        *(__DATA.__model_weights)
        _model_weights_end = .;
    }

    .data :
    {
        *(.data*)
    }

    .bss :
    {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    }

    _kernel_end = .;

    /* Discard unwanted sections */
    /DISCARD/ :
    {
        *(.eh_frame)
        *(.comment)
    }
}