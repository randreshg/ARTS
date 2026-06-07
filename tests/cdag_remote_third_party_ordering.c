/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
******************************************************************************/

/// @file cdag_remote_third_party_ordering.c
/// @brief CDAG ordering for a remote-owned DB with non-owner writer/reader ranks.

#include "arts.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>

#define DEFAULT_STEPS 32

static uint64_t delay_work(uint64_t n) {
  volatile uint64_t acc = 0;
  for (uint64_t i = 0; i < n; ++i) {
    acc += (i ^ (n << 1U)) & 1U;
  }
  return acc - acc;
}

void set_value_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t *value = (uint64_t *)depv[0].ptr;
  if (!value) {
    abort();
  }
  value[0] = paramv[0] + delay_work(paramv[1]);
}

void check_value_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const uint64_t *value = (const uint64_t *)depv[0].ptr;
  uint64_t expected = paramv[0];
  uint64_t step = paramv[1];
  if (!value || value[0] != expected) {
    arts_printf("  FAIL: cdag_remote_third_party_ordering step=%" PRIu64
                " expected=%" PRIu64 " got=%" PRIu64 "\n",
                step, expected, value ? value[0] : UINT64_MAX);
    abort();
  }
}

static void wait_or_abort(arts_guid_t epoch, const char *stage) {
  if (!arts_wait_on_handle(epoch)) {
    arts_printf("  FAIL: cdag_remote_third_party_ordering wait failed at %s\n",
                stage);
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
  if (nodes < 3) {
    arts_printf("  SKIP: cdag_remote_third_party_ordering needs at least 3 "
                "nodes\n");
    arts_shutdown();
    return;
  }

  unsigned int owner = nodes - 1U;
  unsigned int writer = 1U;
  unsigned int reader = (nodes > 3U) ? 2U : 0U;
  arts_printf("=== cdag_remote_third_party_ordering steps=%" PRIu64
              " owner=%u writer=%u reader=%u nodes=%u ===\n",
              steps, owner, writer, reader, nodes);

  void *ptr = NULL;
  arts_guid_t db =
      arts_db_create(&ptr, sizeof(uint64_t), ARTS_DB_DEFAULT,
                     &(arts_hint_t){.route = owner});
  (void)ptr;

  arts_guid_t init_epoch = arts_initialize_and_start_epoch(NULL_GUID, 0);
  uint64_t zero[2] = {0, 0};
  arts_guid_t init = arts_edt_create_with_epoch(
      set_value_edt, 2, zero, 1, init_epoch, &(arts_hint_t){.route = owner});
  arts_add_dependence(db, init, 0, DB_MODE_EW);
  wait_or_abort(init_epoch, "init");

  arts_guid_t epoch = arts_initialize_and_start_epoch(NULL_GUID, 0);
  for (uint64_t step = 1; step <= steps; ++step) {
    uint64_t writer_param[2] = {step, (steps - step) * 512U};
    arts_guid_t writer_edt = arts_edt_create_with_epoch(
        set_value_edt, 2, writer_param, 1, epoch,
        &(arts_hint_t){.route = writer});
    arts_add_dependence(db, writer_edt, 0, DB_MODE_EW);

    uint64_t reader_param[2] = {step, step};
    arts_guid_t reader_edt = arts_edt_create_with_epoch(
        check_value_edt, 2, reader_param, 1, epoch,
        &(arts_hint_t){.route = reader});
    arts_add_dependence(db, reader_edt, 0, DB_MODE_RO);
  }
  wait_or_abort(epoch, "third_party_order");

  arts_printf("  PASS: cdag_remote_third_party_ordering steps=%" PRIu64 "\n",
              steps);
  arts_shutdown();
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return 0;
}
