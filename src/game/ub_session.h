/* Uniblox - a networked game in one process (milestone 9D): the server's
 * game and the client's, joined by a loopback link (net/ub_link.h) that
 * can act like a slow or lossy one.
 *
 * Each frame the client's game runs for the frame's time (the app draws
 * it and gives it the input); the server's runs at a fixed 60 a second,
 * and replicates 20 times a second. Until the client has its first copy
 * of the world there is no client game (ub_session_client is NULL).
 *
 * The server's messages to the client go out after the replication of
 * the same tick, so they never name an instance the client has not had. */
#ifndef UB_SESSION_H
#define UB_SESSION_H

#include "game/ub_game.h"
#include "net/ub_link.h"

#define UB_SERVER_HZ 60

typedef struct {
    bool joined;
    u32 rtt_ms;
    u32 down_bps, up_bps;       /* bytes a second the client got / sent (packets), over the last second */
    u64 down_bytes, up_bytes;   /* in all */
    u32 lost, sent;             /* the link's packets, both ways */
    u32 resent;                 /* reliable segments sent again, both ways */
    u32 reports_rejected;       /* the server's speed check */
    u32 server_ticks;
    ub_repl_stats repl;
} ub_session_stats;

typedef struct ub_session ub_session;

/* Starts the server's game in `w` (taken over, as ub_game_start), and
 * connects the client. sim: the link (NULL: perfect). NULL if out of
 * memory. */
ub_session *ub_session_new(ub_world *w, bool run_scripts, const ub_link_sim *sim);
void ub_session_free(ub_session *s);

/* One frame: the client's game runs for dt with this input; the server
 * runs the ticks owed. */
void ub_session_update(ub_session *s, f32 dt, vec3 move, bool jump);

ub_game *ub_session_client(ub_session *s);   /* NULL until joined */
ub_game *ub_session_server(ub_session *s);
void ub_session_set_sim(ub_session *s, const ub_link_sim *sim);
void ub_session_get_stats(const ub_session *s, ub_session_stats *out);

#endif
