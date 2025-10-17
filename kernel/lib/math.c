/* EMBODIOS Math Library
 * 
 * Basic math functions for kernel space
 */

#include "embodios/types.h"

/* Square root using Newton's method */
float sqrtf(float x)
{
    if (x < 0.0f) return 0.0f;
    if (x == 0.0f) return 0.0f;
    
    float guess = x;
    float epsilon = 0.00001f;
    
    while (1) {
        float next = 0.5f * (guess + x / guess);
        if ((next - guess) < epsilon && (guess - next) < epsilon)
            break;
        guess = next;
    }
    
    return guess;
}

/* Exponential function using Taylor series */
float expf(float x)
{
    /* Clamp input to prevent overflow */
    if (x > 88.0f) return 3.4e38f;
    if (x < -88.0f) return 0.0f;
    
    /* exp(x) = exp(x/2^n)^(2^n) for better convergence */
    int n = 0;
    float y = x;
    
    /* Reduce x to [-0.5, 0.5] */
    while (y > 0.5f || y < -0.5f) {
        y *= 0.5f;
        n++;
    }
    
    /* Taylor series: exp(y) = 1 + y + y^2/2! + y^3/3! + ... */
    float result = 1.0f;
    float term = 1.0f;
    
    for (int i = 1; i < 12; i++) {
        term *= y / i;
        result += term;
    }
    
    /* Square n times to get exp(x) */
    while (n > 0) {
        result *= result;
        n--;
    }
    
    return result;
}

/* Natural logarithm */
float logf(float x)
{
    if (x <= 0.0f) return -1e30f;
    
    /* Normalize x to [1, 2) */
    int exp = 0;
    while (x >= 2.0f) {
        x *= 0.5f;
        exp++;
    }
    while (x < 1.0f) {
        x *= 2.0f;
        exp--;
    }
    
    /* Use series expansion for ln(1+y) where y = x-1 */
    float y = x - 1.0f;
    float result = 0.0f;
    float term = y;
    float y_pow = y;
    
    for (int i = 1; i < 20; i++) {
        result += term / i;
        y_pow *= -y;
        term = y_pow;
    }
    
    /* Add back the exponent part: ln(x*2^exp) = ln(x) + exp*ln(2) */
    result += exp * 0.693147180559945f;
    
    return result;
}

/* Power function */
float powf(float base, float exp)
{
    if (base == 0.0f) return 0.0f;
    if (exp == 0.0f) return 1.0f;
    
    /* Use exp(y * ln(x)) for x^y */
    return expf(exp * logf(base));
}

/* Absolute value */
float fabsf(float x)
{
    return (x < 0.0f) ? -x : x;
}

/* Hyperbolic tangent */
float tanhf(float x)
{
    if (x > 10.0f) return 1.0f;
    if (x < -10.0f) return -1.0f;
    
    float exp2x = expf(2.0f * x);
    return (exp2x - 1.0f) / (exp2x + 1.0f);
}

/* Cosine using Taylor series */
float cosf(float x)
{
    /* Reduce x to [-pi, pi] */
    const float pi = 3.14159265358979323846f;
    const float two_pi = 2.0f * pi;
    
    while (x > pi) x -= two_pi;
    while (x < -pi) x += two_pi;
    
    /* Taylor series: cos(x) = 1 - x^2/2! + x^4/4! - x^6/6! + ... */
    float result = 1.0f;
    float term = 1.0f;
    float x2 = x * x;
    
    for (int i = 1; i < 10; i++) {
        term *= -x2 / ((2*i-1) * (2*i));
        result += term;
    }
    
    return result;
}

/* Sine using Taylor series */
float sinf(float x)
{
    /* Reduce x to [-pi, pi] */
    const float pi = 3.14159265358979323846f;
    const float two_pi = 2.0f * pi;
    
    while (x > pi) x -= two_pi;
    while (x < -pi) x += two_pi;
    
    /* Taylor series: sin(x) = x - x^3/3! + x^5/5! - x^7/7! + ... */
    float result = x;
    float term = x;
    float x2 = x * x;
    
    for (int i = 1; i < 10; i++) {
        term *= -x2 / ((2*i) * (2*i+1));
        result += term;
    }
    
    return result;
}

/* Read timestamp counter for pseudo-random */
uint64_t rdtsc(void)
{
    #ifdef __x86_64__
    uint32_t lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
    #else
    /* Fallback for non-x86 */
    static uint64_t counter = 12345;
    counter = counter * 1103515245 + 12345;
    return counter;
    #endif
}