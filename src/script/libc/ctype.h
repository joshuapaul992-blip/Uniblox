/* Uniblox libc for Lua: the "C" locale, ASCII only. */
#ifndef UBC_CTYPE_H
#define UBC_CTYPE_H
static inline int ubc_isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int ubc_islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int ubc_isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int ubc_isalpha(int c) { return ubc_islower(c) || ubc_isupper(c); }
static inline int ubc_isalnum(int c) { return ubc_isalpha(c) || ubc_isdigit(c); }
static inline int ubc_isxdigit(int c) { return ubc_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int ubc_isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline int ubc_iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
static inline int ubc_isprint(int c) { return c >= 32 && c < 127; }
static inline int ubc_isgraph(int c) { return c > 32 && c < 127; }
static inline int ubc_ispunct(int c) { return ubc_isgraph(c) && !ubc_isalnum(c); }
static inline int ubc_tolower(int c) { return ubc_isupper(c) ? c + 32 : c; }
static inline int ubc_toupper(int c) { return ubc_islower(c) ? c - 32 : c; }
#define isdigit ubc_isdigit
#define islower ubc_islower
#define isupper ubc_isupper
#define isalpha ubc_isalpha
#define isalnum ubc_isalnum
#define isxdigit ubc_isxdigit
#define isspace ubc_isspace
#define iscntrl ubc_iscntrl
#define isprint ubc_isprint
#define isgraph ubc_isgraph
#define ispunct ubc_ispunct
#define tolower ubc_tolower
#define toupper ubc_toupper
#endif
