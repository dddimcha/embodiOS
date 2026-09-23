/* EMBODIOS Native Kernel Entry Point */
#include <embodios/kernel.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/cpu.h>
#include <embodios/percpu.h>
#include <embodios/model.h>
#include <embodios/interrupt.h>
#include <embodios/task.h>
#include <embodios/ai.h>
#include <embodios/dma.h>
#include <embodios/pci.h>
#include <embodios/virtio_blk.h>
#include <embodios/nvme.h>
#include <embodios/virtio_mmio.h>
#include <embodios/virtio_net.h>
#include <embodios/e1000e.h>
#include <embodios/tcpip.h>
#include <embodios/exo.h>
#include <embodios/hal_timer.h>
#include <embodios/can.h>
#include <embodios/simd_kernels.h>
#include <embodios/model_registry.h>
#include <embodios/test.h>
#include <embodios/ui.h>
#if defined(__x86_64__)
#include "../../include/arch/x86_64/paging.h"
#endif

/* Kernel version info */
/* Single source of truth for the kernel version. create_iso.sh
   extracts this via grep to keep the ISO manifest in sync. */
const char* kernel_version = "v0.7.0";
const char* kernel_build = __DATE__ " " __TIME__;

/* External symbols from linker script */
#ifdef __APPLE__
/* On macOS, we'll define dummy symbols for now */
char _kernel_start[1] = {0};
char _kernel_end[1] = {0};
char _bss_start[1] = {0};
char _bss_end[1] = {0};
char _model_weights_start[1] = {0};
char _model_weights_end[1] = {0};
#else
extern char _kernel_start[];
extern char _kernel_end[];
extern char _bss_start[];
extern char _bss_end[];
extern char _model_weights_start[];
extern char _model_weights_end[];
#endif

/* Multiboot2 info from boot.S */
#if defined(__x86_64__)
extern uint32_t multiboot_magic;
extern uint32_t multiboot_info;

/* Multiboot2 constants */
#define MULTIBOOT2_MAGIC 0x36d76289
#define MULTIBOOT2_TAG_CMDLINE 1
#define MULTIBOOT2_TAG_END 0

/* Multiboot2 tag structure */
struct multiboot2_tag {
    uint32_t type;
    uint32_t size;
};

/* Multiboot2 command line tag */
struct multiboot2_tag_cmdline {
    uint32_t type;
    uint32_t size;
    char string[0];
};
#endif

/* Architecture-specific initialization */
extern void arch_early_init(void);
extern void arch_cpu_init(void);
extern void arch_interrupt_init(void);

/* Memory management initialization */
extern void pmm_init(void* mem_start, size_t mem_size);
extern void vmm_init(void);
extern void slab_init(void);

/* Model runtime */
static struct embodios_model* ai_model = NULL;

/* Direct serial output for debug (before console init) */
#if defined(__x86_64__)
static inline void outb_debug(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb_debug(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void debug_serial_char(char c) {
    while (!(inb_debug(0x3FD) & 0x20));
    outb_debug(0x3F8, c);
}
#elif defined(__aarch64__)
/* ARM64: Use assembly UART for HVF compatibility */
extern void uart_putchar(char c);
static inline void debug_serial_char(char c) {
    uart_putchar(c);
}
#else
static inline void debug_serial_char(char c) { (void)c; }
#endif

/* Simple strstr implementation for cmdline parsing */
static char* kernel_strstr(const char* haystack, const char* needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return (char*)haystack;

    while (*haystack) {
        size_t i;
        for (i = 0; i < needle_len && haystack[i] == needle[i]; i++);
        if (i == needle_len) return (char*)haystack;
        haystack++;
    }
    return NULL;
}

#if defined(__x86_64__)
/* Global variable to store test name passed via environment */
extern char __test_name_start[];
extern char __test_name_end[];
static char test_mode_name[64] = {0};

/* Xen PVH hvm_start_info (QEMU -kernel direct boot passes its physical
 * address in ebx; boot.S stores it in multiboot_info). Identity-mapped,
 * so the cmdline physical address can be dereferenced directly. */
#define XEN_HVM_START_MAGIC 0x336ec578U
struct hvm_start_info_k {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t nr_modules;
    uint64_t modlist_paddr;
    uint64_t cmdline_paddr;
    uint64_t rsdp_paddr;
};

/* Fetch the kernel command line from the boot environment.
 * Supports multiboot2 (GRUB/ISO) and Xen PVH (QEMU -kernel ... -append). */
static const char* get_boot_cmdline(void)
{
    /* Check if we have valid multiboot2 info */
    if (multiboot_magic == MULTIBOOT2_MAGIC) {
        /* Multiboot2 info starts with total size and reserved field */
        uint32_t* mbi = (uint32_t*)(uintptr_t)multiboot_info;
        uint32_t total_size = mbi[0];

        /* Iterate through tags */
        struct multiboot2_tag* tag = (struct multiboot2_tag*)&mbi[2];
        uintptr_t end = (uintptr_t)mbi + total_size;

        while ((uintptr_t)tag < end && tag->type != MULTIBOOT2_TAG_END) {
            if (tag->type == MULTIBOOT2_TAG_CMDLINE) {
                struct multiboot2_tag_cmdline* cmdline_tag =
                    (struct multiboot2_tag_cmdline*)tag;
                return cmdline_tag->string;
            }

            /* Move to next tag (tags are 8-byte aligned) */
            tag = (struct multiboot2_tag*)((uintptr_t)tag + ((tag->size + 7) & ~7));
        }
        return NULL;
    }

    /* QEMU -kernel PVH boot: multiboot_info holds hvm_start_info paddr */
    if (multiboot_info != 0) {
        struct hvm_start_info_k* info =
            (struct hvm_start_info_k*)(uintptr_t)multiboot_info;
        if (info->magic == XEN_HVM_START_MAGIC && info->cmdline_paddr != 0) {
            return (const char*)(uintptr_t)info->cmdline_paddr;
        }
    }

    return NULL;
}

/* Parse boot cmdline and check for test mode parameter */
static void check_test_mode_cmdline(void)
{
    bool found_cmdline = false;
    const char* cmdline = get_boot_cmdline();

    if (cmdline) {
        console_printf("Kernel cmdline: %s\n", cmdline);
        found_cmdline = true;

        /* Check for "test" parameter - run all tests */
        if (kernel_strstr(cmdline, "test")) {
                    /* Check if it's "runtest=<name>" for single test */
                    char* runtest = kernel_strstr(cmdline, "runtest=");
                    if (runtest) {
                        /* Extract test name after "runtest=" */
                        const char* test_name = runtest + 8;  /* strlen("runtest=") */

                        /* Find end of test name (space or null) */
                        size_t name_len = 0;
                        while (test_name[name_len] && test_name[name_len] != ' ') {
                            name_len++;
                        }

                        /* Copy test name to temporary buffer */
                        char name_buf[64];
                        if (name_len < sizeof(name_buf)) {
                            size_t i;
                            for (i = 0; i < name_len; i++) {
                                name_buf[i] = test_name[i];
                            }
                            name_buf[i] = '\0';

                            console_printf("Running single test: %s\n", name_buf);
                            test_run_single(name_buf);
                        }
                    } else {
                        /* Just "test" - run all tests */
                        console_printf("Running all tests...\n");
                        test_run_all();
                    }

                    return;
        }
    }

    /* Fallback: For QEMU -kernel boot without multiboot2 loader,
     * check if we should run tests automatically.
     * This is a temporary workaround until we have proper GRUB integration.
     */
    #ifdef AUTO_RUN_TESTS
    if (!found_cmdline) {
        console_printf("Auto-running tests (no cmdline found)...\n");
        test_run_all();
    }
    #endif
}
#endif

/* True once the PIT IRQ is unmasked and IF=1 (not in 'poll' fallback mode).
 * kernel_loop uses this to decide whether hlt is safe while idle. */
static bool interrupts_enabled = false;

bool kernel_interrupts_enabled(void)
{
    return interrupts_enabled;
}

/* Count of hlt idle sleeps in kernel_loop (power management telemetry) */
static uint64_t idle_hlt_count = 0;

uint64_t kernel_idle_hlt_count(void)
{
    return idle_hlt_count;
}

void kernel_main(void)
{
    /* Debug: Mark kernel_main entry */

    /* Early architecture setup */
    arch_early_init();

    /* Initialize console for output */
    console_init();

    /* Interrupt infrastructure: IDT (exceptions -> panic) + remapped PIC.
     * IRQ lines stay masked until arch_enable_interrupts(). */
    arch_interrupt_init();

    /* Test mode runs headless under the QEMU test harness: keep the output
     * free of ANSI escapes so scripts/run_kernel_tests.sh can grep the
     * [TEST]/[PASS]/summary markers reliably. */
#if defined(__x86_64__)
    {
        const char* early_cmdline = get_boot_cmdline();
        if (early_cmdline && kernel_strstr(early_cmdline, "test")) {
            ui_set_color(0);
        }
    }
#endif

    /* Startup banner (logo + version + motto) */
    ui_banner();

    console_printf("EMBODIOS Native Kernel %s\n", kernel_version);
    console_printf("Build: %s\n", kernel_build);
    console_printf("Kernel: %p - %p\n", _kernel_start, _kernel_end);
    
    /* Note: BSS is zeroed in boot.S (_start) before the page tables and
     * stacks in .bss are set up, so all static storage is clean by now. */
    
    /* CPU initialization */
    console_printf("Initializing CPU features...\n");
    arch_cpu_init();

    /* SIMD kernel dispatch: probe AVX2 (CPUID + XCR0), install the fastest
     * safe quantized vec_dot kernels, print the selected backend. */
    simd_kernels_init();

    /* HAL timer (TSC/HPET/PIT): без этого hal_timer_get_milliseconds()
     * всегда 0 — ломаются таймауты и интервалы tcpip/exo (discovery,
     * HTTP-таймауты). arch_timer_init() вызван из arch_cpu_init(). */
    console_printf("Initializing HAL timer...\n");
    hal_timer_init();

    /* Memory management setup */
    console_printf("Initializing memory management...\n");
    void* mem_start = (void*)ALIGN_UP((uintptr_t)_kernel_end, PAGE_SIZE);
#if defined(__x86_64__)
    /* Detect physical RAM from the boot environment (multiboot2 mmap under
     * GRUB/ISO, Xen PVH hvm_start_info memmap under QEMU -kernel, or a
     * conservative 1GB fallback), extend the identity map beyond the
     * boot-time 1GB, and hand the usable regions to the PMM. */
    {
        struct boot_mem_region boot_regions[BOOT_MEM_MAX_REGIONS];
        size_t num_boot = arch_detect_memory(boot_regions, BOOT_MEM_MAX_REGIONS);
        uint64_t map_ceiling = arch_identity_map_ram(boot_regions, num_boot);

        struct pmm_region pmm_regions[BOOT_MEM_MAX_REGIONS];
        size_t num_pmm = 0;
        for (size_t i = 0; i < num_boot; i++) {
            uint64_t base = boot_regions[i].base;
            uint64_t end = base + boot_regions[i].size;
            /* PMM manages memory after the kernel; only use mapped RAM */
            if (base < (uint64_t)(uintptr_t)mem_start) {
                base = (uint64_t)(uintptr_t)mem_start;
            }
            if (end > map_ceiling) {
                end = map_ceiling;
            }
            if (end <= base) {
                continue;
            }
            pmm_regions[num_pmm].base = base;
            pmm_regions[num_pmm].size = end - base;
            num_pmm++;
        }
        pmm_init_regions(mem_start, pmm_regions, num_pmm);
    }
#else
    /* Non-x86 fallback: assume 1GB of RAM */
    size_t total_ram = 1UL * 1024 * 1024 * 1024;
    size_t kernel_size = (uintptr_t)mem_start - (uintptr_t)_kernel_start;
    size_t mem_size = total_ram - kernel_size;
    pmm_init(mem_start, mem_size);
#endif
    vmm_init();
    slab_init();
    
    /* Initialize heap for AI workloads */
    console_printf("Initializing heap allocator...\n");
    heap_init();

    /* Initialize per-CPU data structures */
    console_printf("Initializing per-CPU data structures...\n");
    percpu_init();

    /* Initialize SMP (Symmetric Multi-Processing) */
    console_printf("Initializing SMP...\n");
    arch_smp_init();

    /* Initialize DMA subsystem */
    console_printf("Initializing DMA subsystem...\n");
    dma_init();

    /* Initialize PCI subsystem */
    console_printf("Initializing PCI subsystem...\n");
    pci_init();

    /* VirtIO block driver for loading models from disk */
    console_printf("Initializing VirtIO block driver...\n");
#ifdef __aarch64__
    /* ARM64: Use VirtIO-MMIO (QEMU virt machine uses MMIO, not PCI) */
    virtio_mmio_init();
#else
    /* x86_64: Use VirtIO-PCI */
    virtio_blk_init();
#endif

    /* NVMe block driver for high-performance storage */
    console_printf("Initializing NVMe driver...\n");
    nvme_init();

    /* Optional: Print NVMe info and run self-tests */
    #ifdef NVME_RUN_TESTS
    console_printf("Running NVMe diagnostics...\n");
    nvme_print_info();
    nvme_run_tests();
    #endif

    /* VirtIO network driver */
    console_printf("Initializing VirtIO network driver...\n");
    virtio_net_init();

    /* Intel e1000e network driver (no-op if device absent; tcpip picks
     * whichever NIC is ready: virtio-net first, e1000e as fallback) */
    e1000e_init();

    /* TCP/IP stack */
    console_printf("Initializing TCP/IP stack...\n");
    tcpip_init();

    /* CAN bus driver */
    console_printf("Initializing CAN bus driver...\n");
    can_init(NULL);

    /* Initialize task scheduler */
    console_printf("Initializing task scheduler...\n");
    scheduler_init();

    /* Adopt this boot context as task 'main' so the preemptive scheduler
     * can suspend/resume kernel_loop like any other task. Must happen
     * before sti (IRQ0 -> scheduler_tick needs a valid current_task). */
    scheduler_start();

    /* Run scheduler tests */
    #ifdef SCHEDULER_RUN_TESTS
    scheduler_test_init();
    #endif

    /* Initialize AI runtime */
    console_printf("Initializing AI runtime...\n");
    model_runtime_init();

    /* Check for embedded GGUF model */
    extern int gguf_model_embedded(void);
    extern const uint8_t* get_embedded_gguf_model(size_t* out_size);

    if (gguf_model_embedded()) {
        size_t gguf_size = 0;
        const uint8_t* gguf_data = get_embedded_gguf_model(&gguf_size);
        (void)gguf_data;
        if (gguf_data && gguf_size > 0) {
            ui_boot_model_info();
        } else {
            ui_warn("Embedded GGUF model is present but unreadable");
        }
    } else {
        ui_boot_model_info();
    }
    
    /* Initialize command processor (works without a loaded AI model too) */
    command_processor_init(ai_model);

    /* Interrupt mode decision: PIT IRQ0 + preemptive scheduling are enabled
     * at the end of kernel_main (see below), unless the boot cmdline asks
     * for the legacy polling mode ("poll"). */

    console_printf("[DEBUG] About to call constructors...\n");

    /* Call C++ global constructors for test registration */
    extern void (*__init_array_start[])(void);
    extern void (*__init_array_end[])(void);
    console_printf("[DEBUG] Calling constructors from %p to %p\n",
                   __init_array_start, __init_array_end);
    for (void (**ctor)(void) = __init_array_start; ctor < __init_array_end; ctor++) {
        /* Skip obviously invalid pointers (NULL or > 16MB kernel space) */
        if (*ctor == NULL || (uintptr_t)*ctor > 0x2000000 || (uintptr_t)*ctor < 0x100000) {
            console_printf("[DEBUG] Skipping invalid constructor at %p\n", *ctor);
            continue;
        }
        console_printf("[DEBUG] Calling constructor at %p\n", *ctor);
        (*ctor)();
    }
    console_printf("[DEBUG] Constructors done\n");

    ui_ok("EMBODIOS Ready");
    console_printf("\n");

    /* TEMPORARY: Manually run PMM test to verify framework works */
    /* TODO: Fix multiboot2 cmdline parsing for QEMU -kernel boot */
    /* Disabled - test framework calls shutdown on completion */
    /* console_printf("[DEBUG] Manually running PMM test...\n");
    test_run_single("pmm"); */

    /* Check for test mode command-line parameter (multiboot2 cmdline under
     * GRUB/ISO or PVH hvm_start_info cmdline under QEMU -kernel -append).
     * In test mode the framework runs the tests and shuts QEMU down via
     * the isa-debug-exit device instead of entering the shell loop. */
    #if defined(__x86_64__)
    check_test_mode_cmdline();
    #endif

    /* Auto-run benchmark for testing */
    #ifdef AUTO_BENCHMARK
    console_printf("Auto-running benchmark...\n");
    process_command("benchmark");
    #endif

    /* === Interrupt enable point ===
     * Everything the timer tick path can touch (heap, console, model
     * runtime, scheduler) is initialized by now. Enable PIT ticks and
     * preemptive scheduling unless the boot cmdline forces polling mode. */
#if defined(__x86_64__)
    {
        const char* cmdline = get_boot_cmdline();
        bool want_poll = cmdline && kernel_strstr(cmdline, "poll");
        if (!want_poll) {
            extern void hal_timer_enable(void);
            hal_timer_enable();        /* ungate PIT tick counting */
            scheduler_register_timer();/* scheduler_tick on every IRQ0 */
            arch_enable_interrupts();  /* unmask IRQ0 + sti (PIT only if LAPIC inactive) */
            interrupts_enabled = true;
            extern int lapic_timer_active(void);
            console_printf("Interrupts: ENABLED (%s, preemptive scheduling)\n",
                           lapic_timer_active()
                               ? "LAPIC timer tick @ 1000 Hz (legacy chain @ 100 Hz)"
                               : "PIT IRQ0 @ 100 Hz");
        } else {
            console_printf("Interrupts: DISABLED (cmdline 'poll' -> legacy polling mode)\n");
        }
    }
#endif

    /* Main kernel loop */
    kernel_loop();
}

void kernel_loop(void)
{
    char cmd_buffer[256];
    size_t cmd_pos = 0;
    bool prompt_shown = false;


    while (1) {
        if (!prompt_shown) {
            ui_prompt();
            console_flush();
            prompt_shown = true;
        }

        int c = console_getchar();
        if (c == -1) {
            /* Нет ввода: обслужить фоновые подсистемы (exo: discovery,
             * тензорный транспорт, OpenAI API → tcpip_poll внутри) */
            exo_poll();
            /* Yield to other tasks if scheduler is active */
            schedule();
            /* Sleep until the next IRQ (PIT tick wakes us at 100 Hz).
             * Only safe when interrupts are actually enabled - in poll
             * fallback mode hlt would never wake. */
            if (interrupts_enabled) {
                idle_hlt_count++;
                __asm__ volatile("hlt");
            }
            continue;
        }

        /* Line editing (как console_readline, но неблокирующее ожидание —
         * иначе exo_poll() не выполнялся бы, пока shell ждёт ввод) */
        if (c == '\n' || c == '\r') {
            console_putchar('\n');
            cmd_buffer[cmd_pos] = '\0';
            cmd_pos = 0;
            prompt_shown = false;

            if (cmd_buffer[0] != '\0') {
                process_command(cmd_buffer);
            }

            /* Фоновый шаг и после команды */
            exo_poll();
            schedule();
        } else if (c == '\b' || c == 127) { /* Backspace */
            if (cmd_pos > 0) {
                cmd_pos--;
                console_putchar('\b');
                console_putchar(' ');
                console_putchar('\b');
            }
        } else if (c >= 32 && c < 127 && cmd_pos < sizeof(cmd_buffer) - 1) {
            cmd_buffer[cmd_pos++] = (char)c;
            console_putchar((char)c);
        }
    }
}