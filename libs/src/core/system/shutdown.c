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
#include "arts/system/shutdown.h"
#include "arts/transport/net.h"    /* arts_net_close_gate, _post_shutdown */
#include "arts/transport/socket.h" /* arts_socket_armed */
#include "arts/utils/stdatomic_compat.h"

/* The program ends the run (origin APP).  Once this rank has entered the
 * shutdown state, by any path, a call changes nothing. */
void arts_shutdown() {
  ARTS_INFO("arts_shutdown: rank_count=%u, rank_id=%u", arts_global_rank_count,
            arts_global_rank_id);
  arts_enter_shutdown_state(ARTS_EXIT_APP);
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

void arts_enter_shutdown_state(enum arts_exit_origin origin) {
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
   * shutdown is first recognized, before anything the rank does to end.
   * Stamp the e2e end here so the measured span excludes teardown, matching
   * the reference runtimes (which stop at shutdown reception).  Any thread may
   * execute this; arts_get_time_stamp is thread-independent and the CAS
   * guarantees it runs exactly once per node. */
  if (!arts_global_rank_id && arts_node_info.e2e_marker_enabled)
    arts_node_info.e2e_end_stamp = arts_get_time_stamp();
  bool multirank = arts_global_rank_count > 1;
  if (multirank) {
    /* A send that reads the gate closed is dropped; the post bypasses it. */
    arts_net_close_gate();
  }
  ARTS_INFO("arts_enter_shutdown_state: rank=%u origin=%s", arts_global_rank_id,
            arts_exit_origin_name(origin));
  /* The post goes only from a rank whose end begins here, and only once the
   * mesh is armed, whose acquire makes the peer table and the endpoint the
   * post reads visible to this thread. */
  if (multirank && arts_exit_origin_originates(origin) && arts_socket_armed()) {
    unsigned int posted = arts_net_post_shutdown();
    ARTS_INFO("arts_enter_shutdown_state: posted SHUTDOWN to %u peer(s)",
              posted);
  }
  /* Progress threads stay alive: they keep taking in what arrives. */
  arts_runtime_stop_workers();
  atomic_store_explicit(&arts_node_info.shutdown_entry_done, 1U,
                        memory_order_release);
}
