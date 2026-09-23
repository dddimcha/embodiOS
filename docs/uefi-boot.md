# Direct UEFI Boot (no GRUB)

Since v0.6.0 "Volta", embodiOS can boot directly from UEFI firmware
(OVMF) without GRUB: the firmware's fallback boot path runs
`EFI/BOOT/BOOTX64.EFI` from a FAT ESP, which loads `embodios.elf` itself
and jumps to the kernel's multiboot2 entry.

## Build

```sh
# model must be baked into embodios.elf for chat
cat /mnt/agents/work/models/smollm-q4km.gguf.part_* \
    > /tmp/models/smollm-135m-instruct-q4_k_m.gguf   # if needed
make -C kernel GGUF_MODEL=/tmp/models/smollm-135m-instruct-q4_k_m.gguf
make -C kernel uefi        # produces kernel/BOOTX64.EFI + kernel/esp.img
```

`make uefi` does three things:

1. Compiles `kernel/arch/x86_64/uefi_loader.c` freestanding
   (`-ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone
   -maccumulate-outgoing-args -fno-pic -mcmodel=large`) and links it flat
   at ImageBase `0x10000000` with `ld --emit-relocs`
   (`kernel/arch/x86_64/uefi_loader.ld`).
2. `tools/mkuefi.py` converts that ELF64 to a PE32+ EFI application
   (machine `0x8664`, subsystem 10). The sandbox `objcopy` has **no**
   `efi-app-x86_64` target, which is why a pure-Python emitter exists.
   Every absolute 64-bit reference (compiled `-mcmodel=large`, so they are
   all `R_X86_64_64`) becomes a `DIR64` entry in a synthesized `.reloc`
   base-relocation table, so OVMF may load the image at any base.
3. `tools/mkesp.py` writes a partition-less ("superfloppy") FAT16 image
   (`esp.img`) containing `EFI/BOOT/BOOTX64.EFI` and `embodios.elf`.
   mtools/mkfs.vfat are unavailable in the sandbox, so the FAT writer is
   pure Python (8.3 names only, which suffices).

## Run / test

```sh
# manual
cp $HOME/sysroot/usr/share/qemu/OVMF.fd /tmp/OVMF.fd   # pflash is written
$QEMU_BIN -drive if=pflash,format=raw,file=/tmp/OVMF.fd \
          -drive format=raw,file=kernel/esp.img \
          -m 3072 -nographic -no-reboot

# scripted gate (boot-to-shell checks)
scripts/test_uefi.sh

# scripted gate + chat smoke (patient: TCG)
CHAT=1 scripts/test_uefi.sh
# or directly:
python3 /mnt/agents/work/smoke_uefi.py kernel/esp.img 900 \
    "What is the capital of France?"
```

## Design notes

Loader flow (`efi_main`, ms_abi):

1. **ESP discovery**: `HandleProtocol(ImageHandle, LoadedImage)` ->
   `DeviceHandle` -> `SimpleFileSystem.OpenVolume` -> open
   `\embodios.elf`.
2. **Read whole kernel into a high pool buffer first.** OVMF's memory map
   has firmware-owned areas inside the kernel's physical span
   (1 MiB..~128 MiB): ACPI NVS at 8-9 MiB and a 12 MiB BootServicesData
   pool area at 9-21 MiB. The FAT/DiskIo drivers still need their pool, so
   low memory must not be touched until all file I/O is done.
3. **ELF64 load**: validates `ET_EXEC`/`EM_X86_64`; for each `PT_LOAD`,
   every *free* (EfiConventionalMemory) sub-range of
   `[p_paddr, p_paddr+p_memsz)` is claimed with
   `AllocatePages(AllocateAddress)` so the final memory map marks the
   kernel's home as used. Firmware-owned holes are left alone and
   overwritten later (embodiOS does not use ACPI).
4. **Synthesized multiboot2 info** at physical `0x9000` (4 pages pinned):
   header `{total_size, reserved}`, tag type 1 cmdline `"uefi"`, tag
   type 6 mmap translated from the final `GetMemoryMap`
   (EfiConventionalMemory -> type 1, everything else -> type 2;
   `entry_size=24`, `entry_version=0`, adjacent same-type entries merged),
   end tag type 0 size 8. Tags 8-byte aligned.
5. **ExitBootServices(ImageHandle, MapKey)**, one retry on failure.
   Nothing but serial-port I/O happens between the final `GetMemoryMap`
   and `ExitBootServices` (console printing could allocate and invalidate
   the map key).
6. **Post-EBS copy**: each `PT_LOAD` is memcpy'd from the pool buffer to
   its physical address and the `p_memsz - p_filesz` tail zeroed. No
   firmware code runs any more, so overwriting the holes is safe.
7. **64 -> 32 drop**: `cli`; `lgdt` a flat 32-bit GDT; far return into a
   32-bit compatibility segment; disable paging (`CR0.PG=0`), clear
   `CR4.PAE` and `EFER.LME`; then `EAX=0x36d76289` (multiboot2 magic),
   `EBX=0x9000`, `jmp *e_entry` (verified to be `_start`, 0x100020 for the
   current link). boot.S takes over exactly as it does from GRUB.

### Why the PE has separate RX/RW sections

The first mkuefi.py version folded everything into one RWX `.text`
section. OVMF's image memory protection #GP-faults such images inside
`StartImage` before the entry point runs. `.text` is now
CODE|EXECUTE|READ (RX) and `.rodata`/`.data`/`.bss` go into a separate
INITIALIZED_DATA|READ|WRITE (RW) `.data` section (page-aligned via the
linker script); `.reloc` is DISCARDABLE|READ.

### Why ImageBase is 0x10000000

The kernel loads at physical 1 MiB and spans ~128 MiB. If the loader image
itself sat inside that range (the first draft used 0x400000), the
`AllocateAddress` claims for the kernel span would fail with
EFI_NOT_FOUND. 256 MiB is safely above the footprint with the usual
`-m 3072`.

## Test results (QEMU 7.2 TCG, OVMF from Debian bookworm)

Verified on branch `volta-uefi`:

- `scripts/test_uefi.sh` — all 7 checks PASS:
  `embodios UEFI loader` banner on COM1, `found \embodios.elf`,
  `boot services exited`, `Memmap: boot magic=0x36D76289 info=0x9000`,
  `Memmap: 2817 MB usable RAM in 7 region(s) [multiboot2 mmap]`,
  `Kernel cmdline: uefi`, `embodios> ` shell prompt.
- Chat smoke (`smoke_uefi.py kernel/esp.img 900`): shell at ~13 s,
  `tok/s` marker at ~64 s, answer `The capital of France is Paris.`
  (8 tokens, 1.38 tok/s).
- Regressions: `make test` 6/6 PASS; QEMU `-kernel embodios.elf` smoke
  PASS (Paris answer, 1.39 tok/s); multiboot2/PVH paths untouched
  (boot.S, kernel.c, kernel.ld, paging.c unmodified).

## Limitations / deviations

- **Deviations from the spec's owns list**: an extra file
  `kernel/arch/x86_64/uefi_loader.ld` (linker script; the spec listed only
  `uefi_loader.c`/`.h`). Also `.bss` is zero-filled into the PE raw data by
  mkuefi.py instead of relying on the firmware to zero it (grub-style PE
  images rely on the loader zero-filling VirtualSize > SizeOfRawData;
  folding is simpler and equivalent here).
- The spec's "read into temp buffer, copy per PT_LOAD before
  ExitBootServices" order is adjusted: the copy to final physical
  addresses happens *after* ExitBootServices (see "Read whole kernel..."
  above for why). The mmap tag is still built from the final
  GetMemoryMap before EBS, as specified.
- `mkesp.py` supports only 8.3 file names (sufficient for
  `EFI/BOOT/BOOTX64.EFI` + `embodios.elf`); no LFN entries.
- The ESP is a superfloppy FAT16 (no partition table) — boots fine on
  OVMF; some picky firmwares may want an MBR partition.
- `make clean` does not remove `esp.img`/`BOOTX64.EFI` (the shared clean
  rule was left untouched; they are git-ignored).
- OVMF release builds print nothing on the debug console; loader
  diagnostics go to COM1 and ConOut only.
