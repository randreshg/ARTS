/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/
/// @file term_stack_overflow.c
/// @brief A stack overflow on one rank ends that rank by SIGSEGV under the
///        default action, and its peers learn of the end.
///
/// An EDT on rank argv[1] (1 by default) recurses without bound.  No
/// alternate signal stack exists, so the kernel cannot run the crash handler
/// on the overflowed stack and ends the rank by SIGSEGV (status 139) without
/// any line of the runtime's.  Every other rank learns of the end from that
/// rank's closed connection and ends with 0.

#include "arts.h"

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

/* Read on every call, so the recursion is never proved unbounded. */
static volatile int g_deeper = 1;

static unsigned int deep(unsigned int n) {
  volatile char pad[1024];
  pad[0] = (char)n;
  pad[sizeof(pad) - 1] = (char)(n >> 8);
  if (g_deeper) {
    return deep(n + 1u) + (unsigned int)pad[0];
  }
  return (unsigned int)pad[sizeof(pad) - 1];
}

void overflow_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("STACK_OVERFLOW: rank %u recurses\n", arts_get_current_rank());
  arts_printf("unexpected return %u\n", deep(0));
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  unsigned int origin =
      (argc > 1) ? (unsigned int)strtoul(argv[1], NULL, 10) : 1u;
  if (origin >= arts_get_total_ranks()) {
    origin = 0;
  }
  arts_edt_create(overflow_edt, 0, NULL, 0, &(arts_edt_hint_t){.rank = origin});
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
