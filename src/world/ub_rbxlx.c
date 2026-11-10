/* Uniblox - Roblox XML places and models (.rbxlx, .rbxmx), milestone 8C.
 *
 * Rather than a second importer, the XML is turned into the binary format
 * (docs/xml.md and docs/binary.md in rbx-dom describe both) in memory and
 * read by the binary importer, so everything it keeps (parts, meshes,
 * scripts, GUIs, attributes...) comes from XML files too. Chunks are
 * written uncompressed. Properties of types Uniblox does not keep are
 * left out. */
#include "world/ub_rbxlx.h"
#include "core/ub_string.h"
#include "platform/platform.h"
#include "crt/ub_crt.h"

/* ------------------------------------------------------------ XML nodes */

typedef struct {
    const char *tag;
    u32 tag_len;
    const char *attrs;    /* the raw attribute text */
    u32 attrs_len;
    const char *text;     /* raw content up to the first child or the end tag */
    u32 text_len;
    i32 parent, first_child, last_child, next;
} xnode;

typedef struct {
    xnode *n;
    i32 count, cap;
    bool bad;
} xdoc;

static i32 xnew(xdoc *d)
{
    if (d->count == d->cap) {
        i32 nc = d->cap ? d->cap * 2 : 1024;
        xnode *nn = (xnode *)plat_alloc(sizeof(xnode) * (size_t)nc);
        if (!nn) {
            d->bad = true;
            return -1;
        }
        if (d->n) {
            memcpy(nn, d->n, sizeof(xnode) * (size_t)d->count);
            plat_free(d->n);
        }
        d->n = nn;
        d->cap = nc;
    }
    xnode *x = &d->n[d->count];
    memset(x, 0, sizeof *x);
    x->parent = x->first_child = x->last_child = x->next = -1;
    return d->count++;
}

static bool starts(const char *s, const char *e, const char *lit)
{
    while (*lit) {
        if (s >= e || *s != *lit) return false;
        ++s;
        ++lit;
    }
    return true;
}

/* A tolerant parser: elements, attributes, text, CDATA, comments,
 * declarations. Text is kept raw (entities decoded later). */
static bool xparse(xdoc *d, const char *s, u32 len)
{
    const char *p = s, *e = s + len;
    i32 cur = -1;
    while (p < e && !d->bad) {
        if (*p != '<') {
            ++p;
            continue;
        }
        if (starts(p, e, "<!--")) {
            const char *q = p + 4;
            while (q + 2 < e && !(q[0] == '-' && q[1] == '-' && q[2] == '>')) ++q;
            p = q + 3;
            continue;
        }
        if (starts(p, e, "<![CDATA[")) {
            const char *q = p + 9;
            while (q + 2 < e && !(q[0] == ']' && q[1] == ']' && q[2] == '>')) ++q;
            p = q + 3;
            continue;
        }
        if (p + 1 < e && (p[1] == '?' || p[1] == '!')) {
            while (p < e && *p != '>') ++p;
            ++p;
            continue;
        }
        if (p + 1 < e && p[1] == '/') { /* end tag */
            while (p < e && *p != '>') ++p;
            ++p;
            if (cur >= 0) {
                xnode *x = &d->n[cur];
                if (x->text && x->first_child < 0) x->text_len = (u32)(p - x->text);
                cur = x->parent;
            }
            continue;
        }
        /* a start tag */
        const char *t = p + 1, *te = t;
        while (te < e && *te != ' ' && *te != '\t' && *te != '\r' && *te != '\n' && *te != '>' && *te != '/') ++te;
        const char *a = te, *ae = a;
        bool in_q = false;
        char q = 0;
        while (ae < e && (in_q || *ae != '>')) {
            if (in_q && *ae == q) in_q = false;
            else if (!in_q && (*ae == '"' || *ae == '\'')) {
                in_q = true;
                q = *ae;
            }
            ++ae;
        }
        if (ae >= e) break;
        bool self = ae > a && ae[-1] == '/';
        i32 id = xnew(d);
        if (id < 0) return false;
        xnode *x = &d->n[id];
        x->tag = t;
        x->tag_len = (u32)(te - t);
        x->attrs = a;
        x->attrs_len = (u32)((self ? ae - 1 : ae) - a);
        x->parent = cur;
        if (cur >= 0) {
            xnode *pa = &d->n[cur];
            if (pa->last_child >= 0) d->n[pa->last_child].next = id;
            else pa->first_child = id;
            pa->last_child = id;
        }
        p = ae + 1;
        if (!self) {
            x->text = p;
            cur = id;
        }
    }
    return !d->bad;
}

static bool tag_is(const xnode *x, const char *lit)
{
    u32 n = (u32)ub_strlen(lit);
    return x->tag_len == n && memcmp(x->tag, lit, n) == 0;
}

/* an attribute's raw value (no entities: Roblox's names and referents have none) */
static bool attr(const xnode *x, const char *name, const char **v, u32 *vl)
{
    const char *p = x->attrs, *e = x->attrs + x->attrs_len;
    u32 nl = (u32)ub_strlen(name);
    while (p < e) {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
        const char *k = p;
        while (p < e && *p != '=' && *p != ' ') ++p;
        u32 kl = (u32)(p - k);
        while (p < e && *p != '"' && *p != '\'') ++p;
        if (p >= e) return false;
        char q = *p++;
        const char *vs = p;
        while (p < e && *p != q) ++p;
        if (kl == nl && memcmp(k, name, nl) == 0) {
            *v = vs;
            *vl = (u32)(p - vs);
            return true;
        }
        ++p;
    }
    return false;
}

static i32 child(const xdoc *d, i32 i, const char *tag)
{
    for (i32 c = d->n[i].first_child; c >= 0; c = d->n[c].next)
        if (tag_is(&d->n[c], tag)) return c;
    return -1;
}

/* ------------------------------------------------------------ buffers */

typedef struct {
    u8 *p;
    u32 len, cap;
    bool bad;
} buf;

static void put(buf *b, const void *data, u32 n)
{
    if (b->bad) return;
    if (b->len + n > b->cap) {
        u32 nc = b->cap ? b->cap * 2 : 65536;
        while (nc < b->len + n) nc *= 2;
        u8 *np = (u8 *)plat_alloc(nc);
        if (!np) {
            b->bad = true;
            return;
        }
        if (b->p) {
            memcpy(np, b->p, b->len);
            plat_free(b->p);
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

static void put8(buf *b, u32 v) { u8 x = (u8)v; put(b, &x, 1); }
static void put32(buf *b, u32 v)
{
    u8 x[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
    put(b, x, 4);
}
static void putf(buf *b, f32 f)
{
    u32 u;
    memcpy(&u, &f, 4);
    put32(b, u);
}
static void putstr(buf *b, const void *s, u32 n)
{
    put32(b, n);
    put(b, s, n);
}

/* Roblox's interleaved big-endian arrays */
static void put_interleaved(buf *b, const u32 *v, u32 n)
{
    for (int byte = 0; byte < 4; ++byte)
        for (u32 i = 0; i < n; ++i) put8(b, v[i] >> (24 - 8 * byte));
}

static u32 transform(i32 v) { return ((u32)v << 1) ^ (u32)(v >> 31); }
static u32 rfloat(f32 f)
{
    u32 u;
    memcpy(&u, &f, 4);
    return (u << 1) | (u >> 31); /* the sign bit goes last */
}

/* ------------------------------------------------------------ text */

/* the node's text: entities decoded, CDATA unwrapped, into out */
static u32 text_of(const xnode *x, char *out, u32 cap)
{
    const char *p = x->text, *e = x->text + x->text_len;
    u32 n = 0;
    /* (the raw text ends at "</": keep only before it) */
    const char *end = p;
    while (end < e && !(end[0] == '<' && end + 1 < e && end[1] == '/')) {
        if (starts(end, e, "<![CDATA[")) {
            const char *q = end + 9;
            while (q + 2 < e && !(q[0] == ']' && q[1] == ']' && q[2] == '>')) ++q;
            end = q + 3;
            continue;
        }
        if (*end == '<') break; /* a child element */
        ++end;
    }
    while (p < end && n + 4 < cap) {
        if (starts(p, end, "<![CDATA[")) {
            p += 9;
            while (p + 2 < end && !(p[0] == ']' && p[1] == ']' && p[2] == '>')) out[n++] = *p++;
            p += 3;
            continue;
        }
        if (*p == '&') {
            const char *q = p + 1;
            while (q < end && *q != ';' && q - p < 10) ++q;
            u32 cp = 0;
            bool ok = true;
            if (starts(p, end, "&amp;")) cp = '&';
            else if (starts(p, end, "&lt;")) cp = '<';
            else if (starts(p, end, "&gt;")) cp = '>';
            else if (starts(p, end, "&quot;")) cp = '"';
            else if (starts(p, end, "&apos;")) cp = '\'';
            else if (p[1] == '#') {
                const char *r = p + 2;
                int base = 10;
                if (r < q && (*r == 'x' || *r == 'X')) { base = 16; ++r; }
                for (; r < q; ++r) {
                    int dgt = *r >= '0' && *r <= '9' ? *r - '0' : (*r | 32) >= 'a' && (*r | 32) <= 'f' ? (*r | 32) - 'a' + 10 : 99;
                    if (dgt >= base) { ok = false; break; }
                    cp = cp * (u32)base + (u32)dgt;
                }
            } else ok = false;
            if (ok && q < end) {
                if (cp < 0x80) out[n++] = (char)cp;
                else if (cp < 0x800) { out[n++] = (char)(0xC0 | (cp >> 6)); out[n++] = (char)(0x80 | (cp & 63)); }
                else if (cp < 0x10000) {
                    out[n++] = (char)(0xE0 | (cp >> 12)); out[n++] = (char)(0x80 | ((cp >> 6) & 63)); out[n++] = (char)(0x80 | (cp & 63));
                } else {
                    out[n++] = (char)(0xF0 | (cp >> 18)); out[n++] = (char)(0x80 | ((cp >> 12) & 63));
                    out[n++] = (char)(0x80 | ((cp >> 6) & 63)); out[n++] = (char)(0x80 | (cp & 63));
                }
                p = q + 1;
                continue;
            }
        }
        out[n++] = *p++;
    }
    out[n] = 0;
    return n;
}

/* the text of a child element as a number */
static f64 num(const char *s)
{
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') ++s;
    if ((s[0] | 32) == 'i' || ((s[0] == '-' || s[0] == '+') && (s[1] | 32) == 'i')) {
        volatile f64 big = 1e300;
        f64 inf = big * big;
        return s[0] == '-' ? -inf : inf;
    }
    if ((s[0] | 32) == 'n') { /* NaN, as the binary files keep it */
        volatile f64 zero = 0;
        return zero / zero;
    }
    f64 sign = 1, v = 0, frac = 0, scale = 1;
    if (*s == '-') { sign = -1; ++s; } else if (*s == '+') ++s;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (*s == '.') {
        ++s;
        while (*s >= '0' && *s <= '9') { frac = frac * 10 + (*s++ - '0'); scale *= 10; }
    }
    v += frac / scale;
    if ((*s | 32) == 'e') {
        ++s;
        int es = 1, ex = 0;
        if (*s == '-') { es = -1; ++s; } else if (*s == '+') ++s;
        while (*s >= '0' && *s <= '9') ex = ex * 10 + (*s++ - '0');
        if (ex > 300) ex = 300;
        while (ex-- > 0) v = es > 0 ? v * 10 : v / 10;
    }
    return sign * v;
}

static f64 child_num(const xdoc *d, i32 i, const char *tag)
{
    i32 c = child(d, i, tag);
    if (c < 0) return 0;
    char t[64];
    text_of(&d->n[c], t, sizeof t);
    return num(t);
}

static u32 base64(const char *s, u32 n, u8 *out)
{
    u32 o = 0, acc = 0;
    int bits = 0;
    for (u32 i = 0; i < n; ++i) {
        char c = s[i];
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
              : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) continue;
        acc = (acc << 6) | (u32)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (u8)(acc >> bits);
        }
    }
    return o;
}

/* ------------------------------------------------------------ convert */

typedef struct {
    i32 node;             /* its <Item> */
    i32 parent;           /* item index, -1 root */
    i32 cls;
} xitem;

typedef struct {
    const char *name;
    u32 name_len;
    i32 first, count;     /* items of the class, by index list */
} xclass;

typedef struct {
    const char *name;
    u32 name_len;
    u8 type;              /* binary type id */
    const char *xtype;    /* the XML element name */
    u32 xtype_len;
} xprop;

/* the binary type for an XML property element; 0 = not kept */
static u8 type_of(const xnode *x)
{
    static const struct { const char *t; u8 id; } k[] = {
        { "string", 0x01 }, { "ProtectedString", 0x01 }, { "BinaryString", 0x01 }, { "Content", 0x01 },
        { "ContentId", 0x01 }, { "bool", 0x02 }, { "int", 0x03 }, { "float", 0x04 }, { "double", 0x05 },
        { "UDim", 0x06 }, { "UDim2", 0x07 }, { "BrickColor", 0x0B }, { "Color3", 0x0C }, { "Vector2", 0x0D },
        { "Vector3", 0x0E }, { "CoordinateFrame", 0x10 }, { "token", 0x12 }, { "Ref", 0x13 },
        { "NumberSequence", 0x15 }, { "ColorSequence", 0x16 }, { "NumberRange", 0x17 }, { "Rect2D", 0x18 },
        { "Color3uint8", 0x1A }, { "int64", 0x1B }, { "SharedString", 0x1C }, { "Font", 0x20 },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; ++i)
        if (tag_is(x, k[i].t)) return k[i].id;
    return 0;
}

typedef struct {
    xdoc d;
    xitem *items;
    i32 nitems;
    xclass *classes;
    i32 nclasses;
    i32 *order;           /* item indices grouped by class */
    /* referent string -> item */
    const char **ref_s;
    u32 *ref_len;
    i32 *ref_item;
    u32 ref_cap;
    /* shared strings: md5 key -> index */
    i32 sstr_node[4096];
    const char *sstr_key[4096];
    u32 sstr_key_len[4096];
    i32 nsstr;
    char *scratch;        /* text_of() target */
    u32 scratch_cap;
} conv;

static u32 hash(const char *s, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; ++i) h = (h ^ (u8)s[i]) * 16777619u;
    return h;
}

static i32 ref_find(const conv *c, const char *s, u32 n)
{
    if (!c->ref_cap) return -1;
    u32 h = hash(s, n) & (c->ref_cap - 1);
    for (u32 probes = 0; probes < c->ref_cap; ++probes) {
        if (!c->ref_s[h]) return -1;
        if (c->ref_len[h] == n && !memcmp(c->ref_s[h], s, n)) return c->ref_item[h];
        h = (h + 1) & (c->ref_cap - 1);
    }
    return -1;
}

static void ref_put(conv *c, const char *s, u32 n, i32 item)
{
    u32 h = hash(s, n) & (c->ref_cap - 1);
    while (c->ref_s[h]) h = (h + 1) & (c->ref_cap - 1);
    c->ref_s[h] = s;
    c->ref_len[h] = n;
    c->ref_item[h] = item;
}

static i32 sstr_find(const conv *c, const char *s, u32 n)
{
    for (i32 i = 0; i < c->nsstr; ++i)
        if (c->sstr_key_len[i] == n && !memcmp(c->sstr_key[i], s, n)) return i;
    return -1;
}

/* the item's property node of this name, or -1 */
static i32 prop_node(const conv *c, i32 item, const char *name, u32 nl, u8 type)
{
    i32 props = child(&c->d, c->items[item].node, "Properties");
    if (props < 0) return -1;
    for (i32 p = c->d.n[props].first_child; p >= 0; p = c->d.n[p].next) {
        const char *v;
        u32 vl;
        if (attr(&c->d.n[p], "name", &v, &vl) && vl == nl && !memcmp(v, name, nl)) {
            u8 t = type_of(&c->d.n[p]);
            if (t == 0x01 && type == 0x22 && tag_is(&c->d.n[p], "Content")) t = 0x22;
            return t == type ? p : -1;
        }
    }
    return -1;
}

static const char *text(conv *c, i32 node, u32 *len)
{
    u32 need = c->d.n[node].text_len + 16;
    if (need > c->scratch_cap) {
        plat_free(c->scratch);
        c->scratch = (char *)plat_alloc(need);
        c->scratch_cap = c->scratch ? need : 0;
    }
    if (!c->scratch) {
        *len = 0;
        return "";
    }
    *len = text_of(&c->d.n[node], c->scratch, c->scratch_cap);
    return c->scratch;
}

/* writes one PROP chunk: values for every item of the class */
static void write_prop(conv *c, buf *out, i32 ci, const char *name, u32 nl, u8 type)
{
    const xclass *k = &c->classes[ci];
    u32 n = (u32)k->count;
    const i32 *items = c->order + k->first;
    buf b = { 0 };
    put32(&b, (u32)ci);
    putstr(&b, name, nl);
    put8(&b, type);
    u32 *tmp = (u32 *)plat_alloc(sizeof(u32) * n * 4 + 4);
    if (!tmp) {
        out->bad = true;
        return;
    }
    const xdoc *d = &c->d;
    switch (type) {
    case 0x01: case 0x1C: {
        for (u32 i = 0; i < n; ++i) {
            i32 p = prop_node(c, items[i], name, nl, type);
            u32 len = 0;
            const char *s = "";
            if (p >= 0) {
                const xnode *x = &d->n[p];
                if (type == 0x1C) { /* the key of a shared string */
                    s = text(c, p, &len);
                    while (len && (s[len - 1] == ' ' || s[len - 1] == '\n' || s[len - 1] == '\r')) --len;
                    i32 si = sstr_find(c, s, len);
                    tmp[i] = si >= 0 ? (u32)si : 0xFFFFFFFFu;
                    continue;
                }
                if (tag_is(x, "Content") || tag_is(x, "ContentId")) {
                    i32 u = child(d, p, "url");
                    if (u < 0) u = child(d, p, "uri");
                    if (u >= 0) s = text(c, u, &len);
                    else if (tag_is(x, "ContentId")) s = text(c, p, &len);
                } else {
                    s = text(c, p, &len);
                    if (tag_is(x, "BinaryString")) len = base64(s, len, (u8 *)c->scratch);
                }
            } else if (type == 0x1C) {
                tmp[i] = 0xFFFFFFFFu;
                continue;
            }
            putstr(&b, s, len);
        }
        if (type == 0x1C) put_interleaved(&b, tmp, n);
        break;
    }
    case 0x22: { /* Content: source types (zig-zag: 0 none, 1 uri), the uris, then no objects */
        u32 uris = 0;
        for (u32 i = 0; i < n; ++i) {
            i32 p = prop_node(c, items[i], name, nl, type);
            i32 u = p >= 0 ? child(d, p, "uri") : -1;
            u32 len = 0;
            if (u >= 0) text(c, u, &len);
            tmp[i] = transform(u >= 0 && len ? 1 : 0);
            uris += u >= 0 && len;
        }
        put_interleaved(&b, tmp, n);
        put32(&b, uris);
        for (u32 i = 0; i < n; ++i) {
            i32 p = prop_node(c, items[i], name, nl, type);
            i32 u = p >= 0 ? child(d, p, "uri") : -1;
            u32 len = 0;
            const char *s = u >= 0 ? text(c, u, &len) : "";
            if (len) putstr(&b, s, len);
        }
        put32(&b, 0);
        break;
    }
    case 0x02:
        for (u32 i = 0; i < n; ++i) {
            i32 p = prop_node(c, items[i], name, nl, type);
            u32 len;
            const char *s = p >= 0 ? text(c, p, &len) : "false";
            put8(&b, s[0] == 't' || s[0] == 'T' || s[0] == '1');
        }
        break;
    case 0x03: case 0x04: case 0x0B: case 0x12: {
        for (u32 i = 0; i < n; ++i) {
            i32 p = prop_node(c, items[i], name, nl, type);
            u32 len;
            f64 v = p >= 0 ? num(text(c, p, &len)) : 0;
            tmp[i] = type == 0x03 ? transform((i32)v) : type == 0x04 ? rfloat((f32)v) : (u32)(i64)v;
        }
        put_interleaved(&b, tmp, n);
        break;
    }
    case 0x05:
        for (u32 i = 0; i < n; ++i) {
            i32 p = prop_node(c, items[i], name, nl, type);
            u32 len;
            f64 v = p >= 0 ? num(text(c, p, &len)) : 0;
            put(&b, &v, 8);
        }
        break;
    case 0x1B: { /* Int64: 8 interleaved big-endian bytes, zig-zag */
        u8 *q = (u8 *)plat_alloc(n * 8 + 1);
        if (!q) break;
        for (u32 i = 0; i < n; ++i) {
            i32 p = prop_node(c, items[i], name, nl, type);
            u32 len;
            i64 v = p >= 0 ? (i64)num(text(c, p, &len)) : 0;
            u64 z = ((u64)v << 1) ^ (u64)(v >> 63);
            for (int k2 = 0; k2 < 8; ++k2) q[k2 * n + i] = (u8)(z >> (56 - 8 * k2));
        }
        put(&b, q, n * 8);
        plat_free(q);
        break;
    }
    case 0x06: case 0x07: case 0x0C: case 0x0D: case 0x0E: case 0x18: {
        static const char *const udim[] = { "S", "O" }, *const udim2[] = { "XS", "YS", "XO", "YO" };
        static const char *const rgb[] = { "R", "G", "B" }, *const xyz[] = { "X", "Y", "Z" };
        int comps = type == 0x06 ? 2 : type == 0x07 ? 4 : type == 0x0D ? 2 : type == 0x18 ? 4 : 3;
        for (int k2 = 0; k2 < comps; ++k2) {
            for (u32 i = 0; i < n; ++i) {
                i32 p = prop_node(c, items[i], name, nl, type);
                f64 v = 0;
                if (p >= 0) {
                    if (type == 0x06) v = child_num(d, p, udim[k2]);
                    else if (type == 0x07) v = child_num(d, p, udim2[k2]);
                    else if (type == 0x0C) v = child_num(d, p, rgb[k2]);
                    else if (type == 0x18) {
                        i32 mm = child(d, p, k2 < 2 ? "min" : "max");
                        v = mm >= 0 ? child_num(d, mm, xyz[k2 & 1]) : 0;
                    } else v = child_num(d, p, xyz[k2]);
                }
                bool is_int = (type == 0x06 && k2 == 1) || (type == 0x07 && k2 >= 2);
                tmp[i] = is_int ? transform((i32)v) : rfloat((f32)v);
            }
            put_interleaved(&b, tmp, n);
        }
        break;
    }
    case 0x10: { /* CFrame: each rotation (id 0 + 9 floats), then x, y, z arrays */
        static const char *const r9[] = { "R00", "R01", "R02", "R10", "R11", "R12", "R20", "R21", "R22" };
        for (u32 i = 0; i < n; ++i) {
            i32 p = prop_node(c, items[i], name, nl, type);
            put8(&b, 0);
            for (int k2 = 0; k2 < 9; ++k2) putf(&b, p >= 0 ? (f32)child_num(d, p, r9[k2]) : (k2 % 4 == 0 ? 1.0f : 0.0f));
        }
        static const char *const xyz[] = { "X", "Y", "Z" };
        for (int k2 = 0; k2 < 3; ++k2) {
            for (u32 i = 0; i < n; ++i) {
                i32 p = prop_node(c, items[i], name, nl, type);
                tmp[i] = rfloat(p >= 0 ? (f32)child_num(d, p, xyz[k2]) : 0);
            }
            put_interleaved(&b, tmp, n);
        }
        break;
    }
    case 0x13: { /* Ref: referents (item indices), as deltas */
        i32 prev = 0;
        for (u32 i = 0; i < n; ++i) {
            i32 p = prop_node(c, items[i], name, nl, type);
            i32 r = -1;
            if (p >= 0) {
                u32 len;
                const char *s = text(c, p, &len);
                while (len && (s[len - 1] == ' ' || s[len - 1] == '\n')) --len;
                r = ref_find(c, s, len);
            }
            tmp[i] = transform(r - prev);
            prev = r;
        }
        put_interleaved(&b, tmp, n);
        break;
    }
    case 0x15: case 0x16: { /* sequences as text: "t v e ..." / "t r g b e ..." */
        int per = type == 0x15 ? 3 : 5;
        for (u32 i = 0; i < n; ++i) {
            i32 p = prop_node(c, items[i], name, nl, type);
            f32 vals[20 * 5];
            int nv = 0;
            if (p >= 0) {
                u32 len;
                const char *s = text(c, p, &len), *e = s + len;
                while (s < e && nv < 20 * per) {
                    while (s < e && (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')) ++s;
                    if (s >= e) break;
                    char t[48];
                    int tl = 0;
                    while (s < e && *s != ' ' && *s != '\n' && *s != '\r' && *s != '\t' && tl < 47) t[tl++] = *s++;
                    t[tl] = 0;
                    vals[nv++] = (f32)num(t);
                }
            }
            u32 kp = (u32)(nv / per);
            put32(&b, kp);
            for (u32 j = 0; j < kp * (u32)per; ++j) putf(&b, vals[j]);
        }
        break;
    }
    case 0x17:
        for (u32 i = 0; i < n; ++i) {
            i32 p = prop_node(c, items[i], name, nl, type);
            f32 lo = 0, hi = 0;
            if (p >= 0) {
                u32 len;
                const char *s = text(c, p, &len);
                char t[48];
                int tl = 0;
                while (*s == ' ') ++s;
                while (*s && *s != ' ' && tl < 47) t[tl++] = *s++;
                t[tl] = 0;
                lo = (f32)num(t);
                hi = (f32)num(s);
            }
            putf(&b, lo);
            putf(&b, hi);
        }
        break;
    case 0x1A: { /* Color3uint8 text is 0xAARRGGBB as a number */
        u8 *q = (u8 *)plat_alloc(n * 3 + 1);
        if (!q) break;
        for (u32 i = 0; i < n; ++i) {
            i32 p = prop_node(c, items[i], name, nl, type);
            u32 len;
            u32 v = p >= 0 ? (u32)(i64)num(text(c, p, &len)) : 0xA3A2A5;
            q[i] = (u8)(v >> 16);
            q[n + i] = (u8)(v >> 8);
            q[2 * n + i] = (u8)v;
        }
        put(&b, q, n * 3);
        plat_free(q);
        break;
    }
    case 0x20: /* Font: family, weight, style, cached face */
        for (u32 i = 0; i < n; ++i) {
            i32 p = prop_node(c, items[i], name, nl, type);
            u32 fl = 0, wv = 400;
            const char *fam = "";
            bool italic = false;
            if (p >= 0) {
                i32 f = child(d, p, "Family");
                i32 u = f >= 0 ? child(d, f, "url") : -1;
                char famb[256] = "";
                if (u >= 0) {
                    u32 len;
                    const char *s = text(c, u, &len);
                    fl = len < 255 ? len : 255;
                    memcpy(famb, s, fl);
                }
                wv = (u32)child_num(d, p, "Weight");
                i32 st = child(d, p, "Style");
                if (st >= 0) {
                    u32 len;
                    italic = text(c, st, &len)[0] == 'I';
                }
                putstr(&b, famb, fl);
                put8(&b, wv & 255);
                put8(&b, wv >> 8);
                put8(&b, italic ? 1 : 0);
                putstr(&b, "", 0);
                continue;
            }
            putstr(&b, fam, 0);
            put8(&b, 400 & 255);
            put8(&b, 400 >> 8);
            put8(&b, 0);
            putstr(&b, "", 0);
        }
        break;
    default: break;
    }
    plat_free(tmp);
    /* the chunk, uncompressed */
    put(out, "PROP", 4);
    put32(out, 0);
    put32(out, b.len);
    put32(out, 0);
    put(out, b.p, b.len);
    if (b.bad) out->bad = true;
    plat_free(b.p);
}

bool ub_rbxlx_detect(const u8 *data, u32 size)
{
    u32 i = 0;
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) i = 3; /* UTF-8 mark */
    while (i < size && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n')) ++i;
    if (size - i >= 5 && !memcmp(data + i, "<?xml", 5)) {
        while (i < size && data[i] != '>') ++i;
        ++i;
        while (i < size && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n')) ++i;
    }
    return size - i >= 8 && !memcmp(data + i, "<roblox ", 8);
}

bool ub_rbxlx_to_binary(const u8 *data, u32 size, u8 **out_data, u32 *out_size)
{
    *out_data = NULL;
    *out_size = 0;
    conv c;
    memset(&c, 0, sizeof c);
    bool ok = xparse(&c.d, (const char *)data, size);
    i32 root = -1;
    for (i32 i = 0; ok && i < c.d.count && root < 0; ++i)
        if (tag_is(&c.d.n[i], "roblox") && c.d.n[i].parent < 0) root = i;
    if (root < 0) ok = false;
    /* items, depth first */
    if (ok) {
        i32 total = 0;
        for (i32 i = 0; i < c.d.count; ++i) total += tag_is(&c.d.n[i], "Item");
        c.items = (xitem *)plat_alloc(sizeof(xitem) * (size_t)(total + 1));
        c.order = (i32 *)plat_alloc(sizeof(i32) * (size_t)(total + 1));
        c.classes = (xclass *)plat_alloc(sizeof(xclass) * (size_t)(total + 1));
        c.ref_cap = 16;
        while (c.ref_cap < (u32)total * 2 + 2) c.ref_cap *= 2;
        c.ref_s = (const char **)plat_alloc(sizeof(char *) * c.ref_cap);
        c.ref_len = (u32 *)plat_alloc(sizeof(u32) * c.ref_cap);
        c.ref_item = (i32 *)plat_alloc(sizeof(i32) * c.ref_cap);
        ok = c.items && c.order && c.classes && c.ref_s && c.ref_len && c.ref_item;
        i32 stack[512], parents[512];
        int sp = 0;
        for (i32 ch = ok ? c.d.n[root].first_child : -1; ch >= 0; ch = c.d.n[ch].next)
            if (sp < 512) { /* (pushed in reverse below) */ }
        /* walk: (node, parent item) */
        i32 tmpn[512];
        int nt = 0;
        for (i32 ch = ok ? c.d.n[root].first_child : -1; ch >= 0 && nt < 512; ch = c.d.n[ch].next)
            if (tag_is(&c.d.n[ch], "Item")) tmpn[nt++] = ch;
        for (int k = nt - 1; k >= 0; --k) {
            stack[sp] = tmpn[k];
            parents[sp++] = -1;
        }
        while (ok && sp > 0) {
            i32 node = stack[--sp], parent = parents[sp];
            i32 it = c.nitems++;
            c.items[it].node = node;
            c.items[it].parent = parent;
            const char *v;
            u32 vl;
            if (attr(&c.d.n[node], "referent", &v, &vl) && vl) ref_put(&c, v, vl, it);
            const char *cn = "Folder";
            u32 cl = 6;
            if (attr(&c.d.n[node], "class", &v, &vl) && vl && vl < 47) { cn = v; cl = vl; }
            i32 ci = -1;
            for (i32 k = 0; k < c.nclasses && ci < 0; ++k)
                if (c.classes[k].name_len == cl && !memcmp(c.classes[k].name, cn, cl)) ci = k;
            if (ci < 0) {
                ci = c.nclasses++;
                c.classes[ci].name = cn;
                c.classes[ci].name_len = cl;
                c.classes[ci].count = 0;
            }
            c.items[it].cls = ci;
            c.classes[ci].count++;
            nt = 0;
            for (i32 ch = c.d.n[node].first_child; ch >= 0 && nt < 512; ch = c.d.n[ch].next)
                if (tag_is(&c.d.n[ch], "Item")) tmpn[nt++] = ch;
            for (int k = nt - 1; k >= 0 && sp < 512; --k) {
                stack[sp] = tmpn[k];
                parents[sp++] = it;
            }
        }
        /* group items by class */
        i32 at = 0;
        for (i32 k = 0; k < c.nclasses; ++k) {
            c.classes[k].first = at;
            at += c.classes[k].count;
            c.classes[k].count = 0;
        }
        for (i32 i = 0; i < c.nitems; ++i) {
            xclass *k = &c.classes[c.items[i].cls];
            c.order[k->first + k->count++] = i;
        }
    }
    /* shared strings */
    i32 ss = ok ? child(&c.d, root, "SharedStrings") : -1;
    for (i32 s = ss >= 0 ? c.d.n[ss].first_child : -1; s >= 0 && c.nsstr < 4096; s = c.d.n[s].next) {
        const char *v;
        u32 vl;
        if (!attr(&c.d.n[s], "md5", &v, &vl)) continue;
        c.sstr_node[c.nsstr] = s;
        c.sstr_key[c.nsstr] = v;
        c.sstr_key_len[c.nsstr] = vl;
        c.nsstr++;
    }
    buf out = { 0 };
    if (ok) {
        static const u8 magic[16] = { '<', 'r', 'o', 'b', 'l', 'o', 'x', '!', 0x89, 0xFF, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0 };
        put(&out, magic, 16);
        put32(&out, (u32)c.nclasses);
        put32(&out, (u32)c.nitems);
        put32(&out, 0);
        put32(&out, 0);
        /* SSTR */
        if (c.nsstr) {
            buf b = { 0 };
            put32(&b, 0);
            put32(&b, (u32)c.nsstr);
            for (i32 i = 0; i < c.nsstr; ++i) {
                u8 hashz[16] = { 0 };
                put(&b, hashz, 16);
                u32 len;
                const char *s = text(&c, c.sstr_node[i], &len);
                len = base64(s, len, (u8 *)c.scratch);
                putstr(&b, c.scratch, len);
            }
            put(&out, "SSTR", 4);
            put32(&out, 0);
            put32(&out, b.len);
            put32(&out, 0);
            put(&out, b.p, b.len);
            if (b.bad) out.bad = true;
            plat_free(b.p);
        }
        /* INST per class */
        for (i32 k = 0; k < c.nclasses && !out.bad; ++k) {
            const xclass *x = &c.classes[k];
            buf b = { 0 };
            put32(&b, (u32)k);
            putstr(&b, x->name, x->name_len);
            put8(&b, 0);
            put32(&b, (u32)x->count);
            u32 *refs = (u32 *)plat_alloc(sizeof(u32) * (size_t)x->count + 4);
            if (!refs) { out.bad = true; plat_free(b.p); break; }
            i32 prev = 0;
            for (i32 i = 0; i < x->count; ++i) {
                i32 r = c.order[x->first + i];
                refs[i] = transform(r - prev);
                prev = r;
            }
            put_interleaved(&b, refs, (u32)x->count);
            plat_free(refs);
            put(&out, "INST", 4);
            put32(&out, 0);
            put32(&out, b.len);
            put32(&out, 0);
            put(&out, b.p, b.len);
            if (b.bad) out.bad = true;
            plat_free(b.p);
        }
        /* PROP: per class, each property name its items have (the first type seen) */
        for (i32 k = 0; k < c.nclasses && !out.bad; ++k) {
            const xclass *x = &c.classes[k];
            xprop seen[256];
            int ns = 0;
            for (i32 i = 0; i < x->count && ns < 256; ++i) {
                i32 props = child(&c.d, c.items[c.order[x->first + i]].node, "Properties");
                for (i32 p = props >= 0 ? c.d.n[props].first_child : -1; p >= 0 && ns < 256; p = c.d.n[p].next) {
                    u8 t = type_of(&c.d.n[p]);
                    const char *v;
                    u32 vl;
                    if (!t || !attr(&c.d.n[p], "name", &v, &vl) || !vl) continue;
                    if (tag_is(&c.d.n[p], "Content") && child(&c.d, p, "uri") >= 0 && vl > 7 &&
                        !memcmp(v + vl - 7, "Content", 7)) {
                        /* (the newer Content type: as the binary importer, under the old name) */
                        t = 0x22;
                    }
                    bool dup = false;
                    for (int s2 = 0; s2 < ns && !dup; ++s2) dup = seen[s2].name_len == vl && !memcmp(seen[s2].name, v, vl);
                    if (dup) continue;
                    seen[ns].name = v;
                    seen[ns].name_len = vl;
                    seen[ns].type = t;
                    ns++;
                }
            }
            for (int s2 = 0; s2 < ns && !out.bad; ++s2) write_prop(&c, &out, k, seen[s2].name, seen[s2].name_len, seen[s2].type);
        }
        /* PRNT */
        if (!out.bad) {
            buf b = { 0 };
            put8(&b, 0);
            put32(&b, (u32)c.nitems);
            u32 *a = (u32 *)plat_alloc(sizeof(u32) * (size_t)c.nitems + 4);
            u32 *pa = (u32 *)plat_alloc(sizeof(u32) * (size_t)c.nitems + 4);
            if (a && pa) {
                i32 prev = 0, prevp = 0;
                for (i32 i = 0; i < c.nitems; ++i) {
                    a[i] = transform(i - prev);
                    prev = i;
                    pa[i] = transform(c.items[i].parent - prevp);
                    prevp = c.items[i].parent;
                }
                put_interleaved(&b, a, (u32)c.nitems);
                put_interleaved(&b, pa, (u32)c.nitems);
            } else b.bad = true;
            plat_free(a);
            plat_free(pa);
            put(&out, "PRNT", 4);
            put32(&out, 0);
            put32(&out, b.len);
            put32(&out, 0);
            put(&out, b.p, b.len);
            if (b.bad) out.bad = true;
            plat_free(b.p);
        }
        put(&out, "END\0", 4);
        put32(&out, 0);
        put32(&out, 9);
        put32(&out, 0);
        put(&out, "</roblox>", 9);
    }
    plat_free(c.d.n);
    plat_free(c.items);
    plat_free(c.order);
    plat_free(c.classes);
    plat_free((void *)c.ref_s);
    plat_free(c.ref_len);
    plat_free(c.ref_item);
    plat_free(c.scratch);
    if (!ok || out.bad) {
        plat_free(out.p);
        return false;
    }
    *out_data = out.p;
    *out_size = out.len;
    return true;
}
