/* EMBODIOS ARM64 Kernel Linker Script
 * For Raspberry Pi 3/4/5 (BCM2837/BCM2711/BCM2712)
 *
 * The Pi firmware (start*.elf, arm_64bit=1) loads kernel8.img as a raw
 * binary at 0x80000 and enters at its first byte. QEMU's raspi3b machine
 * does the same for -kernel. Entry _start is at the head of .text.boot.
 */

ENTRY(_start)

SECTIONS
{
    /* Pi firmware / QEMU raspi3b load kernel8.img at 0x80000 */
    . = 0x80000;

    _kernel_start = .;

    .text : {
        *(.text.boot)   /* Boot code first - entry point at image offset 0 */
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
