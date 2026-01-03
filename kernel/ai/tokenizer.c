/* EMBODIOS Byte-Level BPE Tokenizer
 *
 * Implements a byte-level BPE tokenizer suitable for TinyLlama models
 * Designed for efficient operation in kernel space
 *
 * NOTE: When bpe_tokenizer is initialized (from GGUF), this tokenizer
 * forwards all calls to it for proper model-specific tokenization.
 */

#include "embodios/types.h"
#include "embodios/kernel.h"
#include "embodios/console.h"
#include "embodios/mm.h"
#include "embodios/bpe_tokenizer.h"

/* TinyLlama vocabulary configuration */
#define VOCAB_SIZE 32000      /* TinyLlama vocab size */
#define BYTE_TOKENS 256       /* Direct byte mappings */
#define MAX_TOKEN_LEN 64      /* Maximum bytes per token */
#define MAX_MERGES 31744      /* vocab_size - byte_tokens */

/* Special token IDs - TinyLlama compatible */
#define TOKEN_UNK 0
#define TOKEN_BOS 1
#define TOKEN_EOS 2
#define TOKEN_PAD 29999       /* Typically at end of vocab */

/* UTF-8 byte ranges */
#define UTF8_1BYTE_MAX 0x7F
#define UTF8_CONT_MIN 0x80
#define UTF8_CONT_MAX 0xBF
#define UTF8_2BYTE_MIN 0xC0
#define UTF8_3BYTE_MIN 0xE0
#define UTF8_4BYTE_MIN 0xF0

/* BPE merge rule */
struct bpe_merge {
    uint16_t pair[2];    /* Token pair to merge */
    uint16_t new_token;  /* Resulting token */
    int32_t score;       /* Merge priority/frequency */
};

/* Token information */
struct token_info {
    uint8_t bytes[MAX_TOKEN_LEN];  /* Actual bytes of the token */
    uint8_t length;                 /* Number of bytes */
    uint8_t is_special;            /* Special token flag */
};

/* Tokenizer state */
static struct {
    struct token_info* tokens;      /* Token information array */
    struct bpe_merge* merges;       /* BPE merge rules */
    int n_merges;                   /* Number of merge rules */
    bool initialized;
} tokenizer_state = {0};

/* String utilities - use kernel implementations */
size_t strlen(const char* str);
int strncmp(const char* s1, const char* s2, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);

/* Helper: Initialize UTF-8 byte mappings */
static void init_utf8_bytes(void)
{
    /* Common UTF-8 patterns for better text handling */
    static const uint8_t utf8_prefixes[] = {
        0xC2, 0xC3, 0xC4, 0xC5,  /* 2-byte sequences */
        0xE0, 0xE1, 0xE2, 0xE3,  /* 3-byte sequences */
        0xF0, 0xF1               /* 4-byte sequences */
    };
    
    /* Initialize base byte tokens (0-255) */
    for (int i = 0; i < BYTE_TOKENS; i++) {
        tokenizer_state.tokens[i].bytes[0] = (uint8_t)i;
        tokenizer_state.tokens[i].length = 1;
        tokenizer_state.tokens[i].is_special = 0;
    }
}

/* Helper: Add a special token */
static void add_special_token(int token_id, const char* text)
{
    if (token_id >= VOCAB_SIZE) return;
    
    struct token_info* token = &tokenizer_state.tokens[token_id];
    int len = strlen(text);
    if (len > MAX_TOKEN_LEN) len = MAX_TOKEN_LEN;
    
    memcpy(token->bytes, text, len);
    token->length = len;
    token->is_special = 1;
}

/* Initialize common BPE merges for English text */
static void init_common_merges(void)
{
    /* Common English bigrams and trigrams as BPE merges */
    static const struct {
        const char* text;
        int priority;
    } common_patterns[] = {
        /* High frequency English patterns */
        {"th", 1000}, {"he", 999}, {"in", 998}, {"er", 997}, {"an", 996},
        {"re", 995}, {"nd", 994}, {"at", 993}, {"on", 992}, {"nt", 991},
        {"ha", 990}, {"es", 989}, {"st", 988}, {"en", 987}, {"ed", 986},
        {"to", 985}, {"it", 984}, {"ou", 983}, {"ea", 982}, {"hi", 981},
        {"is", 980}, {"or", 979}, {"ti", 978}, {"as", 977}, {"te", 976},
        {"et", 975}, {"ng", 974}, {"of", 973}, {"al", 972}, {"de", 971},
        {"se", 970}, {"le", 969}, {"sa", 968}, {"si", 967}, {"ar", 966},
        {"ve", 965}, {"ra", 964}, {"ld", 963}, {"ur", 962}, {"ing", 961},
        {"the", 960}, {"and", 959}, {"tion", 958}, {"ent", 957}, {"ion", 956},
        {"her", 955}, {"for", 954}, {"tha", 953}, {"nth", 952}, {"int", 951},
        {"ere", 950}, {"tio", 949}, {"ter", 948}, {"est", 947}, {"ers", 946},
        {NULL, 0}
    };
    
    int merge_idx = 0;
    int token_id = BYTE_TOKENS;  /* Start after byte tokens */
    
    /* Add common patterns as merged tokens */
    for (int i = 0; common_patterns[i].text && token_id < VOCAB_SIZE && merge_idx < MAX_MERGES; i++) {
        const char* pattern = common_patterns[i].text;
        int len = strlen(pattern);
        
        if (len >= 2 && len <= MAX_TOKEN_LEN) {
            /* Create token for this pattern */
            struct token_info* token = &tokenizer_state.tokens[token_id];
            memcpy(token->bytes, pattern, len);
            token->length = len;
            token->is_special = 0;
            
            /* Create merge rules for consecutive bytes */
            if (len == 2) {
                tokenizer_state.merges[merge_idx].pair[0] = (uint8_t)pattern[0];
                tokenizer_state.merges[merge_idx].pair[1] = (uint8_t)pattern[1];
                tokenizer_state.merges[merge_idx].new_token = token_id;
                tokenizer_state.merges[merge_idx].score = common_patterns[i].priority;
                merge_idx++;
            }
            
            token_id++;
        }
    }
    
    tokenizer_state.n_merges = merge_idx;
    
    /* Fill remaining tokens with simple byte pairs */
    for (int i = 32; i < 127 && token_id < VOCAB_SIZE; i++) {
        for (int j = 32; j < 127 && token_id < VOCAB_SIZE; j++) {
            struct token_info* token = &tokenizer_state.tokens[token_id];
            token->bytes[0] = i;
            token->bytes[1] = j;
            token->length = 2;
            token->is_special = 0;
            token_id++;
        }
    }
}

/* Initialize tokenizer */
int tokenizer_init(void)
{
    console_printf("Tokenizer: Initializing byte-level BPE tokenizer\n");

    /* Allocate memory for tokens */
    size_t tokens_size = VOCAB_SIZE * sizeof(struct token_info);
    tokenizer_state.tokens = kmalloc(tokens_size);

    if (!tokenizer_state.tokens) {
        console_printf("Tokenizer: Failed to allocate token memory (%zu bytes)\n", tokens_size);
        return -1;
    }
    
    /* Clear token memory */
    memset(tokenizer_state.tokens, 0, tokens_size);
    
    /* Allocate memory for merges */
    size_t merges_size = MAX_MERGES * sizeof(struct bpe_merge);
    tokenizer_state.merges = kmalloc(merges_size);
    
    if (!tokenizer_state.merges) {
        console_printf("Tokenizer: Failed to allocate merge memory (%zu bytes)\n", merges_size);
        kfree(tokenizer_state.tokens);
        tokenizer_state.tokens = NULL;
        return -1;
    }

    /* Initialize UTF-8 byte mappings */
    init_utf8_bytes();

    /* Add special tokens */
    add_special_token(TOKEN_UNK, "<unk>");
    add_special_token(TOKEN_BOS, "<s>");
    add_special_token(TOKEN_EOS, "</s>");
    add_special_token(TOKEN_PAD, "<pad>");

    /* Initialize common BPE merges */
    init_common_merges();

    tokenizer_state.initialized = true;
    console_printf("Tokenizer: Initialized with %d tokens, %d merges\n",
                  VOCAB_SIZE, tokenizer_state.n_merges);

    return 0;
}

/* Helper: Find best BPE merge for a token sequence */
static int find_best_merge(uint16_t* sequence, int seq_len, int* merge_pos)
{
    int best_merge = -1;
    int best_score = -1;
    *merge_pos = -1;
    
    /* Look for the highest scoring merge in the sequence */
    for (int i = 0; i < seq_len - 1; i++) {
        uint16_t pair[2] = {sequence[i], sequence[i + 1]};
        
        /* Search for this pair in merge rules */
        for (int j = 0; j < tokenizer_state.n_merges; j++) {
            if (tokenizer_state.merges[j].pair[0] == pair[0] &&
                tokenizer_state.merges[j].pair[1] == pair[1] &&
                tokenizer_state.merges[j].score > best_score) {
                best_merge = j;
                best_score = tokenizer_state.merges[j].score;
                *merge_pos = i;
            }
        }
    }
    
    return best_merge;
}

/* Helper: Apply BPE merge to sequence */
static int apply_merge(uint16_t* sequence, int seq_len, int merge_idx, int merge_pos)
{
    if (merge_idx < 0 || merge_pos < 0 || merge_pos >= seq_len - 1) {
        return seq_len;
    }
    
    /* Replace pair with new token */
    sequence[merge_pos] = tokenizer_state.merges[merge_idx].new_token;
    
    /* Shift remaining tokens left */
    for (int i = merge_pos + 1; i < seq_len - 1; i++) {
        sequence[i] = sequence[i + 1];
    }
    
    return seq_len - 1;
}

/* Helper: Check if byte is valid UTF-8 start */
static int utf8_char_length(uint8_t byte)
{
    if (byte <= UTF8_1BYTE_MAX) return 1;
    if ((byte & 0xE0) == UTF8_2BYTE_MIN) return 2;
    if ((byte & 0xF0) == UTF8_3BYTE_MIN) return 3;
    if ((byte & 0xF8) == UTF8_4BYTE_MIN) return 4;
    return 1;  /* Treat invalid as single byte */
}

/* Encode text to tokens using BPE */
int tokenizer_encode(const char* text, int* tokens, int max_tokens)
{
    /* Forward to proper BPE tokenizer if available (from GGUF) */
    if (bpe_tokenizer_is_initialized()) {
        return bpe_tokenizer_encode(text, tokens, max_tokens, true, true);
    }

    /* Fallback to built-in tokenizer */
    if (!tokenizer_state.initialized) {
        console_printf("Tokenizer: Not initialized\n");
        return 0;
    }

    int n_tokens = 0;
    int text_len = strlen(text);

    /* Add BOS token */
    if (n_tokens < max_tokens) {
        tokens[n_tokens++] = TOKEN_BOS;
    }

    /* Temporary buffer for BPE processing */
    uint16_t* work_buffer = kmalloc(text_len * sizeof(uint16_t));
    if (!work_buffer) {
        console_printf("Tokenizer: Failed to allocate work buffer\n");
        return n_tokens;
    }

    /* Process text in chunks (respecting UTF-8 boundaries) */
    int i = 0;
    while (i < text_len && n_tokens < max_tokens - 1) {
        int work_len = 0;

        /* Convert chunk to initial byte tokens */
        while (i < text_len && work_len < 512) {  /* Process in 512 byte chunks */
            uint8_t byte = (uint8_t)text[i];
            work_buffer[work_len++] = byte;
            i++;

            /* Stop at word boundaries for better BPE */
            if (byte == ' ' || byte == '\n' || byte == '\t') {
                break;
            }
        }

        /* Apply BPE merges iteratively */
        int merge_applied = 1;
        while (merge_applied && work_len > 1) {
            int merge_pos;
            int merge_idx = find_best_merge(work_buffer, work_len, &merge_pos);

            if (merge_idx >= 0) {
                work_len = apply_merge(work_buffer, work_len, merge_idx, merge_pos);
            } else {
                merge_applied = 0;
            }
        }

        /* Add merged tokens to output */
        for (int j = 0; j < work_len && n_tokens < max_tokens - 1; j++) {
            tokens[n_tokens++] = work_buffer[j];
        }
    }

    /* Add EOS token */
    if (n_tokens < max_tokens) {
        tokens[n_tokens++] = TOKEN_EOS;
    }

    kfree(work_buffer);
    return n_tokens;
}

/* Decode tokens to text */
int tokenizer_decode(int* tokens, int n_tokens, char* text, int max_length)
{
    /* Forward to proper BPE tokenizer if available (from GGUF) */
    if (bpe_tokenizer_is_initialized()) {
        return bpe_tokenizer_decode(tokens, n_tokens, text, max_length);
    }

    /* Fallback to built-in tokenizer */
    if (!tokenizer_state.initialized) {
        console_printf("Tokenizer: Not initialized\n");
        return 0;
    }
    
    int pos = 0;
    
    for (int i = 0; i < n_tokens && pos < max_length - 1; i++) {
        int token = tokens[i];
        
        /* Validate token */
        if (token < 0 || token >= VOCAB_SIZE) {
            console_printf("Tokenizer: Invalid token %d\n", token);
            continue;
        }
        
        struct token_info* info = &tokenizer_state.tokens[token];
        
        /* Skip special tokens unless they're meaningful */
        if (info->is_special) {
            if (token == TOKEN_BOS || token == TOKEN_EOS || token == TOKEN_PAD) {
                continue;  /* Skip these special tokens */
            }
            /* Include <unk> in output */
        }
        
        /* Copy token bytes to output */
        for (int j = 0; j < info->length && pos < max_length - 1; j++) {
            text[pos++] = (char)info->bytes[j];
        }
    }
    
    text[pos] = '\0';
    return pos;
}

/* Decode single token */
const char* tokenizer_decode_token(int token)
{
    /* Forward to proper BPE tokenizer if available (from GGUF) */
    if (bpe_tokenizer_is_initialized()) {
        return bpe_tokenizer_decode_token(token);
    }

    /* Fallback to built-in tokenizer */
    static char buffer[MAX_TOKEN_LEN + 16];  /* Extra space for formatting */

    if (!tokenizer_state.initialized || token < 0 || token >= VOCAB_SIZE) {
        return "<?>";
    }
    
    struct token_info* info = &tokenizer_state.tokens[token];
    
    /* Handle special tokens */
    if (info->is_special) {
        memcpy(buffer, info->bytes, info->length);
        buffer[info->length] = '\0';
        return buffer;
    }
    
    /* Handle regular tokens */
    if (info->length == 1) {
        /* Single byte token */
        uint8_t byte = info->bytes[0];
        if (byte >= 32 && byte < 127) {
            /* Printable ASCII */
            buffer[0] = byte;
            buffer[1] = '\0';
        } else if (byte == '\n') {
            buffer[0] = '\\';
            buffer[1] = 'n';
            buffer[2] = '\0';
        } else if (byte == '\t') {
            buffer[0] = '\\';
            buffer[1] = 't';
            buffer[2] = '\0';
        } else if (byte == '\r') {
            buffer[0] = '\\';
            buffer[1] = 'r';
            buffer[2] = '\0';
        } else {
            /* Non-printable - show as hex */
            buffer[0] = '<';
            buffer[1] = '0';
            buffer[2] = 'x';
            const char* hex = "0123456789ABCDEF";
            buffer[3] = hex[byte >> 4];
            buffer[4] = hex[byte & 0xF];
            buffer[5] = '>';
            buffer[6] = '\0';
        }
    } else {
        /* Multi-byte token */
        int pos = 0;
        for (int i = 0; i < info->length && pos < MAX_TOKEN_LEN; i++) {
            uint8_t byte = info->bytes[i];
            if (byte >= 32 && byte < 127) {
                buffer[pos++] = byte;
            } else {
                /* Show non-printable bytes as dots */
                buffer[pos++] = '.';
            }
        }
        buffer[pos] = '\0';
    }
    
    return buffer;
}

/* Get vocabulary size */
int tokenizer_vocab_size(void)
{
    return VOCAB_SIZE;
}

/* Load tokenizer vocabulary from model data */
int tokenizer_load_vocab(const void* vocab_data, size_t vocab_size)
{
    console_printf("Tokenizer: Loading vocabulary from model (%zu bytes)\n", vocab_size);
    
    /* This would be called when loading a real model with its vocabulary */
    /* For now, we use our pre-initialized vocabulary */
    
    return 0;
}

/* Free tokenizer resources */
void tokenizer_cleanup(void)
{
    if (tokenizer_state.tokens) {
        kfree(tokenizer_state.tokens);
        tokenizer_state.tokens = NULL;
    }
    
    if (tokenizer_state.merges) {
        kfree(tokenizer_state.merges);
        tokenizer_state.merges = NULL;
    }
    
    tokenizer_state.n_merges = 0;
    tokenizer_state.initialized = false;
}