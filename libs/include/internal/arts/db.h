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
#ifndef ARTS_MEMORY_DB_H
#define ARTS_MEMORY_DB_H
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
 * ARTS_DB(0), ARTS_DB_PIN(1), ARTS_DB_CXL(2), ARTS_DB_GPU(3),
 * ARTS_DB_GPU_PIN(4). */
#define ARTS_DB_TYPE_NAME                                                      \
  const char *const arts_db_type_name[] = {"ARTS_DB", "ARTS_DB_PIN",           \
                                           "ARTS_DB_CXL", "ARTS_DB_GPU",       \
                                           "ARTS_DB_GPU_PIN"}

#define GET_DB_TYPE_NAME(x) arts_db_type_name[x]

extern const char *const arts_db_type_name[];

void arts_db_acquire_all(struct arts_edt_s *edt);

/* Whether a dep's acquisition is ordered by the EDT's serialized (RW-cursor)
 * walk: the arm's answer for its mode, with the non-coherent subtypes that
 * bypass the walk excluded.  A pure function of the slot. */
bool arts_dep_is_serialized(arts_edt_dep_t *depv, uint32_t i);

/* Order one EDT's dependence vector and classify it, once, before any of it
 * fires.  `sorted` receives the visit order: by GUID (same-block deps adjacent,
 * so a serialized walk takes blocks in one global order), strongest mode first
 * within a block, ties keeping slot order.  `depv[].alias` receives the
 * classification: the first slot of each block's group owns that block's single
 * acquisition, every later slot naming it is an alias on that acquisition.
 * Slots that acquire nothing — NULL GUID, DB_MODE_NULL, a non-DB kind, an
 * already-resolved slot — are neither, and are left untouched.
 *
 * A pure function of depv, with no runtime state behind it: the classification
 * every later frame reads is decided here and nowhere else. */
void arts_dep_sort_and_classify(arts_edt_dep_t *depv, uint32_t depc,
                                uint32_t *sorted);

/* Advance one EDT's serialized-acquire cursor past `slot`.  It is
 * position-idempotent: it moves only while the cursor still points at `slot`,
 * so a wake for a slot the walk has already passed changes nothing.  It does
 * NOT touch the acquire count, and it fires no dependence of its own: it either
 * flags the walk running in this execution context to continue, or appends the
 * EDT to the flat resume worklist below — so the calling frame grows the stack
 * by nothing.
 *
 * Call it only once the slot's payload is published.  Passing the cursor is
 * what entitles the next frame to fire the following slot, and that frame may
 * read this slot's payload as final; a cursor ahead of an unpublished payload
 * is therefore a state no reader may observe. */
void arts_db_rw_secure(struct arts_edt_s *edt, unsigned int slot);

/* Run the serialized-acquire walk of every EDT on the flat resume worklist, in
 * one loop at this level.  A no-op while a walk is already in progress in this
 * execution context, or while a drain is running: the outermost frame owns the
 * drain, which is what keeps a wake enqueued deep in a nest from stacking a
 * second walk on the first.  Call it where the frame is free of any EDT's
 * acquire state, after the account that may schedule the woken EDT. */
void arts_db_drain_resume_list(void);

/* OOO_DB_ACQUIRE replay (table entry): re-dispatch the ONE deferred local dep
 * through the per-dep 3-way (subtype-aware — ARTS_DB → arts_handler_db_acquire,
 * PIN/GPU/CXL → pinned ptr path, still-absent → re-defer).  item = the
 * just-installed db_s (ignored; the 3-way re-looks-it-up under the drain's
 * pinned ref); args = arts_ooo_args_db_acquire_s {edt, db_guid, slot}.  NOT the
 * coherent handler directly — that would mishandle non-coherent subtypes. */
void arts_db_acquire_replay_dep(void *item, void *args);
void release_dbs(unsigned int depc, arts_edt_dep_t *depv, bool gpu);
void arts_release_created_dbs(void);
void prep_dbs(unsigned int depc, arts_edt_dep_t *depv, bool gpu);

void arts_db_free(void *ptr);

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

/* ── Coherent storage kinds ────────────────────────────────────────────────
 *
 * A "coherent" DB is one whose access goes through the DB-coherence protocol.
 * ARTS_DB is coherent by payload: its bytes live in a per-rank buffer that the
 * protocol moves.  ARTS_DB_CXL is coherent by PERMISSION ONLY: its bytes never
 * move — they sit in the shared CXL window for the whole lifetime of the block
 * — and what the protocol arbitrates is the right to read or write them,
 * together with the cache flush/invalidate that makes a hand-over visible.
 *
 * The pinned kinds (PIN / GPU / GPU_PIN) carry no DB-level coherence at all;
 * they live on their creator rank and rely on hardware coherence plus the
 * program's own event ordering.
 *
 * ARTS_DB_CXL counts as coherent only where a protocol exists that can serve
 * it, which is EXCL and nothing else (see the ARTS_USE_CXL x protocol guard in
 * the top-level CMakeLists for why).  That distinction matters inside this
 * tree and not only at configure time: the benchmark variant libraries build
 * these same sources against VAL and INV while ARTS_USE_CXL is on, and there
 * the CXL storage kind resolves as an ordinary creator-local pinned block — an
 * ordinary GUID, a route entry, descriptor and inline payload from the window,
 * released as a pinned subtype — rather than entering a coherence arm with no
 * way to grant it. */
#if defined(ARTS_USE_CXL) && defined(ARTS_PROTOCOL_EXCL)
#define ARTS_CXL_COHERENT 1
#endif

static inline bool arts_db_type_is_coherent(arts_db_types_t t) {
#ifdef ARTS_CXL_COHERENT
  return t == ARTS_DB || t == ARTS_DB_CXL;
#else
  return t == ARTS_DB;
#endif
}

/* True for a coherent kind whose payload the protocol must MOVE.  CXL blocks
 * answer false: there is exactly one copy of the bytes and every rank already
 * addresses it, so no buffer is ever installed, no payload rides a grant, and
 * no publish travels with a release. */
static inline bool arts_db_type_has_buffer(arts_db_types_t t) {
  return t == ARTS_DB;
}

#ifdef ARTS_CXL_COHERENT
/* Writer-side visibility edge: flush this block's modified CXL cache lines out
 * to the shared window.  Runs BEFORE the release reaches the home, so that a
 * reader the home grants afterwards cannot observe pre-release bytes. */
void arts_cxl_producer_flush(arts_guid_t guid);
/* Reader-side visibility edge: invalidate this rank's cached lines for the
 * block.  Runs AFTER the grant arrives and BEFORE any local EDT reads, so a
 * line cached under an earlier grant cannot survive a writer's turn. */
void arts_cxl_consumer_flush(arts_guid_t guid);
#endif

#ifdef __cplusplus
}
#endif
#endif /* artsDBFUNCTIONS_H */
