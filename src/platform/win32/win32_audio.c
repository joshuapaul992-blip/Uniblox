/* Uniblox - Win32 audio output through waveOut (MME).
 *
 * waveOut exists on every Windows NT version. DirectSound is not used: it
 * is missing on NT 3.51 and only emulated (slowly) on NT 4.0.
 *
 * A small queue of buffers is kept in the driver. A dedicated thread polls
 * every few milliseconds for buffers the driver has finished with, refills
 * them from the mixer and queues them again. Polling (CALLBACK_NULL) is
 * used instead of event/thread callbacks because it behaves the same on
 * every driver and OS version. */
#include "platform/win32/win32_internal.h"
#include "audio/ub_mixer.h"
#include "core/ub_log.h"
#include "core/ub_string.h"

#define AUDIO_BUFFERS 6
/* Frames per buffer at 22050 Hz: 512 frames = 23 ms, so the whole queue
 * is ~140 ms. Scaled for other rates. */
#define AUDIO_FRAMES_AT_22K 512

static HWAVEOUT g_wo;
static WAVEHDR g_hdr[AUDIO_BUFFERS];
static u8 *g_mem;
static i16 *g_mix;
static u32 g_frames;          /* per buffer */
static u32 g_channels, g_bits;
static HANDLE g_thread, g_stop;
static volatile LONG g_underruns;

typedef struct {
    u32 rate, bits, channels;
} audio_format;

/* Tried in order. 8-bit and 11 kHz cover very old cards (Sound Blaster
 * 1.x/2.0 drivers on NT 3.51 may only do 8-bit mono). */
static const audio_format k_formats[] = {
    { 22050, 16, 2 },
    { 22050, 16, 1 },
    { 22050, 8, 2 },
    { 22050, 8, 1 },
    { 11025, 8, 1 },
};

static void fill_buffer(WAVEHDR *h)
{
    ub_mixer_mix(g_mix, g_frames);
    const i16 *src = g_mix;
    if (g_bits == 16 && g_channels == 2) {
        i16 *dst = (i16 *)h->lpData;
        for (u32 i = 0; i < g_frames * 2; ++i) dst[i] = src[i];
    } else if (g_bits == 16) {
        i16 *dst = (i16 *)h->lpData;
        for (u32 i = 0; i < g_frames; ++i) dst[i] = (i16)(((i32)src[i * 2] + src[i * 2 + 1]) >> 1);
    } else if (g_channels == 2) {
        u8 *dst = (u8 *)h->lpData;
        for (u32 i = 0; i < g_frames * 2; ++i) dst[i] = (u8)((src[i] >> 8) + 128);
    } else {
        u8 *dst = (u8 *)h->lpData;
        for (u32 i = 0; i < g_frames; ++i)
            dst[i] = (u8)(((((i32)src[i * 2] + src[i * 2 + 1]) >> 1) >> 8) + 128);
    }
}

static DWORD WINAPI audio_thread(LPVOID arg)
{
    (void)arg;
    while (WaitForSingleObject(g_stop, 5) == WAIT_TIMEOUT) {
        int done = 0;
        for (int i = 0; i < AUDIO_BUFFERS; ++i) {
            volatile DWORD *flags = &g_hdr[i].dwFlags;
            if (*flags & WHDR_DONE) {
                fill_buffer(&g_hdr[i]);
                waveOutWrite(g_wo, &g_hdr[i], sizeof(WAVEHDR));
                done++;
            }
        }
        /* Every buffer drained at once means the queue ran dry. */
        if (done == AUDIO_BUFFERS) InterlockedIncrement((LONG *)&g_underruns);
    }
    return 0;
}

static bool try_open(UINT device, const audio_format *f)
{
    /* PCMWAVEFORMAT (no cbSize field) is the structure NT 3.51's waveOut
     * expects; later versions accept it too for WAVE_FORMAT_PCM. */
    PCMWAVEFORMAT pf;
    memset(&pf, 0, sizeof pf);
    pf.wf.wFormatTag = WAVE_FORMAT_PCM;
    pf.wf.nChannels = (WORD)f->channels;
    pf.wf.nSamplesPerSec = f->rate;
    pf.wf.nBlockAlign = (WORD)(f->channels * f->bits / 8);
    pf.wf.nAvgBytesPerSec = f->rate * pf.wf.nBlockAlign;
    pf.wBitsPerSample = (WORD)f->bits;
    return waveOutOpen(&g_wo, device, (LPWAVEFORMATEX)&pf, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR;
}

bool win32_audio_open(ub_system_info *sys)
{
    UINT ndev = waveOutGetNumDevs();
    const audio_format *chosen = NULL;
    UINT chosen_dev = 0;

    for (int k = 0; k < UB_ARRAY_COUNT(k_formats) && !chosen && ndev; ++k) {
        /* WAVE_MAPPER first (the user's preferred device), then each
         * device directly in case the mapper is missing (NT 3.51). */
        if (try_open(WAVE_MAPPER, &k_formats[k])) {
            chosen = &k_formats[k];
            chosen_dev = WAVE_MAPPER;
            break;
        }
        for (UINT d = 0; d < ndev; ++d) {
            if (try_open(d, &k_formats[k])) {
                chosen = &k_formats[k];
                chosen_dev = d;
                break;
            }
        }
    }

    if (!chosen) {
        ub_mixer_init(0); /* no device: play() becomes a no-op */
        ub_fmt(sys->audio_desc, sizeof sys->audio_desc, "%s",
               ndev ? "audio device would not open" : "no audio device");
        ub_warn("audio: %s (%u devices); continuing without sound", sys->audio_desc, ndev);
        return false;
    }

    g_channels = chosen->channels;
    g_bits = chosen->bits;
    g_frames = AUDIO_FRAMES_AT_22K * chosen->rate / 22050;
    u32 block = g_channels * g_bits / 8;
    u32 buf_bytes = g_frames * block;
    g_mem = (u8 *)plat_alloc((size_t)buf_bytes * AUDIO_BUFFERS);
    g_mix = (i16 *)plat_alloc((size_t)g_frames * 2 * sizeof(i16));
    if (!g_mem || !g_mix) {
        waveOutClose(g_wo);
        g_wo = NULL;
        plat_free(g_mem);
        plat_free(g_mix);
        g_mem = NULL;
        g_mix = NULL;
        ub_mixer_init(0);
        ub_fmt(sys->audio_desc, sizeof sys->audio_desc, "audio: out of memory");
        ub_error("audio: out of memory allocating buffers");
        return false;
    }

    ub_mixer_init(chosen->rate);
    for (int i = 0; i < AUDIO_BUFFERS; ++i) {
        WAVEHDR *h = &g_hdr[i];
        memset(h, 0, sizeof *h);
        h->lpData = (LPSTR)(g_mem + (size_t)i * buf_bytes);
        h->dwBufferLength = buf_bytes;
        waveOutPrepareHeader(g_wo, h, sizeof(WAVEHDR));
        fill_buffer(h);
        waveOutWrite(g_wo, h, sizeof(WAVEHDR));
    }

    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD tid;
    g_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, &tid);
    if (g_thread) SetThreadPriority(g_thread, THREAD_PRIORITY_HIGHEST);

    ub_fmt(sys->audio_desc, sizeof sys->audio_desc, "%u Hz %u-bit %s",
           chosen->rate, chosen->bits, chosen->channels == 2 ? "stereo" : "mono");
    ub_info("audio: %s on %s, %u buffers x %u frames (%u ms queued)",
            sys->audio_desc, chosen_dev == WAVE_MAPPER ? "wave mapper" : "device",
            AUDIO_BUFFERS, g_frames, AUDIO_BUFFERS * g_frames * 1000u / chosen->rate);
    return g_thread != NULL;
}

void win32_audio_close(void)
{
    if (!g_wo) return;
    if (g_thread) {
        SetEvent(g_stop);
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_stop) {
        CloseHandle(g_stop);
        g_stop = NULL;
    }
    waveOutReset(g_wo);
    for (int i = 0; i < AUDIO_BUFFERS; ++i) waveOutUnprepareHeader(g_wo, &g_hdr[i], sizeof(WAVEHDR));
    waveOutClose(g_wo);
    g_wo = NULL;
    if (g_underruns) ub_info("audio: %u buffer underruns", (u32)g_underruns);
    plat_free(g_mem);
    plat_free(g_mix);
    g_mem = NULL;
    g_mix = NULL;
}
