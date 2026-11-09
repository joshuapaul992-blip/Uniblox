/* Uniblox libc for Lua: just enough stdio for Lua to link. Files never
 * open; stdout and stderr go to the log. */
#ifndef UBC_STDIO_H
#define UBC_STDIO_H
#include <stdarg.h>
#include <stddef.h>
typedef struct ubc_file FILE;
extern FILE *ubc_stdin, *ubc_stdout, *ubc_stderr;
#define stdin ubc_stdin
#define stdout ubc_stdout
#define stderr ubc_stderr
#define EOF (-1)
#define BUFSIZ 512
FILE *ubc_fopen(const char *path, const char *mode);
FILE *ubc_freopen(const char *path, const char *mode, FILE *f);
int ubc_fclose(FILE *f);
size_t ubc_fread(void *p, size_t size, size_t n, FILE *f);
int ubc_getc(FILE *f);
int ubc_ungetc(int c, FILE *f);
int ubc_feof(FILE *f);
int ubc_ferror(FILE *f);
int ubc_fflush(FILE *f);
int ubc_fputs(const char *s, FILE *f);
int ubc_fputc(int c, FILE *f);
int ubc_fprintf(FILE *f, const char *fmt, ...);
int ubc_sprintf(char *buf, const char *fmt, ...);
int ubc_snprintf(char *buf, size_t n, const char *fmt, ...);
int ubc_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
#define fopen ubc_fopen
#define freopen ubc_freopen
#define fclose ubc_fclose
#define fread ubc_fread
#define getc ubc_getc
#define ungetc ubc_ungetc
#define feof ubc_feof
#define ferror ubc_ferror
#define fflush ubc_fflush
#define fputs ubc_fputs
#define fputc ubc_fputc
#define fprintf ubc_fprintf
#define sprintf ubc_sprintf
#define snprintf ubc_snprintf
#define vsnprintf ubc_vsnprintf
/* Where stdout/stderr text goes (a whole line at a time). */
void ubc_set_output(void (*fn)(const char *line, int is_error));
#endif
