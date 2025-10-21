/* EMBODIOS TVM Graph Executor
 * 
 * Implements graph-based execution for TVM compiled models.
 * This allows running complex neural network graphs with multiple operators.
 */

#include "embodios/types.h"
#include "embodios/kernel.h"
#include "embodios/console.h"
#include "embodios/mm.h"
#include "embodios/tvm.h"

/* Node in the computation graph */
typedef struct {
    tvm_op_type_t op_type;
    const char* name;
    int* input_indices;   /* Indices of input tensors */
    int num_inputs;
    int output_index;     /* Index of output tensor */
    void* params;         /* Operation-specific parameters */
} tvm_graph_node_t;

/* Operation implementations */

/* Forward declarations for optimized ops */
void tensor_dense_forward(TVMTensor* input, TVMTensor* weight, 
                         TVMTensor* bias, TVMTensor* output);
void tensor_relu_forward(TVMTensor* input, TVMTensor* output);
void tensor_softmax_forward(TVMTensor* input, TVMTensor* output);

/* Dense layer (matrix multiplication) */
static void tvm_op_dense(TVMTensor* input, TVMTensor* weight, TVMTensor* bias, TVMTensor* output)
{
    /* Use optimized implementation */
    tensor_dense_forward(input, weight, bias, output);
}

/* ReLU activation */
static void tvm_op_relu(TVMTensor* input, TVMTensor* output)
{
    /* Use optimized implementation */
    tensor_relu_forward(input, output);
}

/* Softmax activation */
static void tvm_op_softmax(TVMTensor* input, TVMTensor* output)
{
    /* Use optimized implementation */
    tensor_softmax_forward(input, output);
}

/* Create a graph executor */
tvm_graph_executor_t* tvm_graph_executor_create(void)
{
    tvm_graph_executor_t* executor = kmalloc(sizeof(tvm_graph_executor_t));
    if (!executor) return NULL;
    
    memset(executor, 0, sizeof(tvm_graph_executor_t));
    executor->nodes = NULL;
    executor->num_nodes = 0;
    executor->tensors = NULL;
    executor->num_tensors = 0;
    executor->input_indices = NULL;
    executor->num_inputs = 0;
    executor->output_indices = NULL;
    executor->num_outputs = 0;
    
    return executor;
}

/* Add a node to the graph */
int tvm_graph_add_node(tvm_graph_executor_t* executor, tvm_op_type_t op_type,
                       const char* name, int* inputs, int num_inputs, int output)
{
    /* Cast nodes pointer to proper type */
    tvm_graph_node_t* nodes = (tvm_graph_node_t*)executor->nodes;
    
    /* Reallocate nodes array */
    tvm_graph_node_t* new_nodes = kmalloc((executor->num_nodes + 1) * sizeof(tvm_graph_node_t));
    if (!new_nodes) return -1;
    
    if (nodes) {
        memcpy(new_nodes, nodes, executor->num_nodes * sizeof(tvm_graph_node_t));
        kfree(nodes);
    }
    executor->nodes = new_nodes;
    
    /* Initialize new node */
    tvm_graph_node_t* node = &new_nodes[executor->num_nodes];
    node->op_type = op_type;
    node->name = name;
    node->num_inputs = num_inputs;
    node->output_index = output;
    
    /* Copy input indices */
    if (num_inputs > 0) {
        node->input_indices = kmalloc(num_inputs * sizeof(int));
        if (!node->input_indices) return -1;
        memcpy(node->input_indices, inputs, num_inputs * sizeof(int));
    } else {
        node->input_indices = NULL;
    }
    
    executor->num_nodes++;
    return executor->num_nodes - 1;
}

/* Allocate tensor storage */
int tvm_graph_allocate_storage(tvm_graph_executor_t* executor, int num_tensors)
{
    executor->tensors = kmalloc(num_tensors * sizeof(TVMTensor*));
    if (!executor->tensors) return -1;
    
    memset(executor->tensors, 0, num_tensors * sizeof(TVMTensor*));
    executor->num_tensors = num_tensors;
    return 0;
}

/* Set graph inputs/outputs */
void tvm_graph_set_inputs(tvm_graph_executor_t* executor, int* indices, int num)
{
    executor->input_indices = kmalloc(num * sizeof(int));
    if (executor->input_indices) {
        memcpy(executor->input_indices, indices, num * sizeof(int));
        executor->num_inputs = num;
    }
}

void tvm_graph_set_outputs(tvm_graph_executor_t* executor, int* indices, int num)
{
    executor->output_indices = kmalloc(num * sizeof(int));
    if (executor->output_indices) {
        memcpy(executor->output_indices, indices, num * sizeof(int));
        executor->num_outputs = num;
    }
}

/* Execute the graph */
int tvm_graph_execute(tvm_graph_executor_t* executor)
{
    if (!executor || !executor->nodes) return -1;
    
    tvm_graph_node_t* nodes = (tvm_graph_node_t*)executor->nodes;
    
    console_printf("TVM Graph: Executing %d nodes\n", executor->num_nodes);
    
    /* Execute nodes in topological order */
    for (int i = 0; i < executor->num_nodes; i++) {
        tvm_graph_node_t* node = &nodes[i];
        
        console_printf("  Node %d: %s (op=%d)\n", i, node->name, node->op_type);
        
        /* Get input/output tensors */
        TVMTensor** inputs = kmalloc(node->num_inputs * sizeof(TVMTensor*));
        if (!inputs) return -1;
        
        for (int j = 0; j < node->num_inputs; j++) {
            inputs[j] = executor->tensors[node->input_indices[j]];
        }
        TVMTensor* output = executor->tensors[node->output_index];
        
        /* Execute operation */
        switch (node->op_type) {
            case TVM_OP_DENSE:
                if (node->num_inputs >= 2) {
                    TVMTensor* bias = (node->num_inputs > 2) ? inputs[2] : NULL;
                    tvm_op_dense(inputs[0], inputs[1], bias, output);
                }
                break;
                
            case TVM_OP_RELU:
                if (node->num_inputs >= 1) {
                    tvm_op_relu(inputs[0], output);
                }
                break;
                
            case TVM_OP_SOFTMAX:
                if (node->num_inputs >= 1) {
                    tvm_op_softmax(inputs[0], output);
                }
                break;
                
            default:
                console_printf("  Warning: Unimplemented op type %d\n", node->op_type);
                break;
        }
        
        kfree(inputs);
    }
    
    console_printf("TVM Graph: Execution complete\n");
    return 0;
}

/* Free graph executor */
void tvm_graph_executor_free(tvm_graph_executor_t* executor)
{
    if (!executor) return;
    
    /* Free nodes */
    if (executor->nodes) {
        tvm_graph_node_t* nodes = (tvm_graph_node_t*)executor->nodes;
        for (int i = 0; i < executor->num_nodes; i++) {
            if (nodes[i].input_indices) {
                kfree(nodes[i].input_indices);
            }
        }
        kfree(executor->nodes);
    }
    
    /* Free tensors */
    if (executor->tensors) {
        for (int i = 0; i < executor->num_tensors; i++) {
            tvm_tensor_free(executor->tensors[i]);
        }
        kfree(executor->tensors);
    }
    
    /* Free indices */
    if (executor->input_indices) kfree(executor->input_indices);
    if (executor->output_indices) kfree(executor->output_indices);
    
    kfree(executor);
}

/* Create a simple MLP graph for testing */
tvm_graph_executor_t* tvm_create_mlp_graph(int input_dim, int hidden_dim, int output_dim)
{
    tvm_graph_executor_t* executor = tvm_graph_executor_create();
    if (!executor) return NULL;
    
    /* Allocate storage for tensors:
     * 0: input
     * 1: fc1_weight
     * 2: fc1_bias
     * 3: fc1_output
     * 4: relu_output
     * 5: fc2_weight
     * 6: fc2_bias
     * 7: fc2_output
     * 8: softmax_output
     */
    tvm_graph_allocate_storage(executor, 9);
    
    /* Create tensors */
    int64_t input_shape[] = {1, input_dim};
    int64_t fc1_weight_shape[] = {hidden_dim, input_dim};
    int64_t fc1_bias_shape[] = {hidden_dim};
    int64_t fc1_output_shape[] = {1, hidden_dim};
    int64_t fc2_weight_shape[] = {output_dim, hidden_dim};
    int64_t fc2_bias_shape[] = {output_dim};
    int64_t fc2_output_shape[] = {1, output_dim};
    
    executor->tensors[0] = tvm_tensor_create(input_shape, 2, 0);  /* input */
    executor->tensors[1] = tvm_tensor_create(fc1_weight_shape, 2, 0);  /* fc1_weight */
    executor->tensors[2] = tvm_tensor_create(fc1_bias_shape, 1, 0);  /* fc1_bias */
    executor->tensors[3] = tvm_tensor_create(fc1_output_shape, 2, 0);  /* fc1_output */
    executor->tensors[4] = tvm_tensor_create(fc1_output_shape, 2, 0);  /* relu_output */
    executor->tensors[5] = tvm_tensor_create(fc2_weight_shape, 2, 0);  /* fc2_weight */
    executor->tensors[6] = tvm_tensor_create(fc2_bias_shape, 1, 0);  /* fc2_bias */
    executor->tensors[7] = tvm_tensor_create(fc2_output_shape, 2, 0);  /* fc2_output */
    executor->tensors[8] = tvm_tensor_create(fc2_output_shape, 2, 0);  /* softmax_output */
    
    /* Initialize weights with dummy values */
    float* fc1_w = (float*)executor->tensors[1]->data;
    float* fc1_b = (float*)executor->tensors[2]->data;
    float* fc2_w = (float*)executor->tensors[5]->data;
    float* fc2_b = (float*)executor->tensors[6]->data;
    
    /* Simple initialization */
    for (int i = 0; i < hidden_dim * input_dim; i++) {
        fc1_w[i] = 0.01f * (i % 10);
    }
    for (int i = 0; i < hidden_dim; i++) {
        fc1_b[i] = 0.0f;
    }
    for (int i = 0; i < output_dim * hidden_dim; i++) {
        fc2_w[i] = 0.01f * (i % 10);
    }
    for (int i = 0; i < output_dim; i++) {
        fc2_b[i] = 0.0f;
    }
    
    /* Build graph */
    int fc1_inputs[] = {0, 1, 2};  /* input, weight, bias */
    tvm_graph_add_node(executor, TVM_OP_DENSE, "fc1", fc1_inputs, 3, 3);
    
    int relu_inputs[] = {3};  /* fc1_output */
    tvm_graph_add_node(executor, TVM_OP_RELU, "relu1", relu_inputs, 1, 4);
    
    int fc2_inputs[] = {4, 5, 6};  /* relu_output, weight, bias */
    tvm_graph_add_node(executor, TVM_OP_DENSE, "fc2", fc2_inputs, 3, 7);
    
    int softmax_inputs[] = {7};  /* fc2_output */
    tvm_graph_add_node(executor, TVM_OP_SOFTMAX, "softmax", softmax_inputs, 1, 8);
    
    /* Set inputs and outputs */
    int graph_inputs[] = {0};
    int graph_outputs[] = {8};
    tvm_graph_set_inputs(executor, graph_inputs, 1);
    tvm_graph_set_outputs(executor, graph_outputs, 1);
    
    console_printf("TVM Graph: Created MLP with %d inputs, %d hidden, %d outputs\n",
                   input_dim, hidden_dim, output_dim);
    
    return executor;
}