/**
 * GGUF Parser with Enhanced Metadata Extraction
 *
 * Provides complete GGUF format parsing with:
 * - Support for GGUF versions 1, 2, 3
 * - Model architecture extraction
 * - Vocabulary parsing
 * - Metadata validation
 */

#ifndef EMBODIOS_GGUF_PARSER_H
#define EMBODIOS_GGUF_PARSER_H

#include <embodios/types.h>

/* ============================================================================
 * Model Architecture Structure
 * ============================================================================ */

struct gguf_model_arch {
    /* General info */
    char general_architecture[64];
    char general_name[128];
    char general_author[128];
    char general_license[128];
    uint32_t general_file_type;
    uint32_t general_quantization_version;
    uint32_t general_alignment;

    /* Model architecture */
    uint32_t context_length;
    uint32_t embedding_length;
    uint32_t block_count;
    uint32_t feed_forward_length;

    /* Attention parameters */
    uint32_t attention_head_count;
    uint32_t attention_head_count_kv;
    float attention_layer_norm_rms_epsilon;
    uint32_t attention_key_length;
    uint32_t attention_value_length;
    float attention_max_alibi_bias;
    float attention_clamp_kqv;

    /* RoPE parameters */
    uint32_t rope_dimension_count;
    float rope_freq_base;
    float rope_scale_linear;
    uint32_t rope_scaling_type;
    float rope_scaling_factor;
    float rope_scaling_orig_ctx_len;
    uint8_t rope_scaling_finetuned;

    /* Vocabulary */
    uint32_t vocab_size;
    uint32_t bos_token_id;
    uint32_t eos_token_id;
    uint32_t pad_token_id;
    uint32_t sep_token_id;
    uint32_t unk_token_id;
    char tokenizer_model[64];
};

/* ============================================================================
 * Parser API
 * ============================================================================ */

/**
 * Enable or disable debug logging
 * @param enabled Non-zero to enable debug output
 */
void gguf_parser_set_debug(int enabled);

/**
 * Parse GGUF file from memory buffer
 * @param data Pointer to GGUF file data
 * @param size Size of data in bytes
 * @return 0 on success, -1 on error
 */
int gguf_parser_load(const void* data, size_t size);

/**
 * Get parsed model architecture
 * @return Pointer to architecture struct, or NULL if not loaded
 */
const struct gguf_model_arch* gguf_parser_get_arch(void);

/**
 * Get GGUF file version
 * @return Version number (1, 2, or 3)
 */
uint32_t gguf_parser_get_version(void);

/**
 * Get vocabulary token text by index
 * @param index Token index (0-based)
 * @return Token string, or NULL if not available
 */
const char* gguf_parser_get_token(uint32_t index);

/**
 * Get vocabulary size
 * @return Number of tokens in vocabulary
 */
uint32_t gguf_parser_get_vocab_size(void);

/**
 * Get token score (for BPE/SentencePiece)
 * @param index Token index (0-based)
 * @return Token score/priority
 */
float gguf_parser_get_token_score(uint32_t index);

/**
 * Get pointer to tensor data region
 * @return Pointer to start of tensor data
 */
const void* gguf_parser_get_tensor_data(void);

/**
 * Get data alignment used in file
 * @return Alignment in bytes
 */
size_t gguf_parser_get_alignment(void);

/**
 * Free parser resources
 */
void gguf_parser_free(void);

/**
 * Print model summary to console
 */
void gguf_parser_print_summary(void);

/* ============================================================================
 * Convenience Macros
 * ============================================================================ */

/* Get embedding dimension (n_embd) */
#define GGUF_GET_N_EMBD() \
    (gguf_parser_get_arch() ? gguf_parser_get_arch()->embedding_length : 0)

/* Get number of layers (n_layer) */
#define GGUF_GET_N_LAYER() \
    (gguf_parser_get_arch() ? gguf_parser_get_arch()->block_count : 0)

/* Get number of attention heads (n_head) */
#define GGUF_GET_N_HEAD() \
    (gguf_parser_get_arch() ? gguf_parser_get_arch()->attention_head_count : 0)

/* Get number of KV heads (n_head_kv) */
#define GGUF_GET_N_HEAD_KV() \
    (gguf_parser_get_arch() ? gguf_parser_get_arch()->attention_head_count_kv : 0)

/* Get feed-forward dimension (n_ff) */
#define GGUF_GET_N_FF() \
    (gguf_parser_get_arch() ? gguf_parser_get_arch()->feed_forward_length : 0)

/* Get vocabulary size (n_vocab) */
#define GGUF_GET_N_VOCAB() \
    (gguf_parser_get_arch() ? gguf_parser_get_arch()->vocab_size : 0)

/* Get context length */
#define GGUF_GET_CTX_LEN() \
    (gguf_parser_get_arch() ? gguf_parser_get_arch()->context_length : 0)

#endif /* EMBODIOS_GGUF_PARSER_H */
