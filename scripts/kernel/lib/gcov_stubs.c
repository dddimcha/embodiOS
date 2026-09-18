/**
 * @file gcov_stubs.c
 * @brief Bare-metal gcov runtime stubs for kernel coverage instrumentation
 *
 * These stubs allow gcov-instrumented code to link in a bare-metal environment.
 * In a hosted environment, gcov writes .gcda files at program exit. For the
 * kernel, we provide minimal stubs that allow compilation and linking.
 * Actual coverage data extraction will be handled via memory dumps or QEMU.
 */

#include <stdint.h>
#include <stddef.h>

/**
 * gcov_info structure - tracks coverage data for a compilation unit
 * This is a simplified version; the full structure is in gcc/gcov-io.h
 */
struct gcov_info;

/**
 * __gcov_init - Initialize coverage data for a compilation unit
 * Called automatically by compiler-generated code for each .o file
 *
 * @param info: Pointer to coverage data structure
 */
void __gcov_init(struct gcov_info *info) {
    /* Stub: In full implementation, would register info structure
     * for later data collection. For now, we just need to link. */
    (void)info;
}

/**
 * __gcov_exit - Finalize coverage data at program exit
 * Called by atexit handlers inserted by the compiler
 */
void __gcov_exit(void) {
    /* Stub: In full implementation, would write .gcda files.
     * In kernel, this would dump coverage data to memory/serial. */
}

/**
 * __gcov_merge_add - Merge coverage counters (used for incremental coverage)
 *
 * @param counters: Array of coverage counters
 * @param n: Number of counters to merge
 */
void __gcov_merge_add(uint64_t *counters, unsigned int n) {
    /* Stub: In full implementation, would merge counter values
     * from .gcda file with in-memory counters. */
    (void)counters;
    (void)n;
}

/**
 * __gcov_fork - Handle fork() for coverage tracking
 * Not needed in kernel (no fork), but may be referenced
 */
void __gcov_fork(void) {
    /* Not applicable in bare-metal kernel */
}

/**
 * __gcov_execl - Handle exec() family for coverage tracking
 * Not needed in kernel, but may be referenced
 */
void __gcov_execl(const char *path, const char *arg, ...) {
    (void)path;
    (void)arg;
}

/**
 * __gcov_execlp - Handle execlp() for coverage tracking
 * Not needed in kernel, but may be referenced
 */
void __gcov_execlp(const char *file, const char *arg, ...) {
    (void)file;
    (void)arg;
}

/**
 * __gcov_execle - Handle execle() for coverage tracking
 * Not needed in kernel, but may be referenced
 */
void __gcov_execle(const char *path, const char *arg, ...) {
    (void)path;
    (void)arg;
}

/**
 * __gcov_execv - Handle execv() for coverage tracking
 * Not needed in kernel, but may be referenced
 */
void __gcov_execv(const char *path, char *const argv[]) {
    (void)path;
    (void)argv;
}

/**
 * __gcov_execvp - Handle execvp() for coverage tracking
 * Not needed in kernel, but may be referenced
 */
void __gcov_execvp(const char *file, char *const argv[]) {
    (void)file;
    (void)argv;
}

/**
 * __gcov_execve - Handle execve() for coverage tracking
 * Not needed in kernel, but may be referenced
 */
void __gcov_execve(const char *path, char *const argv[], char *const envp[]) {
    (void)path;
    (void)argv;
    (void)envp;
}

/**
 * __gcov_flush - Explicitly flush coverage data
 * Could be called by tests to dump coverage before shutdown
 */
void __gcov_flush(void) {
    /* Stub: In full implementation, would write coverage data.
     * This could be implemented to serialize coverage to memory/serial. */
}

/**
 * __gcov_reset - Reset coverage counters
 * Useful for measuring coverage of specific code sections
 */
void __gcov_reset(void) {
    /* Stub: Would reset all coverage counters to zero */
}

/**
 * __gcov_dump - Dump coverage data
 * Similar to flush but typically used differently
 */
void __gcov_dump(void) {
    /* Stub: Would dump coverage data */
}
