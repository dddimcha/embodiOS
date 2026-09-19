/* Chat Template Support for EMBODIOS
 *
 * Wraps user prompts in the chat format the loaded model was trained with
 * (ChatML, LLaMA-2 [INST], GLM). Format is autodetected from vocabulary
 * marker tokens or can be forced with the 'chatformat' command.
 */

#ifndef _EMBODIOS_CHAT_TEMPLATE_H
#define _EMBODIOS_CHAT_TEMPLATE_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHAT_FORMAT_AUTO = 0,   /* Autodetect from vocabulary (default) */
    CHAT_FORMAT_OFF,        /* Raw prompt, no template */
    CHAT_FORMAT_CHATML,     /* <|im_start|>user ... <|im_end|> (SmolLM, Qwen) */
    CHAT_FORMAT_LLAMA2,     /* [INST] ... [/INST] */
    CHAT_FORMAT_GLM,        /* [gMASK]<sop><|user|> ... <|assistant|> */
} chat_format_t;

/* Configure format (chatformat command). CHAT_FORMAT_AUTO = autodetect. */
void chat_template_set_format(chat_format_t fmt);

/* Get configured format (may be CHAT_FORMAT_AUTO) */
chat_format_t chat_template_get_config(void);

/* Resolve active format: detects from vocab if AUTO.
 * Returns CHAT_FORMAT_OFF if no model/vocab is loaded or no known
 * template markers are found. Prints "[CHAT] Detected chat format: ..."
 * once per detection. */
chat_format_t chat_template_resolve(void);

/* Human-readable format name ("auto", "off", "chatml", "llama2", "glm") */
const char* chat_template_format_name(chat_format_t fmt);

/* Parse format name; returns -1 on unknown name */
int chat_template_parse_name(const char* name);

/* Invalidate detection cache - must be called whenever a new model/vocab
 * is loaded (otherwise the format detected for the previous model sticks) */
void chat_template_invalidate_cache(void);

/* Wrap user prompt in the active chat template.
 * Returns number of bytes written to out (>= 0), or -1 if the active
 * format is OFF / unavailable (caller should use the raw prompt). */
int chat_template_wrap(const char* user_prompt, char* out, size_t out_size);

/* Stop token ID for the active template (e.g. <|im_end|> for ChatML),
 * or -1 to keep the model's default EOS from GGUF metadata. */
int chat_template_stop_token(void);

/* Find token ID by exact vocabulary text, or -1 if absent */
int chat_template_find_token(const char* text);

#ifdef __cplusplus
}
#endif

#endif /* _EMBODIOS_CHAT_TEMPLATE_H */
