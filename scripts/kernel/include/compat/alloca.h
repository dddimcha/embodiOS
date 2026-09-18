/* Kernel stub for alloca.h
 * Stack allocation for llama.cpp bare-metal compatibility
 */

#ifndef _COMPAT_ALLOCA_H
#define _COMPAT_ALLOCA_H

/* Use compiler builtin for stack allocation */
#define alloca(size) __builtin_alloca(size)

#endif /* _COMPAT_ALLOCA_H */
