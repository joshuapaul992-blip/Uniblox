#include "core/ub_path.h"
#include "core/ub_string.h"

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

/* ------------------------------------------------- content addresses */

static bool starts_ci(const char *s, const char *p)
{
    while (*p) {
        if ((*s | 32) != (*p | 32)) return false;
        ++s;
        ++p;
    }
    return true;
}

bool ub_content_path(const char *content, char *path, int cap, const char *ext)
{
    path[0] = 0;
    const char *s = content;
    while (*s == ' ') ++s;
    /* an asset id: rbxassetid://N, ...roblox.com/asset/?id=N, or just N */
    const char *id = NULL;
    if (starts_ci(s, "rbxassetid://")) id = s + 13;
    else if (starts_ci(s, "http://") || starts_ci(s, "https://")) {
        const char *q = s;
        while (*q && !(starts_ci(q, "id=") && (q[-1] == '?' || q[-1] == '&'))) ++q;
        if (*q) id = q + 3;
    } else if (*s >= '0' && *s <= '9') id = s;
    if (id) {
        char num[24];
        int n = 0;
        while (id[n] >= '0' && id[n] <= '9' && n < 20) {
            num[n] = id[n];
            ++n;
        }
        num[n] = 0;
        if (!n) return false;
        ub_fmt(path, cap, "content/%s.%s", num, ext);
        return true;
    }
    if (starts_ci(s, "rbxasset://")) {
        ub_fmt(path, cap, "content/%s", s + 11);
        for (char *c = path; *c; ++c) if (*c == '\\') *c = '/';
        return ub_path_is_safe(path);
    }
    return false;
}

