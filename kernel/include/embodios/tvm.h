/* EMBODIOS TVM Runtime Interface */
#ifndef _EMBODIOS_TVM_H
#define _EMBODIOS_TVM_H

#include <embodios/types.h>

/* Forward declarations */
struct embodios_model;  /* Forward declaration */

/* Tensor structure - exposed for graph executor */
typedef struct TVMTensor {
    void* data;
    int64_t* shape;
    int ndim;
    int dtype;
    int64_t* strides;
    uint64_t byte_offset;
} TVMTensor;

/* Function handle */
typedef struct {
    const char* name;
    void (*func_ptr)(TVMTensor** args, int* type_codes, int num_args, TVMTensor* ret);
    int num_inputs;
    int num_outputs;
} TVMFunction;

/* Module containing compiled functions */
typedef struct TVMModule {
    const char* name;
    TVMFunction* functions;
    int num_functions;
    void* module_data;
    size_t module_size;
} TVMModule;

/* TVM data types */
#define TVM_DTYPE_FLOAT32   0
#define TVM_DTYPE_INT32     1
#define TVM_DTYPE_INT8      2
#define TVM_DTYPE_UINT8     3

/* Initialize TVM runtime */
int tvm_runtime_init(void);

/* Tensor operations */
TVMTensor* tvm_tensor_create(int64_t* shape, int ndim, int dtype);
void tvm_tensor_free(TVMTensor* tensor);

/* Module operations */
TVMModule* tvm_module_load(const void* module_data, size_t size);
int tvm_module_run(TVMModule* module, TVMTensor* input, TVMTensor* output);

/* TVM Runtime structure */
typedef struct TVMRuntime {
    bool initialized;
    void* workspace;
    size_t workspace_size;
} TVMRuntime;

/* Model loader functions - defined after graph executor type */

/* Runtime info */
void tvm_runtime_stats(void);
void* tvm_as_model_backend(void);
TVMRuntime* tvm_get_runtime(void);

/* Graph node structure */
typedef struct TVMGraphNode {
    const char* op_type;
    const char* name;
    int num_inputs;
    int num_outputs;
    int inputs[8];
    int outputs[8];
} TVMGraphNode;

/* Graph executor structure */
typedef struct TVMGraphExecutor {
    TVMGraphNode* nodes;
    int num_nodes;
    TVMTensor** tensors;
    int num_tensors;
    int* input_indices;
    int num_inputs;
    int* output_indices;
    int num_outputs;
} TVMGraphExecutor;

/* Graph executor API */
typedef struct tvm_graph_executor {
    void* nodes;
    int num_nodes;
    TVMTensor** tensors;
    int num_tensors;
    int* input_indices;
    int num_inputs;
    int* output_indices;
    int num_outputs;
} tvm_graph_executor_t;

/* Graph node operation types */
typedef enum {
    TVM_OP_DENSE,
    TVM_OP_ADD,
    TVM_OP_RELU,
    TVM_OP_SOFTMAX,
    TVM_OP_CONV2D,
    TVM_OP_MAXPOOL2D,
    TVM_OP_RESHAPE,
    TVM_OP_CONCAT,
} tvm_op_type_t;

/* Graph executor functions */
tvm_graph_executor_t* tvm_graph_executor_create(void);
void tvm_graph_executor_free(tvm_graph_executor_t* executor);
int tvm_graph_add_node(tvm_graph_executor_t* executor, tvm_op_type_t op_type,
                       const char* name, int* inputs, int num_inputs, int output);
int tvm_graph_allocate_storage(tvm_graph_executor_t* executor, int num_tensors);
void tvm_graph_set_inputs(tvm_graph_executor_t* executor, int* indices, int num);
void tvm_graph_set_outputs(tvm_graph_executor_t* executor, int* indices, int num);
int tvm_graph_execute(tvm_graph_executor_t* executor);

/* Helper to create test graphs */
tvm_graph_executor_t* tvm_create_mlp_graph(int input_dim, int hidden_dim, int output_dim);

/* Model loader functions */
TVMModule* tvm_module_load_from_memory(const void* data, size_t size);
void* tvm_create_test_module(size_t* out_size);
int embodios_model_to_tvm(struct embodios_model* model, tvm_graph_executor_t** out_executor);

#endif /* _EMBODIOS_TVM_H */
