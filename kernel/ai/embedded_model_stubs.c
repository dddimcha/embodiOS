/**
 * EMBODIOS Embedded Model Stubs
 *
 * Provides weak definitions for embedded model symbols.
 * These will be overridden by real symbols when models are embedded
 * via objcopy, but allow the kernel to link when models are not present.
 *
 * Detection: Use an explicit flag that defaults to 0 (not embedded).
 * When the model IS embedded, the real symbols override the weak ones.
 */

#include <embodios/types.h>

/* ============================================================================
 * Embedding detection flags
 * These are set to 1 only when model/tokenizer are actually embedded
 * ============================================================================ */

/* Weak flag - 0 by default, overridden to 1 when model embedded */
int _tinystories_model_present __attribute__((weak)) = 0;
int _tokenizer_present __attribute__((weak)) = 0;

/* ============================================================================
 * TinyStories Model Stubs
 * ============================================================================ */

/* Placeholder stubs - only used when model not embedded */
const uint8_t _binary_tinystories_15m_bin_start[1] __attribute__((weak)) = {0};
const uint8_t _binary_tinystories_15m_bin_end[1] __attribute__((weak)) = {0};

/* ============================================================================
 * Tokenizer Stubs
 * ============================================================================ */

const uint8_t _binary_tokenizer_bin_start[1] __attribute__((weak)) = {0};
const uint8_t _binary_tokenizer_bin_end[1] __attribute__((weak)) = {0};

/**
 * Check if TinyStories model is embedded
 * Returns 1 if embedded, 0 if using stubs
 */
int tinystories_model_embedded(void) {
    /* Check the flag - will be 0 from stub, or 1 if model was embedded */
    return _tinystories_model_present;
}

/**
 * Check if tokenizer is embedded
 */
int tokenizer_embedded(void) {
    return _tokenizer_present;
}
