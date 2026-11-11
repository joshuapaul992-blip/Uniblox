/* Uniblox - host tests for the network layer (src/net). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/ub_conn.h"
#include "net/ub_link.h"
#include "net/ub_netbuf.h"
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

void test_net(void)
{
    test_buffers();
    test_connection();
}
