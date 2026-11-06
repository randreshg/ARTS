/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file shutdown_stdout_closed.c
/// @brief A run whose standard output's reader has gone ends with its own
///        status, without hanging.
///
/// Run as `shutdown_stdout_closed | head -1`: main_edt writes LINES lines to
/// stdout with stdio, most of them after head has read its one line and
/// exited, so those writes fail (SIGPIPE is ignored), and then ends the run.
/// The end's flush of the streams meets the same closed pipe; the status is
/// still 0.

#include "arts.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define LINES 2000u

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  printf("STDOUT_CLOSED: first line\n");
  fflush(stdout);
  struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L};
  nanosleep(&pause, NULL);
  for (unsigned int i = 0; i < LINES; i++) {
    printf("STDOUT_CLOSED: line %u of output nobody reads any more\n", i);
  }
  arts_shutdown();
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
