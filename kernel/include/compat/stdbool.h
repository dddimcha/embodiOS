/* Kernel stub for stdbool.h
 * For llama.cpp bare-metal compatibility
 */

#ifndef _COMPAT_STDBOOL_H
#define _COMPAT_STDBOOL_H

#ifndef __cplusplus
#define bool  _Bool
#define true  1
#define false 0
#endif

#define __bool_true_false_are_defined 1

#endif /* _COMPAT_STDBOOL_H */
