/******************************************************************************
** This material was prepared as an account of work sponsored by an agency   **
** of the United States Government.  Neither the United States Government    **
** nor the United States Department of Energy, nor Battelle, nor any of      **
** their employees, nor any jurisdiction or organization that has cooperated **
** in the development of these materials, makes any warranty, express or     **
** implied, or assumes any legal liability or responsibility for the accuracy,*
** completeness, or usefulness or any information, apparatus, product,       **
** software, or process disclosed, or represents that its use would not      **
** infringe privately owned rights.                                          **
**                                                                           **
** Reference herein to any specific commercial product, process, or service  **
** by trade name, trademark, manufacturer, or otherwise does not necessarily **
** constitute or imply its endorsement, recommendation, or favoring by the   **
** United States Government or any agency thereof, or Battelle Memorial      **
** Institute. The views and opinions of authors expressed herein do not      **
** necessarily state or reflect those of the United States Government or     **
** any agency thereof.                                                       **
**                                                                           **
**                      PACIFIC NORTHWEST NATIONAL LABORATORY                **
**                                  operated by                              **
**                                    BATTELLE                               **
**                                     for the                               **
**                      UNITED STATES DEPARTMENT OF ENERGY                   **
**                         under Contract DE-AC05-76RL01830                  **
**                                                                           **
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
** You may obtain a copy of the License at                                   **
**                                                                           **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
**                                                                           **
** Unless required by applicable law or agreed to in writing, software       **
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT **
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the  **
** License for the specific language governing permissions and limitations   **
******************************************************************************/

#ifndef ARTS_TRANSPORT_SOCKET_H
#define ARTS_TRANSPORT_SOCKET_H
#ifdef __cplusplus
extern "C" {
#endif
#include "arts/system/config.h"
#include <assert.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The bootstrap mesh: one connection per rank pair, opened by the higher rank,
 * whose first 8 bytes are its hello.  arts_socket_fd[peer] is this rank's
 * established connection to peer (non-blocking, close-on-exec, kept alive),
 * -1 at this rank.  Written by arts_mesh_form on the main thread; valid there
 * after it returns, and on any thread from arts_socket_armed returning true
 * until the process ends or, in a tree with a teardown, arts_socket_cleanup. */
extern int *arts_socket_fd;

#define ARTS_MESH_HELLO_MAGIC 0x4d535241u
/* The connector's first bytes on a mesh connection, host-endian like the
 * address frame (every rank of a run shares one ABI). */
struct arts_mesh_hello_s {
  uint32_t magic;
  uint32_t rank;
};
static_assert(sizeof(struct arts_mesh_hello_s) == 8u,
              "the hello is 8 bytes on the wire");

void arts_transport_set_config(struct arts_config_s *config);
/* Main thread, a process of a run of several: resolves every routing-table
 * row and learns this rank; rows that do not resolve end the rank after the
 * pass with a resolve line listing them (arts_mesh_fail). */
void arts_socket_setup(struct arts_config_s *config);
/* Slide a local multi-node run's port block (node_count ports from
 * config->port) past whatever already holds it, by probing the exact ports the
 * run is about to claim; rewrites config->port and every row's port on
 * success.  Callable only from the spawner: the ranks inherit the chosen base
 * and must not probe again.  Returns false when no free block fits below the
 * ephemeral range, leaving the configured block in place. */
bool arts_transport_select_local_port(struct arts_config_s *config);

/* The steps of a process's startup, in their order, and the phases a startup
 * failure line names: every step, and a peer's end seen before the arm. */
enum arts_mesh_phase {
  ARTS_MESH_PHASE_CONFIG,
  ARTS_MESH_PHASE_RESOLVE,
  ARTS_MESH_PHASE_LISTEN,
  ARTS_MESH_PHASE_CONNECT,
  ARTS_MESH_PHASE_FABRIC,
  ARTS_MESH_PHASE_EXCHANGE,
  ARTS_MESH_PHASE_HANGUP,
};

#define ARTS_MESH_EXIT_STATUS 75
#define ARTS_MESH_LINE_MAX 2048u
#define ARTS_MESH_LINE_RANKS 16u
#define ARTS_STARTUP_BUDGET_DEFAULT_S 20u
#define ARTS_STARTUP_BUDGET_MIN_S 10u
#define ARTS_STARTUP_BUDGET_MAX_S 86400u

/* The startup bound.  arts_startup_begin fixes the process's start T0 and
 * its deadline D = T0 + the budget, and arms the startup timer at D + 1 s,
 * which ends a process still inside the bound then with one line naming the
 * step it was in (arts_startup_expired_line) and status 75.  The bound ends
 * at arts_startup_end.  Every function here but the two line writers runs on
 * the main thread, before any runtime thread exists. */

/* Called right after arts_exit_process_init.  Reads ARTS_STARTUP_TIMEOUT_S
 * (unset: 20 s; any set value that is not ASCII decimal digits naming
 * 10-86400 seconds is refused, ARTS_ERROR) and enters the config step; where
 * the injection hooks are compiled it parses ARTS_MESH_FAULT (a malformed
 * value is refused), and in another Debug build refuses it set. */
void arts_startup_begin(void);
/* The budget in seconds and D on CLOCK_MONOTONIC, fixed by
 * arts_startup_begin. */
unsigned int arts_startup_budget_s(void);
uint64_t arts_startup_deadline_ns(void);
/* Enters `step`: the backstop's line names it from now on; then the
 * ARTS_MESH_FAULT hook acts where it names this rank and step.  The first
 * step entered at or past listen publishes to the backstop this rank's id and
 * its routing-table name, so such a step is entered only once both are
 * final. */
void arts_startup_step(enum arts_mesh_phase step);
/* One of the run-wide checks every process of a run makes before any rank
 * starts.  Where the injection hooks are compiled, a set ARTS_MESH_FAULT is
 * refused on a run of one rank, under launcher=lsf (no variable names the
 * rank) and when it names a rank outside the run. */
void arts_startup_check_run(const struct arts_config_s *config);
/* Called in a process that is to be a rank, once its configuration is loaded
 * and the run-wide checks passed.  Where the injection hooks are compiled, a
 * set ARTS_MESH_FAULT acts if it names this rank's config step.  A run of one
 * rank then ends the bound. */
void arts_startup_config_done(const struct arts_config_s *config);
/* Ends the bound; idempotent. */
void arts_startup_end(void);

/* Writes one failure line into buf: at most min(cap - 1, ARTS_MESH_LINE_MAX)
 * bytes ending in a newline, then a NUL, and returns its length.  rank < 0
 * prints "-".  missing (n entries, NULL for none) names the missing ranks,
 * of which the lowest ARTS_MESH_LINE_RANKS are listed with their names from
 * table; an entry that would push the line past its bound is dropped from
 * both lists, and every rank not listed is counted in more.
 * Async-signal-safe. */
size_t arts_mesh_format_line(char *buf, size_t cap, int rank, const char *host,
                             enum arts_mesh_phase phase, const bool *missing,
                             unsigned int n,
                             const struct arts_config_table_s *table);
/* The startup timer's line: the step the process is in, this rank and its
 * table name from the listen step on, no rank and the node name uname gives
 * before it.  Async-signal-safe. */
size_t arts_startup_expired_line(char *buf, size_t cap);
/* A startup this rank cannot complete: ends the startup bound, closes every
 * mesh socket, so its peers see their connections end, and, when the end is
 * its own, ends the process with status 75.  Once this process is a rank
 * (arts_exit_rank_known) it takes the rank's end, and only a taker writes the
 * line; before, it writes the line unless this process's end began
 * (arts_exit_began), and ends the process, or, when the end began, joins it
 * as a late entry.  missing as for arts_mesh_format_line, over the routing
 * table. */
#ifdef __cplusplus
[[noreturn]]
#else
_Noreturn
#endif
void arts_mesh_fail(enum arts_mesh_phase phase, const bool *missing);

/* Main thread, after arts_socket_setup and once this process is a rank:
 * forms the mesh under the startup deadline, entering the listen and connect
 * steps.  It reserves the descriptors the mesh needs, listens on this rank's
 * port, connects to every lower rank (an error retried 100 ms later, a
 * connect still in progress after 1 s re-issued) and takes every higher
 * rank's hello.  It reads no byte past a hello, and returns only with every
 * connection established; otherwise it ends the rank (arts_mesh_fail). */
void arts_mesh_form(struct arts_config_s *config);

/* Turns the mesh into payload-free liveness sentinels once the address
 * exchange has finished: every connection stays open (its lifetime doubles as
 * the peer's: an orderly close and a process's death both surface as an end
 * of file or a hangup at the other end).  Its last act arms the mesh
 * (arts_socket_armed). */
void arts_socket_sentinel_arm(void);

/* Whether the mesh is armed.  True only after arts_socket_sentinel_arm, whose
 * arming publishes to the caller, whatever its thread, the fabric's peer
 * table and endpoint, this rank's id and the mesh's sockets.  Callable from
 * any thread and from a signal handler (one lock-free atomic load).  What it
 * publishes stays valid until the process ends or, in a tree with a teardown,
 * arts_socket_cleanup, which must run only once every thread that may call
 * this or arts_socket_peer_gone has stopped. */
bool arts_socket_armed(void);

/* Whether this rank's connection to `rank` (< arts_global_rank_count) shows
 * the peer gone: closed or reset by it, or dropped by keepalive.  False for
 * this rank and before the mesh is armed.  Non-blocking, callable from any
 * thread and async-signal-safe (an atomic load, poll and recv; it may change
 * errno), until arts_socket_cleanup where a tree has one.  It never clears
 * what it reports: once true, it stays true. */
bool arts_socket_peer_gone(unsigned int rank);

/* Whether a probe of the sentinels is due for the prober whose cadence
 * *next_ns holds (0 = due at once): true when the mesh is armed
 * (arts_socket_armed) and the coarse monotonic clock has reached *next_ns,
 * which it then moves 1 ms past the clock's reading.  Each prober keeps its
 * own *next_ns. */
bool arts_socket_probe_due(uint64_t *next_ns);

/* Non-blocking probe of the sentinels for a closed peer
 * (POLLHUP/POLLERR/EOF): true when a peer's connection closed.  It enters
 * nothing: its caller starts the shutdown, as a rank that learned of the
 * end.  Stands down (false) once this rank's shutdown is in progress.
 * Nothing orders any rank's close after every other rank has taken in a
 * SHUTDOWN post — the post is best effort and reaches only the peers its
 * rank contacted, and no rank waits for anyone before it tears down — so an
 * orderly teardown can reach the probe first; its caller joins the same
 * shutdown.  Serialized internally to a single prober. */
bool arts_socket_sentinel_check(void);
#ifdef ARTS_DEBUG_TEARDOWN
/* Closes the sentinels and frees the mesh's state, once every thread that may
 * read them stopped. */
void arts_socket_cleanup(void);
#endif
#ifdef __cplusplus
}
#endif

#endif
