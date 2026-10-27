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
#include "arts/runtime_state.h"
#include "arts/utils/malloc.h"

#include <assert.h>
#include <stdlib.h>
#include <time.h>

#include "arts/counter/Preamble.h"
#include "arts/counter/counter.h"
#include "arts/db.h"
#include "arts/defs.h"
#include "arts/edt.h"
#include "arts/edt_context.h" /* arts_set/unset_thread_local_edt_info */
#include "arts/gas/guid.h"
#include "arts/gas/route_table.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/system/topology.h"
#include "arts/transport/dispatcher.h"
#include "arts/transport/net.h"
#include "arts/transport/protocol.h"
#include "arts/transport/socket.h"
#include "arts/utils/array_list.h"
#include "arts/utils/random.h"
#include "arts/utils/deque.h"

#ifdef ARTS_USE_GPU
#include "arts/gpu/gpu_internal.h"
#include "arts/gpu/gpu_stream.h"
#endif

ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

#ifdef ARTS_USE_GPU
scheduler_t arts_scheduler_loop[] = {
    (scheduler_t)arts_default_scheduler_loop,
    (scheduler_t)arts_network_before_steal_scheduler_loop,
    (scheduler_t)arts_network_first_scheduler_loop,
    (scheduler_t)arts_gpu_scheduler_loop,
    (scheduler_t)arts_gpu_scheduler_backoff_loop,
    (scheduler_t)arts_gpu_scheduler_demand_loop};
#else
scheduler_t arts_scheduler_loop[] = {
    (scheduler_t)arts_default_scheduler_loop,
    (scheduler_t)arts_network_before_steal_scheduler_loop,
    (scheduler_t)arts_network_first_scheduler_loop};
#endif
const unsigned int arts_scheduler_loop_count =
    sizeof(arts_scheduler_loop) / sizeof(arts_scheduler_loop[0]);

/* Schedule a fully DB-acquired EDT onto the calling thread's own
 * work-stealing deque.  Reached when the acquire phase ends: every
 * acquisition resolved at this rank, or none was needed — either
 * synchronously (initial arts_handle_ready_edt on a worker) or asynchronously
 * (a coherence wake / OoO drain resumes the walk on the progress thread that
 * dispatches the waking message).  A deque has one owner, the only thread that
 * pushes onto it, so only a thread that owns a deque may get here: every thread
 * that dispatches a message owns one. */
void arts_schedule_ready_edt(struct arts_edt_s *edt) {
  INCREMENT_NUM_EDT_ACQUIRE_BY(1);
#ifdef ARTS_USE_GPU
  if (arts_node_info.gpu && !arts_thread_info.my_gpu_deque &&
      edt->edt_type == ARTS_EDT_GPU) {
    /* CUDA callback thread, GPU EDT: deferred via new_edts. */
    arts_store_new_edts(edt);
  } else
#endif
      if (edt->edt_type == ARTS_EDT_GPU) {
    struct arts_deque_s *q = arts_thread_info.my_gpu_deque;
    assert(q != NULL && "only the owner of a GPU deque pushes onto it");
    ARTS_INFO("EDT[Guid:%lu] pushed to GPU deque", edt->guid);
    arts_deque_push_front(q, edt, 0);
  } else {
    struct arts_deque_s *q = arts_thread_info.my_deque;
    assert(q != NULL && "only the owner of a deque pushes onto it");
    ARTS_INFO("EDT[Guid:%lu] pushed to worker deque", edt->guid);
    arts_net_held_edt_runnable();
    arts_deque_push_front(q, edt, 0);
  }
}

/*
 * arts_handle_ready_edt — Transition an EDT from "all deps signaled" to DB
 * acquisition.  Called when depc_needed reaches 0 (the satisfy phase
 * completes).  Seeds the cursor (rw_cursor = 0) and enters
 * arts_db_acquire_all, which sets the readiness counter acquire_remaining to
 * its bias plus the acquisitions, fires all non-serialized (RO) deps at once
 * and walks the serialized (RW) deps by GUID-ordered cursor.  The EDT is
 * scheduled (via arts_schedule_ready_edt) when acquire_remaining reaches 0 —
 * i.e. when every DB dep's data has resolved at this rank — or at once when it
 * acquires nothing.
 */
void arts_handle_ready_edt(struct arts_edt_s *edt) {
  ARTS_INFO("EDT[Guid:%lu] ready — entering sequential DB acquire (depc=%u)",
            edt->guid, edt->depc);
  /* Take the runnable-phase ref: from here the EDT travels by raw pointer
   * through DB acquire (including asynchronous coherence park/resume on other
   * threads), the work-stealing deque, and execution — none of which can
   * recover a control block to ref-count.  Copying the install-time self alias
   * pins the EDT cb alive across that whole span, so a concurrent destroy
   * (which only CAS-detaches the route slot, dropping the install ref) cannot
   * free an EDT that is still queued or running.  Released exactly once at run
   * completion (arts_run_edt / the GPU completion path).  Route-independent:
   * the copy succeeds even if destroy already detached the slot, because the
   * caller still holds a ref (the OoO dispatch pin, or the creating thread).
   * arts_shared_copy(NULL) is a safe no-op for any unarmed path. */
  arts_shared_copy(edt->self_cb);
  edt->rw_cursor = 0;
  arts_db_acquire_all(edt);
}

void arts_run_edt(struct arts_edt_s *edt) {
  uint32_t depc = edt->depc;
  arts_edt_dep_t *depv =
      (arts_edt_dep_t *)(((uint64_t *)(edt + 1)) + edt->paramc);

  arts_edt_t func = edt->func_ptr;
  uint32_t paramc = edt->paramc;
  const uint64_t *paramv = (uint64_t *)(edt + 1);

  ARTS_INFO("Running EDT[Guid:%lu, Deps: %u, Params: %u, DepvPtr: %p]",
            edt->guid, depc, paramc, depv);
  arts_prep_dbs(edt, false);

  arts_set_thread_local_edt_info(edt);

  TIME_EDT_EXEC_START();
#if ARTS_LOG_LEVEL >= 2
  struct timespec start_time;
  struct timespec end_time;
  (void)clock_gettime(CLOCK_MONOTONIC, &start_time);
#endif
  func(paramc, paramv, depc, depv);
  arts_net_held_edt_returned();
#if ARTS_LOG_LEVEL >= 2
  (void)clock_gettime(CLOCK_MONOTONIC, &end_time);
#endif
  TIME_EDT_EXEC_STOP();

  INCREMENT_NUM_EDT_FINISH_BY(1);

  /* Release DBs before signaling completion.  No message order between a
   * release and the completion signal is relied on: each arm's release
   * returns only once a later acquirer is bound to see the writes — an arm
   * that publishes or invalidates at release waits for the home's
   * acknowledgement; where the bytes stay at this rank (write-back, a
   * retained right) the home sends that acquirer here; under exclusion the
   * home grants nobody else until it has paired the release with its payload;
   * and where the payload rests in CXL memory the release is sent only after
   * the bytes are back in the store. */
  arts_release_dbs(edt, false);
  arts_release_created_dbs();

  /* Retire the GUID before any completion signal leaves: whoever observes the
   * completion (output event, finish scope) may create the GUID's next
   * generation and satisfy it at once, and those messages must find this
   * generation gone rather than run against it.  The runnable-phase ref taken
   * in arts_handle_ready_edt keeps the EDT alive past the detach, and its
   * release below is the last drop that runs the deleter. */
  arts_shared_ptr_t sref = edt->self_cb;
  arts_edt_delete(edt);

  /* Output event: satisfied only now, after every release above, so a
   * consumer it wakes cannot acquire one of this EDT's data blocks before
   * the writes are published (a satisfy sent from the EDT body would
   * precede the version bump and a remote reader could be served its stale
   * cached copy).  The payload is whatever the body registered via
   * arts_edt_set_result (NULL_GUID if it registered none). */
  if (edt->output_event != NULL_GUID) {
    arts_event_satisfy_slot(edt->output_event, edt->output_data,
                            ARTS_EVENT_LATCH_DECR_SLOT);
  }

  arts_unset_thread_local_edt_info(true);

#if ARTS_LOG_LEVEL >= 2
  ARTS_INFO("EDT[Guid:%lu] finished (exec_ns=%lu)", edt->guid,
            (unsigned long)(((end_time.tv_sec - start_time.tv_sec) *
                             1000000000ULL) +
                            (end_time.tv_nsec - start_time.tv_nsec)));
#endif
  arts_shared_release(&sref);
}

inline struct arts_edt_s *arts_runtime_steal_from_network() {
  struct arts_edt_s *edt = NULL;
  if (arts_global_rank_count > 1) {
    unsigned int index = arts_thread_info.thread_id;
    for (unsigned int i = 0; i < arts_node_info.progress_thread_count; i++) {
      index = (index + 1) % arts_node_info.progress_thread_count;
      if ((edt = (struct arts_edt_s *)arts_deque_pop_back(
               arts_node_info.progress_deque[index])) != NULL) {
        INCREMENT_NUM_STEAL_SUCCESS_BY(1);
        break;
      }
    }
  }
  return edt;
}

inline struct arts_edt_s *arts_runtime_steal_from_worker() {
  struct arts_edt_s *edt = NULL;
  if (arts_node_info.total_thread_count > 1) {
    long unsigned int steal_loc;
    do {
      /* Through the shared helper, not a raw jrand48: its result is signed, and
       * widening a negative draw into an unsigned victim index skews which
       * deque gets robbed. */
      steal_loc =
          arts_thread_safe_random() % arts_node_info.total_thread_count;
    } while (steal_loc == arts_thread_info.thread_id);
    edt = (struct arts_edt_s *)arts_deque_pop_back(
        arts_node_info.deque[steal_loc]);
    if (edt) {
      INCREMENT_NUM_STEAL_SUCCESS_BY(1);
    }
  }
  return edt;
}

bool arts_network_first_scheduler_loop() {
  struct arts_edt_s *edt_found;
  if (!(edt_found = arts_runtime_steal_from_network())) {
    if (!(edt_found = (struct arts_edt_s *)arts_deque_pop_front(
              arts_thread_info.my_deque))) {
      edt_found = arts_runtime_steal_from_worker();
    }
  }
  if (edt_found) {
    arts_run_edt(edt_found);
    return true;
  }
  return false;
}

bool arts_network_before_steal_scheduler_loop() {
  struct arts_edt_s *edt_found;
  if (!(edt_found = (struct arts_edt_s *)arts_deque_pop_front(
            arts_thread_info.my_deque))) {
    if (!(edt_found = arts_runtime_steal_from_network())) {
      edt_found = arts_runtime_steal_from_worker();
    }
  }

  if (edt_found) {
    arts_run_edt(edt_found);
    return true;
  }
  return false;
}

bool arts_default_scheduler_loop() {
  struct arts_edt_s *edt_found = NULL;
  if (!(edt_found = (struct arts_edt_s *)arts_deque_pop_front(
            arts_thread_info.my_deque))) {
    if (!edt_found) {
      if (!(edt_found = arts_runtime_steal_from_worker())) {
        edt_found = arts_runtime_steal_from_network();
      }
    }
  }

  if (edt_found) {
    arts_run_edt(edt_found);
    return true;
  }
  arts_runtime_idle_pause();
  return false;
}
