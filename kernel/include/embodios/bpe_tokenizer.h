/**
 * EMBODIOS BPE Tokenizer
 *
 * Proper Byte-Pair Encoding tokenizer that loads vocabulary from GGUF files.
 * Compatible with LLaMA/SentencePiece tokenization.
 */

#ifndef EMBODIOS_BPE_TOKENIZER_H
#define EMBODIOS_BPE_TOKENIZER_H

#include <embodios/types.h>

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * bpe_tokenizer_init - Initialize tokenizer from GGUF vocabulary
 *
 * Must be called after gguf_parser_load() has loaded a model.
 * Loads vocabulary from the GGUF parser's extracted data.
 *
 * Returns: 0 on success, -1 on error
 */
int bpe_tokenizer_init(void);

/**
 * bpe_tokenizer_cleanup - Free tokenizer resources
 */
void bpe_tokenizer_cleanup(void);

/**
 * bpe_tokenizer_is_initialized - Check if tokenizer is ready
 */
bool bpe_tokenizer_is_initialized(void);

/* ============================================================================
 * Encoding / Decoding
 * ============================================================================ */

/**
 * bpe_tokenizer_encode - Encode text to tokens
 *
 * @text: Input text (UTF-8)
 * @tokens: Output token array
 * @max_tokens: Maximum tokens to output
 * @add_bos: Add BOS token at start
 * @add_eos: Add EOS token at end
 *
 * Returns: Number of tokens produced
 */
int bpe_tokenizer_encode(const char* text, int* tokens, int max_tokens,
                         bool add_bos, bool add_eos);

/**
 * bpe_tokenizer_decode - Decode tokens to text
 *
 * @tokens: Input token array
 * @n_tokens: Number of tokens
 * @text: Output text buffer
 * @max_len: Maximum output length
 *
 * Returns: Length of output text
 */
int bpe_tokenizer_decode(const int* tokens, int n_tokens, char* text, int max_len);

/**
 * bpe_tokenizer_decode_token - Decode single token to text
 *
 * @token_id: Token ID to decode
 *
 * Returns: Pointer to token text (static buffer, do not free)
 */
const char* bpe_tokenizer_decode_token(int token_id);

/* ============================================================================
 * Vocabulary Info
 * ============================================================================ */

/**
 * bpe_tokenizer_get_vocab_size - Get vocabulary size
 */
uint32_t bpe_tokenizer_get_vocab_size(void);

/**
 * bpe_tokenizer_get_bos - Get BOS (beginning of sequence) token ID
 */
uint32_t bpe_tokenizer_get_bos(void);

/**
 * bpe_tokenizer_get_eos - Get EOS (end of sequence) token ID
 */
uint32_t bpe_tokenizer_get_eos(void);

/* ============================================================================
 * Testing
 * ============================================================================ */

/**
 * bpe_tokenizer_test - Run tokenizer test with sample texts
 */
void bpe_tokenizer_test(void);

#endif /* EMBODIOS_BPE_TOKENIZER_H */
