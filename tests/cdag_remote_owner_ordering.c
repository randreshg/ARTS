/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
******************************************************************************/

/// @file cdag_remote_owner_ordering.c
/// @brief Single-epoch CDAG ordering when rank 0 builds dependencies for a DB
///        owned by another rank.

#include "arts.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>

#define DEFAULT_STEPS 24

void set_value_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t *value = (uint64_t *)depv[0].ptr;
  if (!value) {
    abort();
  }
  value[0] = paramv[0];
}

void check_value_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const uint64_t *value = (const uint64_t *)depv[0].ptr;
  uint64_t expected = paramv[0];
  uint64_t step = paramv[1];
  if (!value || value[0] != expected) {
    arts_printf("  FAIL: remote_owner_ordering step=%" PRIu64
                " expected=%" PRIu64 " got=%" PRIu64 "\n",
                step, expected, value ? value[0] : UINT64_MAX);
    abort();
  }
}

static void wait_or_abort(arts_guid_t epoch, const char *stage) {
  if (!arts_wait_on_handle(epoch)) {
    arts_printf("  FAIL: remote_owner_ordering wait failed at %s\n", stage);
    abort();
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;

  uint64_t steps = DEFAULT_STEPS;
  if (paramc >= 2) {
    char **argv = (char **)paramv[1];
    if (argv && argv[1]) {
      uint64_t parsed = strtoull(argv[1], NULL, 10);
      if (parsed > 0) {
        steps = parsed;
      }
    }
  }

  unsigned int nodes = arts_get_total_nodes();
  if (nodes < 2) {
    arts_printf("  SKIP: cdag_remote_owner_ordering needs at least 2 nodes\n");
    arts_shutdown();
    return;
  }

  unsigned int owner = nodes - 1;
  arts_printf("=== cdag_remote_owner_ordering steps=%" PRIu64
              " owner=%u nodes=%u ===\n",
              steps, owner, nodes);

  void *ptr = NULL;
  arts_guid_t db =
      arts_db_create(&ptr, sizeof(uint64_t), ARTS_DB_DEFAULT,
                     &(arts_hint_t){.route = owner});
  (void)ptr;

  arts_guid_t init_epoch = arts_initialize_and_start_epoch(NULL_GUID, 0);
  uint64_t zero = 0;
  arts_guid_t init = arts_edt_create_with_epoch(
      set_value_edt, 1, &zero, 1, init_epoch, &(arts_hint_t){.route = owner});
  arts_add_dependence(db, init, 0, DB_MODE_EW);
  wait_or_abort(init_epoch, "init");

  arts_guid_t epoch = arts_initialize_and_start_epoch(NULL_GUID, 0);
  for (uint64_t step = 1; step <= steps; ++step) {
    unsigned int writer_route = (step & 1U) ? 0U : owner;
    uint64_t writer_param = step;
    arts_guid_t writer = arts_edt_create_with_epoch(
        set_value_edt, 1, &writer_param, 1, epoch,
        &(arts_hint_t){.route = writer_route});
    arts_add_dependence(db, writer, 0, DB_MODE_EW);

    unsigned int reader_route = (step % 3U == 0U) ? owner : 0U;
    uint64_t reader_param[2] = {step, step};
    arts_guid_t reader = arts_edt_create_with_epoch(
        check_value_edt, 2, reader_param, 1, epoch,
        &(arts_hint_t){.route = reader_route});
    arts_add_dependence(db, reader, 0, DB_MODE_RO);
  }
  wait_or_abort(epoch, "single_epoch_order");

  arts_printf("  PASS: cdag_remote_owner_ordering steps=%" PRIu64 "\n", steps);
  arts_shutdown();
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return 0;
}
