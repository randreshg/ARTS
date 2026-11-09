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
#ifndef ARTS_SYSTEM_SHUTDOWN_H
#define ARTS_SYSTEM_SHUTDOWN_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "arts/utils/stdatomic_compat.h"

/* The bound, in seconds, a rank's end may take once it began: one value per
 * tree, which the build defines for every unit and the test registrations
 * read. */
#ifndef ARTS_EXIT_WATCHDOG_S
#error "ARTS_EXIT_WATCHDOG_S is undefined: the build defines it for every unit"
#endif

/* What made this rank begin its end.  An origin fits in three bits, and 0
 * names none. */
enum arts_exit_origin {
  ARTS_EXIT_APP = 1,       /* the program ends the run */
  ARTS_EXIT_SIGNAL,        /* a terminating signal reached the process */
  ARTS_EXIT_FAULT,         /* a thread of the process faulted */
  ARTS_EXIT_RUNTIME_ERROR, /* the runtime met a fatal error */
  ARTS_EXIT_WIRE,          /* a peer's SHUTDOWN, taken in by a progress loop */
  ARTS_EXIT_SENTINEL,      /* a peer's closed connection, seen by its probe */
  ARTS_EXIT_BACKPRESSURE,  /* either, seen where nothing may be dispatched */
};

/* Whether the end begins on this rank, so it posts SHUTDOWN to its peers.  A
 * rank that learned of another rank's end posts nothing: every rank that
 * originates one posts, and every other rank learns of it from a post or
 * from a closed connection. */
static inline bool arts_exit_origin_originates(enum arts_exit_origin o) {
  switch (o) {
  case ARTS_EXIT_APP:
  case ARTS_EXIT_SIGNAL:
  case ARTS_EXIT_FAULT:
  case ARTS_EXIT_RUNTIME_ERROR:
    return true;
  case ARTS_EXIT_WIRE:
  case ARTS_EXIT_SENTINEL:
  case ARTS_EXIT_BACKPRESSURE:
    return false;
  }
  return false;
}

static inline const char *arts_exit_origin_name(enum arts_exit_origin o) {
  switch (o) {
  case ARTS_EXIT_APP:
    return "APP";
  case ARTS_EXIT_SIGNAL:
    return "SIGNAL";
  case ARTS_EXIT_FAULT:
    return "FAULT";
  case ARTS_EXIT_RUNTIME_ERROR:
    return "RUNTIME_ERROR";
  case ARTS_EXIT_WIRE:
    return "WIRE";
  case ARTS_EXIT_SENTINEL:
    return "SENTINEL";
  case ARTS_EXIT_BACKPRESSURE:
    return "BACKPRESSURE";
  }
  return "NONE";
}

/* What a rank's counter record says of its run and its end.  run_id is drawn
 * by rank 0 and learned by every other rank from rank 0's address frame, and
 * is 0 on a rank that has not learned it; it is a value of its own that
 * publishes nothing, stored and loaded relaxed, so a record written on any
 * thread reads either 0 or the run's id.  origin and the two t0 stamps are
 * written once, by the thread that takes the rank's shutdown point, and read
 * only by that thread.  rc is the rc that thread took the point with, the
 * program's own status folded in once the take has read it, or the rc the end
 * of a process that is no rank ends with; it is a value of its own, stored and
 * read relaxed, so any thread, a signal handler included, may read it once the
 * end began. */
struct arts_exit_meta_s {
  _Atomic(uint64_t) run_id;
  uint64_t t0_mono_ns;     /* CLOCK_MONOTONIC (arts_get_time_stamp) */
  uint64_t t0_realtime_ns; /* CLOCK_REALTIME */
  enum arts_exit_origin origin;
  _Atomic(int) rc;
};

extern struct arts_exit_meta_s arts_exit_meta;

/* The rank's end.  The first thread to take the rank's shutdown point owns
 * it: from then on the rank sends nothing but one best-effort SHUTDOWN post,
 * writes its counter record and ends the process with exit_group, waiting for
 * no other rank and no other thread.  Every later call, on any thread, is a
 * late entry: its rc is folded into the rank's status and its thread parks
 * for good, except on the owner's own thread, where the process ends at once.
 * An end that takes the point before this process became a rank
 * (arts_exit_publish_rank) has late entries too; it posts nothing, writes no
 * record, and ends the process with the highest of its rc and theirs (a
 * FAULT raises its signal).  In the process arts_rt started (not a child it
 * forked), every step from the shutdown point to the process's end is
 * bounded by the watchdog: ARTS_EXIT_WATCHDOG_S seconds, longer by the
 * counter record's estimated write time once the record begins, plus at
 * most a second while the watchdog's line waits for stderr. */
#ifdef __cplusplus
[[noreturn]]
#else
_Noreturn
#endif
void arts_exit(int rc, enum arts_exit_origin origin);

/* Takes the shutdown point for an entry that may not end the process on its
 * own stack yet: true when this call took it (it armed the end and, on a
 * rank, closed the send gate), false when the point was already taken, and
 * the caller returns to its loop.  A WIRE taker finishes the burst it reaped
 * and resumes at its progress loop's top (arts_exit_poll); every other taker
 * calls arts_exit_resume at once.  Called on a rank, or, before the process
 * is one, on the thread that would publish the rank; such a take's end is an
 * unranked end (arts_exit). */
bool arts_exit_take(int rc, enum arts_exit_origin origin);

/* The rest of the end, on the thread that took the shutdown point: a rank's
 * end, or the unranked end a take made before the rank was known. */
#ifdef __cplusplus
[[noreturn]]
#else
_Noreturn
#endif
void arts_exit_resume(void);

/* A progress loop's top: resumes this thread's end when it took the shutdown
 * point at a reap, and in a build with the injection hooks takes it for a
 * rendezvous cut whose count completed. */
void arts_exit_poll(void);

/* Whether the rank's shutdown point was taken; acquire, so what its taker
 * wrote before the take is visible after a true. */
bool arts_shutdown_taken(void);

/* Whether this process's end began: its shutdown point was taken, or the
 * watchdog holds a deadline (the spawner's end arms it without taking the
 * point).  Async-signal-safe. */
bool arts_exit_began(void);

/* Whether this process became a rank (arts_exit_publish_rank); acquire, so
 * what the publication covers is visible after a true. */
bool arts_exit_rank_known(void);

/* arts_rt's first act: names this process as the one whose end arts_exit
 * runs, and makes two disarmed timers on one signal, SIGRTMIN+1, with the
 * one SA_SIGINFO handler they share: the watchdog that bounds that end and
 * the startup timer (arts_exit_startup_arm).  The signal is unblocked on the
 * calling thread, from which every thread the process starts inherits its
 * mask.  Called before any other thread exists.  A process that forks keeps
 * the name, so its child ends with its rc alone.  Refuses (ARTS_ERROR) a
 * process whose timers cannot be made. */
void arts_exit_process_init(void);

/* This process is the spawner: every watchdog line it prints names the
 * spawner, not a rank.  Called first thing by the spawner. */
void arts_exit_mark_spawner(void);

/* The spawner's end begins, to end with rc: an end that has not finished
 * ARTS_EXIT_WATCHDOG_S seconds from now is ended by the watchdog (within a
 * second more, while its line waits for stderr), with rc, or 71 when rc is
 * 0.  A no-op in a process that never ran
 * arts_exit_process_init.  Async-signal-safe; leaves errno as it found it. */
void arts_exit_watchdog_arm(int rc);

/* The startup timer, beside the watchdog on its signal: armed once, on the
 * main thread before any runtime thread exists, to fire at `at_ns`
 * (CLOCK_MONOTONIC); the arming publishes to the timer's handler what the
 * caller wrote before it.  A fire before the disarm takes the shutdown point
 * when it is free and the watchdog holds no deadline, writes
 * arts_startup_expired_line's line if stderr takes it within a second and
 * ends the process with status 75; any other fire does nothing.  Both are
 * no-ops in a process that never ran arts_exit_process_init; the disarm is
 * idempotent. */
void arts_exit_startup_arm(uint64_t at_ns);
void arts_exit_startup_disarm(void);

/* This process became a rank: its id and the rank count are final, and
 * `counter_folder` (copied) is where its counter record goes.  Called once,
 * on the main thread, before any runtime thread exists. */
void arts_exit_publish_rank(const char *counter_folder);

/* The rank's node is ready: its thread table, its counter rows, its pool and
 * the start stamp are written.  Called once, by thread 0, before any EDT of
 * the rank can run. */
void arts_exit_publish_node_ready(void);

/* Whether the node was published ready; acquire, so what the publication
 * covers is visible after a true. */
bool arts_exit_node_ready(void);

/* The copy of the counter folder the rank was published with; read only after
 * the shutdown point was taken. */
const char *arts_exit_counter_folder(void);

/* The rank's status as its end stands: the owner's rc, every late entry's rc
 * and the program's own status, whichever is highest.  Read only after the
 * shutdown point was taken. */
int arts_exit_status(void);

#ifdef ARTS_DEBUG_INJECTION
/* A rendezvous cut's completed count ends the run (origin APP): it may fire
 * inside a registration or an arrival, so it only raises a flag that the
 * next progress loop's top takes the shutdown point on. */
void arts_exit_cut_raise(void);
#endif

/* In a build on the vendored fake CXL library, runs that library's
 * finalizers once per process, as a normal exit would: the process that
 * created the region unlinks it there.  A no-op in every other build. */
void arts_fake_cxl_fini_replay(void);

#ifdef __cplusplus
}
#endif
#endif
