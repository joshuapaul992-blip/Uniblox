/* Uniblox - game file paths.
 *
 * All engine file access goes through relative paths under the folder the
 * .exe lives in, written with '/' separators, e.g. "sounds/jump.wav".
 * Later, untrusted place files and scripts will name files too, so paths
 * are validated rather than passed straight to the OS. */
#ifndef UB_PATH_H
#define UB_PATH_H

#include "core/ub_types.h"

#define UB_PATH_MAX 200 /* leaves room under MAX_PATH (260) for the exe dir */

/* True if path is a safe relative path: non-empty, shorter than
 * UB_PATH_MAX, made of [A-Za-z0-9 _.-] components separated by single '/',
 * with no "." or ".." components, no leading/trailing '/', no drive letter,
 * and no component that is a reserved DOS device name (CON, NUL, COM1...). */
bool ub_path_is_safe(const char *path);

#endif
