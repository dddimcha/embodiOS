/* Fast transformer init - skip model loading */
#include <embodios/types.h>
#include <embodios/kernel.h>
#include <embodios/console.h>
#include <embodios/model.h>

/* Global state */
static int g_initialized = 0;
static struct transformer_config {
    int vocab_size;
    int n_embd;
    int n_layer;
    int n_head;
} g_config;

int transformer_init(struct embodios_model* model)
{
    (void)model; /* unused */
    
    console_printf("Transformer: Fast init (no model load)\n");
    
    /* Set minimal config */
    g_config.vocab_size = 1000;
    g_config.n_embd = 256;
    g_config.n_layer = 2;
    g_config.n_head = 4;
    
    g_initialized = 1;
    
    console_printf("Transformer: Ready\n");
    return 0;
}

/* Stub implementations for now */
void transformer_forward(int* tokens, int n_tokens, float* logits)
{
    (void)tokens;
    (void)n_tokens;
    
    /* Just return some dummy logits */
    for (int i = 0; i < 1000; i++) {
        logits[i] = 0.001f;
    }
    logits[0] = 0.9f; /* High prob for first token */
}

int transformer_sample(float* logits, float temperature)
{
    (void)logits;
    (void)temperature;
    
    /* Just return a simple token */
    return 42; /* "the" or something */
}

void transformer_reset_cache(void)
{
    /* Nothing to reset in stub */
}