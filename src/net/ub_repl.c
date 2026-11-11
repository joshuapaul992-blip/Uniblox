/* Uniblox - replication: the server's world, as a client may see it (see
 * ub_repl.h). */
#include "net/ub_repl.h"
#include "net/ub_netbuf.h"
#include "core/ub_string.h"
#include "platform/platform.h"
#include "crt/ub_crt.h"

enum {
    R_KEY = 1,      /* key, name: a property name's number on the server */
    R_CREATE,       /* net, parent net, class, class name, name, flags, part?, props, source? */
    R_DESTROY,      /* net: it and all in it */
    R_PARENT,       /* net, parent net (-1: none) */
    R_NAME,         /* net, name */
    R_SOURCE,       /* net, code */
    R_PROP,         /* net, key, value */
    R_PROP_DEL,     /* net, key */
    R_PROPS_ALL,    /* net, count, (key, value)...: replaces them all */
    R_PART,         /* tick, net, the part's fields */
    R_XFORM,        /* tick, net, position, rotation (exact): where it came to rest */
    R_MOTION,       /* (unreliable) tick, count, (net, position, rotation in 6 bytes)... */
    R_LIGHTING,
    R_WORLD,        /* name, gravity, fallen-parts height */
    R_MESH,         /* server id, flags, counts, vertices, triangles, pieces */
    R_JOINED,       /* the first full copy is complete */
    R_ORDER,        /* parent net, count, nets...: these children go last, in this order */
};

#define MAX_CLIENTS 32
#define MOTION_BYTES 900   /* an unreliable motion message, at most */

/* ------------------------------------------------------------- values */

static void put_value(ub_wbuf *b, const ub_value *v)
{
    ub_w_u8(b, v->type);
    switch (v->type) {
    case UB_PV_BOOL: ub_w_u8(b, v->v.b ? 1 : 0); break;
    case UB_PV_INT: ub_w_vari(b, v->v.i); break;
    case UB_PV_REF: ub_w_vari(b, v->v.ref); break;
    case UB_PV_ENUM: case UB_PV_BRICKCOLOR: ub_w_varu(b, v->v.e); break;
    case UB_PV_FLOAT: ub_w_f32(b, v->v.f); break;
    case UB_PV_DOUBLE: ub_w_f64(b, v->v.d); break;
    case UB_PV_STRING: ub_w_str(b, v->v.str.s ? v->v.str.s : "", v->v.str.len); break;
    case UB_PV_COLOR3: case UB_PV_VECTOR3: ub_w_vec3(b, v3(v->v.v3.x, v->v.v3.y, v->v.v3.z)); break;
    case UB_PV_VECTOR2: case UB_PV_NUMRANGE: ub_w_f32(b, v->v.v2.x); ub_w_f32(b, v->v.v2.y); break;
    case UB_PV_UDIM: ub_w_f32(b, v->v.ud.s); ub_w_vari(b, v->v.ud.o); break;
    case UB_PV_UDIM2:
        ub_w_f32(b, v->v.ud2.sx);
        ub_w_vari(b, v->v.ud2.ox);
        ub_w_f32(b, v->v.ud2.sy);
        ub_w_vari(b, v->v.ud2.oy);
        break;
    case UB_PV_RECT:
        ub_w_f32(b, v->v.rect.x0);
        ub_w_f32(b, v->v.rect.y0);
        ub_w_f32(b, v->v.rect.x1);
        ub_w_f32(b, v->v.rect.y1);
        break;
    case UB_PV_NUMSEQ: case UB_PV_COLORSEQ: {
        u32 per = v->type == UB_PV_NUMSEQ ? 3 : 4, n = v->v.seq.k ? v->v.seq.n : 0;
        ub_w_varu(b, n);
        for (u32 i = 0; i < n * per; ++i) ub_w_f32(b, v->v.seq.k[i]);
        break;
    }
    case UB_PV_FONT: {
        const char *f = v->v.font.family ? v->v.font.family : "";
        ub_w_str(b, f, (u32)ub_strlen(f));
        ub_w_u16(b, v->v.font.weight);
        ub_w_u8(b, v->v.font.style);
        break;
    }
    case UB_PV_CFRAME:
        for (int i = 0; i < 12; ++i) ub_w_f32(b, v->v.cf.m ? v->v.cf.m[i] : 0);
        break;
    default: break;
    }
}

/* A value read into *v; its arrays and strings point into `scratch`
 * (valid until the next read). False if malformed. */
typedef struct {
    f32 floats[UB_SEQ_MAX_KEYS * 4 + 12];
    char text[512];
} scratch;

static bool get_value(ub_rbuf *r, ub_value *v, scratch *sc)
{
    memset(v, 0, sizeof *v);
    v->type = ub_r_u8(r);
    switch (v->type) {
    case UB_PV_BOOL: v->v.b = ub_r_u8(r) != 0; break;
    case UB_PV_INT: v->v.i = ub_r_vari(r); break;
    case UB_PV_REF: v->v.ref = ub_r_vari(r); break;
    case UB_PV_ENUM: case UB_PV_BRICKCOLOR: v->v.e = ub_r_varu(r); break;
    case UB_PV_FLOAT: v->v.f = ub_r_f32(r); break;
    case UB_PV_DOUBLE: v->v.d = ub_r_f64(r); break;
    case UB_PV_STRING: {
        u32 n;
        const char *s = ub_r_str(r, &n);
        v->v.str.s = (char *)(s ? s : "");
        v->v.str.len = n;
        break;
    }
    case UB_PV_COLOR3: case UB_PV_VECTOR3: {
        vec3 x = ub_r_vec3(r);
        v->v.v3.x = x.x;
        v->v.v3.y = x.y;
        v->v.v3.z = x.z;
        break;
    }
    case UB_PV_VECTOR2: case UB_PV_NUMRANGE: v->v.v2.x = ub_r_f32(r); v->v.v2.y = ub_r_f32(r); break;
    case UB_PV_UDIM: v->v.ud.s = ub_r_f32(r); v->v.ud.o = ub_r_vari(r); break;
    case UB_PV_UDIM2:
        v->v.ud2.sx = ub_r_f32(r);
        v->v.ud2.ox = ub_r_vari(r);
        v->v.ud2.sy = ub_r_f32(r);
        v->v.ud2.oy = ub_r_vari(r);
        break;
    case UB_PV_RECT:
        v->v.rect.x0 = ub_r_f32(r);
        v->v.rect.y0 = ub_r_f32(r);
        v->v.rect.x1 = ub_r_f32(r);
        v->v.rect.y1 = ub_r_f32(r);
        break;
    case UB_PV_NUMSEQ: case UB_PV_COLORSEQ: {
        u32 per = v->type == UB_PV_NUMSEQ ? 3 : 4, n = ub_r_varu(r);
        if (n > UB_SEQ_MAX_KEYS) return false;
        for (u32 i = 0; i < n * per; ++i) sc->floats[i] = ub_r_f32(r);
        v->v.seq.k = sc->floats;
        v->v.seq.n = n;
        break;
    }
    case UB_PV_FONT: {
        u32 n;
        const char *s = ub_r_str(r, &n);
        if (!s || n >= sizeof sc->text) return false;
        memcpy(sc->text, s, n);
        sc->text[n] = 0;
        v->v.font.family = sc->text;
        v->v.font.weight = ub_r_u16(r);
        v->v.font.style = ub_r_u8(r);
        break;
    }
    case UB_PV_CFRAME:
        for (int i = 0; i < 12; ++i) sc->floats[i] = ub_r_f32(r);
        v->v.cf.m = sc->floats;
        break;
    default: return false;
    }
    return !r->err;
}

static void put_part(ub_wbuf *b, const ub_part *p)
{
    ub_w_vec3(b, p->pos);
    ub_w_vec3(b, p->rot.x);
    ub_w_vec3(b, p->rot.y);
    ub_w_vec3(b, p->rot.z);
    ub_w_vec3(b, p->size);
    ub_w_u32(b, p->color);
    ub_w_u8(b, p->shape);
    ub_w_u8(b, p->material);
    ub_w_u8(b, p->transparency);
    ub_w_u8(b, p->flags);
    ub_w_u16(b, p->mesh);
    ub_w_u16(b, p->look);
}

static void get_part(ub_rbuf *r, ub_part *p)
{
    memset(p, 0, sizeof *p);
    p->pos = ub_r_vec3(r);
    p->rot.x = ub_r_vec3(r);
    p->rot.y = ub_r_vec3(r);
    p->rot.z = ub_r_vec3(r);
    p->size = ub_r_vec3(r);
    p->color = ub_r_u32(r);
    p->shape = ub_r_u8(r);
    p->material = ub_r_u8(r);
    p->transparency = ub_r_u8(r);
    p->flags = ub_r_u8(r);
    p->mesh = ub_r_u16(r);
    p->look = ub_r_u16(r);
}

/* a part's looks: everything but where it is */
static bool same_looks(const ub_part *a, const ub_part *b)
{
    return !memcmp(&a->size, &b->size, sizeof a->size) && a->color == b->color && a->shape == b->shape &&
           a->material == b->material && a->transparency == b->transparency && a->flags == b->flags &&
           a->mesh == b->mesh && a->look == b->look;
}

static bool same_place(const ub_part *a, vec3 pos, const mat3 *rot)
{
    return !memcmp(&a->pos, &pos, sizeof pos) && !memcmp(&a->rot, rot, sizeof *rot);
}

static bool grow(void **p, i32 *cap, i32 need, size_t elem)
{
    if (need <= *cap) return true;
    i32 nc = *cap ? *cap : 256;
    while (nc < need) nc *= 2;
    void *n = plat_alloc((size_t)nc * elem);
    if (!n) return false;
    memset(n, 0, (size_t)nc * elem);
    if (*p) memcpy(n, *p, (size_t)*cap * elem);
    plat_free(*p);
    *p = n;
    *cap = nc;
    return true;
}

/* ============================================================== server */

typedef struct {
    ub_part sent;          /* as last sent reliably */
    vec3 mpos;             /* where it was at the last tick (for "still moving?") */
    mat3 mrot;
    bool has, moving;
} rpart;

typedef struct {
    bool used, joined;
    ub_conn *conn;
    u8 *known;             /* per server instance: the client has it */
    i32 known_cap;
    u32 *stamp;            /* per instance: the tick its whole state was last sent */
    i32 stamp_cap;
    u8 *key_sent;          /* per key */
    i32 key_cap;
    i32 meshes_sent;
    ub_lighting light;
    bool light_ok;
    char wname[64];
    f32 gravity, fallen;
    bool world_ok;
    rpart *parts;          /* per server part */
    i32 parts_cap;
    /* the client's tree as it is (what it was sent, as it applied it):
     * parent, children, siblings, by server index */
    i32 *cpar, *cfirst, *clast, *cnext, *cprev;
    i32 tree_cap;
    u32 *touched;          /* per instance: the tick it got a child (for the order check) */
    i32 touched_cap;
    i32 *tlist;            /* those parents, this tick */
    i32 ntl, tl_cap;
    i32 roll;              /* the next part of the rolling check */
    ub_wbuf motion;        /* the unreliable message being filled */
    u32 motion_n;
    u32 motion_count_at;
    ub_repl_stats st;
} rclient;

struct ub_repl_server {
    ub_world *w;
    rclient c[MAX_CLIENTS];
    u32 tick;
    ub_wbuf msg;           /* the message being written */
};

static bool hidden_root(const ub_world *w, i32 i)
{
    const char *cn = ub_world_str(w, w->inst[i].class_name);
    return !ub_strcmp(cn, "ServerScriptService") || !ub_strcmp(cn, "ServerStorage") || !ub_strcmp(cn, "CoreGui");
}

bool ub_repl_visible(const ub_world *w, i32 inst)
{
    for (i32 a = inst, depth = 0; depth < 10000; ++depth) {
        if (!ub_world_alive(w, a) || hidden_root(w, a)) return false;
        i32 p = w->inst[a].parent;
        if (p < 0) return !(w->inst[a].flags & UB_INST_DETACHED);
        a = p;
    }
    return false;
}

/* may the client have a script's code? (LocalScripts and ModuleScripts: not server Scripts') */
static bool code_visible(const ub_world *w, i32 i)
{
    if (w->inst[i].source == UB_NO_STRING) return false;
    const char *cn = ub_world_str(w, w->inst[i].class_name);
    return (w->inst[i].flags & UB_INST_CLIENT) || !ub_strcmp(cn, "LocalScript") || !ub_strcmp(cn, "ModuleScript");
}

ub_repl_server *ub_repl_server_new(ub_world *w)
{
    ub_repl_server *s = (ub_repl_server *)plat_alloc(sizeof *s);
    if (!s) return NULL;
    memset(s, 0, sizeof *s);
    s->w = w;
    ub_wbuf_init(&s->msg, NULL, 0);
    if (!ub_world_journal_start(w)) {
        plat_free(s);
        return NULL;
    }
    return s;
}

static void free_client(rclient *c)
{
    plat_free(c->known);
    plat_free(c->stamp);
    plat_free(c->key_sent);
    plat_free(c->parts);
    plat_free(c->cpar);
    plat_free(c->cfirst);
    plat_free(c->clast);
    plat_free(c->cnext);
    plat_free(c->cprev);
    plat_free(c->touched);
    plat_free(c->tlist);
    ub_wbuf_free(&c->motion);
    memset(c, 0, sizeof *c);
}

void ub_repl_server_free(ub_repl_server *s)
{
    if (!s) return;
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (s->c[i].used) free_client(&s->c[i]);
    ub_wbuf_free(&s->msg);
    ub_world_journal_stop(s->w);
    plat_free(s);
}

int ub_repl_server_add(ub_repl_server *s, ub_conn *conn)
{
    for (int i = 0; i < MAX_CLIENTS; ++i)
        if (!s->c[i].used) {
            memset(&s->c[i], 0, sizeof s->c[i]);
            s->c[i].used = true;
            s->c[i].conn = conn;
            ub_wbuf_init(&s->c[i].motion, NULL, 0);
            return i;
        }
    return -1;
}

void ub_repl_server_remove(ub_repl_server *s, int client)
{
    if (client >= 0 && client < MAX_CLIENTS && s->c[client].used) free_client(&s->c[client]);
}

void ub_repl_server_stats(const ub_repl_server *s, int client, ub_repl_stats *out)
{
    memset(out, 0, sizeof *out);
    if (client >= 0 && client < MAX_CLIENTS && s->c[client].used) *out = s->c[client].st;
}

/* --- writing messages */

static ub_wbuf *begin(ub_repl_server *s, u8 type)
{
    ub_wbuf_clear(&s->msg);
    ub_w_u8(&s->msg, type);
    return &s->msg;
}

static void send_msg(ub_repl_server *s, rclient *c)
{
    if (s->msg.err || !s->msg.len) return;
    u8 type = s->msg.p[0];
    if (ub_conn_send(c->conn, true, s->msg.p, s->msg.len)) {
        c->st.bytes[type <= UB_REPL_LAST ? type : 0] += s->msg.len;
        c->st.messages[type <= UB_REPL_LAST ? type : 0]++;
    }
}

static bool grow_links(i32 **p, i32 *cap, i32 need)
{
    i32 old = *cap;
    if (!grow((void **)p, cap, need, sizeof(i32))) return false;
    for (i32 k = old; k < *cap; ++k) (*p)[k] = -1;
    return true;
}

static bool room_for(rclient *c, i32 inst, i32 parts)
{
    i32 tc = c->tree_cap;
    bool ok = grow((void **)&c->known, &c->known_cap, inst + 1, 1) && grow((void **)&c->stamp, &c->stamp_cap, inst + 1, sizeof(u32)) &&
              grow((void **)&c->parts, &c->parts_cap, parts + 1, sizeof(rpart)) &&
              grow((void **)&c->touched, &c->touched_cap, inst + 1, sizeof(u32));
    if (!ok) return false;
    if (inst + 1 > tc) {
        i32 caps[5] = { tc, tc, tc, tc, tc };
        if (!grow_links(&c->cpar, &caps[0], inst + 1) || !grow_links(&c->cfirst, &caps[1], inst + 1) ||
            !grow_links(&c->clast, &caps[2], inst + 1) || !grow_links(&c->cnext, &caps[3], inst + 1) ||
            !grow_links(&c->cprev, &caps[4], inst + 1))
            return false;
        c->tree_cap = caps[0];
    }
    return true;
}

/* --- the client's tree */

static void ct_unlink(rclient *c, i32 i)
{
    i32 p = c->cpar[i];
    if (p >= 0) {
        if (c->cprev[i] >= 0) c->cnext[c->cprev[i]] = c->cnext[i];
        else c->cfirst[p] = c->cnext[i];
        if (c->cnext[i] >= 0) c->cprev[c->cnext[i]] = c->cprev[i];
        else c->clast[p] = c->cprev[i];
    }
    c->cpar[i] = c->cnext[i] = c->cprev[i] = -1;
}

static void ct_append(ub_repl_server *s, rclient *c, i32 i, i32 p)
{
    c->cpar[i] = p;
    if (p < 0) return; /* (the top level has no order to keep) */
    c->cprev[i] = c->clast[p];
    c->cnext[i] = -1;
    if (c->clast[p] >= 0) c->cnext[c->clast[p]] = i;
    else c->cfirst[p] = i;
    c->clast[p] = i;
    if (c->touched[p] != s->tick + 1 && grow((void **)&c->tlist, &c->tl_cap, c->ntl + 1, sizeof(i32))) {
        c->touched[p] = s->tick + 1;
        c->tlist[c->ntl++] = p;
    }
}

static bool ct_inside(const rclient *c, i32 i, i32 anc)
{
    for (i32 a = i, d = 0; a >= 0 && d < 100000; a = c->cpar[a], ++d)
        if (a == anc) return true;
    return false;
}

/* as the client applies R_PARENT: a new parent still inside it is lifted out first */
static void ct_move(ub_repl_server *s, rclient *c, i32 i, i32 p)
{
    if (p >= 0 && ct_inside(c, p, i)) {
        ct_unlink(c, p);
        ct_append(s, c, p, -1);
    }
    ct_unlink(c, i);
    ct_append(s, c, i, p);
}

static void send_parent(ub_repl_server *s, rclient *c, i32 i, i32 par)
{
    ub_wbuf *b = begin(s, R_PARENT);
    ub_w_varu(b, (u32)i);
    ub_w_vari(b, par);
    send_msg(s, c);
    ct_move(s, c, i, par);
}

static void send_key(ub_repl_server *s, rclient *c, u32 key)
{
    if ((i32)key < c->key_cap && c->key_sent[key]) return;
    if (!grow((void **)&c->key_sent, &c->key_cap, (i32)key + 1, 1)) return;
    ub_wbuf *b = begin(s, R_KEY);
    ub_w_varu(b, key);
    const char *n = ub_world_key_name(s->w, key);
    ub_w_str(b, n, (u32)ub_strlen(n));
    send_msg(s, c);
    c->key_sent[key] = 1;
}

static void send_meshes(ub_repl_server *s, rclient *c)
{
    const ub_world *w = s->w;
    for (; c->meshes_sent < w->mesh_count; ++c->meshes_sent) {
        const ub_wmesh *m = &w->meshes[c->meshes_sent];
        ub_wbuf *b = begin(s, R_MESH);
        ub_w_varu(b, (u32)c->meshes_sent + 1);
        ub_w_varu(b, m->flags);
        ub_w_varu(b, m->nv);
        ub_w_varu(b, m->nt);
        ub_w_varu(b, m->npieces);
        for (u32 i = 0; i < m->nv * 3; ++i) ub_w_f32(b, m->v[i]);
        for (u32 i = 0; i < m->nt * 3; ++i) ub_w_varu(b, m->tri[i]);
        for (u32 i = 0; i < m->npieces; ++i) {
            ub_w_varu(b, m->piece_v[i]);
            ub_w_varu(b, m->piece_t[i]);
        }
        send_msg(s, c);
    }
}

static void send_world(ub_repl_server *s, rclient *c)
{
    const ub_world *w = s->w;
    if (!c->world_ok || ub_strcmp(c->wname, w->name) || c->gravity != w->gravity || c->fallen != w->fallen_height) {
        ub_wbuf *b = begin(s, R_WORLD);
        ub_w_str(b, w->name, (u32)ub_strlen(w->name));
        ub_w_f32(b, w->gravity);
        ub_w_f32(b, w->fallen_height);
        send_msg(s, c);
        ub_fmt(c->wname, sizeof c->wname, "%s", w->name);
        c->gravity = w->gravity;
        c->fallen = w->fallen_height;
        c->world_ok = true;
    }
    if (!c->light_ok || memcmp(&c->light, &w->lighting, sizeof c->light)) {
        const ub_lighting *l = &w->lighting;
        ub_wbuf *b = begin(s, R_LIGHTING);
        ub_w_f32(b, l->clock_time);
        ub_w_f32(b, l->latitude);
        ub_w_u32(b, l->ambient);
        ub_w_u32(b, l->outdoor_ambient);
        ub_w_f32(b, l->brightness);
        ub_w_f32(b, l->fog_end);
        ub_w_u32(b, l->fog_color);
        send_msg(s, c);
        c->light = *l;
        c->light_ok = true;
    }
}

/* one instance, whole (its parent is known) */
static void send_create(ub_repl_server *s, rclient *c, i32 i)
{
    const ub_world *w = s->w;
    const ub_instance *in = &w->inst[i];
    int np = ub_world_prop_count(w, i);
    for (int k = 0; k < np; ++k) send_key(s, c, ub_world_prop_at(w, i, k)->key);
    if (!room_for(c, i, in->part)) return;
    ub_wbuf *b = begin(s, R_CREATE);
    ub_w_varu(b, (u32)i);
    ub_w_vari(b, in->parent);
    ub_w_varu(b, in->cls);
    const char *cn = ub_world_str(w, in->class_name), *nm = ub_world_str(w, in->name);
    ub_w_str(b, cn, (u32)ub_strlen(cn));
    ub_w_str(b, nm, (u32)ub_strlen(nm));
    ub_w_varu(b, (u32)(in->flags & (UB_INST_SAVED_FLAGS | UB_INST_RUNTIME)));
    if (in->part >= 0) {
        const ub_part *p = &w->parts[in->part];
        ub_w_u8(b, 1);
        ub_w_varu(b, s->tick);
        put_part(b, p);
        rpart *rp = &c->parts[in->part];
        rp->sent = *p;
        rp->mpos = p->pos;
        rp->mrot = p->rot;
        rp->has = true;
        rp->moving = false;
    } else {
        ub_w_u8(b, 0);
    }
    ub_w_varu(b, (u32)np);
    for (int k = 0; k < np; ++k) {
        const ub_prop *p = ub_world_prop_at(w, i, k);
        ub_w_varu(b, p->key);
        put_value(b, &p->val);
    }
    if (code_visible(w, i)) {
        const char *src = ub_world_source(w, i);
        ub_w_u8(b, 1);
        ub_w_str(b, src, (u32)ub_strlen(src));
    } else {
        ub_w_u8(b, 0);
    }
    send_msg(s, c);
    c->known[i] = 1;
    c->stamp[i] = s->tick + 1;
    c->cfirst[i] = c->clast[i] = -1;
    ct_append(s, c, i, in->parent);
}

/* an instance and all inside it, parents first; ones the client has move */
static void send_subtree(ub_repl_server *s, rclient *c, i32 root)
{
    const ub_world *w = s->w;
    i32 small[256], *stack = small, cap = 256, sp = 0;
    stack[sp++] = root;
    while (sp > 0) {
        i32 i = stack[--sp];
        if (!ub_world_alive(w, i) || hidden_root(w, i)) continue;
        if (i < c->known_cap && c->known[i]) send_parent(s, c, i, w->inst[i].parent);
        else send_create(s, c, i);
        /* children, last first so the first comes off the stack first */
        i32 kids = 0;
        for (i32 k = w->inst[i].first_child; k >= 0; k = w->inst[k].next_sibling) kids++;
        if (sp + kids > cap) {
            i32 nc = (sp + kids) * 2;
            i32 *ns = (i32 *)plat_alloc(sizeof(i32) * (size_t)nc);
            if (!ns) break;
            memcpy(ns, stack, sizeof(i32) * (size_t)sp);
            if (stack != small) plat_free(stack);
            stack = ns;
            cap = nc;
        }
        i32 at = sp + kids;
        for (i32 k = w->inst[i].first_child; k >= 0; k = w->inst[k].next_sibling) stack[--at] = k;
        sp += kids;
    }
    if (stack != small) plat_free(stack);
}

/* forget an instance and all inside it, as the client has them (it
 * destroyed them) */
static void forget_subtree(ub_repl_server *s, rclient *c, i32 root)
{
    const ub_world *w = s->w;
    ct_unlink(c, root);
    i32 small[256], *stack = small, cap = 256, sp = 0;
    stack[sp++] = root;
    while (sp > 0) {
        i32 i = stack[--sp];
        c->known[i] = 0;
        if (w->inst[i].part >= 0 && w->inst[i].part < c->parts_cap) c->parts[w->inst[i].part].has = false;
        for (i32 k = c->cfirst[i]; k >= 0; k = c->cnext[k]) {
            if (sp == cap) {
                i32 *ns = (i32 *)plat_alloc(sizeof(i32) * (size_t)cap * 2);
                if (!ns) break;
                memcpy(ns, stack, sizeof(i32) * (size_t)sp);
                if (stack != small) plat_free(stack);
                stack = ns;
                cap *= 2;
            }
            stack[sp++] = k;
        }
        c->cfirst[i] = c->clast[i] = c->cpar[i] = c->cnext[i] = c->cprev[i] = -1;
    }
    if (stack != small) plat_free(stack);
}

static void send_destroy(ub_repl_server *s, rclient *c, i32 i)
{
    ub_wbuf *b = begin(s, R_DESTROY);
    ub_w_varu(b, (u32)i);
    send_msg(s, c);
    forget_subtree(s, c, i);
}

static void send_prop(ub_repl_server *s, rclient *c, i32 i, u32 key)
{
    const ub_value *v = ub_world_prop(s->w, i, key);
    send_key(s, c, key);
    ub_wbuf *b = begin(s, v ? R_PROP : R_PROP_DEL);
    ub_w_varu(b, (u32)i);
    ub_w_varu(b, key);
    if (v) put_value(b, v);
    send_msg(s, c);
}

static void send_all_props(ub_repl_server *s, rclient *c, i32 i)
{
    const ub_world *w = s->w;
    int np = ub_world_prop_count(w, i);
    for (int k = 0; k < np; ++k) send_key(s, c, ub_world_prop_at(w, i, k)->key);
    ub_wbuf *b = begin(s, R_PROPS_ALL);
    ub_w_varu(b, (u32)i);
    ub_w_varu(b, (u32)np);
    for (int k = 0; k < np; ++k) {
        const ub_prop *p = ub_world_prop_at(w, i, k);
        ub_w_varu(b, p->key);
        put_value(b, &p->val);
    }
    send_msg(s, c);
}

/* --- parts */

static void motion_flush(ub_repl_server *s, rclient *c)
{
    if (!c->motion_n) return;
    /* the count goes after the header: patch it in (a fixed 2 bytes) */
    c->motion.p[c->motion_count_at] = (u8)(c->motion_n & 0xFF);
    c->motion.p[c->motion_count_at + 1] = (u8)(c->motion_n >> 8);
    if (ub_conn_send(c->conn, false, c->motion.p, c->motion.len)) {
        c->st.bytes[R_MOTION] += c->motion.len;
        c->st.messages[R_MOTION]++;
    }
    ub_wbuf_clear(&c->motion);
    c->motion_n = 0;
    (void)s;
}

static void motion_add(ub_repl_server *s, rclient *c, i32 inst, const ub_part *p)
{
    if (c->motion.len + 24 > MOTION_BYTES) motion_flush(s, c);
    if (!c->motion_n) {
        ub_wbuf_clear(&c->motion);
        ub_w_u8(&c->motion, R_MOTION);
        ub_w_varu(&c->motion, s->tick);
        c->motion_count_at = c->motion.len;
        ub_w_u16(&c->motion, 0);
    }
    ub_w_varu(&c->motion, (u32)inst);
    ub_w_vec3(&c->motion, p->pos);
    ub_w_rot(&c->motion, &p->rot);
    c->motion_n++;
}

static void check_part(ub_repl_server *s, rclient *c, i32 pi)
{
    const ub_world *w = s->w;
    if (pi < 0 || pi >= w->part_count || pi >= c->parts_cap) return;
    i32 inst = w->part_inst[pi];
    if (inst < 0 || inst >= c->known_cap || !c->known[inst] || !ub_world_alive(w, inst)) return;
    rpart *rp = &c->parts[pi];
    if (!rp->has) return;
    const ub_part *p = &w->parts[pi];
    if (!same_looks(p, &rp->sent)) {
        ub_wbuf *b = begin(s, R_PART);
        ub_w_varu(b, s->tick);
        ub_w_varu(b, (u32)inst);
        put_part(b, p);
        send_msg(s, c);
        rp->sent = *p;
        rp->mpos = p->pos;
        rp->mrot = p->rot;
        rp->moving = false;
        return;
    }
    if (same_place(&rp->sent, p->pos, &p->rot)) {
        rp->moving = false;
        return;
    }
    if (!same_place(&(ub_part){ .pos = rp->mpos, .rot = rp->mrot }, p->pos, &p->rot) || !rp->moving) {
        /* moving: where it is now, unreliably */
        motion_add(s, c, inst, p);
        rp->mpos = p->pos;
        rp->mrot = p->rot;
        rp->moving = true;
        return;
    }
    /* still since the last tick: where it rests, reliably */
    ub_wbuf *b = begin(s, R_XFORM);
    ub_w_varu(b, s->tick);
    ub_w_varu(b, (u32)inst);
    ub_w_vec3(b, p->pos);
    ub_w_vec3(b, p->rot.x);
    ub_w_vec3(b, p->rot.y);
    ub_w_vec3(b, p->rot.z);
    send_msg(s, c);
    rp->sent.pos = p->pos;
    rp->sent.rot = p->rot;
    rp->moving = false;
}

/* --- the tick */

static void join(ub_repl_server *s, rclient *c)
{
    const ub_world *w = s->w;
    send_world(s, c);
    send_meshes(s, c);
    for (i32 i = 0; i < w->count; ++i)
        if (w->inst[i].parent < 0 && ub_repl_visible(w, i)) send_subtree(s, c, i);
    ub_wbuf *b = begin(s, R_JOINED);
    ub_w_varu(b, s->tick);
    send_msg(s, c);
    c->joined = true;
    c->ntl = 0;
}

static bool is_known(const rclient *c, i32 i) { return i >= 0 && i < c->known_cap && c->known[i]; }

/* A tick's changes to the tree go in three passes, so the client's tree
 * never has to hold something the server's did not: moves of what the
 * client has (into parents it has, and not ones remade this tick); then
 * what is new or remade (with all inside it, which brings along anything
 * moved into it); then what the client may no longer see. */
static void apply_journal(ub_repl_server *s, rclient *c)
{
    const ub_world *w = s->w;
    const ub_journal *j = w->journal;
    if (!j) return;
    /* 1: moves */
    for (i32 n = 0; n < j->n; ++n) {
        i32 i = j->list[n];
        u8 f = j->flags[i];
        if (i >= w->count || !(f & UB_JN_TREE) || (f & UB_JN_NEW) || !is_known(c, i) || !ub_repl_visible(w, i)) continue;
        i32 par = w->inst[i].parent;
        if (par >= 0 && (!is_known(c, par) || (j->flags[par] & UB_JN_NEW))) continue; /* (it comes with its parent) */
        send_parent(s, c, i, par);
    }
    /* 2: new and remade */
    for (i32 n = 0; n < j->n; ++n) {
        i32 i = j->list[n];
        u8 f = j->flags[i];
        if (i >= w->count || !(f & (UB_JN_NEW | UB_JN_TREE))) continue;
        if ((f & UB_JN_NEW) && is_known(c, i)) send_destroy(s, c, i); /* (the slot's last instance) */
        if (is_known(c, i) || !ub_repl_visible(w, i)) continue;
        i32 par = w->inst[i].parent;
        if (par >= 0 && !is_known(c, par)) continue; /* (it comes with its parent) */
        send_subtree(s, c, i);
    }
    /* 3: gone from view */
    for (i32 n = 0; n < j->n; ++n) {
        i32 i = j->list[n];
        if (i < w->count && (j->flags[i] & UB_JN_TREE) && is_known(c, i) && !ub_repl_visible(w, i)) send_destroy(s, c, i);
    }
    /* children that came this tick may be in another order than the
     * server's: send the tail from where they differ */
    for (i32 t = 0; t < c->ntl; ++t) {
        i32 p = c->tlist[t];
        if (!is_known(c, p) || !ub_world_alive(w, p)) continue;
        i32 sc = w->inst[p].first_child, cc = c->cfirst[p];
        for (;;) {
            while (sc >= 0 && !is_known(c, sc)) sc = w->inst[sc].next_sibling;
            if (sc < 0 || cc < 0 || sc != cc) break;
            sc = w->inst[sc].next_sibling;
            cc = c->cnext[cc];
        }
        if (sc < 0) continue; /* the same */
        ub_wbuf *b = begin(s, R_ORDER);
        ub_w_varu(b, (u32)p);
        u32 n = 0;
        for (i32 k = sc; k >= 0; k = w->inst[k].next_sibling) n += is_known(c, k);
        ub_w_varu(b, n);
        for (i32 k = sc; k >= 0; k = w->inst[k].next_sibling)
            if (is_known(c, k)) ub_w_varu(b, (u32)k);
        send_msg(s, c);
        for (i32 k = sc; k >= 0; k = w->inst[k].next_sibling)
            if (is_known(c, k)) {
                ct_unlink(c, k);
                ct_append(s, c, k, p);
            }
    }
    c->ntl = 0;
    /* names, code, properties, parts of what the client has (and did not just get whole) */
    for (i32 n = 0; n < j->n; ++n) {
        i32 i = j->list[n];
        u8 f = j->flags[i];
        if (i >= w->count || !is_known(c, i) || (i < c->stamp_cap && c->stamp[i] == s->tick + 1) || !ub_repl_visible(w, i)) continue;
        if (f & UB_JN_NAME) {
            const char *nm = ub_world_str(w, w->inst[i].name);
            ub_wbuf *b = begin(s, R_NAME);
            ub_w_varu(b, (u32)i);
            ub_w_str(b, nm, (u32)ub_strlen(nm));
            send_msg(s, c);
        }
        if ((f & UB_JN_SOURCE) && code_visible(w, i)) {
            const char *src = ub_world_source(w, i);
            ub_wbuf *b = begin(s, R_SOURCE);
            ub_w_varu(b, (u32)i);
            ub_w_str(b, src, (u32)ub_strlen(src));
            send_msg(s, c);
        }
        if (f & UB_JN_PROPS) send_all_props(s, c, i);
        if (f & UB_JN_PART) check_part(s, c, w->inst[i].part);
    }
    /* properties noted one by one */
    for (i32 k = 0; k < j->nk; ++k) {
        i32 i = j->keys[k].inst;
        if (i >= w->count || !is_known(c, i) || (j->flags[i] & UB_JN_PROPS)) continue;
        if (i < c->stamp_cap && c->stamp[i] == s->tick + 1) continue;
        if (!ub_repl_visible(w, i)) continue;
        send_prop(s, c, i, j->keys[k].key);
    }
}

static void check_parts(ub_repl_server *s, rclient *c)
{
    const ub_world *w = s->w;
    /* every tick: parts physics moves, and ones still moving; a slice of the rest */
    i32 slice = w->part_count / 8 + 64, from = c->roll;
    for (i32 pi = 0; pi < w->part_count && pi < c->parts_cap; ++pi) {
        const ub_part *p = &w->parts[pi];
        bool rolled = ((pi - from + w->part_count) % (w->part_count ? w->part_count : 1)) < slice;
        if (!(p->flags & UB_PART_ANCHORED) || (p->flags & UB_PART_CARRIED) || c->parts[pi].moving || rolled)
            check_part(s, c, pi);
    }
    c->roll = w->part_count ? (from + slice) % w->part_count : 0;
    motion_flush(s, c);
}

void ub_repl_server_tick(ub_repl_server *s)
{
    for (int k = 0; k < MAX_CLIENTS; ++k) {
        rclient *c = &s->c[k];
        if (!c->used) continue;
        if (!room_for(c, s->w->count, s->w->part_count)) continue;
        if (!c->joined) {
            join(s, c); /* (it is the current state: the journal adds nothing) */
        } else {
            send_world(s, c);
            send_meshes(s, c);
            apply_journal(s, c);
            check_parts(s, c);
        }
        c->st.ticks++;
    }
    ub_world_journal_clear(s->w);
    s->tick++;
}

/* ============================================================== client */

/* every reference property the client holds: an instance it points at
 * may go out of view (the reference reads nil) and come back */
typedef struct {
    i32 inst;   /* local */
    u32 key;    /* local */
    i32 net;
} pending_ref;

struct ub_repl_client {
    ub_world *w;
    i32 *local;            /* per net id: local instance + 1 (0: none) */
    i32 local_cap;
    i32 *net;              /* per local instance: net id + 1 */
    i32 net_cap;
    u32 *keys;             /* per server key: local key + 1 */
    i32 keys_cap;
    u16 *meshes;           /* per server mesh id: local mesh id */
    i32 meshes_cap;
    u32 *ptick;            /* per local part: the tick of what it shows */
    i32 ptick_cap;
    pending_ref *pend;
    i32 npend, pend_cap;
    bool joined;
    scratch sc;
};

ub_repl_client *ub_repl_client_new(ub_world *w)
{
    ub_repl_client *c = (ub_repl_client *)plat_alloc(sizeof *c);
    if (!c) return NULL;
    memset(c, 0, sizeof *c);
    c->w = w;
    return c;
}

void ub_repl_client_free(ub_repl_client *c)
{
    if (!c) return;
    plat_free(c->local);
    plat_free(c->net);
    plat_free(c->keys);
    plat_free(c->meshes);
    plat_free(c->ptick);
    plat_free(c->pend);
    plat_free(c);
}

bool ub_repl_client_joined(const ub_repl_client *c) { return c->joined; }

i32 ub_repl_client_local(const ub_repl_client *c, u32 net)
{
    return (i32)net < c->local_cap ? c->local[net] - 1 : -1;
}

i32 ub_repl_client_net(const ub_repl_client *c, i32 local)
{
    return local >= 0 && local < c->net_cap ? c->net[local] - 1 : -1;
}

static void map(ub_repl_client *c, i32 net, i32 local)
{
    if (net < 0 || local < 0) return;
    if (!grow((void **)&c->local, &c->local_cap, net + 1, sizeof(i32)) || !grow((void **)&c->net, &c->net_cap, local + 1, sizeof(i32)))
        return;
    c->local[net] = local + 1;
    c->net[local] = net + 1;
}

static void unmap_local(ub_repl_client *c, i32 local)
{
    i32 net = ub_repl_client_net(c, local);
    if (net >= 0) c->local[net] = 0;
    if (local < c->net_cap) c->net[local] = 0;
}

static u32 local_key(ub_repl_client *c, u32 key)
{
    return (i32)key < c->keys_cap && c->keys[key] ? c->keys[key] - 1 : UB_NO_STRING;
}

/* a value from the server, with its reference (if any) turned local */
static bool set_prop(ub_repl_client *c, i32 inst, u32 skey, ub_value *v)
{
    u32 key = local_key(c, skey);
    if (key == UB_NO_STRING) return false;
    /* a later value replaces the reference it held */
    for (i32 k = 0; k < c->npend; ++k)
        if (c->pend[k].inst == inst && c->pend[k].key == key) c->pend[k--] = c->pend[--c->npend];
    if (v->type == UB_PV_REF && v->v.ref >= 0) {
        i32 net = v->v.ref, loc = ub_repl_client_local(c, (u32)net);
        v->v.ref = loc;
        if (grow((void **)&c->pend, &c->pend_cap, c->npend + 1, sizeof(pending_ref))) {
            c->pend[c->npend].inst = inst;
            c->pend[c->npend].key = key;
            c->pend[c->npend].net = net;
            c->npend++;
        }
    }
    return ub_world_set_prop(c->w, inst, key, v);
}

/* forget the references an instance's property (key, or all: UB_NO_STRING) held */
static void drop_refs(ub_repl_client *c, i32 inst, u32 key)
{
    for (i32 k = 0; k < c->npend; ++k)
        if (c->pend[k].inst == inst && (key == UB_NO_STRING || c->pend[k].key == key)) c->pend[k--] = c->pend[--c->npend];
}

/* references to net now point at `local` (-1: out of view) */
static void resolve_refs(ub_repl_client *c, i32 net, i32 local)
{
    for (i32 k = 0; k < c->npend; ++k) {
        pending_ref *p = &c->pend[k];
        if (p->net != net || !ub_world_alive(c->w, p->inst)) continue;
        ub_value v;
        memset(&v, 0, sizeof v);
        v.type = UB_PV_REF;
        v.v.ref = local;
        ub_world_set_prop(c->w, p->inst, p->key, &v);
    }
}

static void set_part(ub_repl_client *c, i32 inst, const ub_part *np, u32 tick)
{
    ub_world *w = c->w;
    i32 pi = w->inst[inst].part;
    if (pi < 0) return;
    ub_part p = *np;
    p.mesh = (i32)p.mesh < c->meshes_cap ? c->meshes[p.mesh] : 0;
    p.look = (i32)p.look < c->meshes_cap ? c->meshes[p.look] : 0;
    w->parts[pi] = p;
    if (grow((void **)&c->ptick, &c->ptick_cap, pi + 1, sizeof(u32))) c->ptick[pi] = tick + 1;
    w->epoch++;
}

/* destroys a local instance and all inside it, and frees their slots */
static void destroy_local(ub_repl_client *c, i32 root)
{
    ub_world *w = c->w;
    if (!ub_world_alive(w, root)) return;
    i32 n = 0, cap = 64, *list = (i32 *)plat_alloc(sizeof(i32) * 64);
    if (!list) return;
    list[n++] = root;
    for (i32 k = 0; k < n; ++k)
        for (i32 ch = w->inst[list[k]].first_child; ch >= 0; ch = w->inst[ch].next_sibling) {
            if (n == cap) {
                i32 *nl = (i32 *)plat_alloc(sizeof(i32) * (size_t)cap * 2);
                if (!nl) break;
                memcpy(nl, list, sizeof(i32) * (size_t)n);
                plat_free(list);
                list = nl;
                cap *= 2;
            }
            list[n++] = ch;
        }
    ub_world_destroy(w, root);
    for (i32 k = n - 1; k >= 0; --k) {
        i32 net = ub_repl_client_net(c, list[k]);
        unmap_local(c, list[k]);
        if (net >= 0) resolve_refs(c, net, -1);
        ub_world_reclaim(w, list[k]);
    }
    for (i32 k = 0; k < c->npend; ++k)
        if (!ub_world_alive(w, c->pend[k].inst)) c->pend[k--] = c->pend[--c->npend];
    plat_free(list);
}

static i32 parent_local(ub_repl_client *c, i32 pnet)
{
    return pnet < 0 ? -1 : ub_repl_client_local(c, (u32)pnet);
}

bool ub_repl_client_apply(ub_repl_client *c, const u8 *msg, u32 len)
{
    ub_world *w = c->w;
    ub_rbuf r;
    ub_rbuf_init(&r, msg, len);
    u8 type = ub_r_u8(&r);
    switch (type) {
    case R_KEY: {
        u32 key = ub_r_varu(&r), n;
        const char *s = ub_r_str(&r, &n);
        if (!s || n >= 256 || key > 1000000) return false;
        char name[256];
        memcpy(name, s, n);
        name[n] = 0;
        u32 lk = ub_world_key(w, name);
        if (lk == UB_NO_STRING || !grow((void **)&c->keys, &c->keys_cap, (i32)key + 1, sizeof(u32))) return false;
        c->keys[key] = lk + 1;
        return true;
    }
    case R_CREATE: {
        u32 net = ub_r_varu(&r);
        i32 pnet = ub_r_vari(&r);
        u32 cls = ub_r_varu(&r), cnl, nml;
        const char *cn = ub_r_str(&r, &cnl), *nm = ub_r_str(&r, &nml);
        u32 flags = ub_r_varu(&r);
        if (r.err || cnl >= 128 || nml >= 256 || net > 0x7FFFFFF) return false;
        char cbuf[128], nbuf[256];
        memcpy(cbuf, cn, cnl);
        cbuf[cnl] = 0;
        memcpy(nbuf, nm, nml);
        nbuf[nml] = 0;
        i32 old = ub_repl_client_local(c, net);
        if (old >= 0) destroy_local(c, old); /* (a slot the server reused) */
        i32 parent = parent_local(c, pnet);
        if (pnet >= 0 && parent < 0) return false;
        i32 inst;
        u32 ptick = 0;
        if (ub_r_u8(&r)) {
            ptick = ub_r_varu(&r);
            ub_part p;
            get_part(&r, &p);
            if (r.err) return false;
            ub_part zero;
            memset(&zero, 0, sizeof zero);
            zero.rot = mat3_identity();
            inst = ub_world_add_part(w, parent, (ub_class)cls, cbuf, nbuf, &zero);
            if (inst >= 0) set_part(c, inst, &p, ptick);
        } else {
            inst = ub_world_add(w, parent, (ub_class)cls, cbuf, nbuf);
        }
        if (inst < 0) return false;
        w->inst[inst].flags = (u16)((flags & (UB_INST_SAVED_FLAGS | UB_INST_RUNTIME)) | (w->inst[inst].flags & UB_INST_DETACHED));
        map(c, (i32)net, inst);
        u32 np = ub_r_varu(&r);
        for (u32 k = 0; k < np && !r.err; ++k) {
            u32 key = ub_r_varu(&r);
            ub_value v;
            if (!get_value(&r, &v, &c->sc)) return false;
            set_prop(c, inst, key, &v);
        }
        if (ub_r_u8(&r)) {
            u32 n;
            const char *src = ub_r_str(&r, &n);
            if (!src) return false;
            char *copy = (char *)plat_alloc(n + 1);
            if (copy) {
                memcpy(copy, src, n);
                copy[n] = 0;
                ub_world_set_source(w, inst, copy);
                plat_free(copy);
            }
        }
        resolve_refs(c, (i32)net, inst);
        return !r.err;
    }
    case R_DESTROY: {
        i32 inst = ub_repl_client_local(c, ub_r_varu(&r));
        if (inst >= 0) destroy_local(c, inst);
        return !r.err;
    }
    case R_PARENT: {
        i32 inst = ub_repl_client_local(c, ub_r_varu(&r));
        i32 pnet = ub_r_vari(&r);
        if (r.err || inst < 0) return false;
        i32 parent = parent_local(c, pnet);
        if (pnet >= 0 && parent < 0) return false;
        if (parent >= 0 && ub_world_is_descendant(w, parent, inst)) {
            /* the new parent is still inside it here: its own move comes
             * later in this tick, so lift it out for now */
            ub_world_set_parent(w, parent, -1);
        }
        return ub_world_set_parent(w, inst, parent);
    }
    case R_NAME: case R_SOURCE: {
        i32 inst = ub_repl_client_local(c, ub_r_varu(&r));
        u32 n;
        const char *s = ub_r_str(&r, &n);
        if (!s || inst < 0) return false;
        char *copy = (char *)plat_alloc(n + 1);
        if (!copy) return false;
        memcpy(copy, s, n);
        copy[n] = 0;
        if (type == R_NAME) ub_world_set_name(w, inst, copy);
        else ub_world_set_source(w, inst, copy);
        plat_free(copy);
        return true;
    }
    case R_PROP: case R_PROP_DEL: {
        i32 inst = ub_repl_client_local(c, ub_r_varu(&r));
        u32 key = ub_r_varu(&r);
        if (r.err || inst < 0) return false;
        if (type == R_PROP_DEL) {
            u32 lk = local_key(c, key);
            if (lk != UB_NO_STRING) {
                drop_refs(c, inst, lk);
                ub_world_remove_prop(w, inst, lk);
            }
            return true;
        }
        ub_value v;
        if (!get_value(&r, &v, &c->sc)) return false;
        return set_prop(c, inst, key, &v);
    }
    case R_PROPS_ALL: {
        i32 inst = ub_repl_client_local(c, ub_r_varu(&r));
        u32 np = ub_r_varu(&r);
        if (r.err || inst < 0) return false;
        drop_refs(c, inst, UB_NO_STRING);
        ub_world_clear_props(w, inst);
        for (u32 k = 0; k < np && !r.err; ++k) {
            u32 key = ub_r_varu(&r);
            ub_value v;
            if (!get_value(&r, &v, &c->sc)) return false;
            set_prop(c, inst, key, &v);
        }
        return !r.err;
    }
    case R_PART: {
        u32 tick = ub_r_varu(&r);
        i32 inst = ub_repl_client_local(c, ub_r_varu(&r));
        ub_part p;
        get_part(&r, &p);
        if (r.err || inst < 0) return false;
        set_part(c, inst, &p, tick);
        return true;
    }
    case R_XFORM: {
        u32 tick = ub_r_varu(&r);
        i32 inst = ub_repl_client_local(c, ub_r_varu(&r));
        vec3 pos = ub_r_vec3(&r);
        mat3 rot;
        rot.x = ub_r_vec3(&r);
        rot.y = ub_r_vec3(&r);
        rot.z = ub_r_vec3(&r);
        if (r.err || inst < 0 || w->inst[inst].part < 0) return false;
        i32 pi = w->inst[inst].part;
        w->parts[pi].pos = pos;
        w->parts[pi].rot = rot;
        if (grow((void **)&c->ptick, &c->ptick_cap, pi + 1, sizeof(u32))) c->ptick[pi] = tick + 1;
        w->epoch++;
        return true;
    }
    case R_MOTION: {
        u32 tick = ub_r_varu(&r), n = ub_r_u16(&r);
        for (u32 k = 0; k < n && !r.err; ++k) {
            i32 inst = ub_repl_client_local(c, ub_r_varu(&r));
            vec3 pos = ub_r_vec3(&r);
            mat3 rot = ub_r_rot(&r);
            if (r.err || inst < 0 || w->inst[inst].part < 0) continue;
            i32 pi = w->inst[inst].part;
            /* only if newer than what it shows (motion is unordered) */
            if (pi < c->ptick_cap && c->ptick[pi] > tick) continue;
            w->parts[pi].pos = pos;
            w->parts[pi].rot = rot;
            if (grow((void **)&c->ptick, &c->ptick_cap, pi + 1, sizeof(u32))) c->ptick[pi] = tick + 1;
        }
        w->epoch++;
        return !r.err;
    }
    case R_LIGHTING: {
        ub_lighting l;
        l.clock_time = ub_r_f32(&r);
        l.latitude = ub_r_f32(&r);
        l.ambient = ub_r_u32(&r);
        l.outdoor_ambient = ub_r_u32(&r);
        l.brightness = ub_r_f32(&r);
        l.fog_end = ub_r_f32(&r);
        l.fog_color = ub_r_u32(&r);
        if (r.err) return false;
        w->lighting = l;
        w->epoch++;
        return true;
    }
    case R_WORLD: {
        u32 n;
        const char *s = ub_r_str(&r, &n);
        f32 g = ub_r_f32(&r), fh = ub_r_f32(&r);
        if (!s || r.err) return false;
        if (n >= sizeof w->name) n = sizeof w->name - 1;
        memcpy(w->name, s, n);
        w->name[n] = 0;
        w->gravity = g;
        w->fallen_height = fh;
        return true;
    }
    case R_MESH: {
        u32 id = ub_r_varu(&r), flags = ub_r_varu(&r), nv = ub_r_varu(&r), nt = ub_r_varu(&r), np = ub_r_varu(&r);
        if (r.err || id == 0 || id > 65535 || nv > 1000000 || nt > 2000000 || np > 100000 || ub_r_left(&r) < nv * 12) return false;
        f32 *v = (f32 *)plat_alloc(sizeof(f32) * nv * 3 + 1);
        u32 *tri = (u32 *)plat_alloc(sizeof(u32) * nt * 3 + 1);
        u32 *pv = (u32 *)plat_alloc(sizeof(u32) * np + 1), *pt = (u32 *)plat_alloc(sizeof(u32) * np + 1);
        bool ok = v && tri && pv && pt;
        for (u32 i = 0; ok && i < nv * 3; ++i) v[i] = ub_r_f32(&r);
        for (u32 i = 0; ok && i < nt * 3; ++i) tri[i] = ub_r_varu(&r);
        for (u32 i = 0; ok && i < np; ++i) {
            pv[i] = ub_r_varu(&r);
            pt[i] = ub_r_varu(&r);
        }
        u16 local = ok && !r.err ? ub_world_add_mesh(w, v, nv, tri, nt, pv, pt, np, flags) : 0;
        plat_free(v);
        plat_free(tri);
        plat_free(pv);
        plat_free(pt);
        if (!local || !grow((void **)&c->meshes, &c->meshes_cap, (i32)id + 1, sizeof(u16))) return false;
        c->meshes[id] = local;
        return true;
    }
    case R_JOINED:
        c->joined = true;
        return true;
    case R_ORDER: {
        i32 p = ub_repl_client_local(c, ub_r_varu(&r));
        u32 n = ub_r_varu(&r);
        if (r.err || p < 0) return false;
        for (u32 k = 0; k < n && !r.err; ++k) {
            i32 ch = ub_repl_client_local(c, ub_r_varu(&r));
            if (ch >= 0 && w->inst[ch].parent == p) {
                ub_world_set_parent(w, ch, -1);  /* (to the end: off and on again) */
                ub_world_set_parent(w, ch, p);
            }
        }
        return !r.err;
    }
    default:
        return false;
    }
}
