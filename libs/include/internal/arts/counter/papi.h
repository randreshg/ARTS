/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ARTS_COUNTER_PAPI_H
#define ARTS_COUNTER_PAPI_H
#ifdef __cplusplus
extern "C" {
#endif

/*
 * PAPI hardware events, recorded into the PAPI_<SCOPE>_E<k> counters.
 *
 * The counter cfg decides which scopes are compiled in (a bare scope name,
 * PAPI_EDT=ONCE, enables all six of its slots); arts.cfg's papi_events decides
 * which hardware events the slots hold, in order, so changing the events needs
 * no rebuild.  Every thread that runs the runtime loop owns one PAPI EventSet
 * holding those events, started once and read (never stopped) at each scope
 * edge: a scope's slots accumulate the difference between its begin and end
 * reads.  Reduction and output are the ordinary counter pipeline's.
 *
 * Scopes:
 *   TOTAL_WORKER / TOTAL_PROGRESS — the thread's whole run loop, by role.
 *   EDT — the task body only.  arts_event_wait closes it for the duration of a
 *     blocking wait (beside TIME_EDT_EXEC), so the nested scheduling and the
 *     inner EDTs a parked EDT runs are not charged to it twice: worker
 *     TOTAL minus EDT is exactly the runtime's share.
 *   The rest are runtime regions.  A region re-entered on the same thread is
 *   measured once, by its outermost span.  DIFFERENT regions may nest (a CXL
 *   flush inside DB_RELEASE, a handler run while a send polls the fabric), and
 *   each is inclusive, so regions overlap and must not be summed.
 *
 * Without ARTS_USE_PAPI, or with a scope's counters OFF, its hooks compile to
 * nothing; with every PAPI counter OFF the runtime never initialises PAPI, so a
 * PAPI build still runs on a host whose perf_event_paranoid forbids counting.
 */

#include "arts/counter/Preamble.h"

/* Six slots: the two fixed-function counters (cycles, instructions) plus the
 * four programmable ones a hyperthread gets on Skylake-SP and Sapphire Rapids.
 * The counter.h slot list and the cfg scope expansion share this width. */
#define ARTS_PAPI_MAX_EVENTS 6

/* Order matches the scope blocks of ARTS_COUNTER_LIST: slot k of scope s is
 * counter PAPI_TOTAL_WORKER_E0 + s * ARTS_PAPI_MAX_EVENTS + k. */
typedef enum arts_papi_scope_t {
  ARTS_PAPI_TOTAL_WORKER = 0,
  ARTS_PAPI_TOTAL_PROGRESS,
  ARTS_PAPI_EDT,
  ARTS_PAPI_DB_ACQUIRE,
  ARTS_PAPI_DB_RELEASE,
  ARTS_PAPI_CXL_FLUSH_PRODUCER,
  ARTS_PAPI_CXL_FLUSH_CONSUMER,
  ARTS_PAPI_NET_SEND,
  ARTS_PAPI_MSG_HANDLER,
  ARTS_PAPI_NUM_SCOPES,
} arts_papi_scope_t;

#define ARTS_PAPI_ANY_ENABLED                                                  \
  (ENABLE_PAPI_TOTAL_WORKER_E0 || ENABLE_PAPI_TOTAL_PROGRESS_E0 ||             \
   ENABLE_PAPI_EDT_E0 || ENABLE_PAPI_DB_ACQUIRE_E0 ||                          \
   ENABLE_PAPI_DB_RELEASE_E0 || ENABLE_PAPI_CXL_FLUSH_PRODUCER_E0 ||           \
   ENABLE_PAPI_CXL_FLUSH_CONSUMER_E0 || ENABLE_PAPI_NET_SEND_E0 ||             \
   ENABLE_PAPI_MSG_HANDLER_E0)

#ifdef ARTS_USE_PAPI

/* Process-wide: parse `events` (comma-separated PAPI preset or native names),
 * initialise the library, and prove the set can be counted on this host.
 * Fatal on any failure.  Called once, before any runtime thread exists. */
void arts_papi_node_init(const char *events);
/* Per runtime thread: build and start this thread's EventSet and open its
 * TOTAL scope.  Stop closes the TOTAL scope and releases the EventSet; it must
 * run on the same thread, after the run loop has returned. */
void arts_papi_thread_start(void);
void arts_papi_thread_stop(void);
void arts_papi_scope_begin(arts_papi_scope_t scope);
void arts_papi_scope_end(arts_papi_scope_t scope);
/* The event names the slots hold, for the counter files' metadata. */
unsigned int arts_papi_event_count(void);
const char *arts_papi_event_name(unsigned int slot);

#define ARTS_PAPI_NODE_INIT(events)                                            \
  do {                                                                         \
    if (ARTS_PAPI_ANY_ENABLED) {                                               \
      arts_papi_node_init(events);                                             \
    }                                                                          \
  } while (0)
#define ARTS_PAPI_THREAD_START()                                               \
  do {                                                                         \
    if (ARTS_PAPI_ANY_ENABLED) {                                               \
      arts_papi_thread_start();                                                \
    }                                                                          \
  } while (0)
#define ARTS_PAPI_THREAD_STOP()                                                \
  do {                                                                         \
    if (ARTS_PAPI_ANY_ENABLED) {                                               \
      arts_papi_thread_stop();                                                 \
    }                                                                          \
  } while (0)
#define ARTS_PAPI_BEGIN(scope)                                                 \
  do {                                                                         \
    if (ENABLE_PAPI_##scope##_E0) {                                            \
      arts_papi_scope_begin(ARTS_PAPI_##scope);                                \
    }                                                                          \
  } while (0)
#define ARTS_PAPI_END(scope)                                                   \
  do {                                                                         \
    if (ENABLE_PAPI_##scope##_E0) {                                            \
      arts_papi_scope_end(ARTS_PAPI_##scope);                                  \
    }                                                                          \
  } while (0)

#else /* !ARTS_USE_PAPI */

#define ARTS_PAPI_NODE_INIT(events) ((void)(events))
#define ARTS_PAPI_THREAD_START() ((void)0)
#define ARTS_PAPI_THREAD_STOP() ((void)0)
#define ARTS_PAPI_BEGIN(scope) ((void)0)
#define ARTS_PAPI_END(scope) ((void)0)

#endif /* ARTS_USE_PAPI */

#ifdef __cplusplus
}
#endif
#endif /* ARTS_COUNTER_PAPI_H */
