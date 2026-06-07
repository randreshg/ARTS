/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
******************************************************************************/

/// @file multinode_scaling.c
/// @brief Distributed iterative 1D stencil — CARTS-shaped scaling stress.
///
/// Replicates the CDAG a CARTS-lowered jacobi-style stencil generates: N cells
/// double-buffered across two DB arrays (cur/nxt), block-distributed over the
/// nodes (cell i owned by node i%nodes). Each timestep, cell i is updated by an
/// owner-local EDT that reads its own cur (RO) and its right neighbour's cur
/// (RO, a cross-node halo when the neighbour is owned by another node) and
/// writes nxt (EW). Buffers ping-pong; after T steps a verifier on node 0 reads
/// every cell RO and checks the global checksum against the closed-form
/// reference  S_T = N * 2^T.
///
/// TWO modes, selected by the STENCIL_FRESH env var:
///   - default (ping-pong): the two buffers are reused, so each DB cycles
///     RO -> EW -> RO across timesteps and each cell EDT is MIXED-MODE (one EW
///     dep + two RO deps on reused DBs at different generations).
///   - STENCIL_FRESH=1: a fresh DB is allocated per (timestep, cell), so every
///     DB is written exactly once then read — no RO->EW reuse anywhere.
///
/// KNOWN PRE-EXISTING BUG (reproduced by the default ping-pong mode, NOT
/// introduced by the cross-node coherence work; confirmed present at the
/// pre-O11 base bd7b004): with reused buffers the checksum is wrong/flaky once
/// the graph is non-trivial (e.g. N>=16 at 20 workers flakes 64/70/82; with a
/// single worker it is deterministically wrong). The STENCIL_FRESH variant of
/// the SAME computation is correct for every N,T on 1 and 20 workers, which
/// isolates the fault to a mixed-mode EDT writing a reused DB (EW generation)
/// before that DB's prior RO generation has drained. A pure-EW writer
/// (cdag_multi_reader) and single-DB RO->EW cycling are both correct, so the
/// trigger is specifically EW+RO on the SAME EDT against reused DBs.
///
/// This test is therefore a characterization/reproducer; it is intentionally
/// NOT in the always-green auto-suite. The cross-node halo coherence itself is
/// covered (and passing) by multinode_stencil_halo.
///
/// argv: [N cells] [T timesteps] [W compute weight]   (defaults 16 8 0)
/// Requires ARTS_CONFIG with the desired node_count.

#include "arts.h"
#include <stdlib.h>

#define DEFAULT_N 16
#define DEFAULT_T 8
#define DEFAULT_W 0

// Update rule: nxt[i] = cur[i] + cur[(i+1)%N], plus W units of busy compute
// folded in (and cancelled) so the closed-form reference stays exact while the
// EDT carries a realistic per-cell compute cost.
static long busy_compute(long seed, int w) {
  long acc = 0;
  for (int k = 0; k < w; k++) acc += (seed ^ k) & 1;
  // acc is bounded noise; fold then unfold so it does not perturb the result.
  return acc - acc;
}

// paramv[0] = N, paramv[1] = i, paramv[2] = W.
// depv[0] = nxt[i] (EW), depv[1] = cur[i] (RO), depv[2] = cur[(i+1)%N] (RO).
void init_cell(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  long *cell = (long *)depv[0].ptr;
  if (cell) {
    cell[0] = (long)paramv[0];
  }
}

void cell_update(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  int w = (int)paramv[2];
  long *nxt = (long *)depv[0].ptr;
  const long *self = (const long *)depv[1].ptr;
  const long *right = (const long *)depv[2].ptr;
  if (nxt && self && right) {
    nxt[0] = self[0] + right[0] + busy_compute(self[0], w);
  }
}

// Reads every current-buffer cell RO and checks the global sum. paramv[0]=N,
// paramv[1]=T. depv[i] = cur[i] (RO).
void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  int N = (int)paramv[0];
  int T = (int)paramv[1];
  (void)depc;

  // Closed form: start cur[i]=1. Each step nxt[i]=cur[i]+cur[i+1]. The global
  // sum S_t = sum_i cur_t[i] doubles each step (each value contributes to
  // itself and its left neighbour), so S_T = N * 2^T.
  long long got = 0;
  for (int i = 0; i < N; i++) {
    const long *c = (const long *)depv[i].ptr;
    long v = c ? c[0] : 0;
    got += v;
    if (getenv("STENCIL_DUMP")) arts_printf("    cell[%d]=%ld\n", i, v);
  }
  long long expected = (long long)N * (1LL << T);
  if (got == expected) {
    arts_printf("  PASS: scaling stencil N=%d T=%d checksum=%lld\n", N, T, got);
  } else {
    arts_printf("  FAIL: scaling stencil N=%d T=%d checksum=%lld expected=%lld\n",
                N, T, got, expected);
    abort();
  }
}

void shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;

  int N = DEFAULT_N, T = DEFAULT_T, W = DEFAULT_W;
  if (paramc >= 2) {
    char **argv = (char **)paramv[1];
    if (argv) {
      if (argv[1]) { int v = atoi(argv[1]); if (v > 0) N = v; }
      if (argv[2]) { int v = atoi(argv[2]); if (v > 0) T = v; }
      if (argv[3]) { int v = atoi(argv[3]); if (v >= 0) W = v; }
    }
  }
  if (T > 40) T = 40; // keep N*2^T inside long long

  unsigned int nodes = arts_get_total_nodes();
  arts_printf("=== multinode_scaling N=%d T=%d W=%d nodes=%u ===\n", N, T, W,
              nodes);

  arts_guid_t shut = arts_edt_create(shutdown_edt, 0, NULL, 1, NULL);
  arts_guid_t epoch = arts_initialize_and_start_epoch(shut, 0);

  // Two block-distributed buffers. Cell i owned by node i%nodes.
  arts_guid_t *bufA = (arts_guid_t *)malloc(sizeof(arts_guid_t) * N);
  arts_guid_t *bufB = (arts_guid_t *)malloc(sizeof(arts_guid_t) * N);
  for (int i = 0; i < N; i++) {
    void *pa = NULL, *pb = NULL;
    unsigned int owner = (unsigned int)i % nodes;
    bufA[i] = arts_db_create(&pa, sizeof(long), ARTS_DB_DEFAULT,
                             &(arts_hint_t){.route = owner});
    bufB[i] = arts_db_create(&pb, sizeof(long), ARTS_DB_DEFAULT,
                             &(arts_hint_t){.route = owner});
  }
  for (int i = 0; i < N; i++) {
    unsigned int owner = (unsigned int)i % nodes;
    uint64_t one = 1;
    arts_guid_t ia = arts_edt_create_with_epoch(
        init_cell, 1, &one, 1, epoch, &(arts_hint_t){.route = owner});
    arts_add_dependence(bufA[i], ia, 0, DB_MODE_EW);

    uint64_t zero = 0;
    arts_guid_t ib = arts_edt_create_with_epoch(
        init_cell, 1, &zero, 1, epoch, &(arts_hint_t){.route = owner});
    arts_add_dependence(bufB[i], ib, 0, DB_MODE_EW);
  }

  arts_guid_t *cur = bufA, *nxt = bufB;

  if (getenv("STENCIL_FRESH")) {
    // Discriminator mode: fresh DB per (timestep, cell) — NO buffer reuse, so
    // every DB is written exactly once (EW) then read (RO). This is an
    // unambiguously correct DAG with no RO->EW reuse on any DB. If this passes
    // while ping-pong fails, the fault is multi-generation DB reuse.
    arts_guid_t *prev = (arts_guid_t *)malloc(sizeof(arts_guid_t) * N);
    for (int i = 0; i < N; i++) prev[i] = bufA[i]; // level 0 (=1)
    for (int t = 0; t < T; t++) {
      arts_guid_t *level = (arts_guid_t *)malloc(sizeof(arts_guid_t) * N);
      for (int i = 0; i < N; i++) {
        void *p = NULL;
        unsigned int owner = (unsigned int)i % nodes;
        level[i] = arts_db_create(&p, sizeof(long), ARTS_DB_DEFAULT,
                                  &(arts_hint_t){.route = owner});
      }
      for (int i = 0; i < N; i++) {
        unsigned int owner = (unsigned int)i % nodes;
        uint64_t pv[3] = {(uint64_t)N, (uint64_t)i, (uint64_t)W};
        arts_guid_t e = arts_edt_create_with_epoch(
            cell_update, 3, pv, 3, epoch, &(arts_hint_t){.route = owner});
        arts_add_dependence(level[i], e, 0, DB_MODE_EW);
        arts_add_dependence(prev[i], e, 1, DB_MODE_RO);
        arts_add_dependence(prev[(i + 1) % N], e, 2, DB_MODE_RO);
      }
      free(prev);
      prev = level;
    }
    cur = prev; // final level
  } else {
    // Ping-pong T timesteps, reusing the two buffers (RO->EW->RO per DB).
    for (int t = 0; t < T; t++) {
      for (int i = 0; i < N; i++) {
        unsigned int owner = (unsigned int)i % nodes;
        uint64_t pv[3] = {(uint64_t)N, (uint64_t)i, (uint64_t)W};
        arts_guid_t e = arts_edt_create_with_epoch(
            cell_update, 3, pv, 3, epoch, &(arts_hint_t){.route = owner});
        int righti = (i + 1) % N;
        arts_add_dependence(nxt[i], e, 0, DB_MODE_EW);       // write nxt[i]
        arts_add_dependence(cur[i], e, 1, DB_MODE_RO);       // read self
        arts_add_dependence(cur[righti], e, 2, DB_MODE_RO);  // read right halo
      }
      arts_guid_t *tmp = cur; cur = nxt; nxt = tmp;
    }
  }

  // Verifier on node 0 reads the final buffer.
  uint64_t vpv[2] = {(uint64_t)N, (uint64_t)T};
  arts_guid_t v = arts_edt_create_with_epoch(verify_edt, 2, vpv, N, epoch,
                                             &(arts_hint_t){.route = 0});
  for (int i = 0; i < N; i++) {
    arts_add_dependence(cur[i], v, (uint32_t)i, DB_MODE_RO);
  }

  free(bufA);
  free(bufB);
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return 0;
}
