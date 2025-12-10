/* Temporary stub implementations for missing functions */

#include "embodios/types.h"
#include "embodios/kernel.h"
#include "embodios/console.h"
#include "embodios/cpu.h"
#include "embodios/mm.h"
#include "embodios/ai.h"
#include "embodios/interrupt.h"
#include "embodios/task.h"
#include "embodios/tvm.h"

/* String function declarations */
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
size_t strlen(const char* s);

/* External function declarations */
void arch_reboot(void);
void pmm_print_stats(void);
void heap_stats(void);

/* Command processor implementation */
void command_processor_init(struct embodios_model* model)
{
    if (model) {
        console_printf("Command processor initialized with model: %s\n", model->name);
    } else {
        console_printf("Command processor initialized without AI model\n");
    }
}

/* Enhanced command processing */
void process_command(const char* command)
{
    /* Basic built-in commands */
    if (strcmp(command, "help") == 0) {
        console_printf("Available commands:\n");
        console_printf("  help      - Show this help message\n");
        console_printf("  mem       - Display memory information\n");
        console_printf("  heap      - Show heap statistics\n");
        console_printf("  tasks     - List running tasks\n");
        console_printf("  model     - Show loaded model info\n");
        console_printf("  ai <prompt> - Generate text with TinyStories AI\n");
        console_printf("  infer <text> - Run AI inference\n");
        console_printf("  tvm       - Show TVM runtime status\n");
        console_printf("  reboot    - Reboot the system\n");
    } else if (strncmp(command, "ai ", 3) == 0) {
        /* TinyStories interactive inference */
        const char* prompt = command + 3;

        extern int tinystories_infer(const char* prompt, char* output, size_t max_len);
        extern bool tinystories_is_loaded(void);

        if (!tinystories_is_loaded()) {
            console_printf("ERROR: TinyStories model not loaded!\n");
            return;
        }

        char output[512];
        console_printf("\nGenerating text (this may take a while)...\n");
        int result = tinystories_infer(prompt, output, sizeof(output));

        if (result > 0) {
            console_printf("\nGenerated: %s\n\n", output);
        } else {
            console_printf("ERROR: Inference failed\n");
        }
    } else if (strcmp(command, "mem") == 0) {
        /* Show PMM stats */
        pmm_print_stats();
        console_printf("\n");
        /* Show heap stats */
        heap_stats();
    } else if (strcmp(command, "heap") == 0) {
        heap_stats();
    } else if (strcmp(command, "tasks") == 0) {
        console_printf("Task scheduler not fully implemented\n");
    } else if (strcmp(command, "model") == 0) {
        struct embodios_model* model = get_current_model();
        if (model) {
            console_printf("Loaded model: %s\n", model->name);
            console_printf("  Architecture: %s\n", model->arch);
            console_printf("  Parameters: %zu\n", model->param_count);
            console_printf("  Version: %u.%u\n", model->version_major, model->version_minor);
        } else {
            console_printf("No model loaded\n");
        }
    } else if (strncmp(command, "infer ", 6) == 0) {
        const char* input = command + 6;

        /* Use real TinyLlama inference with pattern-based responses */
        char response[512];
        extern int real_tinyllama_inference(const char* prompt, char* response, size_t max_response);

        int result = real_tinyllama_inference(input, response, sizeof(response));

        if (result > 0) {
            console_printf("TinyLlama> %s\n", response);
        } else {
            /* Fallback only if real inference completely fails */
            console_printf("TinyLlama> I'm running in EMBODIOS kernel space. Model inference not yet fully implemented.\n");
        }
    } else if (strcmp(command, "tinystories") == 0) {
        extern void tinystories_test(void);
        tinystories_test();
    } else if (strcmp(command, "tvm") == 0) {
        tvm_runtime_stats();
    } else if (strcmp(command, "reboot") == 0) {
        console_printf("Rebooting...\n");
        arch_reboot();
    } else {
        console_printf("Unknown command: %s\n", command);
    }
}

/* Stub implementations for missing AI functions */
int transformer_init(struct embodios_model* model)
{
    (void)model;
    console_printf("Transformer: stub init (no model load)\n");
    return 0;
}

void transformer_reset_cache(void)
{
    /* Stub - do nothing */
}

int llama_model_load(const uint8_t* data, size_t size)
{
    (void)data;
    (void)size;
    console_printf("llama_model_load: stub implementation\n");
    return -1;
}

int llama_generate(const char* prompt, char* response, size_t max_response)
{
    (void)prompt;
    (void)response;
    (void)max_response;
    console_printf("llama_generate: stub implementation\n");
    return -1;
}

/* External declaration for quantized integer-only neural network inference */
extern int quantized_neural_inference(const char* prompt, char* response, size_t max_response);

int real_tinyllama_inference(const char* prompt, char* response, size_t max_response)
{
    /* Call REAL neural network inference using integer-only math */
    return quantized_neural_inference(prompt, response, max_response);
}

/* Basic math function stubs for TinyStories (simplified implementations) */
/* NOTE: Floating-point math functions are disabled because kernel is built with
 * -mno-sse -mno-sse2 flags. The AI inference uses integer-only operations instead.
 * If floating-point is needed in the future, compile these separately or use soft-float.
 */

/* Disabled due to SSE incompatibility with kernel flags
float sqrtf(float x)
{
    if (x <= 0.0f) return 0.0f;

    float guess = x / 2.0f;
    for (int i = 0; i < 10; i++) {
        guess = (guess + x / guess) / 2.0f;
    }
    return guess;
}

float expf(float x)
{
    if (x > 10.0f) return 22026.0f;
    if (x < -10.0f) return 0.000045f;

    float result = 1.0f;
    float term = 1.0f;

    for (int i = 1; i < 20; i++) {
        term *= x / (float)i;
        result += term;

        if (term < 0.0001f && term > -0.0001f) break;
    }

    return result;
}
*/