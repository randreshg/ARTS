/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file lsan_check_runs.c
/// @brief The rank's end runs the leak checker of a LeakSanitizer build.
///
/// An EDT allocates a block and drops its last pointer to it, then ends the
/// run.  The process ends with exit_group, which runs no exit-time check, so
/// only the end's own leak check can report the block: LeakSanitizer's report
/// and its exit status (ASan's exitcode, 1) are the evidence.

#include "arts.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Not inlined, so the pointer lives in this frame alone, which returns. */
__attribute__((noinline)) static void leak(void) {
  char *volatile p = malloc(4096);
  if (p != NULL) {
    memset(p, 0x5a, 4096);
  }
  p = NULL;
}

/* Overwrites the stack the frame above used. */
__attribute__((noinline)) static void scrub(void) {
  volatile char pad[16384];
  memset((char *)pad, 0, sizeof(pad));
}

void leak_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  leak();
  scrub();
  arts_printf("LSAN_CHECK_RUNS: a block was leaked\n");
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_edt_create(leak_edt, 0, NULL, 0, &(arts_edt_hint_t){.rank = 0});
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
