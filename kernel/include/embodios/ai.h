/* EMBODIOS AI Runtime Interface */
#ifndef _EMBODIOS_AI_H
#define _EMBODIOS_AI_H

#include <embodios/types.h>
#include <embodios/model.h>

/* Initialize model runtime */
void model_runtime_init(void);

/* Load model from memory (replaces stub) */
void* model_load(const void* data, size_t size);

/* Run inference */
int model_inference(const int *input_tokens, int num_tokens, 
                   int *output_tokens, int max_output);

/* Get current loaded model */
struct embodios_model* get_current_model(void);

/* Unload model */
void model_unload(void);

#endif /* _EMBODIOS_AI_H */