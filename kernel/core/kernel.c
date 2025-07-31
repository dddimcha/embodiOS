/* EMBODIOS Native Kernel Entry Point */
#include <embodios/kernel.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/cpu.h>
#include <embodios/model.h>

/* Kernel version info */
const char* kernel_version = "EMBODIOS v0.1.0-native";
const char* kernel_build = __DATE__ " " __TIME__;

/* External symbols from linker script */
extern char _kernel_start[];
extern char _kernel_end[];
extern char _bss_start[];
extern char _bss_end[];
extern char _model_weights_start[];
extern char _model_weights_end[];

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
    
    /* Clear BSS (should be done in boot code, but ensure it's clean) */
    size_t bss_size = (uintptr_t)_bss_end - (uintptr_t)_bss_start;
    memset(_bss_start, 0, bss_size);
    
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
    
    /* Interrupt handling */
    console_printf("Initializing interrupts...\n");
    arch_interrupt_init();
    
    /* Load AI model if embedded */
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
    } else {
        console_printf("No embedded AI model found\n");
    }
    
    /* Initialize command processor */
    if (ai_model) {
        console_printf("Initializing AI command processor...\n");
        command_processor_init(ai_model);
    }
    
    /* Enable interrupts */
    arch_enable_interrupts();
    
    console_printf("\nEMBODIOS Ready.\n");
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