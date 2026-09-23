/* uefi_loader.ld - link the UEFI loader flat at its PE ImageBase.
 *
 * The PE ImageBase is 0x10000000 (256 MiB, above the kernel footprint of
 * ~1..128 MiB so the kernel PT_LOAD range stays free for AllocateAddress);
 * the first section starts at RVA 0x1000 (one SectionAlignment above the
 * PE headers), so the ELF is linked at 0x10001000. tools/mkuefi.py converts
 * this ELF to PE32+ and turns the preserved (--emit-relocs) R_X86_64_64
 * relocs into a .reloc base-relocation table.
 */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(efi_main)

SECTIONS
{
    . = 0x10001000;

    .text : {
        *(.text*)
    }

    /* PE sections must start at page-aligned RVAs: the .data PE section
     * (RW, non-executable) covers .rodata/.data/.bss below. OVMF rejects
     * or faults on RWX images, so .text (RX) must end on a page boundary. */
    . = ALIGN(0x1000);

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    . = ALIGN(8);
    _loader_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note*)
        *(.comment*)
        *(.gnu.hash)
        *(.gnu.version*)
    }
}
