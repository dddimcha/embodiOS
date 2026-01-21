/* Kernel stub for setjmp.h
 * For llama.cpp bare-metal compatibility
 */

#ifndef _COMPAT_SETJMP_H
#define _COMPAT_SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Jump buffer - architecture dependent */
#if defined(__x86_64__)
typedef long jmp_buf[8]; /* rbx, rbp, r12-r15, rsp, rip */
#elif defined(__aarch64__)
typedef long jmp_buf[22]; /* x19-x30, sp, d8-d15 */
#else
typedef long jmp_buf[16];
#endif

typedef jmp_buf sigjmp_buf;

/* Save current context */
int setjmp(jmp_buf env);
int _setjmp(jmp_buf env);
int sigsetjmp(sigjmp_buf env, int savemask);

/* Restore saved context - never returns */
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
void _longjmp(jmp_buf env, int val) __attribute__((noreturn));
void siglongjmp(sigjmp_buf env, int val) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* _COMPAT_SETJMP_H */
