#ifndef UBC_STDLIB_H
#define UBC_STDLIB_H
#include <stddef.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7FFF
void *ubc_realloc(void *p, size_t n);
void ubc_free(void *p);
void ubc_exit(int code) __attribute__((noreturn));
void ubc_abort(void) __attribute__((noreturn));
double ubc_strtod(const char *s, char **end);
unsigned long ubc_strtoul(const char *s, char **end, int base);
int ubc_rand(void);
void ubc_srand(unsigned seed);
static inline int ubc_abs(int x) { return x < 0 ? -x : x; }
#define realloc ubc_realloc
#define free ubc_free
#define exit ubc_exit
#define abort ubc_abort
#define strtod ubc_strtod
#define strtoul ubc_strtoul
#define rand ubc_rand
#define srand ubc_srand
#define abs ubc_abs
#endif
