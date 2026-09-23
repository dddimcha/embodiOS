/*
 * uefi_loader.c - Direct UEFI boot loader for embodiOS (no GRUB).
 *
 * Built as a freestanding flat object, linked at ImageBase 0x10000000 with
 * relocations preserved (ld --emit-relocs), then converted to PE32+
 * BOOTX64.EFI by tools/mkuefi.py.
 *
 * Flow (design contract, SPEC-v0.6.0 WS-E):
 *   1. Locate the ESP via LoadedImage->DeviceHandle + Simple File System
 *      protocol; open \embodios.elf.
 *   2. Read the whole kernel into a high pool buffer (all file I/O happens
 *      BEFORE any low memory is touched, because the kernel's physical
 *      span (1 MiB..~128 MiB) contains firmware-owned holes on OVMF:
 *      ACPI NVS at 8-9 MiB and a BootServicesData pool area at 9-21 MiB;
 *      clobbering them while the FAT/DiskIo drivers still need their pool
 *      would crash the boot services).
 *   3. Parse the ELF64 header (ET_EXEC, EM_X86_64). For each PT_LOAD span
 *      [p_paddr, p_paddr+p_memsz), claim every FREE (conventional memory)
 *      sub-range with AllocatePages(AllocateAddress) so the final memory
 *      map marks the kernel's home as used; firmware-owned holes are left
 *      alone and simply overwritten later (embodiOS does not use ACPI).
 *   4. Build a minimal multiboot2 boot info at physical 0x9000 from the
 *      FINAL GetMemoryMap: header {total_size, reserved}, tag type 1
 *      cmdline ("uefi"), tag type 6 mmap (EfiConventionalMemory -> type 1,
 *      everything else -> type 2; entry_size=24, entry_version=0), end tag
 *      type 0 size 8. All tags 8-byte aligned.
 *   5. ExitBootServices(ImageHandle, MapKey); retry once on failure.
 *   6. AFTER boot services are gone: copy each PT_LOAD from the pool buffer
 *      to its physical address and zero the p_memsz - p_filesz tail.
 *   7. Drop 64 -> 32 bit: cli, lgdt 32-bit flat GDT, far jump to a 32-bit
 *      compat segment, disable paging (CR0.PG=0), CR4.PAE=0, EFER.LME=0,
 *      then EAX=0x36d76289 (multiboot2 magic), EBX=0x9000, jmp *_start
 *      (ELF e_entry, 0x100000-based).
 *
 * Early progress is reported both on the UEFI console (ConOut, UCS-2) and
 * directly on COM1 (0x3F8) so it is visible on QEMU's serial stdio.
 *
 * All UEFI-calling functions use the Microsoft x64 ABI (ms_abi). The file
 * is fully freestanding: no headers, no libc; the compiler-generated
 * memcpy/memset/memmove calls are satisfied by the implementations below.
 */

/* ------------------------------------------------------------------ */
/* Base types                                                          */
/* ------------------------------------------------------------------ */
typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;
typedef u64                 uefi_uintn;
typedef u64                 efi_status;
typedef void*               efi_handle;
typedef u64                 efi_physical_address;

#define EFIAPI __attribute__((ms_abi))

#define EFI_ERROR(s)    ((s) != 0)
#define EFI_SUCCESS     0ULL
#define EFI_ERR_BIT     (1ULL << 63)
#define EFI_INVALID_PARAMETER  (EFI_ERR_BIT | 2)
#define EFI_BUFFER_TOO_SMALL   (EFI_ERR_BIT | 5)
#define EFI_NOT_FOUND          (EFI_ERR_BIT | 14)

/* ------------------------------------------------------------------ */
/* GUIDs                                                               */
/* ------------------------------------------------------------------ */
struct efi_guid {
    u32 a;
    u16 b, c;
    u8  d[8];
};

/* EFI_LOADED_IMAGE_PROTOCOL_GUID */
static const struct efi_guid LOADED_IMAGE_GUID =
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
/* EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID */
static const struct efi_guid SIMPLE_FS_GUID =
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

/* ------------------------------------------------------------------ */
/* Minimal UEFI protocol/layout definitions (x86-64 native alignment)  */
/* ------------------------------------------------------------------ */
struct efi_table_header {
    u64 signature;
    u32 revision;
    u32 header_size;
    u32 crc32;
    u32 reserved;
};

struct efi_simple_text_output_protocol;
typedef efi_status (EFIAPI *efi_output_string_t)(
    struct efi_simple_text_output_protocol* self, const u16* string);

struct efi_simple_text_output_protocol {
    void* reset;
    efi_output_string_t output_string;
    /* remaining fields unused */
};

/* Memory services */
enum { ALLOCATE_ANY_PAGES = 0, ALLOCATE_MAX_ADDRESS = 1, ALLOCATE_ADDRESS = 2 };
enum { EFI_LOADER_CODE = 1, EFI_LOADER_DATA = 2,
       EFI_BOOT_SERVICES_CODE = 3, EFI_BOOT_SERVICES_DATA = 4,
       EFI_CONVENTIONAL_MEMORY = 7 };

struct efi_memory_descriptor {
    u32 type;
    u32 pad;
    u64 physical_start;
    u64 virtual_start;
    u64 number_of_pages;
    u64 attribute;
};

typedef efi_status (EFIAPI *efi_allocate_pages_t)(
    u32 type, u32 memory_type, uefi_uintn pages, efi_physical_address* memory);
typedef efi_status (EFIAPI *efi_get_memory_map_t)(
    uefi_uintn* memory_map_size, struct efi_memory_descriptor* memory_map,
    uefi_uintn* map_key, uefi_uintn* descriptor_size, u32* descriptor_version);
typedef efi_status (EFIAPI *efi_allocate_pool_t)(
    u32 pool_type, uefi_uintn size, void** buffer);
typedef efi_status (EFIAPI *efi_handle_protocol_t)(
    efi_handle handle, const struct efi_guid* protocol, void** interface);
typedef efi_status (EFIAPI *efi_exit_boot_services_t)(
    efi_handle image_handle, uefi_uintn map_key);

struct efi_boot_services {
    struct efi_table_header hdr;        /* 24 */
    void* raise_tpl;                    /* 24 */
    void* restore_tpl;                  /* 32 */
    efi_allocate_pages_t allocate_pages;/* 40 */
    void* free_pages;                   /* 48 */
    efi_get_memory_map_t get_memory_map;/* 56 */
    efi_allocate_pool_t allocate_pool;  /* 64 */
    void* free_pool;                    /* 72 */
    void* create_event;                 /* 80 */
    void* set_timer;                    /* 88 */
    void* wait_for_event;               /* 96 */
    void* signal_event;                 /* 104 */
    void* close_event;                  /* 112 */
    void* check_event;                  /* 120 */
    void* install_protocol_interface;   /* 128 */
    void* reinstall_protocol_interface; /* 136 */
    void* uninstall_protocol_interface; /* 144 */
    efi_handle_protocol_t handle_protocol; /* 152 */
    void* reserved;                     /* 160 */
    void* register_protocol_notify;     /* 168 */
    void* locate_handle;                /* 176 */
    void* locate_device_path;           /* 184 */
    void* install_configuration_table;  /* 192 */
    void* load_image;                   /* 200 */
    void* start_image;                  /* 208 */
    void* exit;                         /* 216 */
    void* unload_image;                 /* 224 */
    efi_exit_boot_services_t exit_boot_services; /* 232 */
    /* remaining fields unused */
};

struct efi_system_table {
    struct efi_table_header hdr;        /* 24 */
    u16* firmware_vendor;               /* 24 */
    u32 firmware_revision;              /* 32 */
    u32 pad0;                           /* 36 */
    efi_handle con_in_handle;           /* 40 */
    void* con_in;                       /* 48 */
    efi_handle con_out_handle;          /* 56 */
    struct efi_simple_text_output_protocol* con_out; /* 64 */
    efi_handle std_err_handle;          /* 72 */
    struct efi_simple_text_output_protocol* std_err; /* 80 */
    void* runtime_services;             /* 88 */
    struct efi_boot_services* boot_services;         /* 96 */
    u64 number_of_table_entries;        /* 104 */
    void* configuration_table;          /* 112 */
};

struct efi_loaded_image_protocol {
    u32 revision;
    u32 pad;
    efi_handle parent_handle;
    struct efi_system_table* system_table;
    efi_handle device_handle;
    void* file_path;
    void* reserved;
    u32 load_options_size;
    u32 pad2;
    void* load_options;
    void* image_base;
    u64 image_size;
    u32 image_code_type;
    u32 image_data_type;
    void* unload;
};

struct efi_file_protocol;
typedef efi_status (EFIAPI *efi_open_volume_t)(
    void* self, struct efi_file_protocol** root);
typedef efi_status (EFIAPI *efi_file_open_t)(
    struct efi_file_protocol* self, struct efi_file_protocol** new_handle,
    const u16* file_name, u64 open_mode, u64 attributes);
typedef efi_status (EFIAPI *efi_file_close_t)(struct efi_file_protocol* self);
typedef efi_status (EFIAPI *efi_file_read_t)(
    struct efi_file_protocol* self, uefi_uintn* buffer_size, void* buffer);
typedef efi_status (EFIAPI *efi_file_get_position_t)(
    struct efi_file_protocol* self, u64* position);
typedef efi_status (EFIAPI *efi_file_set_position_t)(
    struct efi_file_protocol* self, u64 position);

struct efi_simple_file_system_protocol {
    u64 revision;
    efi_open_volume_t open_volume;
};

struct efi_file_protocol {
    u64 revision;
    efi_file_open_t open;
    efi_file_close_t close;
    void* delete;
    efi_file_read_t read;
    void* write;
    efi_file_get_position_t get_position;
    efi_file_set_position_t set_position;
    /* remaining fields unused */
};

#define EFI_FILE_MODE_READ 0x1ULL

/* ------------------------------------------------------------------ */
/* ELF64                                                               */
/* ------------------------------------------------------------------ */
struct elf64_ehdr {
    u8  ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    u32 type, flags;
    u64 offset, vaddr, paddr, filesz, memsz, align;
};

#define ELF_ET_EXEC   2
#define ELF_EM_X86_64 62
#define ELF_PT_LOAD   1
#define MAX_PHDRS     16

/* ------------------------------------------------------------------ */
/* Multiboot2 boot info (synthesized at MBI_PHYS)                      */
/* ------------------------------------------------------------------ */
#define MBI_PHYS        0x9000ULL
#define MBI_PAGES       4           /* 16 KiB capacity */
#define MB2_TAG_END     0
#define MB2_TAG_CMDLINE 1
#define MB2_TAG_MMAP    6
#define MB2_MMAP_AVAILABLE 1
#define MB2_MMAP_RESERVED  2

#define KERNEL_CMDLINE  "uefi"

/* ------------------------------------------------------------------ */
/* COM1 serial debug (the practical log channel under QEMU -nographic) */
/* ------------------------------------------------------------------ */
#define COM1 0x3F8

static inline void outb(u16 port, u8 val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 inb(u16 port)
{
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* interrupts off */
    outb(COM1 + 3, 0x80);   /* DLAB */
    outb(COM1 + 0, 0x03);   /* 38400 baud lo */
    outb(COM1 + 1, 0x00);   /* hi */
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 4, 0x0B);   /* RTS/DSR */
}

static void serial_putc(char c)
{
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (u8)c);
}

static void serial_puts(const char* s)
{
    while (*s) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s++);
    }
}

static void serial_hex(u64 v)
{
    char buf[17];
    int i;
    for (i = 15; i >= 0; i--) {
        unsigned d = (unsigned)(v & 0xF);
        buf[i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    }
    buf[16] = 0;
    serial_puts("0x");
    serial_puts(buf);
}

static void serial_dec(u64 v)
{
    char buf[24];
    int i = 0;
    if (v == 0) {
        serial_putc('0');
        return;
    }
    while (v) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0)
        serial_putc(buf[--i]);
}

/* ------------------------------------------------------------------ */
/* UEFI console output (UCS-2)                                         */
/* ------------------------------------------------------------------ */
static struct efi_simple_text_output_protocol* g_con_out;

static void conout_puts(const char* s)
{
    u16 buf[128];
    u32 i = 0;
    if (!g_con_out)
        return;
    while (*s && i < sizeof(buf) / sizeof(buf[0]) - 2) {
        if (*s == '\n')
            buf[i++] = '\r';
        buf[i++] = (u16)*s++;
    }
    buf[i] = 0;
    g_con_out->output_string(g_con_out, buf);
}

static void log_puts(const char* s)
{
    serial_puts(s);
    conout_puts(s);
}

/* ------------------------------------------------------------------ */
/* Freestanding memory ops (also satisfy compiler-generated calls)     */
/* ------------------------------------------------------------------ */
void* memcpy(void* dst, const void* src, uefi_uintn n)
{
    u8* d = dst;
    const u8* s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void* memmove(void* dst, const void* src, uefi_uintn n)
{
    u8* d = dst;
    const u8* s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void* memset(void* dst, int c, uefi_uintn n)
{
    u8* d = dst;
    while (n--)
        *d++ = (u8)c;
    return dst;
}

/* ------------------------------------------------------------------ */
/* 32-bit transition GDT                                               */
/* ------------------------------------------------------------------ */
static const u64 gdt32[] = {
    0x0000000000000000ULL,  /* 0x00 null */
    0x00AF9A000000FFFFULL,  /* 0x08 64-bit code (unused, kept for sanity) */
    0x00CF9A000000FFFFULL,  /* 0x10 32-bit flat code, base 0 limit 4G D=1 */
    0x00CF92000000FFFFULL,  /* 0x18 32-bit flat data */
};

struct gdt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

static struct gdt_ptr gdt32_desc = {
    sizeof(gdt32) - 1,
    (u64)(const void*)gdt32,
};

/* ------------------------------------------------------------------ */
/* Final drop: 64-bit long mode -> 32-bit protected mode -> kernel     */
/*                                                                     */
/* Runs after ExitBootServices with firmware identity page tables      */
/* still active. mbi arrives in RBX (untouched by the mode switch),    */
/* entry in RSI.                                                       */
/* ------------------------------------------------------------------ */
static void __attribute__((noreturn)) enter_kernel(u32 entry, u32 mbi)
{
    __asm__ volatile(
        "cli\n\t"
        "lgdt %[gd]\n\t"
        /* far return into the 32-bit compatibility code segment (0x10) */
        "pushq $0x10\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        ".code32\n"
        "1:\n\t"
        "movl $0x18, %%eax\n\t"
        "movl %%eax, %%ds\n\t"
        "movl %%eax, %%es\n\t"
        "movl %%eax, %%ss\n\t"
        "movl %%eax, %%fs\n\t"
        "movl %%eax, %%gs\n\t"
        /* disable paging */
        "movl %%cr0, %%eax\n\t"
        "andl $0x7fffffff, %%eax\n\t"
        "movl %%eax, %%cr0\n\t"
        /* clear CR4.PAE */
        "movl %%cr4, %%eax\n\t"
        "andl $0xffffffdf, %%eax\n\t"
        "movl %%eax, %%cr4\n\t"
        /* clear EFER.LME -> legacy 32-bit protected mode */
        "movl $0xc0000080, %%ecx\n\t"
        "rdmsr\n\t"
        "andl $0xfffffeff, %%eax\n\t"
        "wrmsr\n\t"
        /* multiboot2 handoff: EAX=magic, EBX=mbi physical, jump _start */
        "movl $0x36d76289, %%eax\n\t"
        "jmp *%%esi\n\t"
        ".code64\n"
        :
        : [gd] "m" (gdt32_desc), "b" ((u64)mbi), "S" ((u64)entry)
        : "eax", "ecx", "edx", "memory", "cc");
    __builtin_unreachable();
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static struct efi_system_table* g_st;
static struct efi_boot_services* g_bs;

static void __attribute__((noreturn)) die(const char* msg, efi_status status)
{
    serial_puts("\nUEFI loader FATAL: ");
    serial_puts(msg);
    serial_puts(" status=");
    serial_hex(status);
    serial_puts("\n");
    for (;;)
        __asm__ volatile ("cli; hlt");
}

/* Claim every FREE (conventional-memory) page inside [addr, addr+size) so
 * the memory map marks the kernel's future home as used. Firmware-owned
 * holes (ACPI NVS, BootServicesData, ...) are left untouched here; their
 * contents are overwritten after ExitBootServices, when no firmware code
 * runs any more (embodiOS does not use ACPI). */
static void claim_span(u64 addr, u64 size,
                       struct efi_memory_descriptor* map, uefi_uintn map_size,
                       uefi_uintn desc_size)
{
    u64 end = addr + size;
    u64 entries = map_size / desc_size;
    u64 i;
    u8* e = (u8*)map;
    for (i = 0; i < entries; i++, e += desc_size) {
        struct efi_memory_descriptor* d = (struct efi_memory_descriptor*)e;
        u64 mstart = d->physical_start;
        u64 mend = mstart + d->number_of_pages * 4096ULL;
        u64 s, en;
        efi_physical_address base;
        efi_status st;
        if (d->type != EFI_CONVENTIONAL_MEMORY)
            continue;
        if (mend <= addr || mstart >= end)
            continue;
        s = mstart < addr ? addr : mstart;
        en = mend > end ? end : mend;
        base = s;
        st = g_bs->allocate_pages(ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                                  (en - s) >> 12, &base);
        if (EFI_ERROR(st)) {
            serial_puts("claim_span: AllocatePages failed at ");
            serial_hex(s);
            serial_puts(" status=");
            serial_hex(st);
            serial_puts("\n");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Multiboot2 info builder (writes at MBI_PHYS; no boot-service calls) */
/* ------------------------------------------------------------------ */
static u32 build_mb2_info(struct efi_memory_descriptor* map, uefi_uintn map_size,
                          uefi_uintn desc_size)
{
    u8* base = (u8*)(uefi_uintn)MBI_PHYS;
    u8* p = base + 8;               /* skip header, filled in at the end */
    u8* limit = base + MBI_PAGES * 4096 - 16;
    u64 entries = map_size / desc_size;
    u64 i;
    u32 mmap_count = 0;
    u8* mmap_tag;
    u8* e;

    /* tag type 1: command line */
    {
        const char* cmd = KERNEL_CMDLINE;
        u32 len = 0;
        u32 j;
        while (cmd[len])
            len++;
        *(u32*)p = MB2_TAG_CMDLINE;
        *(u32*)(p + 4) = 8 + len + 1;
        for (j = 0; j < len + 1; j++)
            p[8 + j] = (u8)cmd[j];
        p += (8 + len + 1 + 7) & ~7U;
    }

    /* tag type 6: memory map (entry_size=24, entry_version=0) */
    mmap_tag = p;
    p += 16;
    e = (u8*)map;
    {
        u64 prev_addr = 0, prev_len = 0;
        u32 prev_type = 0;
        int have_prev = 0;
        for (i = 0; i < entries; i++, e += desc_size) {
            struct efi_memory_descriptor* d = (struct efi_memory_descriptor*)e;
            u64 addr = d->physical_start;
            u64 len = d->number_of_pages * 4096ULL;
            u32 type = (d->type == EFI_CONVENTIONAL_MEMORY)
                       ? MB2_MMAP_AVAILABLE : MB2_MMAP_RESERVED;
            if (len == 0)
                continue;
            /* merge with previous entry if contiguous and same type */
            if (have_prev && prev_type == type && prev_addr + prev_len == addr) {
                prev_len += len;
                /* patch the previous entry's length */
                *(u64*)(p - 24 + 8) = prev_len;
                continue;
            }
            if (p + 24 > limit)
                break;               /* capacity guard (never hit in practice) */
            *(u64*)(p + 0) = addr;
            *(u64*)(p + 8) = len;
            *(u32*)(p + 16) = type;
            *(u32*)(p + 20) = 0;
            p += 24;
            mmap_count++;
            prev_addr = addr;
            prev_len = len;
            prev_type = type;
            have_prev = 1;
        }
    }
    *(u32*)(mmap_tag + 0) = MB2_TAG_MMAP;
    *(u32*)(mmap_tag + 4) = 16 + 24 * mmap_count;
    *(u32*)(mmap_tag + 8) = 24;      /* entry_size */
    *(u32*)(mmap_tag + 12) = 0;      /* entry_version */
    p = mmap_tag + ((16 + 24 * mmap_count + 7) & ~7U);

    /* end tag type 0 size 8 */
    *(u32*)p = MB2_TAG_END;
    *(u32*)(p + 4) = 8;
    p += 8;

    /* header */
    {
        u32 total = (u32)(p - base);
        *(u32*)base = total;
        *(u32*)(base + 4) = 0;
        return total;
    }
}

/* ------------------------------------------------------------------ */
/* efi_main                                                            */
/* ------------------------------------------------------------------ */
efi_status EFIAPI efi_main(efi_handle image, struct efi_system_table* st)
{
    efi_status status;
    struct efi_loaded_image_protocol* li;
    struct efi_simple_file_system_protocol* fs;
    struct efi_file_protocol* root;
    struct efi_file_protocol* file;
    struct elf64_ehdr ehdr;
    struct elf64_phdr phdrs[MAX_PHDRS];
    u8* kbuf;                   /* high pool buffer holding the whole kernel */
    u64 ksize;
    u32 entry;
    u32 i;
    /* Final memory map state, kept for the post-EBS claim/patch step */
    uefi_uintn map_size = 0, map_key = 0, desc_size = 0;
    u32 desc_version = 0;
    struct efi_memory_descriptor* map = 0;

    serial_init();
    g_st = st;
    g_bs = st->boot_services;
    g_con_out = st->con_out;

    log_puts("\nembodios UEFI loader v0.6.0 (direct boot, no GRUB)\n");

    /* 1. ESP via LoadedImage->DeviceHandle + Simple File System -------- */
    status = g_bs->handle_protocol(image, &LOADED_IMAGE_GUID, (void**)&li);
    if (EFI_ERROR(status))
        die("HandleProtocol(LoadedImage)", status);
    status = g_bs->handle_protocol(li->device_handle, &SIMPLE_FS_GUID, (void**)&fs);
    if (EFI_ERROR(status))
        die("HandleProtocol(SimpleFS)", status);
    status = fs->open_volume(fs, &root);
    if (EFI_ERROR(status))
        die("OpenVolume", status);

    {
        static const u16 path[] = {
            '\\', 'e', 'm', 'b', 'o', 'd', 'i', 'o', 's', '.', 'e', 'l', 'f', 0
        };
        status = root->open(root, &file, path, EFI_FILE_MODE_READ, 0);
    }
    if (EFI_ERROR(status))
        die("open \\embodios.elf", status);
    log_puts("found \\embodios.elf\n");

    /* 2. Read the whole kernel into a high pool buffer ------------------
     * All file I/O must finish before the kernel's physical span is
     * written: that span contains firmware-owned areas (ACPI NVS,
     * BootServicesData pool) which the FAT/DiskIo drivers still need. */
    status = file->set_position(file, 0xFFFFFFFFFFFFFFFFULL);
    if (EFI_ERROR(status))
        die("seek end", status);
    status = file->get_position(file, &ksize);
    if (EFI_ERROR(status))
        die("get file size", status);
    serial_puts("kernel file size ");
    serial_dec(ksize);
    serial_puts(" bytes\n");
    status = g_bs->allocate_pool(EFI_LOADER_DATA, ksize, (void**)&kbuf);
    if (EFI_ERROR(status))
        die("AllocatePool for kernel", status);
    serial_puts("kernel buffer at ");
    serial_hex((u64)(uefi_uintn)kbuf);
    serial_puts("\n");
    status = file->set_position(file, 0);
    if (EFI_ERROR(status))
        die("seek start", status);
    {
        /* chunked reads (some FAT drivers balk at very large requests) */
        u64 done = 0;
        while (done < ksize) {
            uefi_uintn chunk = 16 * 1024 * 1024;
            if (chunk > ksize - done)
                chunk = (uefi_uintn)(ksize - done);
            status = file->read(file, &chunk, kbuf + done);
            if (EFI_ERROR(status) || chunk == 0)
                die("read kernel", status);
            done += chunk;
        }
    }
    file->close(file);
    log_puts("kernel read complete\n");

    /* 3. ELF64 header + program headers -------------------------------- */
    memcpy(&ehdr, kbuf, sizeof(ehdr));
    if (ehdr.ident[0] != 0x7F || ehdr.ident[1] != 'E' ||
        ehdr.ident[2] != 'L' || ehdr.ident[3] != 'F' ||
        ehdr.ident[4] != 2 || ehdr.ident[5] != 1)
        die("not an ELF64 LE file", 0);
    if (ehdr.type != ELF_ET_EXEC || ehdr.machine != ELF_EM_X86_64)
        die("ELF not ET_EXEC/EM_X86_64", 0);
    if (ehdr.phentsize != sizeof(struct elf64_phdr) || ehdr.phnum > MAX_PHDRS)
        die("bad program headers", 0);
    memcpy(phdrs, kbuf + ehdr.phoff, ehdr.phnum * sizeof(struct elf64_phdr));

    entry = 0;
    for (i = 0; i < ehdr.phnum; i++) {
        struct elf64_phdr* ph = &phdrs[i];
        if (ph->type != ELF_PT_LOAD || ph->memsz == 0)
            continue;
        serial_puts("LOAD paddr=");
        serial_hex(ph->paddr);
        serial_puts(" filesz=");
        serial_hex(ph->filesz);
        serial_puts(" memsz=");
        serial_hex(ph->memsz);
        serial_puts("\n");
        if (ehdr.entry >= ph->paddr && ehdr.entry < ph->paddr + ph->memsz
            && ehdr.entry < 0x100000000ULL)
            entry = (u32)ehdr.entry;
    }
    if (entry == 0)
        die("entry point outside loaded segments", 0);
    serial_puts("kernel entry ");
    serial_hex(entry);
    serial_puts("\n");

    /* 4. multiboot2 info pages pinned at 0x9000 ------------------------ */
    {
        efi_physical_address mbi = MBI_PHYS;
        status = g_bs->allocate_pages(ALLOCATE_ADDRESS, EFI_LOADER_DATA,
                                      MBI_PAGES, &mbi);
        if (EFI_ERROR(status))
            die("AllocatePages @0x9000", status);
    }

    /* 5. Claim the free sub-ranges of every PT_LOAD span, so the final
     *    memory map marks the kernel's home as used.                    */
    {
        uefi_uintn cur;
        status = g_bs->get_memory_map(&map_size, 0, &map_key,
                                      &desc_size, &desc_version);
        if (status != EFI_BUFFER_TOO_SMALL)
            die("GetMemoryMap size query", status);
        map_size += 8 * desc_size;
        status = g_bs->allocate_pool(EFI_LOADER_DATA, map_size, (void**)&map);
        if (EFI_ERROR(status))
            die("AllocatePool for memory map", status);
        cur = map_size;
        status = g_bs->get_memory_map(&cur, map, &map_key,
                                      &desc_size, &desc_version);
        if (EFI_ERROR(status))
            die("GetMemoryMap", status);
        for (i = 0; i < ehdr.phnum; i++) {
            struct elf64_phdr* ph = &phdrs[i];
            u64 base, span;
            if (ph->type != ELF_PT_LOAD || ph->memsz == 0)
                continue;
            base = ph->paddr & ~0xFFFULL;
            span = ((ph->paddr + ph->memsz + 0xFFFULL) & ~0xFFFULL) - base;
            claim_span(base, span, map, cur, desc_size);
        }
    }

    /* 6. final memory map -> mb2 mmap tag -> ExitBootServices ---------- */
    {
        int attempt;

        for (attempt = 0; attempt < 2; attempt++) {
            uefi_uintn cur_size = map_size;
            u64 entries, ram_bytes = 0, j;
            u8* e;
            status = g_bs->get_memory_map(&cur_size, map, &map_key,
                                          &desc_size, &desc_version);
            if (EFI_ERROR(status))
                die("GetMemoryMap", status);

            /* log summary BEFORE ExitBootServices (serial only: printing
             * to ConOut could allocate and invalidate map_key) */
            entries = cur_size / desc_size;
            e = (u8*)map;
            for (j = 0; j < entries; j++, e += desc_size) {
                struct efi_memory_descriptor* d = (struct efi_memory_descriptor*)e;
                if (d->type == EFI_CONVENTIONAL_MEMORY)
                    ram_bytes += d->number_of_pages * 4096ULL;
            }
            serial_puts("EFI memory map: ");
            serial_dec(entries);
            serial_puts(" entries, conventional RAM ");
            serial_dec(ram_bytes / (1024 * 1024));
            serial_puts(" MiB\n");

            {
                u32 total = build_mb2_info(map, cur_size, desc_size);
                serial_puts("mb2 info ");
                serial_dec(total);
                serial_puts(" bytes at ");
                serial_hex(MBI_PHYS);
                serial_puts("\n");
            }

            status = g_bs->exit_boot_services(image, map_key);
            if (!EFI_ERROR(status))
                break;
            serial_puts("ExitBootServices failed, retrying\n");
        }
        if (EFI_ERROR(status))
            die("ExitBootServices", status);
    }
    serial_puts("boot services exited\n");

    /* 7. Boot services are gone: copy each PT_LOAD to its physical
     *    address and zero the bss tail. Overwriting firmware-owned holes
     *    inside the span is safe now.                                  */
    for (i = 0; i < ehdr.phnum; i++) {
        struct elf64_phdr* ph = &phdrs[i];
        if (ph->type != ELF_PT_LOAD || ph->memsz == 0)
            continue;
        memcpy((void*)(uefi_uintn)ph->paddr, kbuf + ph->offset, ph->filesz);
        if (ph->memsz > ph->filesz)
            memset((void*)(uefi_uintn)(ph->paddr + ph->filesz), 0,
                   ph->memsz - ph->filesz);
    }

    /* 8. drop to 32-bit and jump to _start (does not return) ----------- */
    enter_kernel(entry, (u32)MBI_PHYS);
}
