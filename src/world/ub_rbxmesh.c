/* Uniblox - Roblox .mesh files (milestone 8C), for MeshParts and
 * SpecialMeshes whose meshes the player has put in the content folder
 * (nothing is downloaded). Versions 1.00 and 1.01 are text; 2.00 to 5.00
 * binary, as the community has documented them: a "version x.yy\n" line,
 * a header (its own size first), vertices, faces (three u32 each), then
 * level-of-detail face offsets (3.00+; the first level is used). Bones,
 * skinning and facial animation data are skipped. */
#include "world/ub_rbxmesh.h"
#include "core/ub_string.h"
#include "platform/platform.h"
#include "crt/ub_crt.h"

static u32 le32(const u8 *q) { return q[0] | ((u32)q[1] << 8) | ((u32)q[2] << 16) | ((u32)q[3] << 24); }
static u32 le16(const u8 *q) { return q[0] | ((u32)q[1] << 8); }
static f32 lef(const u8 *q)
{
    u32 u = le32(q);
    f32 f;
    memcpy(&f, &u, 4);
    return f;
}

static bool sane(f32 f) { return f > -1e5f && f < 1e5f; }

/* version 1: "[x,y,z][nx,ny,nz][u,v,w]" for each of a face's 3 vertices */
static bool parse_text(const char *s, const char *end, bool half, f32 **vo, u32 *nvo, u32 **to, u32 *nto)
{
    while (s < end && *s != '\n') ++s; /* version line */
    if (s < end) ++s;
    u32 faces = 0;
    while (s < end && *s >= '0' && *s <= '9') faces = faces * 10 + (u32)(*s++ - '0');
    if (!faces || faces > 500000) return false;
    f32 *v = (f32 *)plat_alloc(sizeof(f32) * faces * 9 + sizeof(u32) * faces * 3);
    if (!v) return false;
    u32 *t = (u32 *)(v + faces * 9);
    u32 nv = 0;
    for (u32 f = 0; f < faces * 3; ++f) {
        /* the position: the first bracket of three */
        while (s < end && *s != '[') ++s;
        if (s >= end) break;
        ++s;
        for (int k = 0; k < 3; ++k) {
            char num[48];
            int n = 0;
            while (s < end && (*s == ' ' || *s == ',')) ++s;
            while (s < end && n < 47 && ((*s >= '0' && *s <= '9') || *s == '-' || *s == '+' || *s == '.' || *s == 'e' || *s == 'E'))
                num[n++] = *s++;
            num[n] = 0;
            f32 x = 0;
            if (!ub_parse_num(num, &x)) x = 0;
            v[nv * 3 + k] = half ? x * 0.5f : x;
        }
        if (!sane(v[nv * 3]) || !sane(v[nv * 3 + 1]) || !sane(v[nv * 3 + 2])) break;
        t[nv] = nv;
        nv++;
        for (int skip = 0; skip < 2; ++skip) { /* normal and uv brackets */
            while (s < end && *s != '[') ++s;
            while (s < end && *s != ']') ++s;
        }
    }
    if (nv != faces * 3) { /* cut short or damaged */
        plat_free(v);
        return false;
    }
    *vo = v;
    *nvo = nv;
    *to = t;
    *nto = nv / 3;
    return true;
}

bool ub_rbxmesh_parse(const u8 *d, u32 len, f32 **vo, u32 *nvo, u32 **to, u32 *nto)
{
    *vo = NULL;
    *to = NULL;
    *nvo = *nto = 0;
    if (len < 13 || memcmp(d, "version ", 8) != 0) return false;
    int major = d[8] - '0';
    if (major == 1) return parse_text((const char *)d, (const char *)d + len, d[11] == '0' && d[12] == '0', vo, nvo, to, nto);
    if (major < 2 || major > 5) return false;
    u32 p = 0;
    while (p < len && d[p] != '\n') ++p;
    ++p; /* after the version line */
    if (len - p < 12) return false;
    u32 hsize = le16(d + p);
    u32 vsize, nverts, nfaces, nlods = 0, lod0_start = 0, lod0_end = 0;
    u32 skip_after_verts = 0;
    if (major == 2 || major == 3) {
        vsize = d[p + 2];
        if (d[p + 3] != 12) return false;
        if (major == 2) {
            nverts = le32(d + p + 4);
            nfaces = le32(d + p + 8);
        } else {
            if (len - p < 16) return false;
            nlods = le16(d + p + 6);
            nverts = le32(d + p + 8);
            nfaces = le32(d + p + 12);
        }
    } else {
        if (len - p < 24) return false;
        nverts = le32(d + p + 4);
        nfaces = le32(d + p + 8);
        nlods = le16(d + p + 12);
        u32 bones = le16(d + p + 14);
        vsize = 40;
        if (bones) skip_after_verts = 8; /* per vertex: skinning */
    }
    if (vsize < 12 || nverts == 0 || nverts > 1000000 || nfaces == 0 || nfaces > 2000000) return false;
    p += hsize;
    u32 vbytes = vsize + skip_after_verts;
    if (p > len || (u64)nverts * vsize > len - p) return false;
    u32 vstart = p;
    p += nverts * vsize;
    if (skip_after_verts) {
        if ((u64)nverts * skip_after_verts > len - p) return false;
        p += nverts * skip_after_verts;
    }
    (void)vbytes;
    if ((u64)nfaces * 12 > len - p) return false;
    u32 fstart = p;
    p += nfaces * 12;
    lod0_end = nfaces;
    if (nlods >= 2 && (u64)nlods * 4 <= len - p) {
        lod0_start = le32(d + p);
        lod0_end = le32(d + p + 4);
        if (lod0_start >= lod0_end || lod0_end > nfaces) {
            lod0_start = 0;
            lod0_end = nfaces;
        }
    }
    u32 nt = lod0_end - lod0_start;
    f32 *v = (f32 *)plat_alloc(sizeof(f32) * nverts * 3 + sizeof(u32) * nt * 3);
    if (!v) return false;
    u32 *t = (u32 *)(v + nverts * 3);
    for (u32 i = 0; i < nverts; ++i) {
        const u8 *q = d + vstart + i * vsize;
        v[i * 3] = lef(q);
        v[i * 3 + 1] = lef(q + 4);
        v[i * 3 + 2] = lef(q + 8);
        if (!sane(v[i * 3]) || !sane(v[i * 3 + 1]) || !sane(v[i * 3 + 2])) {
            plat_free(v);
            return false;
        }
    }
    u32 k = 0;
    for (u32 f = lod0_start; f < lod0_end; ++f) {
        const u8 *q = d + fstart + f * 12;
        u32 a = le32(q), b = le32(q + 4), c = le32(q + 8);
        if (a >= nverts || b >= nverts || c >= nverts) continue;
        t[k * 3] = a;
        t[k * 3 + 1] = b;
        t[k * 3 + 2] = c;
        k++;
    }
    if (!k) {
        plat_free(v);
        return false;
    }
    *vo = v;
    *nvo = nverts;
    *to = t;
    *nto = k;
    return true;
}
