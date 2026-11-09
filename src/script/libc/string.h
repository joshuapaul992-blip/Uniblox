#ifndef UBC_STRING_H
#define UBC_STRING_H
#include <stddef.h>
/* the memory functions keep their names: crt/ub_crt.c (or the host C library) */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *ubc_memchr(const void *p, int c, size_t n);
size_t ubc_strlen(const char *s);
int ubc_strcmp(const char *a, const char *b);
int ubc_strcoll(const char *a, const char *b);
char *ubc_strchr(const char *s, int c);
char *ubc_strpbrk(const char *s, const char *set);
size_t ubc_strspn(const char *s, const char *set);
size_t ubc_strcspn(const char *s, const char *set);
char *ubc_strstr(const char *s, const char *sub);
char *ubc_strcpy(char *d, const char *s);
char *ubc_strncpy(char *d, const char *s, size_t n);
char *ubc_strcat(char *d, const char *s);
char *ubc_strncat(char *d, const char *s, size_t n);
char *ubc_strerror(int e);
#define memchr ubc_memchr
#define strlen ubc_strlen
#define strcmp ubc_strcmp
#define strcoll ubc_strcoll
#define strchr ubc_strchr
#define strpbrk ubc_strpbrk
#define strspn ubc_strspn
#define strcspn ubc_strcspn
#define strstr ubc_strstr
#define strcpy ubc_strcpy
#define strncpy ubc_strncpy
#define strcat ubc_strcat
#define strncat ubc_strncat
#define strerror ubc_strerror
#endif
