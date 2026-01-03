/**
 * EMBODIOS Embedded Model Stubs
 *
 * Provides weak definitions for embedded model symbols.
 * These will be overridden by real symbols when models are embedded
 * via objcopy, but allow the kernel to link when models are not present.
 */

#include <embodios/types.h>

/* ============================================================================
 * TinyStories Model Weak Stubs
 * ============================================================================ */

/**
 * Weak stub for TinyStories model start symbol
 * Overridden by objcopy when model is embedded
 */
const uint8_t _binary_tinystories_15m_bin_start[1] __attribute__((weak)) = {0};

/**
 * Weak stub for TinyStories model end symbol
 * Points to same location as start when model not embedded (size = 0)
 */
const uint8_t _binary_tinystories_15m_bin_end[1] __attribute__((weak)) = {0};

/* ============================================================================
 * Tokenizer Weak Stubs
 * ============================================================================ */

/**
 * Weak stub for tokenizer start symbol
 */
const uint8_t _binary_tokenizer_bin_start[1] __attribute__((weak)) = {0};

/**
 * Weak stub for tokenizer end symbol
 */
const uint8_t _binary_tokenizer_bin_end[1] __attribute__((weak)) = {0};
