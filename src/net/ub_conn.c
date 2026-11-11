/* Uniblox - a connection's reliable stream and unreliable messages over
 * datagrams (see ub_conn.h). */
#include "net/ub_conn.h"
#include "net/ub_link.h"
#include "net/ub_netbuf.h"
#include "platform/platform.h"
#include "crt/ub_crt.h"

#define SEG_MAX 1000           /* stream bytes in a segment */
#define WINDOW (256u * 1024u)  /* stream bytes in flight, at most */
#define RECORDS 1024           /* packets remembered until acknowledged */
#define REC_SEGS 8
#define T_SEGMENT 1
#define T_UNRELIABLE 2
#define PENDING_MAX (4u * 1024u * 1024u) /* out-of-order stream bytes kept */

typedef struct {
    u32 off;
    u16 len;
    bool acked, sent;
    u64 sent_at;
} seg;

typedef struct {
    bool used, acked;
    u16 seq;
    u64 sent_at;
    u8 nseg;
    u32 seg_off[REC_SEGS];
} record;

typedef struct {
    u32 off, len;
    u8 *data;
} pending;

struct ub_conn {
    /* sending: the stream from out_base on */
    u8 *out;
    u32 out_len, out_cap;
    u32 out_base;          /* stream offset of out[0] */
    u32 next_new;          /* first stream byte never put in a segment */
    seg *segs;             /* in flight, by offset */
    int nsegs, segs_cap;
    record rec[RECORDS];
    u16 seq;               /* the next packet's */
    ub_wbuf unrel;         /* unreliable messages to send: length, bytes... */
    /* receiving */
    u16 remote_seq;
    u32 recv_bits;         /* bit i: remote_seq - 1 - i arrived */
    bool any_received, need_ack;
    u64 ack_owed_since;    /* when data first arrived unacknowledged */
    u32 recv_off;          /* the next stream byte expected */
    u8 *in;                /* stream bytes arrived in order, not yet read as messages */
    u32 in_len, in_cap, in_read;
    pending *pend;
    int npend, pend_cap;
    u32 pend_bytes;
    ub_wbuf unrel_in;      /* unreliable messages arrived: length, bytes... */
    u32 unrel_read;
    /* timing */
    f32 srtt_ms;
    u32 rate;
    f64 tokens;
    u64 last_flush;
    ub_conn_stats st;
};

static bool seq_newer(u16 a, u16 b) { return (u16)(a - b) != 0 && (u16)(a - b) < 0x8000; }

static bool grow(void **p, int *cap, int need, size_t elem)
{
    if (need <= *cap) return true;
    int nc = *cap ? *cap * 2 : 32;
    while (nc < need) nc *= 2;
    void *n = plat_alloc((size_t)nc * elem);
    if (!n) return false;
    if (*p) memcpy(n, *p, (size_t)*cap * elem);
    plat_free(*p);
    *p = n;
    *cap = nc;
    return true;
}

static bool grow_bytes(u8 **p, u32 *cap, u32 need)
{
    if (need <= *cap) return true;
    u32 nc = *cap ? *cap : 4096;
    while (nc < need) nc *= 2;
    u8 *n = (u8 *)plat_alloc(nc);
    if (!n) return false;
    if (*p) memcpy(n, *p, *cap);
    plat_free(*p);
    *p = n;
    *cap = nc;
    return true;
}

ub_conn *ub_conn_new(u32 bytes_per_s)
{
    ub_conn *c = (ub_conn *)plat_alloc(sizeof *c);
    if (!c) return NULL;
    memset(c, 0, sizeof *c);
    ub_wbuf_init(&c->unrel, NULL, 0);
    ub_wbuf_init(&c->unrel_in, NULL, 0);
    c->srtt_ms = 100;
    c->rate = bytes_per_s;
    return c;
}

void ub_conn_free(ub_conn *c)
{
    if (!c) return;
    plat_free(c->out);
    plat_free(c->segs);
    plat_free(c->in);
    for (int i = 0; i < c->npend; ++i) plat_free(c->pend[i].data);
    plat_free(c->pend);
    ub_wbuf_free(&c->unrel);
    ub_wbuf_free(&c->unrel_in);
    plat_free(c);
}

void ub_conn_set_rate(ub_conn *c, u32 bytes_per_s) { c->rate = bytes_per_s; }

bool ub_conn_send(ub_conn *c, bool reliable, const void *msg, u32 len)
{
    if (!reliable) {
        if (len > UB_CONN_MAX_UNRELIABLE) return false;
        ub_w_varu(&c->unrel, len);
        ub_w_bytes(&c->unrel, msg, len);
        c->st.unreliable_out += len;
        return !c->unrel.err;
    }
    u8 hdr[5];
    ub_wbuf h;
    ub_wbuf_init(&h, hdr, sizeof hdr);
    ub_w_varu(&h, len);
    if (!grow_bytes(&c->out, &c->out_cap, c->out_len + h.len + len)) return false;
    memcpy(c->out + c->out_len, hdr, h.len);
    if (len) memcpy(c->out + c->out_len + h.len, msg, len);
    c->out_len += h.len + len;
    c->st.reliable_out += len;
    return true;
}

u32 ub_conn_backlog(const ub_conn *c)
{
    u32 first = c->next_new;
    for (int i = 0; i < c->nsegs; ++i)
        if (!c->segs[i].acked && c->segs[i].off < first) first = c->segs[i].off;
    return c->out_base + c->out_len - first;
}

/* ------------------------------------------------------------ sending */

static void acked_segment(ub_conn *c, u32 off)
{
    for (int i = 0; i < c->nsegs; ++i)
        if (c->segs[i].off == off) {
            c->segs[i].acked = true;
            return;
        }
}

/* drops acknowledged segments at the front, and the stream bytes before them */
static void trim(ub_conn *c)
{
    int k = 0;
    while (k < c->nsegs && c->segs[k].acked) ++k;
    if (k) {
        memmove(c->segs, c->segs + k, sizeof(seg) * (size_t)(c->nsegs - k));
        c->nsegs -= k;
    }
    u32 base = c->nsegs ? c->segs[0].off : c->next_new;
    u32 drop = base - c->out_base;
    if (drop && drop <= c->out_len) {
        memmove(c->out, c->out + drop, c->out_len - drop);
        c->out_len -= drop;
        c->out_base = base;
    }
}

typedef struct {
    u8 buf[UB_LINK_MTU];
    ub_wbuf w;
    record *r;
} packet;

static void begin_packet(ub_conn *c, packet *p, u64 now)
{
    ub_wbuf_init(&p->w, p->buf, sizeof p->buf);
    ub_w_u16(&p->w, c->seq);
    ub_w_u8(&p->w, c->any_received ? 1 : 0); /* the acknowledgement below means something */
    ub_w_u16(&p->w, c->remote_seq);
    ub_w_u32(&p->w, c->recv_bits);
    ub_w_u32(&p->w, c->recv_off); /* and the stream, up to here (covers what the packets' acknowledgements miss) */
    p->r = &c->rec[c->seq % RECORDS];
    memset(p->r, 0, sizeof *p->r);
    p->r->used = true;
    p->r->seq = c->seq;
    p->r->sent_at = now;
}

static void end_packet(ub_conn *c, packet *p, void (*out)(void *, const u8 *, u32), void *ctx)
{
    out(ctx, p->buf, p->w.len);
    c->st.packets_out++;
    c->st.bytes_out += p->w.len;
    c->tokens -= (f64)p->w.len;
    c->seq++;
    c->need_ack = false;
}

static bool add_segment(ub_conn *c, packet *p, seg *s, u64 now)
{
    if (p->r->nseg >= REC_SEGS || p->w.len + 1 + 4 + 2 + s->len > sizeof p->buf) return false;
    ub_w_u8(&p->w, T_SEGMENT);
    ub_w_u32(&p->w, s->off);
    ub_w_varu(&p->w, s->len);
    ub_w_bytes(&p->w, c->out + (s->off - c->out_base), s->len);
    if (s->sent) c->st.segments_resent++;
    s->sent = true;
    s->sent_at = now;
    p->r->seg_off[p->r->nseg++] = s->off;
    return true;
}

#define ACK_DELAY_US 25000u

void ub_conn_flush(ub_conn *c, u64 now_us, void (*out)(void *ctx, const u8 *pkt, u32 len), void *ctx)
{
    /* the bandwidth budget */
    if (c->rate) {
        f64 dt = c->last_flush ? (f64)(now_us - c->last_flush) * 1e-6 : 0;
        c->tokens += dt * c->rate;
        f64 burst = c->rate / 8.0 + UB_LINK_MTU;
        if (c->tokens > burst) c->tokens = burst;
    } else {
        c->tokens = 1e18;
    }
    c->last_flush = now_us;
    trim(c);
    /* new segments, while the window allows */
    u32 end = c->out_base + c->out_len;
    while (c->next_new < end && c->next_new - (c->nsegs ? c->segs[0].off : c->next_new) < WINDOW) {
        if (!grow((void **)&c->segs, &c->segs_cap, c->nsegs + 1, sizeof(seg))) break;
        seg *s = &c->segs[c->nsegs++];
        s->off = c->next_new;
        s->len = (u16)(end - c->next_new < SEG_MAX ? end - c->next_new : SEG_MAX);
        s->acked = s->sent = false;
        s->sent_at = 0;
        c->next_new += s->len;
    }
    u64 rto = (u64)(c->srtt_ms * 2.0f + 20.0f) * 1000u;
    if (rto < 30000) rto = 30000;
    if (rto > 1000000) rto = 1000000;

    packet p;
    bool open = false;
    int si = 0;
    u32 ur = 0; /* unreliable messages read so far */
    for (;;) {
        if (c->tokens <= 0) break;
        if (!open) {
            begin_packet(c, &p, now_us);
            open = true;
        }
        bool added = false;
        /* segments: unsent ones, and ones not acknowledged in time */
        while (si < c->nsegs) {
            seg *s = &c->segs[si];
            if (s->acked || (s->sent && now_us - s->sent_at < rto)) {
                ++si;
                continue;
            }
            if (!add_segment(c, &p, s, now_us)) break;
            added = true;
            ++si;
        }
        bool full = si < c->nsegs;
        /* unreliable messages */
        while (!full && ur < c->unrel.len) {
            ub_rbuf r;
            ub_rbuf_init(&r, c->unrel.p + ur, c->unrel.len - ur);
            u32 n = ub_r_varu(&r);
            if (p.w.len + 1 + 2 + n > sizeof p.buf) {
                full = true;
                break;
            }
            ub_w_u8(&p.w, T_UNRELIABLE);
            ub_w_varu(&p.w, n);
            ub_w_bytes(&p.w, c->unrel.p + ur + r.pos, n);
            ur += r.pos + n;
            added = true;
        }
        /* an acknowledgement alone waits a little for data to ride with */
        if (added || (c->need_ack && now_us - c->ack_owed_since >= ACK_DELAY_US)) {
            end_packet(c, &p, out, ctx);
            open = false;
        }
        if (!full) break;
    }
    /* what did not fit the budget waits (unreliable: dropped, they go stale) */
    ub_wbuf_clear(&c->unrel);
}

/* ---------------------------------------------------------- receiving */

static void take_ack(ub_conn *c, u16 ack, u32 bits, u64 now)
{
    for (int i = 0; i <= 32; ++i) {
        if (i > 0 && !(bits & (1u << (i - 1)))) continue;
        u16 s = (u16)(ack - i);
        record *r = &c->rec[s % RECORDS];
        if (!r->used || r->seq != s || r->acked) continue;
        r->acked = true;
        f32 sample = (f32)(now - r->sent_at) / 1000.0f;
        c->srtt_ms += (sample - c->srtt_ms) * 0.125f;
        for (int k = 0; k < r->nseg; ++k) acked_segment(c, r->seg_off[k]);
    }
}

static void stream_append(ub_conn *c, const u8 *d, u32 n)
{
    if (c->in_read > 0 && c->in_read == c->in_len) c->in_len = c->in_read = 0;
    if (!grow_bytes(&c->in, &c->in_cap, c->in_len + n)) return; /* (lost: never happens in practice) */
    memcpy(c->in + c->in_len, d, n);
    c->in_len += n;
    c->recv_off += n;
}

static void take_segment(ub_conn *c, u32 off, const u8 *d, u32 n)
{
    if (off + n <= c->recv_off) return; /* had it */
    if (off > c->recv_off) {
        /* early: keep it until the gap fills (once) */
        for (int i = 0; i < c->npend; ++i)
            if (c->pend[i].off == off) return;
        if (c->pend_bytes + n > PENDING_MAX || !grow((void **)&c->pend, &c->pend_cap, c->npend + 1, sizeof(pending))) return;
        u8 *copy = (u8 *)plat_alloc(n);
        if (!copy) return;
        memcpy(copy, d, n);
        pending *p = &c->pend[c->npend++];
        p->off = off;
        p->len = n;
        p->data = copy;
        c->pend_bytes += n;
        return;
    }
    stream_append(c, d + (c->recv_off - off), off + n - c->recv_off);
    /* and the early ones that now follow on */
    for (bool more = true; more;) {
        more = false;
        for (int i = 0; i < c->npend; ++i) {
            pending *p = &c->pend[i];
            if (p->off > c->recv_off) continue;
            if (p->off + p->len > c->recv_off) stream_append(c, p->data + (c->recv_off - p->off), p->off + p->len - c->recv_off);
            plat_free(p->data);
            c->pend_bytes -= p->len;
            c->pend[i] = c->pend[--c->npend];
            more = true;
            break;
        }
    }
}

void ub_conn_receive(ub_conn *c, const void *pkt, u32 len, u64 now_us)
{
    ub_rbuf r;
    ub_rbuf_init(&r, pkt, len);
    u16 seq = ub_r_u16(&r);
    u8 flags = ub_r_u8(&r);
    u16 ack = ub_r_u16(&r);
    u32 bits = ub_r_u32(&r), stream_ack = ub_r_u32(&r);
    if (r.err) return;
    /* seen it? */
    if (c->any_received) {
        if (seq == c->remote_seq) { c->st.packets_dup++; return; }
        if (seq_newer(seq, c->remote_seq)) {
            u16 d = (u16)(seq - c->remote_seq);
            c->recv_bits = d >= 32 ? (d == 32 ? 1u << 31 : 0) : (c->recv_bits << d) | (1u << (d - 1));
            c->remote_seq = seq;
        } else {
            u16 d = (u16)(c->remote_seq - seq);
            if (d > 32 || (c->recv_bits & (1u << (d - 1)))) { c->st.packets_dup++; return; }
            c->recv_bits |= 1u << (d - 1);
        }
    } else {
        c->any_received = true;
        c->remote_seq = seq;
        c->recv_bits = 0;
    }
    c->st.packets_in++;
    c->st.bytes_in += len;
    if (flags & 1) take_ack(c, ack, bits, now_us);
    for (int i = 0; i < c->nsegs; ++i)
        if (c->segs[i].off + c->segs[i].len <= stream_ack) c->segs[i].acked = true;
    bool data = false;
    while (ub_r_left(&r) > 0 && !r.err) {
        u8 type = ub_r_u8(&r);
        if (type == T_SEGMENT) {
            u32 off = ub_r_u32(&r), n = ub_r_varu(&r);
            const u8 *d = ub_r_span(&r, n);
            if (!d) break;
            take_segment(c, off, d, n);
            data = true;
        } else if (type == T_UNRELIABLE) {
            u32 n = ub_r_varu(&r);
            const u8 *d = ub_r_span(&r, n);
            if (!d) break;
            ub_w_varu(&c->unrel_in, n);
            ub_w_bytes(&c->unrel_in, d, n);
            c->st.unreliable_in += n;
            data = true;
        } else {
            break; /* not ours */
        }
    }
    if (data && !c->need_ack) {
        c->need_ack = true;
        c->ack_owed_since = now_us;
    }
}

bool ub_conn_next(ub_conn *c, const u8 **msg, u32 *len, bool *reliable)
{
    /* reliable: a whole message in the stream? */
    if (c->in_read < c->in_len) {
        ub_rbuf r;
        ub_rbuf_init(&r, c->in + c->in_read, c->in_len - c->in_read);
        u32 n = ub_r_varu(&r);
        if (!r.err && ub_r_left(&r) >= n) {
            *msg = c->in + c->in_read + r.pos;
            *len = n;
            *reliable = true;
            c->in_read += r.pos + n;
            c->st.reliable_in += n;
            return true;
        }
        /* a message still coming: move what is left to the front */
        if (c->in_read > 0) {
            memmove(c->in, c->in + c->in_read, c->in_len - c->in_read);
            c->in_len -= c->in_read;
            c->in_read = 0;
        }
    } else if (c->in_read) {
        c->in_len = c->in_read = 0;
    }
    if (c->unrel_read < c->unrel_in.len) {
        ub_rbuf r;
        ub_rbuf_init(&r, c->unrel_in.p + c->unrel_read, c->unrel_in.len - c->unrel_read);
        u32 n = ub_r_varu(&r);
        *msg = c->unrel_in.p + c->unrel_read + r.pos;
        *len = n;
        *reliable = false;
        c->unrel_read += r.pos + n;
        return true;
    }
    c->unrel_read = 0;
    ub_wbuf_clear(&c->unrel_in);
    return false;
}

void ub_conn_get_stats(const ub_conn *c, ub_conn_stats *out)
{
    *out = c->st;
    out->rtt_ms = (u32)(c->srtt_ms + 0.5f);
}
