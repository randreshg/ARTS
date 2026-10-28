/* SPDX-License-Identifier: Apache-2.0 */
/*
 * PAPI hardware events into the PAPI_* counters (see arts/counter/papi.h for
 * the scope model).  Compiled only under ARTS_USE_PAPI.
 *
 * One EventSet per runtime thread, started once and never stopped mid-run:
 * scope edges are PAPI_read calls, so nested and overlapping scopes cost one
 * read each and never disturb one another.  The event list is fixed for the
 * process (every thread counts the same events in the same slot order), which
 * is what lets the counter pipeline reduce slot k across threads and ranks.
 * Multiplexing is never enabled: a multiplexed read is an extrapolation, and
 * the difference of two extrapolations around a short EDT is noise.
 */
#include "arts/counter/papi.h"

#include <papi.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arts/counter/counter.h"
#include "arts/runtime_state.h"
#include "arts/system/print.h"
#include "arts/system/topology.h"

/* Slot k of scope s lives at a fixed counter index, which only holds while the
 * scope blocks of ARTS_COUNTER_LIST stay contiguous and in enum order. */
_Static_assert(PAPI_TOTAL_WORKER_E5 == PAPI_TOTAL_WORKER_E0 + 5,
               "PAPI slots of a scope must be contiguous");
_Static_assert(PAPI_MSG_HANDLER_E0 ==
                   PAPI_TOTAL_WORKER_E0 +
                       ARTS_PAPI_MSG_HANDLER * ARTS_PAPI_MAX_EVENTS,
               "PAPI scope blocks in counter.h must follow arts_papi_scope_t");

#define ARTS_PAPI_DEFAULT_EVENTS                                               \
  "PAPI_TOT_CYC,PAPI_TOT_INS,PAPI_L1_DCM,PAPI_L2_TCM,PAPI_L3_TCM,PAPI_TLB_DM"
#define ARTS_PAPI_NAME_MAX 128

static char papi_names[ARTS_PAPI_MAX_EVENTS][ARTS_PAPI_NAME_MAX];
static int papi_codes[ARTS_PAPI_MAX_EVENTS];
static unsigned int papi_count;

static ARTS_THREAD_LOCAL int papi_eventset = PAPI_NULL;
static ARTS_THREAD_LOCAL bool papi_running;
static ARTS_THREAD_LOCAL arts_papi_scope_t papi_total_scope;
static ARTS_THREAD_LOCAL unsigned int papi_depth[ARTS_PAPI_NUM_SCOPES];
static ARTS_THREAD_LOCAL long long papi_start[ARTS_PAPI_NUM_SCOPES]
                                             [ARTS_PAPI_MAX_EVENTS];

static unsigned long papi_thread_id(void) {
  return (unsigned long)pthread_self();
}

/* The perf_event component's own diagnosis (e.g. "perf_event_paranoid
 * prohibits...") is far more useful than a bare PAPI_EPERM. */
static const char *papi_disabled_reason(void) {
  int cidx = PAPI_get_component_index("perf_event");
  const PAPI_component_info_t *ci = PAPI_get_component_info(cidx < 0 ? 0 : cidx);
  if (ci != NULL && ci->disabled) {
    return ci->disabled_reason;
  }
  return NULL;
}

static void papi_fail(const char *what, int rc) {
  const char *reason = papi_disabled_reason();
  ARTS_ERROR("papi: %s failed: %s%s%s%s", what, PAPI_strerror(rc),
             reason ? " (perf_event component disabled: " : "",
             reason ? reason : "", reason ? ")" : "");
}

/* Split `events` on commas into papi_names, trimming blanks. */
static void papi_parse_events(const char *events) {
  const char *p = (events && *events) ? events : ARTS_PAPI_DEFAULT_EVENTS;
  papi_count = 0;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ',') {
      p++;
    }
    if (!*p) {
      break;
    }
    const char *end = p;
    while (*end && *end != ',') {
      end++;
    }
    const char *last = end;
    while (last > p && (last[-1] == ' ' || last[-1] == '\t')) {
      last--;
    }
    size_t len = (size_t)(last - p);
    if (papi_count == ARTS_PAPI_MAX_EVENTS) {
      ARTS_ERROR("papi: papi_events names more than %d events ('%s'); each "
                 "scope has %d slots",
                 ARTS_PAPI_MAX_EVENTS, events, ARTS_PAPI_MAX_EVENTS);
    }
    if (len >= ARTS_PAPI_NAME_MAX) {
      ARTS_ERROR("papi: event name '%.*s' is too long", (int)len, p);
    }
    memcpy(papi_names[papi_count], p, len);
    papi_names[papi_count][len] = '\0';
    papi_count++;
    p = end;
  }
  if (papi_count == 0) {
    ARTS_ERROR("papi: papi_events '%s' names no event", events);
  }
}

/* Build an EventSet holding every configured event; fatal on any refusal. */
static int papi_build_eventset(void) {
  int set = PAPI_NULL;
  int rc = PAPI_create_eventset(&set);
  if (rc != PAPI_OK) {
    papi_fail("PAPI_create_eventset", rc);
  }
  for (unsigned int k = 0; k < papi_count; k++) {
    rc = PAPI_add_event(set, papi_codes[k]);
    if (rc == PAPI_ECNFLCT) {
      /* The events so far already hold every counter this one could use (or
       * it needs a counter another does): the set does not fit. */
      ARTS_ERROR("papi: cannot count '%s' together with the %u event(s) before "
                 "it in papi_events: %s (too many or conflicting events for "
                 "this core's counters; split the list across runs)",
                 papi_names[k], k, PAPI_strerror(rc));
    }
    if (rc != PAPI_OK) {
      /* Anything else is the host refusing to count at all (a disabled
       * perf_event component under perf_event_paranoid > 2, no PMU), which
       * the component's own reason explains. */
      char what[ARTS_PAPI_NAME_MAX + 32];
      snprintf(what, sizeof(what), "adding '%s'", papi_names[k]);
      papi_fail(what, rc);
    }
  }
  return set;
}

static void papi_release_eventset(int *set) {
  (void)PAPI_cleanup_eventset(*set);
  (void)PAPI_destroy_eventset(set);
  *set = PAPI_NULL;
}

void arts_papi_node_init(const char *events) {
  papi_parse_events(events);

  int rc = PAPI_library_init(PAPI_VER_CURRENT);
  if (rc != PAPI_VER_CURRENT) {
    papi_fail("PAPI_library_init", rc);
  }
  rc = PAPI_thread_init(papi_thread_id);
  if (rc != PAPI_OK) {
    papi_fail("PAPI_thread_init", rc);
  }
  for (unsigned int k = 0; k < papi_count; k++) {
    rc = PAPI_event_name_to_code(papi_names[k], &papi_codes[k]);
    if (rc != PAPI_OK) {
      ARTS_ERROR("papi: unknown event '%s' (%s); list this host's events with "
                 "papi_avail / papi_native_avail",
                 papi_names[k], PAPI_strerror(rc));
    }
  }

  /* Prove the whole set starts here, on the main thread, before any worker
   * exists: a host that cannot count (perf_event_paranoid, a conflicting set)
   * fails once with one diagnosis instead of on every thread mid-startup. */
  int trial = papi_build_eventset();
  rc = PAPI_start(trial);
  if (rc != PAPI_OK) {
    papi_fail("PAPI_start", rc);
  }
  long long scratch[ARTS_PAPI_MAX_EVENTS];
  (void)PAPI_stop(trial, scratch);
  papi_release_eventset(&trial);
}

void arts_papi_thread_start(void) {
  int rc = PAPI_register_thread();
  if (rc != PAPI_OK) {
    papi_fail("PAPI_register_thread", rc);
  }
  papi_eventset = papi_build_eventset();
  memset(papi_depth, 0, sizeof(papi_depth));
  rc = PAPI_start(papi_eventset);
  if (rc != PAPI_OK) {
    papi_fail("PAPI_start", rc);
  }
  papi_running = true;
  /* The role is fixed for the thread's life, so the TOTAL scope is chosen
   * once; progress threads never run EDTs, so theirs is pure runtime. */
  papi_total_scope = arts_thread_info.role == ARTS_ROLE_PROGRESS
                         ? ARTS_PAPI_TOTAL_PROGRESS
                         : ARTS_PAPI_TOTAL_WORKER;
  arts_papi_scope_begin(papi_total_scope);
}

/* Charge [papi_start[scope], now) to the scope's slots.  The counter writes
 * stay the shared atomic ones, so the capture thread may sample them. */
static void papi_accumulate(arts_papi_scope_t scope, const long long *now) {
  arts_counter_t *slots =
      &arts_thread_local_counters[PAPI_TOTAL_WORKER_E0 +
                                  (unsigned int)scope * ARTS_PAPI_MAX_EVENTS];
  for (unsigned int k = 0; k < papi_count; k++) {
    long long delta = now[k] - papi_start[scope][k];
    if (delta > 0) {
      arts_counter_increment_by(&slots[k], (uint64_t)delta);
    }
  }
}

void arts_papi_thread_stop(void) {
  if (!papi_running) {
    return;
  }
  long long now[ARTS_PAPI_MAX_EVENTS];
  int rc = PAPI_stop(papi_eventset, now);
  if (rc != PAPI_OK) {
    papi_fail("PAPI_stop", rc);
  }
  papi_running = false;
  /* Only the TOTAL scope can still be open: the run loop has returned, so no
   * EDT or runtime region is in flight on this thread. */
  papi_accumulate(papi_total_scope, now);
  papi_release_eventset(&papi_eventset);
  (void)PAPI_unregister_thread();
}

void arts_papi_scope_begin(arts_papi_scope_t scope) {
  /* Threads that never ran arts_papi_thread_start (the counter capture
   * thread, the signal watcher) pass through: they have no EventSet. */
  if (!papi_running || papi_depth[scope]++) {
    return;
  }
  int rc = PAPI_read(papi_eventset, papi_start[scope]);
  if (ARTS_UNLIKELY(rc != PAPI_OK)) {
    papi_fail("PAPI_read", rc);
  }
}

void arts_papi_scope_end(arts_papi_scope_t scope) {
  if (!papi_running || papi_depth[scope] == 0 || --papi_depth[scope]) {
    return;
  }
  long long now[ARTS_PAPI_MAX_EVENTS];
  int rc = PAPI_read(papi_eventset, now);
  if (ARTS_UNLIKELY(rc != PAPI_OK)) {
    papi_fail("PAPI_read", rc);
  }
  papi_accumulate(scope, now);
}

unsigned int arts_papi_event_count(void) { return papi_count; }

const char *arts_papi_event_name(unsigned int slot) {
  return slot < papi_count ? papi_names[slot] : NULL;
}
