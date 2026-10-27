/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file threads_pin_affinity.c
/// @brief Verifies arts_thread_init applied the per-thread CPU affinity: each
///        worker thread's effective affinity matches its assigned pu_id.
///
/// When config->pin_threads is on (non-Apple), arts_thread_init builds a
/// cpu_set_t with arts_thread_masks[i].pu_id for each spawned thread via
/// pthread_attr_setaffinity_np (and pins thread 0 via pthread_setaffinity_np).
/// The observable invariant: each worker thread, queried with
/// pthread_getaffinity_np, is bound to exactly the single PU recorded in its
/// entry of the thread table.  main_edt walks the table: for every worker
/// entry it queries the thread through its handle in arts_thread_handles.
///
/// The expectation follows the run's own config (its `pin`
/// key, read before the runtime starts).  With pin on, a worker that is not
/// bound to exactly its pu_id FAILS, whatever the host; with pin off there is
/// nothing to bind and each worker reports a SKIP for its check (the test
/// still passes).  On Apple (__APPLE__) there is no pthread_getaffinity_np and
/// pinning is a no-op -> the walk self-skips.
///
/// Single-node, every configuration (protocol-agnostic; affinity behavior is
/// config).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "arts.h"
#include "arts/system/config.h"
#include "arts/system/threads.h"
#include "arts/system/topology.h"

#include <stdbool.h>

#if !defined(__APPLE__)
#include <pthread.h>
#include <sched.h>
#endif

#include "../test_failure_status.h"

/// The run's `pin` setting, read from its config before the runtime starts.
static bool g_pin_requested;

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("=== threads_pin_affinity ===\n");
#if !defined(__APPLE__)
  /* The startup barriers order the thread table's writes before any EDT, and
   * every thread it names runs until shutdown. */
  unsigned int workers = arts_get_workers_per_rank();
  unsigned int checked = 0;
  unsigned int bound = 0;
  for (unsigned int i = 0; i < arts_thread_config->thread_count; i++) {
    const struct thread_mask_s *m = &arts_thread_masks[i];
    if (m->role != ARTS_ROLE_WORKER) {
      continue;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    if (pthread_getaffinity_np(arts_thread_handles[i], sizeof(set), &set) !=
        0) {
      arts_printf("  FAIL pin affinity: getaffinity failed (worker pu_id=%u)\n",
                  m->pu_id);
      arts_test_fail();
      continue;
    }
    checked++;
    int ncpus = CPU_COUNT(&set);
    if (!g_pin_requested) {
      arts_printf("  SKIP pin affinity: pin off in this config (worker "
                  "pu_id=%u, %d CPUs in affinity)\n",
                  m->pu_id, ncpus);
    } else if (ncpus == 1 && CPU_ISSET((int)m->pu_id, &set)) {
      bound++;
      arts_printf("  PASS pin affinity: worker bound to pu_id=%u\n", m->pu_id);
    } else {
      arts_printf("  FAIL pin affinity: pin on, but worker pu_id=%u is bound "
                  "to %d CPUs%s\n",
                  m->pu_id, ncpus,
                  CPU_ISSET((int)m->pu_id, &set) ? ""
                                                 : ", its own not among them");
      arts_test_fail();
    }
  }
  if (workers == 0 || checked != workers) {
    arts_printf("FAIL threads_pin_affinity: %u of %u workers checked\n",
                checked, workers);
    arts_test_fail();
  } else if (g_pin_requested && bound != workers) {
    arts_printf("FAIL threads_pin_affinity: %u of %u workers bound\n", bound,
                workers);
    arts_test_fail();
  } else {
    arts_printf("PASS threads_pin_affinity: %u of %u workers checked, %u "
                "bound (pin %s)\n",
                checked, workers, bound, g_pin_requested ? "on" : "off");
  }
#else
  arts_printf("  SKIP pin affinity: Apple (no pthread_getaffinity_np)\n");
  arts_printf("PASS threads_pin_affinity: nothing to check on Apple\n");
#endif
  arts_shutdown();
}

int main(int argc, char **argv) {
  struct arts_config_s cfg;
  arts_config_load(&cfg);
  g_pin_requested = cfg.pin_threads;
  arts_config_destroy(&cfg);
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
