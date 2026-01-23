/* Temporary stub implementations for missing functions */

#include "embodios/ai.h"
#include "embodios/block.h"
#include "embodios/bpe_tokenizer.h"
#include "embodios/console.h"
#include "embodios/cpu.h"
#include "embodios/dma.h"
#include "embodios/gguf_parser.h"
#include "embodios/interrupt.h"
#include "embodios/kernel.h"
#include "embodios/mm.h"
#include "embodios/model_registry.h"
#include "embodios/pci.h"
#include "embodios/task.h"
#include "embodios/tvm.h"
#include "embodios/types.h"
#include "embodios/virtio_blk.h"

/* String function declarations */
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
size_t strlen(const char *s);

/* External function declarations */
void arch_reboot(void);
void pmm_print_stats(void);
void heap_stats(void);

/* Helper to parse integer from string */
static int parse_int(const char *s)
{
    int result = 0;
    int sign = 1;

    while (*s == ' ')
        s++; /* Skip whitespace */

    if (*s == '-') {
        sign = -1;
        s++;
    }

    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }

    return result * sign;
}

/* Command processor implementation */
void command_processor_init(struct embodios_model *model)
{
    if (model) {
        console_printf("Command processor initialized with model: %s\n", model->name);
    } else {
        console_printf("Command processor initialized without AI model\n");
    }
}

/* Enhanced command processing */
void process_command(const char *command)
{
    /* Basic built-in commands */
    if (strcmp(command, "help") == 0) {
        console_printf("\nEMBODIOS Commands:\n");
        console_printf("================\n\n");
        console_printf("AI:\n");
        console_printf("  chat <message>  - Chat with the AI model\n");
        console_printf("  benchmark       - Run inference benchmark\n");
        console_printf("\n");
        console_printf("System:\n");
        console_printf("  help            - Show this help\n");
        console_printf("  mem             - Show memory info\n");
        console_printf("  heap            - Show heap stats\n");
        console_printf("  lspci           - List PCI devices\n");
        console_printf("  reboot          - Reboot system\n");
        console_printf("\n");
        console_printf("Type 'help advanced' for all commands.\n");
    } else if (strcmp(command, "help advanced") == 0) {
        console_printf("\nAdvanced Commands:\n");
        console_printf("==================\n\n");
        console_printf("Model Management:\n");
        console_printf("  models, model, model load/switch/unload\n");
        console_printf("\n");
        console_printf("AI Inference (legacy):\n");
        console_printf("  ai, infer, gguf, stream, ggufinit, streaminit, bpeinit, bpetest\n");
        console_printf("\n");
        console_printf("Hardware:\n");
        console_printf("  dmatest, dmastats, pcitest, pcistats\n");
        console_printf("\n");
        console_printf("Storage:\n");
        console_printf("  blkinfo, blktest, blkperf, blkread, blkdevs, loadmodel, loadtiny\n");
        console_printf("\n");
        console_printf("Network:\n");
        console_printf("  net, netinfo, nettest, ping <ip>\n");
        console_printf("\n");
        console_printf("Testing:\n");
        console_printf("  locktest, quanttest, quantbench, benchgguf, validate\n");
    } else if (strncmp(command, "chat ", 5) == 0) {
        /* Simple unified chat command - auto-initializes everything */
        extern int streaming_inference_init(void);
        extern bool streaming_inference_is_ready(void);
        extern int streaming_inference_generate(const int *, int, int *, int);
        extern const char *streaming_inference_get_token(int);
        extern const uint8_t *get_embedded_gguf_model(size_t *out_size);
        extern int gguf_load_model(void *data, size_t size);
        extern int gguf_model_embedded(void);
        extern const struct gguf_model_arch *gguf_parser_get_arch(void);

        const char *prompt = command + 5;
        while (*prompt == ' ') prompt++; /* Skip whitespace */

        if (*prompt == '\0') {
            console_printf("Usage: chat <your message>\n");
            console_printf("Example: chat Hello, how are you?\n");
            return;
        }

        /* Auto-initialize: Load model if needed */
        if (!gguf_parser_get_arch()) {
            if (!gguf_model_embedded()) {
                console_printf("Error: No AI model available\n");
                return;
            }
            size_t gguf_size = 0;
            const uint8_t *gguf_data = get_embedded_gguf_model(&gguf_size);
            if (!gguf_data || gguf_size == 0) {
                console_printf("Error: Failed to get model data\n");
                return;
            }
            console_printf("Loading model...\n");
            if (gguf_load_model((void *)gguf_data, gguf_size) < 0) {
                console_printf("Error: Model load failed\n");
                return;
            }
        }

        /* Auto-initialize: BPE tokenizer */
        if (!bpe_tokenizer_is_initialized()) {
            bpe_tokenizer_init();
        }

        /* Auto-initialize: Inference engine */
        if (!streaming_inference_is_ready()) {
            if (streaming_inference_init() != 0) {
                console_printf("Error: Inference init failed\n");
                return;
            }
        }

        /* Tokenize */
        int prompt_tokens[256];
        int prompt_len = 0;
        if (bpe_tokenizer_is_initialized()) {
            prompt_len = bpe_tokenizer_encode(prompt, prompt_tokens, 256, false, false);
        }
        if (prompt_len <= 0) {
            prompt_tokens[0] = 1; /* BOS fallback */
            prompt_len = 1;
        }

        console_printf("\nYou: %s\n", prompt);
        console_printf("AI: ");

        /* Generate */
        int output_tokens[128];
        int generated = streaming_inference_generate(prompt_tokens, prompt_len, output_tokens, 50);

        if (generated > 0) {
            /* Decode tokens to readable text */
            char decoded[512];
            int len = bpe_tokenizer_decode(output_tokens, generated, decoded, sizeof(decoded));
            if (len > 0) {
                console_printf("%s", decoded);
            } else {
                /* Fallback to raw tokens */
                for (int i = 0; i < generated; i++) {
                    const char *tok = streaming_inference_get_token(output_tokens[i]);
                    if (tok) console_printf("%s", tok);
                }
            }
            console_printf("\n\n");
        } else {
            console_printf("(no response)\n\n");
        }
    } else if (strncmp(command, "ai ", 3) == 0) {
        /* TinyStories interactive inference */
        const char *prompt = command + 3;

        extern int tinystories_infer(const char *prompt, char *output, size_t max_len);
        extern bool tinystories_is_loaded(void);

        if (!tinystories_is_loaded()) {
            console_printf("ERROR: TinyStories model not loaded!\n");
            return;
        }

        char output[512];
        console_printf("\nGenerating text (this may take a while)...\n");
        int result = tinystories_infer(prompt, output, sizeof(output));

        if (result > 0) {
            console_printf("\nGenerated: %s\n\n", output);
        } else {
            console_printf("ERROR: Inference failed\n");
        }
    } else if (strcmp(command, "mem") == 0) {
        /* Show PMM stats */
        pmm_print_stats();
        console_printf("\n");
        /* Show heap stats */
        heap_stats();
    } else if (strcmp(command, "heap") == 0) {
        heap_stats();
    } else if (strcmp(command, "tasks") == 0) {
        console_printf("Task scheduler not fully implemented\n");
    } else if (strcmp(command, "models") == 0) {
        /* List all loaded models */
        model_registry_print_status();
    } else if (strcmp(command, "model") == 0) {
        /* Show active model info */
        struct embodios_model *model = model_registry_get_active();
        if (model) {
            int id = model_registry_get_active_id();
            console_printf("Active model [%d]: %s\n", id, model->name);
            console_printf("  Architecture: %s\n", model->arch);
            console_printf("  Parameters: %zu\n", model->param_count);
            console_printf("  Version: %u.%u\n", model->version_major, model->version_minor);
        } else {
            console_printf("No active model\n");
            console_printf("Use 'model load <name>' to load a model\n");
        }
    } else if (strncmp(command, "model load ", 11) == 0) {
        /* Load embedded model by name */
        const char *name = command + 11;
        while (*name == ' ')
            name++; /* Skip whitespace */

        if (*name == '\0') {
            console_printf("Usage: model load <name>\n");
            console_printf("Available: tinystories\n");
            return;
        }

        int result = model_registry_load_embedded(name);
        if (result >= 0) {
            console_printf("Model loaded successfully with ID %d\n", result);
        } else {
            console_printf("Failed to load model: %s\n", model_registry_strerror(result));
        }
    } else if (strncmp(command, "model switch ", 13) == 0) {
        /* Switch to model by ID */
        const char *id_str = command + 13;
        int model_id = parse_int(id_str);

        int result = model_registry_switch(model_id);
        if (result == 0) {
            struct embodios_model *model = model_registry_get_active();
            console_printf("Switched to model %d: %s\n", model_id,
                           model ? model->name : "(unknown)");
        } else {
            console_printf("Failed to switch: %s\n", model_registry_strerror(result));
        }
    } else if (strncmp(command, "model unload ", 13) == 0) {
        /* Unload model by ID */
        const char *id_str = command + 13;
        int model_id = parse_int(id_str);

        int result = model_registry_unload(model_id);
        if (result == 0) {
            console_printf("Model %d unloaded\n", model_id);
        } else {
            console_printf("Failed to unload: %s\n", model_registry_strerror(result));
        }
    } else if (strncmp(command, "infer ", 6) == 0) {
        const char *input = command + 6;

        /* Use real TinyLlama inference with pattern-based responses */
        char response[512];
        extern int real_tinyllama_inference(const char *prompt, char *response,
                                            size_t max_response);

        int result = real_tinyllama_inference(input, response, sizeof(response));

        if (result > 0) {
            console_printf("TinyLlama> %s\n", response);
        } else {
            /* Fallback only if real inference completely fails */
            console_printf("TinyLlama> I'm running in EMBODIOS kernel space. Model inference not "
                           "yet fully implemented.\n");
        }
    } else if (strcmp(command, "tinystories") == 0) {
        extern void tinystories_test(void);
        tinystories_test();
    } else if (strcmp(command, "tvm") == 0) {
        tvm_runtime_stats();
    } else if (strcmp(command, "dmatest") == 0) {
        dma_run_tests();
    } else if (strcmp(command, "dmastats") == 0) {
        dma_print_stats();
        dma_dump_allocations();
    } else if (strcmp(command, "lspci") == 0) {
        pci_print_devices();
    } else if (strcmp(command, "pcitest") == 0) {
        pci_run_tests();
    } else if (strcmp(command, "pcistats") == 0) {
        pci_print_stats();
    } else if (strcmp(command, "blkinfo") == 0) {
        virtio_blk_info();
    } else if (strcmp(command, "blktest") == 0) {
        virtio_blk_test();
    } else if (strcmp(command, "blkperf") == 0) {
        virtio_blk_perf_test();
    } else if (strncmp(command, "blkread ", 8) == 0) {
        /* Parse: blkread <sector> [count] */
        const char *args = command + 8;
        uint64_t sector = 0;
        uint32_t count = 1;

        /* Parse sector number */
        while (*args >= '0' && *args <= '9') {
            sector = sector * 10 + (*args - '0');
            args++;
        }

        /* Skip whitespace and parse optional count */
        while (*args == ' ')
            args++;
        if (*args >= '0' && *args <= '9') {
            count = 0;
            while (*args >= '0' && *args <= '9') {
                count = count * 10 + (*args - '0');
                args++;
            }
        }

        virtio_blk_read_cmd(sector, count);
    } else if (strcmp(command, "blkdevs") == 0) {
        block_print_devices();
    } else if (strncmp(command, "loadext ", 8) == 0) {
        /* Load GGUF model from external memory address (e.g., QEMU loader device) */
        extern int gguf_load_model(void *data, size_t size);

        const char *addr_str = command + 8;
        unsigned long long addr = 0;
        size_t model_size = 104857600; /* Default 100MB - GGUF files self-describe size */

        /* Parse hex address */
        while (*addr_str == ' ') addr_str++;
        if (addr_str[0] == '0' && (addr_str[1] == 'x' || addr_str[1] == 'X')) {
            addr_str += 2;
        }
        while (*addr_str) {
            char c = *addr_str++;
            if (c >= '0' && c <= '9') addr = (addr << 4) | (c - '0');
            else if (c >= 'a' && c <= 'f') addr = (addr << 4) | (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') addr = (addr << 4) | (c - 'A' + 10);
            else break;
        }

        if (addr == 0) {
            console_printf("Usage: loadext <hex_address>\n");
            console_printf("Example: loadext 0x10000000\n");
            return;
        }

        console_printf("Loading GGUF model from address 0x%llx...\n", addr);
        int ret = gguf_load_model((void *)(uintptr_t)addr, model_size);
        if (ret == 0) {
            console_printf("Model loaded successfully!\n");
            gguf_parser_print_summary();

            /* Initialize BPE tokenizer from loaded model */
            console_printf("\nInitializing tokenizer...\n");
            if (bpe_tokenizer_init() == 0) {
                console_printf("Tokenizer ready.\n");
            }
        } else {
            console_printf("Failed to load model (error %d)\n", ret);
        }
    } else if (strcmp(command, "loadmodel") == 0) {
        /* Load GGUF model from first VirtIO block device */
        block_device_t *dev = block_get_device_by_index(0);
        if (!dev) {
            console_printf("ERROR: No block device available\n");
            console_printf("Make sure QEMU is started with a VirtIO disk\n");
            return;
        }

        console_printf("Loading GGUF model from %s...\n", dev->name);
        int ret = gguf_load_from_block(dev, 0, 0); /* Auto-detect size */
        if (ret == 0) {
            console_printf("Model loaded successfully!\n");
            gguf_parser_print_summary();

            /* Initialize BPE tokenizer from loaded model */
            console_printf("\nInitializing tokenizer...\n");
            if (bpe_tokenizer_init() == 0) {
                console_printf("Tokenizer ready.\n");
            }
        } else {
            console_printf("Failed to load model (error %d)\n", ret);
        }
    } else if (strcmp(command, "loadtiny") == 0) {
        /* Load TinyStories model from VirtIO disk */
        extern int tinystories_load_from_disk(void);
        int ret = tinystories_load_from_disk();
        if (ret == 0) {
            console_printf("TinyStories model ready for inference!\n");
            console_printf("Use 'ai <prompt>' to generate text.\n");
        } else {
            console_printf("Failed to load TinyStories model (error %d)\n", ret);
        }
    } else if (strcmp(command, "blkstats") == 0) {
        virtio_blk_print_stats();
    } else if (strcmp(command, "locktest") == 0) {
        extern int lock_run_tests(void);
        lock_run_tests();
    } else if (strcmp(command, "quanttest") == 0) {
        extern int run_quantized_tests(void);
        run_quantized_tests();
    } else if (strcmp(command, "quantbench") == 0) {
        extern int run_quantized_benchmarks(void);
        run_quantized_benchmarks();
    } else if (strcmp(command, "bpeinit") == 0) {
        if (bpe_tokenizer_is_initialized()) {
            console_printf("BPE tokenizer already initialized\n");
        } else {
            int result = bpe_tokenizer_init();
            if (result == 0) {
                console_printf("BPE tokenizer initialized successfully\n");
            } else {
                console_printf("BPE tokenizer initialization failed\n");
            }
        }
    } else if (strcmp(command, "bpetest") == 0) {
        if (!bpe_tokenizer_is_initialized()) {
            console_printf("BPE tokenizer not initialized. Run 'bpeinit' first.\n");
        } else {
            bpe_tokenizer_test();
        }
    } else if (strcmp(command, "ggufinit") == 0) {
        /* Initialize GGUF inference engine */
        extern int gguf_inference_init(void);
        extern bool gguf_inference_is_ready(void);

        if (gguf_inference_is_ready()) {
            console_printf("GGUF inference engine already initialized\n");
        } else {
            int result = gguf_inference_init();
            if (result == 0) {
                console_printf("GGUF inference engine initialized successfully\n");
            } else {
                console_printf("Failed to initialize GGUF inference engine\n");
            }
        }
    } else if (strncmp(command, "gguf ", 5) == 0) {
        /* GGUF model inference */
        extern int gguf_inference_init(void);
        extern bool gguf_inference_is_ready(void);
        extern int gguf_inference_generate(const int *, int, int *, int);
        extern const char *gguf_inference_get_token(int);
        extern const uint8_t *get_embedded_gguf_model(size_t *out_size);
        extern int gguf_load_model(void *data, size_t size);
        extern int gguf_model_embedded(void);
        extern const struct gguf_model_arch *gguf_parser_get_arch(void);

        const char *prompt = command + 5;

        /* Step 1: Load GGUF model if not already loaded */
        if (!gguf_parser_get_arch()) {
            if (!gguf_model_embedded()) {
                console_printf("ERROR: No GGUF model embedded in kernel\n");
                return;
            }
            size_t gguf_size = 0;
            const uint8_t *gguf_data = get_embedded_gguf_model(&gguf_size);
            if (!gguf_data || gguf_size == 0) {
                console_printf("ERROR: Failed to get embedded GGUF model\n");
                return;
            }
            console_printf("Loading GGUF model (%zu MB)...\n", gguf_size / (1024 * 1024));
            if (gguf_load_model((void *)gguf_data, gguf_size) < 0) {
                console_printf("ERROR: Failed to parse GGUF model\n");
                return;
            }
            console_printf("GGUF model loaded\n");
        }

        /* Step 2: Initialize BPE tokenizer if not ready */
        if (!bpe_tokenizer_is_initialized()) {
            console_printf("Initializing BPE tokenizer...\n");
            if (bpe_tokenizer_init() != 0) {
                console_printf("WARNING: BPE tokenizer init failed\n");
            }
        }

        /* Step 3: Initialize inference engine if not ready */
        if (!gguf_inference_is_ready()) {
            console_printf("Initializing GGUF inference engine...\n");
            if (gguf_inference_init() != 0) {
                console_printf("ERROR: Failed to initialize GGUF inference\n");
                return;
            }
        }

        console_printf("\nGenerating with GGUF model...\n");
        console_printf("Prompt: \"%s\"\n", prompt);

        /* Simple tokenization - use BPE if available */
        int prompt_tokens[256];
        int prompt_len = 0;

        if (bpe_tokenizer_is_initialized()) {
            console_printf("Tokenizing with BPE...\n");
            prompt_len = bpe_tokenizer_encode(prompt, prompt_tokens, 256, false, false);
            console_printf("Tokenized: %d tokens\n", prompt_len);
            if (prompt_len <= 0) {
                console_printf("ERROR: Failed to tokenize prompt\n");
                return;
            }
            /* Print tokens for debug */
            console_printf("Token IDs: ");
            for (int i = 0; i < prompt_len && i < 10; i++) {
                console_printf("%d ", prompt_tokens[i]);
            }
            console_printf("\n");
        } else {
            /* Fallback: BOS token + simple char-to-token (not ideal) */
            prompt_tokens[0] = 1; /* BOS */
            prompt_len = 1;
            console_printf("WARNING: BPE not initialized, using BOS only\n");
        }

        /* Generate */
        console_printf("Starting inference...\n");
        int output_tokens[128];
        int generated = gguf_inference_generate(prompt_tokens, prompt_len, output_tokens, 50);
        console_printf("Inference complete, generated=%d\n", generated);

        if (generated > 0) {
            console_printf("\nGenerated %d tokens:\n", generated);
            for (int i = 0; i < generated; i++) {
                const char *tok = gguf_inference_get_token(output_tokens[i]);
                if (tok) {
                    console_printf("%s", tok);
                }
            }
            console_printf("\n");
        } else {
            console_printf("ERROR: Generation failed\n");
        }
    } else if (strcmp(command, "streaminit") == 0) {
        /* Initialize streaming inference engine (memory-efficient) */
        extern int streaming_inference_init(void);
        extern bool streaming_inference_is_ready(void);

        if (streaming_inference_is_ready()) {
            console_printf("Streaming inference engine already initialized\n");
        } else {
            console_printf("Initializing streaming inference (on-the-fly dequant)...\n");
            int result = streaming_inference_init();
            if (result == 0) {
                console_printf("Streaming inference engine initialized successfully\n");
            } else {
                console_printf("Failed to initialize streaming inference engine\n");
            }
        }
    } else if (strncmp(command, "stream ", 7) == 0) {
        /* Streaming inference (memory-efficient) */
        extern int streaming_inference_init(void);
        extern bool streaming_inference_is_ready(void);
        extern int streaming_inference_generate(const int *, int, int *, int);
        extern const char *streaming_inference_get_token(int);

        const char *prompt = command + 7;

        if (!streaming_inference_is_ready()) {
            console_printf("Initializing streaming inference engine...\n");
            if (streaming_inference_init() != 0) {
                console_printf("ERROR: Failed to initialize streaming inference\n");
                return;
            }
        }

        console_printf("\nGenerating with streaming inference...\n");
        console_printf("Prompt: \"%s\"\n", prompt);

        /* Tokenization */
        int prompt_tokens[256];
        int prompt_len = 0;

        if (bpe_tokenizer_is_initialized()) {
            console_printf("Tokenizing with BPE...\n");
            prompt_len = bpe_tokenizer_encode(prompt, prompt_tokens, 256, false, false);
            console_printf("Tokenized: %d tokens\n", prompt_len);
            /* DEBUG: Print token IDs */
            console_printf("Token IDs: ");
            for (int i = 0; i < prompt_len && i < 16; i++) {
                console_printf("%d ", prompt_tokens[i]);
            }
            console_printf("\n");
            if (prompt_len <= 0) {
                console_printf("ERROR: Failed to tokenize prompt\n");
                return;
            }
        } else {
            prompt_tokens[0] = 1; /* BOS */
            prompt_len = 1;
            console_printf("WARNING: BPE not initialized, using BOS only\n");
        }

        /* Generate */
        console_printf("Starting streaming inference...\n");
        int output_tokens[128];
        int generated = streaming_inference_generate(prompt_tokens, prompt_len, output_tokens, 50);
        console_printf("Generation complete, generated=%d\n", generated);

        if (generated > 0) {
            console_printf("\nGenerated %d tokens:\n", generated);
            for (int i = 0; i < generated; i++) {
                const char *tok = streaming_inference_get_token(output_tokens[i]);
                if (tok) {
                    console_printf("%s", tok);
                }
            }
            console_printf("\n");
        } else {
            console_printf("ERROR: Streaming generation failed\n");
        }
    } else if (strcmp(command, "benchmark") == 0) {
        /* Run REAL GGUF inference benchmark (not simulation) */
        extern int benchmark_init(void);
        extern int benchmark_gguf_inference(void *, const char *, int);
        extern int streaming_inference_init(void);
        extern const uint8_t *get_embedded_gguf_model(size_t *out_size);
        extern int gguf_load_model(void *data, size_t size);
        extern int gguf_model_embedded(void);
        extern const struct gguf_model_arch *gguf_parser_get_arch(void);

        benchmark_init();
        console_printf("Initializing REAL inference engine...\n");

        /* Step 1: Check if model already loaded (from VirtIO or embedded) */
        if (!gguf_parser_get_arch()) {
            /* No model loaded yet - try embedded */
            if (!gguf_model_embedded()) {
                console_printf("ERROR: No model loaded. Use 'loadmodel' first or embed model in kernel.\n");
                console_printf("Falling back to quick system benchmark...\n");
                extern int benchmark_quick_check(void);
                benchmark_quick_check();
                return;
            }

            /* Step 2: Load and parse the embedded GGUF model */
            size_t gguf_size = 0;
            const uint8_t *gguf_data = get_embedded_gguf_model(&gguf_size);
            if (!gguf_data || gguf_size == 0) {
                console_printf("ERROR: Failed to get embedded GGUF model\n");
                return;
            }
            console_printf("Loading embedded GGUF model (%zu MB)...\n", gguf_size / (1024 * 1024));

            int load_result = gguf_load_model((void *)gguf_data, gguf_size);
            if (load_result < 0) {
                console_printf("ERROR: Failed to parse GGUF model: %d\n", load_result);
                return;
            }
            console_printf("GGUF model parsed successfully\n");
        } else {
            console_printf("Using already loaded model\n");
        }

        /* Step 3: Initialize streaming inference engine */
        console_printf("[BENCH] Calling streaming_inference_init...\n");
        console_flush();
        int init_result = streaming_inference_init();
        console_printf("[BENCH] streaming_inference_init returned %d\n", init_result);
        console_flush();
        if (init_result != 0) {
            console_printf("ERROR: Failed to init inference engine: %d\n", init_result);
            console_printf("Falling back to quick system benchmark...\n");
            extern int benchmark_quick_check(void);
            benchmark_quick_check();
        } else {
            /* Step 4: Run REAL inference benchmark */
            console_printf("[BENCH] Init OK, allocating result buffer...\n");
            console_flush();
            void *result = kmalloc(256); /* inference_benchmark_t */
            console_printf("[BENCH] result=%p\n", result);
            console_flush();
            if (result) {
                console_printf("Running REAL inference (20 tokens)...\n");
                console_flush();
                benchmark_gguf_inference(result, "Once upon a time", 20);
                console_printf("[BENCH] benchmark_gguf_inference done\n");
                console_flush();
                kfree(result);
            } else {
                console_printf("[BENCH] Failed to allocate result buffer!\n");
            }
        }
    } else if (strcmp(command, "benchgguf") == 0) {
        /* Run GGUF inference benchmark */
        extern int benchmark_init(void);
        extern int benchmark_gguf_inference(void *, const char *, int);

        benchmark_init();

        /* Use a standard test prompt */
        void *result = kmalloc(256); /* inference_benchmark_t */
        if (result) {
            benchmark_gguf_inference(result, "Once upon a time there was", 100);
            kfree(result);
        }
    } else if (strcmp(command, "validate") == 0) {
        /* Validate model meets performance targets */
        extern int benchmark_init(void);
        extern int benchmark_validate_gguf_model(const char *);

        benchmark_init();

        /* Get model name from GGUF parser if available */
        extern const char *gguf_get_model_name(void);
        const char *model_name = gguf_get_model_name();
        if (!model_name) {
            model_name = "Embedded GGUF Model";
        }

        int passed = benchmark_validate_gguf_model(model_name);
        console_printf("\nValidation complete: %d tests passed\n", passed);
    } else if (strcmp(command, "net") == 0 || strcmp(command, "netinfo") == 0) {
        /* Show network information */
        extern void tcpip_print_info(void);
        extern void virtio_net_print_info(void);
        extern bool virtio_net_is_ready(void);

        if (virtio_net_is_ready()) {
            virtio_net_print_info();
        } else {
            console_printf("VirtIO-Net: Not available\n");
        }
        tcpip_print_info();
    } else if (strcmp(command, "nettest") == 0) {
        /* Run network self-tests */
        extern int tcpip_run_tests(void);
        extern int virtio_net_run_tests(void);
        extern bool virtio_net_is_ready(void);

        if (virtio_net_is_ready()) {
            virtio_net_run_tests();
        }
        tcpip_run_tests();
    } else if (strcmp(command, "tcpserver") == 0 || strcmp(command, "server") == 0) {
        /* Start TCP echo server */
        extern int tcpip_start_server(uint16_t port);
        console_printf("Starting TCP echo server on port 80...\n");
        tcpip_start_server(80);
    } else if (strncmp(command, "ping ", 5) == 0) {
        /* Ping command */
        extern int tcpip_ping(uint32_t dst_ip, uint16_t id, uint16_t seq);
        extern uint32_t ip_from_string(const char *str);
        extern int tcpip_poll(void);
        #define NET_ERR_UNREACHABLE -5

        const char *ip_str = command + 5;
        while (*ip_str == ' ') ip_str++;

        if (*ip_str == '\0') {
            console_printf("Usage: ping <ip_address>\n");
            console_printf("Example: ping 10.0.2.2\n");
        } else {
            uint32_t dst_ip = ip_from_string(ip_str);
            console_printf("Pinging %s...\n", ip_str);

            for (int i = 0; i < 4; i++) {
                int ret = tcpip_ping(dst_ip, 1, i + 1);
                if (ret == 0) {
                    console_printf("  [%d] ICMP echo request sent\n", i + 1);
                } else if (ret == NET_ERR_UNREACHABLE) {
                    /* ARP request sent, need to wait for reply */
                    console_printf("  [%d] Resolving MAC (ARP)...\n", i + 1);
                    /* Poll for ARP reply */
                    for (int j = 0; j < 500000; j++) {
                        tcpip_poll();
                    }
                    /* Retry sending ping */
                    ret = tcpip_ping(dst_ip, 1, i + 1);
                    if (ret == 0) {
                        console_printf("  [%d] ICMP echo request sent\n", i + 1);
                    } else {
                        console_printf("  [%d] Failed: %d\n", i + 1, ret);
                    }
                } else {
                    console_printf("  [%d] Failed: %d\n", i + 1, ret);
                }
                /* Poll for ICMP echo replies */
                for (int j = 0; j < 200000; j++) {
                    tcpip_poll();
                }
            }
            console_printf("Done (use 'net' to see ICMP statistics)\n");
        }
        #undef NET_ERR_UNREACHABLE
    } else if (strcmp(command, "reboot") == 0) {
        console_printf("Rebooting...\n");
        arch_reboot();
    } else {
        console_printf("Unknown command: %s\n", command);
    }
}

/* Note: transformer_init and transformer_reset_cache are now in transformer_full.c */

int llama_model_load(const uint8_t *data, size_t size)
{
    (void)data;
    (void)size;
    console_printf("llama_model_load: stub implementation\n");
    return -1;
}

int llama_generate(const char *prompt, char *response, size_t max_response)
{
    (void)prompt;
    (void)response;
    (void)max_response;
    console_printf("llama_generate: stub implementation\n");
    return -1;
}

/* External declaration for TinyLlama inference (from tinyllama_gguf_inference.c) */
extern int tinyllama_inference(const char *prompt, char *response, size_t max_response);

/* External declaration for quantized integer-only neural network inference (fallback) */
extern int quantized_neural_inference(const char *prompt, char *response, size_t max_response);

int real_tinyllama_inference(const char *prompt, char *response, size_t max_response)
{
    /* Try TinyLlama GGUF inference first (uses real BPE tokenizer) */
    int result = tinyllama_inference(prompt, response, max_response);

    if (result > 0) {
        return result;
    }

    /* Fallback to quantized inference if TinyLlama fails */
    console_printf("[Inference] TinyLlama failed, using quantized fallback\n");
    return quantized_neural_inference(prompt, response, max_response);
}

/* Basic math function stubs for TinyStories (simplified implementations) */
/* NOTE: Floating-point math functions are disabled because kernel is built with
 * -mno-sse -mno-sse2 flags. The AI inference uses integer-only operations instead.
 * If floating-point is needed in the future, compile these separately or use soft-float.
 */

/* Disabled due to SSE incompatibility with kernel flags
float sqrtf(float x)
{
    if (x <= 0.0f) return 0.0f;

    float guess = x / 2.0f;
    for (int i = 0; i < 10; i++) {
        guess = (guess + x / guess) / 2.0f;
    }
    return guess;
}

float expf(float x)
{
    if (x > 10.0f) return 22026.0f;
    if (x < -10.0f) return 0.000045f;

    float result = 1.0f;
    float term = 1.0f;

    for (int i = 1; i < 20; i++) {
        term *= x / (float)i;
        result += term;

        if (term < 0.0001f && term > -0.0001f) break;
    }

    return result;
}
*/

/* Stubs for GGUF/TVM functions not yet fully implemented (weak, can be overridden) */
__attribute__((weak))
void *gguf_get_tensor(void *ctx, const char *name, size_t *size)
{
    (void)ctx;
    (void)name;
    if (size)
        *size = 0;
    return NULL;
}

__attribute__((weak))
int gguf_get_model_config(void *ctx, void *config)
{
    (void)ctx;
    (void)config;
    return -1;
}

int tinyllama_forward_tvm(void *input, void *output)
{
    (void)input;
    (void)output;
    return -1;
}

int tinyllama_forward(void *input, void *output)
{
    (void)input;
    (void)output;
    return -1;
}
