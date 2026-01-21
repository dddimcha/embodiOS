/* Kernel stub for inttypes.h
 * Integer format specifiers for llama.cpp bare-metal compatibility
 */

#ifndef _COMPAT_INTTYPES_H
#define _COMPAT_INTTYPES_H

#include "stdint.h"

/* Format specifiers for printf/scanf */

/* Signed integers */
#define PRId8   "d"
#define PRId16  "d"
#define PRId32  "d"
#define PRId64  "lld"

#define PRIi8   "i"
#define PRIi16  "i"
#define PRIi32  "i"
#define PRIi64  "lli"

/* Unsigned integers */
#define PRIu8   "u"
#define PRIu16  "u"
#define PRIu32  "u"
#define PRIu64  "llu"

/* Hexadecimal */
#define PRIx8   "x"
#define PRIx16  "x"
#define PRIx32  "x"
#define PRIx64  "llx"

#define PRIX8   "X"
#define PRIX16  "X"
#define PRIX32  "X"
#define PRIX64  "llX"

/* Octal */
#define PRIo8   "o"
#define PRIo16  "o"
#define PRIo32  "o"
#define PRIo64  "llo"

/* Pointer */
#define PRIdPTR "ld"
#define PRIiPTR "li"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define PRIXPTR "lX"
#define PRIoPTR "lo"

/* Max width */
#define PRIdMAX "lld"
#define PRIiMAX "lli"
#define PRIuMAX "llu"
#define PRIxMAX "llx"
#define PRIXMAX "llX"
#define PRIoMAX "llo"

/* Scanf format specifiers */
#define SCNd8   "hhd"
#define SCNd16  "hd"
#define SCNd32  "d"
#define SCNd64  "lld"

#define SCNi8   "hhi"
#define SCNi16  "hi"
#define SCNi32  "i"
#define SCNi64  "lli"

#define SCNu8   "hhu"
#define SCNu16  "hu"
#define SCNu32  "u"
#define SCNu64  "llu"

#define SCNx8   "hhx"
#define SCNx16  "hx"
#define SCNx32  "x"
#define SCNx64  "llx"

/* Integer conversion functions */
#ifdef __cplusplus
extern "C" {
#endif

intmax_t strtoimax(const char* nptr, char** endptr, int base);
uintmax_t strtoumax(const char* nptr, char** endptr, int base);

#ifdef __cplusplus
}
#endif

#endif /* _COMPAT_INTTYPES_H */
