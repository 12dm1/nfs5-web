/* Compile-test shim for wasm32 without a sysroot: libm comes from wasi-libc at link time. */
#pragma once
double sqrt(double); double fabs(double); double floor(double); double ceil(double);
double trunc(double); double nearbyint(double); double sin(double); double cos(double);
double tan(double); double atan2(double,double); double exp2(double); double log2(double);
double fmod(double,double); double ldexp(double,int);
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define signbit(x) __builtin_signbit(x)
#define NAN __builtin_nanf("")
#define INFINITY __builtin_inff()
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
