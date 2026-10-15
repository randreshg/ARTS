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
#include "arts/system/threads.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "arts.h"
#include "arts/runtime_state.h"
#include "arts/system/print.h"
#include "arts/transport/dispatcher.h"
#include "arts/transport/net.h" /* arts_net_drain_outstanding */
#include "arts/utils/stdatomic_compat.h"

/*
 * arts_shutdown — Initiate global shutdown of the ARTS runtime.
 *
 * Enters the shutdown state as its initiator (arts_enter_shutdown_state): on a
 * multi-node run it sends MSG_SHUTDOWN to every other rank over the fabric,
 * like any other message, and waits (bounded) for those sends to complete;
 * then it stops this rank's worker threads.  A rank that receives the message
 * enters the same state without rebroadcasting (arts_handler_shutdown).  Once
 * this rank has entered the shutdown state, by any path, a call changes
 * nothing.
 *
 * Called from:
 *   - User code via the arts_shutdown() public API.
 */
void arts_shutdown() {
  ARTS_INFO("arts_shutdown: rank_count=%u, rank_id=%u", arts_global_rank_count,
            arts_global_rank_id);
  /* Phase A entry — arts_enter_shutdown_state handles both the
   * multi-node broadcast + drain and the local worker-thread stop. */
  arts_enter_shutdown_state(/* initiator = */ true);
  (void)fflush(stdout);
}

/* Other threads may hold any runtime state half-way through an update, so
 * nothing that assumes a quiescent process may run: no exit handler, no
 * destructor, no allocator or sanitizer teardown.  It waits for no other
 * thread: a stream another thread holds is left as it is (a flush to a sink
 * that accepts nothing can still block in write), and the process ends through
 * the exit_group system call itself, since a sanitizer interposes _exit with
 * its own exit-time checks. */
_Noreturn void arts_exit_now(uint8_t status) {
  FILE *const streams[] = {stdout, stderr};
  for (size_t i = 0; i < sizeof(streams) / sizeof(streams[0]); i++) {
    if (ftrylockfile(streams[i]) == 0) {
      (void)fflush_unlocked(streams[i]);
      funlockfile(streams[i]);
    }
  }
  (void)syscall(SYS_exit_group, (int)status);
  _exit(status);
}

/* Every stdio stream is flushed first, as exit() does, so the wait for a
 * stream another thread is in the middle of using is exit()'s too. */
_Noreturn void arts_abort(uint8_t error_code) {
  (void)fflush(NULL);
  arts_exit_now(error_code);
}

/*
 * arts_enter_shutdown_state — the single internal CAS-gated mechanism for
 * transitioning a rank into SHUTTING_DOWN state.  Both the initiator side
 * (arts_shutdown) and the passive side (arts_handler_shutdown) reach this.
 *
 * Called from:
 *   - arts_shutdown() on the user EDT path (initiator = true)
 *   - the signal watcher on a terminating signal (initiator = true)
 *   - arts_handler_shutdown() — the MSG_SHUTDOWN wire RX path
 *     (initiator = false)
 *   - the liveness sentinel (arts_socket_sentinel_check) when a peer's
 *     bootstrap connection closes before this rank's shutdown began
 *     (initiator = false)
 *
 * Idempotent: repeated calls after the first are no-ops. The CAS on
 * shutdown_state ensures exactly one caller performs the broadcast and
 * stop-workers step.
 */
void arts_enter_shutdown_state(bool initiator) {
  /* seq_cst: this CAS and the stop walk's loads of local_spin are one side
   * of the stop-registration store->load pair; arts_runtime_private_init's
   * store of local_spin and load of shutdown_state are the other. */
  unsigned int running = 0U;
  if (!atomic_compare_exchange_strong_explicit(
          &arts_node_info.shutdown_state, &running, 1U, memory_order_seq_cst,
          memory_order_relaxed)) {
    return; /* another thread / handler already started shutdown */
  }
  /* End-to-end marker (rank 0): this CAS is the single idempotent point where
   * shutdown is first recognized — before the broadcast + outstanding-drain
   * teardown below.  Stamp the e2e end here so the measured span excludes
   * teardown, matching the reference runtimes (which stop at shutdown
   * reception).  Any thread may execute this; arts_get_time_stamp is
   * thread-independent and the CAS guarantees it runs exactly once per node. */
  if (!arts_global_rank_id && arts_node_info.e2e_marker_enabled)
    arts_node_info.e2e_end_stamp = arts_get_time_stamp();
  ARTS_INFO("arts_enter_shutdown_state: rank=%u initiator=%d",
            arts_global_rank_id, (int)initiator);
  if (initiator && arts_global_rank_count > 1) {
    /* Phase A.1: broadcast MSG_SHUTDOWN to every other rank (over the fabric,
     * like every other message). */
    arts_transport_broadcast_shutdown();
    /* Phase A.2: wait (bounded) for those fabric sends to complete so the
     * SHUTDOWN frames leave this node before teardown.  The progress threads
     * are still running and dispatch what arrives; the drain only reaps
     * completions and polls the outstanding-TX count, since a handler it
     * dispatched could ready an EDT on a thread that owns no deque. */
    arts_net_drain_outstanding(500U /* SHUTDOWN_DRAIN_MS */);
  }
  /* Phase A.3: stop worker threads.  Progress threads remain alive so they
   * can flush any in-flight traffic and deliver any inbound MSG_SHUTDOWN. */
  arts_runtime_stop_workers();
  atomic_store_explicit(&arts_node_info.shutdown_entry_done, 1U,
                        memory_order_release);
}

/*
 * arts_handler_shutdown — passive RX entry.  The wire dispatcher
 * calls this directly on MSG_SHUTDOWN.  Lightweight: the idempotent CAS gate
 * (0→1) + worker-thread stop signal, with NO rebroadcast and NO drain-wait
 * (those are the initiator arts_shutdown's responsibility).  Implemented as the
 * non-initiator arts_enter_shutdown_state path so the CAS gate stays the single
 * source of idempotence shared with the initiator side.
 */
void arts_handler_shutdown(void) {
  arts_enter_shutdown_state(/* initiator = */ false);
}
