/* Minimal math.h for tmnt_code.so: the functions come from the Vita runtime (newlib). */
#ifndef SO_MATH_H
#define SO_MATH_H
#define NAN (__builtin_nanf(""))
#define INFINITY (__builtin_inff())
#define signbit(x) __builtin_signbit(x)
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
double fabs(double); float fabsf(float);
double sqrt(double); float sqrtf(float);
double sin(double); double cos(double); double atan2(double, double);
double trunc(double); double floor(double); double ceil(double); double round(double);
double nearbyint(double); double remainder(double, double); double fmod(double, double);
double ldexp(double, int); double log(double); double log2(double); double exp2(double);
#endif
