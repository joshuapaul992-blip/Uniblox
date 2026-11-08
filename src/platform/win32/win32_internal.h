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

#endif
