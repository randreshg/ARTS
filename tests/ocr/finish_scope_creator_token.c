/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* finish_scope_creator_token — a finish scope whose creator hands its token
 * back fires when its members finish, while the creator still runs.
 *
 * The OCR finish EDT's sequence through the native API: the creating EDT makes
 * a finish scope on its own rank, creates a worker on the last rank in it,
 * binds a continuation to the scope, and releases its creator-token.  The
 * worker creates a child on the first rank, which inherits the scope.  The
 * creator then keeps running; the continuation must observe the child done
 * and run before the creator returns.  A token held until the creator
 * completes would delay the fire past that point, and the creator gives up
 * after a bound.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#include "arts.h"
#include "../test_failure_status.h"

#define WAIT_NS (30ull * 1000000000ull)

static atomic_uint g_child_done;
static atomic_uint g_fired;

static uint64_t now_ns(void) {
  struct timespec t;
  (void)clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void child(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  atomic_store_explicit(&g_child_done, 1u, memory_order_release);
}

static void worker(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  (void)arts_edt_create(child, 0, NULL, 0, &(arts_edt_hint_t){.rank = 0u});
}

static void continuation(uint32_t paramc, const uint64_t *paramv,
                         uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (atomic_load_explicit(&g_child_done, memory_order_acquire) != 1u) {
    arts_printf("FAIL: finish_scope_creator_token: the scope fired before "
                "the worker's child finished\n");
    arts_test_fail();
  }
  atomic_store_explicit(&g_fired, 1u, memory_order_release);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int last = arts_get_total_ranks() - 1u;
  arts_guid_t scope = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  (void)arts_edt_create(
      worker, 0, NULL, 0,
      &(arts_edt_hint_t){.rank = last, .finish_event = scope});
  arts_guid_t cont =
      arts_edt_create(continuation, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0u});
  arts_add_dependence(scope, cont, 0, DB_MODE_NULL);
  arts_event_release_creator_token(scope);

  uint64_t start = now_ns();
  while (atomic_load_explicit(&g_fired, memory_order_acquire) == 0u &&
         now_ns() - start < WAIT_NS) {
  }
  if (atomic_load_explicit(&g_fired, memory_order_acquire) == 0u) {
    arts_printf("FAIL: finish_scope_creator_token: the scope did not fire "
                "while its creator ran\n");
    arts_test_fail();
  } else if (arts_test_status() == 0) {
    arts_printf("PASS: finish_scope_creator_token\n");
  }
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
