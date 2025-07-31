#ifndef EMBODIOS_MODEL_H
#define EMBODIOS_MODEL_H

#include <embodios/types.h>

/* Model capabilities */
#define MODEL_CAP_TEXT_GEN      0x0001
#define MODEL_CAP_CODE_GEN      0x0002
#define MODEL_CAP_REASONING     0x0004
#define MODEL_CAP_VISION        0x0008

/* Model metadata */
struct embodios_model {
    uint32_t magic;              /* 'EMBO' = 0x454D424F */
    uint32_t version_major;
    uint32_t version_minor;
    char name[64];
    char arch[32];              /* Architecture: transformer, mlp, etc */
    size_t param_count;
    size_t memory_required;     /* Required workspace memory */
    uint32_t capabilities;      /* Bitmask of MODEL_CAP_* */
    uint32_t tokenizer_type;    /* 0=none, 1=ascii, 2=bpe, etc */
    uint32_t reserved[8];       /* Reserved for future use */
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