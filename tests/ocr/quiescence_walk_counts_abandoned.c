/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file quiescence_walk_counts_abandoned.c
/// @brief The EDTs a shutdown leaves runnable in a deque reach the teardown's
///        quiescence walk.
///
/// The run has one worker.  Its main_edt makes EDTS_LEFT EDTs with no
/// dependence, which go to that worker's deque, and calls arts_shutdown: no
/// thread is left to run or steal them, so the deque holds exactly that many at
/// the teardown.  The walk reports the shutdown non-quiescent and names the
/// count; the test's registration matches it.

#include <stdint.h>

#include "arts.h"

#include "../test_failure_status.h"

#define EDTS_LEFT 64u

static void left_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("FAIL: an EDT left in the deque ran\n");
  arts_test_fail();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  for (unsigned int i = 0; i < EDTS_LEFT; i++) {
    (void)arts_edt_create(left_edt, 0, NULL, 0, NULL);
  }
  arts_shutdown();
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
