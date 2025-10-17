/* REAL Embedded Weights from TinyLlama GGUF
 * This loads the ACTUAL trained weights from the GGUF file
 */

#include <embodios/types.h>
#include <embodios/console.h>

/* External GGUF loader */
extern int gguf_load_model(void* data, size_t size);
extern void* gguf_get_tensor(const char* name, size_t* out_size);

/* External model data */
extern const uint8_t _binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_start[];
extern const uint8_t _binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_end[];

static struct {
    int initialized;
    void* gguf_data;
    size_t gguf_size;
} g_weights = {0};

/* Initialize and load GGUF model */
void init_embedded_weights(void)
{
    if (g_weights.initialized) {
        console_printf("[Weights] Already initialized\n");
        return;
    }

    console_printf("[Weights] Checking for embedded GGUF model...\n");

    g_weights.gguf_data = (void*)_binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_start;
    g_weights.gguf_size = (size_t)(_binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_end -
                                    _binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_start);

    if (g_weights.gguf_size > 0) {
        console_printf("[Weights] Found GGUF model: %zu MB\n", g_weights.gguf_size / (1024 * 1024));
        console_printf("[Weights] Attempting to load GGUF model...\n");

        if (gguf_load_model(g_weights.gguf_data, g_weights.gguf_size) < 0) {
            console_printf("[Weights] ERROR: Failed to load GGUF!\n");
            console_printf("[Weights] Continuing without model weights\n");
        } else {
            console_printf("[Weights] GGUF model loaded successfully!\n");
        }
    } else {
        console_printf("[Weights] No embedded model found\n");
    }

    g_weights.initialized = 1;
    console_printf("[Weights] Init complete\n");
}

/* Get embeddings/weights from GGUF */
void* get_embeddings(const char* name)
{
    if (!g_weights.initialized) {
        console_printf("[Weights] WARNING: Not initialized, initializing now\n");
        init_embedded_weights();
    }

    console_printf("[Weights] Requested: '%s'\n", name);

    const char* gguf_names[4] = {NULL, NULL, NULL, NULL};
    int n_names = 0;

    if (name[0] == 't' && name[1] == 'o' && name[2] == 'k') {
        gguf_names[n_names++] = "token_embd.weight";
        gguf_names[n_names++] = "model.embed_tokens.weight";
        gguf_names[n_names++] = "tok_embeddings.weight";
    } else if (name[0] == 'p' && name[1] == 'o') {
        gguf_names[n_names++] = "position_embd.weight";
        gguf_names[n_names++] = "model.embed_positions.weight";
    } else if (name[0] == 'l' && name[1] == 'n' && name[2] == '_') {
        gguf_names[n_names++] = "output_norm.weight";
        gguf_names[n_names++] = "model.norm.weight";
        gguf_names[n_names++] = "norm.weight";
    } else if (name[0] == 'l' && name[1] == 'm') {
        gguf_names[n_names++] = "output.weight";
        gguf_names[n_names++] = "lm_head.weight";
        gguf_names[n_names++] = "model.lm_head.weight";
    }

    if (n_names == 0) {
        console_printf("[Weights] Unknown weight: %s\n", name);
        return NULL;
    }

    for (int i = 0; i < n_names; i++) {
        if (!gguf_names[i]) break;

        size_t size = 0;
        void* tensor = gguf_get_tensor(gguf_names[i], &size);

        if (tensor) {
            console_printf("[Weights] Found %s → %s (%zu bytes)\n", name, gguf_names[i], size);
            return tensor;
        }
    }

    console_printf("[Weights] NOT FOUND: %s (tried %d names)\n", name, n_names);

    return NULL;
}
