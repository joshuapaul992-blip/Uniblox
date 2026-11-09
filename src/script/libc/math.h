/* Uniblox libc for Lua: double-precision maths (x87 instructions). */
#ifndef UBC_MATH_H
#define UBC_MATH_H
#define HUGE_VAL (__builtin_huge_val())
double ubc_floor(double x);
double ubc_ceil(double x);
double ubc_fabs(double x);
double ubc_fmod(double x, double y);
double ubc_modf(double x, double *ip);
double ubc_frexp(double x, int *e);
double ubc_ldexp(double x, int e);
double ubc_sqrt(double x);
double ubc_pow(double x, double y);
double ubc_exp(double x);
double ubc_log(double x);
double ubc_log10(double x);
double ubc_sin(double x);
double ubc_cos(double x);
double ubc_tan(double x);
double ubc_asin(double x);
double ubc_acos(double x);
double ubc_atan(double x);
double ubc_atan2(double y, double x);
double ubc_sinh(double x);
double ubc_cosh(double x);
double ubc_tanh(double x);
#define floor ubc_floor
#define ceil ubc_ceil
#define fabs ubc_fabs
#define fmod ubc_fmod
#define modf ubc_modf
#define frexp ubc_frexp
#define ldexp ubc_ldexp
#define sqrt ubc_sqrt
#define pow ubc_pow
#define exp ubc_exp
#define log ubc_log
#define log10 ubc_log10
#define sin ubc_sin
#define cos ubc_cos
#define tan ubc_tan
#define asin ubc_asin
#define acos ubc_acos
#define atan ubc_atan
#define atan2 ubc_atan2
#define sinh ubc_sinh
#define cosh ubc_cosh
#define tanh ubc_tanh
#endif
