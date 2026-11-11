/* Uniblox - one end of a connection over datagrams (a ub_link today, UDP
 * in milestone 10).
 *
 * Two kinds of message:
 * - reliable: arrive once, in the order sent, whatever their size. They
 *   go into a byte stream cut into segments; a segment is sent again until
 *   a packet carrying it is acknowledged.
 * - unreliable: sent once, as they come; lost ones stay lost (positions:
 *   the next one will do). At most UB_CONN_MAX_UNRELIABLE bytes each.
 *
 * Packets: a 16-bit sequence number, the last one received and a bitmask
 * of the 32 before it (each packet acknowledges up to 33), how much of
 * the stream has arrived in order (for long bursts), then segments
 * and messages. Duplicated packets are dropped. Segments are sent again
 * after twice the round trip (30 ms to 1 s). */
#ifndef UB_CONN_H
#define UB_CONN_H

#include "core/ub_types.h"

#define UB_CONN_MAX_UNRELIABLE 1000

typedef struct ub_conn ub_conn;

/* bytes_per_s: what it may send, 0 = no limit */
ub_conn *ub_conn_new(u32 bytes_per_s);
void ub_conn_free(ub_conn *c);
void ub_conn_set_rate(ub_conn *c, u32 bytes_per_s);

/* Queues a message; false if out of memory or (unreliable) too big. */
bool ub_conn_send(ub_conn *c, bool reliable, const void *msg, u32 len);
/* What to send now (resends, new data, acknowledgements): calls out()
 * for each packet. */
void ub_conn_flush(ub_conn *c, u64 now_us, void (*out)(void *ctx, const u8 *pkt, u32 len), void *ctx);
/* A packet that arrived. */
void ub_conn_receive(ub_conn *c, const void *pkt, u32 len, u64 now_us);
/* The next message that arrived (reliable ones in order first); valid
 * until the next ub_conn_next or ub_conn_receive. False when there are
 * none. */
bool ub_conn_next(ub_conn *c, const u8 **msg, u32 *len, bool *reliable);
/* Reliable bytes sent but not yet acknowledged, or not yet sent. */
u32 ub_conn_backlog(const ub_conn *c);

typedef struct {
    u32 rtt_ms;                 /* smoothed round trip */
    u64 bytes_out, bytes_in;    /* packets' bytes */
    u32 packets_out, packets_in, packets_dup;
    u32 segments_resent;
    u64 reliable_out, reliable_in;   /* message bytes */
    u64 unreliable_out, unreliable_in;
} ub_conn_stats;
void ub_conn_get_stats(const ub_conn *c, ub_conn_stats *out);

#endif
