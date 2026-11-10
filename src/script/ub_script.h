/* Uniblox - scripting: the place's Lua scripts, sandboxed.
 *
 * One Lua 5.1 state runs every script; each script has its own globals,
 * with the Roblox-style API on top (game, workspace, script, Instance,
 * Vector3, CFrame, Color3, wait, task, ...). Scripts run as coroutines:
 * wait() and event handlers yield and resume on a schedule kept here.
 *
 * Hostile scripts are contained:
 *   - no files, OS, debug library, bytecode or loadstring;
 *   - a script that runs too long without waiting is stopped (and all of
 *     it disconnected), so `while true do end` cannot freeze the game;
 *   - all scripts together get a fixed amount of memory, and a script
 *     that uses it up is stopped; they may make only so many instances;
 *   - string functions that run in C have their own work limits;
 *   - each script's globals and library tables are its own, and
 *     metatables are locked, so one script cannot tamper with another. */
#ifndef UB_SCRIPT_H
#define UB_SCRIPT_H

#include "core/ub_types.h"

struct ub_game;

#define UB_SCRIPT_MEMORY (64u << 20)       /* bytes, all scripts together */
#define UB_SCRIPT_TIME_LIMIT_US 250000u    /* longest run without yielding */
#define UB_SCRIPT_OUTPUT_LINES 8           /* kept for the overlay */
#define UB_SCRIPT_MAX_INSTANCES 100000u    /* instances scripts may make (Instance.new, Clone) */

typedef struct {
    u32 scripts;          /* started */
    u32 stopped;          /* stopped for running too long */
    u32 errors;
    u32 threads;          /* waiting or scheduled */
    u32 memory_kb;
    u32 skipped_client;   /* LocalScripts and client scripts (not run yet) */
} ub_script_stats;

bool ub_script_init(struct ub_game *g);
void ub_script_shutdown(void);
/* Runs the place's scripts (Scripts in the Workspace and
 * ServerScriptService), then the player joins (PlayerAdded,
 * CharacterAdded). */
void ub_script_start(void);
/* Runs a piece of code as if it were a Script under `parent` (tests and
 * the console). Returns false on a compile error. */
bool ub_script_run(const char *name, const char *source, i32 parent);

/* Each frame: before physics (waits, Stepped) and after (Heartbeat,
 * tweens, Debris). */
void ub_script_begin_frame(f32 dt);
void ub_script_end_frame(f32 dt);

/* Events from the game. */
void ub_script_touched(i32 inst_a, i32 inst_b);
void ub_script_died(void);
void ub_script_respawned(void);
/* Does some script listen to this part's Touched? (Then it reports
 * touches even when not solid.) */
bool ub_script_watches_touch(i32 inst);

void ub_script_get_stats(ub_script_stats *out);
/* The last printed lines, oldest first; is_error marks errors and warnings. */
int ub_script_output(const char **lines, bool *is_error, int max);

#endif
