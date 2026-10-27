/* SPDX-License-Identifier: Apache-2.0
 *
 * A destroy that acts on an EDT while the create body that installed it is
 * still running must see the EDT's final finish scope.
 *
 * An EDT created on another rank, inside a finish scope homed on its creator,
 * joins that scope through a local proxy event on its home, whose fire
 * carries the completion's DECR back.  The create body that installs the EDT
 * on its home runs on a progress thread; from the install on, a destroy
 * dispatched on any other thread can retire the EDT and send its DECR to
 * whatever finish_event it reads.  So finish_event must already name the
 * proxy when the EDT is installed: a destroy that read the creator's scope
 * there would DECR the scope directly, and a proxy made afterwards would never
 * fire.
 *
 * The run holds the create body right after the install (the runtime's
 * injection hook) and has a task on the home's worker destroy the EDT in that
 * window, from another thread:
 *   - at the install, finish_event names an event of the home (the proxy);
 *   - the destroy runs while the body is held, and the proxy it DECRs fires
 *     and is reclaimed before the body goes on;
 *   - the scope drains, and the shutdown is quiescent.
 * The destroyer is started, and seen running, before the EDT is created, so
 * the held body waits on no thread of its own rank's progress engine.
 *
 * Whitebox: the hook is compiled only where ARTS_DEBUG_INJECTION is.
 */
#include "arts.h"

#include "arts/edt.h"
#include "arts/gas/route_table.h"
#include "arts/runtime_types.h"
#include "arts/utils/shared.h"

#include "../test_failure_status.h"

#include <sched.h>
#include <stdint.h>
#include <time.h>

#define WAIT_NS (20ull * 1000000000ull)

static _Atomic(arts_guid_t) g_victim;    /* the held EDT, once installed */
static _Atomic(bool) g_destroyed;        /* the destroy has returned */

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void victim(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("FAIL: edt_create_destroy_proxy ran the destroyed EDT\n");
  arts_test_fail();
}

static void fail(const char *what) {
  arts_printf("FAIL: edt_create_destroy_proxy: %s\n", what);
  arts_test_fail();
}

/* The create body of the victim, held right after its install. */
static void installed_hook(struct arts_edt_s *edt) {
  if (edt->func_ptr != victim) {
    return;
  }
  arts_guid_t proxy = edt->finish_event;
  bool proxy_here = proxy != NULL_GUID &&
                    arts_guid_get_rank(proxy) == arts_get_current_rank();
  if (!proxy_here) {
    fail("finish_event does not name the home's proxy at the install");
  }
  atomic_store_explicit(&g_victim, edt->guid, memory_order_release);
  uint64_t deadline = now_ns() + WAIT_NS;
  while (!atomic_load_explicit(&g_destroyed, memory_order_acquire)) {
    if (now_ns() > deadline) {
      fail("the destroy never ran while the create body was held");
      return;
    }
    sched_yield();
  }
  if (!proxy_here) {
    return;
  }
  /* The destroy DECRed the proxy, whose fire reclaims it. */
  deadline = now_ns() + WAIT_NS;
  for (;;) {
    arts_shared_ptr_t h = arts_route_table_lookup_event(proxy);
    bool live = arts_shared_get(h) != NULL;
    arts_shared_release(&h);
    if (!live) {
      return;
    }
    if (now_ns() > deadline) {
      fail("the proxy the destroy reached never fired");
      return;
    }
    sched_yield();
  }
}

/* On the victim's home: tells the creator it is running, then destroys the
 * victim while its create body is held. */
static void destroyer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_event_satisfy((arts_guid_t)paramv[0], NULL_GUID);
  uint64_t deadline = now_ns() + WAIT_NS;
  arts_guid_t v;
  while ((v = atomic_load_explicit(&g_victim, memory_order_acquire)) ==
         NULL_GUID) {
    if (now_ns() > deadline) {
      fail("the victim was never installed");
      return;
    }
    sched_yield();
  }
  arts_edt_destroy(v);
  atomic_store_explicit(&g_destroyed, true, memory_order_release);
}

/* On the creator's rank once the destroyer runs: creates the victim. */
static void creator(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t scope = (arts_guid_t)paramv[0];
  unsigned int home = (unsigned int)paramv[1];
  (void)arts_edt_create(
      victim, 0, NULL, 1,
      &(arts_edt_hint_t){.rank = home, .finish_event = scope});
}

/* Gated on the scope.  paramv[0] = the event the destroyer satisfied. */
static void finale(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_event_destroy((arts_guid_t)paramv[0]);
  if (arts_test_status() == 0) {
    arts_printf("PASS: edt_create_destroy_proxy\n");
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int home = arts_get_total_ranks() - 1u;
  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t running = arts_event_create(NULL);
  uint64_t dp[1] = {(uint64_t)running};
  (void)arts_edt_create(
      destroyer, 1, dp, 0,
      &(arts_edt_hint_t){.rank = home, .finish_event = scope});
  uint64_t cp[2] = {(uint64_t)scope, home};
  arts_guid_t c = arts_edt_create(
      creator, 2, cp, 1,
      &(arts_edt_hint_t){.rank = arts_get_current_rank(),
                         .finish_event = scope});
  arts_add_dependence(running, c, 0, DB_MODE_NULL);
  arts_guid_t f = arts_edt_create(
      finale, 1, dp, 1, &(arts_edt_hint_t){.rank = arts_get_current_rank()});
  arts_add_dependence(scope, f, 0, DB_MODE_NULL);
}

int main(int argc, char **argv) {
  arts_edt_installed_hook = installed_hook;
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
