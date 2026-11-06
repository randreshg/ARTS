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
#ifndef ARTS_RUNTIME_STATE_H
#define ARTS_RUNTIME_STATE_H
#ifdef __cplusplus
extern "C" {
#endif
#include "arts/counter/counter.h"
#include "arts/defs.h"
#include "arts/gas/route_table.h"
#include "arts/runtime_types.h"
#include "arts/system/topology.h"
#include "arts/utils/array_list.h"
#include "arts/utils/stdatomic_compat.h"
#include "arts/utils/vector.h"
#ifdef ARTS_USE_CXL
#include "arts/cxl/arena.h"
#include <pthread.h>
#endif

struct arts_runtime_shared_s {
  bool (*scheduler)();
  struct arts_deque_s **deque;
  struct arts_deque_s **progress_deque;
  struct arts_deque_s **gpu_deque;
#ifdef ARTS_USE_CXL
  arts_cxl_arena_table_t *cxl_arenas;
  pthread_mutex_t cxl_arena_lock;
  unsigned int cxl_db_dev_count; /**< Number of CXL DB arenas (devices). */
  /* Arena index of the next round-robin allocation: only its own value. */
  _Atomic(unsigned int) cxl_db_rr_idx;
  unsigned int cxl_db_static_device; /**< Device index for static allocation. */
#endif
  /* This rank's route tables, arts_guid_table_count of them: built by
   * arts_route_table_node_init before any runtime thread exists (thread
   * creation publishes them) and freed after the teardown's stop.  A key's
   * table is the pure function of its key that arts_get_route_table
   * computes. */
  struct arts_route_table_s **route_table;
  struct arts_route_table_s **gpu_route_table;
  /* local_spin[i] points at thread i's `alive`, stored by thread i before its
   * ready_to_push arrival; read only after node_ready is acquired (by the
   * teardown's stopper), which orders the store before the read and thread
   * i's own true before the stopper's false. */
  _Atomic(bool) **local_spin;
  /* [thread_id] -> that thread's stop slot, the one arts_thread_stop_slot
   * names on the thread. */
  struct arts_thread_stop_s *thread_stop;
  unsigned int worker_thread_count;
  unsigned int progress_thread_count;
  unsigned int total_thread_count;
  /* Barriers counted down from the thread count (arts_runtime_barrier):
   * each arrival's decrement releases what its thread wrote before it, and
   * the acquire load that reads zero takes every arrival's. */
  _Atomic(unsigned int) ready_to_push;
  _Atomic(unsigned int) ready_to_parallel_start;
  _Atomic(unsigned int) ready_to_inspect;
  /* End-to-end wall-clock marker (rank 0 only), independent of the counter
   * subsystem: the start stamp is taken when the main application EDT is
   * about to become runnable, and the end is the rank's shutdown point; their
   * difference is printed as "[E2E] <ns>" at the end when e2e_marker_enabled.
   * The flag is set from $ARTS_E2E_MARKER before any other thread exists, and
   * the start stamp is published by node_ready. */
  uint64_t e2e_start_stamp;
  int e2e_marker_enabled;
  /* Round-robin counter used by arts_db_create when the caller passes
   * NULL hint (no node preference) — distributes home rank across all
   * nodes so DBs aren't all pinned to the creator.  Only its own value. */
  _Atomic(unsigned int) db_rr_route;
  /* Round-robin counter used by arts_edt_create when the caller passes
   * NULL hint (no node preference) — distributes execution rank across
   * all nodes so hint-less work isn't all pinned to the creator.  Only its
   * own value. */
  _Atomic(unsigned int) edt_rr_route;
  unsigned int gpu;
  unsigned int gpu_locality;
  unsigned int gpu_fit;
  unsigned int gpu_lc_sync;
  unsigned int gpu_max_edts;
  unsigned int gpu_p2p;
  unsigned int gpu_route_table_size;
  unsigned int gpu_route_table_entries;
  uint64_t gpu_max_memory;
  bool free_db_after_gpu_run;
  bool run_gpu_gc_idle;
  bool run_gpu_gc_pre_edt;
  bool delete_zeros_gpu_gc;
  bool gpu_buff_on;
  uint64_t **keys;
  /* [thread_id] -> that thread's counters, stored before the thread's first
   * barrier arrival, which node_ready orders before the record's reads; the
   * capture thread starts after the last startup barrier, and a runtime
   * thread never exits, so every row stays alive. */
  arts_counter_t **live_counters;
  arts_array_list_t ***capture_arrays; // [thread_id][counter_index] - capture
                                       // history (PERIODIC)
  uint64_t counter_capture_interval;
} ARTS_ALIGNED(64);

struct arts_runtime_private_s {
  struct arts_deque_s *my_deque;
  struct arts_deque_s *my_gpu_deque;
  unsigned int pu_id;
  unsigned int thread_id;
  unsigned int group_pos;
  unsigned int numa_domain_id;
  /* Cleared (release) by a stopper on another thread; the thread's loops
   * read it with acquire, so a stopped thread sees what preceded the stop.
   * The owner's own store is relaxed: only it reads it, and its ready_to_push
   * arrival, which node_ready carries to the stopper, orders it before any
   * stop. */
  _Atomic(bool) alive;
  enum arts_thread_role role;
  /* This thread's pseudo-random stream: a private counter, and a key that
     makes the stream itself different from every other thread's rather than
     the same sequence started at a different point. */
  uint64_t rng_counter;
  uint64_t rng_key;
};

extern struct arts_runtime_shared_s arts_node_info;
extern ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;

/* A runtime thread's stop slot.  Its owner stores a stopped value (release)
 * once it has left its loop for good, after every access it makes to shared
 * runtime state, and never stores again; a stopper acquires it, so a stopped
 * thread's last accesses happen before what the stopper does next.  RUNNING
 * is the zero the node init stores. */
enum {
  ARTS_THREAD_RUNNING = 0,
  ARTS_THREAD_STOPPED,
  /* Stopped where shared state may be half-way through an update. */
  ARTS_THREAD_STOPPED_UNSAFE
};
struct arts_thread_stop_s {
  _Atomic(unsigned int) state;
  /* Written by a thread that entered the rank's end after another thread took
   * it, before its stopped release: the EDT it was running and its
   * created-DB list, both left as they were. */
  struct arts_edt_s *caller_edt;
  arts_vector_t *caller_dbs;
} ARTS_ALIGNED(64);
/* This thread's slot in arts_node_info.thread_stop; NULL on a thread the
 * runtime did not create. */
extern ARTS_THREAD_LOCAL_IE struct arts_thread_stop_s *arts_thread_stop_slot;

/* Pending work a shutdown dropped instead of performing, counted by the
 * sites that drop it (relaxed increments; the teardown's acquire of every
 * thread's stop slot is what makes a later read see them).  Together with
 * what the teardown can still count in place -- EDTs admitted but never
 * finished, messages still parked on a route slot's OoO list (a create whose
 * occupant was never destroyed, or any message whose object never installed)
 * -- it says whether a shutdown was quiescent. */
struct arts_shutdown_abandon_s {
  /* Runnable EDTs dropped from a deque unrun. */
  _Atomic(unsigned int) queued_edts;
  /* Release waits the shutdown cut short or left unawaited, one per wait,
   * counted wherever a release stops waiting for its reply because the
   * shutdown began -- on any arm, so an arm with nothing for the walk to
   * judge still counts.  That can only make a shutdown read as
   * non-quiescent more often; the states the walk reports unconditionally
   * are never masked by it. */
  _Atomic(unsigned int) waits;
};
extern struct arts_shutdown_abandon_s arts_shutdown_abandon;

#ifdef __cplusplus
}
#endif

/* Back-compat: the runtime function prototypes (and the scheduler_t typedef /
 * arts_scheduler_loop extern) used to live here.  They now live in
 * arts/runtime.h.  Include it at the end so every existing runtime_state.h
 * includer still transitively sees them without source churn.  This header must
 * be included AFTER the struct layouts above so the include direction stays
 * one-way (runtime_state.h -> runtime.h); runtime.h itself only
 * forward-declares the struct types it references, never including this header
 * back. */
#include "arts/runtime.h"

#endif
