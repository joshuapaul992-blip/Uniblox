#ifndef UBC_LOCALE_H
#define UBC_LOCALE_H
struct lconv {
    char *decimal_point;
};
struct lconv *ubc_localeconv(void);
#define localeconv ubc_localeconv
#endif
