/* Kernel stub for assert.h
 * For llama.cpp bare-metal compatibility
 */

#ifndef _COMPAT_ASSERT_H
#define _COMPAT_ASSERT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration of panic function */
void kpanic(const char* message);

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) \
    ((expr) ? (void)0 : kpanic("Assertion failed: " #expr))
#endif

/* Static assert (C11) */
#ifndef static_assert
#define static_assert _Static_assert
#endif

#ifdef __cplusplus
}
#endif

#endif /* _COMPAT_ASSERT_H */
