#ifndef UBC_ERRNO_H
#define UBC_ERRNO_H
/* mingw's <stddef.h> pulls in corecrt.h, which defines errno as the CRT's;
 * include it first so ours wins */
#include <stddef.h>
extern int ubc_errno;
#undef errno
#define errno ubc_errno
#define ENOENT 2
#define ERANGE 34
#endif
