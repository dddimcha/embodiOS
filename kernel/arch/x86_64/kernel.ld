/* EMBODIOS x86_64 Kernel Linker Script */
ENTRY(_start)

SECTIONS
{
    /* Physical load address */
    . = 0x100000;

    _kernel_start = .;

    /* --- RX segment: code and read-only data -------------------------- */

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

    /* --- RW segment: writable data and BSS ------------------------------
     * Page-align the start so the linker can emit a separate LOAD segment
     * with RW (not RWX) permissions. */
    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data :
    {
        *(.data*)

        /* Constructor arrays for __attribute__((constructor)).
         * ALIGN(8) is mandatory: .init_array entries are 8-byte pointers
         * and the linker aligns the input section within the output
         * section AFTER the symbol assignment, so without ALIGN the
         * __init_array_start symbol can point 4 bytes before the real
         * array (observed when .data starts with a 4-byte flag). */
        . = ALIGN(8);
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        KEEP(*(.ctors))
        KEEP(*(.ctors.*))
        KEEP(*(SORT(.ctors.*)))
        . = ALIGN(8);
        __init_array_end = .;
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