#ifndef EMBODIOS_MODEL_H
#define EMBODIOS_MODEL_H

#include <embodios/types.h>

/* Model metadata */
struct embodios_model {
    char name[64];
    char version[32];
    size_t param_count;
    size_t layer_count;
    void* weights;
    void* context;
};

/* Tensor structure */
struct tensor {
    float* data;
    size_t* shape;
    size_t ndim;
    size_t size;
};

/* Model operations */
struct embodios_model* model_load(const void* data, size_t size);
void model_unload(struct embodios_model* model);
struct tensor* model_forward(struct embodios_model* model, struct tensor* input);

/* Tensor operations */
struct tensor* tensor_create(size_t* shape, size_t ndim);
void tensor_free(struct tensor* t);
void tensor_fill(struct tensor* t, float value);

/* Natural language processing */
int tokenize(const char* text, int* tokens, size_t max_tokens);
char* detokenize(const int* tokens, size_t count);

#endif /* EMBODIOS_MODEL_H */