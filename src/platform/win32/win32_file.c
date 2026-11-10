/* Uniblox - Win32 files, memory, time and mutexes. */
#include "platform/win32/win32_internal.h"
#include "core/ub_log.h"
#include "core/ub_path.h"
#include "core/ub_string.h"
#include "core/ub_utf.h"
#include "crt/ub_crt.h"

static HANDLE g_heap;
static WCHAR g_base[MAX_PATH];
static int g_base_len;

/* ------------------------------------------------------------- memory */

void *plat_alloc(size_t bytes)
{
    if (!g_heap) g_heap = GetProcessHeap();
    return HeapAlloc(g_heap, HEAP_ZERO_MEMORY, bytes ? bytes : 1);
}

void plat_free(void *p)
{
    if (p) HeapFree(g_heap, 0, p);
}

/* --------------------------------------------------------------- time */

u64 plat_time_us(void)
{
    static LARGE_INTEGER freq, start;
    static int mode; /* 0 = not initialised, 1 = QPC, 2 = GetTickCount */
    if (mode == 0) {
        if (QueryPerformanceFrequency(&freq) && freq.QuadPart > 0) {
            QueryPerformanceCounter(&start);
            mode = 1;
        } else {
            mode = 2;
        }
    }
    if (mode == 1) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        u64 ticks = (u64)(now.QuadPart - start.QuadPart);
        u64 f = (u64)freq.QuadPart;
        return (ticks / f) * 1000000u + ((ticks % f) * 1000000u) / f;
    }
    return (u64)GetTickCount() * 1000u;
}

void plat_local_time(ub_datetime *out)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    out->year = st.wYear;
    out->month = st.wMonth;
    out->day = st.wDay;
    out->hour = st.wHour;
    out->minute = st.wMinute;
    out->second = st.wSecond;
}

void plat_debug_output(const char *text)
{
    OutputDebugStringA(text);
}

/* ------------------------------------------------------------ mutexes */

struct ub_mutex {
    CRITICAL_SECTION cs;
};

ub_mutex *plat_mutex_create(void)
{
    ub_mutex *m = (ub_mutex *)plat_alloc(sizeof *m);
    if (m) InitializeCriticalSection(&m->cs);
    return m;
}

void plat_mutex_destroy(ub_mutex *m)
{
    if (!m) return;
    DeleteCriticalSection(&m->cs);
    plat_free(m);
}

void plat_mutex_lock(ub_mutex *m)
{
    EnterCriticalSection(&m->cs);
}

void plat_mutex_unlock(ub_mutex *m)
{
    LeaveCriticalSection(&m->cs);
}

/* -------------------------------------------------------------- paths */

void win32_init_base_dir(void)
{
    DWORD n = GetModuleFileNameW(NULL, g_base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        g_base[0] = 0; /* fall back to the current directory */
        g_base_len = 0;
        return;
    }
    int cut = 0;
    for (int i = 0; i < (int)n; ++i) {
        if (g_base[i] == L'\\' || g_base[i] == L'/' || g_base[i] == L':') cut = i + 1;
    }
    g_base[cut] = 0;
    g_base_len = cut;
}

const WCHAR *win32_base_dir(void)
{
    return g_base;
}

bool win32_full_path(const char *game_path, WCHAR *out, int cap)
{
    if (!ub_path_is_safe(game_path)) {
        ub_warn("rejected unsafe path \"%s\"", game_path ? game_path : "(null)");
        return false;
    }
    /* Safe paths are pure ASCII, so the UTF-16 length equals strlen. */
    int len = (int)ub_strlen(game_path);
    if (g_base_len + len + 1 > cap) {
        ub_warn("path too long: \"%s\"", game_path);
        return false;
    }
    memcpy(out, g_base, (size_t)g_base_len * sizeof(WCHAR));
    u16 *rest = (u16 *)(out + g_base_len);
    int n = ub_utf8_to_utf16(game_path, rest, cap - g_base_len);
    for (int i = 0; i < n; ++i) {
        if (rest[i] == '/') rest[i] = '\\';
    }
    return true;
}

/* -------------------------------------------------------------- files */

struct ub_file {
    HANDLE h;
};

ub_file *plat_file_open(const char *path, ub_file_mode mode)
{
    WCHAR full[MAX_PATH];
    if (!win32_full_path(path, full, MAX_PATH)) return NULL;

    DWORD access = mode == UB_FILE_READ ? GENERIC_READ : GENERIC_WRITE;
    DWORD disp = mode == UB_FILE_READ ? OPEN_EXISTING :
                 mode == UB_FILE_WRITE ? CREATE_ALWAYS : OPEN_ALWAYS;
    HANDLE h = CreateFileW(full, access, FILE_SHARE_READ, NULL, disp,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (mode == UB_FILE_APPEND) SetFilePointer(h, 0, NULL, FILE_END);

    ub_file *f = (ub_file *)plat_alloc(sizeof *f);
    if (!f) {
        CloseHandle(h);
        return NULL;
    }
    f->h = h;
    return f;
}

i32 plat_file_read(ub_file *f, void *buf, u32 bytes)
{
    DWORD got = 0;
    if (bytes > 0x7FFFFFFFu) bytes = 0x7FFFFFFFu;
    if (!ReadFile(f->h, buf, bytes, &got, NULL)) return -1;
    return (i32)got;
}

bool plat_file_write(ub_file *f, const void *buf, u32 bytes)
{
    const u8 *p = (const u8 *)buf;
    while (bytes) {
        DWORD put = 0;
        if (!WriteFile(f->h, p, bytes, &put, NULL) || put == 0) return false;
        p += put;
        bytes -= put;
    }
    return true;
}

u32 plat_file_size(ub_file *f)
{
    DWORD size = GetFileSize(f->h, NULL);
    return size == 0xFFFFFFFFu ? 0 : size;
}

void plat_file_close(ub_file *f)
{
    if (!f) return;
    CloseHandle(f->h);
    plat_free(f);
}

bool plat_file_read_all(const char *path, u32 max_bytes, void **data, u32 *size)
{
    *data = NULL;
    *size = 0;
    ub_file *f = plat_file_open(path, UB_FILE_READ);
    if (!f) return false;
    u32 len = plat_file_size(f);
    bool ok = false;
    if (len <= max_bytes && len < 0x7FFFFFFFu) {
        u8 *buf = (u8 *)plat_alloc((size_t)len + 1);
        if (buf) {
            u32 done = 0;
            while (done < len) {
                i32 got = plat_file_read(f, buf + done, len - done);
                if (got <= 0) break;
                done += (u32)got;
            }
            if (done == len) {
                buf[len] = 0;
                *data = buf;
                *size = len;
                ok = true;
            } else {
                plat_free(buf);
            }
        }
    } else {
        ub_warn("%s is %u bytes, over the %u byte limit", path, len, max_bytes);
    }
    plat_file_close(f);
    return ok;
}

bool plat_file_write_all(const char *path, const void *data, u32 size)
{
    char tmp_path[UB_PATH_MAX + 8];
    ub_fmt(tmp_path, sizeof tmp_path, "%s.tmp", path);
    WCHAR tmp_full[MAX_PATH], full[MAX_PATH];
    if (!win32_full_path(path, full, MAX_PATH)) return false;
    if (!win32_full_path(tmp_path, tmp_full, MAX_PATH)) return false;

    HANDLE h = CreateFileW(tmp_full, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    ub_file f = { h };
    bool ok = plat_file_write(&f, data, size) && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp_full, full, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp_full);
    return ok;
}

bool plat_file_rename(const char *old_path, const char *new_path)
{
    WCHAR a[MAX_PATH], b[MAX_PATH];
    if (!win32_full_path(old_path, a, MAX_PATH) || !win32_full_path(new_path, b, MAX_PATH))
        return false;
    return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING) != 0;
}

bool plat_file_exists(const char *path)
{
    WCHAR full[MAX_PATH];
    if (!win32_full_path(path, full, MAX_PATH)) return false;
    DWORD attr = GetFileAttributesW(full);
    return attr != 0xFFFFFFFFu && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool plat_list_files(const char *dir, ub_list_fn fn, void *user)
{
    WCHAR full[MAX_PATH];
    /* "*" is not a safe-path character: check the folder, then add it */
    if (dir[0] && !win32_full_path(dir, full, MAX_PATH - 3)) return false;
    if (!dir[0]) {
        int n = 0;
        while (win32_base_dir()[n] && n < MAX_PATH - 3) { full[n] = win32_base_dir()[n]; ++n; }
        full[n] = 0;
    }
    int len = 0;
    while (full[len]) ++len;
    if (len && full[len - 1] != L'\\') full[len++] = L'\\';
    full[len++] = L'*';
    full[len] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(full, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        char name[MAX_PATH * 3];
        ub_utf16_to_utf8((const u16 *)fd.cFileName, name, sizeof name);
        if ((name[0] == '.' && name[1] == 0) || (name[0] == '.' && name[1] == '.' && name[2] == 0)) continue;
        fn(user, name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, fd.nFileSizeLow);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

bool plat_make_dir(const char *path)
{
    WCHAR full[MAX_PATH];
    if (!win32_full_path(path, full, MAX_PATH)) return false;
    CreateDirectoryW(full, NULL);
    DWORD attr = GetFileAttributesW(full);
    return attr != 0xFFFFFFFFu && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool plat_file_read_user(const char *path, u32 max_bytes, void **data, u32 *size)
{
    *data = NULL;
    *size = 0;
    u16 wide[MAX_PATH];
    if (ub_utf8_to_utf16(path, wide, MAX_PATH) <= 0) return false;
    HANDLE h = CreateFileW((LPCWSTR)wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    bool relative = !(wide[0] == '\\' || wide[0] == '/' || (wide[0] && wide[1] == ':'));
    if (h == INVALID_HANDLE_VALUE && relative && g_base_len > 0) {
        /* not in the current directory: try next to uniblox.exe */
        WCHAR full[MAX_PATH];
        int n = 0;
        while (wide[n]) ++n;
        if (g_base_len + n + 1 <= MAX_PATH) {
            memcpy(full, g_base, (size_t)g_base_len * sizeof(WCHAR));
            memcpy(full + g_base_len, wide, (size_t)(n + 1) * sizeof(WCHAR));
            h = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        }
    }
    if (h == INVALID_HANDLE_VALUE) return false;
    ub_file f = { h };
    u32 len = plat_file_size(&f);
    bool ok = false;
    if (len <= max_bytes && len < 0x7FFFFFFFu) {
        u8 *buf = (u8 *)plat_alloc((size_t)len + 1);
        if (buf) {
            u32 done = 0;
            while (done < len) {
                i32 got = plat_file_read(&f, buf + done, len - done);
                if (got <= 0) break;
                done += (u32)got;
            }
            if (done == len) {
                buf[len] = 0;
                *data = buf;
                *size = len;
                ok = true;
            } else {
                plat_free(buf);
            }
        }
    } else {
        ub_warn("%s is %u bytes, over the %u byte limit", path, len, max_bytes);
    }
    CloseHandle(h);
    return ok;
}

bool plat_command_line_arg(char *out, int cap)
{
    const WCHAR *c = GetCommandLineW();
    if (!c || cap <= 0) return false;
    /* skip the program name, quoted or not */
    if (*c == L'"') {
        ++c;
        while (*c && *c != L'"') ++c;
        if (*c) ++c;
    } else {
        while (*c && *c != L' ' && *c != L'\t') ++c;
    }
    while (*c == L' ' || *c == L'\t') ++c;
    if (!*c) return false;
    u16 arg[MAX_PATH];
    int n = 0;
    bool quoted = *c == L'"';
    if (quoted) ++c;
    while (*c && n < MAX_PATH - 1) {
        if (quoted ? *c == L'"' : (*c == L' ' || *c == L'\t')) break;
        arg[n++] = (u16)*c++;
    }
    arg[n] = 0;
    return ub_utf16_to_utf8(arg, out, cap) > 0;
}
