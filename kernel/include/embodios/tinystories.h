/**
 * TinyStories-15M Model Interface
 */

#ifndef EMBODIOS_TINYSTORIES_H
#define EMBODIOS_TINYSTORIES_H

#include <embodios/types.h>

/**
 * Load TinyStories-15M model from embedded binary
 * @return 0 on success, -1 on error
 */
int tinystories_load_model(void);

/**
 * Run inference with TinyStories model
 * @param prompt Input text prompt
 * @param output Buffer for generated text
 * @param max_output_len Maximum length of output buffer
 * @return Number of characters generated, or -1 on error
 */
int tinystories_infer(const char* prompt, char* output, int max_output_len);

/**
 * Test TinyStories model (loads and runs demo inference)
 */
void tinystories_test(void);

/**
 * Check if TinyStories model is loaded
 * @return true if model is loaded, false otherwise
 */
bool tinystories_is_loaded(void);

#endif // EMBODIOS_TINYSTORIES_H
