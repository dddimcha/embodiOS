/* EMBODIOS Boot-time Model Loading
 * Integrates model loading into kernel boot process
 */

#include <embodios/types.h>
#include <embodios/kernel.h>
#include <embodios/console.h>
#include <embodios/cmdline.h>

/* External functions */
extern int model_manager_init(void);
extern int initrd_scan_models(void);
extern int model_set_default(const char* name);
extern void model_list(void);
extern struct ai_model* model_get_default(void);

/* Kernel command line parameters */
static char requested_model[64] = {0};
static bool verbose_boot = false;

/* Parse model-related kernel parameters */
static void parse_model_cmdline(const char* cmdline) {
    if (!cmdline) return;
    
    /* Look for embodios.model=xxx */
    const char* model_param = strstr(cmdline, "embodios.model=");
    if (model_param) {
        model_param += strlen("embodios.model=");
        
        /* Extract model name */
        int i = 0;
        while (model_param[i] && model_param[i] != ' ' && i < 63) {
            requested_model[i] = model_param[i];
            i++;
        }
        requested_model[i] = '\0';
        
        console_printf("Boot: Requested model: %s\n", requested_model);
    }
    
    /* Check for verbose flag */
    if (strstr(cmdline, "embodios.verbose")) {
        verbose_boot = true;
    }
}

/* Initialize AI models during boot */
int boot_init_models(const char* cmdline) {
    console_printf("\n=== EMBODIOS AI Model Initialization ===\n");
    
    /* Parse command line */
    parse_model_cmdline(cmdline);
    
    /* Initialize model manager */
    if (model_manager_init() < 0) {
        console_printf("Boot: Failed to initialize model manager\n");
        return -1;
    }
    
    /* Scan initrd for models */
    int models_found = initrd_scan_models();
    
    /* Select default model */
    if (requested_model[0]) {
        /* User requested specific model */
        if (model_set_default(requested_model) < 0) {
            console_printf("Boot: Requested model '%s' not found\n", 
                          requested_model);
            console_printf("Boot: Falling back to embedded model\n");
        }
    } else if (models_found > 0) {
        /* Auto-select from loaded models */
        /* For now, embedded model remains default unless specified */
        console_printf("Boot: Using embedded model (specify embodios.model=xxx to change)\n");
    }
    
    /* Show loaded models if verbose */
    if (verbose_boot) {
        model_list();
    } else {
        /* Show summary */
        struct ai_model* default_model = model_get_default();
        if (default_model) {
            console_printf("Boot: Default model: %s\n", 
                          default_model->meta.name);
        }
    }
    
    console_printf("=== AI Model Initialization Complete ===\n\n");
    
    return 0;
}

/* Get boot model status */
void boot_get_model_status(char* buffer, size_t size) {
    int written = 0;
    
    if (requested_model[0]) {
        written = snprintf(buffer, size, "Requested: %s, ", requested_model);
    }
    
    struct ai_model* model = model_get_default();
    if (model) {
        snprintf(buffer + written, size - written,
                "Active: %s", model->meta.name);
    } else {
        snprintf(buffer + written, size - written, "No model active");
    }
}