/* Uniblox libc for Lua: setjmp/longjmp. On Windows our own (i386
 * assembly in ub_libc.c); the host tests use the system's. */
#ifndef UBC_SETJMP_H
#define UBC_SETJMP_H
#if defined(_WIN32) && defined(__i386__)
typedef int ubc_jmp_buf[6]; /* ebx, esi, edi, ebp, esp, eip */
int ubc_setjmp(ubc_jmp_buf b) __attribute__((returns_twice));
void ubc_longjmp(ubc_jmp_buf b, int v) __attribute__((noreturn));
#define jmp_buf ubc_jmp_buf
#define setjmp ubc_setjmp
#define longjmp ubc_longjmp
#else
#include_next <setjmp.h>
#endif
#endif
