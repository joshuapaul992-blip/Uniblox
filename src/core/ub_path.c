#include "core/ub_path.h"

static bool path_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ' ';
}

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* "CON", "con.txt", "COM1.wav" etc. open devices on Windows, not files. */
static bool is_dos_device(const char *comp, int len)
{
    static const char *const names[] = { "CON", "PRN", "AUX", "NUL", "CLOCK$" };
    int base = 0;
    while (base < len && comp[base] != '.') ++base;
    for (int i = 0; i < UB_ARRAY_COUNT(names); ++i) {
        const char *n = names[i];
        int k = 0;
        while (n[k] && k < base && upper(comp[k]) == n[k]) ++k;
        if (!n[k] && k == base) return true;
    }
    if (base == 4 && comp[3] >= '1' && comp[3] <= '9') {
        char a = upper(comp[0]), b = upper(comp[1]), c = upper(comp[2]);
        if ((a == 'C' && b == 'O' && c == 'M') || (a == 'L' && b == 'P' && c == 'T'))
            return true;
    }
    return false;
}

bool ub_path_is_safe(const char *path)
{
    if (!path || !path[0]) return false;
    int len = 0;
    const char *comp = path;
    for (const char *p = path;; ++p, ++len) {
        if (len >= UB_PATH_MAX) return false;
        if (*p == '/' || *p == '\0') {
            int clen = (int)(p - comp);
            if (clen == 0) return false;                       /* "//", leading or trailing '/' */
            if (comp[0] == '.' && (clen == 1 || (clen == 2 && comp[1] == '.')))
                return false;                                   /* "." or ".." */
            if (comp[clen - 1] == '.' || comp[clen - 1] == ' ')
                return false;                                   /* Windows strips these silently */
            if (comp[0] == ' ') return false;
            if (is_dos_device(comp, clen)) return false;
            if (*p == '\0') return true;
            comp = p + 1;
        } else if (!path_char_ok(*p)) {
            return false;                                       /* '\\', ':', '*', control chars... */
        }
    }
}
