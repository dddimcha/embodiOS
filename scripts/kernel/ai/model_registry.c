/**
 * EMBODIOS Model Registry
 *
 * Multi-model runtime support for loading, switching, and unloading
 * AI models at runtime without kernel reboot.
 *
 * Features:
 * - Up to 3 models loaded simultaneously
 * - Active model switching for inference
 * - Memory tracking and cleanup
 * - Statistics and debugging
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/model.h>
#include <embodios/model_registry.h>

/* External string functions */
extern int strcmp(const char* s1, const char* s2);
extern size_t strlen(const char* s);
extern void* memset(void* s, int c, size_t n);

/* External model loader */
extern struct embodios_model* load_model_from_memory(void* data, size_t size);

/* ============================================================================
 * Registry State
 * ============================================================================ */

static struct {
    bool initialized;
    model_slot_t slots[MODEL_REGISTRY_MAX_MODELS];
    int active_id;
    model_registry_stats_t stats;
} g_registry = {
    .initialized = false,
    .active_id = MODEL_ID_INVALID
};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * get_cycles - Get current CPU cycle count for timestamps
 *
 * Returns: Current cycle count
 */
static uint64_t get_cycles(void)
{
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    static uint64_t counter = 0;
    return counter++;
#endif
}

/**
 * safe_strncpy - Safe string copy with null termination
 * @dest: Destination buffer
 * @src: Source string
 * @size: Size of destination buffer
 */
static void safe_strncpy(char* dest, const char* src, size_t size)
{
    if (size == 0) return;

    size_t i;
    for (i = 0; i < size - 1 && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

/**
 * find_free_slot - Find first available slot
 *
 * Returns: Slot index (0-2), or -1 if none available
 */
static int find_free_slot(void)
{
    for (int i = 0; i < MODEL_REGISTRY_MAX_MODELS; i++) {
        if (g_registry.slots[i].state == MODEL_SLOT_FREE) {
            return i;
        }
    }
    return -1;
}

/**
 * is_valid_id - Check if model ID is valid
 * @model_id: Model ID to check
 *
 * Returns: true if valid (0-2), false otherwise
 */
static bool is_valid_id(int model_id)
{
    return model_id >= 0 && model_id < MODEL_REGISTRY_MAX_MODELS;
}

/* ============================================================================
 * Initialization and Shutdown
 * ============================================================================ */

/**
 * model_registry_init - Initialize the model registry
 *
 * Initializes all slots and statistics. Must be called before
 * any other registry functions.
 *
 * Returns: 0 on success
 */
int model_registry_init(void)
{
    if (g_registry.initialized) {
        return 0;  /* Already initialized */
    }

    console_printf("[ModelRegistry] Initializing...\n");

    /* Initialize all slots */
    for (int i = 0; i < MODEL_REGISTRY_MAX_MODELS; i++) {
        g_registry.slots[i].id = i;
        g_registry.slots[i].state = MODEL_SLOT_FREE;
        g_registry.slots[i].model = NULL;
        g_registry.slots[i].workspace = NULL;
        g_registry.slots[i].workspace_size = 0;
        g_registry.slots[i].load_time = 0;
        g_registry.slots[i].last_used = 0;
        g_registry.slots[i].inference_count = 0;
        g_registry.slots[i].source_path[0] = '\0';
    }

    /* Initialize statistics */
    g_registry.stats.total_loads = 0;
    g_registry.stats.total_unloads = 0;
    g_registry.stats.total_switches = 0;
    g_registry.stats.current_count = 0;
    g_registry.stats.total_memory_used = 0;

    g_registry.active_id = MODEL_ID_INVALID;
    g_registry.initialized = true;

    console_printf("[ModelRegistry] Initialized with %d slots\n",
                   MODEL_REGISTRY_MAX_MODELS);

    return 0;
}

/**
 * model_registry_shutdown - Shutdown registry and unload all models
 *
 * Unloads all loaded models and frees resources.
 */
void model_registry_shutdown(void)
{
    if (!g_registry.initialized) {
        return;
    }

    console_printf("[ModelRegistry] Shutting down...\n");

    /* Unload all models */
    for (int i = 0; i < MODEL_REGISTRY_MAX_MODELS; i++) {
        if (g_registry.slots[i].state != MODEL_SLOT_FREE) {
            model_registry_unload(i);
        }
    }

    g_registry.initialized = false;
    g_registry.active_id = MODEL_ID_INVALID;

    console_printf("[ModelRegistry] Shutdown complete\n");
}

/**
 * model_registry_is_initialized - Check initialization status
 *
 * Returns: true if initialized
 */
bool model_registry_is_initialized(void)
{
    return g_registry.initialized;
}

/* ============================================================================
 * Model Loading
 * ============================================================================ */

/**
 * model_registry_load - Load a model into the registry
 * @data: Pointer to model data (GGUF format)
 * @size: Size of model data
 * @name: Optional name override (NULL to auto-detect)
 *
 * Parses the model, allocates workspace, and registers it in
 * an available slot. Does not automatically activate the model.
 *
 * Returns: Model ID (0-2) on success, negative error on failure
 */
int model_registry_load(const void* data, size_t size, const char* name)
{
    if (!g_registry.initialized) {
        console_printf("[ModelRegistry] ERROR: Not initialized\n");
        return MODEL_REG_ERR_NOT_INIT;
    }

    /* Find free slot */
    int slot_id = find_free_slot();
    if (slot_id < 0) {
        console_printf("[ModelRegistry] ERROR: No free slots (max %d models)\n",
                       MODEL_REGISTRY_MAX_MODELS);
        return MODEL_REG_ERR_NO_SLOT;
    }

    model_slot_t* slot = &g_registry.slots[slot_id];
    slot->state = MODEL_SLOT_LOADING;

    console_printf("[ModelRegistry] Loading model into slot %d...\n", slot_id);

    /* Load and parse model */
    struct embodios_model* model = load_model_from_memory((void*)data, size);
    if (!model) {
        console_printf("[ModelRegistry] ERROR: Failed to parse model\n");
        slot->state = MODEL_SLOT_FREE;
        return MODEL_REG_ERR_INVALID_DATA;
    }

    /* Allocate workspace */
    size_t workspace_size = model->memory_required;
    if (workspace_size == 0) {
        workspace_size = 32 * 1024 * 1024;  /* Default 32MB */
    }

    void* workspace = kmalloc(workspace_size);
    if (!workspace) {
        console_printf("[ModelRegistry] ERROR: Failed to allocate %zu MB workspace\n",
                       workspace_size / (1024 * 1024));
        kfree(model);
        slot->state = MODEL_SLOT_FREE;
        return MODEL_REG_ERR_NO_MEMORY;
    }

    /* Initialize slot */
    slot->model = model;
    slot->workspace = workspace;
    slot->workspace_size = workspace_size;
    slot->load_time = get_cycles();
    slot->last_used = slot->load_time;
    slot->inference_count = 0;

    /* Set source name */
    if (name && name[0] != '\0') {
        safe_strncpy(slot->source_path, name, sizeof(slot->source_path));
    } else {
        safe_strncpy(slot->source_path, model->name, sizeof(slot->source_path));
    }

    slot->state = MODEL_SLOT_READY;

    /* Update statistics */
    g_registry.stats.total_loads++;
    g_registry.stats.current_count++;
    g_registry.stats.total_memory_used += model->size + workspace_size;

    console_printf("[ModelRegistry] Loaded '%s' in slot %d\n",
                   slot->source_path, slot_id);
    console_printf("  Size: %zu KB, Workspace: %zu MB\n",
                   model->size / 1024, workspace_size / (1024 * 1024));

    /* Auto-activate if this is the only model */
    if (g_registry.stats.current_count == 1) {
        model_registry_switch(slot_id);
    }

    return slot_id;
}

/* External symbols for embedded models - defined in embedded_model_stubs.c */
extern const uint8_t _binary_tinystories_15m_bin_start[];
extern const uint8_t _binary_tinystories_15m_bin_end[];

/**
 * model_registry_load_embedded - Load an embedded model by name
 * @name: Model name ("tinystories", etc.)
 *
 * Loads a model embedded in the kernel binary.
 *
 * Returns: Model ID on success, negative error on failure
 */
int model_registry_load_embedded(const char* name)
{
    if (strcmp(name, "tinystories") == 0 || strcmp(name, "tinystories-15m") == 0) {
        /* Check if model is embedded by comparing start/end addresses.
         * Weak stubs provide zero-size arrays, so end > start only when
         * real model data is linked in via objcopy. */
        const uint8_t* start = &_binary_tinystories_15m_bin_start[0];
        const uint8_t* end = &_binary_tinystories_15m_bin_end[0];
        size_t size = (size_t)(end - start);

        if (size > 0 && size < 1024 * 1024 * 1024) {  /* Sanity check: < 1GB */
            console_printf("[ModelRegistry] Loading embedded TinyStories (%zu MB)\n",
                           size / (1024 * 1024));
            return model_registry_load(start, size, "TinyStories-15M");
        }

        console_printf("[ModelRegistry] TinyStories model not embedded in kernel\n");
        console_printf("  To embed: place tinystories-15m.bin in models/ and rebuild\n");
        return MODEL_REG_ERR_NOT_FOUND;
    }

    console_printf("[ModelRegistry] Unknown embedded model: %s\n", name);
    console_printf("  Available: tinystories\n");
    return MODEL_REG_ERR_NOT_FOUND;
}

/* ============================================================================
 * Model Unloading
 * ============================================================================ */

/**
 * model_registry_unload - Unload a model from the registry
 * @model_id: ID of model to unload
 *
 * Frees model and workspace memory. Cannot unload the active model
 * if other models are loaded (must switch first).
 *
 * Returns: 0 on success, negative error on failure
 */
int model_registry_unload(int model_id)
{
    if (!g_registry.initialized) {
        return MODEL_REG_ERR_NOT_INIT;
    }

    if (!is_valid_id(model_id)) {
        console_printf("[ModelRegistry] ERROR: Invalid model ID %d\n", model_id);
        return MODEL_REG_ERR_INVALID_ID;
    }

    model_slot_t* slot = &g_registry.slots[model_id];

    if (slot->state == MODEL_SLOT_FREE) {
        console_printf("[ModelRegistry] ERROR: Slot %d not loaded\n", model_id);
        return MODEL_REG_ERR_NOT_LOADED;
    }

    /* Check if this is the active model */
    if (g_registry.active_id == model_id) {
        /* Find another model to switch to */
        int alt_id = MODEL_ID_INVALID;
        for (int i = 0; i < MODEL_REGISTRY_MAX_MODELS; i++) {
            if (i != model_id && g_registry.slots[i].state == MODEL_SLOT_READY) {
                alt_id = i;
                break;
            }
        }

        if (alt_id != MODEL_ID_INVALID) {
            /* Switch to alternate model first */
            model_registry_switch(alt_id);
        } else {
            /* This is the only model, deactivate */
            g_registry.active_id = MODEL_ID_INVALID;
        }
    }

    console_printf("[ModelRegistry] Unloading model from slot %d...\n", model_id);

    slot->state = MODEL_SLOT_UNLOADING;

    /* Update memory stats before freeing */
    size_t freed_memory = slot->workspace_size;
    if (slot->model) {
        freed_memory += slot->model->size;
    }

    /* Free workspace */
    if (slot->workspace) {
        kfree(slot->workspace);
        slot->workspace = NULL;
    }

    /* Free model structure */
    if (slot->model) {
        kfree(slot->model);
        slot->model = NULL;
    }

    /* Reset slot */
    slot->state = MODEL_SLOT_FREE;
    slot->workspace_size = 0;
    slot->load_time = 0;
    slot->last_used = 0;
    slot->inference_count = 0;
    slot->source_path[0] = '\0';

    /* Update statistics */
    g_registry.stats.total_unloads++;
    g_registry.stats.current_count--;
    g_registry.stats.total_memory_used -= freed_memory;

    console_printf("[ModelRegistry] Slot %d unloaded, freed %zu KB\n",
                   model_id, freed_memory / 1024);

    return MODEL_REG_OK;
}

/* ============================================================================
 * Model Switching
 * ============================================================================ */

/**
 * model_registry_switch - Switch active model
 * @model_id: ID of model to activate
 *
 * Makes the specified model active for inference. The previous
 * active model remains loaded but becomes inactive.
 *
 * Returns: 0 on success, negative error on failure
 */
int model_registry_switch(int model_id)
{
    if (!g_registry.initialized) {
        return MODEL_REG_ERR_NOT_INIT;
    }

    if (!is_valid_id(model_id)) {
        console_printf("[ModelRegistry] ERROR: Invalid model ID %d\n", model_id);
        return MODEL_REG_ERR_INVALID_ID;
    }

    model_slot_t* slot = &g_registry.slots[model_id];

    if (slot->state != MODEL_SLOT_READY && slot->state != MODEL_SLOT_ACTIVE) {
        console_printf("[ModelRegistry] ERROR: Model %d not loaded\n", model_id);
        return MODEL_REG_ERR_NOT_LOADED;
    }

    /* Already active? */
    if (g_registry.active_id == model_id) {
        return MODEL_REG_OK;
    }

    /* Deactivate previous model */
    if (g_registry.active_id != MODEL_ID_INVALID) {
        model_slot_t* prev = &g_registry.slots[g_registry.active_id];
        prev->state = MODEL_SLOT_READY;
    }

    /* Activate new model */
    slot->state = MODEL_SLOT_ACTIVE;
    slot->last_used = get_cycles();
    g_registry.active_id = model_id;

    /* Update statistics */
    g_registry.stats.total_switches++;

    console_printf("[ModelRegistry] Switched to model %d: '%s'\n",
                   model_id, slot->source_path);

    return MODEL_REG_OK;
}

/**
 * model_registry_get_active - Get the active model
 *
 * Returns: Pointer to active model, NULL if none
 */
struct embodios_model* model_registry_get_active(void)
{
    if (!g_registry.initialized || g_registry.active_id == MODEL_ID_INVALID) {
        return NULL;
    }
    return g_registry.slots[g_registry.active_id].model;
}

/**
 * model_registry_get_active_id - Get active model ID
 *
 * Returns: Active model ID, MODEL_ID_INVALID if none
 */
int model_registry_get_active_id(void)
{
    if (!g_registry.initialized) {
        return MODEL_ID_INVALID;
    }
    return g_registry.active_id;
}

/* ============================================================================
 * Model Queries
 * ============================================================================ */

/**
 * model_registry_get - Get model by ID
 * @model_id: Model ID (0-2)
 *
 * Returns: Model pointer, NULL if not loaded
 */
struct embodios_model* model_registry_get(int model_id)
{
    if (!g_registry.initialized || !is_valid_id(model_id)) {
        return NULL;
    }
    return g_registry.slots[model_id].model;
}

/**
 * model_registry_get_slot - Get slot by ID
 * @model_id: Model ID (0-2)
 *
 * Returns: Slot pointer, NULL if invalid
 */
const model_slot_t* model_registry_get_slot(int model_id)
{
    if (!is_valid_id(model_id)) {
        return NULL;
    }
    return &g_registry.slots[model_id];
}

/**
 * model_registry_find_by_name - Find model by name
 * @name: Model name to search
 *
 * Returns: Model ID if found, MODEL_ID_INVALID otherwise
 */
int model_registry_find_by_name(const char* name)
{
    if (!g_registry.initialized || !name) {
        return MODEL_ID_INVALID;
    }

    for (int i = 0; i < MODEL_REGISTRY_MAX_MODELS; i++) {
        if (g_registry.slots[i].state != MODEL_SLOT_FREE) {
            if (strcmp(g_registry.slots[i].source_path, name) == 0) {
                return i;
            }
            if (g_registry.slots[i].model &&
                strcmp(g_registry.slots[i].model->name, name) == 0) {
                return i;
            }
        }
    }

    return MODEL_ID_INVALID;
}

/**
 * model_registry_count - Get loaded model count
 *
 * Returns: Number of loaded models (0-3)
 */
int model_registry_count(void)
{
    if (!g_registry.initialized) {
        return 0;
    }
    return g_registry.stats.current_count;
}

/**
 * model_registry_has_free_slot - Check for available slot
 *
 * Returns: true if slot available
 */
bool model_registry_has_free_slot(void)
{
    return find_free_slot() >= 0;
}

/* ============================================================================
 * Statistics and Debugging
 * ============================================================================ */

/**
 * model_registry_get_stats - Get registry statistics
 * @stats: Output structure
 */
void model_registry_get_stats(model_registry_stats_t* stats)
{
    if (!stats) return;

    if (!g_registry.initialized) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    *stats = g_registry.stats;
}

/**
 * model_registry_print_status - Print registry status
 *
 * Shows all slots, loaded models, and their states.
 */
void model_registry_print_status(void)
{
    console_printf("\n========================================\n");
    console_printf("Model Registry Status\n");
    console_printf("========================================\n\n");

    if (!g_registry.initialized) {
        console_printf("Registry not initialized\n");
        return;
    }

    console_printf("Loaded: %d/%d models\n",
                   g_registry.stats.current_count, MODEL_REGISTRY_MAX_MODELS);
    console_printf("Active: %d\n\n", g_registry.active_id);

    console_printf("Slot  State     Name                    Memory\n");
    console_printf("----  --------  ----------------------  --------\n");

    for (int i = 0; i < MODEL_REGISTRY_MAX_MODELS; i++) {
        model_slot_t* slot = &g_registry.slots[i];

        const char* state_str = "FREE";
        switch (slot->state) {
            case MODEL_SLOT_LOADING:  state_str = "LOADING"; break;
            case MODEL_SLOT_READY:    state_str = "READY"; break;
            case MODEL_SLOT_ACTIVE:   state_str = "ACTIVE*"; break;
            case MODEL_SLOT_UNLOADING: state_str = "UNLOAD"; break;
            default: break;
        }

        if (slot->state == MODEL_SLOT_FREE) {
            console_printf("[%d]   %-8s  -\n", i, state_str);
        } else {
            console_printf("[%d]   %-8s  %-22s  %zu KB\n",
                           i, state_str,
                           slot->source_path[0] ? slot->source_path : "(unnamed)",
                           slot->workspace_size / 1024);
        }
    }

    console_printf("\n");
}

/**
 * model_registry_print_stats - Print registry statistics
 */
void model_registry_print_stats(void)
{
    console_printf("\n[ModelRegistry] Statistics:\n");
    console_printf("  Total loads: %d\n", g_registry.stats.total_loads);
    console_printf("  Total unloads: %d\n", g_registry.stats.total_unloads);
    console_printf("  Total switches: %d\n", g_registry.stats.total_switches);
    console_printf("  Current count: %d\n", g_registry.stats.current_count);
    console_printf("  Memory used: %zu KB\n",
                   g_registry.stats.total_memory_used / 1024);
}

/**
 * model_registry_record_inference - Record inference on active model
 */
void model_registry_record_inference(void)
{
    if (!g_registry.initialized || g_registry.active_id == MODEL_ID_INVALID) {
        return;
    }

    model_slot_t* slot = &g_registry.slots[g_registry.active_id];
    slot->inference_count++;
    slot->last_used = get_cycles();
}

/* ============================================================================
 * Error Handling
 * ============================================================================ */

/**
 * model_registry_strerror - Get error message
 * @err: Error code
 *
 * Returns: Human-readable error string
 */
const char* model_registry_strerror(int err)
{
    switch (err) {
        case MODEL_REG_OK:              return "Success";
        case MODEL_REG_ERR_NOT_INIT:    return "Registry not initialized";
        case MODEL_REG_ERR_NO_SLOT:     return "No free model slots";
        case MODEL_REG_ERR_INVALID_DATA: return "Invalid model data";
        case MODEL_REG_ERR_NO_MEMORY:   return "Out of memory";
        case MODEL_REG_ERR_INVALID_ID:  return "Invalid model ID";
        case MODEL_REG_ERR_NOT_LOADED:  return "Model not loaded";
        case MODEL_REG_ERR_IS_ACTIVE:   return "Model is active";
        case MODEL_REG_ERR_NOT_FOUND:   return "Model not found";
        default:                        return "Unknown error";
    }
}
