/* Uniblox - host tests for the network layer (src/net). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/ub_conn.h"
#include "net/ub_link.h"
#include "net/ub_netbuf.h"
#include "net/ub_repl.h"
#include "game/ub_demo_place.h"
#include "world/ub_classes.h"
#include "test.h"

/* ------------------------------------------------------------ buffers */

static void test_buffers(void)
{
    ub_wbuf w;
    ub_wbuf_init(&w, NULL, 0);
    static const u32 vals[] = { 0, 1, 127, 128, 300, 16383, 16384, 0x7FFFFFFF, 0xFFFFFFFFu };
    static const i32 ivals[] = { 0, -1, 1, -64, 64, -100000, 2147483647, -2147483647 - 1 };
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; ++i) ub_w_varu(&w, vals[i]);
    for (size_t i = 0; i < sizeof ivals / sizeof ivals[0]; ++i) ub_w_vari(&w, ivals[i]);
    ub_w_u8(&w, 0xAB);
    ub_w_u16(&w, 0xBEEF);
    ub_w_u32(&w, 0xDEADBEEFu);
    ub_w_f32(&w, -1.5f);
    ub_w_f64(&w, 3.25);
    ub_w_str(&w, "hello", 5);
    ub_w_vec3(&w, v3(1, -2, 3.5f));
    CHECK(!w.err);
    ub_rbuf r;
    ub_rbuf_init(&r, w.p, w.len);
    bool ok = true;
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; ++i) ok &= ub_r_varu(&r) == vals[i];
    for (size_t i = 0; i < sizeof ivals / sizeof ivals[0]; ++i) ok &= ub_r_vari(&r) == ivals[i];
    CHECK(ok);
    CHECK(ub_r_u8(&r) == 0xAB && ub_r_u16(&r) == 0xBEEF && ub_r_u32(&r) == 0xDEADBEEFu);
    CHECK(ub_r_f32(&r) == -1.5f && ub_r_f64(&r) == 3.25);
    u32 n;
    const char *s = ub_r_str(&r, &n);
    CHECK(s && n == 5 && !memcmp(s, "hello", 5));
    vec3 v = ub_r_vec3(&r);
    CHECK(v.x == 1 && v.y == -2 && v.z == 3.5f && !r.err && ub_r_left(&r) == 0);
    /* reading past the end: zeros and an error, never out of bounds */
    CHECK(ub_r_u32(&r) == 0 && r.err);
    /* a fixed buffer refuses to grow */
    u8 small[4];
    ub_wbuf f;
    ub_wbuf_init(&f, small, sizeof small);
    ub_w_u32(&f, 1);
    CHECK(!f.err);
    ub_w_u8(&f, 1);
    CHECK(f.err && f.len == 4);
    /* a malformed varint (too long) is an error */
    u8 bad[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
    ub_rbuf_init(&r, bad, sizeof bad);
    ub_r_varu(&r);
    CHECK(r.err);
    /* rotations: 6 bytes, every element within 1e-3 */
    srand(7);
    f32 worst = 0;
    for (int i = 0; i < 2000; ++i) {
        mat3 m = mat3_from_euler((f32)(rand() % 6283) / 1000.0f, (f32)(rand() % 6283) / 1000.0f, (f32)(rand() % 6283) / 1000.0f);
        if (i == 0) m = mat3_identity();
        ub_wbuf_clear(&w);
        ub_w_rot(&w, &m);
        CHECK(w.len == 6);
        ub_rbuf_init(&r, w.p, w.len);
        mat3 b = ub_r_rot(&r);
        const f32 *pa = &m.x.x, *pb = &b.x.x;
        for (int k = 0; k < 9; ++k) {
            f32 e = fabsf(pa[k] - pb[k]);
            if (e > worst) worst = e;
        }
    }
    printf("  rotations in 6 bytes: largest error %.2e\n", (double)worst);
    CHECK(worst < 1e-3f);
    ub_wbuf_free(&w);
}

/* --------------------------------------------------------- connection */

typedef struct {
    ub_link *link;
    int from;
    u64 now;
} out_ctx;

static void to_link(void *ctx, const u8 *pkt, u32 len)
{
    out_ctx *o = (out_ctx *)ctx;
    ub_link_send(o->link, o->from, pkt, len, o->now);
}

/* A message's bytes from its number: what it should hold. */
static u32 msg_size(u32 i) { return i % 97 == 0 ? 100000 + i : (i * 2654435761u) % 3000; }
static u8 msg_byte(u32 i, u32 k) { return (u8)(i * 31u + k * 7u); }

/* n reliable messages from end 0 to end 1 (and replies back) through a
 * link, stepping 10 ms at a time; returns the steps it took (or -1). */
static int run_link(const ub_link_sim *sim, u32 n, u32 rate, ub_conn_stats *st0)
{
    ub_link *l = ub_link_new(sim);
    ub_conn *a = ub_conn_new(rate), *b = ub_conn_new(rate);
    static u8 buf[200000], pkt[UB_LINK_MTU];
    u32 sent = 0, got = 0, back = 0, unrel = 0, bad = 0;
    static u8 seen[20000];
    memset(seen, 0, sizeof seen);
    int steps = 0;
    u64 now = 1000;
    while ((got < n || back < n) && steps < 20000) {
        /* a's messages, a few at a time */
        for (int k = 0; k < 7 && sent < n; ++k, ++sent) {
            u32 sz = msg_size(sent);
            for (u32 j = 0; j < sz; ++j) buf[j] = msg_byte(sent, j);
            if (sz >= 4) memcpy(buf, &sent, 4);
            CHECK(ub_conn_send(a, true, buf, sz));
        }
        /* and an unreliable one each step */
        u32 tick = (u32)steps;
        CHECK(ub_conn_send(a, false, &tick, 4));
        out_ctx oa = { l, 0, now }, ob = { l, 1, now };
        ub_conn_flush(a, now, to_link, &oa);
        ub_conn_flush(b, now, to_link, &ob);
        now += 10000;
        int len;
        while ((len = ub_link_recv(l, 1, pkt, sizeof pkt, now)) >= 0) ub_conn_receive(b, pkt, (u32)len, now);
        while ((len = ub_link_recv(l, 0, pkt, sizeof pkt, now)) >= 0) ub_conn_receive(a, pkt, (u32)len, now);
        const u8 *m;
        u32 ml;
        bool rel;
        while (ub_conn_next(b, &m, &ml, &rel)) {
            if (!rel) {
                u32 t;
                memcpy(&t, m, 4);
                if (ml != 4 || t >= 20000 || seen[t]) bad++; /* never twice (but in any order) */
                else seen[t] = 1;
                unrel++;
                continue;
            }
            u32 want = got, sz = msg_size(want);
            bool same = ml == sz;
            for (u32 j = sz >= 4 ? 4 : 0; same && j < sz; ++j) same = m[j] == msg_byte(want, j);
            if (same && sz >= 4) {
                u32 id;
                memcpy(&id, m, 4);
                same = id == want;
            }
            if (!same) bad++;
            got++;
            ub_conn_send(b, true, &want, 4); /* a reply */
        }
        while (ub_conn_next(a, &m, &ml, &rel)) {
            u32 id;
            memcpy(&id, m, 4);
            if (!rel || ml != 4 || id != back) bad++;
            back++;
        }
        steps++;
    }
    ub_conn_get_stats(a, st0);
    CHECK(bad == 0 && got == n && back == n);
    if (bad) printf("    %u bad messages\n", bad);
    CHECK(unrel > 0);
    ub_conn_free(a);
    ub_conn_free(b);
    ub_link_free(l);
    return got == n && back == n ? steps : -1;
}

static void test_connection(void)
{
    ub_conn_stats st;
    /* a perfect link */
    int steps = run_link(NULL, 2000, 0, &st);
    printf("  2000 messages (%llu KB), perfect link: %d steps of 10 ms, %u packets, %u resent\n",
           (unsigned long long)(st.reliable_out / 1024), steps, st.packets_out, st.segments_resent);
    CHECK(steps > 0 && st.segments_resent == 0);
    /* a bad one: 20% lost, 5% twice, 40-100 ms each way */
    ub_link_sim bad = { 40, 60, 200, 50, 0, 12345 };
    steps = run_link(&bad, 2000, 0, &st);
    printf("  the same, 20%% lost, 5%% twice, 40-100 ms: %d steps, %u resent, round trip %u ms\n", steps,
           st.segments_resent, st.rtt_ms);
    CHECK(steps > 0 && st.segments_resent > 0 && st.rtt_ms > 60 && st.rtt_ms < 400);
    /* 56k: 5 KB/s each way, with a matching budget */
    ub_link_sim slow = { 60, 10, 0, 0, 5000, 9 };
    ub_link *l = ub_link_new(&slow);
    ub_conn *a = ub_conn_new(5000), *b = ub_conn_new(5000);
    static u8 big[50000];
    for (u32 i = 0; i < sizeof big; ++i) big[i] = (u8)i;
    CHECK(ub_conn_send(a, true, big, sizeof big));
    u64 now = 0;
    int t = 0;
    bool done = false;
    static u8 pkt[UB_LINK_MTU];
    while (!done && t < 3000) {
        out_ctx oa = { l, 0, now }, ob = { l, 1, now };
        ub_conn_flush(a, now, to_link, &oa);
        ub_conn_flush(b, now, to_link, &ob);
        now += 10000;
        int len;
        while ((len = ub_link_recv(l, 1, pkt, sizeof pkt, now)) >= 0) ub_conn_receive(b, pkt, (u32)len, now);
        while ((len = ub_link_recv(l, 0, pkt, sizeof pkt, now)) >= 0) ub_conn_receive(a, pkt, (u32)len, now);
        const u8 *m;
        u32 ml;
        bool rel;
        while (ub_conn_next(b, &m, &ml, &rel)) done = rel && ml == sizeof big && !memcmp(m, big, sizeof big);
        t++;
    }
    printf("  50 KB over 56k (5 KB/s): %.1f s\n", (double)t / 100.0);
    CHECK(done && t > 900 && t < 1400); /* about 10 s, not faster than the line */
    for (int k = 0; k < 50; ++k) { /* the last acknowledgements come back */
        out_ctx oa = { l, 0, now }, ob = { l, 1, now };
        ub_conn_flush(a, now, to_link, &oa);
        ub_conn_flush(b, now, to_link, &ob);
        now += 10000;
        int len;
        while ((len = ub_link_recv(l, 1, pkt, sizeof pkt, now)) >= 0) ub_conn_receive(b, pkt, (u32)len, now);
        while ((len = ub_link_recv(l, 0, pkt, sizeof pkt, now)) >= 0) ub_conn_receive(a, pkt, (u32)len, now);
    }
    CHECK(ub_conn_backlog(a) == 0);
    ub_conn_free(a);
    ub_conn_free(b);
    ub_link_free(l);
    /* rubbish packets do no harm */
    ub_conn *c = ub_conn_new(0);
    srand(3);
    for (int i = 0; i < 20000; ++i) {
        u8 junk[64];
        int n = rand() % 64;
        for (int k = 0; k < n; ++k) junk[k] = (u8)rand();
        ub_conn_receive(c, junk, (u32)n, 0);
    }
    const u8 *m;
    u32 ml;
    bool rel;
    int msgs = 0;
    while (ub_conn_next(c, &m, &ml, &rel) && msgs < 100000) msgs++;
    CHECK(msgs < 100000);
    ub_conn_free(c);
}

/* -------------------------------------------------------- replication */

static u32 g_rng = 1;
static u32 rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static f32 rndf(f32 lo, f32 hi) { return lo + (hi - lo) * (f32)(rnd() % 10001) / 10000.0f; }

typedef struct {
    ub_world sw, cw;
    ub_repl_server *rs;
    ub_repl_client *rc;
    ub_link *link;
    ub_conn *a, *b;
    u64 now;
    u32 bad_msgs;
} rig;

static void rig_step(rig *g)
{
    ub_repl_server_tick(g->rs);
    out_ctx oa = { g->link, 0, g->now }, ob = { g->link, 1, g->now };
    ub_conn_flush(g->a, g->now, to_link, &oa);
    ub_conn_flush(g->b, g->now, to_link, &ob);
    g->now += 50000; /* 20 ticks a second */
    static u8 pkt[UB_LINK_MTU];
    int len;
    while ((len = ub_link_recv(g->link, 1, pkt, sizeof pkt, g->now)) >= 0) ub_conn_receive(g->b, pkt, (u32)len, g->now);
    while ((len = ub_link_recv(g->link, 0, pkt, sizeof pkt, g->now)) >= 0) ub_conn_receive(g->a, pkt, (u32)len, g->now);
    const u8 *m;
    u32 ml;
    bool rel;
    while (ub_conn_next(g->b, &m, &ml, &rel))
        if (!ub_repl_client_apply(g->rc, m, ml)) {
            if (g->bad_msgs++ < 5) printf("    message type %d refused\n", ml ? m[0] : -1);
        }
    while (ub_conn_next(g->a, &m, &ml, &rel)) {}
}

static void rig_open(rig *g, const ub_link_sim *sim)
{
    g->rs = ub_repl_server_new(&g->sw);
    ub_world_init(&g->cw);
    g->rc = ub_repl_client_new(&g->cw);
    g->link = ub_link_new(sim);
    g->a = ub_conn_new(0);
    g->b = ub_conn_new(0);
    g->now = 1000;
    g->bad_msgs = 0;
    CHECK(g->rs && g->rc && ub_repl_server_add(g->rs, g->a) == 0);
}

static void rig_close(rig *g)
{
    ub_repl_server_free(g->rs);
    ub_repl_client_free(g->rc);
    ub_conn_free(g->a);
    ub_conn_free(g->b);
    ub_link_free(g->link);
    ub_world_free(&g->sw);
    ub_world_free(&g->cw);
}

/* until everything sent has arrived and parts have come to rest */
static void rig_settle(rig *g)
{
    for (int i = 0; i < 4000; ++i) {
        rig_step(g);
        if (i > 20 && ub_conn_backlog(g->a) == 0) break;
    }
    for (int i = 0; i < 20; ++i) rig_step(g);
}

static int g_diffs;
static void diff(const char *what, i32 inst)
{
    if (g_diffs++ < 8) printf("    differs: %s (server instance %d)\n", what, inst);
}

static bool same_val(const rig *g, const ub_value *sv, const ub_value *cv)
{
    if (sv->type != cv->type) return false;
    if (sv->type != UB_PV_REF) return ub_value_equal(sv, cv);
    i32 want = sv->v.ref >= 0 && ub_repl_visible(&g->sw, sv->v.ref) ? ub_repl_client_local(g->rc, (u32)sv->v.ref) : -1;
    return cv->v.ref == want;
}

/* the client's world is exactly what it may see of the server's */
static void compare_worlds(const rig *g)
{
    const ub_world *s = &g->sw, *c = &g->cw;
    g_diffs = 0;
    i32 visible = 0;
    for (i32 i = 0; i < s->count; ++i) {
        if (!ub_repl_visible(s, i)) continue;
        visible++;
        i32 l = ub_repl_client_local(g->rc, (u32)i);
        if (l < 0 || !ub_world_alive(c, l)) { diff("missing", i); continue; }
        const ub_instance *a = &s->inst[i], *b = &c->inst[l];
        if (strcmp(ub_world_str(s, a->class_name), ub_world_str(c, b->class_name)) || a->cls != b->cls) diff("class", i);
        if (strcmp(ub_world_str(s, a->name), ub_world_str(c, b->name))) diff("name", i);
        i32 sp = a->parent, cp = b->parent;
        if ((sp < 0) != (cp < 0) || (sp >= 0 && ub_repl_client_local(g->rc, (u32)sp) != cp)) diff("parent", i);
        /* children: the same visible ones, in the same order */
        i32 sc = a->first_child, cc = b->first_child;
        for (;;) {
            while (sc >= 0 && !ub_repl_visible(s, sc)) sc = s->inst[sc].next_sibling;
            if (sc < 0 || cc < 0) break;
            if (ub_repl_client_local(g->rc, (u32)sc) != cc) { diff("child order", i); break; }
            sc = s->inst[sc].next_sibling;
            cc = c->inst[cc].next_sibling;
        }
        if ((sc < 0) != (cc < 0)) diff("children", i);
        int np = ub_world_prop_count(s, i);
        if (np != ub_world_prop_count(c, l)) {
            diff("property count", i);
            for (int k = 0; k < ub_world_prop_count(c, l); ++k) {
                const char *kn = ub_world_key_name(c, ub_world_prop_at(c, l, k)->key);
                if (!ub_world_prop_named(s, i, kn)) printf("      the client has %s too (%s)\n", kn, ub_world_str(s, a->class_name));
            }
        }
        for (int k = 0; k < np; ++k) {
            const ub_prop *p = ub_world_prop_at(s, i, k);
            const ub_value *cv = ub_world_prop_named(c, l, ub_world_key_name(s, p->key));
            if (!cv || !same_val(g, &p->val, cv)) diff(ub_world_key_name(s, p->key), i);
        }
        const char *cn = ub_world_str(s, a->class_name);
        bool code = (a->flags & UB_INST_CLIENT) || !strcmp(cn, "LocalScript") || !strcmp(cn, "ModuleScript");
        if (strcmp(code ? ub_world_source(s, i) : "", ub_world_source(c, l))) diff("source", i);
        if ((a->part >= 0) != (b->part >= 0)) diff("part", i);
        else if (a->part >= 0) {
            const ub_part *x = &s->parts[a->part], *y = &c->parts[b->part];
            if (memcmp(&x->pos, &y->pos, sizeof x->pos) || memcmp(&x->rot, &y->rot, sizeof x->rot)) diff("part place", i);
            if (memcmp(&x->size, &y->size, sizeof x->size) || x->color != y->color || x->shape != y->shape ||
                x->material != y->material || x->transparency != y->transparency || x->flags != y->flags)
                diff("part looks", i);
            if ((x->mesh != 0) != (y->mesh != 0) || (x->mesh && s->meshes[x->mesh - 1].nt != c->meshes[y->mesh - 1].nt))
                diff("part mesh", i);
        }
    }
    i32 alive = 0;
    for (i32 i = 0; i < c->count; ++i) alive += ub_world_alive(c, i);
    if (alive != visible) {
        printf("    the client has %d instances, the server shows %d\n", alive, visible);
        g_diffs++;
    }
    if (memcmp(&s->lighting, &c->lighting, sizeof s->lighting) || strcmp(s->name, c->name) || s->gravity != c->gravity)
        diff("lighting or world", -1);
    CHECK(g_diffs == 0);
}

static i32 pick(const ub_world *w, bool alive_only)
{
    for (int t = 0; t < 50; ++t) {
        i32 i = (i32)(rnd() % (u32)w->count);
        if (!alive_only || ub_world_alive(w, i)) return i;
    }
    return -1;
}

static bool is_service(const ub_world *w, i32 i) { return w->inst[i].parent < 0 && !(w->inst[i].flags & UB_INST_DETACHED); }

static void mutate(rig *g, i32 storage, u16 mesh)
{
    ub_world *w = &g->sw;
    u32 op = rnd() % 100;
    i32 i = pick(w, true), j = pick(w, true);
    if (i < 0 || j < 0) return;
    char name[32];
    snprintf(name, sizeof name, "n%u", rnd() % 1000);
    if (op < 15) { /* make something */
        i32 parent = rnd() % 4 == 0 ? storage : rnd() % 8 == 0 ? -1 : (is_service(w, j) || rnd() % 2 ? j : w->workspace);
        if (rnd() % 2) {
            ub_part p;
            memset(&p, 0, sizeof p);
            p.pos = v3(rndf(-50, 50), rndf(0, 20), rndf(-50, 50));
            p.rot = mat3_from_euler(rndf(0, 6), rndf(0, 6), 0);
            p.size = v3(rndf(1, 5), rndf(1, 5), rndf(1, 5));
            p.color = rnd() & 0xFFFFFF;
            p.flags = rnd() % 2 ? UB_PART_ANCHORED : 0;
            p.mesh = rnd() % 5 == 0 ? mesh : 0;
            ub_world_add_part(w, parent, UB_CLASS_PART, NULL, name, &p);
        } else {
            static const char *const cls[] = { "Folder", "Model", "LocalScript", "Script", "ModuleScript", "IntValue" };
            const char *cn = cls[rnd() % 6];
            i32 n = ub_world_add(w, parent, UB_CLASS_OTHER, cn, name);
            if (n >= 0 && cn[1] != 'o' && cn[0] != 'F' && cn[0] != 'I') ub_world_set_source(w, n, "print('hi')");
        }
    } else if (op < 27) { /* move it somewhere */
        if (!is_service(w, i)) ub_world_set_parent(w, i, rnd() % 5 == 0 ? storage : rnd() % 10 == 0 ? -1 : j);
    } else if (op < 35) { /* destroy it (and sometimes free its slot for reuse) */
        if (!is_service(w, i) && i != storage) {
            ub_world_destroy(w, i);
            if (rnd() % 2) ub_world_reclaim(w, i);
        }
    } else if (op < 42) {
        ub_world_set_name(w, i, name);
    } else if (op < 70) { /* properties of all kinds */
        ub_value v;
        memset(&v, 0, sizeof v);
        static f32 seqk[8] = { 0, 1, 0, 1, 0.5f, 0, 1, 1 };
        static const char *const keys[] = { "Value", "Target", "Text", "Size", "Color", "Ratio", "Flag", "Seq", "@Attr", "Font", "Where" };
        int k = (int)(rnd() % 11);
        switch (k) {
        case 0: v = ub_vint((i32)(rnd() % 2000) - 1000); break;
        case 1: v.type = UB_PV_REF; v.v.ref = rnd() % 3 ? j : -1; break;
        case 2: v = ub_vstr(name, (u32)strlen(name)); break;
        case 3: v.type = UB_PV_UDIM2; v.v.ud2.sx = rndf(0, 1); v.v.ud2.ox = (i32)(rnd() % 100); break;
        case 4: v.type = UB_PV_COLOR3; v.v.c3.r = rndf(0, 1); v.v.c3.g = rndf(0, 1); v.v.c3.b = 0.5f; break;
        case 5: v.type = UB_PV_DOUBLE; v.v.d = rndf(-1, 1) * 1e6; break;
        case 6: v = ub_vbool(rnd() % 2); break;
        case 7: v.type = UB_PV_NUMSEQ; v.v.seq.k = seqk; v.v.seq.n = 2; break;
        case 8: v = ub_venum(rnd() % 50); break;
        case 9: v.type = UB_PV_FONT; v.v.font.family = (char *)"rbxasset://fonts/families/Arial.json"; v.v.font.weight = 700; break;
        default: {
            static f32 cf[12];
            for (int q = 0; q < 12; ++q) cf[q] = rndf(-3, 3);
            v.type = UB_PV_CFRAME;
            v.v.cf.m = cf;
        }
        }
        if (rnd() % 8 == 0) ub_world_remove_prop(w, i, ub_world_key(w, keys[k]));
        else ub_world_set_prop_named(w, i, keys[k], &v);
    } else if (op < 75) {
        if (w->inst[i].source != UB_NO_STRING) ub_world_set_source(w, i, rnd() % 2 ? "return 1" : "print(2)");
    } else if (op < 97) { /* parts: they move (as physics moves them), change, rest */
        i32 pi = w->inst[i].part;
        if (pi < 0) return;
        ub_part *p = &w->parts[pi];
        u32 what = rnd() % 10;
        if (what < 6) {
            p->pos = v3_add(p->pos, v3(rndf(-1, 1), rndf(-1, 1), rndf(-1, 1)));
            p->rot = mat3_from_euler(rndf(0, 6), rndf(0, 6), rndf(0, 6));
            if (what == 0) p->flags &= (u8)~UB_PART_ANCHORED;
        } else if (what < 8) {
            p->color = rnd() & 0xFFFFFF;
            p->size = v3(rndf(1, 5), 1, 1);
        } else {
            p->transparency = (u8)(rnd() % 256);
            p->mesh = rnd() % 2 ? mesh : 0;
        }
        if (rnd() % 2) ub_world_note(w, i, UB_JN_PART); /* (half the time the replicator has to notice by itself) */
    } else if (op < 99) {
        w->lighting.clock_time = rndf(0, 24);
    } else {
        f32 v[9] = { 0, 0, 0, 1, 0, 0, 0, 1, rndf(0, 1) };
        u32 tri[3] = { 0, 1, 2 }, pv = 3, pt = 1;
        ub_world_add_mesh(w, v, 3, tri, 1, &pv, &pt, 1, 0);
    }
}

static void test_replication(void)
{
    rig g;
    /* the demo place, joined over a perfect link */
    ub_demo_place(&g.sw);
    i32 storage = ub_world_add(&g.sw, -1, UB_CLASS_OTHER, "ServerStorage", "ServerStorage");
    ub_world_add(&g.sw, storage, UB_CLASS_FOLDER, NULL, "Secret");
    f32 bv[24];
    for (int k = 0; k < 8; ++k) {
        bv[k * 3] = (k & 1) ? 0.5f : -0.5f;
        bv[k * 3 + 1] = (k & 2) ? 0.5f : -0.5f;
        bv[k * 3 + 2] = (k & 4) ? 0.5f : -0.5f;
    }
    u32 btri[36] = { 0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6, 0, 1, 5, 0, 5, 4, 2, 6, 7, 2, 7, 3, 0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5 };
    u32 bpv = 8, bpt = 12;
    u16 mesh = ub_world_add_mesh(&g.sw, bv, 8, btri, 12, &bpv, &bpt, 1, 0);
    CHECK(mesh != 0);
    rig_open(&g, NULL);
    rig_step(&g);
    rig_settle(&g);
    CHECK(ub_repl_client_joined(g.rc) && g.bad_msgs == 0);
    compare_worlds(&g);
    ub_repl_stats st;
    ub_repl_server_stats(g.rs, 0, &st);
    u64 total = 0;
    for (int t = 0; t <= UB_REPL_LAST; ++t) total += st.bytes[t];
    printf("  the demo place joined: %d instances, %u KB\n", g.cw.count, (u32)(total / 1024));
    /* the server's secrets stay there */
    i32 secret = -1;
    for (i32 i = 0; i < g.cw.count; ++i)
        if (ub_world_alive(&g.cw, i) && (!strcmp(ub_world_str(&g.cw, g.cw.inst[i].name), "Secret") ||
                                          !strcmp(ub_world_str(&g.cw, g.cw.inst[i].class_name), "ServerScriptService")))
            secret = i;
    CHECK(secret < 0);
    /* parts moving: unreliable while they move, reliable where they stop */
    ub_repl_server_stats(g.rs, 0, &st);
    u64 before = st.bytes[12] + st.bytes[11]; /* (motion, rest) */
    i32 moved = 0;
    for (int tick = 0; tick < 40; ++tick) {
        moved = 0;
        for (i32 pi = 0; pi < g.sw.part_count && moved < 20; ++pi) {
            if (g.sw.part_inst[pi] < 0 || !ub_repl_visible(&g.sw, g.sw.part_inst[pi])) continue;
            g.sw.parts[pi].pos.y += 0.05f;
            moved++;
        }
        rig_step(&g);
    }
    ub_repl_server_stats(g.rs, 0, &st);
    printf("  20 parts moving, 2 s: %u bytes a second\n", (u32)((st.bytes[12] + st.bytes[11] - before) / 2));
    rig_settle(&g);
    compare_worlds(&g);
    rig_close(&g);

    /* fuzz: random changes, a lossy, reordering, duplicating link */
    ub_link_sim bad = { 30, 80, 150, 50, 0, 77 };
    int rounds = getenv("UB_FUZZ_ROUNDS") ? atoi(getenv("UB_FUZZ_ROUNDS")) : 3;
    for (int round = 0; round < rounds; ++round) {
        ub_world_init(&g.sw);
        if (round % 3 == 2) ub_demo_place(&g.sw);
        i32 ws = ub_world_workspace(&g.sw);
        ub_world_add(&g.sw, -1, UB_CLASS_OTHER, "Lighting", "Lighting");
        storage = ub_world_add(&g.sw, -1, UB_CLASS_OTHER, "ServerStorage", "ServerStorage");
        ub_world_add(&g.sw, -1, UB_CLASS_OTHER, "ServerScriptService", "ServerScriptService");
        for (int k = 0; k < 20; ++k) ub_world_add(&g.sw, k % 3 ? ws : storage, UB_CLASS_MODEL, NULL, "M");
        mesh = ub_world_add_mesh(&g.sw, bv, 8, btri, 12, &bpv, &bpt, 1, 0);
        g_rng = 1234567u + (u32)round;
        rig_open(&g, round == 0 ? NULL : &bad);
        int steps = round % 3 == 2 ? 200 : 400;
        for (int t = 0; t < steps; ++t) {
            for (int k = 0; k < 12; ++k) mutate(&g, storage, mesh);
            rig_step(&g);
        }
        rig_settle(&g);
        printf("  fuzz %d (%s link): %d server instances, %d on the client\n", round, round == 0 ? "perfect" : "bad",
               g.sw.count, g.cw.count);
        CHECK(g.bad_msgs == 0);
        compare_worlds(&g);
        rig_close(&g);
    }
}

void test_net(void)
{
    test_buffers();
    test_connection();
    test_replication();
}
