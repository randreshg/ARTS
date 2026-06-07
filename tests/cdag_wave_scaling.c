/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
******************************************************************************/

/// @file cdag_wave_scaling.c
/// @brief Local ARTS CDAG scaling wave, with generated-code-shaped DB reuse.
///
/// Creates a runtime-generated ping-pong graph:
///   next[i] = cur[i] + cur[(i + 1) % blocks]
/// for several timesteps over ARTS_DB_DEFAULT DBs.  This stresses the local
/// CDAG frontier with EW + RO + RO mixed-mode EDTs and reused buffers, without
/// CARTS or compiler lowering in the path.
///
/// By default, each timestep is launched in its own epoch and waited on,
/// matching the CARTS-generated "launch batch, add deps, wait, continue" shape.
/// Pass staged_epochs=0 to put the whole wave in one epoch and isolate raw CDAG
/// frontier ordering.
///
/// The DB owner is block % nodes by default so the same binary becomes a real
/// RDMA/CDAG diagnostic under a multi-node ARTS_CONFIG. Pass distributed=0 to
/// force all DBs/EDTs to rank 0.
///
/// argv: [blocks] [timesteps] [compute_weight] [staged_epochs] [distributed]
/// defaults: 512 blocks, 12 timesteps, 0 compute weight, staged epochs on,
/// distributed ownership on.

#include "arts.h"
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#define DEFAULT_BLOCKS 512
#define DEFAULT_TIMESTEPS 12
#define DEFAULT_WEIGHT 0
#define DEFAULT_STAGED_EPOCHS 1
#define DEFAULT_DISTRIBUTED 1

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static uint64_t busy_compute(uint64_t seed, uint64_t weight) {
  uint64_t acc = 0;
  for (uint64_t i = 0; i < weight; ++i)
    acc += (seed ^ (i * 0x9e3779b97f4a7c15ULL)) & 1ULL;
  return acc - acc;
}

void init_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t *cell = (uint64_t *)depv[0].ptr;
  if (!cell)
    abort();
  cell[0] = paramv[0];
}

void update_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t weight = paramv[0];
  uint64_t *next = (uint64_t *)depv[0].ptr;
  const uint64_t *self = (const uint64_t *)depv[1].ptr;
  const uint64_t *right = (const uint64_t *)depv[2].ptr;
  if (!next || !self || !right)
    abort();
  next[0] = self[0] + right[0] + busy_compute(self[0], weight);
}

void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  uint64_t blocks = paramv[0];
  uint64_t timesteps = paramv[1];
  uint64_t start = paramv[2];
  uint64_t got = 0;
  for (uint32_t i = 0; i < depc; ++i) {
    const uint64_t *value = (const uint64_t *)depv[i].ptr;
    if (!value)
      abort();
    got += value[0];
  }

  if (timesteps >= 64 || blocks > (UINT64_MAX >> timesteps)) {
    arts_printf("  FAIL: cdag_wave expected checksum overflow blocks=%" PRIu64
                " steps=%" PRIu64 "\n",
                blocks, timesteps);
    abort();
  }

  uint64_t expected = blocks << timesteps;
  uint64_t elapsed = now_ns() - start;
  double seconds = (double)elapsed / 1000000000.0;
  if (seconds == 0.0)
    seconds = 0.000000001;
  double edt_count = (double)(blocks * timesteps);
  double dep_count = edt_count * 3.0 + (double)blocks;
  if (got != expected) {
    arts_printf("  FAIL: cdag_wave blocks=%" PRIu64 " steps=%" PRIu64
                " checksum=%" PRIu64 " expected=%" PRIu64 "\n",
                blocks, timesteps, got, expected);
    abort();
  }

  arts_printf("  PASS: cdag_wave blocks=%" PRIu64 " steps=%" PRIu64
              " checksum=%" PRIu64 " elapsed=%.6f s edt_rate=%.3f/s"
              " dep_rate=%.3f/s\n",
              blocks, timesteps, got, seconds, edt_count / seconds,
              dep_count / seconds);
}

static void wait_or_abort(arts_guid_t epoch, const char *stage,
                          uint64_t step) {
  bool ok = arts_wait_on_handle(epoch);
  if (!ok) {
    arts_printf("  FAIL: cdag_wave epoch wait failed at %s step=%" PRIu64
                "\n",
                stage, step);
    abort();
  }
}

static void launch_step(arts_guid_t epoch, arts_guid_t *cur, arts_guid_t *next,
                        uint64_t blocks, uint64_t weight, unsigned int nodes,
                        bool distributed) {
  for (uint64_t i = 0; i < blocks; ++i) {
    unsigned int owner = distributed ? (unsigned int)(i % nodes) : 0U;
    uint64_t pv[1] = {weight};
    arts_guid_t edt = arts_edt_create_with_epoch(
        update_edt, 1, pv, 3, epoch, &(arts_hint_t){.route = owner});
    arts_add_dependence(next[i], edt, 0, DB_MODE_EW);
    arts_add_dependence(cur[i], edt, 1, DB_MODE_RO);
    arts_add_dependence(cur[(i + 1) % blocks], edt, 2, DB_MODE_RO);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;

  uint64_t blocks = DEFAULT_BLOCKS;
  uint64_t timesteps = DEFAULT_TIMESTEPS;
  uint64_t weight = DEFAULT_WEIGHT;
  uint64_t staged_epochs = DEFAULT_STAGED_EPOCHS;
  uint64_t distributed_arg = DEFAULT_DISTRIBUTED;
  if (paramc >= 2) {
    char **argv = (char **)paramv[1];
    if (argv) {
      if (argv[1]) {
        uint64_t v = strtoull(argv[1], NULL, 10);
        if (v > 0)
          blocks = v;
      }
      if (argv[2]) {
        uint64_t v = strtoull(argv[2], NULL, 10);
        if (v > 0)
          timesteps = v;
      }
      if (argv[3])
        weight = strtoull(argv[3], NULL, 10);
      if (argv[4])
        staged_epochs = strtoull(argv[4], NULL, 10) ? 1 : 0;
      if (argv[5])
        distributed_arg = strtoull(argv[5], NULL, 10) ? 1 : 0;
    }
  }
  if (timesteps > 50)
    timesteps = 50;
  if (blocks > UINT32_MAX) {
    arts_printf("  FAIL: cdag_wave blocks=%" PRIu64
                " exceeds uint32_t dependence slots\n",
                blocks);
    abort();
  }
  if (timesteps >= 64 || blocks > (UINT64_MAX >> timesteps)) {
    arts_printf("  FAIL: cdag_wave requested checksum would overflow blocks=%" PRIu64
                " steps=%" PRIu64 "\n",
                blocks, timesteps);
    abort();
  }

  unsigned int nodes = arts_get_total_nodes();
  if (nodes == 0)
    nodes = 1;
  bool distributed = distributed_arg && nodes > 1;

  arts_printf("=== cdag_wave_scaling blocks=%" PRIu64 " steps=%" PRIu64
              " weight=%" PRIu64 " staged_epochs=%" PRIu64
              " distributed=%u nodes=%u workers=%u ===\n",
              blocks, timesteps, weight, staged_epochs, distributed ? 1U : 0U,
              nodes,
              arts_get_total_workers());

  arts_guid_t *buf_a = (arts_guid_t *)malloc(sizeof(arts_guid_t) * blocks);
  arts_guid_t *buf_b = (arts_guid_t *)malloc(sizeof(arts_guid_t) * blocks);
  if (!buf_a || !buf_b)
    abort();

  for (uint64_t i = 0; i < blocks; ++i) {
    void *pa = NULL;
    void *pb = NULL;
    unsigned int owner = distributed ? (unsigned int)(i % nodes) : 0U;
    buf_a[i] = arts_db_create(&pa, sizeof(uint64_t), ARTS_DB_DEFAULT,
                              &(arts_hint_t){.route = owner});
    buf_b[i] = arts_db_create(&pb, sizeof(uint64_t), ARTS_DB_DEFAULT,
                              &(arts_hint_t){.route = owner});
    (void)pa;
    (void)pb;
  }

  uint64_t start = now_ns();
  arts_guid_t init_epoch = arts_initialize_and_start_epoch(NULL_GUID, 0);
  for (uint64_t i = 0; i < blocks; ++i) {
    unsigned int owner = distributed ? (unsigned int)(i % nodes) : 0U;
    uint64_t one = 1;
    arts_guid_t init_a = arts_edt_create_with_epoch(
        init_edt, 1, &one, 1, init_epoch, &(arts_hint_t){.route = owner});
    arts_add_dependence(buf_a[i], init_a, 0, DB_MODE_EW);

    uint64_t zero = 0;
    arts_guid_t init_b = arts_edt_create_with_epoch(
        init_edt, 1, &zero, 1, init_epoch, &(arts_hint_t){.route = owner});
    arts_add_dependence(buf_b[i], init_b, 0, DB_MODE_EW);
  }
  wait_or_abort(init_epoch, "init", 0);

  arts_guid_t *cur = buf_a;
  arts_guid_t *next = buf_b;

  if (staged_epochs) {
    for (uint64_t t = 0; t < timesteps; ++t) {
      arts_guid_t epoch = arts_initialize_and_start_epoch(NULL_GUID, 0);
      launch_step(epoch, cur, next, blocks, weight, nodes, distributed);
      wait_or_abort(epoch, "step", t);
      arts_guid_t *tmp = cur;
      cur = next;
      next = tmp;
    }
  } else {
    arts_guid_t epoch = arts_initialize_and_start_epoch(NULL_GUID, 0);
    for (uint64_t t = 0; t < timesteps; ++t) {
      launch_step(epoch, cur, next, blocks, weight, nodes, distributed);
      arts_guid_t *tmp = cur;
      cur = next;
      next = tmp;
    }
    wait_or_abort(epoch, "single_epoch_wave", timesteps);
  }

  arts_guid_t verify_epoch = arts_initialize_and_start_epoch(NULL_GUID, 0);
  uint64_t vpv[3] = {blocks, timesteps, start};
  arts_guid_t verifier = arts_edt_create_with_epoch(
      verify_edt, 3, vpv, (uint32_t)blocks, verify_epoch,
      &(arts_hint_t){.route = 0});
  for (uint64_t i = 0; i < blocks; ++i)
    arts_add_dependence(cur[i], verifier, (uint32_t)i, DB_MODE_RO);
  wait_or_abort(verify_epoch, "verify", timesteps);

  free(buf_a);
  free(buf_b);
  arts_shutdown();
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return 0;
}
