/* Kernel stub for math.h
 * Math functions for AI inference
 * For llama.cpp bare-metal compatibility
 */

#ifndef _COMPAT_MATH_H
#define _COMPAT_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Constants */
#define M_E        2.71828182845904523536
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

/* Infinity and NaN */
#define INFINITY __builtin_inff()
#define NAN      __builtin_nanf("")
#define HUGE_VAL __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()

/* Classification macros */
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define isnormal(x)   __builtin_isnormal(x)
#define signbit(x)    __builtin_signbit(x)
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)

/* Basic functions - float */
float sqrtf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float roundf(float x);
float truncf(float x);
float fmodf(float x, float y);
float remainderf(float x, float y);
float copysignf(float x, float y);
float fmaxf(float x, float y);
float fminf(float x, float y);

/* Exponential and logarithmic - float */
float expf(float x);
float exp2f(float x);
float expm1f(float x);
float logf(float x);
float log2f(float x);
float log10f(float x);
float log1pf(float x);
float powf(float x, float y);

/* Trigonometric - float */
float sinf(float x);
float cosf(float x);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);

/* Basic functions - double */
double sqrt(double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);
double fmod(double x, double y);
double remainder(double x, double y);
double copysign(double x, double y);
double fmax(double x, double y);
double fmin(double x, double y);

/* Exponential and logarithmic - double */
double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double pow(double x, double y);

/* Trigonometric - double */
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);

/* Hyperbolic inverse */
float asinhf(float x);
float acoshf(float x);
float atanhf(float x);
double asinh(double x);
double acosh(double x);
double atanh(double x);

/* Error and gamma functions */
float erff(float x);
float erfcf(float x);
float tgammaf(float x);
float lgammaf(float x);
double erf(double x);
double erfc(double x);
double tgamma(double x);
double lgamma(double x);

/* FMA */
float fmaf(float x, float y, float z);
double fma(double x, double y, double z);

/* Decomposition */
float frexpf(float x, int* exp);
float ldexpf(float x, int exp);
float modff(float x, float* iptr);
float scalbnf(float x, int n);
double frexp(double x, int* exp);
double ldexp(double x, int exp);
double modf(double x, double* iptr);
double scalbn(double x, int n);

/* Integer rounding */
long lroundf(float x);
long lround(double x);
long long llroundf(float x);
long long llround(double x);

#ifdef __cplusplus
}
#endif

#endif /* _COMPAT_MATH_H */
