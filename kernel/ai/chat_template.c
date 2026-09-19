/* Chat Template Support for EMBODIOS
 *
 * Autodetects the chat format from vocabulary marker tokens and wraps
 * user prompts accordingly. Supported formats:
 *  - ChatML  (<|im_start|>/<|im_end|>)  - SmolLM-Instruct, Qwen, etc.
 *  - LLaMA-2 ([INST] ... [/INST])
 *  - GLM     ([gMASK]sop <|user|> ... <|assistant|>)
 */

#include <embodios/chat_template.h>
#include <embodios/console.h>
#include <embodios/gguf_parser.h>
#include <embodios/types.h>

/* String functions */
size_t strlen(const char* s);
int strcmp(const char* s1, const char* s2);
int snprintf(char* str, size_t size, const char* format, ...);

/* State */
static chat_format_t g_config = CHAT_FORMAT_AUTO;
static int g_detected = -1;  /* -1 = cache invalid */

void chat_template_set_format(chat_format_t fmt)
{
    g_config = fmt;
}

chat_format_t chat_template_get_config(void)
{
    return g_config;
}

void chat_template_invalidate_cache(void)
{
    g_detected = -1;
}

const char* chat_template_format_name(chat_format_t fmt)
{
    switch (fmt) {
        case CHAT_FORMAT_AUTO:   return "auto";
        case CHAT_FORMAT_OFF:    return "off";
        case CHAT_FORMAT_CHATML: return "chatml";
        case CHAT_FORMAT_LLAMA2: return "llama2";
        case CHAT_FORMAT_GLM:    return "glm";
        case CHAT_FORMAT_LLAMA3: return "llama3";
        default:                 return "unknown";
    }
}

int chat_template_parse_name(const char* name)
{
    if (strcmp(name, "auto") == 0)   return CHAT_FORMAT_AUTO;
    if (strcmp(name, "off") == 0)    return CHAT_FORMAT_OFF;
    if (strcmp(name, "chatml") == 0) return CHAT_FORMAT_CHATML;
    if (strcmp(name, "llama2") == 0) return CHAT_FORMAT_LLAMA2;
    if (strcmp(name, "glm") == 0)    return CHAT_FORMAT_GLM;
    if (strcmp(name, "llama3") == 0) return CHAT_FORMAT_LLAMA3;
    return -1;
}

int chat_template_find_token(const char* text)
{
    uint32_t vocab_size = gguf_parser_get_vocab_size();
    for (uint32_t i = 0; i < vocab_size; i++) {
        const char* tok = gguf_parser_get_token(i);
        if (tok && strcmp(tok, text) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Detect format from vocabulary marker tokens */
static chat_format_t detect_format(void)
{
    if (gguf_parser_get_vocab_size() == 0) {
        return CHAT_FORMAT_OFF;  /* No model/vocab loaded */
    }

    if (chat_template_find_token("<|im_start|>") >= 0 &&
        chat_template_find_token("<|im_end|>") >= 0) {
        return CHAT_FORMAT_CHATML;
    }
    if (chat_template_find_token("<|start_header_id|>") >= 0 &&
        chat_template_find_token("<|eot_id|>") >= 0) {
        return CHAT_FORMAT_LLAMA3;
    }
    if (chat_template_find_token("[INST]") >= 0 &&
        chat_template_find_token("[/INST]") >= 0) {
        return CHAT_FORMAT_LLAMA2;
    }
    if (chat_template_find_token("[gMASK]") >= 0) {
        return CHAT_FORMAT_GLM;
    }
    return CHAT_FORMAT_OFF;
}

chat_format_t chat_template_resolve(void)
{
    if (g_config != CHAT_FORMAT_AUTO) {
        return g_config;
    }
    if (g_detected < 0) {
        g_detected = (int)detect_format();
        console_printf("[CHAT] Detected chat format: %s\n",
                       chat_template_format_name((chat_format_t)g_detected));
    }
    return (chat_format_t)g_detected;
}

int chat_template_wrap(const char* user_prompt, char* out, size_t out_size)
{
    chat_format_t fmt = chat_template_resolve();
    int len = -1;

    switch (fmt) {
        case CHAT_FORMAT_CHATML:
            len = snprintf(out, out_size,
                           "<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n",
                           user_prompt);
            break;
        case CHAT_FORMAT_LLAMA2:
            len = snprintf(out, out_size,
                           "[INST] %s [/INST]",
                           user_prompt);
            break;
        case CHAT_FORMAT_GLM:
            /* GLM-4 chat format: [gMASK]<sop><|user|>\n{q}<|assistant|>
             * (<sop> is a real special token in the GLM vocab) */
            len = snprintf(out, out_size,
                           "[gMASK]<sop><|user|>\n%s<|assistant|>",
                           user_prompt);
            break;
        case CHAT_FORMAT_LLAMA3:
            /* Llama-3.x chat format (BOS is prepended by the tokenizer):
             * <|start_header_id|>user<|end_header_id|>\n\n{q}<|eot_id|>
             * <|start_header_id|>assistant<|end_header_id|>\n\n */
            len = snprintf(out, out_size,
                           "<|start_header_id|>user<|end_header_id|>\n\n"
                           "%s<|eot_id|>"
                           "<|start_header_id|>assistant<|end_header_id|>\n\n",
                           user_prompt);
            break;
        case CHAT_FORMAT_OFF:
        case CHAT_FORMAT_AUTO:
        default:
            return -1;  /* Caller uses raw prompt */
    }

    if (len < 0 || (size_t)len >= out_size) {
        return -1;  /* Truncated - safer to fall back to raw prompt */
    }
    return len;
}

int chat_template_stop_token(void)
{
    chat_format_t fmt = chat_template_resolve();

    switch (fmt) {
        case CHAT_FORMAT_CHATML:
            /* ChatML models stop on <|im_end|> (SmolLM: id 2, same as eos) */
            return chat_template_find_token("<|im_end|>");
        case CHAT_FORMAT_GLM:
            /* GLM-4 chat actually stops on <|user|> (id 151336 upstream);
             * <|endoftext|> (151329) stays as metadata EOS */
            return chat_template_find_token("<|user|>");
        case CHAT_FORMAT_LLAMA3:
            /* Llama-3.x instruct turns end with <|eot_id|> (128009) */
            return chat_template_find_token("<|eot_id|>");
        case CHAT_FORMAT_LLAMA2:
        default:
            return -1;  /* Keep model default EOS from metadata */
    }
}
