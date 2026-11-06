/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file shutdown_from_user_thread.c
/// @brief A thread the program starts, not the runtime, ends the run.
///
/// An EDT on rank 0 starts a detached pthread and returns; the thread calls
/// arts_shutdown.  The thread is no runtime thread, so the end stops every
/// runtime thread, the main thread included, and nothing it runs is exempt:
/// with every EDT finished, the teardown runs whole and its walk finds the
/// shutdown quiescent.  Status 0.

#include "arts.h"

#include <pthread.h>
#include <stdint.h>

#include "../test_failure_status.h"

static void *ender(void *arg) {
  (void)arg;
  arts_printf("USER_THREAD: a program thread ends the run\n");
  arts_shutdown();
}

void start_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  pthread_t t;
  if (pthread_create(&t, NULL, ender, NULL) != 0) {
    arts_printf("  FAIL: pthread_create failed\n");
    arts_test_fail();
    arts_shutdown();
  }
  (void)pthread_detach(t);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_edt_create(start_edt, 0, NULL, 0, &(arts_edt_hint_t){.rank = 0});
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
