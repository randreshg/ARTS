/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/* shutdown_cuts_running_edt -- the run ends while an EDT body still runs.
 *
 * The spinner's body never returns on a worker other than thread 0: it raises
 * `spinning` and then waits for a flag nobody sets.  On thread 0, whose loop
 * ends in the node's teardown, it creates a copy of itself and returns, so
 * the spin never holds that thread; the copy goes onto thread 0's own deque
 * and spins once a thief takes it.  The trigger waits for `spinning` and
 * calls arts_shutdown.  The run then ends with the spinner's worker still in
 * its body, and its status is 0.
 *
 * Whitebox: it reads arts_thread_info for the thread it runs on.  One rank.
 */

#include <stdint.h>

#include "arts.h"
#include "arts/runtime_state.h"
#include "arts/system/threads.h" /* arts_runtime_idle_pause */

/* Relaxed: flags that publish nothing else. */
static _Atomic(unsigned int) spinning;
static _Atomic(unsigned int) never_set;

static void spinner_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_thread_info.thread_id == 0u) {
    arts_edt_hint_t hint = ARTS_EDT_HINT_DEFAULTS;
    (void)arts_edt_create(spinner_edt, 0, NULL, 0, &hint);
    return;
  }
  atomic_store_explicit(&spinning, 1u, memory_order_relaxed);
  while (atomic_load_explicit(&never_set, memory_order_relaxed) == 0u) {
    arts_runtime_idle_pause();
  }
}

static void trigger_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  while (atomic_load_explicit(&spinning, memory_order_relaxed) == 0u) {
    arts_runtime_idle_pause();
  }
  arts_printf("shutdown_cuts_running_edt: the spinner runs; shutting down\n");
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_edt_hint_t hint = ARTS_EDT_HINT_DEFAULTS;
  (void)arts_edt_create(spinner_edt, 0, NULL, 0, &hint);
  (void)arts_edt_create(trigger_edt, 0, NULL, 0, &hint);
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
