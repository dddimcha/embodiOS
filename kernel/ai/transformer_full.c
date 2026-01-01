/* EMBODIOS Transformer Implementation
 * 
 * Implements transformer architecture for language models
 * Supports GPT-style autoregressive models
 */

#include "embodios/types.h"
#include "embodios/kernel.h"
#include "embodios/console.h"
#include "embodios/mm.h"
#include "embodios/tvm.h"
#include "embodios/model.h"
#include "embodios/gguf.h"
#include "embodios/embeddings.h"
#include "embodios/kv_cache_enhanced.h"

/* Math functions */
float sqrtf(float x);
float expf(float x);

/* String functions */
void* memset(void* s, int c, size_t n);

/* External tensor ops */
void tensor_gemm(float* A, float* B, float* C, int M, int N, int K, float alpha, float beta);
void tensor_softmax_forward(TVMTensor* input, TVMTensor* output);
void tensor_add(TVMTensor* a, TVMTensor* b, TVMTensor* output);

/* Transformer configuration */
struct transformer_config {
    int vocab_size;
    int n_layers;
    int n_heads;
    int n_embd;
    int max_seq_len;
    int hidden_dim;
};

/* Transformer weights */
struct transformer_weights {
    /* Token embeddings */
    float* token_embedding;     /* [vocab_size, n_embd] */
    float* position_embedding;  /* [max_seq_len, n_embd] */
    
    /* Attention weights per layer */
    float** q_weight;          /* [n_layers][n_embd, n_embd] */
    float** k_weight;          /* [n_layers][n_embd, n_embd] */
    float** v_weight;          /* [n_layers][n_embd, n_embd] */
    float** o_weight;          /* [n_layers][n_embd, n_embd] */
    
    /* FFN weights per layer */
    float** ffn_weight1;       /* [n_layers][n_embd, hidden_dim] */
    float** ffn_weight2;       /* [n_layers][hidden_dim, n_embd] */
    
    /* Layer norm */
    float** ln1_weight;        /* [n_layers][n_embd] */
    float** ln1_bias;          /* [n_layers][n_embd] */
    float** ln2_weight;        /* [n_layers][n_embd] */
    float** ln2_bias;          /* [n_layers][n_embd] */
    
    /* Final layer norm */
    float* ln_f_weight;        /* [n_embd] */
    float* ln_f_bias;          /* [n_embd] */
    
    /* Output projection */
    float* lm_head;            /* [n_embd, vocab_size] */
};

/* Transformer state */
struct transformer_state {
    struct transformer_config config;
    struct transformer_weights weights;
    
    /* Activation buffers */
    float* x;                  /* Current activations [seq_len, n_embd] */
    float* xb;                 /* Residual branch [seq_len, n_embd] */
    float* xb2;                /* Second residual [seq_len, n_embd] */
    float* q;                  /* Query [seq_len, n_embd] */
    float* k;                  /* Key [seq_len, n_embd] */
    float* v;                  /* Value [seq_len, n_embd] */
    float* att;                /* Attention scores [n_heads, seq_len, seq_len] */
    float* logits;             /* Output logits [vocab_size] */
    
    /* KV cache for autoregressive generation */
    float** key_cache;         /* [n_layers][max_seq_len, n_embd] */
    float** value_cache;       /* [n_layers][max_seq_len, n_embd] */
    int cache_pos;             /* Current position in cache */
};

static struct transformer_state* g_transformer = NULL;

/* Layer normalization */
static void layer_norm(float* out, float* x, float* weight, float* bias, int size)
{
    /* Calculate mean */
    float mean = 0.0f;
    for (int i = 0; i < size; i++) {
        mean += x[i];
    }
    mean /= size;
    
    /* Calculate variance */
    float var = 0.0f;
    for (int i = 0; i < size; i++) {
        float diff = x[i] - mean;
        var += diff * diff;
    }
    var /= size;
    
    /* Normalize */
    float inv_std = 1.0f / sqrtf(var + 1e-5f);
    for (int i = 0; i < size; i++) {
        out[i] = (x[i] - mean) * inv_std;
        if (weight) out[i] *= weight[i];
        if (bias) out[i] += bias[i];
    }
}

/* GELU activation */
static void gelu(float* x, int size)
{
    for (int i = 0; i < size; i++) {
        float val = x[i];
        /* Approximation: 0.5 * x * (1 + tanh(sqrt(2/pi) * (x + 0.044715 * x^3))) */
        float x3 = val * val * val;
        float tanh_arg = 0.7978845608f * (val + 0.044715f * x3);
        
        /* Fast tanh approximation */
        float tanh_val = tanh_arg;
        if (tanh_arg > 3.0f) tanh_val = 1.0f;
        else if (tanh_arg < -3.0f) tanh_val = -1.0f;
        else {
            float x2 = tanh_arg * tanh_arg;
            tanh_val = tanh_arg * (27.0f + x2) / (27.0f + 9.0f * x2);
        }
        
        x[i] = 0.5f * val * (1.0f + tanh_val);
    }
}

/* Multi-head attention */
static void multi_head_attention(
    struct transformer_state* s,
    int layer,
    int seq_len)
{
    struct transformer_config* c = &s->config;
    struct transformer_weights* w = &s->weights;

    int head_dim = c->n_embd / c->n_heads;

    /* Compute Q, K, V projections */
    tensor_gemm(s->x, w->q_weight[layer], s->q, seq_len, c->n_embd, c->n_embd, 1.0f, 0.0f);
    tensor_gemm(s->x, w->k_weight[layer], s->k, seq_len, c->n_embd, c->n_embd, 1.0f, 0.0f);
    tensor_gemm(s->x, w->v_weight[layer], s->v, seq_len, c->n_embd, c->n_embd, 1.0f, 0.0f);

    /* Try to use enhanced KV cache if available */
    kv_cache_t* enh_cache = kv_cache_get_global();
    bool use_enhanced = (enh_cache && kv_cache_is_valid(enh_cache) &&
                         (uint32_t)layer < enh_cache->config.n_layers);

    /* Store in KV cache */
    if (use_enhanced) {
        /* Use enhanced KV cache - batch store for all positions */
        kv_cache_store_batch_f32(enh_cache, layer, s->cache_pos, seq_len, s->k, s->v);
    } else if (s->key_cache && s->value_cache) {
        /* Fallback to inline cache */
        for (int pos = 0; pos < seq_len; pos++) {
            int cache_idx = s->cache_pos + pos;
            if (cache_idx < c->max_seq_len) {
                for (int i = 0; i < c->n_embd; i++) {
                    s->key_cache[layer][cache_idx * c->n_embd + i] = s->k[pos * c->n_embd + i];
                    s->value_cache[layer][cache_idx * c->n_embd + i] = s->v[pos * c->n_embd + i];
                }
            }
        }
    }

    /* Get pointers to cached K/V for attention computation */
    const float* cached_keys = NULL;
    const float* cached_values = NULL;

    if (use_enhanced) {
        cached_keys = kv_cache_get_key_ptr_f32(enh_cache, layer);
        cached_values = kv_cache_get_value_ptr_f32(enh_cache, layer);
    } else if (s->key_cache && s->value_cache) {
        cached_keys = s->key_cache[layer];
        cached_values = s->value_cache[layer];
    }

    /* Need cached KV for attention */
    if (!cached_keys || !cached_values) {
        return;
    }

    /* Compute attention for each head */
    for (int h = 0; h < c->n_heads; h++) {
        /* Compute attention scores: Q @ K^T / sqrt(head_dim) */
        float scale = 1.0f / sqrtf((float)head_dim);

        for (int i = 0; i < seq_len; i++) {
            for (int j = 0; j <= s->cache_pos + i; j++) {
                float score = 0.0f;

                /* Dot product of Q[i] and K[j] for this head */
                for (int k = 0; k < head_dim; k++) {
                    int q_idx = i * c->n_embd + h * head_dim + k;
                    int k_idx = j * c->n_embd + h * head_dim + k;
                    score += s->q[q_idx] * cached_keys[k_idx];
                }

                s->att[h * seq_len * c->max_seq_len + i * c->max_seq_len + j] = score * scale;
            }
        }

        /* Apply causal mask and softmax */
        for (int i = 0; i < seq_len; i++) {
            float* att_row = &s->att[h * seq_len * c->max_seq_len + i * c->max_seq_len];

            /* Apply causal mask */
            for (int j = s->cache_pos + i + 1; j < c->max_seq_len; j++) {
                att_row[j] = -1e9f;
            }

            /* Softmax */
            float max_val = att_row[0];
            for (int j = 1; j <= s->cache_pos + i; j++) {
                if (att_row[j] > max_val) max_val = att_row[j];
            }

            float sum = 0.0f;
            for (int j = 0; j <= s->cache_pos + i; j++) {
                att_row[j] = expf(att_row[j] - max_val);
                sum += att_row[j];
            }

            for (int j = 0; j <= s->cache_pos + i; j++) {
                att_row[j] /= sum;
            }
        }

        /* Apply attention to values */
        for (int i = 0; i < seq_len; i++) {
            for (int k = 0; k < head_dim; k++) {
                float sum = 0.0f;
                float* att_row = &s->att[h * seq_len * c->max_seq_len + i * c->max_seq_len];

                for (int j = 0; j <= s->cache_pos + i; j++) {
                    int v_idx = j * c->n_embd + h * head_dim + k;
                    sum += att_row[j] * cached_values[v_idx];
                }

                s->xb[i * c->n_embd + h * head_dim + k] = sum;
            }
        }
    }

    /* Output projection */
    tensor_gemm(s->xb, w->o_weight[layer], s->x, seq_len, c->n_embd, c->n_embd, 1.0f, 1.0f);
}

/* Feed-forward network */
static void ffn(struct transformer_state* s, int layer, int seq_len)
{
    struct transformer_config* c = &s->config;
    struct transformer_weights* w = &s->weights;
    
    /* First linear layer */
    tensor_gemm(s->x, w->ffn_weight1[layer], s->xb, seq_len, c->hidden_dim, c->n_embd, 1.0f, 0.0f);
    
    /* GELU activation */
    gelu(s->xb, seq_len * c->hidden_dim);
    
    /* Second linear layer */
    tensor_gemm(s->xb, w->ffn_weight2[layer], s->x, seq_len, c->n_embd, c->hidden_dim, 1.0f, 1.0f);
}

/* Forward pass through transformer */
void transformer_forward(int* tokens, int n_tokens, float* logits)
{
    struct transformer_state* s = g_transformer;
    struct transformer_config* c = &s->config;
    struct transformer_weights* w = &s->weights;
    
    console_printf("Transformer: Forward pass with %d tokens\n", n_tokens);
    
    /* Check if using TinyLlama */
    if (c->vocab_size == 32000) {  /* TinyLlama actual config */
        extern void tinyllama_forward_tvm(int* tokens, int n_tokens, float* logits);
        tinyllama_forward_tvm(tokens, n_tokens, logits);
        return;
    } else if (c->vocab_size == 1000) {  /* TinyLlama test config */
        extern void tinyllama_forward(int* tokens, int n_tokens, float* logits);
        tinyllama_forward(tokens, n_tokens, logits);
        return;
    }
    
    /* For testing without weights, just generate random logits */
    if (!w->token_embedding) {
        console_printf("Transformer: Using random logits (no weights loaded)\n");
        for (int i = 0; i < c->vocab_size; i++) {
            /* Simple pseudo-random based on token */
            logits[i] = ((float)(tokens[0] * 31 + i * 17) / 1000.0f) - 0.5f;
        }
        return;
    }
    
    /* Token + position embeddings - use pre-computed cache if available */
    embedding_cache_t* emb_cache = embedding_get_global();

    if (emb_cache && embedding_validate_cache(emb_cache)) {
        /* Fast path: use pre-computed embedding cache */
        for (int i = 0; i < n_tokens; i++) {
            int pos = s->cache_pos + i;
            embedding_lookup(emb_cache, tokens[i], pos,
                            &s->x[i * c->n_embd]);
        }
    } else {
        /* Fallback: compute embeddings directly from weights */
        for (int i = 0; i < n_tokens; i++) {
            int token = tokens[i];
            int pos = s->cache_pos + i;

            /* Bounds validation to prevent buffer overflow */
            if (token < 0 || token >= c->vocab_size) {
                console_printf("Transformer: Invalid token %d (vocab=%d)\n",
                               token, c->vocab_size);
                token = 0;  /* Use padding token */
            }
            if (pos < 0 || pos >= c->max_seq_len) {
                console_printf("Transformer: Position %d exceeds max %d\n",
                               pos, c->max_seq_len);
                pos = c->max_seq_len - 1;  /* Clamp to max */
            }

            for (int j = 0; j < c->n_embd; j++) {
                s->x[i * c->n_embd + j] =
                    w->token_embedding[token * c->n_embd + j] +
                    w->position_embedding[pos * c->n_embd + j];
            }
        }
    }
    
    /* Forward through layers */
    for (int l = 0; l < c->n_layers; l++) {
        /* Save residual */
        for (int i = 0; i < n_tokens * c->n_embd; i++) {
            s->xb[i] = s->x[i];
        }
        
        /* Layer norm 1 */
        for (int i = 0; i < n_tokens; i++) {
            layer_norm(&s->x[i * c->n_embd], &s->x[i * c->n_embd], 
                      w->ln1_weight[l], w->ln1_bias[l], c->n_embd);
        }
        
        /* Multi-head attention */
        multi_head_attention(s, l, n_tokens);
        
        /* Residual connection */
        for (int i = 0; i < n_tokens * c->n_embd; i++) {
            s->x[i] = s->xb[i] + s->x[i];
            s->xb[i] = s->x[i];  /* Save for next residual */
        }
        
        /* Layer norm 2 */
        for (int i = 0; i < n_tokens; i++) {
            layer_norm(&s->x[i * c->n_embd], &s->x[i * c->n_embd], 
                      w->ln2_weight[l], w->ln2_bias[l], c->n_embd);
        }
        
        /* FFN */
        ffn(s, l, n_tokens);
        
        /* Residual connection */
        for (int i = 0; i < n_tokens * c->n_embd; i++) {
            s->x[i] = s->xb[i] + s->x[i];
        }
    }
    
    /* Final layer norm */
    int last_token = n_tokens - 1;
    layer_norm(&s->x[last_token * c->n_embd], &s->x[last_token * c->n_embd], 
              w->ln_f_weight, w->ln_f_bias, c->n_embd);
    
    /* LM head to get logits */
    tensor_gemm(&s->x[last_token * c->n_embd], w->lm_head, logits, 
                1, c->vocab_size, c->n_embd, 1.0f, 0.0f);
    
    /* Update cache position */
    s->cache_pos += n_tokens;
}

/* String comparison helper */
static int contains_str(const char* haystack, const char* needle)
{
    while (*haystack) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n) return 1;
        haystack++;
    }
    return 0;
}

/* Initialize transformer from model data */
int transformer_init(struct embodios_model* model)
{
    (void)model; /* Unused parameter */

    console_printf("Transformer: Starting minimal init\n");

    /* Initialize with minimal demo configuration */
    static struct transformer_state demo_state;
    g_transformer = &demo_state;
    g_transformer->config.vocab_size = 1000;
    g_transformer->config.n_layers = 2;
    g_transformer->config.n_heads = 8;
    g_transformer->config.n_embd = 256;
    g_transformer->config.max_seq_len = 128;

    console_printf("Transformer: Demo mode initialized\n");
    return 0;
}

/* Get next token using sampling */
int transformer_sample(float* logits, float temperature)
{
    struct transformer_config* c = &g_transformer->config;
    
    /* Apply temperature */
    if (temperature != 1.0f) {
        for (int i = 0; i < c->vocab_size; i++) {
            logits[i] /= temperature;
        }
    }
    
    /* Softmax */
    float max_val = logits[0];
    for (int i = 1; i < c->vocab_size; i++) {
        if (logits[i] > max_val) max_val = logits[i];
    }
    
    float sum = 0.0f;
    for (int i = 0; i < c->vocab_size; i++) {
        logits[i] = expf(logits[i] - max_val);
        sum += logits[i];
    }
    
    /* For now, greedy sampling (argmax) */
    int best_idx = 0;
    float best_prob = logits[0] / sum;
    for (int i = 1; i < c->vocab_size; i++) {
        float prob = logits[i] / sum;
        if (prob > best_prob) {
            best_prob = prob;
            best_idx = i;
        }
    }
    
    return best_idx;
}

/* Reset KV cache for new generation */
void transformer_reset_cache(void)
{
    if (g_transformer) {
        g_transformer->cache_pos = 0;
    }

    /* Also reset enhanced KV cache if available */
    kv_cache_t* enh_cache = kv_cache_get_global();
    if (enh_cache && kv_cache_is_valid(enh_cache)) {
        kv_cache_reset(enh_cache);
    }
}

/* Free transformer resources */
void transformer_free(void)
{
    console_printf("Transformer: Cleanup\n");
    /* TODO: Free allocated memory */
}