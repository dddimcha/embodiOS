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
#include <embodios/tcpip.h>
#include <embodios/can.h>
#include <embodios/model_registry.h>

/* Kernel version info */
const char* kernel_version = "EMBODIOS v0.1.0-native";
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

void kernel_main(void)
{
    /* Debug: Mark kernel_main entry */
    debug_serial_char('K');

    /* Early architecture setup */
    debug_serial_char('a');
    arch_early_init();
    debug_serial_char('b');

    /* Initialize console for output */
    debug_serial_char('c');
    console_init();
    debug_serial_char('d');
    console_printf("EMBODIOS Native Kernel %s\n", kernel_version);
    debug_serial_char('e');
    console_printf("Build: %s\n", kernel_build);
    console_printf("Kernel: %p - %p\n", _kernel_start, _kernel_end);
    
    /* Note: BSS is NOT cleared here because boot stack is in .bss and is in use.
     * The multiboot loader should have already zeroed BSS.
     * If needed, clear specific subsections before use. */
    
    /* CPU initialization */
    console_printf("Initializing CPU features...\n");
    debug_serial_char('f');
    arch_cpu_init();
    debug_serial_char('g');
    
    /* Memory management setup */
    console_printf("Initializing memory management...\n");
    /* Calculate available memory after kernel
     * Currently limited to 1GB due to page table setup in boot.S
     * TODO: Extend page tables for larger memory support */
    size_t total_ram = 1UL * 1024 * 1024 * 1024; /* 1GB - limited by page tables */
    void* mem_start = (void*)ALIGN_UP((uintptr_t)_kernel_end, PAGE_SIZE);
    size_t kernel_size = (uintptr_t)mem_start - (uintptr_t)_kernel_start;
    size_t mem_size = total_ram - kernel_size;
    pmm_init(mem_start, mem_size);
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

    /* TCP/IP stack */
    console_printf("Initializing TCP/IP stack...\n");
    tcpip_init();

    /* CAN bus driver */
    console_printf("Initializing CAN bus driver...\n");
    can_init(NULL);

    /* Initialize task scheduler */
    console_printf("Initializing task scheduler...\n");
    scheduler_init();

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
        if (gguf_data && gguf_size > 0) {
            console_printf("GGUF model embedded: %zu MB\n", gguf_size / (1024*1024));
            console_printf("Use 'benchmark' command to test inference\n");
        }
    } else {
        console_printf("No GGUF model embedded\n");
    }
    
    /* Initialize command processor */
    if (ai_model) {
        console_printf("Initializing AI command processor...\n");
        command_processor_init(ai_model);
    }

    /* Enable interrupts - DISABLED for UEFI */
    /* arch_enable_interrupts(); */

    console_printf("\nEMBODIOS Ready (polling mode - no interrupts).\n");
    console_printf("Type 'help' for available commands.\n\n");
    
    /* Auto-run benchmark for testing */
    #ifdef AUTO_BENCHMARK
    console_printf("Auto-running benchmark...\n");
    process_command("benchmark");
    #endif
    /* Main kernel loop */
    kernel_loop();
}

void kernel_loop(void)
{
    char cmd_buffer[256];
    
    while (1) {
        console_printf("> ");
        console_readline(cmd_buffer, sizeof(cmd_buffer));
        
        if (cmd_buffer[0] != '\0') {
            process_command(cmd_buffer);
        }
        
        /* Yield to other tasks if scheduler is active */
        schedule();
    }
}