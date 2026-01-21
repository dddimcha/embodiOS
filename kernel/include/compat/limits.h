/* Kernel stub for limits.h
 * For llama.cpp bare-metal compatibility
 */

#ifndef _COMPAT_LIMITS_H
#define _COMPAT_LIMITS_H

/* Number of bits in a char */
#define CHAR_BIT 8

/* Minimum and maximum values a char can hold */
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255

/* Minimum and maximum values a char can hold (signed or unsigned) */
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX

/* Minimum and maximum values a short int can hold */
#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define USHRT_MAX 65535

/* Minimum and maximum values an int can hold */
#define INT_MIN (-2147483647-1)
#define INT_MAX 2147483647
#define UINT_MAX 4294967295U

/* Minimum and maximum values a long int can hold */
#define LONG_MIN  (-9223372036854775807L-1)
#define LONG_MAX  9223372036854775807L
#define ULONG_MAX 18446744073709551615UL

/* Minimum and maximum values a long long int can hold */
#define LLONG_MIN  (-9223372036854775807LL-1)
#define LLONG_MAX  9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL

/* Maximum bytes in a multibyte character */
#define MB_LEN_MAX 4

/* Miscellaneous */
#define SSIZE_MAX LONG_MAX
#define PATH_MAX  4096
#define NAME_MAX  255

#endif /* _COMPAT_LIMITS_H */
