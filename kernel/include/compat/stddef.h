/* Kernel stub for stddef.h
 * For llama.cpp bare-metal compatibility
 */

#ifndef _COMPAT_STDDEF_H
#define _COMPAT_STDDEF_H

/* Size type */
#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef unsigned long size_t;
#endif

/* Signed size type */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif

/* Pointer difference type */
#ifndef _PTRDIFF_T_DEFINED
#define _PTRDIFF_T_DEFINED
typedef long ptrdiff_t;
#endif

/* Wide character type */
#ifndef _WCHAR_T_DEFINED
#define _WCHAR_T_DEFINED
#ifndef __cplusplus
typedef int wchar_t;
#endif
#endif

/* NULL pointer */
#ifndef NULL
#ifdef __cplusplus
#define NULL nullptr
#else
#define NULL ((void*)0)
#endif
#endif

/* Offset of member in structure */
#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

/* Max alignment type */
typedef struct {
    long long __ll __attribute__((__aligned__(__alignof__(long long))));
    long double __ld __attribute__((__aligned__(__alignof__(long double))));
} max_align_t;

#endif /* _COMPAT_STDDEF_H */
