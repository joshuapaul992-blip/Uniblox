#include "audio/ub_sound.h"
#include "audio/ub_mixer.h"
#include "platform/platform.h"

static u32 rd16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 rd32(const u8 *p) { return rd16(p) | (rd16(p + 2) << 16); }

const char *ub_wav_result_name(ub_wav_result r)
{
    switch (r) {
    case UB_WAV_OK: return "ok";
    case UB_WAV_NOT_RIFF: return "not a RIFF/WAVE file";
    case UB_WAV_UNSUPPORTED: return "unsupported format (need PCM 8/16-bit, 1-2 channels)";
    case UB_WAV_CORRUPT: return "corrupt file";
    case UB_WAV_TOO_LARGE: return "too long";
    case UB_WAV_NO_MEMORY: return "out of memory";
    }
    return "?";
}

ub_wav_result ub_wav_parse(const void *data, u32 size, ub_sound *out)
{
    const u8 *p = (const u8 *)data;
    out->samples = NULL;
    out->frames = 0;
    if (size < 12 || rd32(p) != 0x46464952u /* RIFF */ || rd32(p + 8) != 0x45564157u /* WAVE */)
        return UB_WAV_NOT_RIFF;

    u32 channels = 0, rate = 0, bits = 0, block = 0;
    const u8 *pcm = NULL;
    u32 pcm_bytes = 0;

    /* Walk chunks. All arithmetic is done on remaining-byte counts so a
     * huge chunk size cannot overflow a pointer. */
    u32 off = 12;
    while (size - off >= 8) {
        u32 id = rd32(p + off);
        u32 len = rd32(p + off + 4);
        off += 8;
        u32 avail = size - off;
        if (id == 0x20746D66u) { /* "fmt " */
            if (len < 16 || len > avail) return UB_WAV_CORRUPT;
            u32 tag = rd16(p + off);
            channels = rd16(p + off + 2);
            rate = rd32(p + off + 4);
            block = rd16(p + off + 12);
            bits = rd16(p + off + 14);
            if (tag == 0xFFFE && len >= 40) {
                /* WAVE_FORMAT_EXTENSIBLE: sub-format GUID starts at +24;
                 * its first two bytes are the real format tag. */
                tag = rd16(p + off + 24);
            }
            if (tag != 1) return UB_WAV_UNSUPPORTED;
        } else if (id == 0x61746164u) { /* "data" */
            pcm = p + off;
            pcm_bytes = len > avail ? avail : len; /* tolerate truncated files */
            break;
        }
        if (len > avail) return UB_WAV_CORRUPT;
        off += len;
        if ((len & 1) && off < size) off++; /* chunks are word aligned */
    }

    if (!pcm || channels == 0) return UB_WAV_CORRUPT;
    if (channels > 2 || (bits != 8 && bits != 16)) return UB_WAV_UNSUPPORTED;
    if (rate < 1000 || rate > 96000) return UB_WAV_UNSUPPORTED;
    if (block != channels * (bits / 8)) return UB_WAV_CORRUPT;

    u32 frames = pcm_bytes / block;
    if (frames == 0) return UB_WAV_CORRUPT;
    if (frames > UB_WAV_MAX_FRAMES) return UB_WAV_TOO_LARGE;

    u32 count = frames * channels;
    i16 *s = (i16 *)plat_alloc((size_t)count * sizeof(i16));
    if (!s) return UB_WAV_NO_MEMORY;
    if (bits == 8) {
        for (u32 i = 0; i < count; ++i) s[i] = (i16)(((i32)pcm[i] - 128) * 256);
    } else {
        for (u32 i = 0; i < count; ++i) s[i] = (i16)rd16(pcm + i * 2);
    }

    out->samples = s;
    out->frames = frames;
    out->rate = rate;
    out->channels = channels;
    return UB_WAV_OK;
}

void ub_sound_free(ub_sound *s)
{
    if (!s || !s->samples) return;
    ub_mixer_stop_sound(s);
    plat_free(s->samples);
    s->samples = NULL;
    s->frames = 0;
}
