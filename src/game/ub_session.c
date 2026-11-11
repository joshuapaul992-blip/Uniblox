/* Uniblox - a networked game in one process (see ub_session.h). */
#include "game/ub_session.h"
#include "core/ub_log.h"
#include "net/ub_conn.h"
#include "net/ub_netbuf.h"
#include "net/ub_repl.h"
#include "platform/platform.h"
#include "crt/ub_crt.h"

#define SIDE_SERVER 0
#define SIDE_CLIENT 1
#define TICKS_PER_REPL (UB_SERVER_HZ / 20)

struct ub_session {
    ub_game server, client;
    bool client_ok, scripts;
    ub_link *link;
    ub_conn *sc, *cc;          /* the server's end, the client's */
    ub_repl_server *rs;
    int rclient;
    ub_repl_client *rc;
    ub_world cworld;           /* the client's copy until its game takes it */
    ub_game_link slink, clink;
    ub_wbuf held;              /* the server's messages, held for the next replication */
    bool hello_sent;
    u64 now_us;
    f32 accum;
    u32 ticks;
    /* traffic: the last second's */
    u64 win_start_us, win_down0, win_up0;
    u32 down_bps, up_bps;
};

/* ---------------------------------------------------------- the links */

static void server_send(void *ctx, bool reliable, const u8 *msg, u32 len)
{
    ub_session *s = (ub_session *)ctx;
    if (!reliable) {
        ub_conn_send(s->sc, false, msg, len);
        return;
    }
    ub_w_varu(&s->held, len);
    ub_w_bytes(&s->held, msg, len);
}

static u32 server_to_net(void *ctx, i32 inst)
{
    ub_session *s = (ub_session *)ctx;
    const ub_world *w = &s->server.world;
    return inst >= 0 && ub_world_alive(w, inst) && ub_repl_visible(w, inst) ? (u32)inst : UB_NET_NONE;
}

static i32 server_to_local(void *ctx, u32 net)
{
    ub_session *s = (ub_session *)ctx;
    const ub_world *w = &s->server.world;
    return net < (u32)w->count && ub_world_alive(w, (i32)net) && ub_repl_visible(w, (i32)net) ? (i32)net : -1;
}

static void client_send(void *ctx, bool reliable, const u8 *msg, u32 len)
{
    ub_session *s = (ub_session *)ctx;
    ub_conn_send(s->cc, reliable, msg, len);
}

static u32 client_to_net(void *ctx, i32 inst)
{
    ub_session *s = (ub_session *)ctx;
    i32 n = ub_repl_client_net(s->rc, inst);
    return n >= 0 ? (u32)n : UB_NET_NONE;
}

static i32 client_to_local(void *ctx, u32 net)
{
    ub_session *s = (ub_session *)ctx;
    return net == UB_NET_NONE ? -1 : ub_repl_client_local(s->rc, net);
}

/* ---------------------------------------------------- replication hooks */

static void hook_destroy(void *ctx, i32 root)
{
    ub_session *s = (ub_session *)ctx;
    ub_game_destroy_from_server(&s->client, root);
}

static void hook_part(void *ctx, i32 inst, int what, u32 tick)
{
    ub_session *s = (ub_session *)ctx;
    ub_game_part_from_server(&s->client, inst, what, tick);
}

/* ------------------------------------------------------------ packets */

typedef struct {
    ub_session *s;
    int from;
} out_ctx;

static void to_link(void *ctx, const u8 *pkt, u32 len)
{
    out_ctx *o = (out_ctx *)ctx;
    ub_link_send(o->s->link, o->from, pkt, len, o->s->now_us);
}

static void flush(ub_session *s)
{
    out_ctx os = { s, SIDE_SERVER }, oc = { s, SIDE_CLIENT };
    ub_conn_flush(s->sc, s->now_us, to_link, &os);
    ub_conn_flush(s->cc, s->now_us, to_link, &oc);
}

static void deliver(ub_session *s)
{
    static u8 pkt[UB_LINK_MTU];
    int len;
    while ((len = ub_link_recv(s->link, SIDE_CLIENT, pkt, sizeof pkt, s->now_us)) >= 0)
        ub_conn_receive(s->cc, pkt, (u32)len, s->now_us);
    while ((len = ub_link_recv(s->link, SIDE_SERVER, pkt, sizeof pkt, s->now_us)) >= 0)
        ub_conn_receive(s->sc, pkt, (u32)len, s->now_us);
}

/* -------------------------------------------------------------- sides */

static void server_receive(ub_session *s)
{
    const u8 *m;
    u32 len;
    bool rel;
    while (ub_conn_next(s->sc, &m, &len, &rel)) ub_game_message(&s->server, m, len);
}

static void server_tick(ub_session *s)
{
    ub_game_update(&s->server, 1.0f / UB_SERVER_HZ, v3(0, 0, 0), false);
    if (++s->ticks % TICKS_PER_REPL) return;
    ub_repl_server_tick(s->rs);
    if (!s->hello_sent) { /* who the player is: after the first copy */
        u8 buf[64];
        u32 n = ub_game_hello(&s->server, buf, sizeof buf);
        if (n) ub_conn_send(s->sc, true, buf, n);
        s->hello_sent = true;
    }
    /* then what the server's game said since the last replication */
    ub_rbuf r;
    ub_rbuf_init(&r, s->held.p, s->held.len);
    while (ub_r_left(&r) > 0) {
        u32 n = ub_r_varu(&r);
        const u8 *msg = ub_r_span(&r, n);
        if (!msg) break;
        ub_conn_send(s->sc, true, msg, n);
    }
    ub_wbuf_clear(&s->held);
}

static void client_join(ub_session *s, const u8 *msg, u32 len)
{
    ub_rbuf r;
    ub_rbuf_init(&r, msg, len);
    ub_r_u8(&r);
    i32 ids[UB_HELLO_IDS];
    for (int i = 0; i < UB_HELLO_IDS; ++i) {
        i32 net = ub_r_vari(&r);
        ids[i] = net >= 0 ? ub_repl_client_local(s->rc, (u32)net) : -1;
    }
    if (r.err || s->client_ok || !ub_repl_client_joined(s->rc)) return;
    ub_game_start_client(&s->client, &s->cworld, ids, s->scripts, &s->clink);
    ub_repl_client_set_world(s->rc, &s->client.world);
    ub_repl_hooks h = { s, hook_destroy, hook_part };
    ub_repl_client_hooks(s->rc, &h);
    s->client_ok = true;
}

static void client_receive(ub_session *s)
{
    ub_world *w = s->client_ok ? &s->client.world : &s->cworld;
    if (w->journal) ub_world_journal_clear(w); /* (from here on: what replication changes) */
    const u8 *m;
    u32 len;
    bool rel;
    bool any = false;
    while (ub_conn_next(s->cc, &m, &len, &rel)) {
        if (!len) continue;
        if (m[0] <= UB_REPL_LAST) {
            if (!ub_repl_client_apply(s->rc, m, len)) ub_debug("client: a replication message did not apply (type %u)", m[0]);
            any = true;
        } else if (m[0] == UB_MSG_HELLO) {
            if (any && s->client_ok) ub_game_after_replication(&s->client);
            client_join(s, m, len);
            any = false;
        } else if (s->client_ok) {
            if (any) ub_game_after_replication(&s->client); /* (a message refers to what came before it) */
            any = false;
            ub_game_message(&s->client, m, len);
        }
    }
    if (any && s->client_ok) ub_game_after_replication(&s->client);
}

/* --------------------------------------------------------------- api */

ub_session *ub_session_new(ub_world *w, bool run_scripts, const ub_link_sim *sim)
{
    ub_session *s = (ub_session *)plat_alloc(sizeof *s);
    if (!s) return NULL;
    memset(s, 0, sizeof *s);
    s->scripts = run_scripts;
    s->link = ub_link_new(sim);
    u32 rate = sim && sim->bytes_per_s ? sim->bytes_per_s : 0;
    s->sc = ub_conn_new(rate);
    s->cc = ub_conn_new(rate);
    ub_wbuf_init(&s->held, NULL, 0);
    ub_world_init(&s->cworld);
    s->rc = ub_repl_client_new(&s->cworld);
    if (!s->link || !s->sc || !s->cc || !s->rc || !ub_world_journal_start(&s->cworld)) {
        ub_session_free(s);
        return NULL;
    }
    s->slink = (ub_game_link){ s, server_send, server_to_net, server_to_local };
    s->clink = (ub_game_link){ s, client_send, client_to_net, client_to_local };
    ub_game_start_server(&s->server, w, run_scripts, &s->slink);
    s->rs = ub_repl_server_new(&s->server.world);
    s->rclient = s->rs ? ub_repl_server_add(s->rs, s->sc) : -1;
    if (s->rclient < 0) {
        ub_session_free(s);
        return NULL;
    }
    ub_repl_server_set_owned(s->rs, s->rclient, s->server.char_model);
    ub_info("session: server started, client connecting");
    return s;
}

void ub_session_free(ub_session *s)
{
    if (!s) return;
    if (s->client_ok) ub_game_stop(&s->client);
    ub_world_free(&s->cworld);
    ub_repl_client_free(s->rc);
    ub_repl_server_free(s->rs);
    if (s->server.world.count || s->server.phys_ok || s->server.scripts) ub_game_stop(&s->server);
    ub_conn_free(s->sc);
    ub_conn_free(s->cc);
    ub_link_free(s->link);
    ub_wbuf_free(&s->held);
    plat_free(s);
}

void ub_session_update(ub_session *s, f32 dt, vec3 move, bool jump)
{
    if (dt > 0.25f) dt = 0.25f;
    s->now_us += (u64)(dt * 1.0e6f);
    deliver(s);
    server_receive(s);
    s->accum += dt;
    int n = 0;
    while (s->accum >= 1.0f / UB_SERVER_HZ && n < 8) {
        server_tick(s);
        s->accum -= 1.0f / UB_SERVER_HZ;
        n++;
    }
    if (n == 8) s->accum = 0; /* too slow to keep up: the server slows down */
    flush(s);
    deliver(s);
    client_receive(s);
    if (s->client_ok) ub_game_update(&s->client, dt, move, jump);
    flush(s);
    /* traffic over the last second */
    if (s->now_us - s->win_start_us >= 1000000u) {
        ub_conn_stats cs, ss;
        ub_conn_get_stats(s->cc, &cs);
        ub_conn_get_stats(s->sc, &ss);
        f64 secs = (f64)(s->now_us - s->win_start_us) / 1.0e6;
        s->down_bps = (u32)((f64)(cs.bytes_in - s->win_down0) / secs);
        s->up_bps = (u32)((f64)(cs.bytes_out - s->win_up0) / secs);
        s->win_down0 = cs.bytes_in;
        s->win_up0 = cs.bytes_out;
        s->win_start_us = s->now_us;
        (void)ss;
    }
}

ub_game *ub_session_client(ub_session *s) { return s && s->client_ok ? &s->client : NULL; }
ub_game *ub_session_server(ub_session *s) { return s ? &s->server : NULL; }

void ub_session_set_sim(ub_session *s, const ub_link_sim *sim)
{
    ub_link_set_sim(s->link, sim);
    u32 rate = sim && sim->bytes_per_s ? sim->bytes_per_s : 0;
    ub_conn_set_rate(s->sc, rate);
    ub_conn_set_rate(s->cc, rate);
}

void ub_session_get_stats(const ub_session *s, ub_session_stats *out)
{
    memset(out, 0, sizeof *out);
    out->joined = s->client_ok;
    ub_conn_stats cs, ss;
    ub_conn_get_stats(s->cc, &cs);
    ub_conn_get_stats(s->sc, &ss);
    out->rtt_ms = cs.rtt_ms;
    out->down_bps = s->down_bps;
    out->up_bps = s->up_bps;
    out->down_bytes = cs.bytes_in;
    out->up_bytes = cs.bytes_out;
    out->resent = cs.segments_resent + ss.segments_resent;
    ub_link_stats a, b;
    ub_link_get_stats(s->link, SIDE_SERVER, &a);
    ub_link_get_stats(s->link, SIDE_CLIENT, &b);
    out->lost = a.lost + b.lost;
    out->sent = a.sent + b.sent;
    out->reports_rejected = s->server.report.rejected;
    out->server_ticks = s->ticks;
    ub_repl_server_stats(s->rs, s->rclient, &out->repl);
}
