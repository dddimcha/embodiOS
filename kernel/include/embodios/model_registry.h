/**
 * EMBODIOS Model Registry
 *
 * Multi-model runtime support: load, switch, and unload AI models
 * without rebooting the kernel. Supports up to 3 models simultaneously.
 */

#ifndef EMBODIOS_MODEL_REGISTRY_H
#define EMBODIOS_MODEL_REGISTRY_H

#include <embodios/types.h>
#include <embodios/model.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define MODEL_REGISTRY_MAX_MODELS   3       /* Maximum simultaneous models */
#define MODEL_ID_INVALID            (-1)    /* Invalid model ID */
#define MODEL_NAME_MAX_LEN          64      /* Maximum model name length */

/* ============================================================================
 * Model Slot States
 * ============================================================================ */

typedef enum {
    MODEL_SLOT_FREE = 0,        /* Slot is available */
    MODEL_SLOT_LOADING,         /* Model is being loaded */
    MODEL_SLOT_READY,           /* Model loaded and ready */
    MODEL_SLOT_ACTIVE,          /* Model is currently active */
    MODEL_SLOT_UNLOADING        /* Model is being unloaded */
} model_slot_state_t;

/* ============================================================================
 * Model Slot Structure
 * ============================================================================ */

typedef struct model_slot {
    int id;                             /* Slot ID (0, 1, 2) */
    model_slot_state_t state;           /* Current state */
    struct embodios_model* model;       /* Model structure */
    void* workspace;                    /* Model workspace memory */
    size_t workspace_size;              /* Workspace size in bytes */
    uint64_t load_time;                 /* Load timestamp (cycles) */
    uint64_t last_used;                 /* Last inference timestamp */
    uint32_t inference_count;           /* Number of inferences run */
    char source_path[128];              /* Source path/identifier */
} model_slot_t;

/* ============================================================================
 * Registry Statistics
 * ============================================================================ */

typedef struct model_registry_stats {
    int total_loads;            /* Total models loaded since init */
    int total_unloads;          /* Total models unloaded */
    int total_switches;         /* Total model switches */
    int current_count;          /* Currently loaded models */
    size_t total_memory_used;   /* Total memory used by models */
} model_registry_stats_t;

/* ============================================================================
 * Registry Initialization and Shutdown
 * ============================================================================ */

/**
 * model_registry_init - Initialize the model registry
 *
 * Must be called before any other registry functions.
 * Called automatically by model_runtime_init().
 *
 * Returns: 0 on success, negative error code on failure
 */
int model_registry_init(void);

/**
 * model_registry_shutdown - Shutdown the registry and unload all models
 *
 * Unloads all models and frees all resources.
 */
void model_registry_shutdown(void);

/**
 * model_registry_is_initialized - Check if registry is initialized
 *
 * Returns: true if initialized, false otherwise
 */
bool model_registry_is_initialized(void);

/* ============================================================================
 * Model Loading and Unloading
 * ============================================================================ */

/**
 * model_registry_load - Load a model from memory into the registry
 * @data: Pointer to model data (GGUF format)
 * @size: Size of model data in bytes
 * @name: Optional name for the model (NULL to auto-detect)
 *
 * Parses the model, allocates workspace, and registers it.
 * The model is NOT automatically made active.
 *
 * Returns: Model ID (0-2) on success, negative error code on failure
 *   -1: Registry not initialized
 *   -2: No free slots available
 *   -3: Invalid model data
 *   -4: Memory allocation failed
 */
int model_registry_load(const void* data, size_t size, const char* name);

/**
 * model_registry_load_from_embedded - Load an embedded model by name
 * @name: Name of embedded model ("tinyllama", "phi2", etc.)
 *
 * Loads a model that was embedded into the kernel at build time.
 *
 * Returns: Model ID on success, negative error code on failure
 */
int model_registry_load_embedded(const char* name);

/**
 * model_registry_unload - Unload a model from the registry
 * @model_id: ID of the model to unload
 *
 * Frees model memory and workspace. Cannot unload the active model
 * unless it's the only loaded model (which will deactivate it first).
 *
 * Returns: 0 on success, negative error code on failure
 *   -1: Invalid model ID
 *   -2: Model not loaded
 *   -3: Cannot unload active model (switch first)
 */
int model_registry_unload(int model_id);

/* ============================================================================
 * Model Switching
 * ============================================================================ */

/**
 * model_registry_switch - Switch to a different loaded model
 * @model_id: ID of the model to activate
 *
 * Makes the specified model active for inference. The previously
 * active model remains loaded but becomes inactive.
 *
 * Returns: 0 on success, negative error code on failure
 *   -1: Invalid model ID
 *   -2: Model not loaded
 */
int model_registry_switch(int model_id);

/**
 * model_registry_get_active - Get the currently active model
 *
 * Returns: Pointer to active model, NULL if none active
 */
struct embodios_model* model_registry_get_active(void);

/**
 * model_registry_get_active_id - Get the ID of the active model
 *
 * Returns: Active model ID (0-2), MODEL_ID_INVALID if none
 */
int model_registry_get_active_id(void);

/* ============================================================================
 * Model Queries
 * ============================================================================ */

/**
 * model_registry_get - Get model by ID
 * @model_id: Model ID (0-2)
 *
 * Returns: Pointer to model, NULL if not loaded
 */
struct embodios_model* model_registry_get(int model_id);

/**
 * model_registry_get_slot - Get model slot by ID
 * @model_id: Model ID (0-2)
 *
 * Returns: Pointer to slot, NULL if invalid ID
 */
const model_slot_t* model_registry_get_slot(int model_id);

/**
 * model_registry_find_by_name - Find model ID by name
 * @name: Model name to search for
 *
 * Returns: Model ID if found, MODEL_ID_INVALID if not found
 */
int model_registry_find_by_name(const char* name);

/**
 * model_registry_count - Get number of loaded models
 *
 * Returns: Number of models currently loaded (0-3)
 */
int model_registry_count(void);

/**
 * model_registry_has_free_slot - Check if a slot is available
 *
 * Returns: true if at least one slot is free, false otherwise
 */
bool model_registry_has_free_slot(void);

/* ============================================================================
 * Statistics and Debugging
 * ============================================================================ */

/**
 * model_registry_get_stats - Get registry statistics
 * @stats: Output structure for statistics
 */
void model_registry_get_stats(model_registry_stats_t* stats);

/**
 * model_registry_print_status - Print registry status to console
 *
 * Shows all loaded models, their states, and memory usage.
 */
void model_registry_print_status(void);

/**
 * model_registry_print_stats - Print registry statistics
 */
void model_registry_print_stats(void);

/* ============================================================================
 * Inference Integration
 * ============================================================================ */

/**
 * model_registry_record_inference - Record an inference on active model
 *
 * Called by inference engine to update statistics.
 */
void model_registry_record_inference(void);

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define MODEL_REG_OK                0
#define MODEL_REG_ERR_NOT_INIT      (-1)
#define MODEL_REG_ERR_NO_SLOT       (-2)
#define MODEL_REG_ERR_INVALID_DATA  (-3)
#define MODEL_REG_ERR_NO_MEMORY     (-4)
#define MODEL_REG_ERR_INVALID_ID    (-5)
#define MODEL_REG_ERR_NOT_LOADED    (-6)
#define MODEL_REG_ERR_IS_ACTIVE     (-7)
#define MODEL_REG_ERR_NOT_FOUND     (-8)

/**
 * model_registry_strerror - Get error message for error code
 * @err: Error code
 *
 * Returns: Human-readable error string
 */
const char* model_registry_strerror(int err);

#endif /* EMBODIOS_MODEL_REGISTRY_H */
