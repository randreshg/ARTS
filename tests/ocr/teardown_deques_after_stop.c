/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/* teardown_deques_after_stop -- the shutdown comes while workers steal.
 *
 * The main EDT spreads empty EDTs over every rank and calls arts_shutdown at
 * once, so idle workers are still taking EDTs from one another's deques, and
 * from the progress threads', when the stop reaches them.  The teardown then
 * frees every deque, which must come after the last thief's last access: a
 * race detector reports a free that does not.  EDTs the shutdown leaves in a
 * deque are abandoned, which is legal.
 */

#include <stdint.h>

#include "arts.h"

#define EMPTY_EDTS 64u

static void empty_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  const unsigned int ranks = arts_get_total_ranks();
  for (unsigned int i = 0; i < EMPTY_EDTS; i++) {
    arts_edt_hint_t hint = ARTS_EDT_HINT_DEFAULTS;
    hint.rank = i % ranks;
    (void)arts_edt_create(empty_edt, 0, NULL, 0, &hint);
  }
  arts_printf("PASS: teardown_deques_after_stop made %u EDTs\n", EMPTY_EDTS);
  arts_shutdown();
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
