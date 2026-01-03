/**
 * TinyStories-15M Inference Engine for EMBODIOS
 * Implements llama.c-style transformer inference in kernel
 *
 * REAL AI INFERENCE - Ported from llama.c
 */

#include <embodios/types.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/gguf_parser.h>
#include <embodios/block.h>

/* Forward declarations for kernel functions */
extern void* memcpy(void* dest, const void* src, size_t n);
extern void* memset(void* s, int c, size_t n);
// SSE-enabled math functions (implemented in lib/math.c)
extern float sqrtf(float x);
extern float expf(float x);
extern float powf(float base, float exp);
extern float cosf(float x);
extern float sinf(float x);
extern float tanhf(float x);
extern float fabsf(float x);
extern float logf(float x);

// Simple timer implementation using x86_64 TSC (time-stamp counter)
static inline uint64_t get_timestamp(void) {
#ifdef __x86_64__
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    return 0;
#endif
}

/* SSE2 intrinsics for x86_64 optimization */
#ifdef __x86_64__
/* Prevent mm_malloc.h from being included - it needs stdlib.h */
#define _MM_MALLOC_H_INCLUDED
#include <emmintrin.h>  // SSE2
#endif

// Model configuration (TinyStories-15M)
typedef struct {
    int dim;        // transformer dimension (288)
    int hidden_dim; // for ffn layers (768)
    int n_layers;   // number of layers (6)
    int n_heads;    // number of query heads (6)
    int n_kv_heads; // number of key/value heads (6)
    int vocab_size; // vocabulary size (32000)
    int seq_len;    // max sequence length (256)
} TinyStoriesConfig;

// Transformer weights (same layout as llama.c)
typedef struct {
    float* token_embedding_table;    // (vocab_size, dim)
    float* rms_att_weight;           // (layer, dim) rmsnorm weights
    float* rms_ffn_weight;           // (layer, dim)
    float* wq;                       // (layer, dim, n_heads * head_size)
    float* wk;                       // (layer, dim, n_kv_heads * head_size)
    float* wv;                       // (layer, dim, n_kv_heads * head_size)
    float* wo;                       // (layer, n_heads * head_size, dim)
    float* w1;                       // (layer, hidden_dim, dim)
    float* w2;                       // (layer, dim, hidden_dim)
    float* w3;                       // (layer, hidden_dim, dim)
    float* rms_final_weight;         // (dim,)
    float* wcls;                     // classifier weights for logits
} TinyStoriesWeights;

// Runtime state (same as llama.c RunState)
typedef struct {
    float* x;           // activation at current time stamp (dim,)
    float* xb;          // same, but inside a residual branch (dim,)
    float* xb2;         // an additional buffer just for convenience (dim,)
    float* hb;          // buffer for hidden dimension in the ffn (hidden_dim,)
    float* hb2;         // buffer for hidden dimension in the ffn (hidden_dim,)
    float* q;           // query (dim,)
    float* k;           // key (dim,)
    float* v;           // value (dim,)
    float* att;         // buffer for scores/attention values (n_heads, seq_len)
    float* logits;      // output logits (vocab_size,)
    float* key_cache;   // (layer, seq_len, kv_dim)
    float* value_cache; // (layer, seq_len, kv_dim)
} TinyStoriesRunState;

// Vocabulary for tokenization
typedef struct {
    char** vocab;       // array of token strings
    float* vocab_scores; // scores for each token
    int vocab_size;
    int max_token_length;
} Vocabulary;

// Default configuration (fallback when GGUF parsing fails)
// TinyStories-15M values for backwards compatibility
#define DEFAULT_DIM         288
#define DEFAULT_HIDDEN_DIM  768
#define DEFAULT_N_LAYERS    6
#define DEFAULT_N_HEADS     6
#define DEFAULT_N_KV_HEADS  6
#define DEFAULT_VOCAB_SIZE  32000
#define DEFAULT_SEQ_LEN     256

// Global model state - initialized with defaults, updated from GGUF if available
static TinyStoriesConfig g_config = {
    .dim = DEFAULT_DIM,
    .hidden_dim = DEFAULT_HIDDEN_DIM,
    .n_layers = DEFAULT_N_LAYERS,
    .n_heads = DEFAULT_N_HEADS,
    .n_kv_heads = DEFAULT_N_KV_HEADS,
    .vocab_size = DEFAULT_VOCAB_SIZE,
    .seq_len = DEFAULT_SEQ_LEN
};

// Flag to track if config was loaded from GGUF
static bool g_config_from_gguf = false;
static TinyStoriesWeights g_weights;
static TinyStoriesRunState g_state;
static Vocabulary g_vocab;
static bool g_model_loaded = false;

// Token decoding buffers (llama.c pattern)
static char g_token_fallback[32];  // fallback buffer for token strings
static char g_byte_pieces[512];    // buffer for byte tokens (256 bytes * 2 chars each)

// External symbols for embedded model (if available)
extern const uint8_t _binary_tinystories_15m_bin_start[] __attribute__((weak));
extern const uint8_t _binary_tinystories_15m_bin_end[] __attribute__((weak));

// External symbols for embedded tokenizer (if available)
extern const uint8_t _binary_tokenizer_bin_start[] __attribute__((weak));
extern const uint8_t _binary_tokenizer_bin_end[] __attribute__((weak));

// ----------------------------------------------------------------------------
// Neural net blocks - PORTED FROM llama.c (lines 182-229)

/**
 * RMS normalization (llama.c line 182)
 * SSE2-optimized for x86_64
 */
static void rmsnorm(float* o, float* x, float* weight, int size) {
#ifdef __x86_64__
    // SSE2: calculate sum of squares (4 floats at a time)
    __m128 ss_vec = _mm_setzero_ps();
    int j = 0;
    for (; j + 3 < size; j += 4) {
        __m128 x_vec = _mm_loadu_ps(&x[j]);
        __m128 sq = _mm_mul_ps(x_vec, x_vec);
        ss_vec = _mm_add_ps(ss_vec, sq);
    }
    // Horizontal sum
    float ss_array[4];
    _mm_storeu_ps(ss_array, ss_vec);
    float ss = ss_array[0] + ss_array[1] + ss_array[2] + ss_array[3];
    // Handle remainder
    for (; j < size; j++) {
        ss += x[j] * x[j];
    }
    ss /= size;
    ss += 1e-5f;
    ss = 1.0f / sqrtf(ss);

    // SSE2: normalize and scale (4 floats at a time)
    __m128 ss_vec_broadcast = _mm_set1_ps(ss);
    j = 0;
    for (; j + 3 < size; j += 4) {
        __m128 x_vec = _mm_loadu_ps(&x[j]);
        __m128 w_vec = _mm_loadu_ps(&weight[j]);
        __m128 scaled = _mm_mul_ps(x_vec, ss_vec_broadcast);
        __m128 result = _mm_mul_ps(w_vec, scaled);
        _mm_storeu_ps(&o[j], result);
    }
    // Handle remainder
    for (; j < size; j++) {
        o[j] = weight[j] * (ss * x[j]);
    }
#else
    // Scalar fallback
    float ss = 0.0f;
    for (int j = 0; j < size; j++) {
        ss += x[j] * x[j];
    }
    ss /= size;
    ss += 1e-5f;
    ss = 1.0f / sqrtf(ss);
    for (int j = 0; j < size; j++) {
        o[j] = weight[j] * (ss * x[j]);
    }
#endif
}

/**
 * Softmax (llama.c line 197)
 */
static void softmax(float* x, int size) {
    // find max value (for numerical stability)
    float max_val = x[0];
    for (int i = 1; i < size; i++) {
        if (x[i] > max_val) {
            max_val = x[i];
        }
    }
    // exp and sum
    float sum = 0.0f;
    for (int i = 0; i < size; i++) {
        x[i] = expf(x[i] - max_val);
        sum += x[i];
    }
    // normalize
    for (int i = 0; i < size; i++) {
        x[i] /= sum;
    }
}

/**
 * Matrix-vector multiplication (llama.c line 217)
 * W (d,n) @ x (n,) -> xout (d,)
 * SSE2-optimized for x86_64 (~4x faster than scalar)
 */
static void matmul(float* xout, float* x, float* w, int n, int d) {
    // by far the most amount of time is spent inside this little function
#ifdef __x86_64__
    // SSE2 version: process 4 floats at a time
    for (int i = 0; i < d; i++) {
        __m128 sum_vec = _mm_setzero_ps();  // accumulator for 4 floats
        int j = 0;

        // Process 4 elements at a time with SSE2
        for (; j + 3 < n; j += 4) {
            __m128 w_vec = _mm_loadu_ps(&w[i * n + j]);  // load 4 weights
            __m128 x_vec = _mm_loadu_ps(&x[j]);           // load 4 inputs
            __m128 prod = _mm_mul_ps(w_vec, x_vec);       // multiply
            sum_vec = _mm_add_ps(sum_vec, prod);          // accumulate
        }

        // Horizontal sum of the 4 accumulators
        float sum_array[4];
        _mm_storeu_ps(sum_array, sum_vec);
        float val = sum_array[0] + sum_array[1] + sum_array[2] + sum_array[3];

        // Handle remaining elements (scalar)
        for (; j < n; j++) {
            val += w[i * n + j] * x[j];
        }

        xout[i] = val;
    }
#else
    // Fallback scalar version for non-x86_64
    for (int i = 0; i < d; i++) {
        float val = 0.0f;
        for (int j = 0; j < n; j++) {
            val += w[i * n + j] * x[j];
        }
        xout[i] = val;
    }
#endif
}

// ----------------------------------------------------------------------------
// Weight mapping - PORTED FROM llama.c (lines 111-140)

/**
 * Map weight pointers into model memory (llama.c memory_map_weights)
 * Adapted for kernel: model data is already in memory
 */
static void memory_map_weights(TinyStoriesWeights* w, TinyStoriesConfig* p,
                               float* ptr, int shared_weights) {
    int head_size = p->dim / p->n_heads;
    // make sure the multiplications below are done in 64bit to fit large models
    unsigned long long n_layers = p->n_layers;

    w->token_embedding_table = ptr;
    ptr += p->vocab_size * p->dim;
    w->rms_att_weight = ptr;
    ptr += n_layers * p->dim;
    w->wq = ptr;
    ptr += n_layers * p->dim * (p->n_heads * head_size);
    w->wk = ptr;
    ptr += n_layers * p->dim * (p->n_kv_heads * head_size);
    w->wv = ptr;
    ptr += n_layers * p->dim * (p->n_kv_heads * head_size);
    w->wo = ptr;
    ptr += n_layers * (p->n_heads * head_size) * p->dim;
    w->rms_ffn_weight = ptr;
    ptr += n_layers * p->dim;
    w->w1 = ptr;
    ptr += n_layers * p->dim * p->hidden_dim;
    w->w2 = ptr;
    ptr += n_layers * p->hidden_dim * p->dim;
    w->w3 = ptr;
    ptr += n_layers * p->dim * p->hidden_dim;
    w->rms_final_weight = ptr;
    ptr += p->dim;
    ptr += p->seq_len * head_size / 2; // skip what used to be freq_cis_real (for RoPE)
    ptr += p->seq_len * head_size / 2; // skip what used to be freq_cis_imag (for RoPE)
    w->wcls = shared_weights ? w->token_embedding_table : ptr;
}

/**
 * Load vocabulary from model file (llama.c style)
 * Vocabulary comes after weights in the model file
 */
static int load_vocabulary(const uint8_t* vocab_ptr, TinyStoriesConfig* p) {
    console_printf("Loading vocabulary...\n");

    // Read max_token_length
    const int* max_len_ptr = (const int*)vocab_ptr;
    g_vocab.max_token_length = *max_len_ptr;
    g_vocab.vocab_size = p->vocab_size;

    console_printf("  max_token_length: %d\n", g_vocab.max_token_length);

    // Allocate arrays
    g_vocab.vocab = (char**)heap_alloc(p->vocab_size * sizeof(char*));
    g_vocab.vocab_scores = (float*)heap_alloc(p->vocab_size * sizeof(float));

    if (!g_vocab.vocab || !g_vocab.vocab_scores) {
        console_printf("ERROR: Failed to allocate vocabulary arrays\n");
        return -1;
    }

    // Read tokens (skip first int which is max_token_length)
    const uint8_t* ptr = vocab_ptr + sizeof(int);

    for (int i = 0; i < p->vocab_size; i++) {
        // Read score
        g_vocab.vocab_scores[i] = *(const float*)ptr;
        ptr += sizeof(float);

        // Read token length
        int len = *(const int*)ptr;
        ptr += sizeof(int);

        // Allocate and copy token string
        g_vocab.vocab[i] = (char*)heap_alloc(len + 1);
        if (!g_vocab.vocab[i]) {
            console_printf("ERROR: Failed to allocate token %d\n", i);
            return -1;
        }

        memcpy(g_vocab.vocab[i], ptr, len);
        g_vocab.vocab[i][len] = '\0';
        ptr += len;
    }

    console_printf("Vocabulary loaded: %d tokens\n", p->vocab_size);

    // Initialize byte_pieces buffer for raw byte tokens (llama.c pattern)
    // Each byte value is stored as a 2-char null-terminated string
    for (int i = 0; i < 256; i++) {
        g_byte_pieces[i * 2] = (unsigned char)i;
        g_byte_pieces[i * 2 + 1] = '\0';
    }

    return 0;
}

/**
 * Allocate RunState buffers using kernel heap (llama.c malloc_run_state)
 * Adapted to use heap_alloc() instead of calloc()
 */
static int malloc_run_state(TinyStoriesRunState* s, TinyStoriesConfig* p) {
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;

    s->x = (float*)heap_alloc(p->dim * sizeof(float));
    s->xb = (float*)heap_alloc(p->dim * sizeof(float));
    s->xb2 = (float*)heap_alloc(p->dim * sizeof(float));
    s->hb = (float*)heap_alloc(p->hidden_dim * sizeof(float));
    s->hb2 = (float*)heap_alloc(p->hidden_dim * sizeof(float));
    s->q = (float*)heap_alloc(p->dim * sizeof(float));
    s->key_cache = (float*)heap_alloc(p->n_layers * p->seq_len * kv_dim * sizeof(float));
    s->value_cache = (float*)heap_alloc(p->n_layers * p->seq_len * kv_dim * sizeof(float));
    s->att = (float*)heap_alloc(p->n_heads * p->seq_len * sizeof(float));
    s->logits = (float*)heap_alloc(p->vocab_size * sizeof(float));

    // ensure all allocations succeeded
    if (!s->x || !s->xb || !s->xb2 || !s->hb || !s->hb2 || !s->q
     || !s->key_cache || !s->value_cache || !s->att || !s->logits) {
        console_printf("ERROR: RunState allocation failed!\n");
        return -1;
    }

    // Zero-initialize the memory (heap_alloc doesn't zero like calloc)
    memset(s->x, 0, p->dim * sizeof(float));
    memset(s->xb, 0, p->dim * sizeof(float));
    memset(s->xb2, 0, p->dim * sizeof(float));
    memset(s->hb, 0, p->hidden_dim * sizeof(float));
    memset(s->hb2, 0, p->hidden_dim * sizeof(float));
    memset(s->q, 0, p->dim * sizeof(float));
    memset(s->key_cache, 0, p->n_layers * p->seq_len * kv_dim * sizeof(float));
    memset(s->value_cache, 0, p->n_layers * p->seq_len * kv_dim * sizeof(float));
    memset(s->att, 0, p->n_heads * p->seq_len * sizeof(float));
    memset(s->logits, 0, p->vocab_size * sizeof(float));

    return 0;
}

/* Disk-loaded model buffer */
static uint8_t* g_disk_model_data = NULL;
static size_t g_disk_model_size = 0;

/**
 * Load model from raw data buffer
 * Called by tinystories_load_model() or tinystories_load_from_disk()
 */
static int tinystories_load_from_data(const uint8_t* model_data, size_t model_size) {
    console_printf("Model size: %zu MB\n", model_size / (1024 * 1024));

    // Try GGUF format first (check magic number: "GGUF")
    if (model_size >= 4 && model_data[0] == 'G' && model_data[1] == 'G' &&
        model_data[2] == 'U' && model_data[3] == 'F') {

        console_printf("Detected GGUF format, parsing metadata...\n");

        if (gguf_parser_load(model_data, model_size) == 0) {
            const struct gguf_model_arch* arch = gguf_parser_get_arch();
            if (arch) {
                // Load config from GGUF metadata
                g_config.dim = arch->embedding_length;
                g_config.hidden_dim = arch->feed_forward_length;
                g_config.n_layers = arch->block_count;
                g_config.n_heads = arch->attention_head_count;
                g_config.n_kv_heads = arch->attention_head_count_kv;
                g_config.vocab_size = arch->vocab_size;
                g_config.seq_len = arch->context_length;
                g_config_from_gguf = true;

                console_printf("Configuration (from GGUF metadata):\n");
                console_printf("  Model: %s\n", arch->general_name);
                console_printf("  Architecture: %s\n", arch->general_architecture);
            }
        } else {
            console_printf("GGUF parsing failed, using defaults\n");
        }
    } else {
        console_printf("Non-GGUF format, using default config\n");
    }

    // Print final configuration
    console_printf("Configuration%s:\n", g_config_from_gguf ? " (GGUF)" : " (defaults)");
    console_printf("  dim: %d\n", g_config.dim);
    console_printf("  hidden_dim: %d\n", g_config.hidden_dim);
    console_printf("  n_layers: %d\n", g_config.n_layers);
    console_printf("  n_heads: %d\n", g_config.n_heads);
    console_printf("  n_kv_heads: %d\n", g_config.n_kv_heads);
    console_printf("  vocab_size: %d\n", g_config.vocab_size);
    console_printf("  seq_len: %d\n", g_config.seq_len);

    // Validate config before proceeding
    if (g_config.dim == 0 || g_config.n_heads == 0 || g_config.vocab_size == 0) {
        console_printf("ERROR: Invalid model configuration\n");
        return 0;
    }

    // Determine weight layout based on format
    int shared_weights = 1;
    float* weights_ptr;

    if (g_config_from_gguf) {
        // GGUF: weights start after header and metadata
        weights_ptr = (float*)gguf_parser_get_tensor_data();
    } else {
        // Legacy: weights start after 7-int config header
        weights_ptr = (float*)(model_data + sizeof(TinyStoriesConfig));
    }

    if (!weights_ptr) {
        console_printf("ERROR: Could not locate weight data\n");
        return 0;
    }

    // Map weight pointers
    memory_map_weights(&g_weights, &g_config, weights_ptr, shared_weights);

    // Calculate vocabulary offset (after all weights)
    int head_size = g_config.dim / g_config.n_heads;
    size_t weights_size = 0;
    weights_size += g_config.vocab_size * g_config.dim;  // token_embedding_table
    weights_size += g_config.n_layers * g_config.dim;    // rms_att_weight
    weights_size += g_config.n_layers * g_config.dim * (g_config.n_heads * head_size);  // wq
    weights_size += g_config.n_layers * g_config.dim * (g_config.n_kv_heads * head_size);  // wk
    weights_size += g_config.n_layers * g_config.dim * (g_config.n_kv_heads * head_size);  // wv
    weights_size += g_config.n_layers * (g_config.n_heads * head_size) * g_config.dim;  // wo
    weights_size += g_config.n_layers * g_config.dim;    // rms_ffn_weight
    weights_size += g_config.n_layers * g_config.dim * g_config.hidden_dim;  // w1
    weights_size += g_config.n_layers * g_config.hidden_dim * g_config.dim;  // w2
    weights_size += g_config.n_layers * g_config.dim * g_config.hidden_dim;  // w3
    weights_size += g_config.dim;                        // rms_final_weight
    weights_size += g_config.seq_len * head_size / 2;    // freq_cis_real (skipped)
    weights_size += g_config.seq_len * head_size / 2;    // freq_cis_imag (skipped)
    if (!shared_weights) {
        weights_size += g_config.vocab_size * g_config.dim;  // wcls
    }

    // Try to load vocabulary from embedded tokenizer first
    extern int tokenizer_embedded(void);
    const uint8_t* vocab_ptr = NULL;
    if (tokenizer_embedded()) {
        console_printf("Loading vocabulary from embedded tokenizer...\n");
        vocab_ptr = _binary_tokenizer_bin_start;
    } else {
        // Fallback: try to find vocabulary in model file
        // llama.c format: config (28 bytes) + weights + vocab
        console_printf("Loading vocabulary from model file...\n");
        vocab_ptr = model_data + sizeof(TinyStoriesConfig) + weights_size * sizeof(float);
    }

    // Load vocabulary (may fail if not embedded in model file)
    if (load_vocabulary(vocab_ptr, &g_config) != 0) {
        console_printf("WARNING: Could not load vocabulary\n");
        console_printf("Using fallback ASCII tokenizer\n");
        // Clear vocab structure to indicate no vocabulary loaded
        g_vocab.vocab = NULL;
        g_vocab.vocab_scores = NULL;
        g_vocab.vocab_size = 0;
        g_vocab.max_token_length = 0;
    }

    // Allocate RunState buffers
    if (malloc_run_state(&g_state, &g_config) != 0) {
        console_printf("ERROR: Failed to allocate RunState\n");
        return -1;
    }

    console_printf("TinyStories model loaded successfully!\n");
    console_printf("REAL INFERENCE ENGINE READY - ported from llama.c\n");

    g_model_loaded = true;
    return 0;
}

/* External helper to check if model is embedded */
extern int tinystories_model_embedded(void);

/**
 * Load model from embedded binary data
 */
int tinystories_load_model(void) {
    console_printf("Loading AI model...\n");

    // Check if model weights are embedded using helper function
    if (!tinystories_model_embedded()) {
        console_printf("WARNING: No model weights embedded in kernel\n");
        console_printf("AI inference will use fallback mode.\n");
        console_printf("Use 'loadtiny' command to load from disk.\n");
        return 0;  // Success but no weights
    }

    const uint8_t* model_data = _binary_tinystories_15m_bin_start;
    size_t model_size = (size_t)(_binary_tinystories_15m_bin_end - _binary_tinystories_15m_bin_start);

    return tinystories_load_from_data(model_data, model_size);
}

/**
 * Load TinyStories model from VirtIO block device
 * Reads the entire model file from disk sector 0 into heap memory
 */
int tinystories_load_from_disk(void) {
    console_printf("\n");
    console_printf("Loading TinyStories model from disk...\n");

    // Get the first block device
    block_device_t* dev = block_get_device_by_index(0);
    if (!dev) {
        console_printf("ERROR: No block device available\n");
        console_printf("Make sure QEMU has a VirtIO disk attached.\n");
        return -1;
    }

    console_printf("Using block device: %s\n", dev->name);
    console_printf("Device capacity: %llu sectors\n", (unsigned long long)dev->total_sectors);

    // Read the first sector to get the config header
    uint8_t header[512];
    if (block_read(dev, 0, 1, header) != 0) {
        console_printf("ERROR: Failed to read header sector\n");
        return -2;
    }

    // Parse config from header (first 28 bytes = 7 ints)
    int* config = (int*)header;
    int dim = config[0];
    int hidden_dim = config[1];
    int n_layers = config[2];
    int n_heads = config[3];
    int n_kv_heads = config[4];
    int vocab_size = config[5];
    int seq_len = config[6];

    // Validate config (basic sanity checks)
    if (dim <= 0 || dim > 8192 || hidden_dim <= 0 || hidden_dim > 32768 ||
        n_layers <= 0 || n_layers > 128 || n_heads <= 0 || n_heads > 256 ||
        vocab_size <= 0 || vocab_size > 256000) {
        console_printf("ERROR: Invalid model config in disk header\n");
        console_printf("  dim=%d hidden=%d layers=%d heads=%d vocab=%d\n",
                       dim, hidden_dim, n_layers, n_heads, vocab_size);
        console_printf("This might not be a TinyStories .bin file.\n");
        return -3;
    }

    console_printf("Model config from disk:\n");
    console_printf("  dim: %d, hidden: %d, layers: %d\n", dim, hidden_dim, n_layers);
    console_printf("  heads: %d, kv_heads: %d, vocab: %d\n", n_heads, n_kv_heads, vocab_size);

    // Calculate approximate model size
    // This is a rough estimate - the actual calculation is complex
    int head_size = dim / n_heads;
    size_t param_bytes = 0;
    param_bytes += vocab_size * dim * 4;  // token embeddings
    param_bytes += n_layers * dim * 4;    // rms_att
    param_bytes += n_layers * dim * (n_heads * head_size) * 4;  // wq
    param_bytes += n_layers * dim * (n_kv_heads * head_size) * 4;  // wk
    param_bytes += n_layers * dim * (n_kv_heads * head_size) * 4;  // wv
    param_bytes += n_layers * (n_heads * head_size) * dim * 4;  // wo
    param_bytes += n_layers * dim * 4;    // rms_ffn
    param_bytes += n_layers * dim * hidden_dim * 4;  // w1
    param_bytes += n_layers * hidden_dim * dim * 4;  // w2
    param_bytes += n_layers * dim * hidden_dim * 4;  // w3
    param_bytes += dim * 4;               // rms_final
    param_bytes += seq_len * head_size;   // freq_cis

    // Add extra for vocab data (scores + token strings)
    size_t model_size = param_bytes + 28 + vocab_size * 64;  // rough estimate

    // Round up to sector boundary
    uint32_t sector_count = (model_size + 511) / 512;

    // Cap at device capacity
    if (sector_count > dev->total_sectors) {
        sector_count = (uint32_t)dev->total_sectors;
    }

    size_t total_bytes = sector_count * 512ULL;
    console_printf("Reading %u sectors (%zu MB)...\n", sector_count, total_bytes / (1024*1024));

    // Free any previously loaded disk model
    if (g_disk_model_data) {
        heap_free(g_disk_model_data);
        g_disk_model_data = NULL;
    }

    // Allocate memory for model
    g_disk_model_data = (uint8_t*)heap_alloc(total_bytes);
    if (!g_disk_model_data) {
        console_printf("ERROR: Failed to allocate %zu bytes for model\n", total_bytes);
        return -4;
    }

    // Read model from disk in chunks
    #define CHUNK_SECTORS 256  // Read 128KB at a time
    uint32_t sectors_read = 0;
    uint32_t progress_last = 0;

    while (sectors_read < sector_count) {
        uint32_t chunk = sector_count - sectors_read;
        if (chunk > CHUNK_SECTORS) chunk = CHUNK_SECTORS;

        int ret = block_read(dev, sectors_read, chunk, g_disk_model_data + sectors_read * 512);
        if (ret != 0) {
            console_printf("ERROR: Disk read failed at sector %u (error %d)\n", sectors_read, ret);
            heap_free(g_disk_model_data);
            g_disk_model_data = NULL;
            return -5;
        }

        sectors_read += chunk;

        // Show progress every 10%
        uint32_t progress = (sectors_read * 100) / sector_count;
        if (progress >= progress_last + 10) {
            console_printf("  %u%% (%u MB read)\n", progress, (sectors_read * 512) / (1024*1024));
            progress_last = progress;
        }
    }

    console_printf("Disk read complete.\n");
    g_disk_model_size = total_bytes;

    // Now load from the buffer
    return tinystories_load_from_data(g_disk_model_data, g_disk_model_size);
}

// ----------------------------------------------------------------------------
// Transformer forward pass - PORTED FROM llama.c (lines 231-362)

/**
 * Forward pass through transformer to get logits for next token
 * This is the REAL inference engine from llama.c
 */
static float* forward(int token, int pos) {
    // convenience variables
    TinyStoriesConfig* p = &g_config;
    TinyStoriesWeights* w = &g_weights;
    TinyStoriesRunState* s = &g_state;
    float *x = s->x;
    int dim = p->dim;
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    int kv_mul = p->n_heads / p->n_kv_heads; // integer multiplier of the kv sharing in multiquery
    int hidden_dim = p->hidden_dim;
    int head_size = dim / p->n_heads;

    // Precompute 1/sqrt(head_size) for attention scaling
    float inv_sqrt_head_size = 1.0f / sqrtf((float)head_size);

    // copy the token embedding into x
    float* content_row = w->token_embedding_table + token * dim;
    memcpy(x, content_row, dim*sizeof(*x));

    // forward all the layers
    for(unsigned long long l = 0; l < p->n_layers; l++) {

        // attention rmsnorm
        rmsnorm(s->xb, x, w->rms_att_weight + l*dim, dim);

        // key and value point to the kv cache
        int loff = l * p->seq_len * kv_dim; // kv cache layer offset for convenience
        s->k = s->key_cache + loff + pos * kv_dim;
        s->v = s->value_cache + loff + pos * kv_dim;

        // qkv matmuls for this position
        matmul(s->q, s->xb, w->wq + l*dim*dim, dim, dim);
        matmul(s->k, s->xb, w->wk + l*dim*kv_dim, dim, kv_dim);
        matmul(s->v, s->xb, w->wv + l*dim*kv_dim, dim, kv_dim);

        // RoPE relative positional encoding: complex-valued rotate q and k in each head
        // This is CRITICAL for the model to distinguish token positions
        for (int i = 0; i < dim; i += 2) {
            int head_dim = i % head_size;
            float freq = 1.0f / powf(10000.0f, head_dim / (float)head_size);
            float val = pos * freq;
            float fcr = cosf(val);
            float fci = sinf(val);
            int rotn = i < kv_dim ? 2 : 1; // how many vectors? 2 = q & k, 1 = q only
            for (int v = 0; v < rotn; v++) {
                float* vec = v == 0 ? s->q : s->k; // the vector to rotate (query or key)
                float v0 = vec[i];
                float v1 = vec[i+1];
                vec[i]   = v0 * fcr - v1 * fci;
                vec[i+1] = v0 * fci + v1 * fcr;
            }
        }

        // multihead attention. iterate over all heads
        for (int h = 0; h < p->n_heads; h++) {
            // get the query vector for this head
            float* q = s->q + h * head_size;
            // attention scores for this head
            float* att = s->att + h * p->seq_len;
            // iterate over all timesteps, including the current one
            for (int t = 0; t <= pos; t++) {
                // get the key vector for this head and at this timestep
                float* k = s->key_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
                // calculate the attention score as the dot product of q and k
#ifdef __x86_64__
                // SSE2 dot product
                __m128 score_vec = _mm_setzero_ps();
                int i = 0;
                for (; i + 3 < head_size; i += 4) {
                    __m128 q_vec = _mm_loadu_ps(&q[i]);
                    __m128 k_vec = _mm_loadu_ps(&k[i]);
                    __m128 prod = _mm_mul_ps(q_vec, k_vec);
                    score_vec = _mm_add_ps(score_vec, prod);
                }
                float score_array[4];
                _mm_storeu_ps(score_array, score_vec);
                float score = score_array[0] + score_array[1] + score_array[2] + score_array[3];
                for (; i < head_size; i++) {
                    score += q[i] * k[i];
                }
#else
                float score = 0.0f;
                for (int i = 0; i < head_size; i++) {
                    score += q[i] * k[i];
                }
#endif
                score *= inv_sqrt_head_size;
                // save the score to the attention buffer
                att[t] = score;
            }

            // softmax the scores to get attention weights, from 0..pos inclusively
            softmax(att, pos + 1);

            // weighted sum of the values, store back into xb
            float* xb = s->xb + h * head_size;
            memset(xb, 0, head_size * sizeof(float));
            for (int t = 0; t <= pos; t++) {
                // get the value vector for this head and at this timestep
                float* v = s->value_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
                // get the attention weight for this timestep
                float a = att[t];
#ifdef __x86_64__
                // SSE2 weighted accumulation
                __m128 a_vec = _mm_set1_ps(a);
                int i = 0;
                for (; i + 3 < head_size; i += 4) {
                    __m128 xb_vec = _mm_loadu_ps(&xb[i]);
                    __m128 v_vec = _mm_loadu_ps(&v[i]);
                    __m128 weighted = _mm_mul_ps(a_vec, v_vec);
                    xb_vec = _mm_add_ps(xb_vec, weighted);
                    _mm_storeu_ps(&xb[i], xb_vec);
                }
                for (; i < head_size; i++) {
                    xb[i] += a * v[i];
                }
#else
                // accumulate the weighted value into xb
                for (int i = 0; i < head_size; i++) {
                    xb[i] += a * v[i];
                }
#endif
            }
        }

        // final matmul to get the output of the attention
        matmul(s->xb2, s->xb, w->wo + l*dim*dim, dim, dim);

        // residual connection back into x
        for (int i = 0; i < dim; i++) {
            x[i] += s->xb2[i];
        }

        // ffn rmsnorm
        rmsnorm(s->xb, x, w->rms_ffn_weight + l*dim, dim);

        // Now for FFN in PyTorch we have: self.w2(F.silu(self.w1(x)) * self.w3(x))
        // first calculate self.w1(x) and self.w3(x)
        matmul(s->hb, s->xb, w->w1 + l*dim*hidden_dim, dim, hidden_dim);
        matmul(s->hb2, s->xb, w->w3 + l*dim*hidden_dim, dim, hidden_dim);

        // SwiGLU non-linearity
        for (int i = 0; i < hidden_dim; i++) {
            float val = s->hb[i];
            // silu(x)=x*σ(x), where σ(x) is the logistic sigmoid
            val *= (1.0f / (1.0f + expf(-val)));
            // elementwise multiply with w3(x)
            val *= s->hb2[i];
            s->hb[i] = val;
        }

        // final matmul to get the output of the ffn
        matmul(s->xb, s->hb, w->w2 + l*dim*hidden_dim, hidden_dim, dim);

        // residual connection
        for (int i = 0; i < dim; i++) {
            x[i] += s->xb[i];
        }
    }

    // final rmsnorm
    rmsnorm(x, x, w->rms_final_weight, dim);

    // classifier into logits
    matmul(s->logits, x, w->wcls, p->dim, p->vocab_size);
    return s->logits;
}

// ----------------------------------------------------------------------------
// Tokenization (simplified for demo - real tokenizer would use BPE)

/**
 * Simple tokenizer (character-level for demo)
 * TODO: Replace with proper BPE tokenizer
 */
static int tinystories_tokenize(const char* text, int* tokens, int max_tokens) {
    int n = 0;
    for (const char* p = text; *p && n < max_tokens; p++) {
        if (*p >= 'a' && *p <= 'z') {
            tokens[n++] = (*p - 'a' + 1);
        } else if (*p >= 'A' && *p <= 'Z') {
            tokens[n++] = (*p - 'A' + 1);
        } else if (*p == ' ') {
            tokens[n++] = 0;
        }
    }
    return n;
}

/**
 * Decode a token using the loaded vocabulary (BPE)
 * Fallback to simple ASCII if vocabulary not loaded
 */
static const char* tinystories_decode_token(int token) {
    // If vocabulary is loaded, use it
    if (g_vocab.vocab != NULL && token >= 0 && token < g_vocab.vocab_size) {
        const char* piece = g_vocab.vocab[token];

        // Handle raw byte tokens like '<0x01>'
        // Parse and return the actual byte
        unsigned char byte_val;
        if (piece[0] == '<' && piece[1] == '0' && piece[2] == 'x') {
            // Try to parse hex value (support both uppercase and lowercase)
            int hex_val = 0;
            if (piece[3] >= '0' && piece[3] <= '9') {
                hex_val = (piece[3] - '0') << 4;
            } else if (piece[3] >= 'A' && piece[3] <= 'F') {
                hex_val = (piece[3] - 'A' + 10) << 4;
            } else if (piece[3] >= 'a' && piece[3] <= 'f') {
                hex_val = (piece[3] - 'a' + 10) << 4;
            }

            if (piece[4] >= '0' && piece[4] <= '9') {
                hex_val |= (piece[4] - '0');
            } else if (piece[4] >= 'A' && piece[4] <= 'F') {
                hex_val |= (piece[4] - 'A' + 10);
            } else if (piece[4] >= 'a' && piece[4] <= 'f') {
                hex_val |= (piece[4] - 'a' + 10);
            }

            if (piece[5] == '>') {
                // Valid byte token - return from byte_pieces buffer
                byte_val = (unsigned char)hex_val;
                return &g_byte_pieces[byte_val * 2];
            }
        }

        return piece;
    }

    // Fallback: simple ASCII mapping for tokens 0-26 (space + a-z)
    if (token == 0) {
        return " ";
    } else if (token >= 1 && token <= 26) {
        g_token_fallback[0] = 'a' + (token - 1);
        g_token_fallback[1] = '\0';
        return g_token_fallback;
    }

    // For other tokens, show token ID
    // Simple itoa implementation
    char* p = g_token_fallback;
    *p++ = '[';

    int num = token;
    char digits[10];
    int i = 0;

    if (num == 0) {
        digits[i++] = '0';
    } else {
        while (num > 0 && i < 10) {
            digits[i++] = '0' + (num % 10);
            num /= 10;
        }
    }

    // Reverse digits
    while (i > 0) {
        *p++ = digits[--i];
    }

    *p++ = ']';
    *p = '\0';

    return g_token_fallback;
}

/**
 * Sampling helper functions from llama.c
 */

// RNG functions (xorshift)
static unsigned int random_u32(unsigned long long *state) {
    *state ^= *state >> 12;
    *state ^= *state << 25;
    *state ^= *state >> 27;
    return (*state * 0x2545F4914F6CDD1Dull) >> 32;
}

static float random_f32(unsigned long long *state) {
    return (random_u32(state) >> 8) / 16777216.0f;
}

// Sample from probability distribution
static int sample_mult(float* probabilities, int n, float coin) {
    float cdf = 0.0f;
    for (int i = 0; i < n; i++) {
        cdf += probabilities[i];
        if (coin < cdf) {
            return i;
        }
    }
    return n - 1; // in case of rounding errors
}

/**
 * Run REAL inference using the transformer
 * NO MORE HARDCODED RESPONSES - this uses actual model weights
 */
int tinystories_infer(const char* prompt, char* output, int max_output_len) {
    if (!g_model_loaded) {
        console_printf("ERROR: Model not loaded\n");
        return -1;
    }

    console_printf("\n");
    console_printf("=== REAL AI INFERENCE (llama.c engine) ===\n");
    console_printf("Prompt: \"%s\"\n", prompt);

    // CRITICAL: Reset KV caches before each new generation
    // Without this, the model reuses cached attention from previous generations
    // and produces identical output for all prompts
    int kv_dim = (g_config.dim * g_config.n_kv_heads) / g_config.n_heads;
    size_t cache_size = g_config.n_layers * g_config.seq_len * kv_dim * sizeof(float);
    memset(g_state.key_cache, 0, cache_size);
    memset(g_state.value_cache, 0, cache_size);

    // Tokenize input (using simple tokenizer for now)
    int prompt_tokens[256];
    int n_prompt_tokens = tinystories_tokenize(prompt, prompt_tokens, 256);
    console_printf("Tokenized %d tokens from prompt\n", n_prompt_tokens);

    // Start autoregressive generation
    int max_gen_len = max_output_len < 50 ? max_output_len : 50; // generate up to 50 tokens
    int output_len = 0;

    // Initialize RNG for sampling (using simple seed based on prompt)
    static unsigned long long rng_state = 1234567890ULL;
    float temperature = 0.9f;  // temperature for sampling (0.9 = creative but coherent)

    console_printf("Generating text");

    // Start timing for performance metrics (TSC)
    uint64_t start_time = get_timestamp();

    // Process prompt tokens first (prefill)
    int pos = 0;
    float* logits = NULL;  // Will hold logits from last forward pass
    for (int i = 0; i < n_prompt_tokens; i++) {
        logits = forward(prompt_tokens[i], pos);
        pos++;
    }

    // Generate new tokens (decode)
    // CRITICAL: Use "sample → decode → forward" pattern from llama.c
    // NOT "forward → sample → decode" which skips the first token!
    for (int i = 0; i < max_gen_len && output_len < max_output_len - 1; i++) {
        // Sample next token using temperature-based sampling (from llama.c)
        int next = 0;
        if (logits != NULL) {
            // Apply temperature to logits
            for (int j = 0; j < g_config.vocab_size; j++) {
                logits[j] /= temperature;
            }
            // Apply softmax to convert to probabilities
            softmax(logits, g_config.vocab_size);
            // Sample from probability distribution
            float coin = random_f32(&rng_state);
            next = sample_mult(logits, g_config.vocab_size, coin);
        }

        // Decode token using BPE vocabulary
        const char* token_str = tinystories_decode_token(next);

        // Append token string to output
        int token_len = 0;
        for (const char* p = token_str; *p && output_len < max_output_len - 1; p++) {
            // Handle special BPE tokens that start with Ġ (space marker)
            if (*p == (char)0xC4 && *(p+1) == (char)0xA0) {
                // This is the BPE space token "Ġ" (U+0120 encoded as UTF-8)
                output[output_len++] = ' ';
                console_printf(" ");
                p++; // skip second byte
            } else if (*p == '\n' || (*p >= 32 && *p <= 126)) {
                // Printable ASCII or newline
                output[output_len++] = *p;
                console_printf("%c", *p);
            }
            token_len++;
            if (token_len > 100) break; // safety limit
        }

        // Get logits for NEXT token (ready for next iteration)
        // CRITICAL: Pass current pos, THEN increment for next iteration
        logits = forward(next, pos);
        pos++;

        // Stop if we hit end token or some other condition
        // (For now, just generate the requested number of tokens)
    }

    output[output_len] = '\0';
    console_printf("\n");

    // Calculate performance metrics using TSC
    uint64_t end_time = get_timestamp();
    uint64_t elapsed_cycles = end_time - start_time;

    // Estimate elapsed time (assuming ~2 GHz CPU for QEMU/real hardware)
    // For more accuracy on real hardware, this should be calibrated
    uint64_t cpu_freq_mhz = 2000;  // Assume 2 GHz = 2000 MHz
    uint64_t elapsed_ms = elapsed_cycles / (cpu_freq_mhz * 1000);

    // Calculate tokens/second and ms/token
    float tokens_per_second = 0.0f;
    float ms_per_token = 0.0f;
    if (elapsed_ms > 0) {
        tokens_per_second = (float)(max_gen_len * 1000) / (float)elapsed_ms;
        ms_per_token = (float)elapsed_ms / (float)max_gen_len;
    }

    // Convert floats to integers for display (console_printf doesn't support %f)
    int tps_whole = (int)tokens_per_second;
    int tps_frac = (int)((tokens_per_second - tps_whole) * 100);
    int mpt_whole = (int)ms_per_token;
    int mpt_frac = (int)((ms_per_token - mpt_whole) * 10);

    // Display performance metrics
    console_printf("\n");  // Add blank line for readability
    console_printf("Generated %d tokens in %d ms\n", max_gen_len, (int)elapsed_ms);
    console_printf("Performance: ");
    console_printf("%d", tps_whole);
    console_printf(".");
    if (tps_frac < 10) console_printf("0");  // Manual zero-padding for 2 digits
    console_printf("%d", tps_frac);
    console_printf(" tokens/sec, ");
    console_printf("%d", mpt_whole);
    console_printf(".");
    console_printf("%d", mpt_frac);
    console_printf(" ms/token\n");
    console_printf("\n=== INFERENCE COMPLETE ===\n\n");

    return output_len;
}

/**
 * Check if TinyStories model is loaded
 */
bool tinystories_is_loaded(void) {
    return g_model_loaded;
}

/**
 * Interactive TinyStories - load model and wait for prompts
 */
void tinystories_interactive_init(void) {
    console_printf("\n");
    console_printf("═══════════════════════════════════════════════════════════\n");
    console_printf("  TinyStories-15M Interactive AI\n");
    console_printf("  EMBODIOS Kernel - llama.c engine\n");
    console_printf("═══════════════════════════════════════════════════════════\n");
    console_printf("\n");

    // Try to load model (will succeed even without weights)
    tinystories_load_model();

    if (g_model_loaded) {
        console_printf("AI Model Ready!\n");
        console_printf("Model: %d layers, %d dim, %d vocab\n",
                       g_config.n_layers, g_config.dim, g_config.vocab_size);
        console_printf("\nType 'ai <prompt>' to generate text\n");
        console_printf("Example: ai Once upon a time\n");
    } else {
        console_printf("AI Model: Not available (no weights embedded)\n");
        console_printf("To enable AI: embed tinystories-15m.bin in kernel build\n");
    }

    console_printf("═══════════════════════════════════════════════════════════\n");
    console_printf("\n");
}

/**
 * Test TinyStories model with REAL inference
 */
void tinystories_test(void) {
    console_printf("\n");
    console_printf("═══════════════════════════════════════════════════════════\n");
    console_printf("  TinyStories-15M REAL INFERENCE TEST\n");
    console_printf("  EMBODIOS Kernel - llama.c engine\n");
    console_printf("═══════════════════════════════════════════════════════════\n");
    console_printf("\n");

    // Load model
    if (tinystories_load_model() != 0) {
        console_printf("Failed to load model\n");
        return;
    }

    // Run test inference with REAL transformer
    char output[256];
    const char* test_prompt = "Once upon a time";

    console_printf("Running REAL transformer inference (no hardcoded responses)\n\n");
    tinystories_infer(test_prompt, output, sizeof(output));

    console_printf("\n");
    console_printf("TinyStories test complete!\n");
    console_printf("Model: %d layers, %d params\n", g_config.n_layers, g_config.dim);
    console_printf("Inference: REAL (ported from llama.c)\n");
    console_printf("\n");
    console_printf("═══════════════════════════════════════════════════════════\n");
    console_printf("\n");
}

