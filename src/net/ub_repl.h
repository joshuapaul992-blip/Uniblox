/* Uniblox - replication (milestone 9D): the server's world, as each client
 * may see it, kept up to date on the client.
 *
 * The server's replicator reads the world's journal (ub_world.h) every
 * tick and sends each client what changed: instances made, destroyed,
 * moved, renamed; properties, scripts' code, parts' looks (reliably) and
 * parts' movement (unreliably while they move, then reliably where they
 * come to rest); meshes, lighting and the world's settings. A joining
 * client first gets everything it may see.
 *
 * What a client may not see: ServerScriptService, ServerStorage and
 * CoreGui (and all inside them), instances with no parent, and server
 * Scripts' code (the Script itself is sent, without it).
 *
 * An instance's network id is its index on the server; the client maps it
 * to its own. Messages are the connection's: a type byte (1 to
 * UB_REPL_LAST), then the fields. */
#ifndef UB_REPL_H
#define UB_REPL_H

#include "net/ub_conn.h"
#include "world/ub_world.h"

#define UB_REPL_LAST 31 /* message types above are not replication's */

typedef struct ub_repl_server ub_repl_server;

/* Starts the world's journal (the world must outlive the replicator). */
ub_repl_server *ub_repl_server_new(ub_world *w);
void ub_repl_server_free(ub_repl_server *s);
/* A client on this connection (it gets everything at the next tick);
 * returns its number, or -1. */
int ub_repl_server_add(ub_repl_server *s, ub_conn *conn);
void ub_repl_server_remove(ub_repl_server *s, int client);
/* Sends what changed since the last tick to every client. */
void ub_repl_server_tick(ub_repl_server *s);
/* May a client see this instance? */
bool ub_repl_visible(const ub_world *w, i32 inst);

typedef struct {
    u32 ticks;
    u64 bytes[UB_REPL_LAST + 1];   /* message bytes sent, by type */
    u32 messages[UB_REPL_LAST + 1];
} ub_repl_stats;
void ub_repl_server_stats(const ub_repl_server *s, int client, ub_repl_stats *out);

typedef struct ub_repl_client ub_repl_client;

/* w: an empty world (ub_world_init) that becomes the copy. */
ub_repl_client *ub_repl_client_new(ub_world *w);
void ub_repl_client_free(ub_repl_client *c);
/* A replication message; false if it is malformed (it changes nothing
 * further). */
bool ub_repl_client_apply(ub_repl_client *c, const u8 *msg, u32 len);
/* Has the first full copy arrived? */
bool ub_repl_client_joined(const ub_repl_client *c);
/* The client's instance for a network id (-1 none), and the other way. */
i32 ub_repl_client_local(const ub_repl_client *c, u32 net);
i32 ub_repl_client_net(const ub_repl_client *c, i32 local);

#endif
