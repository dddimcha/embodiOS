/* Minimal AI model runtime for EMBODIOS */

/* Kernel AI runtime - using integer-only inference for ARM64 compatibility */

#include "embodios/types.h"
#include "embodios/kernel.h"
#include "embodios/console.h"
#include "embodios/model.h"
#include "embodios/mm.h"
#include "embodios/tvm.h"

/* Model runtime state */
typedef struct {
    struct embodios_model *model;
    void *workspace;
    size_t workspace_size;
    bool initialized;
} model_runtime_t;

static model_runtime_t runtime = {
    .model = NULL,
    .workspace = NULL,
    .workspace_size = 0,
    .initialized = false
};

/* Initialize model runtime */
void model_runtime_init(void)
{
    runtime.initialized = true;
    console_printf("AI Runtime: Initialized\n");
    
    /* Initialize TVM backend */
    tvm_runtime_init();
    console_printf("AI Runtime: TVM backend ready\n");
}

/* Load model from memory */
struct embodios_model* model_load(const void* data, size_t size)
{
    if (!runtime.initialized) {
        console_printf("AI Runtime: Not initialized\n");
        return NULL;
    }
    
    /* Validate model header */
    if (size < sizeof(struct embodios_model)) {
        console_printf("AI Runtime: Model data too small\n");
        return NULL;
    }
    
    struct embodios_model *model = (struct embodios_model*)data;
    
    /* Basic validation */
    if (model->magic != 0x454D424F) { /* 'EMBO' */
        console_printf("AI Runtime: Invalid model magic\n");
        return NULL;
    }
    
    /* Allocate workspace based on model requirements */
    size_t workspace_size = model->memory_required;
    if (workspace_size == 0) {
        /* Default to 64MB if not specified */
        workspace_size = 64 * 1024 * 1024;
    }
    
    void *workspace = kmalloc(workspace_size);
    if (!workspace) {
        console_printf("AI Runtime: Failed to allocate %zu bytes workspace\n", workspace_size);
        return NULL;
    }
    
    /* Store model info */
    runtime.model = model;
    runtime.workspace = workspace;
    runtime.workspace_size = workspace_size;
    
    console_printf("AI Runtime: Loaded model '%s' v%u.%u\n", 
                   model->name, 
                   model->version_major, 
                   model->version_minor);
    console_printf("  Architecture: %s\n", model->arch);
    console_printf("  Parameters: %zu\n", model->param_count);
    console_printf("  Workspace: %zu MB\n", workspace_size / (1024 * 1024));
    
    return model;
}

/* Global graph executor for model inference */
static tvm_graph_executor_t* model_graph = NULL;

/* Run inference (simplified - integer-only) */
int model_inference(const int *input_tokens, int num_tokens,
                   int *output_tokens, int max_output)
{
    if (!runtime.model) {
        console_printf("AI Runtime: No model loaded\n");
        return -1;
    }
    
    console_printf("AI Runtime: Running inference with %d tokens\n", num_tokens);

    /* Simple integer-based inference (no floating-point, no graph executor) */
    /* Generate demo output tokens based on input */
    int output_len = 0;

    /* Echo input with simple transformations */
    for (int t = 0; t < num_tokens && output_len < max_output; t++) {
        /* Add input token */
        output_tokens[output_len++] = input_tokens[t];

        /* Add transformed token */
        if (output_len < max_output) {
            output_tokens[output_len++] = (input_tokens[t] + 1) % 256;
        }
    }

    /* Add some demo response tokens */
    const int demo_response[] = {72, 101, 108, 108, 111, 33};  /* "Hello!" */
    for (int i = 0; i < 6 && output_len < max_output; i++) {
        output_tokens[output_len++] = demo_response[i];
    }
    
    console_printf("AI Runtime: Inference complete, generated %d tokens\n", output_len);
    return output_len;
}

/* Get model info */
struct embodios_model* get_current_model(void)
{
    return runtime.model;
}

/* Unload model */
void model_unload(struct embodios_model* model)
{
    /* For now, we only support one loaded model */
    if (model != runtime.model) {
        console_printf("AI Runtime: Model not loaded\n");
        return;
    }
    
    /* Note: graph executor not used in integer-only mode */
    model_graph = NULL;

    if (runtime.workspace) {
        kfree(runtime.workspace);
        runtime.workspace = NULL;
    }
    runtime.model = NULL;
    runtime.workspace_size = 0;
    console_printf("AI Runtime: Model unloaded\n");
}

/* Inference API implementation */

/* External functions */
int transformer_init(struct embodios_model* model);
void transformer_reset_cache(void);

int tokenizer_init(void);
int tokenizer_encode(const char* text, int* tokens, int max_tokens);
int tokenizer_decode(int* tokens, int n_tokens, char* output, size_t max_output);
const char* tokenizer_decode_token(int token);

/* String functions */
size_t strlen(const char* s);

static int g_inference_initialized = 0;

/* Initialize inference engine (model-based wrapper) */
int model_inference_init(struct embodios_model* model)
{
    console_printf("Initializing inference engine...\n");
    
    /* Initialize tokenizer */
    if (tokenizer_init() < 0) {
        console_printf("Failed to initialize tokenizer\n");
        return -1;
    }
    
    /* Initialize transformer */
    if (transformer_init(model) < 0) {
        console_printf("Failed to initialize transformer\n");
        return -1;
    }
    
    runtime.model = model;
    g_inference_initialized = 1;
    
    console_printf("Inference engine initialized successfully\n");
    return 0;
}

/* Run inference on input text */
int inference_run(const char* input, char* output, size_t output_size)
{
    /* Use real TinyLlama GGUF integer inference */
    extern int tinyllama_integer_inference(const char* prompt, char* response, size_t max_response);

    console_printf("Running GGUF integer inference: \"%s\"\n", input);

    /* Call the real inference engine */
    int result = tinyllama_integer_inference(input, output, output_size);

    if (result < 0) {
        console_printf("GGUF inference failed\n");
        /* Fallback to simple response */
        const char* fallback = "TinyLlama inference failed. Check model loading.";
        size_t i = 0;
        while (fallback[i] && i < output_size - 1) {
            output[i] = fallback[i];
            i++;
        }
        output[i] = '\0';
        return -1;
    }

    console_printf("GGUF inference complete: %d chars\n", result);
    return 0;
}

/* Test inference */
void inference_test(void)
{
    console_printf("Running inference test...\n");
    
    const char* test_prompts[] = {
        "Hello",
        "What is 2+2?",
        "Tell me a joke",
        NULL
    };
    
    char output[512];
    
    for (int i = 0; test_prompts[i]; i++) {
        console_printf("\nTest %d: \"%s\"\n", i + 1, test_prompts[i]);
        
        if (inference_run(test_prompts[i], output, sizeof(output)) == 0) {
            console_printf("Response: %s\n", output);
        } else {
            console_printf("Test failed\n");
        }
    }
}

/* Show inference statistics */
void inference_stats(void)
{
    console_printf("Inference Statistics:\n");
    console_printf("  Initialized: %s\n", g_inference_initialized ? "Yes" : "No");
    
    if (runtime.model) {
        console_printf("  Model: %s\n", runtime.model->name);
        console_printf("  Architecture: %s\n", runtime.model->arch);
    } else {
        console_printf("  Model: None\n");
    }
    
    /* TODO: Add more stats like tokens processed, inference time, etc. */
}