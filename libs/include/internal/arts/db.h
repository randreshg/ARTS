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
#ifndef ARTS_DB_H
#define ARTS_DB_H
#ifdef __cplusplus
extern "C" {
#endif

#include "arts/runtime_types.h"

/* Indexed by arts_guid_kind_t value (RESERVED=0, DB=1, EVENT=2, EDT=3).
 * Array size == ARTS_GUID_LAST (4); slot 0 is the reserved/NULL sentinel. */
#define ARTS_TYPE_NAME                                                         \
  const char *const arts_type_name[] = {"ARTS_GUID_RESERVED", "ARTS_GUID_DB",  \
                                        "ARTS_GUID_EVENT", "ARTS_GUID_EDT"}

#define GET_TYPE_NAME(x) arts_type_name[x]

extern const char *const arts_type_name[];

/* Internal-only access modes used by GPU dispatch, in the reserved range
 * arts.h anchors with DB_MODE_INTERNAL_BASE.  Each constant is cast to the
 * public type so it can be stored in arts_edt_dep_t.mode and compared
 * against it same-type; a cast integer constant expression remains valid in
 * case labels.  A GPU-less build has no producer or consumer of these, so
 * they do not exist there at all. */
#ifdef ARTS_USE_GPU
/** GPU LC -> CPU synchronous copy. */
#define DB_MODE_LC_SYNC ((arts_db_access_mode_t)(DB_MODE_INTERNAL_BASE + 0))
/** Allocate on GPU, no host -> GPU copy. */
#define DB_MODE_LC_NO_COPY ((arts_db_access_mode_t)(DB_MODE_INTERNAL_BASE + 1))
/** GPU zero-initialization. */
#define DB_MODE_MEMSET ((arts_db_access_mode_t)(DB_MODE_INTERNAL_BASE + 2))
#endif

#define DB_MODE_NAME                                                           \
  const char *const db_mode_name[] = {"DB_MODE_NULL", "DB_MODE_RO",            \
                                      "DB_MODE_RW"};                           \
  const char *const db_mode_internal_name[] = {                                \
      "DB_MODE_LC_SYNC", "DB_MODE_LC_NO_COPY", "DB_MODE_MEMSET"}

#define GET_DB_MODE_NAME(x)                                                    \
  ((x) >= DB_MODE_INTERNAL_BASE                                                \
       ? db_mode_internal_name[(x) - DB_MODE_INTERNAL_BASE]                    \
       : db_mode_name[x])

extern const char *const db_mode_name[];
extern const char *const db_mode_internal_name[];

/* Order MUST match the arts_db_types_t enum in arts.h:
 * ARTS_DB(0), ARTS_DB_GPU(1), ARTS_DB_GPU_PIN(2). */
#define ARTS_DB_TYPE_NAME                                                      \
  const char *const arts_db_type_name[] = {"ARTS_DB", "ARTS_DB_GPU",           \
                                           "ARTS_DB_GPU_PIN"}

#define GET_DB_TYPE_NAME(x) arts_db_type_name[x]

extern const char *const arts_db_type_name[];

/* The acquire phase's per-slot state, one record per dependence slot.  The
 * frames of one EDT's acquire phase (its walk, the resolution of a parked
 * slot, an alias fill, an out-of-order replay) meet here and never in the
 * public dependence vector, which no frame writes until the phase ends: the
 * frame whose decrement of acquire_remaining reaches zero copies `ptr` into
 * depv[i].ptr for every slot that resolved to a payload before it schedules
 * the EDT.  A slot the acquire phase does not resolve keeps what its satisfy
 * wrote (guid, mode, NULL ptr).
 *
 * Every owner slot the phase hands to an arm is resolved by exactly one
 * frame: the frame that fired it, or the one frame a plane's take hands its
 * waiter to.  An alias is filled exactly once, by its owner's resolver.  So
 * the plain members have one writer each, and every reader is ordered after
 * it by acquire_remaining's RMW chain (the writer's decrement, then the zero
 * edge's) or by the publication of the slot to its resolver.
 *
 * Zero-filled storage is the unresolved state of every member. */
struct arts_dep_acq_s {
  /* The slot's payload: written by the slot's one resolver before its
   * account; read by the frame whose account reaches zero. */
  void *ptr;
  /* The descriptor pin the slot holds from its firing to its release.  An
   * owner's is stored by the frame that fires it, before the arm's handler
   * can publish the slot; an alias's by its owner's resolver.  Reaches the
   * resolver through the handler's publication of the waiter, the release
   * through acquire_remaining's zero edge and the hand-off to the run. */
  arts_shared_ptr_t pin;
  /* The storage kind: written before the handler, or by the alias fill; the
   * release routes on it. */
  arts_db_types_t subtype;
  /* An alias on another slot's acquisition (arts_dep_sort_and_classify),
   * decided before anything fires. */
  bool alias;
  /* Written by Debug builds only: the slot's one resolver exchanges it to 1,
   * and a second resolution aborts.  Relaxed: only its own value. */
  _Atomic(uint8_t) resolved;
};

/* The storage kind slot `slot` of `edt` resolved to. */
static inline arts_db_types_t arts_dep_subtype(const struct arts_edt_s *edt,
                                               uint32_t slot) {
  return edt->dep_acq[slot].subtype;
}

/* Every resolution's first act, before the record, the pin, the alias bit or
 * depv is read: a Debug build aborts on a slot resolved after its EDT's
 * acquire phase ended or resolved twice.  A Release build checks nothing. */
#ifndef NDEBUG
void arts_dep_acq_resolve_once(struct arts_edt_s *edt, uint32_t slot);
#else
static inline void arts_dep_acq_resolve_once(struct arts_edt_s *edt,
                                             uint32_t slot) {
  (void)edt;
  (void)slot;
}
#endif

/* A dependence that the frame which fired it resolved on the spot (a local
 * hit).  Such a slot never parked, so this frame is its one resolver. */
static inline void arts_db_acquire_local_hit(struct arts_edt_s *edt,
                                             uint32_t slot, void *ptr) {
  arts_dep_acq_resolve_once(edt, slot);
  edt->dep_acq[slot].ptr = ptr;
}

/* Begin one EDT's acquire phase: allocate its acquire record and visit order
 * and classify its dependence vector (arts_dep_sort_and_classify), once.  A
 * no-op for an EDT whose phase has begun or that has no dependence. */
void arts_db_acquire_prepare(struct arts_edt_s *edt);

void arts_db_acquire_all(struct arts_edt_s *edt);

/* Whether a dep's acquisition is ordered by the EDT's serialized (RW-cursor)
 * walk: the arm's answer for its mode, whatever the block's storage kind (no
 * arm serializes a device-side internal mode).  A pure function of the
 * slot. */
bool arts_dep_is_serialized(const arts_edt_dep_t *depv, uint32_t i);

/* Order one EDT's dependence vector and classify it, once, before any of it
 * fires.  `sorted` receives the visit order: by GUID (same-block deps adjacent,
 * so a serialized walk takes blocks in one global order), strongest mode first
 * within a block, ties keeping slot order.  `acq[].alias` receives the
 * classification: the first slot of each block's group owns that block's single
 * acquisition, every later slot naming it is an alias on that acquisition.
 * Slots that acquire nothing — NULL GUID, DB_MODE_NULL, a non-DB kind, a slot
 * already carrying a pointer — are neither, and are left untouched.
 *
 * A pure function of depv, with no runtime state behind it: the classification
 * every later frame reads is decided here and nowhere else. */
void arts_dep_sort_and_classify(const arts_edt_dep_t *depv,
                                struct arts_dep_acq_s *acq, uint32_t depc,
                                uint32_t *sorted);

/* Advance one EDT's serialized-acquire cursor past `slot`, which the cursor
 * must be at: a serialized slot is fired only at the cursor, the cursor moves
 * only past a secured slot, and the slot has one resolver.  It fires no
 * dependence of its own: it either flags the walk running in this execution
 * context to continue, or appends the EDT to the flat resume worklist below —
 * so the calling frame grows the stack by nothing.
 *
 * Returns true when it appended the EDT.  The caller's account for `slot` then
 * belongs to the worklist entry: the drain issues it after the walk, so the
 * walk runs while the EDT still owes an account and nothing can schedule it.
 * On false the caller accounts the slot itself.
 *
 * Call it only once the slot's payload is published.  Passing the cursor is
 * what entitles the next frame to fire the following slot, and that frame may
 * read this slot's payload as final; a cursor ahead of an unpublished payload
 * is therefore a state no reader may observe. */
bool arts_db_rw_secure(struct arts_edt_s *edt, unsigned int slot);

/* Run the serialized-acquire walk of every EDT on the flat resume worklist, in
 * one loop at this level, each followed by the account its entry carries.  A
 * no-op while a walk is already in progress in this execution context, or
 * while a drain is running: the outermost frame owns the drain, which is what
 * keeps a wake enqueued deep in a nest from stacking a second walk on the
 * first. */
void arts_db_drain_resume_list(void);

/* Free the calling thread's resume worklist.  Called once the thread has left
 * its scheduler loop, when nothing can append to the list any more. */
void arts_db_resume_list_free(void);

/* OOO_DB_ACQUIRE replay (table entry): re-dispatch the ONE deferred local dep
 * through the per-dep 3-way (subtype-aware — ARTS_DB → arts_handler_db_acquire,
 * a GPU kind → its inline ptr path, still-absent → re-defer).  item = the
 * just-installed db_s (the 3-way re-looks it up; the OoO dispatcher's pin on
 * it keeps the descriptor alive across the call); args =
 * arts_ooo_args_db_acquire_s {edt, db_guid, slot}.  NOT the coherent handler
 * directly — that would mishandle non-coherent subtypes. */
void arts_db_acquire_replay_dep(void *item, void *args);
void release_dbs(struct arts_edt_s *edt, bool gpu);
void arts_release_created_dbs(void);
void prep_dbs(struct arts_edt_s *edt, bool gpu);

void arts_db_free(void *ptr);

/* Free a coherent descriptor's storage.  Under ARTS_DEBUG_DB_QUARANTINE it is
 * filled and held back instead, and checked when it leaves the quarantine and
 * at teardown that nothing wrote to it. */
void arts_db_descriptor_free(struct arts_db_s *db);
#ifdef ARTS_DEBUG_DB_QUARANTINE
/* Check and free every descriptor still held back.  Called once every other
 * thread that can free a descriptor has been joined; skipped, with a warning,
 * when a thread the shutdown gave up joining may still free. */
void arts_db_descriptor_quarantine_drain(void);
#endif

/* Install a descriptor a create built on this rank, named by the control
 * block arts_route_table_make_handle made for it.  On success the slot takes
 * over the ref `cb` carries and, when this rank is not the block's home, the
 * block is announced to its home.  On a loss nothing changes: the ref is
 * still the caller's.  The OOO_DB_CREATE body's local shape. */
bool arts_db_create_install_local(arts_shared_ptr_t cb);

/* User-visible payload pointer for a DB: a coherent DB's installed coherence
 * buffer, and the inline payload after the wrapping struct for every other
 * subtype.  Defined in the per-protocol-compiled runtime so the inline offset
 * always matches that protocol's actual struct arts_db_s size — a caller
 * linked across protocol variants cannot compute it from its own view of the
 * protocol-conditional layout.  NULL-safe; NULL if no buffer is yet installed
 * on a coherent DB. */
void *arts_db_user_ptr(struct arts_db_s *db);

/* Internal pre/post-yield helpers used when an EDT yields (e.g.
 * arts_event_wait). Not part of the public ARTS API. */
void arts_wait_release_dbs(void);
void arts_wait_reacquire_dbs(void);

#ifdef __cplusplus
}
#endif
#endif /* artsDBFUNCTIONS_H */
