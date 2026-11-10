/* Uniblox - host unit tests for the OS-independent engine code.
 * Build and run with:  make test
 * (compiled natively with AddressSanitizer + UBSan, not cross-compiled) */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio/ub_mixer.h"
#include "audio/ub_sound.h"
#include "core/ub_math.h"
#include "core/ub_path.h"
#include "core/ub_string.h"
#include "core/ub_utf.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "test.h"

int g_failures, g_checks;

/* --------------------------------------------------------------- fmt */

static void test_fmt(void)
{
    {
        char nb[32];
        f32 v;
        ub_fmt_num(nb, sizeof nb, 2.0f, 3); CHECK_STR(nb, "2");
        ub_fmt_num(nb, sizeof nb, 0.5f, 3); CHECK_STR(nb, "0.5");
        ub_fmt_num(nb, sizeof nb, -1.25f, 3); CHECK_STR(nb, "-1.25");
        ub_fmt_num(nb, sizeof nb, 3.14159f, 2); CHECK_STR(nb, "3.14");
        ub_fmt_num(nb, sizeof nb, 0.05f, 2); CHECK_STR(nb, "0.05");
        ub_fmt_num(nb, sizeof nb, -0.0001f, 3); CHECK_STR(nb, "0");
        ub_fmt_num(nb, sizeof nb, 196.2f, 3); CHECK_STR(nb, "196.2");
        CHECK(ub_parse_num(" 12 ", &v) && v == 12);
        CHECK(ub_parse_num("-0.5", &v) && v == -0.5f);
        CHECK(ub_parse_num(".25", &v) && v == 0.25f);
        CHECK(ub_parse_num("3.", &v) && v == 3);
        CHECK(ub_parse_num("1e3", &v) && v == 1000);
        CHECK(!ub_parse_num("", &v) && !ub_parse_num("-", &v) && !ub_parse_num("1x", &v) && !ub_parse_num("1e", &v));
    }
    char b[64];
    ub_fmt(b, sizeof b, "%d %d %u", 42, -7, 4000000000u); CHECK_STR(b, "42 -7 4000000000");
    ub_fmt(b, sizeof b, "%d", (i32)0x80000000u);           CHECK_STR(b, "-2147483648");
    ub_fmt(b, sizeof b, "[%5d][%05d][%-3d]", 42, -42, 1);   CHECK_STR(b, "[   42][-0042][%-3d]");
    ub_fmt(b, sizeof b, "%x %X %08x", 255, 255, 0xBEEFu);   CHECK_STR(b, "ff FF 0000beef");
    ub_fmt(b, sizeof b, "%s|%6s|%c|%%", "hi", "ab", 'z');   CHECK_STR(b, "hi|    ab|z|%");
    ub_fmt(b, sizeof b, "%s", (const char *)NULL);           CHECK_STR(b, "(null)");
    int n = ub_fmt(b, 6, "hello world");
    CHECK(n == 11);
    CHECK_STR(b, "hello");
    ub_fmt(b, sizeof b, "end%");                             CHECK_STR(b, "end");
}

/* --------------------------------------------------------------- utf */

static void test_utf(void)
{
    const char *s = "h\xC3\xA9llo \xE2\x82\xAC \xF0\x9D\x84\x9E"; /* hé llo € 𝄞 */
    u16 w[32];
    int n = ub_utf8_to_utf16(s, w, 32);
    CHECK(n == 10);
    CHECK(w[1] == 0xE9 && w[6] == 0x20AC);
    CHECK(w[8] == 0xD834 && w[9] == 0xDD1E); /* surrogate pair */
    char back[64];
    ub_utf16_to_utf8(w, back, sizeof back);
    CHECK_STR(back, s);

    const char *bad = "\xC0\xAF" "a" "\xED\xA0\x80" "\xFF" "\xC3" "b";
    const char *p = bad;
    CHECK(ub_utf8_decode(&p) == 0xFFFD);  /* overlong '/' (2 bytes) */
    CHECK(ub_utf8_decode(&p) == 'a');
    CHECK(ub_utf8_decode(&p) == 0xFFFD);  /* encoded surrogate */
    CHECK(ub_utf8_decode(&p) == 0xFFFD);  /* FF */
    CHECK(ub_utf8_decode(&p) == 0xFFFD);  /* C3 without continuation */
    CHECK(ub_utf8_decode(&p) == 'b');     /* ...does not swallow the next char */
    CHECK(ub_utf8_decode(&p) == 0);

    /* truncation never splits a character */
    char small[5];
    ub_utf16_to_utf8(w, small, sizeof small); /* "h" + é(2) + "l" = 4 bytes */
    CHECK_STR(small, "h\xC3\xA9l");
    u16 tiny[3];
    CHECK(ub_utf8_to_utf16("\xF0\x9D\x84\x9E", tiny, 2) == 0); /* pair needs 2 + NUL */
}

/* -------------------------------------------------------------- path */

static void test_path(void)
{
    CHECK(ub_path_is_safe("uniblox.log"));
    CHECK(ub_path_is_safe("sounds/jump.wav"));
    CHECK(ub_path_is_safe("my places/castle-2_final.ubx"));
    CHECK(ub_path_is_safe(".hidden"));
    CHECK(ub_path_is_safe("console.txt"));
    CHECK(ub_path_is_safe("com10"));
    CHECK(ub_path_is_safe("com"));

    CHECK(!ub_path_is_safe(""));
    CHECK(!ub_path_is_safe(NULL));
    CHECK(!ub_path_is_safe("/etc/passwd"));
    CHECK(!ub_path_is_safe("sounds/"));
    CHECK(!ub_path_is_safe("a//b"));
    CHECK(!ub_path_is_safe(".."));
    CHECK(!ub_path_is_safe("../uniblox.exe"));
    CHECK(!ub_path_is_safe("a/../../b"));
    CHECK(!ub_path_is_safe("./a"));
    CHECK(!ub_path_is_safe("C:x.txt"));
    CHECK(!ub_path_is_safe("a\\b"));
    CHECK(!ub_path_is_safe("CON"));
    CHECK(!ub_path_is_safe("con.txt"));
    CHECK(!ub_path_is_safe("sounds/NUL.wav"));
    CHECK(!ub_path_is_safe("com1"));
    CHECK(!ub_path_is_safe("LPT9.log"));
    CHECK(!ub_path_is_safe("clock$"));
    CHECK(!ub_path_is_safe("file."));
    CHECK(!ub_path_is_safe("file "));
    CHECK(!ub_path_is_safe(" file"));
    CHECK(!ub_path_is_safe("a*b"));
    CHECK(!ub_path_is_safe("tab\there"));
    char longp[300];
    memset(longp, 'a', sizeof longp - 1);
    longp[sizeof longp - 1] = 0;
    CHECK(!ub_path_is_safe(longp));
}

/* -------------------------------------------------------------- math */

static void test_math(void)
{
    double worst = 0;
    for (int i = -200000; i <= 200000; ++i) {
        f32 x = (f32)i * 0.0005f; /* -100 .. 100 rad */
        double es = fabs((double)ub_sinf(x) - sin((double)x));
        double ec = fabs((double)ub_cosf(x) - cos((double)x));
        if (es > worst) worst = es;
        if (ec > worst) worst = ec;
    }
    printf("  sin/cos max error over [-100, 100]: %.2e\n", worst);
    CHECK(worst < 5e-5);
    CHECK(fabs(ub_wrap_angle(7.0f) - (7.0 - 2 * M_PI)) < 1e-5);
    CHECK(fabs(ub_wrap_angle(-4.0f) - (-4.0 + 2 * M_PI)) < 1e-5);
    CHECK(ub_roundf(2.5f) == 3 && ub_roundf(-2.5f) == -3 && ub_roundf(-0.4f) == 0);
    double worst_atan = 0;
    for (int i = 0; i < 3600; ++i) {
        double a = i * (2 * M_PI / 3600) - M_PI;
        for (double r = 0.01; r < 1000; r *= 10) {
            f32 y = (f32)(sin(a) * r), x = (f32)(cos(a) * r);
            double e = fabs(remainder((double)ub_atan2f(y, x) - atan2((double)y, (double)x), 2 * M_PI));
            if (e > worst_atan) worst_atan = e;
        }
    }
    printf("  atan2 max error: %.2e\n", worst_atan);
    CHECK(worst_atan < 2e-5);
    CHECK(ub_atan2f(0, 0) == 0.0f);
}

/* --------------------------------------------------------------- wav */

static u8 g_wav[256];

static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

/* Builds a WAV; returns its size. data_len_field lets tests lie. */
static u32 make_wav(u32 tag, u32 ch, u32 rate, u32 bits, const void *pcm, u32 pcm_len,
                    u32 data_len_field, bool junk_chunk)
{
    u8 *p = g_wav;
    memcpy(p, "RIFF", 4);
    memcpy(p + 8, "WAVE", 4);
    u32 off = 12;
    if (junk_chunk) { /* odd-sized chunk to test word-alignment padding */
        memcpy(p + off, "LIST", 4);
        put32(p + off + 4, 3);
        memcpy(p + off + 8, "abc\0", 4);
        off += 12;
    }
    memcpy(p + off, "fmt ", 4);
    put32(p + off + 4, 16);
    put16(p + off + 8, tag);
    put16(p + off + 10, ch);
    put32(p + off + 12, rate);
    put32(p + off + 16, rate * ch * bits / 8);
    put16(p + off + 20, ch * bits / 8);
    put16(p + off + 22, bits);
    off += 24;
    memcpy(p + off, "data", 4);
    put32(p + off + 4, data_len_field);
    memcpy(p + off + 8, pcm, pcm_len);
    off += 8 + pcm_len;
    put32(p + 4, off - 8);
    return off;
}

static void test_wav(void)
{
    ub_sound s;
    i16 st[] = { 100, -100, 200, -200, 300, -300 };
    u32 n = make_wav(1, 2, 22050, 16, st, sizeof st, sizeof st, false);
    CHECK(ub_wav_parse(g_wav, n, &s) == UB_WAV_OK);
    CHECK(s.frames == 3 && s.channels == 2 && s.rate == 22050);
    CHECK(s.samples && s.samples[4] == 300 && s.samples[5] == -300);
    ub_sound_free(&s);
    CHECK(s.samples == NULL);

    u8 m8[] = { 128, 255, 0, 192, 7 };
    n = make_wav(1, 1, 11025, 8, m8, sizeof m8, sizeof m8, true);
    CHECK(ub_wav_parse(g_wav, n, &s) == UB_WAV_OK);
    CHECK(s.frames == 5 && s.samples[0] == 0 && s.samples[1] == 127 * 256 && s.samples[2] == -32768);
    ub_sound_free(&s);

    /* data chunk claims more than the file holds: use what is there */
    n = make_wav(1, 1, 22050, 16, st, sizeof st, 100000, false);
    CHECK(ub_wav_parse(g_wav, n, &s) == UB_WAV_OK && s.frames == 6);
    ub_sound_free(&s);

    CHECK(ub_wav_parse(g_wav, 8, &s) == UB_WAV_NOT_RIFF);
    CHECK(ub_wav_parse("RIFX....WAVE", 12, &s) == UB_WAV_NOT_RIFF);
    n = make_wav(2, 1, 22050, 16, st, sizeof st, sizeof st, false);   /* ADPCM */
    CHECK(ub_wav_parse(g_wav, n, &s) == UB_WAV_UNSUPPORTED);
    n = make_wav(1, 3, 22050, 16, st, sizeof st, sizeof st, false);   /* 3 channels */
    CHECK(ub_wav_parse(g_wav, n, &s) == UB_WAV_UNSUPPORTED);
    n = make_wav(1, 1, 22050, 24, st, sizeof st, sizeof st, false);   /* 24-bit */
    CHECK(ub_wav_parse(g_wav, n, &s) == UB_WAV_UNSUPPORTED);
    n = make_wav(1, 1, 22050, 16, st, 0, 0, false);                   /* empty data */
    CHECK(ub_wav_parse(g_wav, n, &s) == UB_WAV_CORRUPT);

    /* a huge chunk length before "fmt " must not overflow or crash */
    n = make_wav(1, 1, 22050, 16, st, sizeof st, sizeof st, true);
    put32(g_wav + 16, 0xFFFFFFF0u);
    CHECK(ub_wav_parse(g_wav, n, &s) == UB_WAV_CORRUPT);
    /* fmt chunk longer than the file */
    n = make_wav(1, 1, 22050, 16, st, sizeof st, sizeof st, false);
    put32(g_wav + 16, 1000);
    CHECK(ub_wav_parse(g_wav, n, &s) == UB_WAV_CORRUPT);
    /* every truncation of a valid file is handled */
    n = make_wav(1, 2, 22050, 16, st, sizeof st, sizeof st, true);
    for (u32 len = 0; len < n; ++len) {
        ub_wav_result r = ub_wav_parse(g_wav, len, &s);
        if (r == UB_WAV_OK) ub_sound_free(&s);
    }
}

/* ------------------------------------------------------------- mixer */

static i16 g_out[4096 * 2];

static void test_mixer(void)
{
    CHECK(ub_mixer_init(22050));
    ub_mixer_set_master_volume(1.0f);

    i16 flat[100];
    for (int i = 0; i < 100; ++i) flat[i] = 10000;
    ub_sound mono = { flat, 100, 22050, 1 };

    /* centred mono plays at 3/4 gain on both sides, then ends */
    ub_voice v = ub_mixer_play(&mono, ub_play_default());
    CHECK(v != 0 && ub_mixer_is_playing(v));
    ub_mixer_mix(g_out, 50);
    CHECK(g_out[0] == 7500 && g_out[1] == 7500 && g_out[98] == 7500);
    ub_mixer_mix(g_out, 60);
    CHECK(g_out[49 * 2] == 7500 && g_out[50 * 2] == 0); /* ended after frame 100 */
    CHECK(!ub_mixer_is_playing(v) && ub_mixer_active_voices() == 0);

    /* hard right pan */
    ub_play_params p = ub_play_default();
    p.pan = 1.0f;
    v = ub_mixer_play(&mono, p);
    ub_mixer_mix(g_out, 10);
    CHECK(g_out[0] == 0 && g_out[1] == 7500);
    ub_mixer_stop(v);
    CHECK(!ub_mixer_is_playing(v));

    /* double pitch finishes in half the time */
    p = ub_play_default();
    p.pitch = 2.0f;
    v = ub_mixer_play(&mono, p);
    ub_mixer_mix(g_out, 49);
    CHECK(ub_mixer_is_playing(v));
    ub_mixer_mix(g_out, 2);
    CHECK(!ub_mixer_is_playing(v));

    /* an 11025 Hz sound lasts twice as long at 22050, with interpolation */
    i16 ramp[2] = { 0, 10000 };
    ub_sound slow = { ramp, 2, 11025, 1 };
    v = ub_mixer_play(&slow, ub_play_default());
    ub_mixer_mix(g_out, 4);
    CHECK(g_out[0] == 0 && g_out[2] == 3750 && g_out[4] == 7500);
    CHECK(!ub_mixer_is_playing(v));

    /* looping keeps going */
    p = ub_play_default();
    p.loop = true;
    v = ub_mixer_play(&mono, p);
    ub_mixer_mix(g_out, 4096);
    CHECK(ub_mixer_is_playing(v) && g_out[4095 * 2] == 7500);
    ub_mixer_stop_sound(&mono);
    CHECK(!ub_mixer_is_playing(v));

    /* full-scale extremes: interpolation must not overflow (UBSan checks),
     * and the sum clips instead of wrapping */
    i16 loud[2] = { -32768, 32767 };
    ub_sound edge = { loud, 2, 11025, 1 };
    for (int i = 0; i < 8; ++i) ub_mixer_play(&edge, ub_play_default());
    ub_mixer_mix(g_out, 3);
    CHECK(g_out[0] == -32768 && g_out[4] == 32767);
    ub_mixer_stop_all();

    /* voice stealing: the 33rd one-shot replaces the oldest */
    ub_voice first = ub_mixer_play(&mono, ub_play_default());
    for (int i = 1; i < UB_MIXER_VOICES; ++i) ub_mixer_play(&mono, ub_play_default());
    CHECK(ub_mixer_active_voices() == UB_MIXER_VOICES);
    ub_voice extra = ub_mixer_play(&mono, ub_play_default());
    CHECK(extra != 0 && extra != first);
    CHECK(!ub_mixer_is_playing(first) && ub_mixer_is_playing(extra));
    ub_mixer_stop_all();

    /* all looping: nothing to steal */
    p = ub_play_default();
    p.loop = true;
    for (int i = 0; i < UB_MIXER_VOICES; ++i) ub_mixer_play(&mono, p);
    CHECK(ub_mixer_play(&mono, ub_play_default()) == 0);
    ub_mixer_stop_all();

    /* master volume and empty sounds */
    ub_mixer_set_master_volume(0.5f);
    ub_mixer_play(&mono, ub_play_default());
    ub_mixer_mix(g_out, 1);
    CHECK(g_out[0] == 3750);
    ub_sound empty = { NULL, 0, 22050, 1 };
    CHECK(ub_mixer_play(&empty, ub_play_default()) == 0);
    CHECK(ub_mixer_play(NULL, ub_play_default()) == 0);
    CHECK(ub_mixer_play(&mono, ub_play_default()) != 0);
    CHECK(ub_mixer_is_playing(0) == false);
    ub_mixer_shutdown();

    /* no output device: nothing starts */
    CHECK(ub_mixer_init(0));
    CHECK(ub_mixer_play(&mono, ub_play_default()) == 0);
    CHECK(ub_mixer_active_voices() == 0);
    ub_mixer_shutdown();
}

int main(int argc, char **argv)
{
    /* uniblox_tests [name...] runs only the named tests */
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "fmt", test_fmt }, { "utf", test_utf }, { "path", test_path },
        { "math", test_math }, { "wav", test_wav }, { "mixer", test_mixer },
        { "crt", test_crt }, { "compress", test_compress }, { "world", test_world }, { "physics", test_physics }, { "libc", test_libc }, { "script", test_script }, { "game", test_game }, { "ui", test_ui }, { "settings", test_settings }, { "editor", test_editor }, { "editor app", test_editor_app }, { "present", test_present }, { "gui", test_gui }, { "vec", test_vec }, { "palette", test_palette }, { "render", test_render }, { "render 2d", test_render_2d },
        { "render fuzz", test_render_fuzz }, { "meshes", test_meshes },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; ++i) {
        bool wanted = argc < 2;
        for (int a = 1; a < argc; ++a) wanted |= strcmp(argv[a], tests[i].name) == 0;
        if (!wanted) continue;
        printf("%s\n", tests[i].name);
        tests[i].fn();
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
