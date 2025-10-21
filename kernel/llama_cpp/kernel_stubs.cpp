/* Minimal C++ runtime and libc stubs for llama.cpp in kernel
 * Provides just enough to run llama.cpp without full OS
 */

#include "../include/embodios/types.h"

extern "C" {
    void* kmalloc(size_t size);
    void kfree(void* ptr);
    void* memcpy(void* dest, const void* src, size_t n);
    void* memset(void* s, int c, size_t n);
    int strcmp(const char* s1, const char* s2);
    size_t strlen(const char* s);
    char* strstr(const char* haystack, const char* needle);
}

/* ========================================================================
 * C++ Runtime (new/delete operators)
 * ======================================================================== */

void* operator new(size_t size) {
    return kmalloc(size);
}

void* operator new[](size_t size) {
    return kmalloc(size);
}

void operator delete(void* ptr) noexcept {
    kfree(ptr);
}

void operator delete[](void* ptr) noexcept {
    kfree(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
    kfree(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
    kfree(ptr);
}

/* ========================================================================
 * pthread stubs (single-threaded kernel)
 * ======================================================================== */

typedef struct { int unused; } pthread_mutex_t;
typedef struct { int unused; } pthread_mutexattr_t;
typedef struct { int unused; } pthread_t;
typedef struct { int unused; } pthread_attr_t;

extern "C" {

int pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*) {
    return 0; // Success - no-op in single-threaded kernel
}

int pthread_mutex_destroy(pthread_mutex_t*) {
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t*) {
    return 0;  // Always succeeds (no contention)
}

int pthread_mutex_unlock(pthread_mutex_t*) {
    return 0;
}

int pthread_create(pthread_t*, const pthread_attr_t*, void*(*)(void*), void*) {
    return -1; // Not supported - kernel is single-threaded
}

int pthread_join(pthread_t, void**) {
    return -1;
}

} // extern "C"

/* ========================================================================
 * FILE* stubs (memory-mapped, no disk I/O)
 * ======================================================================== */

struct __FILE {
    uint8_t* data;
    size_t size;
    size_t pos;
    int eof;
    int error;
};

typedef struct __FILE FILE;

// External model data
extern const uint8_t _binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_start[];
extern const uint8_t _binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_end[];

extern "C" {

FILE* fopen(const char* filename, const char* mode) {
    // Check if opening embedded model
    if (filename && (strstr(filename, "tinyllama") || strstr(filename, ".gguf"))) {
        FILE* f = (FILE*)kmalloc(sizeof(FILE));
        if (f) {
            f->data = (uint8_t*)_binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_start;
            f->size = (size_t)(_binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_end -
                              _binary_tinyllama_1_1b_chat_v1_0_Q4_K_M_gguf_start);
            f->pos = 0;
            f->eof = 0;
            f->error = 0;
        }
        return f;
    }
    return nullptr; // Other files not supported
}

int fclose(FILE* stream) {
    if (stream) kfree(stream);
    return 0;
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (!stream || !ptr) return 0;

    size_t bytes_to_read = size * nmemb;
    size_t bytes_available = stream->size - stream->pos;
    size_t bytes_read = (bytes_to_read < bytes_available) ? bytes_to_read : bytes_available;

    memcpy(ptr, stream->data + stream->pos, bytes_read);
    stream->pos += bytes_read;

    if (stream->pos >= stream->size) {
        stream->eof = 1;
    }

    return bytes_read / size;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    (void)ptr; (void)size; (void)nmemb; (void)stream;
    return 0; // Write not supported
}

int fseek(FILE* stream, long offset, int whence) {
    if (!stream) return -1;

    switch (whence) {
        case 0: // SEEK_SET
            stream->pos = (size_t)offset;
            break;
        case 1: // SEEK_CUR
            stream->pos += offset;
            break;
        case 2: // SEEK_END
            stream->pos = stream->size + offset;
            break;
        default:
            return -1;
    }

    stream->eof = 0;
    return 0;
}

long ftell(FILE* stream) {
    return stream ? (long)stream->pos : -1L;
}

int feof(FILE* stream) {
    return stream ? stream->eof : 1;
}

int ferror(FILE* stream) {
    return stream ? stream->error : 1;
}

void clearerr(FILE* stream) {
    if (stream) {
        stream->eof = 0;
        stream->error = 0;
    }
}

} // extern "C"

/* ========================================================================
 * Additional C stdlib stubs
 * ======================================================================== */

extern "C" {

void* calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void* ptr = kmalloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return nullptr;
    }

    // Simple realloc: allocate new, copy, free old
    void* new_ptr = kmalloc(size);
    if (new_ptr && ptr) {
        // We don't know old size, so this is unsafe
        // Real implementation would track sizes
        memcpy(new_ptr, ptr, size);
        kfree(ptr);
    }
    return new_ptr;
}

int fprintf(FILE* stream, const char* format, ...) {
    (void)stream; (void)format;
    return 0; // Printing not supported
}

int printf(const char* format, ...) {
    (void)format;
    return 0;
}

int snprintf(char* str, size_t size, const char* format, ...) {
    (void)str; (void)size; (void)format;
    return 0;
}

void abort(void) {
    while(1) {} // Hang
}

void exit(int status) {
    (void)status;
    while(1) {}
}

} // extern "C"
