/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file shutdown_sigterm_inflight_drain.c
/// @brief A SIGTERM to rank 0 while satisfies from every other rank are in
///        flight to it ends the run gracefully, with every counted send
///        completed.
///
/// Rank 0 keeps LANES chains per other rank running.  One round of a chain:
/// rank 0 creates an event and a consumer EDT whose only dependence is that
/// event, then has the other rank satisfy the event, so each satisfy that
/// reaches rank 0 is the last dependence of an EDT there and readies it.  The
/// consumer starts the chain's next round.  Once one chain has run ROUNDS
/// rounds, its consumer signals the process with a process-directed SIGTERM
/// (never raise(): a thread-directed signal stays pending on the worker, which
/// blocks the termination signals).  The signal watcher then initiates the
/// shutdown: it broadcasts the shutdown and waits for its sends to complete
/// while satisfies keep arriving.  Only a thread that owns a deque may ready
/// an EDT, so that wait must dispatch none of them.
///
/// Every rank's status says how its run ended; the master's carries the
/// launcher's verdict on every rank it spawned.

#include "arts.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "../test_failure_status.h"

/* Chains per other rank. */
#define LANES 32u
/* Rounds the signalling chain runs before the signal. */
#define ROUNDS 64u

static void arm(unsigned int peer, unsigned int lane, unsigned int round);

/// Runs on the other rank: satisfies rank 0's event.  paramv[0] = event.
void satisfier(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_event_satisfy_slot((arts_guid_t)paramv[0], NULL_GUID,
                          ARTS_EVENT_LATCH_DECR_SLOT);
}

/// Runs on rank 0 once its event fired.  paramv = {peer, lane, round}.
void consumer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  unsigned int peer = (unsigned int)paramv[0];
  unsigned int lane = (unsigned int)paramv[1];
  unsigned int round = (unsigned int)paramv[2];
  if (arts_get_current_rank() != 0) {
    arts_printf("  FAIL: a consumer ran on rank %u\n", arts_get_current_rank());
    arts_test_fail();
  }
  if (peer == 1 && lane == 0 && round == ROUNDS) {
    arts_printf("  signalling rank 0 with %u chains per rank in flight\n",
                LANES);
    kill(getpid(), SIGTERM);
  }
  arm(peer, lane, round + 1);
}

static void arm(unsigned int peer, unsigned int lane, unsigned int round) {
  arts_event_hint_t eh = ARTS_EVENT_HINT_COUNTED(1);
  eh.rank = 0;
  arts_guid_t ev = arts_event_create(&eh);
  uint64_t cp[3] = {peer, lane, round};
  arts_guid_t c =
      arts_edt_create(consumer, 3, cp, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(ev, c, 0, DB_MODE_NULL);
  uint64_t sp[1] = {(uint64_t)ev};
  arts_edt_create(satisfier, 1, sp, 0, &(arts_edt_hint_t){.rank = peer});
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int ranks = arts_get_total_ranks();
  arts_printf("=== shutdown_sigterm_inflight_drain: %u ranks ===\n", ranks);
  if (ranks < 2) {
    arts_printf("  FAIL: needs at least two ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  for (unsigned int peer = 1; peer < ranks; peer++) {
    for (unsigned int lane = 0; lane < LANES; lane++) {
      arm(peer, lane, 1);
    }
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  printf("[rank %u] SIGTERM_DRAIN_EXIT\n", arts_get_current_rank());
  return rc ? 1 : arts_test_status();
}
