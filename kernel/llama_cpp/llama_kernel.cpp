/* LLaMA.cpp kernel wrapper - Real implementation
 * Provides AI inference using llama.cpp with memory-backed model
 */

#include "../include/embodios/types.h"
#include "../include/embodios/console.h"
#include "include/llama.h"
#include <string.h>

extern "C" {

// External model data (embedded GGUF)
extern const uint8_t _binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_start[];
extern const uint8_t _binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_end[];

struct llama_kernel_context {
    llama_model* model;
    llama_context* ctx;
    llama_sampler* sampler;
    int initialized;
};

static struct llama_kernel_context g_llama = {nullptr, nullptr, nullptr, 0};

/* Initialize llama.cpp with embedded model */
int llama_kernel_init(void) {
    if (g_llama.initialized) {
        console_printf("[LLaMA] Already initialized\n");
        return 0;
    }

    console_printf("[LLaMA] Initializing llama.cpp in kernel...\n");

    // Get embedded model size
    size_t model_size = (size_t)(_binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_end -
                                  _binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_start);
    console_printf("[LLaMA] Embedded model: %zu MB\n", model_size / (1024 * 1024));

    // Initialize llama backend
    llama_backend_init();
    console_printf("[LLaMA] Backend initialized\n");

    // Set up model parameters
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = 0;  // CPU only in kernel
    model_params.use_mmap = false;   // Use memory directly (no mmap in kernel)

    // Load model from memory using fake filename
    // Our custom fopen() in kernel_stubs.cpp will return memory-backed FILE*
    console_printf("[LLaMA] Loading model from embedded memory...\n");
    g_llama.model = llama_load_model_from_file("tinyllama.gguf", model_params);

    if (!g_llama.model) {
        console_printf("[LLaMA] ERROR: Failed to load model!\n");
        return -1;
    }
    console_printf("[LLaMA] Model loaded successfully!\n");

    // Create context for inference
    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = 512;          // Context size
    ctx_params.n_batch = 128;        // Batch size
    ctx_params.n_threads = 1;        // Single-threaded in kernel

    g_llama.ctx = llama_new_context_with_model(g_llama.model, ctx_params);
    if (!g_llama.ctx) {
        console_printf("[LLaMA] ERROR: Failed to create context!\n");
        llama_free_model(g_llama.model);
        g_llama.model = nullptr;
        return -1;
    }
    console_printf("[LLaMA] Context created\n");

    // Create sampler for token sampling
    auto sparams = llama_sampler_chain_default_params();
    g_llama.sampler = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(g_llama.sampler, llama_sampler_init_greedy());

    console_printf("[LLaMA] Sampler initialized\n");

    g_llama.initialized = 1;
    console_printf("[LLaMA] ✅ Initialization complete!\n");
    return 0;
}

/* Run inference with prompt */
int llama_kernel_infer(const char* prompt, char* response, size_t max_response) {
    if (!g_llama.initialized) {
        console_printf("[LLaMA] Not initialized\n");
        return -1;
    }

    if (!g_llama.model || !g_llama.ctx) {
        console_printf("[LLaMA] Model not loaded\n");
        return -1;
    }

    console_printf("[LLaMA] Inference: '%s'\n", prompt);

    // Get vocab for tokenization
    const struct llama_vocab* vocab = llama_model_get_vocab(g_llama.model);

    // Tokenize the prompt
    const int n_prompt_tokens = -llama_tokenize(
        vocab, prompt, strlen(prompt), nullptr, 0, true, true
    );

    if (n_prompt_tokens <= 0) {
        console_printf("[LLaMA] ERROR: Failed to tokenize prompt\n");
        return -1;
    }

    llama_token* tokens = new llama_token[n_prompt_tokens + 256];  // +256 for generation
    if (!tokens) {
        console_printf("[LLaMA] ERROR: Out of memory\n");
        return -1;
    }

    int n_tokens = llama_tokenize(
        vocab, prompt, strlen(prompt), tokens, n_prompt_tokens, true, true
    );

    console_printf("[LLaMA] Tokenized: %d tokens\n", n_tokens);

    // Evaluate the prompt
    llama_batch batch = llama_batch_get_one(tokens, n_tokens);
    if (llama_decode(g_llama.ctx, batch)) {
        console_printf("[LLaMA] ERROR: Failed to eval prompt\n");
        delete[] tokens;
        return -1;
    }

    // Generate response tokens (up to 32 tokens)
    size_t response_pos = 0;
    const int max_tokens = 32;

    // vocab already obtained above for tokenization

    for (int i = 0; i < max_tokens; i++) {
        // Sample next token
        llama_token new_token = llama_sampler_sample(g_llama.sampler, g_llama.ctx, -1);

        // Check for end of sequence
        if (llama_vocab_is_eog(vocab, new_token)) {
            break;
        }

        // Decode token to text
        char buf[32];
        int n_chars = llama_token_to_piece(vocab, new_token, buf, sizeof(buf), 0, true);

        if (n_chars > 0 && response_pos + n_chars < max_response - 1) {
            for (int j = 0; j < n_chars; j++) {
                response[response_pos++] = buf[j];
            }
        }

        // Evaluate next token
        tokens[n_tokens++] = new_token;
        llama_batch next_batch = llama_batch_get_one(&new_token, 1);
        if (llama_decode(g_llama.ctx, next_batch)) {
            break;
        }
    }

    response[response_pos] = '\0';
    delete[] tokens;

    console_printf("[LLaMA] Generated %zu chars\n", response_pos);
    return 0;
}

/* Cleanup */
void llama_kernel_free(void) {
    if (g_llama.sampler) {
        llama_sampler_free(g_llama.sampler);
        g_llama.sampler = nullptr;
    }
    if (g_llama.ctx) {
        llama_free(g_llama.ctx);
        g_llama.ctx = nullptr;
    }
    if (g_llama.model) {
        llama_free_model(g_llama.model);
        g_llama.model = nullptr;
    }
    llama_backend_free();
    g_llama.initialized = 0;
    console_printf("[LLaMA] Cleaned up\n");
}

} // extern "C"
