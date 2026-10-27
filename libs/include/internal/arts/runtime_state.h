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
  struct arts_route_table_s **route_table;
  struct arts_route_table_s **gpu_route_table;
  struct arts_route_table_s *remote_route_table[ARTS_REMOTE_ROUTE_SHARDS];
  /* local_spin[i] points at thread i's `alive`: NULL until thread i has
   * written its role and set `alive`, which the store of the pointer
   * publishes.  That store and its load of shutdown_state
   * (arts_runtime_private_init) against the shutdown CAS and the stop walk's
   * load of the pointer (arts_enter_shutdown_state) are a store->load pair:
   * all four are seq_cst, so a stop that began before thread i registered is
   * seen by one of the two sides. */
  _Atomic(_Atomic(bool) *) *local_spin;
  /* Per-thread role, indexed by thread_id (0..total_thread_count-1).
   * Populated during arts_runtime_private_init so that shutdown paths
   * can filter threads by role (workers vs. progress). */
  unsigned int *thread_roles;
  unsigned int worker_thread_count;
  unsigned int progress_thread_count;
  unsigned int total_thread_count;
  /* Barriers counted down from the thread count (arts_runtime_barrier):
   * each arrival's decrement releases what its thread wrote before it, and
   * the acquire load that reads zero takes every arrival's. */
  _Atomic(unsigned int) ready_to_push;
  _Atomic(unsigned int) ready_to_parallel_start;
  _Atomic(unsigned int) ready_to_inspect;
  _Atomic(unsigned int) ready_to_clean;
  /* 0 = running, 1 = shutting down.  Written only by the 0 -> 1 CAS in
   * arts_enter_shutdown_state, which every path into the shutdown takes
   * first, and seq_cst there as one side of the stop-registration pair (see
   * local_spin).  Reads outside the pair are acquire and learn only that a
   * shutdown began, so long-running loops (the connect retry) can bail out. */
  _Atomic(unsigned int) shutdown_state;
  /* Stored (release) once the CAS winner's entry has finished, its stop walk
   * included; teardown waits for it (acquire) before it frees anything the
   * entry touches or reads the end stamp the entry took. */
  _Atomic(unsigned int) shutdown_entry_done;
  /* End-to-end wall-clock marker (rank 0 only), independent of the counter
   * subsystem.  e2e_start_stamp is taken when the main application EDT
   * becomes runnable (init complete — so application init callbacks run
   * inside the span); e2e_end_stamp is taken at shutdown recognition (the
   * enter-shutdown CAS, before any teardown/drain).  When e2e_marker_enabled
   * their difference is printed as "[E2E] <ns>" on rank 0 at exit.  Gated so
   * a normal run is never perturbed.  The flag is set from $ARTS_E2E_MARKER
   * before any other thread exists, the start stamp is written and read by
   * thread 0 alone, and the end stamp is read only after
   * shutdown_entry_done. */
  uint64_t e2e_start_stamp;
  uint64_t e2e_end_stamp;
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
  const char *counter_folder;
  /* [thread_id] -> that thread's counters, stored before the thread's first
   * barrier arrival; the capture thread starts after the last startup
   * barrier and is joined before thread 0's teardown barrier arrival, so
   * every thread it reads is still alive. */
  arts_counter_t **live_counters;
  /* [thread_id][counter_index]: each thread's final counts, stored when it
   * leaves the runtime and read after it is joined. */
  uint64_t **saved_counters;
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
   * The owner's own stores are relaxed: only it reads them, and the seq_cst
   * publication of local_spin orders its first store before any stop. */
  _Atomic(bool) alive;
  enum arts_thread_role role;
  /* This thread's pseudo-random stream: a private counter, and a key that
     makes the stream itself different from every other thread's rather than
     the same sequence started at a different point. */
  uint64_t rng_counter;
  uint64_t rng_key;
};

extern struct arts_runtime_shared_s arts_node_info;
extern ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

/* Pending work a shutdown dropped instead of performing, counted by the
 * sites that drop it (relaxed increments; the thread joins that teardown
 * already performs are what make a later read see them).  Together with what
 * the teardown can still count in place -- EDTs admitted but never finished,
 * messages still parked on a route slot's OoO list (a create whose occupant
 * was never destroyed, or any message whose object never installed) -- it
 * says whether a shutdown was quiescent. */
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
  /* Runtime threads the shutdown gave up joining.  Not work left undone but
   * the premise every teardown read rests on: while one is nonzero the
   * quiescence walk does not run, since a thread still alive may be mid
   * transition on anything it would read. */
  _Atomic(unsigned int) unjoined_threads;
  /* Which threads those are, by thread index: NULL until the first, then
   * total_thread_count flags.  Written only by the main thread's join loop and
   * read only by that thread afterwards.  Such a thread is past the teardown
   * barrier but may still write its own counter row, so its counts are left
   * out of every counter report. */
  bool *unjoined;
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
