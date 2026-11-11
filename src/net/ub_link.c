/* Uniblox - a loopback datagram link with a network simulator (ub_link.h). */
#include "net/ub_link.h"
#include "platform/platform.h"
#include "crt/ub_crt.h"

typedef struct {
    u64 at;             /* when it arrives */
    u32 order;          /* (ties keep the order sent) */
    u16 len;
    u8 data[UB_LINK_MTU];
} pkt;

typedef struct {
    pkt *q;
    int n, cap;
    u64 wire_free;      /* the bandwidth cap: when the wire is free again */
    ub_link_stats st;
} lane;

struct ub_link {
    ub_link_sim sim;
    lane lanes[2];      /* [from] */
    u32 rng, order;
};

static u32 rnd(ub_link *l)
{
    l->rng ^= l->rng << 13;
    l->rng ^= l->rng >> 17;
    l->rng ^= l->rng << 5;
    return l->rng;
}

ub_link *ub_link_new(const ub_link_sim *sim)
{
    ub_link *l = (ub_link *)plat_alloc(sizeof *l);
    if (!l) return NULL;
    memset(l, 0, sizeof *l);
    ub_link_set_sim(l, sim);
    return l;
}

void ub_link_free(ub_link *l)
{
    if (!l) return;
    for (int i = 0; i < 2; ++i) plat_free(l->lanes[i].q);
    plat_free(l);
}

void ub_link_set_sim(ub_link *l, const ub_link_sim *sim)
{
    if (sim) l->sim = *sim;
    else memset(&l->sim, 0, sizeof l->sim);
    l->rng = l->sim.seed ? l->sim.seed : 0x9E3779B9u;
}

static bool enqueue(ub_link *l, lane *ln, const void *data, u32 len, u64 at)
{
    if (ln->n == ln->cap) {
        int nc = ln->cap ? ln->cap * 2 : 64;
        pkt *nq = (pkt *)plat_alloc(sizeof(pkt) * (size_t)nc);
        if (!nq) return false;
        if (ln->n) memcpy(nq, ln->q, sizeof(pkt) * (size_t)ln->n);
        plat_free(ln->q);
        ln->q = nq;
        ln->cap = nc;
    }
    pkt *p = &ln->q[ln->n++];
    p->at = at;
    p->order = l->order++;
    p->len = (u16)len;
    memcpy(p->data, data, len);
    return true;
}

bool ub_link_send(ub_link *l, int from, const void *data, u32 len, u64 now_us)
{
    if (!l || (from != 0 && from != 1) || len > UB_LINK_MTU) return false;
    lane *ln = &l->lanes[from];
    const ub_link_sim *s = &l->sim;
    ln->st.sent++;
    ln->st.bytes += len;
    /* the wire: a packet waits for the one before it to be sent */
    u64 t = now_us;
    if (s->bytes_per_s) {
        if (ln->wire_free > t) t = ln->wire_free;
        t += (u64)(len + 28) * 1000000u / s->bytes_per_s; /* (28: IP and UDP headers) */
        ln->wire_free = t;
    }
    if (s->loss_pct && rnd(l) % 1000 < s->loss_pct) {
        ln->st.lost++;
        return true; /* gone: the sender cannot tell */
    }
    u64 at = t + (u64)s->delay_ms * 1000u + (s->jitter_ms ? (u64)(rnd(l) % (s->jitter_ms * 1000u + 1)) : 0);
    if (!enqueue(l, ln, data, len, at)) return false;
    if (s->dup_pct && rnd(l) % 1000 < s->dup_pct) {
        ln->st.duplicated++;
        enqueue(l, ln, data, len, at + (s->jitter_ms ? (u64)(rnd(l) % (s->jitter_ms * 1000u + 1)) : 1000));
    }
    return true;
}

int ub_link_recv(ub_link *l, int to, void *buf, u32 cap, u64 now_us)
{
    if (!l || (to != 0 && to != 1)) return -1;
    lane *ln = &l->lanes[1 - to];
    int best = -1;
    for (int i = 0; i < ln->n; ++i) {
        const pkt *p = &ln->q[i];
        if (p->at > now_us) continue;
        if (best < 0 || p->at < ln->q[best].at || (p->at == ln->q[best].at && p->order < ln->q[best].order)) best = i;
    }
    if (best < 0) return -1;
    pkt *p = &ln->q[best];
    int len = p->len <= cap ? p->len : -1;
    if (len >= 0) memcpy(buf, p->data, (size_t)len);
    ln->q[best] = ln->q[--ln->n];
    if (len >= 0) ln->st.delivered++;
    return len < 0 ? ub_link_recv(l, to, buf, cap, now_us) : len;
}

void ub_link_get_stats(const ub_link *l, int from, ub_link_stats *out)
{
    *out = l->lanes[from & 1].st;
}
