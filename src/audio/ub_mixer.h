/* Uniblox - software audio mixer.
 *
 * OS-independent. The platform backend opens the sound device, calls
 * ub_mixer_init() with its sample rate, and pulls mixed audio from its own
 * audio thread with ub_mixer_mix(). Game code calls ub_mixer_play() and
 * friends from the main thread. All functions are thread-safe.
 *
 * Output is always signed 16-bit interleaved stereo; the backend converts
 * to the device format (mono, 8-bit) if needed. */
#ifndef UB_MIXER_H
#define UB_MIXER_H

#include "audio/ub_sound.h"

#define UB_MIXER_VOICES 32

/* 0 means "no voice"; handles stay unique as voices are reused. */
typedef u32 ub_voice;

typedef struct {
    f32 volume;  /* 0..1 (values above 1 amplify, clipped at 4) */
    f32 pan;     /* -1 left .. 0 centre .. +1 right */
    f32 pitch;   /* playback speed, 1 = normal; clamped to 0.05..8 */
    bool loop;
} ub_play_params;

static inline ub_play_params ub_play_default(void)
{
    ub_play_params p = { 1.0f, 0.0f, 1.0f, false };
    return p;
}

/* output_rate 0 means there is no output device: play() then does nothing
 * and returns 0, so voices never pile up waiting to be mixed. */
bool ub_mixer_init(u32 output_rate);
void ub_mixer_shutdown(void);
u32 ub_mixer_rate(void);

/* Fill out with frames of stereo audio (2 * frames samples). */
void ub_mixer_mix(i16 *out, u32 frames);

/* Start a sound. If all voices are busy the oldest non-looping voice is
 * replaced. Returns 0 if the sound is empty or nothing could be freed. */
ub_voice ub_mixer_play(const ub_sound *s, ub_play_params params);
void ub_mixer_stop(ub_voice v);
void ub_mixer_stop_sound(const ub_sound *s);  /* every voice using s */
void ub_mixer_stop_all(void);
bool ub_mixer_is_playing(ub_voice v);
void ub_mixer_set_master_volume(f32 volume); /* 0..1 */
f32 ub_mixer_master_volume(void);
int ub_mixer_active_voices(void);

#endif
