/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
******************************************************************************/

/// @file epoch_wait_nested_current.c
/// @brief Regression for nested arts_wait_on_handle epoch-stack restoration.

#include "arts.h"

#include <stdint.h>
#include <stdlib.h>

static volatile unsigned int child_count = 0;
static volatile unsigned int post_wait_count = 0;
static volatile unsigned int failure_count = 0;

static void fail_guid(const char *label, arts_guid_t got, arts_guid_t expected) {
  arts_printf("  FAIL: %s current=%lu expected=%lu\n", label, (uint64_t)got,
              (uint64_t)expected);
  __sync_fetch_and_add((unsigned int *)&failure_count, 1);
}

void child_task(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t expected = (arts_guid_t)paramv[0];
  arts_guid_t current = arts_get_current_epoch_guid();
  if (current != expected) {
    fail_guid("child epoch", current, expected);
  }
  __sync_fetch_and_add((unsigned int *)&child_count, 1);
}

void post_wait_task(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t expected = (arts_guid_t)paramv[0];
  arts_guid_t current = arts_get_current_epoch_guid();
  if (current != expected) {
    fail_guid("post-wait implicit epoch", current, expected);
  }
  __sync_fetch_and_add((unsigned int *)&post_wait_count, 1);
}

void parent_task(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t parent_epoch = (arts_guid_t)paramv[0];
  arts_guid_t current = arts_get_current_epoch_guid();
  if (current != parent_epoch) {
    fail_guid("parent entry", current, parent_epoch);
  }

  arts_guid_t child_epoch = arts_initialize_and_start_epoch(NULL_GUID, 0);
  uint64_t child_param = (uint64_t)child_epoch;
  arts_guid_t child = arts_edt_create_with_epoch(
      child_task, 1, &child_param, 0, child_epoch, &(arts_hint_t){.route = 0});
  if (child == NULL_GUID || !arts_wait_on_handle(child_epoch)) {
    arts_printf("  FAIL: child epoch wait failed\n");
    __sync_fetch_and_add((unsigned int *)&failure_count, 1);
    return;
  }

  current = arts_get_current_epoch_guid();
  if (current != parent_epoch) {
    fail_guid("parent after child wait", current, parent_epoch);
  }

  uint64_t parent_param = (uint64_t)parent_epoch;
  arts_guid_t post =
      arts_edt_create(post_wait_task, 1, &parent_param, 0,
                      &(arts_hint_t){.route = 0});
  if (post == NULL_GUID) {
    arts_printf("  FAIL: post-wait EDT create failed\n");
    __sync_fetch_and_add((unsigned int *)&failure_count, 1);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== epoch_wait_nested_current ===\n");
  child_count = 0;
  post_wait_count = 0;
  failure_count = 0;

  arts_guid_t parent_epoch = arts_initialize_and_start_epoch(NULL_GUID, 0);
  uint64_t parent_param = (uint64_t)parent_epoch;
  arts_guid_t parent = arts_edt_create_with_epoch(
      parent_task, 1, &parent_param, 0, parent_epoch,
      &(arts_hint_t){.route = 0});
  if (parent == NULL_GUID || !arts_wait_on_handle(parent_epoch)) {
    arts_printf("  FAIL: parent epoch wait failed\n");
    abort();
  }

  if (child_count != 1 || post_wait_count != 1 || failure_count != 0) {
    arts_printf("  FAIL: child=%u post_wait=%u failures=%u\n", child_count,
                post_wait_count, failure_count);
    abort();
  }

  arts_printf("  PASS: nested wait restores parent epoch for implicit EDTs\n");
  arts_shutdown();
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return 0;
}
