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
 * Remote RO readers pre-registered on a frontier generation in CDAG order
 * (arts_register_remote_ro_reader). Each carries the consumer node, its EDT
 * guid, and the dep slot. When the generation reaches head, each reader is
 * served a TARGETED per-generation snapshot of the (post-write) owner DB via
 * arts_remote_signal_edt_with_ptr — a copy delivered straight to the EDT slot,
 * bypassing the route-table cache so a later EW overwrite cannot make the read
 * stale, and so repeated reads of the same GUID across timesteps each get the
 * correct generation's value.
 */
struct arts_ro_reader_s {
  struct arts_ro_reader_s *next;
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
   * Set when a remote EW writer was pre-registered in CDAG order at
   * arts_add_dependence time (arts_register_remote_ew_writer) — i.e. before
   * the writer's own full DB request reaches the owner. This makes a
   * following LOCAL RO reader on the owner land on a *later* frontier so it
   * cannot race ahead of the remote write. exDelivered tracks whether the
   * DB has already been shipped to the pre-registered writer, so the late
   * full request does not double-deliver.
   */
  bool exPreRegistered;
  bool exDelivered;

  /*
   * Number of owner-local RO readers registered on this generation that have
   * not yet completed. A reader-only generation (no exclusive writer) is not
   * retired by any writer update, so it must be retired by its own consumers:
   * each owner-local RO reader increments this when it joins the generation
   * (arts_push_db_to_list) and decrements it on release; the reader that
   * drives it to zero progresses the frontier. This is what lets a strict
   * W -> R -> W -> R chain advance deterministically once readers and the
   * following writer occupy separate generations.
   * (roOutstanding itself is declared once below.)
   */

  /*
   * Set the first time this generation is signaled as head (by
   * arts_signal_frontier_local/remote). A generation can reach head by two
   * paths that may race: a predecessor's retirement (arts_progress_frontier)
   * and self-promotion when the generation is registered over a settled, empty
   * frontier (arts_signal_fresh_head). This flag makes the head signal
   * idempotent so consumers are satisfied exactly once.
   */
  bool headSignaled;

  /*
   * Remote RO readers pre-registered on this generation in CDAG order. Served a
   * targeted snapshot when the generation reaches head (roReadersServed guards
   * exactly-once delivery). roReadersCount is the number of registered readers.
   */
  unsigned int roReadersCount;
  bool roReadersServed;
  struct arts_ro_reader_s roReaders;

  /*
   * Owner-LOCAL RO readers pre-registered on this generation in CDAG order
   * (single-node eager prereg). Unlike roReaders (remote, served a copy
   * snapshot), these are delivered IN-PLACE (DB_MODE_RO) by the normal acquire
   * path: each reader's later phase-2 acquire JOINS this reserved generation by
   * matching its edt_guid here (the reader dual of localWriteEdtGuid), keeping
   * the in-place pointer + roOutstanding accounting. localRoReadersPos is the
   * append cursor; localRoPending is reserved-minus-joined (a generation with
   * pending reservations is not yet drained — a reader is still coming). A
   * matched entry's edt_guid is cleared to NULL_GUID so it is not re-joined.
   */
  unsigned int localRoReadersPos;
  unsigned int localRoPending;
  struct arts_ro_reader_s localRoReaders;

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
   * Outstanding owner-local read-only (RO) consumers on a PURE-RO head
   * generation. Keeps the RO head (and the live tile buffer) alive until every
   * local reader of this version has released, so the next EW cannot overwrite
   * the buffer while a reader still needs version t. The head is retired
   * (arts_progress_frontier) only when this reaches 0.
   *
   * Seeded once per reader at registration (arts_push_db_to_list) and
   * decremented per local RO release (release_dbs); the reader that drives it to
   * 0 retires the head. EW-driven frontiers (single-pass, matmul, 1-node) carry
   * no RO consumers here and retire on their write-release path instead.
   *
   * Remote RO halo readers are NOT counted here: they are pre-registered in
   * CDAG order on the roReaders list and served exactly once via
   * arts_serve_ro_readers when the generation reaches head.
   */
  volatile unsigned int roOutstanding;

  /*
   * Copy-based RO slice requests (ESD) are delayed here when they cannot read
   * from the current frontier yet. They do not create cached full-DB copies.
   */
  unsigned int slicePosition;
  struct arts_delayed_slice_request_s sliceDelayed;
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
bool arts_close_frontier(struct arts_db_list_s *db_list,
                         struct arts_db_frontier_iterator_s *iter);

/*
 * Pre-register a remote EW writer on the owner's frontier in CDAG order.
 * Called from arts_add_dependence_ex when a locally-owned DB is given an EW
 * dependence whose destination EDT lives on another node. Reserves an
 * exclusive-writer frontier generation for that writer *before* its own full
 * DB request arrives, so a subsequent local RO reader is ordered after it.
 * Returns true if a new pre-registration generation was created.
 */
bool arts_register_remote_ew_writer(struct arts_db_s *db, unsigned int rank,
                                    arts_guid_t edt_guid, unsigned int slot,
                                    arts_db_access_mode_t mode);

/*
 * Reserve a dedicated CDAG generation for a LOCAL EW writer at
 * arts_add_dependence time. The writer later joins this generation via the
 * normal acquire path (localWriteEdtGuid match) and progresses it on release.
 */
bool arts_register_local_ew_writer(struct arts_db_s *db, arts_guid_t edt_guid);

/*
 * Reserve a generation for an owner-LOCAL RO reader in CDAG order (single-node
 * eager prereg). Records edt_guid on the chosen reader generation's
 * localRoReaders list so the reader's later phase-2 acquire joins THIS
 * generation in-place (DB_MODE_RO), rather than racing onto whatever generation
 * happens to be head when the reader EDT runs. A following EW writer
 * (arts_register_local_ew_writer) then lands on a strictly-later sealed
 * generation, fixing the RO -> EW reused-DB order deterministically.
 */
bool arts_register_local_ro_reader(struct arts_db_s *db, arts_guid_t edt_guid,
                                   unsigned int slot);

/*
 * Phase-2 join: if edt_guid was pre-registered as an owner-local RO reader
 * (arts_register_local_ro_reader), claim its reserved generation in-place,
 * seeding roOutstanding and (when the generation is not yet head) queuing it on
 * localDelayed for in-place delivery at promotion. Returns true if a
 * reservation was claimed; *on_head is set when the reserved generation is the
 * current head (caller reads db+1 immediately). Returns false (no-op) when the
 * reader was not pre-registered (the ordinary acquire path then applies).
 */
bool arts_claim_local_ro_reader(struct arts_db_s *db, struct arts_edt_s *edt,
                                arts_guid_t edt_guid, unsigned int slot,
                                arts_db_access_mode_t mode, bool *on_head);

/*
 * Late-binding helper for the writer's own full DB request. If the writer was
 * pre-registered (arts_register_remote_ew_writer), find its frontier; report
 * via on_head whether it is the current head and via deliver whether the DB
 * still needs to be shipped (and atomically claims delivery). Returns true if
 * a matching pre-registration was found (caller must not create a duplicate).
 */
bool arts_claim_remote_ew_writer(struct arts_db_s *db, unsigned int rank,
                                 arts_guid_t edt_guid, bool *on_head,
                                 bool *deliver);

/*
 * Retire one owner-local RO reader from the DB's current head generation.
 * Called on RO release for an owner-local DB with a CDAG frontier. A
 * reader-only generation has no writer update to progress it, so the last
 * reader to complete (roOutstanding -> 0) progresses the frontier, promoting
 * the next generation (typically the following EW writer). No-op if the head
 * is not a reader-only generation awaiting consumers.
 */
void arts_retire_local_ro_reader(struct arts_db_s *db);

/*
 * Pre-register a remote RO reader on the owner's frontier in CDAG order
 * (dual of arts_register_remote_ew_writer). Called from arts_add_dependence_ex
 * when a locally-owned DB is given an RO dependence whose destination EDT lives
 * on another node. The reader joins the current reader generation (a generation
 * with no exclusive writer) or a fresh one after a writer, so it is ordered
 * strictly after any earlier writer and before any later writer. At promotion
 * the reader is served a targeted snapshot (arts_remote_signal_edt_with_ptr).
 * Returns true if registered.
 */
bool arts_register_remote_ro_reader(struct arts_db_s *db, unsigned int rank,
                                    arts_guid_t edt_guid, unsigned int slot);

/*
 * Late-binding helper for the reader's own remote DB request. Returns true if a
 * matching pre-registration (same node + edt_guid) exists on any frontier
 * generation — in which case the owner must NOT ship the DB now (the targeted
 * snapshot at promotion serves it), avoiding a double-delivery / stale read.
 */
bool arts_remote_ro_reader_preregistered(struct arts_db_s *db,
                                         unsigned int rank,
                                         arts_guid_t edt_guid);
#ifdef __cplusplus
}
#endif

#endif /* ARTSDBLIST_H */
