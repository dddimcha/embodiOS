/**
 * @file ggml-backend-stubs.c
 * @brief Stub implementations for GGML backend functions
 *
 * In embodiOS, we use a simple kernel memory allocator (kmalloc/kfree)
 * rather than the full GGML backend abstraction layer.
 * These stubs provide minimal implementations to satisfy linker.
 */

#include "../include/ggml.h"
#include "../include/ggml-backend.h"
#include "../include/ggml-alloc.h"

/* External kernel memory functions */
extern void* kmalloc(size_t size);
extern void* kzalloc(size_t size);
extern void kfree(void* ptr);
extern void console_printf(const char* fmt, ...);

#define kprintf console_printf

/* ============================================================================
 * Simple kernel-based backend buffer
 * ============================================================================ */

/* Simple buffer structure for kernel memory */
struct ggml_backend_buffer {
    void* data;
    size_t size;
    int in_use;
};

/* Simple buffer type */
struct ggml_backend_buffer_type {
    const char* name;
    size_t alignment;
    size_t max_size;
};

/* Default buffer type */
static struct ggml_backend_buffer_type kernel_buffer_type = {
    .name = "kernel",
    .alignment = 32,
    .max_size = 1024UL * 1024 * 1024  /* 1GB max */
};

/* Buffer pool for simple management */
#define MAX_BUFFERS 64
static struct ggml_backend_buffer buffer_pool[MAX_BUFFERS];
static int buffer_pool_init = 0;

static void init_buffer_pool(void) {
    if (!buffer_pool_init) {
        for (int i = 0; i < MAX_BUFFERS; i++) {
            buffer_pool[i].data = NULL;
            buffer_pool[i].size = 0;
            buffer_pool[i].in_use = 0;
        }
        buffer_pool_init = 1;
    }
}

/* ============================================================================
 * Backend buffer type functions
 * ============================================================================ */

const char* ggml_backend_buft_name(ggml_backend_buffer_type_t buft) {
    if (!buft) return "unknown";
    return buft->name;
}

size_t ggml_backend_buft_get_alignment(ggml_backend_buffer_type_t buft) {
    if (!buft) return 32;
    return buft->alignment;
}

size_t ggml_backend_buft_get_max_size(ggml_backend_buffer_type_t buft) {
    if (!buft) return 1024UL * 1024 * 1024;
    return buft->max_size;
}

size_t ggml_backend_buft_get_alloc_size(ggml_backend_buffer_type_t buft, const struct ggml_tensor* tensor) {
    (void)buft;
    if (!tensor) return 0;
    return ggml_nbytes(tensor);
}

ggml_backend_buffer_t ggml_backend_buft_alloc_buffer(ggml_backend_buffer_type_t buft, size_t size) {
    (void)buft;
    init_buffer_pool();

    /* Find free buffer slot */
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (!buffer_pool[i].in_use) {
            void* data = kmalloc(size);
            if (!data) {
                kprintf("GGML: Failed to allocate %zu bytes\n", size);
                return NULL;
            }
            buffer_pool[i].data = data;
            buffer_pool[i].size = size;
            buffer_pool[i].in_use = 1;
            return &buffer_pool[i];
        }
    }
    kprintf("GGML: Buffer pool exhausted\n");
    return NULL;
}

/* ============================================================================
 * Backend buffer functions
 * ============================================================================ */

void ggml_backend_buffer_free(ggml_backend_buffer_t buffer) {
    if (!buffer) return;
    if (buffer->data) {
        kfree(buffer->data);
        buffer->data = NULL;
    }
    buffer->size = 0;
    buffer->in_use = 0;
}

size_t ggml_backend_buffer_get_size(ggml_backend_buffer_t buffer) {
    if (!buffer) return 0;
    return buffer->size;
}

void* ggml_backend_buffer_get_base(ggml_backend_buffer_t buffer) {
    if (!buffer) return NULL;
    return buffer->data;
}

void ggml_backend_buffer_reset(ggml_backend_buffer_t buffer) {
    if (!buffer || !buffer->data) return;
    /* Zero the buffer */
    char* p = (char*)buffer->data;
    for (size_t i = 0; i < buffer->size; i++) {
        p[i] = 0;
    }
}

ggml_backend_buffer_type_t ggml_backend_buffer_get_type(ggml_backend_buffer_t buffer) {
    (void)buffer;
    return &kernel_buffer_type;
}

/* ============================================================================
 * Tensor allocation functions
 * ============================================================================ */

enum ggml_status ggml_backend_tensor_alloc(ggml_backend_buffer_t buffer, struct ggml_tensor* tensor, void* addr) {
    if (!tensor) return GGML_STATUS_FAILED;
    (void)buffer;
    tensor->data = addr;
    return GGML_STATUS_SUCCESS;
}

enum ggml_status ggml_backend_view_init(struct ggml_tensor* tensor) {
    if (!tensor) return GGML_STATUS_FAILED;
    /* For views, data pointer is already set via src */
    if (tensor->view_src && tensor->view_src->data) {
        tensor->data = (char*)tensor->view_src->data + tensor->view_offs;
    }
    return GGML_STATUS_SUCCESS;
}

/* ============================================================================
 * Multi-buffer allocation (stub)
 * ============================================================================ */

ggml_backend_buffer_t ggml_backend_multi_buffer_alloc_buffer(
    ggml_backend_buffer_t* buffers, size_t n_buffers) {
    (void)buffers;
    (void)n_buffers;
    /* For simplicity, just return first buffer or allocate new one */
    if (n_buffers > 0 && buffers && buffers[0]) {
        return buffers[0];
    }
    return NULL;
}

/* ============================================================================
 * Backend functions
 * ============================================================================ */

ggml_backend_buffer_type_t ggml_backend_get_default_buffer_type(ggml_backend_t backend) {
    (void)backend;
    return &kernel_buffer_type;
}

/* ============================================================================
 * Additional stubs that may be needed
 * ============================================================================ */

bool ggml_backend_supports_op(ggml_backend_t backend, const struct ggml_tensor* op) {
    (void)backend;
    (void)op;
    return true;  /* CPU backend supports all ops */
}

bool ggml_backend_supports_buft(ggml_backend_t backend, ggml_backend_buffer_type_t buft) {
    (void)backend;
    (void)buft;
    return true;
}

ggml_guid_t ggml_backend_guid(ggml_backend_t backend) {
    (void)backend;
    static ggml_guid guid = {0};  /* ggml_guid is uint8_t[16] */
    return &guid;
}

const char* ggml_backend_name(ggml_backend_t backend) {
    (void)backend;
    return "embodios-cpu";
}

void ggml_backend_free(ggml_backend_t backend) {
    (void)backend;
    /* Nothing to free for kernel backend */
}

/* ============================================================================
 * Critical section stubs (single-threaded kernel)
 * ============================================================================ */

void ggml_critical_section_start(void) {
    /* Single-threaded kernel - no locking needed */
}

void ggml_critical_section_end(void) {
    /* Single-threaded kernel - no locking needed */
}

/* ============================================================================
 * Additional backend buffer functions
 * ============================================================================ */

void ggml_backend_buffer_set_usage(ggml_backend_buffer_t buffer, enum ggml_backend_buffer_usage usage) {
    (void)buffer;
    (void)usage;
    /* Usage hint - ignored in kernel implementation */
}

size_t ggml_backend_buffer_get_alignment(ggml_backend_buffer_t buffer) {
    (void)buffer;
    return 32;  /* 32-byte alignment for SIMD */
}

size_t ggml_backend_buffer_get_alloc_size(ggml_backend_buffer_t buffer, const struct ggml_tensor* tensor) {
    (void)buffer;
    if (!tensor) return 0;
    return ggml_nbytes(tensor);
}

/* ============================================================================
 * Tensor data operations
 * ============================================================================ */

void ggml_backend_tensor_memset(struct ggml_tensor* tensor, uint8_t value, size_t offset, size_t size) {
    if (!tensor || !tensor->data) return;
    uint8_t* p = (uint8_t*)tensor->data + offset;
    for (size_t i = 0; i < size; i++) {
        p[i] = value;
    }
}

void ggml_backend_tensor_set(struct ggml_tensor* tensor, const void* data, size_t offset, size_t size) {
    if (!tensor || !tensor->data || !data) return;
    char* dst = (char*)tensor->data + offset;
    const char* src = (const char*)data;
    for (size_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
}

void ggml_backend_tensor_get(const struct ggml_tensor* tensor, void* data, size_t offset, size_t size) {
    if (!tensor || !tensor->data || !data) return;
    const char* src = (const char*)tensor->data + offset;
    char* dst = (char*)data;
    for (size_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
}

/* ============================================================================
 * kpanic alias for GGML abort
 * ============================================================================ */

/* GGML uses kpanic for assertions - redirect to kernel panic */
void kpanic(const char* msg) __attribute__((noreturn));
void kpanic(const char* msg) {
    kprintf("GGML PANIC: %s\n", msg);
    /* Hang the system */
    while (1) {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("hlt");
#elif defined(__aarch64__)
        __asm__ volatile("wfi");
#else
        /* spin */
#endif
    }
}
