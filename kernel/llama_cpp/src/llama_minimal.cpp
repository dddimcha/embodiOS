/* Minimal llama.cpp API implementation for bare-metal kernel
 * Only implements the functions we actually use in llama_kernel.cpp
 */

#include "../include/llama.h"
#include "../include/ggml.h"
#include <string.h>

// Stub implementations - just enough to compile and link

void llama_backend_init(void) {
    // Initialize GGML time
    ggml_time_init();

    // Initialize f16 tables
    struct ggml_init_params params = { 0, NULL, false };
    struct ggml_context * ctx = ggml_init(params);
    ggml_free(ctx);
}

void llama_backend_free(void) {
    // Nothing to free in minimal version
}

// Model loading - stub for now
struct llama_model * llama_load_model_from_file(
        const char * path_model,
        struct llama_model_params params) {
    (void)path_model;
    (void)params;
    // TODO: Implement minimal GGUF loading
    return NULL;
}

void llama_model_free(struct llama_model * model) {
    if (model) {
        // TODO: Free model resources
    }
}

// Context creation - stub
struct llama_context * llama_new_context_with_model(
        struct llama_model * model,
        struct llama_context_params params) {
    (void)model;
    (void)params;
    // TODO: Implement minimal context
    return NULL;
}

void llama_free(struct llama_context * ctx) {
    if (ctx) {
        // TODO: Free context resources
    }
}

// Model parameters
struct llama_model_params llama_model_default_params(void) {
    struct llama_model_params result = {};
    result.n_gpu_layers = 0;
    result.use_mmap = false;
    return result;
}

// Context parameters
struct llama_context_params llama_context_default_params(void) {
    struct llama_context_params result = {};
    result.n_ctx = 512;
    result.n_batch = 128;
    result.n_threads = 1;
    return result;
}

// Vocab functions - minimal stubs
const struct llama_vocab* llama_model_get_vocab(const struct llama_model * model) {
    (void)model;
    // TODO: Return actual vocab
    return NULL;
}

// Tokenization
int32_t llama_tokenize(
        const struct llama_vocab * vocab,
        const char * text,
        int32_t text_len,
        llama_token * tokens,
        int32_t n_tokens_max,
        bool add_special,
        bool parse_special) {
    (void)vocab; (void)text; (void)text_len; (void)add_special; (void)parse_special;

    // Minimal tokenization: one token per character
    int n_tokens = 0;
    for (int i = 0; i < text_len && n_tokens < n_tokens_max; i++) {
        if (tokens) {
            tokens[n_tokens] = (llama_token)text[i];
        }
        n_tokens++;
    }
    return n_tokens;
}

// Token to text
int32_t llama_token_to_piece(
        const struct llama_vocab * vocab,
        llama_token token,
        char * buf,
        int32_t length,
        int32_t lstrip,
        bool special) {
    (void)vocab; (void)lstrip; (void)special;

    if (buf && length > 0) {
        buf[0] = (char)token;
        if (length > 1) {
            buf[1] = '\0';
        }
        return 1;
    }
    return 0;
}

// Token checking
bool llama_vocab_is_eog(const struct llama_vocab * vocab, llama_token token) {
    (void)vocab;
    return token == 0 || token == 2; // 0=padding, 2=EOS
}

// Batch creation
struct llama_batch llama_batch_get_one(
        llama_token * tokens,
        int32_t n_tokens) {
    struct llama_batch result = {};
    result.token = tokens;
    result.n_tokens = n_tokens;
    return result;
}

// Decoding
int32_t llama_decode(
        struct llama_context * ctx,
        struct llama_batch batch) {
    (void)ctx; (void)batch;
    // TODO: Implement actual forward pass through model
    return 0; // Success
}

// Sampling
struct llama_sampler_chain_params llama_sampler_chain_default_params(void) {
    struct llama_sampler_chain_params result = {};
    result.no_perf = true;
    return result;
}

struct llama_sampler * llama_sampler_chain_init(
        struct llama_sampler_chain_params params) {
    (void)params;
    // TODO: Implement sampler
    return NULL;
}

void llama_sampler_chain_add(
        struct llama_sampler * chain,
        struct llama_sampler * smpl) {
    (void)chain; (void)smpl;
}

struct llama_sampler * llama_sampler_init_greedy(void) {
    // TODO: Implement greedy sampler
    return NULL;
}

llama_token llama_sampler_sample(
        struct llama_sampler * smpl,
        struct llama_context * ctx,
        int32_t idx) {
    (void)smpl; (void)ctx; (void)idx;
    // TODO: Implement sampling from logits
    return 'A'; // Stub: return 'A' for now
}

void llama_sampler_free(struct llama_sampler * smpl) {
    if (smpl) {
        // TODO: Free sampler
    }
}

int64_t llama_time_us(void) {
    return ggml_time_us();
}

// ========================================================================
// Minimal GGML stubs (our GGML source files are incomplete)
// ========================================================================

extern "C" {

static int64_t g_time_start = 0;

void ggml_time_init(void) {
    g_time_start = 0; // TODO: Get real time
}

int64_t ggml_time_us(void) {
    // TODO: Return real microseconds since ggml_time_init()
    return g_time_start++;
}

struct ggml_context * ggml_init(struct ggml_init_params params) {
    (void)params;
    // TODO: Implement context initialization
    return NULL;
}

void ggml_free(struct ggml_context * ctx) {
    (void)ctx;
    // TODO: Free context
}

} // extern "C"
