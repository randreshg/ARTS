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
#ifndef ARTS_MEMORY_FRONTIER_H
#define ARTS_MEMORY_FRONTIER_H
#ifdef __cplusplus
extern "C" {
#endif

#include "arts/runtime_types.h"

#define DBSPERELEMENT 8

struct arts_db_element_s {
  struct arts_db_element_s *next;
  unsigned int array[DBSPERELEMENT];
};

struct arts_local_delayed_edt_s {
  struct arts_local_delayed_edt_s *next;
  struct arts_edt_s *edt[DBSPERELEMENT];
  unsigned int slot[DBSPERELEMENT];
  arts_db_access_mode_t mode[DBSPERELEMENT];
};

struct arts_delayed_slice_request_s {
  struct arts_delayed_slice_request_s *next;
  struct arts_edt_s *edt[DBSPERELEMENT];
  arts_guid_t edt_guid[DBSPERELEMENT];
  unsigned int slot[DBSPERELEMENT];
  uint32_t flags[DBSPERELEMENT];
  uint64_t offset[DBSPERELEMENT];
  uint64_t size[DBSPERELEMENT];
};

/*
 * Remote RO readers pre-registered on this frontier generation. Unlike the
 * untargeted RO-node entries in frontier->list (served by an untargeted push
 * that relies on a matching pending pull), these carry (node, edt_guid, slot)
 * and are served by a targeted per-generation push when the frontier becomes
 * head, so the reader observes the snapshot of exactly its CDAG generation,
 * never a later (overwritten) buffer.
 */
struct arts_remote_ro_reader_s {
  struct arts_remote_ro_reader_s *next;
  unsigned int node[DBSPERELEMENT];
  arts_guid_t edt_guid[DBSPERELEMENT];
  unsigned int slot[DBSPERELEMENT];
};

struct arts_db_frontier_s {
  struct arts_db_element_s list;
  unsigned int position;
  struct arts_db_frontier_s *next;
  volatile unsigned int lock;

  /*
   * Remote writer slot — at most one remote writer per frontier.
   * Set when a remote node requests WRITE access (write && !local).
   * Signaled by arts_signal_frontier_local/remote when frontier progresses.
   */
  unsigned int exNode;
  arts_guid_t exEdtGuid;
  struct arts_edt_s *exEdt;
  unsigned int exSlot;
  arts_db_access_mode_t exMode;

  /*
   * Local writer owner for this frontier. Multiple EW/MEMSET slots from the
   * same EDT to the same DB are one logical CDAG acquisition and must share
   * this frontier.
   */
  arts_guid_t localWriteEdtGuid;
  struct arts_edt_s *localWriteEdt;

  /*
   * This is dumb, but we need somewhere to store requests
   * that are from the guid owner but cannot be satisfied
   * because of the memory model
   */
  unsigned int localPosition;
  struct arts_local_delayed_edt_s localDelayed;

  /*
   * Outstanding read-only (RO) consumers for a PURE-RO head frontier. Counts
   * BOTH local RO readers AND remote RO halo snapshots, so the RO head (and
   * therefore the live tile buffer) stays alive until every consumer of this
   * version has captured its data. The head is retired (arts_progress_frontier)
   * only when this reaches 0, i.e. when local readers have released and every
   * remote snapshot has been memcpy'd out — which guards the next EW from
   * overwriting the buffer while a halo neighbour still needs version t.
   *
   * Seeding / accounting (all under the frontier lock or via atomics):
   *   - Local readers: seeded once with frontier->localPosition when the
   *     PURE-RO frontier becomes head (arts_signal_frontier_local); decremented
   *     per local RO release in release_dbs.
   *   - Remote readers present at promotion: counted into roOutstanding for the
   *     remote nodes in frontier->list, then PUSH-snapshotted synchronously and
   *     decremented immediately (arts_signal_frontier_local). The snapshot is a
   *     memcpy (arts_remote_send_db_snapshot), so the bytes are safely captured
   *     before the decrement / any retirement.
   *   - Remote readers arriving while the head is alive: bumped before and
   *     dropped after their synchronous snapshot in arts_remote_db_send_check.
   *     The bump pins the head across the (already-synchronous) copy so a
   *     concurrent last-local-release cannot retire underneath the copy.
   *
   * roMarkedHead distinguishes "0 because this is an EW/non-RO frontier" from
   * "0 because a PURE-RO head fully drained": only a frontier that was seeded
   * as a PURE-RO head may be retired by the RO release path. EW-driven
   * frontiers keep roMarkedHead == 0 and retire on their write-release path, so
   * single-pass / 1-node / matmul schedules are untouched.
   */
  volatile unsigned int roOutstanding;
  volatile unsigned int roMarkedHead;

  /*
   * Copy-based RO slice requests (ESD) are delayed here when they cannot read
   * from the current frontier yet. They do not create cached full-DB copies.
   */
  unsigned int slicePosition;
  struct arts_delayed_slice_request_s sliceDelayed;

  /* Pre-registered remote RO readers for this generation (targeted serve). */
  unsigned int roReaderPosition;
  struct arts_remote_ro_reader_s roReaders;
};

struct arts_db_list_s {
  struct arts_db_frontier_s *head;
  struct arts_db_frontier_s *tail;
  volatile unsigned int reader;
  volatile unsigned int writer;
};

struct arts_db_frontier_iterator_s {
  struct arts_db_frontier_s *frontier;
  unsigned int currentIndex;
  struct arts_db_element_s *currentElement;
};

struct arts_db_frontier_s *arts_new_db_frontier();
struct arts_db_list_s *arts_new_db_list();
void arts_delete_db_list(struct arts_db_list_s *db_list);
unsigned int arts_current_frontier_size(struct arts_db_list_s *db_list);
bool arts_db_frontier_iter_init(struct arts_db_frontier_iterator_s *iter,
                                struct arts_db_frontier_s *frontier);
unsigned int
arts_db_frontier_iter_size(struct arts_db_frontier_iterator_s *iter);
bool arts_db_frontier_iter_next(struct arts_db_frontier_iterator_s *iter,
                                unsigned int *next);
bool arts_db_frontier_iter_has_next(struct arts_db_frontier_iterator_s *iter);
void arts_progress_frontier(struct arts_db_s *db, unsigned int rank);
/*
 * Underflow-safe decrement of a PURE-RO head's roOutstanding counter.
 * Atomically decrements *counter only while it is > 0 and returns true iff this
 * caller drove it from 1 to 0 (i.e. the head is now fully drained and the
 * caller should retire it). A decrement attempted at 0 is a no-op returning
 * false, so a consumer not accounted in the seed (e.g. a direct on-head local
 * RO acquire) can never underflow the counter or trigger a spurious retire.
 */
bool arts_ro_outstanding_dec_and_test(volatile unsigned int *counter);
bool arts_progress_and_get_frontier(struct arts_db_list_s *db_list,
                                    struct arts_db_frontier_iterator_s *iter);
bool arts_push_db_to_list(struct arts_db_list_s *db_list, unsigned int data,
                          bool write, bool local, bool bypass,
                          struct arts_edt_s *edt, arts_guid_t edt_guid,
                          unsigned int slot, arts_db_access_mode_t mode,
                          bool *on_head);
bool arts_request_db_slice(struct arts_db_s *db, struct arts_edt_s *local_edt,
                           arts_guid_t edt_guid, unsigned int slot,
                           uint64_t offset, uint64_t size, uint32_t flags);
/*
 * Pre-register a remote RO reader on the DB's frontier, preserving its CDAG
 * generation: the reader is served a targeted snapshot of its generation when
 * that frontier becomes head (or immediately if it lands on the head). This is
 * the cross-node RO-after-EW coherence fix — the reader observes its version,
 * never a later overwritten buffer. Returns true if registered (caller must NOT
 * also send the immediate signal); false to fall back to the legacy path.
 */
bool arts_register_remote_ro_reader(struct arts_db_s *db, unsigned int node,
                                    arts_guid_t edt_guid, unsigned int slot);
bool arts_close_frontier(struct arts_db_list_s *db_list,
                         struct arts_db_frontier_iterator_s *iter);
#ifdef __cplusplus
}
#endif

#endif /* ARTSDBLIST_H */
