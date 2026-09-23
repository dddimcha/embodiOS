#!/usr/bin/env python3
"""mkuefi.py - pure-Python PE32+ (EFI application) emitter for embodiOS.

The sandbox objcopy has NO efi-app-x86_64 target, so we convert a statically
linked, freestanding ELF64 executable (the UEFI loader, linked at a fixed
image base with `ld --emit-relocs`) into a PE32+ EFI application ourselves.

Input ELF requirements (produced by the kernel Makefile `uefi` target):
  - ELF64 x86-64 EXEC, linked one SECTION_ALIGNMENT (0x1000) above the PE
    ImageBase (uefi_loader.ld links at 0x10001000 for ImageBase 0x10000000)
  - .text* first, then a page-aligned boundary, then .rodata*/.data*/.bss*
  - relocation records preserved (--emit-relocs); every absolute 64-bit
    reference appears as R_X86_64_64 so it can be turned into a PE base
    relocation (DIR64). PC-relative relocs are already resolved and ignored;
    any other absolute type (32/32S/GOTPCREL...) is a hard error because it
    would break if OVMF loads the image at a non-preferred base.

Output: PE32+, machine 0x8664, subsystem 10 (EFI application), sections
.text (RX, from ELF .text*), .data (RW, from .rodata*/.data*/.bss*) and
.reloc (base relocation table). Sections are deliberately not merged into
one RWX blob: OVMF's image memory protection faults on W+X sections.

Usage: mkuefi.py <input.elf> <output.efi>
"""
import struct
import sys

# ---- ELF constants ---------------------------------------------------------
SHT_PROGBITS = 1
SHT_NOBITS = 8
SHT_RELA = 4
SHF_ALLOC = 0x2
EM_X86_64 = 62
ET_EXEC = 2

R_X86_64_NONE = 0
R_X86_64_64 = 1
R_X86_64_PC32 = 2
R_X86_64_GOT32 = 3
R_X86_64_PLT32 = 4
R_X86_64_PC64 = 20
# Anything else that embeds an absolute or link-time address would need a
# base relocation we cannot express; fail loudly instead of shipping a
# broken image.
_ALLOWED_IGNORED = {R_X86_64_NONE, R_X86_64_PC32, R_X86_64_PLT32, R_X86_64_PC64}

# ---- PE constants ----------------------------------------------------------
PE_MACHINE_X64 = 0x8664
PE_CHARACTERISTICS = 0x0222  # EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE | DEBUG_STRIPPED
PE32PLUS_MAGIC = 0x20B
SUBSYSTEM_EFI_APPLICATION = 10
SECTION_ALIGNMENT = 0x1000
FILE_ALIGNMENT = 0x200

# Section characteristics
SCN_TEXT = 0x60000020   # CODE | EXECUTE | READ
SCN_DATA = 0xC0000040   # INITIALIZED_DATA | READ | WRITE
SCN_RELOC = 0x42000040  # INITIALIZED_DATA | DISCARDABLE | READ

DIR64 = 10  # IMAGE_REL_BASED_DIR64


class ElfError(Exception):
    pass


def parse_elf(data):
    if data[:4] != b'\x7fELF':
        raise ElfError('not an ELF file')
    if data[4] != 2 or data[5] != 1:
        raise ElfError('need ELF64 little-endian')
    (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags,
     e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = \
        struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
    if e_machine != EM_X86_64:
        raise ElfError('e_machine != EM_X86_64')
    if e_type != ET_EXEC:
        raise ElfError('need a statically linked ET_EXEC (link with ld first)')
    if e_shoff == 0 or e_shnum == 0:
        raise ElfError('no section headers')

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        (name, stype, flags, addr, offset, size, link, info,
         addralign, entsize) = struct.unpack_from('<IIQQQQIIQQ', data, off)
        sections.append({
            'idx': i, 'name_off': name, 'type': stype, 'flags': flags,
            'addr': addr, 'offset': offset, 'size': size, 'link': link,
            'info': info, 'entsize': entsize,
        })

    # Section name string table
    shstr = sections[e_shstrndx]
    strtab_base = shstr['offset']
    for s in sections:
        end = data.index(b'\x00', strtab_base + s['name_off'])
        s['name'] = data[strtab_base + s['name_off']:end].decode()

    return e_entry, sections


def align_up(v, a):
    return (v + a - 1) // a * a


def slice_group(data, group):
    """Concatenate a group of address-ordered ELF sections, zero-filling
    gaps and SHT_NOBITS. Returns (bytes, base_addr)."""
    chunks = []
    cursor = group[0]['addr']
    for s in group:
        if s['addr'] < cursor:
            raise ElfError('overlapping alloc sections at 0x%x' % s['addr'])
        chunks.append(b'\x00' * (s['addr'] - cursor))
        cursor = s['addr']
        if s['type'] == SHT_PROGBITS:
            chunks.append(data[s['offset']:s['offset'] + s['size']])
        else:
            chunks.append(b'\x00' * s['size'])
        cursor += s['size']
    return b''.join(chunks), group[0]['addr']


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    inp, outp = sys.argv[1], sys.argv[2]
    data = open(inp, 'rb').read()
    e_entry, sections = parse_elf(data)

    alloc = [s for s in sections if s['flags'] & SHF_ALLOC and s['size'] > 0
             and s['type'] in (SHT_PROGBITS, SHT_NOBITS)]
    if not alloc:
        raise ElfError('no allocatable sections found')
    alloc.sort(key=lambda s: s['addr'])

    # Split into the RX text group and the RW data group. The linker script
    # guarantees: .text* sections first, then a page-aligned boundary, then
    # everything else.
    text_secs = [s for s in alloc if s['name'].startswith('.text')]
    data_secs = [s for s in alloc if not s['name'].startswith('.text')]
    if not text_secs:
        raise ElfError('no .text sections')
    if text_secs[-1]['addr'] > (data_secs[0]['addr'] if data_secs else 0) \
            and data_secs:
        raise ElfError('.text and data sections are interleaved')

    first_addr = text_secs[0]['addr']
    if first_addr % SECTION_ALIGNMENT != 0 or first_addr < SECTION_ALIGNMENT:
        raise ElfError('first alloc section at 0x%x; expected ImageBase+0x1000'
                       % first_addr)
    image_base = first_addr - SECTION_ALIGNMENT

    text_body, text_addr = slice_group(data, text_secs)
    text_rva = text_addr - image_base
    if text_rva != SECTION_ALIGNMENT:
        raise ElfError('text RVA 0x%x != 0x1000' % text_rva)
    text_vsize = len(text_body)
    text_mem_end = align_up(text_rva + text_vsize, SECTION_ALIGNMENT)

    if data_secs:
        data_body, data_addr = slice_group(data, data_secs)
        data_rva = data_addr - image_base
        if data_rva % SECTION_ALIGNMENT != 0:
            raise ElfError('data group RVA 0x%x not page aligned; '
                           'add ALIGN(0x1000) in the linker script' % data_rva)
        if data_rva < text_mem_end:
            raise ElfError('data group overlaps text group')
    else:
        data_body, data_rva = b'', text_mem_end
    data_vsize = len(data_body)
    data_mem_end = align_up(data_rva + data_vsize, SECTION_ALIGNMENT)

    # Collect R_X86_64_64 relocations targeting alloc sections.
    reloc_rvas = []
    for s in sections:
        if s['type'] != SHT_RELA:
            continue
        target = sections[s['info']] if s['info'] < len(sections) else None
        if target is None or not (target['flags'] & SHF_ALLOC):
            continue  # relocs against debug/note sections: irrelevant
        n = s['size'] // (s['entsize'] or 24)
        for i in range(n):
            r_offset, r_info, r_addend = struct.unpack_from(
                '<QQq', data, s['offset'] + i * 24)
            r_type = r_info & 0xFFFFFFFF
            if r_type == R_X86_64_64:
                rva = r_offset - image_base
                if not (text_rva <= rva < text_rva + text_vsize) and \
                   not (data_rva <= rva < data_rva + data_vsize):
                    raise ElfError('R_X86_64_64 outside image: 0x%x' % r_offset)
                reloc_rvas.append(rva)
            elif r_type in _ALLOWED_IGNORED:
                continue
            else:
                raise ElfError(
                    'unsupported relocation type %d in %s at 0x%x; rebuild the '
                    'loader with -mcmodel=large -fno-pic so every absolute '
                    'reference is R_X86_64_64' % (r_type, s['name'], r_offset))
    reloc_rvas = sorted(set(reloc_rvas))

    # Emit .reloc: base relocation blocks, one per 4KB page, entries are
    # 16-bit (offset:12 | type:4), block padded to 4 bytes.
    reloc = b''
    i = 0
    while i < len(reloc_rvas):
        page = reloc_rvas[i] & ~0xFFF
        offs = []
        while i < len(reloc_rvas) and (reloc_rvas[i] & ~0xFFF) == page:
            offs.append(reloc_rvas[i] & 0xFFF)
            i += 1
        block = struct.pack('<II', page,
                            8 + 2 * len(offs) + (2 if len(offs) % 2 else 0))
        entries = [(DIR64 << 12) | o for o in offs]
        if len(entries) % 2:
            entries.append(0)  # padding entry (ABSOLUTE)
        block += struct.pack('<%dH' % len(entries), *entries)
        reloc += block
    reloc_rva = data_mem_end if reloc else 0

    # ---- Layout -------------------------------------------------------------
    n_sections = 1 + (1 if data_secs else 0) + (1 if reloc else 0)
    headers_size = 0x80 + 4 + 20 + 0xF0 + 40 * n_sections
    size_of_headers = align_up(headers_size, FILE_ALIGNMENT)

    cursor = size_of_headers
    text_raw_ptr = cursor
    text_raw_size = align_up(text_vsize, FILE_ALIGNMENT)
    cursor += text_raw_size
    if data_secs:
        data_raw_ptr = cursor
        data_raw_size = align_up(data_vsize, FILE_ALIGNMENT)
        cursor += data_raw_size
    else:
        data_raw_ptr = data_raw_size = 0
    if reloc:
        reloc_raw_ptr = cursor
        reloc_raw_size = align_up(len(reloc), FILE_ALIGNMENT)
        cursor += reloc_raw_size
    else:
        reloc_raw_ptr = reloc_raw_size = 0

    size_of_image = data_mem_end + (align_up(len(reloc), SECTION_ALIGNMENT)
                                    if reloc else 0)
    entry_rva = e_entry - image_base
    if not (text_rva <= entry_rva < text_rva + text_vsize):
        raise ElfError('entry point 0x%x outside .text' % e_entry)

    # ---- Headers ------------------------------------------------------------
    out = bytearray()
    dos = bytearray(0x80)
    dos[0:2] = b'MZ'
    struct.pack_into('<I', dos, 0x3C, 0x80)  # e_lfanew
    out += dos

    out += b'PE\x00\x00'
    out += struct.pack('<HHIIIHH',
                       PE_MACHINE_X64, n_sections, 0, 0, 0,
                       0xF0, PE_CHARACTERISTICS)

    opt = bytearray(0xF0)
    struct.pack_into('<HBB', opt, 0, PE32PLUS_MAGIC, 14, 0)
    struct.pack_into('<III', opt, 4,
                     text_raw_size,                            # SizeOfCode
                     data_raw_size + reloc_raw_size,           # SizeOfInitializedData
                     0)  # SizeOfUninitializedData (bss folded into .data raw)
    struct.pack_into('<II', opt, 16, entry_rva, text_rva)
    struct.pack_into('<Q', opt, 24, image_base)
    struct.pack_into('<II', opt, 32, SECTION_ALIGNMENT, FILE_ALIGNMENT)
    struct.pack_into('<HHHHHH', opt, 40, 6, 0, 0, 0, 6, 0)
    struct.pack_into('<I', opt, 56, size_of_image)
    struct.pack_into('<I', opt, 60, size_of_headers)
    struct.pack_into('<I', opt, 64, 0)                        # CheckSum
    struct.pack_into('<HH', opt, 68, SUBSYSTEM_EFI_APPLICATION, 0)
    struct.pack_into('<QQQQ', opt, 72, 0x100000, 0x1000, 0x100000, 0x1000)
    struct.pack_into('<I', opt, 104, 0)   # LoaderFlags
    struct.pack_into('<I', opt, 108, 16)  # NumberOfRvaAndSizes
    if reloc:
        struct.pack_into('<II', opt, 112 + 5 * 8, reloc_rva, len(reloc))
    out += opt

    def section_header(name, vsize, vaddr, rawsize, rawptr, chars):
        return struct.pack('<8sIIIIIIHHI', name, vsize, vaddr, rawsize,
                           rawptr, 0, 0, 0, 0, chars)

    out += section_header(b'.text', text_vsize, text_rva, text_raw_size,
                          text_raw_ptr, SCN_TEXT)
    if data_secs:
        out += section_header(b'.data', data_vsize, data_rva, data_raw_size,
                              data_raw_ptr, SCN_DATA)
    if reloc:
        out += section_header(b'.reloc', len(reloc), reloc_rva,
                              reloc_raw_size, reloc_raw_ptr, SCN_RELOC)

    out += b'\x00' * (size_of_headers - len(out))
    out += text_body
    out += b'\x00' * (text_raw_size - text_vsize)
    if data_secs:
        out += data_body
        out += b'\x00' * (data_raw_size - data_vsize)
    if reloc:
        out += reloc
        out += b'\x00' * (reloc_raw_size - len(reloc))

    open(outp, 'wb').write(bytes(out))
    print('mkuefi: %s -> %s' % (inp, outp))
    print('  image base 0x%x, entry RVA 0x%x, %d base relocations, %d bytes'
          % (image_base, entry_rva, len(reloc_rvas), len(out)))


if __name__ == '__main__':
    try:
        main()
    except ElfError as e:
        print('mkuefi: error: %s' % e, file=sys.stderr)
        sys.exit(1)
