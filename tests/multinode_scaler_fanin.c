/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
******************************************************************************/

/// @file multinode_scaler_fanin.c
/// @brief ARTS-only scaler/fan-in stressor, with no CARTS compiler in the path.
///
/// The graph intentionally mirrors the communication shape that atax/bicg use
/// after CARTS lowering: block-distributed DBs, stage-by-stage epochs, and many
/// owner-routed EDTs that read a local block plus a neighboring remote block
/// before writing the next block buffer.  The arithmetic is simple and exact so
/// runtime progress, timeout, or checksum failures are easy to attribute.
///
/// argv: [blocks] [elements_per_block] [iterations] [peer_offset]
/// defaults: 128 blocks, 16 elements, 4 iterations, peer_offset=1.

#include "arts.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>

#define DEFAULT_BLOCKS 128
#define DEFAULT_ELEMS 16
#define DEFAULT_ITERS 4
#define DEFAULT_PEER_OFFSET 1

static uint64_t expected_sum(uint64_t blocks, uint64_t elems, uint64_t iters) {
  uint64_t total = 0;
  for (uint64_t block = 0; block < blocks; ++block) {
    for (uint64_t elem = 0; elem < elems; ++elem)
      total += block + elem + 1;
  }
  uint64_t count = blocks * elems;
  for (uint64_t iter = 0; iter < iters; ++iter)
    total = 2 * (total + count * (iter + 1));
  return total;
}

void init_block_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t block = paramv[0];
  uint64_t elems = paramv[1];
  uint64_t *out = (uint64_t *)depv[0].ptr;
  if (!out)
    abort();
  for (uint64_t elem = 0; elem < elems; ++elem)
    out[elem] = block + elem + 1;
}

void produce_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t elems = paramv[1];
  uint64_t iter = paramv[2];
  uint64_t *partial = (uint64_t *)depv[0].ptr;
  const uint64_t *cur = (const uint64_t *)depv[1].ptr;
  if (!partial || !cur)
    abort();
  for (uint64_t elem = 0; elem < elems; ++elem)
    partial[elem] = cur[elem] + iter + 1;
}

void fanin_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t elems = paramv[2];
  uint64_t *next = (uint64_t *)depv[0].ptr;
  const uint64_t *self = (const uint64_t *)depv[1].ptr;
  const uint64_t *peer = (const uint64_t *)depv[2].ptr;
  if (!next || !self || !peer)
    abort();
  for (uint64_t elem = 0; elem < elems; ++elem)
    next[elem] = self[elem] + peer[elem];
}

void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  uint64_t blocks = paramv[0];
  uint64_t elems = paramv[1];
  uint64_t iters = paramv[2];
  uint64_t got = 0;
  for (uint32_t block = 0; block < depc; ++block) {
    const uint64_t *data = (const uint64_t *)depv[block].ptr;
    if (!data)
      abort();
    for (uint64_t elem = 0; elem < elems; ++elem)
      got += data[elem];
  }
  uint64_t expected = expected_sum(blocks, elems, iters);
  if (got != expected) {
    arts_printf("  FAIL: scaler_fanin blocks=%" PRIu64 " elems=%" PRIu64
                " iters=%" PRIu64 " checksum=%" PRIu64
                " expected=%" PRIu64 "\n",
                blocks, elems, iters, got, expected);
    abort();
  }
  arts_printf("  PASS: scaler_fanin blocks=%" PRIu64 " elems=%" PRIu64
              " iters=%" PRIu64 " checksum=%" PRIu64 "\n",
              blocks, elems, iters, got);
}

void shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}

static arts_guid_t *create_block_array(uint64_t blocks, uint64_t elems,
                                       unsigned int nodes) {
  arts_guid_t *guids = (arts_guid_t *)malloc(sizeof(arts_guid_t) * blocks);
  if (!guids)
    abort();
  for (uint64_t block = 0; block < blocks; ++block) {
    void *ptr = NULL;
    unsigned int owner = (unsigned int)(block % nodes);
    guids[block] =
        arts_db_create(&ptr, elems * sizeof(uint64_t), ARTS_DB_DEFAULT,
                       &(arts_hint_t){.route = owner});
    arts_db_release(guids[block]);
  }
  return guids;
}

static void wait_or_abort(arts_guid_t epoch, const char *stage,
                          uint64_t iter) {
  bool ok = arts_wait_on_handle(epoch);
  if (!ok) {
    arts_printf("  FAIL: scaler_fanin epoch wait failed at %s iter=%" PRIu64
                "\n",
                stage, iter);
    abort();
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;

  uint64_t blocks = DEFAULT_BLOCKS;
  uint64_t elems = DEFAULT_ELEMS;
  uint64_t iters = DEFAULT_ITERS;
  uint64_t peerOffset = DEFAULT_PEER_OFFSET;
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
          elems = v;
      }
      if (argv[3]) {
        uint64_t v = strtoull(argv[3], NULL, 10);
        if (v > 0)
          iters = v;
      }
      if (argv[4]) {
        uint64_t v = strtoull(argv[4], NULL, 10);
        if (v > 0)
          peerOffset = v;
      }
    }
  }

  unsigned int nodes = arts_get_total_nodes();
  if (nodes == 0)
    nodes = 1;
  peerOffset %= blocks;
  if (peerOffset == 0 && blocks > 1)
    peerOffset = 1;

  arts_printf("=== multinode_scaler_fanin blocks=%" PRIu64 " elems=%" PRIu64
              " iters=%" PRIu64 " peer_offset=%" PRIu64 " nodes=%u ===\n",
              blocks, elems, iters, peerOffset, nodes);

  arts_guid_t *bufA = create_block_array(blocks, elems, nodes);
  arts_guid_t *bufB = create_block_array(blocks, elems, nodes);
  arts_guid_t *partial = create_block_array(blocks, elems, nodes);

  arts_guid_t initEpoch = arts_initialize_and_start_epoch(NULL_GUID, 0);
  for (uint64_t block = 0; block < blocks; ++block) {
    unsigned int owner = (unsigned int)(block % nodes);
    uint64_t pv[2] = {block, elems};
    arts_guid_t edt = arts_edt_create_with_epoch(
        init_block_edt, 2, pv, 1, initEpoch, &(arts_hint_t){.route = owner});
    arts_add_dependence(bufA[block], edt, 0, DB_MODE_EW);
  }
  wait_or_abort(initEpoch, "init", 0);
  arts_printf("  progress: init complete\n");

  arts_guid_t *cur = bufA;
  arts_guid_t *next = bufB;
  for (uint64_t iter = 0; iter < iters; ++iter) {
    arts_guid_t produceEpoch = arts_initialize_and_start_epoch(NULL_GUID, 0);
    for (uint64_t block = 0; block < blocks; ++block) {
      unsigned int owner = (unsigned int)(block % nodes);
      uint64_t pv[3] = {block, elems, iter};
      arts_guid_t edt = arts_edt_create_with_epoch(
          produce_edt, 3, pv, 2, produceEpoch,
          &(arts_hint_t){.route = owner});
      arts_add_dependence(partial[block], edt, 0, DB_MODE_EW);
      arts_add_dependence(cur[block], edt, 1, DB_MODE_RO);
    }
    wait_or_abort(produceEpoch, "produce", iter);

    arts_guid_t faninEpoch = arts_initialize_and_start_epoch(NULL_GUID, 0);
    for (uint64_t block = 0; block < blocks; ++block) {
      uint64_t peer = (block + peerOffset) % blocks;
      unsigned int owner = (unsigned int)(block % nodes);
      uint64_t pv[4] = {block, peer, elems, iter};
      arts_guid_t edt = arts_edt_create_with_epoch(
          fanin_edt, 4, pv, 3, faninEpoch, &(arts_hint_t){.route = owner});
      arts_add_dependence(next[block], edt, 0, DB_MODE_EW);
      arts_add_dependence(partial[block], edt, 1, DB_MODE_RO);
      arts_add_dependence(partial[peer], edt, 2, DB_MODE_RO);
    }
    wait_or_abort(faninEpoch, "fanin", iter);

    arts_guid_t *tmp = cur;
    cur = next;
    next = tmp;
    arts_printf("  progress: iter=%" PRIu64 " complete\n", iter + 1);
  }

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1,
                                     &(arts_hint_t){.route = 0});
  arts_guid_t verifyEpoch = arts_initialize_and_start_epoch(shut, 0);
  uint64_t vpv[3] = {blocks, elems, iters};
  arts_guid_t verifier = arts_edt_create_with_epoch(
      verify_edt, 3, vpv, (uint32_t)blocks, verifyEpoch,
      &(arts_hint_t){.route = 0});
  for (uint64_t block = 0; block < blocks; ++block)
    arts_add_dependence(cur[block], verifier, (uint32_t)block, DB_MODE_RO);

  free(bufA);
  free(bufB);
  free(partial);
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return 0;
}
