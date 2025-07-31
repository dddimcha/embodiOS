/* Example embedded AI model for testing */

#include "embodios/model.h"

/* Simple example model data */
const struct embodios_model example_model __attribute__((section("__DATA,__model_weights"))) = {
    .magic = 0x454D424F,  /* 'EMBO' */
    .version_major = 1,
    .version_minor = 0,
    .name = "TinyTest",
    .arch = "simple_mlp",
    .param_count = 1024,  /* 1K parameters for testing */
    .memory_required = 1024 * 1024,  /* 1MB workspace */
    .capabilities = MODEL_CAP_TEXT_GEN,
    .tokenizer_type = 1,  /* Simple ASCII tokenizer */
    .reserved = {0}
};

/* Example weight data (just for testing) */
const float example_weights[] __attribute__((section("__DATA,__model_weights"))) = {
    0.1f, 0.2f, 0.3f, 0.4f,  /* Example weight values */
    /* ... more weights would go here ... */
};