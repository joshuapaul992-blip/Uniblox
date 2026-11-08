/* Uniblox - sound data. */
#ifndef UB_SOUND_H
#define UB_SOUND_H

#include "core/ub_types.h"

/* Decoded PCM, always signed 16-bit, interleaved if stereo. */
typedef struct {
    i16 *samples;
    u32 frames;     /* samples per channel */
    u32 rate;       /* Hz */
    u32 channels;   /* 1 or 2 */
} ub_sound;

typedef enum {
    UB_WAV_OK,
    UB_WAV_NOT_RIFF,
    UB_WAV_UNSUPPORTED,  /* compressed, >2 channels, odd bit depth... */
    UB_WAV_CORRUPT,
    UB_WAV_TOO_LARGE,
    UB_WAV_NO_MEMORY,
} ub_wav_result;

/* Upper bound on decoded size, so a hostile file cannot exhaust memory. */
#define UB_WAV_MAX_FRAMES (60u * 48000u)

/* Parse an in-memory .wav (PCM 8/16-bit, mono/stereo, 1000-96000 Hz).
 * On success out->samples is plat_alloc'd; free with ub_sound_free.
 * The input is treated as untrusted. */
ub_wav_result ub_wav_parse(const void *data, u32 size, ub_sound *out);
const char *ub_wav_result_name(ub_wav_result r);

/* Stops any voices still playing the sound, then frees its samples. */
void ub_sound_free(ub_sound *s);

#endif
