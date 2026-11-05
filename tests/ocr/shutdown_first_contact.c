/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file shutdown_first_contact.c
/// @brief A rank posts SHUTDOWN only to the peers it contacted; a peer it
///        never contacted learns of the end when a connection closes.
///
/// Four ranks.  Rank 1 sends one message to rank 0 and one to rank 2 (an EDT
/// create on each) and none to rank 3.  Each created EDT answers on a latch
/// on rank 1, and rank 1 ends the run once both answers came back, so both
/// creates were accepted before its end began, and the posting threads have
/// marked both contacts: the post goes to ranks 0 and 2 unless such a thread
/// is still between the accept and its mark, the miss I2 allows.  (Ending
/// right after the creates would leave the count open: a first send to a
/// peer may wait in the retry ring while the connection is made, and the post
/// skips a peer whose send it has not seen accepted.)  Rank 3 can learn of
/// the end only from a closed connection, since only rank 1 begins the end
/// and a rank that learns of it posts nothing.  The run ends with status 0.
/// Where the build prints INFO, rank 3's entry names that origin and rank 1's
/// post names the two peers.

#include "arts.h"

#include <stdint.h>

#include "../test_failure_status.h"

/// Runs on rank 0 and on rank 2.  paramv = {rank 1's latch}.
void answer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_event_satisfy_slot((arts_guid_t)paramv[0], NULL_GUID,
                          ARTS_EVENT_LATCH_DECR_SLOT);
}

void finish(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
            arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("FIRST_CONTACT: rank 1 ends the run, rank 3 never contacted\n");
  arts_shutdown();
}

void first(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
           arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_event_hint_t eh = ARTS_EVENT_HINT_LATCH(2);
  arts_guid_t latch = arts_event_create(&eh);
  arts_guid_t fin =
      arts_edt_create(finish, 0, NULL, 1, &(arts_edt_hint_t){.rank = 1});
  arts_add_dependence(latch, fin, 0, DB_MODE_NULL);
  uint64_t p[1] = {(uint64_t)latch};
  arts_edt_create(answer, 1, p, 0, &(arts_edt_hint_t){.rank = 0});
  arts_edt_create(answer, 1, p, 0, &(arts_edt_hint_t){.rank = 2});
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_get_total_ranks() != 4) {
    arts_printf("  FAIL: needs exactly four ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  arts_edt_create(first, 0, NULL, 0, &(arts_edt_hint_t){.rank = 1});
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
