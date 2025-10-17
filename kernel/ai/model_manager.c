/* EMBODIOS Model Manager
 * Manages multiple AI models in kernel memory
 */

#include <embodios/types.h>
#include <embodios/kernel.h>
#include <embodios/console.h>
#include <embodios/mm.h>

#define MAX_MODELS 8
#define MAX_MODEL_NAME 64
#define MODEL_HEAP_SIZE (256 * 1024 * 1024)  /* 256MB for models */

/* Model formats */
enum model_format {
    MODEL_FORMAT_UNKNOWN = 0,
    MODEL_FORMAT_EMBEDDED,   /* Built-in kernel weights */
    MODEL_FORMAT_GGUF,      /* GGUF from initrd */
    MODEL_FORMAT_EMB,       /* EMBODIOS optimized */
    MODEL_FORMAT_RAW        /* Raw weights */
};

/* Model capabilities */
enum model_capability {
    MODEL_CAP_TEXT_GEN = (1 << 0),
    MODEL_CAP_CODE_GEN = (1 << 1),
    MODEL_CAP_CHAT     = (1 << 2),
    MODEL_CAP_INSTRUCT = (1 << 3)
};

/* Model metadata */
struct model_metadata {
    char name[MAX_MODEL_NAME];
    char description[128];
    enum model_format format;
    uint32_t capabilities;
    
    /* Model parameters */
    uint32_t vocab_size;
    uint32_t hidden_dim;
    uint32_t n_layers;
    uint32_t n_heads;
    uint32_t context_length;
    
    /* Memory info */
    size_t weight_size;
    size_t runtime_memory;
};

/* AI Model structure */
struct ai_model {
    struct model_metadata meta;
    
    /* Model data */
    void* weights;          /* Pointer to weight data */
    void* runtime_buffer;   /* Runtime workspace */
    
    /* Function pointers for model operations */
    int (*load)(struct ai_model* model, const void* data, size_t size);
    int (*inference)(struct ai_model* model, const char* prompt, 
                    char* response, size_t max_len);
    void (*unload)(struct ai_model* model);
    
    /* Usage stats */
    uint64_t inference_count;
    uint64_t total_tokens;
    
    /* List node */
    struct ai_model* next;
};

/* Model manager state */
static struct {
    struct ai_model* models[MAX_MODELS];
    struct ai_model* default_model;
    int model_count;
    
    /* Memory management */
    void* model_heap;
    size_t heap_used;
    size_t heap_size;
    
    /* Embedded model (always available) */
    struct ai_model embedded_model;
} manager = {
    .model_count = 0,
    .default_model = NULL,
    .heap_used = 0,
    .heap_size = MODEL_HEAP_SIZE
};

/* Forward declarations */
static int embedded_model_inference(struct ai_model* model, const char* prompt,
                                  char* response, size_t max_len);
extern int kernel_inference(const char* prompt, char* output, int max_output);

/* Initialize model manager */
int model_manager_init(void) {
    console_printf("Model Manager: Initializing...\n");
    
    /* Allocate model heap */
    manager.model_heap = kmalloc(MODEL_HEAP_SIZE);
    if (!manager.model_heap) {
        console_printf("Model Manager: Failed to allocate heap\n");
        return -1;
    }
    
    /* Initialize embedded model */
    struct ai_model* embedded = &manager.embedded_model;
    memset(embedded, 0, sizeof(struct ai_model));
    
    strcpy(embedded->meta.name, "embedded-tinyllama");
    strcpy(embedded->meta.description, "Built-in kernel AI model");
    embedded->meta.format = MODEL_FORMAT_EMBEDDED;
    embedded->meta.capabilities = MODEL_CAP_TEXT_GEN | MODEL_CAP_CHAT;
    embedded->meta.vocab_size = 100;
    embedded->meta.hidden_dim = 256;
    embedded->meta.n_layers = 2;
    embedded->meta.n_heads = 4;
    embedded->meta.context_length = 128;
    embedded->meta.weight_size = 100 * 1024;  /* 100KB */
    embedded->meta.runtime_memory = 512 * 1024;  /* 512KB */
    
    embedded->inference = embedded_model_inference;
    
    /* Register embedded model */
    model_register(embedded, "embedded");
    model_set_default("embedded");
    
    console_printf("Model Manager: Initialized with embedded model\n");
    console_printf("Model Manager: Heap size: %d MB\n", MODEL_HEAP_SIZE / 1024 / 1024);
    
    return 0;
}

/* Allocate memory from model heap */
void* model_heap_alloc(size_t size) {
    /* Simple bump allocator */
    if (manager.heap_used + size > manager.heap_size) {
        console_printf("Model Manager: Heap exhausted (%zu/%zu bytes)\n",
                      manager.heap_used, manager.heap_size);
        return NULL;
    }
    
    void* ptr = (char*)manager.model_heap + manager.heap_used;
    manager.heap_used += size;
    
    /* Align to 16 bytes */
    manager.heap_used = (manager.heap_used + 15) & ~15;
    
    return ptr;
}

/* Register a model */
int model_register(struct ai_model* model, const char* name) {
    if (!model || !name) return -1;
    
    if (manager.model_count >= MAX_MODELS) {
        console_printf("Model Manager: Maximum models reached\n");
        return -1;
    }
    
    /* Set name if not already set */
    if (model->meta.name[0] == '\0') {
        strncpy(model->meta.name, name, MAX_MODEL_NAME - 1);
    }
    
    /* Add to registry */
    manager.models[manager.model_count++] = model;
    
    console_printf("Model Manager: Registered model '%s' (%s)\n",
                  model->meta.name, model->meta.description);
    
    return 0;
}

/* Get model by name */
struct ai_model* model_get(const char* name) {
    if (!name) return NULL;
    
    for (int i = 0; i < manager.model_count; i++) {
        if (strcmp(manager.models[i]->meta.name, name) == 0) {
            return manager.models[i];
        }
    }
    
    return NULL;
}

/* Get default model */
struct ai_model* model_get_default(void) {
    return manager.default_model;
}

/* Set default model */
int model_set_default(const char* name) {
    struct ai_model* model = model_get(name);
    if (!model) {
        console_printf("Model Manager: Model '%s' not found\n", name);
        return -1;
    }
    
    manager.default_model = model;
    console_printf("Model Manager: Default model set to '%s'\n", name);
    
    return 0;
}

/* List all models */
void model_list(void) {
    console_printf("=== Loaded Models ===\n");
    console_printf("Count: %d/%d\n", manager.model_count, MAX_MODELS);
    console_printf("Heap: %zu/%zu KB used\n\n", 
                  manager.heap_used / 1024, manager.heap_size / 1024);
    
    for (int i = 0; i < manager.model_count; i++) {
        struct ai_model* model = manager.models[i];
        console_printf("[%d] %s%s\n", i, model->meta.name,
                      (model == manager.default_model) ? " (default)" : "");
        console_printf("    Format: %s, Size: %zu KB\n",
                      model->meta.format == MODEL_FORMAT_EMBEDDED ? "embedded" :
                      model->meta.format == MODEL_FORMAT_GGUF ? "GGUF" :
                      model->meta.format == MODEL_FORMAT_EMB ? "EMB" : "unknown",
                      model->meta.weight_size / 1024);
        console_printf("    Params: %u vocab, %u dim, %u layers\n",
                      model->meta.vocab_size,
                      model->meta.hidden_dim,
                      model->meta.n_layers);
        console_printf("    Stats: %lu inferences, %lu tokens\n",
                      model->inference_count,
                      model->total_tokens);
    }
}

/* Run inference with specific model */
int inference_run_with_model(const char* model_name,
                            const char* prompt,
                            char* response,
                            size_t max_len) {
    struct ai_model* model;
    
    /* Get model */
    if (model_name) {
        model = model_get(model_name);
    } else {
        model = model_get_default();
    }
    
    if (!model) {
        console_printf("Model Manager: No model available\n");
        return -1;
    }
    
    /* Run inference */
    if (model->inference) {
        int result = model->inference(model, prompt, response, max_len);
        
        /* Update stats */
        if (result == 0) {
            model->inference_count++;
            /* Simple token estimate */
            model->total_tokens += strlen(response) / 5;
        }
        
        return result;
    }
    
    return -1;
}

/* Embedded model inference */
static int embedded_model_inference(struct ai_model* model, const char* prompt,
                                  char* response, size_t max_len) {
    /* Use our kernel implementation */
    return kernel_inference(prompt, response, max_len);
}

/* Get model info for status */
void model_get_status(char* buffer, size_t size) {
    struct ai_model* model = model_get_default();
    
    if (model) {
        snprintf(buffer, size,
                "Model: %s (%zu KB), Inferences: %lu",
                model->meta.name,
                model->meta.weight_size / 1024,
                model->inference_count);
    } else {
        snprintf(buffer, size, "No model loaded");
    }
}