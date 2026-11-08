#include "audio/ub_mixer.h"
#include "core/ub_math.h"
#include "platform/platform.h"

/* Gains are 8.8 fixed point (256 = unity); positions 32.16. */
typedef struct {
    const ub_sound *sound;
    u32 pos;        /* integer frame */
    u32 frac;       /* 0..65535 */
    u32 step;       /* 16.16 frames per output frame */
    i32 gain_l, gain_r;
    u32 started;    /* age counter for voice stealing */
    u16 generation;
    bool loop;
    bool active;
} voice;

static voice g_voices[UB_MIXER_VOICES];
static ub_mutex *g_lock;
static u32 g_rate = 22050;
static bool g_silent;      /* no output device */
static i32 g_master = 256;
static u32 g_clock;
static i32 g_accum[1024 * 2];

#define LOCK()   do { if (g_lock) plat_mutex_lock(g_lock); } while (0)
#define UNLOCK() do { if (g_lock) plat_mutex_unlock(g_lock); } while (0)

bool ub_mixer_init(u32 output_rate)
{
    if (!g_lock) g_lock = plat_mutex_create();
    LOCK();
    g_silent = output_rate == 0;
    g_rate = output_rate ? output_rate : 22050;
    for (int i = 0; i < UB_MIXER_VOICES; ++i) g_voices[i].active = false;
    UNLOCK();
    return g_lock != NULL;
}

void ub_mixer_shutdown(void)
{
    ub_mixer_stop_all();
    if (g_lock) {
        plat_mutex_destroy(g_lock);
        g_lock = NULL;
    }
}

u32 ub_mixer_rate(void)
{
    return g_rate;
}

static ub_voice make_handle(int index)
{
    return ((u32)g_voices[index].generation << 8) | (u32)(index + 1);
}

static voice *lookup(ub_voice h)
{
    u32 index = (h & 0xFF);
    if (index == 0 || index > UB_MIXER_VOICES) return NULL;
    voice *v = &g_voices[index - 1];
    if (!v->active || v->generation != (u16)(h >> 8)) return NULL;
    return v;
}

ub_voice ub_mixer_play(const ub_sound *s, ub_play_params pp)
{
    if (!s || !s->samples || s->frames == 0 || g_silent) return 0;
    f32 vol = ub_clampf(pp.volume, 0.0f, 4.0f);
    f32 pan = ub_clampf(pp.pan, -1.0f, 1.0f);
    f32 pitch = ub_clampf(pp.pitch, 0.05f, 8.0f);

    LOCK();
    int slot = -1;
    for (int i = 0; i < UB_MIXER_VOICES; ++i) {
        if (!g_voices[i].active) { slot = i; break; }
    }
    if (slot < 0) { /* steal the oldest one-shot */
        u32 best_age = 0;
        for (int i = 0; i < UB_MIXER_VOICES; ++i) {
            u32 age = g_clock - g_voices[i].started;
            if (!g_voices[i].loop && (slot < 0 || age > best_age)) {
                slot = i;
                best_age = age;
            }
        }
    }
    ub_voice h = 0;
    if (slot >= 0) {
        voice *v = &g_voices[slot];
        v->sound = s;
        v->pos = 0;
        v->frac = 0;
        f32 step = pitch * (f32)s->rate / (f32)g_rate * 65536.0f;
        v->step = (u32)UB_CLAMP(step, 1.0f, 65536.0f * 64.0f);
        /* Linear pan law: centre is -6 dB per side, hard pan is full. */
        v->gain_l = (i32)(vol * (pan <= 0 ? 1.0f : 1.0f - pan) * 256.0f);
        v->gain_r = (i32)(vol * (pan >= 0 ? 1.0f : 1.0f + pan) * 256.0f);
        if (s->channels == 1) { /* keep centred mono at the same loudness */
            v->gain_l = v->gain_l * 3 / 4;
            v->gain_r = v->gain_r * 3 / 4;
        }
        v->loop = pp.loop;
        v->started = g_clock++;
        v->generation++;
        v->active = true;
        h = make_handle(slot);
    }
    UNLOCK();
    return h;
}

void ub_mixer_stop(ub_voice h)
{
    LOCK();
    voice *v = lookup(h);
    if (v) v->active = false;
    UNLOCK();
}

void ub_mixer_stop_sound(const ub_sound *s)
{
    LOCK();
    for (int i = 0; i < UB_MIXER_VOICES; ++i) {
        if (g_voices[i].sound == s) g_voices[i].active = false;
    }
    UNLOCK();
}

void ub_mixer_stop_all(void)
{
    LOCK();
    for (int i = 0; i < UB_MIXER_VOICES; ++i) g_voices[i].active = false;
    UNLOCK();
}

bool ub_mixer_is_playing(ub_voice h)
{
    LOCK();
    bool r = lookup(h) != NULL;
    UNLOCK();
    return r;
}

void ub_mixer_set_master_volume(f32 volume)
{
    LOCK();
    g_master = (i32)(ub_clampf(volume, 0.0f, 1.0f) * 256.0f);
    UNLOCK();
}

f32 ub_mixer_master_volume(void)
{
    return (f32)g_master / 256.0f;
}

int ub_mixer_active_voices(void)
{
    int n = 0;
    LOCK();
    for (int i = 0; i < UB_MIXER_VOICES; ++i) n += g_voices[i].active ? 1 : 0;
    UNLOCK();
    return n;
}

/* Mix one voice into acc[0..2*frames). Linear interpolation between
 * neighbouring source frames. */
static void mix_voice(voice *v, i32 *acc, u32 frames)
{
    const ub_sound *s = v->sound;
    const i16 *src = s->samples;
    const u32 n = s->frames;
    const bool stereo = s->channels == 2;
    for (u32 i = 0; i < frames; ++i) {
        u32 next = v->pos + 1 < n ? v->pos + 1 : (v->loop ? 0 : v->pos);
        /* 15-bit fraction: a 16-bit sample difference (up to 65535) times
         * 32767 still fits in an i32. */
        i32 f = (i32)(v->frac >> 1);
        i32 l, r;
        if (stereo) {
            i32 l0 = src[v->pos * 2], l1 = src[next * 2];
            i32 r0 = src[v->pos * 2 + 1], r1 = src[next * 2 + 1];
            l = l0 + (((l1 - l0) * f) >> 15);
            r = r0 + (((r1 - r0) * f) >> 15);
        } else {
            i32 a = src[v->pos], b = src[next];
            l = r = a + (((b - a) * f) >> 15);
        }
        acc[i * 2] += (l * v->gain_l) >> 8;
        acc[i * 2 + 1] += (r * v->gain_r) >> 8;

        u32 adv = v->frac + v->step;
        v->pos += adv >> 16;
        v->frac = adv & 0xFFFF;
        if (v->pos >= n) { /* finished: stop now so is_playing is accurate */
            if (!v->loop) { v->active = false; return; }
            v->pos %= n;
        }
    }
}

void ub_mixer_mix(i16 *out, u32 frames)
{
    while (frames) {
        u32 chunk = frames > 1024 ? 1024 : frames;
        for (u32 i = 0; i < chunk * 2; ++i) g_accum[i] = 0;

        LOCK();
        for (int k = 0; k < UB_MIXER_VOICES; ++k) {
            if (g_voices[k].active) mix_voice(&g_voices[k], g_accum, chunk);
        }
        i32 master = g_master;
        UNLOCK();

        for (u32 i = 0; i < chunk * 2; ++i) {
            i32 x = (g_accum[i] * master) >> 8;
            out[i] = (i16)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
        }
        out += chunk * 2;
        frames -= chunk;
    }
}
