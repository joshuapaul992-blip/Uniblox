/* Uniblox - shared declarations between the Win32 backend files.
 *
 * Baseline target is Windows NT 3.51: only call APIs listed in
 * tools/nt351_imports.txt (make check enforces it). Anything newer must be
 * loaded with GetProcAddress and have a fallback. */
#ifndef UB_WIN32_INTERNAL_H
#define UB_WIN32_INTERNAL_H

#include <windows.h>
#include <mmsystem.h>

#include "platform/platform.h"

/* Remember the folder holding the .exe; all game paths are under it. */
void win32_init_base_dir(void);
const WCHAR *win32_base_dir(void);

/* Game path ("sounds/jump.wav") -> full Windows path. Fails (and logs) if
 * the path is unsafe or too long. */
bool win32_full_path(const char *game_path, WCHAR *out, int cap);

/* Opens the first sound device/format that works, starts the mixer and the
 * audio thread. Always initialises the mixer, even with no device, so game
 * code can call ub_mixer_play() unconditionally. Fills sys->audio_desc. */
bool win32_audio_open(ub_system_info *sys);
void win32_audio_close(void);

/* software renderer output (win32_display.c) */
bool win32_sw_init(const ub_system_info *sys, int sw_height);
ub_framebuffer *win32_sw_prepare(int client_w, int client_h);
void win32_sw_present(HDC wdc, ub_frame *frame);
bool win32_sw_palette_message(HWND hwnd, UINT msg, WPARAM wp, LRESULT *result);
void win32_sw_shutdown(void);

/* OpenGL context (win32_gl.c). software_driver is set when the OpenGL
 * found renders on the CPU (Microsoft's "GDI Generic", WINVBE's ICD,
 * Mesa llvmpipe/softpipe/swrast). */
bool win32_gl_init(HWND hwnd, bool vsync, bool *software_driver);
void win32_gl_swap(void);
void win32_gl_shutdown(void);

#endif
