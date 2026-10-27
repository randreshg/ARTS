/* SPDX-License-Identifier: Apache-2.0
 *
 * The acquire census: every acquisition an arm takes is counted once, on the
 * acquiring rank, by where the permission and the copy it needs are.  It is
 * NUM_DB_ACQUIRE_LOCAL_HIT when this rank holds them -- the write right for a
 * write, a valid copy it may read or a right covering the read for a read --
 * and NUM_DB_ACQUIRE_REMOTE when they had to come from another rank.
 * Whatever messages the acquisition caused, and whatever it waited behind,
 * decide nothing.
 *
 * The state at the acquisition's decision classifies it.  One the arm serves
 * at its decision is counted where it is served.  One it parks is counted by
 * the serve that claims its slot.  A waiter whose park found this rank
 * already holding what it needs -- a write right installed but not yet
 * runnable -- carries that verdict and is a local hit whichever path serves
 * it.  Any other waiter is classified by where what it waits for comes from,
 * which the serve path knows: a path that installs a permission or a copy
 * transferred from another rank opens a remote serve section around the
 * waiters it serves, a path that serves them from what this rank already
 * holds (a phase it flips on its own rank included) a local one, and the
 * waiters of a block's first publication count from its publisher's rank
 * whichever drain wakes them.  Under the voluntary-return release policy the
 * write right belongs to the block's home, so every acquisition on the home
 * rank is a local hit.
 */
#ifndef ARTS_COHERENCE_CENSUS_H
#define ARTS_COHERENCE_CENSUS_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "arts.h" /* arts_guid_get_rank */
#include "arts/counter/Preamble.h"
#include "arts/system/identity.h" /* arts_global_rank_id */

#define ARTS_DB_CENSUS                                                         \
  (ENABLE_NUM_DB_ACQUIRE_LOCAL_HIT || ENABLE_NUM_DB_ACQUIRE_REMOTE)

/* The kind of serve section a thread is in. */
enum {
  ARTS_DB_SERVE_NONE = 0,
  ARTS_DB_SERVE_LOCAL,
  ARTS_DB_SERVE_REMOTE,
};

#if ARTS_DB_CENSUS
extern ARTS_THREAD_LOCAL uint8_t arts_db_census_serve;
/* Set by a write decision that refused a turn on a right this rank holds
 * (installed, its confirmation still out); taken by the park that follows. */
extern ARTS_THREAD_LOCAL bool arts_db_census_held;
/* A waiter's verdict at its park, written before the operation that links
 * it and read by the serve that takes it. */
#define ARTS_DB_CENSUS_HELD(w) ((w)->census_held)
#define ARTS_DB_CENSUS_SET_HELD(w, held) ((w)->census_held = (held))
#else
#define ARTS_DB_CENSUS_HELD(w) false
#define ARTS_DB_CENSUS_SET_HELD(w, held) ((void)(held))
#endif

static inline void arts_db_census_note_held(bool held) {
#if ARTS_DB_CENSUS
  arts_db_census_held = held;
#else
  (void)held;
#endif
}

static inline bool arts_db_census_take_held(void) {
#if ARTS_DB_CENSUS
  bool held = arts_db_census_held;
  arts_db_census_held = false;
  return held;
#else
  return false;
#endif
}

/* Opens a serve section; returns the enclosing one, which
 * arts_db_census_served restores. */
static inline uint8_t arts_db_census_serving(bool remote) {
#if ARTS_DB_CENSUS
  uint8_t outer = arts_db_census_serve;
  arts_db_census_serve = remote ? ARTS_DB_SERVE_REMOTE : ARTS_DB_SERVE_LOCAL;
  return outer;
#else
  (void)remote;
  return ARTS_DB_SERVE_NONE;
#endif
}

/* Around the wake of one parked waiter: one whose park found this rank
 * holding what it needs is a local hit, whatever section the serve opened. */
static inline uint8_t arts_db_census_serving_waiter(bool held) {
#if ARTS_DB_CENSUS
  uint8_t outer = arts_db_census_serve;
  if (held) {
    arts_db_census_serve = ARTS_DB_SERVE_LOCAL;
  }
  return outer;
#else
  (void)held;
  return ARTS_DB_SERVE_NONE;
#endif
}

/* A serve section for what `rank` sent this rank. */
static inline uint8_t arts_db_census_serving_from(unsigned int rank) {
  return arts_db_census_serving(rank != arts_global_rank_id);
}

/* Around an arm's decision: a decision is never part of the serve that
 * reached it, so a serve it makes declares its own kind. */
static inline uint8_t arts_db_census_deciding(void) {
#if ARTS_DB_CENSUS
  uint8_t outer = arts_db_census_serve;
  arts_db_census_serve = ARTS_DB_SERVE_NONE;
  return outer;
#else
  return ARTS_DB_SERVE_NONE;
#endif
}

static inline void arts_db_census_served(uint8_t outer) {
#if ARTS_DB_CENSUS
  arts_db_census_serve = outer;
#else
  (void)outer;
#endif
}

/* Whether this thread's serve section is a remote one. */
static inline bool arts_db_census_serving_remote(void) {
#if ARTS_DB_CENSUS
  return arts_db_census_serve == ARTS_DB_SERVE_REMOTE;
#else
  return false;
#endif
}

/* A parked acquisition of db_guid whose slot this thread's serve resolved. */
static inline void arts_db_census_count_serve(arts_guid_t db_guid) {
#if ARTS_DB_CENSUS
  assert(arts_db_census_serve != ARTS_DB_SERVE_NONE &&
         "a serve path claimed a parked acquisition without its kind");
#ifdef ARTS_RELEASE_PURGE
  if (arts_guid_get_rank(db_guid) == arts_global_rank_id) {
    INCREMENT_NUM_DB_ACQUIRE_LOCAL_HIT_BY(1);
    return;
  }
#else
  (void)db_guid;
#endif
  if (arts_db_census_serve == ARTS_DB_SERVE_REMOTE) {
    INCREMENT_NUM_DB_ACQUIRE_REMOTE_BY(1);
  } else {
    INCREMENT_NUM_DB_ACQUIRE_LOCAL_HIT_BY(1);
  }
#else
  (void)db_guid;
#endif
}

#endif /* ARTS_COHERENCE_CENSUS_H */
