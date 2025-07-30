/* EMBODIOS Main Header File */
#ifndef EMBODIOS_H
#define EMBODIOS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version information */
#define EMBODIOS_VERSION_MAJOR 0
#define EMBODIOS_VERSION_MINOR 1
#define EMBODIOS_VERSION_PATCH 0

/* Maximum limits */
#define MAX_TOKENS 2048
#define MAX_COMMAND_LENGTH 256
#define MAX_OUTPUT_LENGTH 4096

/* Hardware token definitions */
#include "tokens.h"

/* HAL definitions */
#include "hal.h"

/* Memory management */
void memory_init(void);
void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t count, size_t size);
void* realloc(void* ptr, size_t new_size);
void* page_alloc(size_t num_pages);
void page_free(void* ptr);
void memory_stats(size_t* allocated, size_t* free_mem, size_t* total);

/* String operations */
void* memcpy(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);
int memcmp(const void* s1, const void* s2, size_t n);
size_t strlen(const char* s);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
char* strstr(const char* haystack, const char* needle);

/* Model state */
typedef struct {
    void* weights;
    size_t weight_size;
    void* scratch_buffer;
    size_t scratch_size;
    uint32_t hidden_size;
    uint32_t vocab_size;
    uint32_t num_layers;
} model_state_t;

/* Inference result */
typedef struct {
    int32_t* tokens;
    size_t length;
    float* logits;
    float confidence;
} inference_result_t;

/* Model functions */
model_state_t* model_init(void);
void model_cleanup(model_state_t* model);
inference_result_t model_inference(model_state_t* model, 
                                 const int32_t* input_tokens, 
                                 size_t input_length,
                                 int32_t* output_tokens);

/* Natural language processing */
hardware_command_t process_command(const char* input);
void command_to_tokens(const hardware_command_t* cmd, int32_t* tokens, size_t* len);
void text_to_tokens(const char* text, int32_t* tokens, size_t* len);
void tokens_to_text(const int32_t* tokens, size_t len, char* output);

/* AI output processing */
void process_ai_output(hal_state_t* hal, const int32_t* tokens, size_t len);

/* Kernel functions */
int kernel_init(void);
void kernel_main_loop(void);
void kernel_shutdown(void);
void kernel_panic(const char* message);

/* Interrupt handling */
typedef void (*interrupt_handler_t)(void);
void interrupt_init(void);
void interrupt_register(uint8_t irq, interrupt_handler_t handler);
void interrupt_enable(uint8_t irq);
void interrupt_disable(uint8_t irq);

/* Debug output */
void debug_print(const char* format, ...);
void debug_hexdump(const void* data, size_t len);

/* System information */
typedef struct {
    uint64_t cpu_features;
    uint32_t cpu_cores;
    uint64_t memory_total;
    uint64_t memory_available;
    char cpu_vendor[13];
    char cpu_model[49];
} system_info_t;

void get_system_info(system_info_t* info);

/* Performance monitoring */
typedef struct {
    uint64_t inference_count;
    uint64_t total_inference_time;
    uint64_t command_count;
    uint64_t total_command_time;
    uint64_t memory_allocated;
    uint64_t memory_freed;
} performance_stats_t;

void performance_init(void);
void performance_start_timer(void);
uint64_t performance_end_timer(void);
void performance_get_stats(performance_stats_t* stats);

/* CPU feature flags */
#define CPU_FEATURE_SSE     (1ULL << 0)
#define CPU_FEATURE_SSE2    (1ULL << 1)
#define CPU_FEATURE_SSE3    (1ULL << 2)
#define CPU_FEATURE_SSSE3   (1ULL << 3)
#define CPU_FEATURE_SSE4_1  (1ULL << 4)
#define CPU_FEATURE_SSE4_2  (1ULL << 5)
#define CPU_FEATURE_AVX     (1ULL << 6)
#define CPU_FEATURE_AVX2    (1ULL << 7)
#define CPU_FEATURE_AVX512F (1ULL << 8)
#define CPU_FEATURE_FMA     (1ULL << 9)

/* Error codes */
#define EMBODIOS_SUCCESS     0
#define EMBODIOS_ERROR      -1
#define EMBODIOS_ENOMEM     -2
#define EMBODIOS_EINVAL     -3
#define EMBODIOS_ENODEV     -4
#define EMBODIOS_ETIMEOUT   -5

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_H */