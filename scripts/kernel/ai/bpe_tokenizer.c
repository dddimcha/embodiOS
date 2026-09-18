/**
 * EMBODIOS BPE Tokenizer
 *
 * Proper Byte-Pair Encoding tokenizer that loads vocabulary from GGUF files.
 * Compatible with LLaMA/SentencePiece tokenization.
 *
 * Algorithm:
 * 1. Convert text to UTF-8 bytes
 * 2. Look up longest matching tokens in vocabulary
 * 3. Use scores for tie-breaking (greedy longest match)
 */

#include <embodios/console.h>
#include <embodios/gguf_parser.h>
#include <embodios/kernel.h>
#include <embodios/mm.h>
#include <embodios/test.h>
#include <embodios/types.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define BPE_MAX_TOKEN_LEN 64    /* Maximum token length in bytes */
#define BPE_HASH_SIZE     65536 /* Hash table size for vocab lookup */
#define BPE_BYTE_FALLBACK 256   /* Start of byte fallback tokens */

/* Special token IDs (LLaMA defaults) */
#define BPE_TOKEN_UNK 0
#define BPE_TOKEN_BOS 1
#define BPE_TOKEN_EOS 2

/* ============================================================================
 * Data Structures
 * ============================================================================ */

/**
 * Vocabulary entry for hash table lookup
 */
struct bpe_vocab_entry {
    char *text;                   /* Token text (UTF-8) */
    uint32_t token_id;            /* Token ID */
    float score;                  /* Token score (for merge priority) */
    uint8_t length;               /* Text length in bytes */
    struct bpe_vocab_entry *next; /* Hash collision chain */
};

/**
 * Tokenizer types - determines preprocessing style
 */
#define BPE_TYPE_UNKNOWN      0
#define BPE_TYPE_SENTENCEPIECE 1  /* LLaMA/SentencePiece style - uses ▁ for spaces */
#define BPE_TYPE_GPT2         2  /* GPT-2/BPE style - uses Ġ for spaces */

/**
 * BPE tokenizer state
 */
static struct {
    struct bpe_vocab_entry **hash_table; /* Hash table for text->id lookup */
    char **id_to_text;                   /* Array for id->text lookup */
    float *scores;                       /* Token scores */
    uint32_t vocab_size;                 /* Vocabulary size */
    uint32_t bos_token;                  /* BOS token ID */
    uint32_t eos_token;                  /* EOS token ID */
    uint32_t unk_token;                  /* UNK token ID */
    int tokenizer_type;                  /* BPE_TYPE_* - preprocessing style */
    bool initialized;

    /* Memory pools for O(1) allocation */
    struct bpe_vocab_entry *entry_pool;  /* Pre-allocated entry pool */
    uint32_t entry_pool_idx;             /* Next free entry index */
    char *text_pool;                     /* Pre-allocated text pool */
    size_t text_pool_idx;                /* Next free text position */
    size_t text_pool_size;               /* Total text pool size */
} g_bpe = {0};

/* ============================================================================
 * String Utilities
 * ============================================================================ */

extern size_t strlen(const char *s);
extern int strcmp(const char *s1, const char *s2);
extern int strncmp(const char *s1, const char *s2, size_t n);
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *s, int c, size_t n);

/**
 * djb2_hash - Simple string hash function
 */
static uint32_t djb2_hash(const char *str, size_t len)
{
    uint32_t hash = 5381;
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + (uint8_t)str[i];
    }
    return hash;
}

/* ============================================================================
 * Vocabulary Management
 * ============================================================================ */

/**
 * bpe_vocab_insert - Insert token into hash table
 */
static int bpe_vocab_insert(const char *text, uint32_t token_id, float score)
{
    if (!text || !g_bpe.hash_table || !g_bpe.entry_pool || !g_bpe.text_pool)
        return -1;

    size_t len = strlen(text);
    if (len == 0 || len > BPE_MAX_TOKEN_LEN)
        return -1;

    /* Check pool space */
    if (g_bpe.entry_pool_idx >= g_bpe.vocab_size)
        return -1;
    if (g_bpe.text_pool_idx + len + 1 > g_bpe.text_pool_size)
        return -1;

    /* Allocate entry from pool - O(1) */
    struct bpe_vocab_entry *entry = &g_bpe.entry_pool[g_bpe.entry_pool_idx++];

    /* Allocate text from pool - O(1) */
    entry->text = &g_bpe.text_pool[g_bpe.text_pool_idx];
    g_bpe.text_pool_idx += len + 1;

    memcpy(entry->text, text, len);
    entry->text[len] = '\0';
    entry->token_id = token_id;
    entry->score = score;
    entry->length = (uint8_t)len;

    /* Insert into hash table */
    uint32_t hash = djb2_hash(text, len) % BPE_HASH_SIZE;
    entry->next = g_bpe.hash_table[hash];
    g_bpe.hash_table[hash] = entry;

    /* Store reverse mapping */
    if (token_id < g_bpe.vocab_size) {
        g_bpe.id_to_text[token_id] = entry->text;
        g_bpe.scores[token_id] = score;
    }

    return 0;
}

/**
 * bpe_vocab_lookup - Look up token ID by text
 */
static int bpe_vocab_lookup(const char *text, size_t len)
{
    if (!text || !g_bpe.hash_table || len == 0)
        return -1;

    uint32_t hash = djb2_hash(text, len) % BPE_HASH_SIZE;
    struct bpe_vocab_entry *entry = g_bpe.hash_table[hash];

    while (entry) {
        if (entry->length == len && strncmp(entry->text, text, len) == 0) {
            return (int)entry->token_id;
        }
        entry = entry->next;
    }

    return -1; /* Not found */
}

/* ============================================================================
 * BPE Encoding
 * ============================================================================ */

/**
 * bpe_encode_greedy - Greedy longest-match tokenization
 *
 * For each position, find the longest matching token in vocabulary.
 * This is the standard approach for SentencePiece/LLaMA tokenizers.
 */
static int bpe_encode_greedy(const char *text, int *tokens, int max_tokens)
{
    int n_tokens = 0;
    size_t text_len = strlen(text);
    size_t pos = 0;

    while (pos < text_len && n_tokens < max_tokens) {
        int best_token = -1;
        size_t best_len = 0;

        /* Try to match longest token starting at current position */
        for (size_t len = 1; len <= BPE_MAX_TOKEN_LEN && pos + len <= text_len; len++) {
            int token_id = bpe_vocab_lookup(text + pos, len);
            if (token_id >= 0) {
                /* Found a match - keep looking for longer */
                best_token = token_id;
                best_len = len;
            }
        }

        if (best_token >= 0) {
            /* Use the longest matching token */
            tokens[n_tokens++] = best_token;
            pos += best_len;
        } else {
            /* No match - use byte fallback */
            uint8_t byte = (uint8_t)text[pos];

            /* Try to find byte token (format: <0xXX>) */
            char byte_token[8];
            byte_token[0] = '<';
            byte_token[1] = '0';
            byte_token[2] = 'x';
            const char *hex = "0123456789ABCDEF";
            byte_token[3] = hex[byte >> 4];
            byte_token[4] = hex[byte & 0xF];
            byte_token[5] = '>';
            byte_token[6] = '\0';

            int byte_id = bpe_vocab_lookup(byte_token, 6);
            if (byte_id >= 0) {
                tokens[n_tokens++] = byte_id;
            } else {
                /* Ultimate fallback: UNK token */
                tokens[n_tokens++] = g_bpe.unk_token;
            }
            pos++;
        }
    }

    return n_tokens;
}

/**
 * bpe_preprocess_text - Preprocess text based on tokenizer type
 *
 * For SentencePiece (LLaMA): Convert spaces to '▁' (U+2581)
 * For GPT-2/BPE: Pass through as-is (vocab already has Ġ for spaces)
 */
static int bpe_preprocess_text(const char *input, char *output, size_t max_len)
{
    size_t in_pos = 0;
    size_t out_pos = 0;
    size_t in_len = strlen(input);

    /* GPT-2 style: convert spaces to 'Ġ' (U+0120 = 0xC4 0xA0 in UTF-8)
     * The vocab has tokens like "Ġupon" for " upon", so we need to convert
     * ASCII space (0x20) to the Ġ prefix (0xC4 0xA0) before words.
     */
    if (g_bpe.tokenizer_type == BPE_TYPE_GPT2) {
        bool at_word_start = false;  /* First word doesn't get space prefix */

        while (in_pos < in_len && out_pos < max_len - 3) {
            uint8_t c = (uint8_t)input[in_pos];

            if (c == ' ' || c == '\t') {
                /* Space - next non-space char gets Ġ prefix */
                at_word_start = true;
                in_pos++;
            } else if (c == '\n' || c == '\r') {
                /* Newline - copy as-is, next char gets prefix */
                output[out_pos++] = (char)c;
                at_word_start = true;
                in_pos++;
            } else {
                /* Regular character - add Ġ prefix if preceded by space */
                if (at_word_start) {
                    /* Add Ġ prefix (U+0120 = 0xC4 0xA0 in UTF-8) */
                    output[out_pos++] = (char)0xC4;
                    output[out_pos++] = (char)0xA0;
                }
                output[out_pos++] = (char)c;
                at_word_start = false;
                in_pos++;
            }
        }
        output[out_pos] = '\0';
        return (int)out_pos;
    }

    /* SentencePiece style: convert spaces to ▁ marker */
    bool at_word_start = true;

    /* SentencePiece underscore: ▁ (U+2581) = 0xE2 0x96 0x81 in UTF-8 */
    const char sp_space[] = {(char)0xE2, (char)0x96, (char)0x81, 0};

    while (in_pos < in_len && out_pos < max_len - 4) {
        uint8_t c = (uint8_t)input[in_pos];

        if (c == ' ' || c == '\t') {
            /* Space - add SentencePiece marker for next word */
            at_word_start = true;
            in_pos++;
        } else if (c == '\n' || c == '\r') {
            /* Newline - copy as-is */
            output[out_pos++] = (char)c;
            at_word_start = true;
            in_pos++;
        } else {
            /* Regular character */
            if (at_word_start && out_pos > 0) {
                /* Add SentencePiece space marker before word */
                output[out_pos++] = sp_space[0];
                output[out_pos++] = sp_space[1];
                output[out_pos++] = sp_space[2];
            }
            output[out_pos++] = (char)c;
            at_word_start = false;
            in_pos++;
        }
    }

    output[out_pos] = '\0';
    return (int)out_pos;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * bpe_tokenizer_init - Initialize tokenizer from GGUF vocabulary
 *
 * Must be called after gguf_parser_load() has loaded a model.
 *
 * Returns: 0 on success, -1 on error
 */
int bpe_tokenizer_init(void)
{
    /* Check if GGUF vocabulary is loaded */
    uint32_t vocab_size = gguf_parser_get_vocab_size();
    if (vocab_size == 0) {
        console_printf("Error: No vocabulary loaded\n");
        return -1;
    }

    g_bpe.vocab_size = vocab_size;

    /* Allocate hash table using heap_alloc (not kmalloc) */
    g_bpe.hash_table = heap_alloc(BPE_HASH_SIZE * sizeof(struct bpe_vocab_entry *));
    if (!g_bpe.hash_table) {
        console_printf("Error: Tokenizer memory allocation failed\n");
        return -1;
    }
    memset(g_bpe.hash_table, 0, BPE_HASH_SIZE * sizeof(struct bpe_vocab_entry *));

    /* Allocate id->text array */
    g_bpe.id_to_text = heap_alloc(vocab_size * sizeof(char *));
    if (!g_bpe.id_to_text) {
        console_printf("Error: Tokenizer memory allocation failed\n");
        return -1;
    }
    memset(g_bpe.id_to_text, 0, vocab_size * sizeof(char *));

    /* Allocate scores array */
    g_bpe.scores = heap_alloc(vocab_size * sizeof(float));
    if (!g_bpe.scores) {
        console_printf("Error: Tokenizer memory allocation failed\n");
        return -1;
    }
    memset(g_bpe.scores, 0, vocab_size * sizeof(float));

    /* Pre-allocate entry pool - single allocation for all vocab entries */
    g_bpe.entry_pool = heap_alloc(vocab_size * sizeof(struct bpe_vocab_entry));
    if (!g_bpe.entry_pool) {
        console_printf("Error: Tokenizer memory allocation failed\n");
        return -1;
    }
    g_bpe.entry_pool_idx = 0;

    /* Pre-allocate text pool - estimate ~16 bytes average per token */
    g_bpe.text_pool_size = vocab_size * 16;
    g_bpe.text_pool = heap_alloc(g_bpe.text_pool_size);
    if (!g_bpe.text_pool) {
        console_printf("Error: Tokenizer memory allocation failed\n");
        return -1;
    }
    g_bpe.text_pool_idx = 0;

    /* Get special token IDs and tokenizer type from GGUF */
    const struct gguf_model_arch *arch = gguf_parser_get_arch();
    if (arch) {
        g_bpe.bos_token = arch->bos_token_id;
        g_bpe.eos_token = arch->eos_token_id;
        g_bpe.unk_token = 0; /* Usually 0 */

        /* Detect tokenizer type from GGUF metadata */
        if (arch->tokenizer_model[0] != '\0') {
            if (strcmp(arch->tokenizer_model, "gpt2") == 0 ||
                strcmp(arch->tokenizer_model, "smollm") == 0) {
                g_bpe.tokenizer_type = BPE_TYPE_GPT2;
            } else if (strcmp(arch->tokenizer_model, "llama") == 0 ||
                       strncmp(arch->tokenizer_model, "sentence", 8) == 0) {
                g_bpe.tokenizer_type = BPE_TYPE_SENTENCEPIECE;
            } else {
                /* Default to GPT-2 for unknown types (more common) */
                g_bpe.tokenizer_type = BPE_TYPE_GPT2;
            }
        } else {
            /* No tokenizer model specified, default to GPT-2 */
            g_bpe.tokenizer_type = BPE_TYPE_GPT2;
        }
    } else {
        g_bpe.bos_token = BPE_TOKEN_BOS;
        g_bpe.eos_token = BPE_TOKEN_EOS;
        g_bpe.unk_token = BPE_TOKEN_UNK;
        g_bpe.tokenizer_type = BPE_TYPE_GPT2; /* Default */
    }

    /* Load all tokens into hash table */
    for (uint32_t i = 0; i < vocab_size; i++) {
        const char *text = gguf_parser_get_token(i);
        float score = gguf_parser_get_token_score(i);

        if (text && strlen(text) > 0) {
            bpe_vocab_insert(text, i, score);
        }
    }

    g_bpe.initialized = true;
    return 0;
}

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
int bpe_tokenizer_encode(const char *text, int *tokens, int max_tokens, bool add_bos, bool add_eos)
{
    if (!g_bpe.initialized || !text || !tokens || max_tokens <= 0) {
        return 0;
    }

    int n_tokens = 0;

    /* Add BOS token if requested */
    if (add_bos && n_tokens < max_tokens) {
        tokens[n_tokens++] = (int)g_bpe.bos_token;
    }

    /* Preprocess text for SentencePiece compatibility */
    size_t text_len = strlen(text);
    char *processed = heap_alloc(text_len * 4 + 1); /* Worst case: 3 bytes per char */
    if (!processed) {
        return n_tokens;
    }

    bpe_preprocess_text(text, processed, text_len * 4);

    /* Encode using greedy longest match */
    int encoded = bpe_encode_greedy(processed, tokens + n_tokens, max_tokens - n_tokens - 1);
    n_tokens += encoded;

    /* Note: heap_alloc doesn't have free, but this is small and short-lived */

    /* Add EOS token if requested */
    if (add_eos && n_tokens < max_tokens) {
        tokens[n_tokens++] = (int)g_bpe.eos_token;
    }

    return n_tokens;
}

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
int bpe_tokenizer_decode(const int *tokens, int n_tokens, char *text, int max_len)
{
    if (!g_bpe.initialized || !tokens || !text || max_len <= 0) {
        return 0;
    }

    int pos = 0;

    for (int i = 0; i < n_tokens && pos < max_len - 1; i++) {
        int token_id = tokens[i];

        /* Skip special tokens */
        if (token_id == (int)g_bpe.bos_token || token_id == (int)g_bpe.eos_token) {
            continue;
        }

        /* Get token text */
        if (token_id >= 0 && token_id < (int)g_bpe.vocab_size) {
            const char *token_text = g_bpe.id_to_text[token_id];
            if (token_text) {
                size_t len = strlen(token_text);

                /* Handle SentencePiece space marker (▁) */
                if (len >= 3 && (uint8_t)token_text[0] == 0xE2 && (uint8_t)token_text[1] == 0x96 &&
                    (uint8_t)token_text[2] == 0x81) {
                    /* Replace ▁ with space */
                    if (pos > 0 && pos < max_len - 1) {
                        text[pos++] = ' ';
                    }
                    /* Copy rest of token */
                    for (size_t j = 3; j < len && pos < max_len - 1; j++) {
                        text[pos++] = token_text[j];
                    }
                } else if (len >= 2 && (uint8_t)token_text[0] == 0xC4 && (uint8_t)token_text[1] == 0xA0) {
                    /* Handle GPT-2 space marker (Ġ = U+0120) */
                    /* Replace Ġ with space */
                    if (pos < max_len - 1) {
                        text[pos++] = ' ';
                    }
                    /* Copy rest of token */
                    for (size_t j = 2; j < len && pos < max_len - 1; j++) {
                        text[pos++] = token_text[j];
                    }
                } else {
                    /* Copy token text, converting GPT-2 special chars:
                     * Ġ (U+0120 = 0xC4 0xA0) -> space
                     * Ċ (U+010A = 0xC4 0x8A) -> newline
                     */
                    for (size_t j = 0; j < len && pos < max_len - 1; j++) {
                        uint8_t c = (uint8_t)token_text[j];
                        if (c == 0xC4 && j + 1 < len) {
                            uint8_t c2 = (uint8_t)token_text[j + 1];
                            if (c2 == 0xA0) {
                                /* Ġ -> space */
                                text[pos++] = ' ';
                                j++;
                                continue;
                            } else if (c2 == 0x8A) {
                                /* Ċ -> newline */
                                text[pos++] = '\n';
                                j++;
                                continue;
                            }
                        }
                        text[pos++] = token_text[j];
                    }
                }
            }
        }
    }

    text[pos] = '\0';
    return pos;
}

/**
 * bpe_tokenizer_decode_token - Decode single token to text
 *
 * Returns: Pointer to token text (static buffer)
 */
const char *bpe_tokenizer_decode_token(int token_id)
{
    static char buffer[BPE_MAX_TOKEN_LEN + 1];

    if (!g_bpe.initialized) {
        return "<not_init>";
    }

    if (token_id == (int)g_bpe.bos_token)
        return "<s>";
    if (token_id == (int)g_bpe.eos_token)
        return "</s>";
    if (token_id == (int)g_bpe.unk_token)
        return "<unk>";

    if (token_id >= 0 && token_id < (int)g_bpe.vocab_size) {
        const char *text = g_bpe.id_to_text[token_id];
        if (text) {
            size_t len = strlen(text);

            /* Convert Ġ (GPT-2 space marker) to space for display */
            if (len >= 2 && (uint8_t)text[0] == 0xC4 && (uint8_t)text[1] == 0xA0) {
                buffer[0] = ' ';
                size_t i;
                for (i = 2; i < len && i - 2 < BPE_MAX_TOKEN_LEN - 1; i++) {
                    buffer[i - 1] = text[i];
                }
                buffer[i - 1] = '\0';
                return buffer;
            }

            return text;
        }
    }

    buffer[0] = '<';
    buffer[1] = '?';
    buffer[2] = '>';
    buffer[3] = '\0';
    return buffer;
}

/**
 * bpe_tokenizer_get_vocab_size - Get vocabulary size
 */
uint32_t bpe_tokenizer_get_vocab_size(void) { return g_bpe.vocab_size; }

/**
 * bpe_tokenizer_get_bos - Get BOS token ID
 */
uint32_t bpe_tokenizer_get_bos(void) { return g_bpe.bos_token; }

/**
 * bpe_tokenizer_get_eos - Get EOS token ID
 */
uint32_t bpe_tokenizer_get_eos(void) { return g_bpe.eos_token; }

/**
 * bpe_tokenizer_is_initialized - Check if tokenizer is ready
 */
bool bpe_tokenizer_is_initialized(void) { return g_bpe.initialized; }

/**
 * bpe_tokenizer_cleanup - Free tokenizer resources
 */
void bpe_tokenizer_cleanup(void)
{
    if (g_bpe.hash_table) {
        /* Free all hash entries */
        for (int i = 0; i < BPE_HASH_SIZE; i++) {
            struct bpe_vocab_entry *entry = g_bpe.hash_table[i];
            while (entry) {
                struct bpe_vocab_entry *next = entry->next;
                if (entry->text)
                    kfree(entry->text);
                kfree(entry);
                entry = next;
            }
        }
        kfree(g_bpe.hash_table);
        g_bpe.hash_table = NULL;
    }

    if (g_bpe.id_to_text) {
        /* Text pointers are owned by hash entries, already freed */
        kfree(g_bpe.id_to_text);
        g_bpe.id_to_text = NULL;
    }

    if (g_bpe.scores) {
        kfree(g_bpe.scores);
        g_bpe.scores = NULL;
    }

    g_bpe.vocab_size = 0;
    g_bpe.initialized = false;
}

/**
 * bpe_tokenizer_test - Test tokenizer with sample text
 */
void bpe_tokenizer_test(void)
{
    console_printf("\n=== BPE Tokenizer Test ===\n");

    if (!g_bpe.initialized) {
        console_printf("ERROR: Tokenizer not initialized\n");
        return;
    }

    const char *test_texts[] = {"Hello", "Hello world", "Once upon a time", "The quick brown fox",
                                NULL};

    int tokens[64];
    char decoded[256];

    for (int i = 0; test_texts[i]; i++) {
        console_printf("\nInput: \"%s\"\n", test_texts[i]);

        int n = bpe_tokenizer_encode(test_texts[i], tokens, 64, false, false);
        console_printf("Tokens (%d): ", n);
        for (int j = 0; j < n && j < 20; j++) {
            console_printf("%d ", tokens[j]);
        }
        console_printf("\n");

        console_printf("Decoded: ");
        for (int j = 0; j < n && j < 20; j++) {
            console_printf("'%s' ", bpe_tokenizer_decode_token(tokens[j]));
        }
        console_printf("\n");

        bpe_tokenizer_decode(tokens, n, decoded, sizeof(decoded));
        console_printf("Reconstructed: \"%s\"\n", decoded);
    }

    console_printf("\n=== Test Complete ===\n");
}

/* ============================================================================
 * Unit Test Registration
 * ============================================================================ */

/**
 * test_bpe_tokenizer - Unit test for BPE tokenizer
 *
 * This test requires a GGUF model to be loaded first. For now, it's a
 * placeholder that reports the tokenizer state.
 */
static int test_bpe_tokenizer(void)
{
    console_printf("TEST: BPE Tokenizer\n");

    if (!g_bpe.initialized) {
        console_printf("SKIP: Tokenizer not initialized (requires GGUF model)\n");
        console_printf("PASS: BPE tokenizer test skipped (no model loaded)\n");
        return 0;
    }

    /* Run actual tokenizer test */
    bpe_tokenizer_test();

    /* Verify that decode converts Ġ to space */
    const char *test_texts[] = {"Hello", "Hello world", NULL};
    int tokens[64];
    char decoded[256];

    for (int i = 0; test_texts[i]; i++) {
        int n = bpe_tokenizer_encode(test_texts[i], tokens, 64, false, false);
        bpe_tokenizer_decode(tokens, n, decoded, sizeof(decoded));

        /* Check that decoded text matches input (spaces preserved) */
        if (strcmp(decoded, test_texts[i]) != 0) {
            console_printf("FAIL: Decoded text mismatch\n");
            console_printf("  Expected: '%s'\n", test_texts[i]);
            console_printf("  Got:      '%s'\n", decoded);
            return -1;
        }

        /* Check that individual tokens show spaces, not Ġ */
        for (int j = 0; j < n; j++) {
            const char *token_text = bpe_tokenizer_decode_token(tokens[j]);
            /* Verify no Ġ character (0xC4 0xA0) in output */
            for (size_t k = 0; token_text[k]; k++) {
                if (k + 1 < strlen(token_text) &&
                    (uint8_t)token_text[k] == 0xC4 && (uint8_t)token_text[k + 1] == 0xA0) {
                    console_printf("FAIL: Token still contains Ġ character\n");
                    return -1;
                }
            }
        }
    }

    console_printf("PASS: BPE tokenizer test passed\n");
    return 0;
}

/* Test case structure */
static struct test_case test_case_bpe = {.name = "bpe", .func = test_bpe_tokenizer, .next = NULL};

/* Test registration using constructor attribute */
static void __attribute__((constructor)) test_register_bpe(void) { test_register(&test_case_bpe); }
