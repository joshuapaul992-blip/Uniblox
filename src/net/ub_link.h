/* Uniblox - a datagram link between two ends in one process (loopback),
 * with a simulator for a worse network: delay, jitter (which also
 * reorders), loss, duplicates and a bandwidth cap. Milestone 10 adds the
 * same over UDP. Packets are at most UB_LINK_MTU bytes. */
#ifndef UB_LINK_H
#define UB_LINK_H

#include "core/ub_types.h"

#define UB_LINK_MTU 1200

typedef struct {
    u32 delay_ms;       /* one way */
    u32 jitter_ms;      /* up to this much more, at random (packets overtake each other) */
    u32 loss_pct;       /* packets lost, in 1/1000 */
    u32 dup_pct;        /* packets that arrive twice, in 1/1000 */
    u32 bytes_per_s;    /* each way, 0 = no limit (56k: about 5000) */
    u32 seed;
} ub_link_sim;

typedef struct ub_link ub_link;

ub_link *ub_link_new(const ub_link_sim *sim); /* sim NULL: a perfect link */
void ub_link_free(ub_link *l);
void ub_link_set_sim(ub_link *l, const ub_link_sim *sim);
/* end 0 sends to end 1 and the other way; false if it is too big or out
 * of memory (a lost packet, as far as the sender can tell) */
bool ub_link_send(ub_link *l, int from, const void *data, u32 len, u64 now_us);
/* the next packet that has arrived at end `to` by now: its length, or -1 */
int ub_link_recv(ub_link *l, int to, void *buf, u32 cap, u64 now_us);

typedef struct {
    u32 sent, delivered, lost, duplicated;
    u64 bytes;
} ub_link_stats;
void ub_link_get_stats(const ub_link *l, int from, ub_link_stats *out);

#endif
