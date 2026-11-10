/* Uniblox - Sound objects (milestone 8C). A Sound plays the WAV file the
 * player has put in the content folder for its SoundId
 * (content/<asset id>.wav, or content/<path> for rbxasset://...);
 * nothing is downloaded. Play, Stop, Pause, Resume, Playing, IsPlaying,
 * IsLoaded, TimeLength, TimePosition; Played, Ended, Stopped, Paused,
 * Resumed and Loaded. A Sound in a part is quieter with distance
 * (RollOffMinDistance..RollOffMaxDistance) when it starts. Without a sound
 * device, or the file, sounds still report playing for their length. */
#include <string.h>

#include "audio/ub_mixer.h"
#include "core/ub_path.h"
#include "platform/platform.h"
#include "script/ub_sapi.h"

#define W (&S.g->world)
#define MAX_SOUNDS 64
#define MAX_FILES 128

static bool streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

typedef struct {
    char path[UB_PATH_MAX + 16];
    ub_sound snd;
    bool ok;
} sfile;

typedef struct {
    i32 inst;
    ub_voice voice;
    f64 started;          /* script time it (re)started */
    f64 offset;           /* seconds already played before that (resume) */
    f64 length;           /* seconds, 0 if unknown */
    bool playing, paused;
} splay;

static sfile *g_files;
static int g_nfiles;
static splay g_play[MAX_SOUNDS];
static int g_nplay;

static const ub_value *prop(i32 i, const char *k) { return ub_world_prop_named(W, i, k); }
static f64 fnum(i32 i, const char *k, f64 d)
{
    const ub_value *v = prop(i, k);
    return !v ? d : v->type == UB_PV_FLOAT ? v->v.f : v->type == UB_PV_DOUBLE ? v->v.d : v->type == UB_PV_BOOL ? v->v.b : d;
}

static const ub_sound *load(i32 inst)
{
    const ub_value *id = prop(inst, "SoundId");
    if (!id || id->type != UB_PV_STRING || !id->v.str.len) return NULL;
    char path[UB_PATH_MAX + 16];
    if (!ub_content_path(id->v.str.s, path, (int)sizeof path, "wav")) return NULL;
    for (int i = 0; i < g_nfiles; ++i)
        if (streq(g_files[i].path, path)) return g_files[i].ok ? &g_files[i].snd : NULL;
    if (!g_files) g_files = (sfile *)plat_alloc(sizeof(sfile) * MAX_FILES);
    if (!g_files || g_nfiles >= MAX_FILES) return NULL;
    sfile *f = &g_files[g_nfiles++];
    memset(f, 0, sizeof *f);
    snprintf(f->path, sizeof f->path, "%s", path);
    void *data = NULL;
    u32 size = 0;
    if (ub_path_is_safe(path) && plat_file_exists(path) && plat_file_read_all(path, 32u << 20, &data, &size))
        f->ok = ub_wav_parse(data, size, &f->snd) == UB_WAV_OK;
    plat_free(data);
    return f->ok ? &f->snd : NULL;
}

static splay *find(i32 inst, bool make)
{
    for (int i = 0; i < g_nplay; ++i)
        if (g_play[i].inst == inst) return &g_play[i];
    if (!make || g_nplay >= MAX_SOUNDS) return NULL;
    splay *p = &g_play[g_nplay++];
    memset(p, 0, sizeof *p);
    p->inst = inst;
    return p;
}

static bool is_sound(i32 i)
{
    return i >= 0 && ub_world_alive(W, i) && streq(ub_world_str(W, W->inst[i].class_name), "Sound");
}

static void set_playing_prop(i32 inst, bool on)
{
    ub_value v = ub_vbool(on);
    ub_world_set_prop_named(W, inst, "Playing", &v);
}

static void start(lua_State *L, i32 inst, f64 from, int ev)
{
    splay *p = find(inst, true);
    if (!p) return;
    if (p->voice) ub_mixer_stop(p->voice);
    const ub_sound *s = load(inst);
    p->length = s && s->rate ? (f64)s->frames / (f64)s->rate : 0;
    f64 speed = fnum(inst, "PlaybackSpeed", 1);
    if (speed > 0 && p->length > 0) p->length /= speed;
    f32 vol = (f32)fnum(inst, "Volume", 0.5);
    /* in a part: quieter with distance from the character */
    i32 parent = W->inst[inst].parent;
    if (parent >= 0 && W->inst[parent].part >= 0 && S.g->char_model >= 0) {
        vec3 d = v3_sub(W->parts[W->inst[parent].part].pos, v3_add(S.g->feet, v3(0, 3, 0)));
        f32 dist = v3_length(d), lo = (f32)fnum(inst, "RollOffMinDistance", 10), hi = (f32)fnum(inst, "RollOffMaxDistance", 10000);
        if (dist > lo) vol *= dist >= hi ? 0.0f : lo / dist;
    }
    ub_play_params pp = ub_play_default();
    pp.volume = vol < 0 ? 0 : vol;
    pp.pitch = (f32)(speed > 0 ? speed : 1);
    pp.loop = fnum(inst, "Looped", 0) != 0;
    p->voice = s ? ub_mixer_play(s, pp) : 0;
    p->started = script_time();
    p->offset = from;
    p->playing = true;
    p->paused = false;
    set_playing_prop(inst, true);
    if (ev >= 0) {
        lua_pushstring(L, "");
        fire_signal(L, inst, ev, NULL, 1);
    }
}

static void halt(lua_State *L, i32 inst, bool pause, int ev)
{
    splay *p = find(inst, false);
    if (!p || !p->playing) return;
    if (p->voice) ub_mixer_stop(p->voice);
    p->voice = 0;
    p->offset = pause ? p->offset + (script_time() - p->started) : 0;
    p->playing = false;
    p->paused = pause;
    set_playing_prop(inst, false);
    if (ev >= 0) {
        lua_pushstring(L, "");
        fire_signal(L, inst, ev, NULL, 1);
    }
}

static int m_play(lua_State *L) { start(L, check_inst(L, 1), 0, EV_PLAYED); return 0; }
static int m_stop(lua_State *L) { halt(L, check_inst(L, 1), false, EV_STOPPED); return 0; }
static int m_pause(lua_State *L) { halt(L, check_inst(L, 1), true, EV_PAUSED); return 0; }
static int m_resume(lua_State *L)
{
    i32 i = check_inst(L, 1);
    splay *p = find(i, false);
    start(L, i, p && p->paused ? p->offset : 0, EV_RESUMED);
    return 0;
}

static int m_playlocal(lua_State *L) /* SoundService:PlayLocalSound(sound) */
{
    i32 i = check_inst(L, 2);
    if (is_sound(i)) start(L, i, 0, EV_PLAYED);
    return 0;
}

lua_CFunction sound_method(i32 idx, const char *k)
{
    if (idx == VIRT_SOUNDSERVICE && streq(k, "PlayLocalSound")) return m_playlocal;
    if (!is_sound(idx)) return NULL;
    if (streq(k, "Play")) return m_play;
    if (streq(k, "Stop")) return m_stop;
    if (streq(k, "Pause")) return m_pause;
    if (streq(k, "Resume")) return m_resume;
    return NULL;
}

int sound_event(i32 idx, const char *k)
{
    if (!is_sound(idx)) return -1;
    if (streq(k, "Played")) return EV_PLAYED;
    if (streq(k, "Ended") || streq(k, "DidLoop")) return streq(k, "Ended") ? EV_ENDED : EV_NEVER;
    if (streq(k, "Stopped")) return EV_STOPPED;
    if (streq(k, "Paused")) return EV_PAUSED;
    if (streq(k, "Resumed")) return EV_RESUMED;
    if (streq(k, "Loaded")) return EV_SOUNDLOADED;
    return -1;
}

bool sound_get(lua_State *L, i32 idx, const char *k)
{
    if (!is_sound(idx)) return false;
    splay *p = find(idx, false);
    if (streq(k, "IsPlaying") || streq(k, "Playing")) lua_pushboolean(L, p && p->playing);
    else if (streq(k, "IsPaused")) lua_pushboolean(L, p && p->paused);
    else if (streq(k, "IsLoaded")) lua_pushboolean(L, load(idx) != NULL);
    else if (streq(k, "TimeLength")) {
        const ub_sound *s = load(idx);
        lua_pushnumber(L, s && s->rate ? (f64)s->frames / (f64)s->rate : 0);
    } else if (streq(k, "TimePosition")) {
        f64 t = !p ? 0 : p->playing ? p->offset + (script_time() - p->started) : p->offset;
        if (p && p->length > 0 && t > p->length) t = fnum(idx, "Looped", 0) != 0 ? t - p->length * (f64)(i64)(t / p->length) : p->length;
        lua_pushnumber(L, t);
    } else return false;
    return true;
}

bool sound_set(lua_State *L, i32 idx, const char *k, int v)
{
    if (!is_sound(idx) || !streq(k, "Playing")) return false;
    if (lua_toboolean(L, v)) start(L, idx, 0, EV_PLAYED);
    else halt(L, idx, false, EV_STOPPED);
    return true;
}

/* each frame: sounds that finished; sounds placed playing start */
static bool g_first = true;

void sound_pump(lua_State *L)
{
    if (g_first) { /* a place's sounds with Playing on start once the game does */
        g_first = false;
        for (i32 i = 0; i < W->count; ++i)
            if (is_sound(i) && fnum(i, "Playing", 0) != 0 && sapi_in_game(i)) start(L, i, 0, -1);
    }
    f64 now = script_time();
    for (int k = 0; k < g_nplay;) {
        splay *p = &g_play[k];
        if (!ub_world_alive(W, p->inst)) {
            if (p->voice) ub_mixer_stop(p->voice);
            g_play[k] = g_play[--g_nplay];
            continue;
        }
        bool looped = fnum(p->inst, "Looped", 0) != 0;
        if (p->playing && !looped) {
            bool done = p->voice ? !ub_mixer_is_playing(p->voice) : (p->length <= 0 || now - p->started + p->offset >= p->length);
            if (done) {
                p->playing = false;
                p->voice = 0;
                p->offset = 0;
                set_playing_prop(p->inst, false);
                lua_pushstring(L, "");
                fire_signal(L, p->inst, EV_ENDED, NULL, 1);
                continue; /* (the handler may have played it again) */
            }
        }
        ++k;
    }
}

void sound_reset(void)
{
    g_first = true;
    for (int i = 0; i < g_nplay; ++i)
        if (g_play[i].voice) ub_mixer_stop(g_play[i].voice);
    g_nplay = 0;
    for (int i = 0; i < g_nfiles; ++i) ub_sound_free(&g_files[i].snd);
    plat_free(g_files);
    g_files = NULL;
    g_nfiles = 0;
}
