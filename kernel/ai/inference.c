/* EMBODIOS AI Inference Interface
 * 
 * Provides simple command interface for running AI inference.
 * This is the main user-facing API for the AI-first OS.
 */

#include "embodios/types.h"
#include "embodios/kernel.h"
#include "embodios/console.h"
#include "embodios/mm.h"
#include "embodios/model.h"
#include "embodios/tvm.h"
/* #include "embodios/timer.h" */ /* Timer header not yet implemented */

/* Timer function stubs (until timer.h is implemented) */
static inline uint64_t timer_get_ticks(void) { return 0; }
static inline uint64_t timer_get_frequency(void) { return 1000000; /* 1MHz */ }

/* External functions */
void tokenizer_init(void);
int tokenizer_encode(const char* text, int* tokens, int max_tokens);
int tokenizer_decode(int* tokens, int n_tokens, char* text, int max_length);
const char* tokenizer_decode_token(int token);
int transformer_init(struct embodios_model* model);
void transformer_forward(int* tokens, int n_tokens, float* logits);
int transformer_sample(float* logits, float temperature);
void transformer_reset_cache(void);

/* Parallel execution functions */
int parallel_get_num_threads(void);
typedef void (*work_func_t)(void* arg, int thread_id, int start, int end);
void parallel_for(work_func_t func, void* arg, int total_items, int chunk_size);

/* Function declarations */
int snprintf(char* buffer, size_t size, const char* format, ...);
size_t strlen(const char* str);

/* Inference state */
static struct {
    struct embodios_model* model;
    bool initialized;
    uint64_t inference_count;
    uint64_t total_time_ms;
    int* token_buffer;
    float* logits_buffer;
} inference_state = {
    .initialized = false,
    .inference_count = 0,
    .total_time_ms = 0
};

/* Initialize inference engine with embedded model */
int inference_init(struct embodios_model* model)
{
    if (!model) {
        console_printf("Inference: No model provided\n");
        return -1;
    }
    
    console_printf("Inference: Initializing with model '%s'\n", model->name);
    
    /* Store model reference */
    inference_state.model = model;
    
    /* Initialize tokenizer */
    tokenizer_init();
    console_printf("Inference: Tokenizer initialized\n");
    
    /* Initialize transformer */
    if (transformer_init(model) < 0) {
        console_printf("Inference: Failed to initialize transformer\n");
        return -1;
    }
    console_printf("Inference: Transformer initialized\n");
    
    /* Allocate token and logits buffers - REDUCED for embedded */
    inference_state.token_buffer = kmalloc(512 * sizeof(int));
    inference_state.logits_buffer = kmalloc(1000 * sizeof(float)); /* Match vocab size */
    
    if (!inference_state.token_buffer || !inference_state.logits_buffer) {
        console_printf("Inference: Failed to allocate buffers\n");
        return -1;
    }
    
    inference_state.initialized = true;
    console_printf("Inference: Engine initialized successfully\n");
    return 0;
}

/* Run inference on input text */
int inference_run(const char* input_text, char* output_buffer, size_t output_size)
{
    if (!inference_state.initialized) {
        console_printf("Inference: Engine not initialized\n");
        return -1;
    }
    
    console_printf("Inference: Processing input: '%s'\n", input_text);
    
    /* Start timing */
    uint64_t start_time = timer_get_ticks();
    
    /* Tokenize input */
    int n_tokens = tokenizer_encode(input_text, inference_state.token_buffer, 512);
    if (n_tokens <= 0) {
        console_printf("Inference: Failed to tokenize input\n");
        return -1;
    }
    
    /* Reset transformer cache for new generation */
    transformer_reset_cache();
    
    /* Run forward pass */
    transformer_forward(inference_state.token_buffer, n_tokens, inference_state.logits_buffer);
    
    /* Generate response tokens */
    int generated[256];
    int n_generated = 0;
    int max_tokens = 50;
    
    for (int i = 0; i < max_tokens; i++) {
        /* Sample next token */
        int next_token = transformer_sample(inference_state.logits_buffer, 0.7f);
        generated[n_generated++] = next_token;
        
        /* Check for EOS token */
        if (next_token == 258) {  /* EOS token */
            break;
        }
        
        /* Run forward pass with new token */
        transformer_forward(&next_token, 1, inference_state.logits_buffer);
    }
    
    /* Calculate inference time */
    uint64_t end_time = timer_get_ticks();
    uint64_t elapsed_ms = (end_time - start_time) * 1000 / timer_get_frequency();
    
    /* Update statistics */
    inference_state.inference_count++;
    inference_state.total_time_ms += elapsed_ms;
    
    /* Decode generated tokens */
    tokenizer_decode(generated, n_generated, output_buffer, output_size);
    
    console_printf("Generated %d tokens in %lu ms\n", n_generated, elapsed_ms);
    
    return 0;
}

/* Batch processing arguments for parallel execution */
typedef struct {
    const char** inputs;
    char** outputs;
    size_t* output_sizes;
    int* success_flags;     /* Per-item success/failure tracking */
    int* token_counts;      /* Per-item token counts */
} batch_args_t;

/* Worker function for parallel batch processing */
static void batch_worker(void* arg, int thread_id, int start, int end) {
    batch_args_t* batch = (batch_args_t*)arg;
    (void)thread_id;  /* Unused - for future thread-local optimizations */

    /* Allocate per-thread buffers to avoid race conditions */
    int* token_buffer = kmalloc(512 * sizeof(int));
    float* logits_buffer = kmalloc(1000 * sizeof(float));
    int* generated = kmalloc(256 * sizeof(int));

    if (!token_buffer || !logits_buffer || !generated) {
        console_printf("Inference: Batch[worker] failed to allocate buffers\n");
        /* Mark all items in this range as failed */
        for (int i = start; i < end; i++) {
            batch->success_flags[i] = 0;
        }
        /* Clean up allocated buffers */
        if (token_buffer) kfree(token_buffer);
        if (logits_buffer) kfree(logits_buffer);
        if (generated) kfree(generated);
        return;
    }

    /* Process each batch item in our range */
    for (int i = start; i < end; i++) {
        const char* input = batch->inputs[i];
        char* output = batch->outputs[i];
        size_t output_size = batch->output_sizes[i];

        /* Tokenize input */
        int n_tokens = tokenizer_encode(input, token_buffer, 512);
        if (n_tokens <= 0) {
            console_printf("Inference: Batch[%d] tokenization failed\n", i);
            batch->success_flags[i] = 0;
            continue;
        }

        /* Reset transformer cache for new generation */
        transformer_reset_cache();

        /* Run forward pass */
        transformer_forward(token_buffer, n_tokens, logits_buffer);

        /* Generate response tokens */
        int n_generated = 0;
        int max_tokens = 50;

        for (int j = 0; j < max_tokens; j++) {
            /* Sample next token */
            int next_token = transformer_sample(logits_buffer, 0.7f);
            generated[n_generated++] = next_token;

            /* Check for EOS token */
            if (next_token == 258) {  /* EOS token */
                break;
            }

            /* Run forward pass with new token */
            transformer_forward(&next_token, 1, logits_buffer);
        }

        /* Decode generated tokens */
        tokenizer_decode(generated, n_generated, output, output_size);

        /* Mark as successful */
        batch->success_flags[i] = 1;
        batch->token_counts[i] = n_generated;
    }

    /* Free per-thread buffers */
    kfree(token_buffer);
    kfree(logits_buffer);
    kfree(generated);
}

/* Run batch inference on multiple inputs */
int inference_run_batch(const char** inputs, int n_inputs, char** outputs, size_t* output_sizes)
{
    if (!inference_state.initialized) {
        console_printf("Inference: Engine not initialized\n");
        return -1;
    }

    if (!inputs || !outputs || !output_sizes || n_inputs <= 0) {
        console_printf("Inference: Invalid batch parameters\n");
        return -1;
    }

    /* Get number of available threads */
    int num_threads = parallel_get_num_threads();

    console_printf("Inference: Processing batch of %d inputs using %d threads\n",
                   n_inputs, num_threads);

    /* Allocate per-item tracking arrays */
    int* success_flags = kmalloc(n_inputs * sizeof(int));
    int* token_counts = kmalloc(n_inputs * sizeof(int));

    if (!success_flags || !token_counts) {
        console_printf("Inference: Failed to allocate batch tracking arrays\n");
        if (success_flags) kfree(success_flags);
        if (token_counts) kfree(token_counts);
        return -1;
    }

    /* Initialize tracking arrays */
    for (int i = 0; i < n_inputs; i++) {
        success_flags[i] = 0;
        token_counts[i] = 0;
    }

    /* Start timing for entire batch */
    uint64_t batch_start_time = timer_get_ticks();

    /* Set up batch processing arguments */
    batch_args_t batch_args = {
        .inputs = inputs,
        .outputs = outputs,
        .output_sizes = output_sizes,
        .success_flags = success_flags,
        .token_counts = token_counts
    };

    /* Execute batch in parallel - chunk size of 1 for fine-grained distribution */
    parallel_for(batch_worker, &batch_args, n_inputs, 1);

    /* Calculate total batch time */
    uint64_t batch_end_time = timer_get_ticks();
    uint64_t batch_elapsed_ms = (batch_end_time - batch_start_time) * 1000 / timer_get_frequency();

    /* Count successful completions and total tokens */
    int success_count = 0;
    int total_tokens = 0;
    for (int i = 0; i < n_inputs; i++) {
        if (success_flags[i]) {
            success_count++;
            total_tokens += token_counts[i];
        }
    }

    /* Update global statistics */
    inference_state.inference_count += success_count;
    inference_state.total_time_ms += batch_elapsed_ms;

    console_printf("Inference: Batch completed: %d/%d successful in %lu ms (%d total tokens)\n",
                   success_count, n_inputs, batch_elapsed_ms, total_tokens);

    if (num_threads > 1 && batch_elapsed_ms > 0) {
        console_printf("Inference: Throughput: %.2f inferences/sec\n",
                       (float)success_count * 1000.0f / (float)batch_elapsed_ms);
    }

    /* Clean up tracking arrays */
    kfree(success_flags);
    kfree(token_counts);

    return (success_count == n_inputs) ? 0 : -1;
}

/* Get inference statistics */
void inference_stats(void)
{
    console_printf("=== Inference Statistics ===\n");
    console_printf("Model: %s\n", 
                   inference_state.model ? inference_state.model->name : "None");
    console_printf("Initialized: %s\n", 
                   inference_state.initialized ? "Yes" : "No");
    console_printf("Inference count: %lu\n", inference_state.inference_count);
    
    if (inference_state.inference_count > 0) {
        uint64_t avg_time = inference_state.total_time_ms / inference_state.inference_count;
        console_printf("Average inference time: %lu ms\n", avg_time);
        console_printf("Total inference time: %lu ms\n", inference_state.total_time_ms);
        console_printf("Tokens per second: %.1f\n", 
                      1000.0f / (float)avg_time * 25.0f);  /* Assuming ~25 tokens per inference */
    }
}

/* Test inference with sample inputs */
void inference_test(void)
{
    console_printf("=== Inference Test ===\n");
    
    if (!inference_state.initialized) {
        console_printf("Inference engine not initialized\n");
        return;
    }
    
    /* Test inputs */
    const char* test_inputs[] = {
        "Hello, world!",
        "What is the meaning of life?",
        "EMBODIOS AI test",
        "1234567890",
        "The quick brown fox jumps over the lazy dog"
    };
    
    char output_buffer[256];
    
    for (int i = 0; i < 5; i++) {
        console_printf("\nTest %d: '%s'\n", i + 1, test_inputs[i]);
        
        if (inference_run(test_inputs[i], output_buffer, sizeof(output_buffer)) == 0) {
            console_printf("%s\n", output_buffer);
        } else {
            console_printf("Inference failed\n");
        }
    }
    
    console_printf("\n");
    inference_stats();
}