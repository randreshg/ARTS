/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_size_round_once — a rank pays at most one size round per cache.
 *
 * A request from a cache that does not know the block's size carries no
 * landing, and its home answers with the size alone (a size round) before
 * the request is served.  A GUID a create minted carries the block's size, so
 * its caches never pay one; a reserved GUID carries none, so a rank's cache
 * of it pays one, after which it knows the size.  Per block homed on the
 * first rank and created there, the turn alternates between the second rank
 * and the first for 16 rounds: with `writes` the second rank takes a write
 * turn every round, with `reads` a read turn (the first rank writing in
 * between), so the second rank sends a fresh request every round.  The size
 * rounds the home sent are read back from the first rank's counter file (the
 * sum of NUM_GRANT_SIZE_CTS, NUM_SNAPSHOT_SIZE_CTS, NUM_EXCL_SIZE_CTS and
 * NUM_INV_SIZE_CTS): at most one per block for `reserved`, none for `minted`.
 * Registered only where the tree's counter config captures those counters.
 *
 * Arguments: reserved|minted writes|reads.  Needs two ranks.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arts.h"
#include "../test_failure_status.h"

#define NBLOCKS 8u
#define NROUNDS 16u

static arts_guid_t edt_on(arts_edt_t fn, unsigned int rank, uint32_t paramc,
                          const uint64_t *paramv, uint32_t depc) {
  arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
  h.rank = rank;
  return arts_edt_create(fn, paramc, paramv, depc, &h);
}

/* paramv: g, step, steps, all, reads */
static void step(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  if (depv[0].ptr == NULL) {
    arts_printf("FAIL: db_size_round_once got no storage\n");
    arts_test_fail();
    return;
  }
  if (depv[0].mode == DB_MODE_RW) {
    atomic_fetch_add_explicit((_Atomic(uint32_t) *)depv[0].ptr, 1u,
                              memory_order_relaxed);
  } else {
    (void)atomic_load_explicit((_Atomic(uint32_t) *)depv[0].ptr,
                               memory_order_relaxed);
  }
  arts_guid_t g = (arts_guid_t)paramv[0];
  uint64_t s = paramv[1] + 1u;
  if (s < paramv[2]) {
    uint64_t pv[5] = {paramv[0], s, paramv[2], paramv[3], paramv[4]};
    unsigned int rank = (s & 1u) ? 0u : 1u;
    arts_guid_t nx = edt_on(step, rank, 5, pv, 1);
    arts_add_dependence(g, nx, 0,
                        (rank == 1u && paramv[4]) ? DB_MODE_RO : DB_MODE_RW);
  } else {
    arts_db_destroy(g);
    arts_event_satisfy((arts_guid_t)paramv[3], NULL_GUID);
  }
}

/* runs on the first rank. paramv: reserved g or 0, steps, all, reads */
static void maker(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  void *p = NULL;
  if (g != NULL_GUID) {
    p = arts_db_create_with_guid(g, 4096u, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  } else {
    g = arts_db_create(&p, 4096u, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  }
  atomic_store_explicit((_Atomic(uint32_t) *)p, 0u, memory_order_relaxed);
  arts_db_release(g, DB_MODE_RW);
  uint64_t pv[5] = {(uint64_t)g, 0u, paramv[1], paramv[2], paramv[3]};
  arts_guid_t first = edt_on(step, 1u, 5, pv, 1);
  arts_add_dependence(g, first, 0, paramv[3] ? DB_MODE_RO : DB_MODE_RW);
}

static void terminal(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}

static bool g_reserved = true, g_reads = false;

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  if (arts_get_total_ranks() < 2u) {
    arts_printf("FAIL: db_size_round_once needs two ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  (void)paramv;
  arts_event_hint_t lh = ARTS_EVENT_HINT_LATCH((int32_t)NBLOCKS);
  lh.rank = 0u;
  arts_guid_t all = arts_event_create(&lh);
  arts_guid_t t = edt_on(terminal, 0u, 0, NULL, 1);
  arts_add_dependence(all, t, 0, DB_MODE_NULL);
  for (unsigned int b = 0; b < NBLOCKS; b++) {
    arts_guid_t g =
        g_reserved ? arts_guid_reserve(ARTS_GUID_DB, 0u) : NULL_GUID;
    uint64_t pv[4] = {(uint64_t)g, 2u * NROUNDS, (uint64_t)all,
                      g_reads ? 1u : 0u};
    (void)edt_on(maker, 0u, 4, pv, 0);
  }
}

/* Adds the "value" of counter object `name` in the JSON to *sum; false when
 * the file carries no such counter. */
static bool counter_value(const char *json, const char *name, uint64_t *sum) {
  char key[128];
  (void)snprintf(key, sizeof(key), "\"%s\"", name);
  const char *obj = strstr(json, key);
  if (obj == NULL) {
    return false;
  }
  const char *v = strstr(obj, "\"value\"");
  if (v == NULL) {
    return false;
  }
  v = strchr(v + strlen("\"value\""), ':');
  if (v == NULL) {
    return false;
  }
  *sum += strtoull(v + 1, NULL, 10);
  return true;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "minted") == 0) {
      g_reserved = false;
    } else if (strcmp(argv[i], "reads") == 0) {
      g_reads = true;
    }
  }
  if (arts_rt(argc, argv) != 0) {
    printf("FAIL: db_size_round_once: the runtime run failed\n");
    return 1;
  }
  if (arts_test_status() != 0) {
    return 1;
  }
  /* Only the first rank writes the counter file, after its runtime ends. */
  if (arts_get_current_rank() != 0u) {
    return 0;
  }
  FILE *fp = fopen("./counters/n0.json", "r");
  if (fp == NULL) {
    printf("FAIL: db_size_round_once: ./counters/n0.json not found\n");
    return 1;
  }
  (void)fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  (void)fseek(fp, 0, SEEK_SET);
  if (sz <= 0) {
    (void)fclose(fp);
    printf("FAIL: db_size_round_once: n0.json is empty\n");
    return 1;
  }
  char *buf = (char *)malloc((size_t)sz + 1);
  size_t got = fread(buf, 1, (size_t)sz, fp);
  (void)fclose(fp);
  buf[got] = '\0';
  uint64_t rounds = 0u;
  bool found = counter_value(buf, "NUM_GRANT_SIZE_CTS", &rounds);
  found |= counter_value(buf, "NUM_SNAPSHOT_SIZE_CTS", &rounds);
  found |= counter_value(buf, "NUM_EXCL_SIZE_CTS", &rounds);
  found |= counter_value(buf, "NUM_INV_SIZE_CTS", &rounds);
  free(buf);
  if (!found) {
    printf("FAIL: db_size_round_once: n0.json carries no size-round counter\n");
    return 1;
  }
  uint64_t limit = g_reserved ? NBLOCKS : 0u;
  if (rounds > limit) {
    printf("FAIL: db_size_round_once: %llu size rounds for %u %s blocks "
           "(%s), at most %llu\n",
           (unsigned long long)rounds, NBLOCKS,
           g_reserved ? "reserved" : "minted", g_reads ? "reads" : "writes",
           (unsigned long long)limit);
    return 1;
  }
  printf("PASS: db_size_round_once %llu size rounds for %u %s blocks (%s)\n",
         (unsigned long long)rounds, NBLOCKS,
         g_reserved ? "reserved" : "minted", g_reads ? "reads" : "writes");
  return 0;
}
