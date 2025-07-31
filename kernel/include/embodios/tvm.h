/* EMBODIOS TVM Runtime Interface */
#ifndef _EMBODIOS_TVM_H
#define _EMBODIOS_TVM_H

#include <embodios/types.h>

/* Forward declarations */
typedef struct TVMTensor TVMTensor;
typedef struct TVMModule TVMModule;

/* TVM data types */
#define TVM_DTYPE_FLOAT32   0
#define TVM_DTYPE_INT32     1
#define TVM_DTYPE_INT8      2
#define TVM_DTYPE_UINT8     3

/* Initialize TVM runtime */
void tvm_runtime_init(void);

/* Tensor operations */
TVMTensor* tvm_tensor_create(int64_t* shape, int ndim, int dtype);
void tvm_tensor_free(TVMTensor* tensor);

/* Module operations */
TVMModule* tvm_module_load(const void* module_data, size_t size);
int tvm_module_run(TVMModule* module, TVMTensor* input, TVMTensor* output);

/* Runtime info */
void tvm_runtime_stats(void);
void* tvm_as_model_backend(void);

#endif /* _EMBODIOS_TVM_H */