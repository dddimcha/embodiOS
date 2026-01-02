/* EMBODIOS Native Kernel Entry Point */
#include <embodios/kernel.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/cpu.h>
#include <embodios/model.h>
#include <embodios/interrupt.h>
#include <embodios/task.h>
#include <embodios/ai.h>
#include <embodios/dma.h>

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

void kernel_main(void)
{
    /* Early architecture setup */
    arch_early_init();
    
    /* Initialize console for output */
    console_init();
    console_printf("EMBODIOS Native Kernel %s\n", kernel_version);
    console_printf("Build: %s\n", kernel_build);
    console_printf("Kernel: %p - %p\n", _kernel_start, _kernel_end);
    
    /* Note: BSS is NOT cleared here because boot stack is in .bss and is in use.
     * The multiboot loader should have already zeroed BSS.
     * If needed, clear specific subsections before use. */
    
    /* CPU initialization */
    console_printf("Initializing CPU features...\n");
    arch_cpu_init();
    
    /* Memory management setup */
    console_printf("Initializing memory management...\n");
    size_t mem_size = 256 * 1024 * 1024; /* Default 256MB */
    void* mem_start = (void*)ALIGN_UP((uintptr_t)_kernel_end, PAGE_SIZE);
    pmm_init(mem_start, mem_size);
    vmm_init();
    slab_init();
    
    /* Initialize heap for AI workloads */
    console_printf("Initializing heap allocator...\n");
    heap_init();

    /* Initialize DMA subsystem */
    console_printf("Initializing DMA subsystem...\n");
    dma_init();

    /* Initialize task scheduler */
    console_printf("Initializing task scheduler...\n");
    scheduler_init();
    
    /* Initialize AI runtime */
    console_printf("Initializing AI runtime...\n");
    model_runtime_init();

    /* Load TinyStories-15M model for interactive use */
    extern void tinystories_interactive_init(void);
    tinystories_interactive_init();

    /* Try loading embedded GGUF model first */
#if 0
    /* Temporarily disabled - model embedding not configured */
    extern const uint8_t _binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_start[];
    extern const uint8_t _binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_end[];
    extern int gguf_integer_load(void* data, size_t size);

    size_t gguf_size = (size_t)(_binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_end -
                                 _binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_start);
    if (gguf_size > 0) {
        console_printf("Loading GGUF model (%zu MB)...\n", gguf_size / (1024*1024));

        if (gguf_integer_load((void*)_binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_start, gguf_size) == 0) {
            console_printf("GGUF model loaded successfully!\n");
        } else {
            console_printf("Failed to load GGUF model\n");
        }
    } else {
        console_printf("No embedded GGUF model found\n");
    }
#else
    console_printf("GGUF model embedding not configured\n");
#endif

    /* Interrupt handling - DISABLED for UEFI compatibility */
    console_printf("Interrupts disabled for UEFI boot compatibility\n");
    /* arch_interrupt_init(); */  /* Causes crash in UEFI mode */

    /* Load AI model if embedded (EMBODIOS format) */
    size_t model_size = (uintptr_t)_model_weights_end - (uintptr_t)_model_weights_start;
    if (model_size > 0) {
        console_printf("Loading AI model (%zu bytes)...\n", model_size);
        ai_model = model_load(_model_weights_start, model_size);
        if (ai_model) {
            console_printf("Model loaded: %s\n", ai_model->name);
            console_printf("Parameters: %zu\n", ai_model->param_count);
        } else {
            console_printf("Failed to load AI model\n");
        }
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