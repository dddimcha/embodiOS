/* Minimal C++ runtime for llama.cpp in bare-metal kernel
 *
 * This file provides ONLY C++ specific functionality:
 * - new/delete operators (mapped to kmalloc/kfree)
 * - pthread stubs (single-threaded kernel)
 *
 * All C library functions are provided by lib/compat_stubs.c
 */

#include "../include/embodios/types.h"

extern "C" {
    void* kmalloc(size_t size);
    void kfree(void* ptr);
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

/* Placement new - no allocation, just return pointer */
void* operator new(size_t, void* ptr) noexcept {
    return ptr;
}

void* operator new[](size_t, void* ptr) noexcept {
    return ptr;
}

/* ========================================================================
 * pthread stubs (single-threaded kernel)
 * ======================================================================== */

typedef struct { int unused; } pthread_mutex_t;
typedef struct { int unused; } pthread_mutexattr_t;
typedef struct { int unused; } pthread_t;
typedef struct { int unused; } pthread_attr_t;
typedef struct { int unused; } pthread_cond_t;
typedef struct { int unused; } pthread_condattr_t;
typedef struct { int unused; } pthread_rwlock_t;
typedef struct { int unused; } pthread_rwlockattr_t;
typedef unsigned long pthread_key_t;

extern "C" {

/* Mutex functions - no-ops in single-threaded kernel */
int pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*) {
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t*) {
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t*) {
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t*) {
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t*) {
    return 0;
}

/* Condition variable functions - no-ops */
int pthread_cond_init(pthread_cond_t*, const pthread_condattr_t*) {
    return 0;
}

int pthread_cond_destroy(pthread_cond_t*) {
    return 0;
}

int pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*) {
    return 0;
}

int pthread_cond_signal(pthread_cond_t*) {
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t*) {
    return 0;
}

/* Read-write lock functions - no-ops */
int pthread_rwlock_init(pthread_rwlock_t*, const pthread_rwlockattr_t*) {
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t*) {
    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t*) {
    return 0;
}

int pthread_rwlock_wrlock(pthread_rwlock_t*) {
    return 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t*) {
    return 0;
}

/* Thread functions - not supported in single-threaded kernel */
int pthread_create(pthread_t*, const pthread_attr_t*, void*(*)(void*), void*) {
    return -1;
}

int pthread_join(pthread_t, void**) {
    return -1;
}

int pthread_detach(pthread_t) {
    return -1;
}

pthread_t pthread_self(void) {
    pthread_t t = {0};
    return t;
}

int pthread_equal(pthread_t, pthread_t) {
    return 1; /* Only one thread, so always equal */
}

/* Thread-local storage - simple static storage for single thread */
static void* tls_values[64] = {nullptr};
static int tls_next_key = 0;

int pthread_key_create(pthread_key_t* key, void (*)(void*)) {
    if (tls_next_key >= 64) return -1;
    *key = tls_next_key++;
    return 0;
}

int pthread_key_delete(pthread_key_t key) {
    if (key >= 64) return -1;
    tls_values[key] = nullptr;
    return 0;
}

void* pthread_getspecific(pthread_key_t key) {
    if (key >= 64) return nullptr;
    return tls_values[key];
}

int pthread_setspecific(pthread_key_t key, const void* value) {
    if (key >= 64) return -1;
    tls_values[key] = (void*)value;
    return 0;
}

/* Once initialization */
typedef int pthread_once_t;

int pthread_once(pthread_once_t* once_control, void (*init_routine)(void)) {
    if (*once_control == 0) {
        *once_control = 1;
        init_routine();
    }
    return 0;
}

} /* extern "C" */
