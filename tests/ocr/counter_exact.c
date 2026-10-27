/******************************************************************************
** Copyright 2019 Battelle Memorial Institute
** Licensed under the Apache License, Version 2.0
******************************************************************************/

/// @file counter_exact.c
/// @brief Exact counter values of a known program, read back from the
///        per-rank counter files.
///
/// Usage: counter_exact <shape> [<rank>:<COUNTER>=<value>]...
///
///   one    One rank.  Blocks of S bytes: K created with the creator's hold,
///          M with ARTS_DB_PROP_NO_ACQUIRE, Z of size zero with the hold, and
///          one more, NO_ACQUIRE, on a reserved GUID whose read consumer is
///          given its dependence before the create.  Each block is read by
///          one EDT and then, ordered after that EDT's release, written and
///          destroyed by another; one writer also names its block a second
///          time, read-only, which is the same acquisition.
///   two    Two ranks.  One block homed on rank 0 and created there with the
///          hold, then, each step ordered after the previous step's release:
///          N readers on rank 1, a writer on rank 1, a reader on rank 1, and
///          a reader on rank 0 that destroys the block.  Rank 0 also creates
///          two blocks homed on rank 1, one with the hold and one without,
///          which nobody acquires and the last step destroys.
///   home   Two ranks.  One block homed on rank 0 and created there with the
///          hold, then, each step ordered after the previous step's release:
///          a writer on rank 1, a reader on rank 0, and a writer on rank 0
///          that destroys the block.  Nothing orders rank 1's release
///          reaching the home before rank 0's request does.
///   recall Two ranks.  One block homed on rank 0 and created there with the
///          hold, then, the second step ordered after the first's release: a
///          reader on rank 1 and a writer on rank 0 that destroys the block.
///          The write right stays with rank 0 throughout, whatever the write
///          has to take back from rank 1.
///   claim  Two ranks.  An EDT on rank 1 gives a reader on rank 1 its
///          dependence on a GUID reserved on rank 0, which acquires it at
///          once, and then creates the block with the hold.  The last of the
///          two destroys it.
///   ping   Two ranks.  Rank 0 creates a block of P bytes homed there with
///          the hold and, after its release, an EDT on rank 1 that reads the
///          block and shuts the run down.
///   pub    Two ranks.  A reader on rank 0 is given its dependence on a GUID
///          reserved on rank 0, which it acquires at once, before an EDT on
///          rank 1 creates the block with the hold: the reader waits on the
///          block's home for that create and for the creator's first
///          publication.  The last of the two destroys the block.
///   join   Two ranks.  One block homed on rank 0 and created there with the
///          hold; after its release an EDT on rank 1 makes two readers of it
///          acquire there at once, the second while the first's request is
///          out.  The first, once it runs, makes a third reader acquire at
///          once while it still holds its own.  The last of them destroys
///          the block.
///   gate   Two ranks.  As join, except that a writer on rank 1 acquires
///          right after the two readers, so its request goes out while theirs
///          waits and the write turn serves all three; and the third
///          acquisition, made by the first reader once it runs, is a write:
///          the write right is on rank 1 by then, confirmed by the home or
///          not.  The last of them destroys the block.
///
/// Each expectation names the counter file of a rank (./counters/n<rank>.json,
/// written by that rank at its teardown) and the exact value the counter must
/// hold there, or <rank>:<COUNTER> whose value it must equal; the expected
/// values are the registration's, per cell.  Rank 0 reads every file once its
/// run, and the ranks it spawned, have ended.

#include "arts.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../test_failure_status.h"

#define S_BYTES 256u
#define K_HELD 4u
#define M_NOACQ 3u
#define Z_ZERO 2u
#define N_READERS 3u

enum {
  SHAPE_ONE,
  SHAPE_TWO,
  SHAPE_HOME,
  SHAPE_RECALL,
  SHAPE_CLAIM,
  SHAPE_PING,
  SHAPE_PUB,
  SHAPE_JOIN,
  SHAPE_GATE
};
static int g_shape = -1;

static arts_guid_t edt_on(arts_edt_t fn, unsigned int rank, uint32_t paramc,
                          const uint64_t *paramv, uint32_t depc,
                          arts_guid_t output) {
  arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
  h.rank = rank;
  h.output_event = output;
  return arts_edt_create(fn, paramc, paramv, depc, &h);
}

static arts_guid_t latch(int32_t n) {
  arts_event_hint_t h = ARTS_EVENT_HINT_LATCH(n);
  return arts_event_create(&h);
}

static void terminal(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}

/* ---- one ---------------------------------------------------------------- */

static void one_reader(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
}

/* paramv: the block */
static void one_writer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  if (depv[0].ptr != NULL) {
    memset(depv[0].ptr, 0x5a, S_BYTES);
  }
  arts_db_destroy((arts_guid_t)paramv[0]);
}

/* A read of `g`, then a write ordered after the read's release.  The writer's
 * output joins `done`.  With `alias` the writer names `g` once more,
 * read-only. */
static void one_chain(arts_guid_t g, arts_guid_t done, bool alias) {
  arts_guid_t after_read = latch(1);
  uint64_t pv[1] = {(uint64_t)g};
  arts_guid_t w = edt_on(one_writer, 0u, 1, pv, alias ? 3u : 2u, done);
  arts_add_dependence(g, w, 0, DB_MODE_RW);
  arts_add_dependence(after_read, w, 1, DB_MODE_NULL);
  if (alias) {
    arts_add_dependence(g, w, 2, DB_MODE_RO);
  }
  arts_guid_t r = edt_on(one_reader, 0u, 0, NULL, 1, after_read);
  arts_add_dependence(g, r, 0, DB_MODE_RO);
}

static void one_main(void) {
  const uint32_t blocks = K_HELD + M_NOACQ + Z_ZERO + 1u;
  arts_guid_t done = latch((int32_t)blocks);
  arts_guid_t t = edt_on(terminal, 0u, 0, NULL, 1, NULL_GUID);
  arts_add_dependence(done, t, 0, DB_MODE_NULL);

  arts_guid_t g[K_HELD + M_NOACQ + Z_ZERO];
  uint32_t n = 0;
  for (uint32_t i = 0; i < K_HELD; i++, n++) {
    void *p = NULL;
    g[n] = arts_db_create(&p, S_BYTES, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
    memset(p, 0, S_BYTES);
    arts_db_release(g[n], DB_MODE_RW);
  }
  for (uint32_t i = 0; i < M_NOACQ; i++, n++) {
    void *p = NULL;
    g[n] = arts_db_create(&p, S_BYTES, ARTS_DB, ARTS_DB_PROP_NO_ACQUIRE, NULL);
  }
  for (uint32_t i = 0; i < Z_ZERO; i++, n++) {
    void *p = NULL;
    g[n] = arts_db_create(&p, 0u, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
    arts_db_release(g[n], DB_MODE_RW);
  }
  for (uint32_t i = 0; i < n; i++) {
    one_chain(g[i], done, i == 0u);
  }
  arts_guid_t reserved = arts_guid_reserve(ARTS_GUID_DB, 0u);
  one_chain(reserved, done, false);
  (void)arts_db_create_with_guid(reserved, S_BYTES, ARTS_DB,
                                 ARTS_DB_PROP_NO_ACQUIRE, NULL);
}

/* ---- two ---------------------------------------------------------------- */

/* paramv: the block, whether this step is the last, and two more blocks the
 * last step destroys (NULL_GUID for none) */
static void two_step(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  if (depv[0].ptr == NULL) {
    arts_printf("FAIL: counter_exact got no storage\n");
    arts_test_fail();
  } else if (depv[0].mode == DB_MODE_RW) {
    memset(depv[0].ptr, 0x3c, S_BYTES);
  }
  if (paramv[1] != 0u) {
    arts_db_destroy((arts_guid_t)paramv[0]);
    for (uint32_t i = 2; i < 4u; i++) {
      if ((arts_guid_t)paramv[i] != NULL_GUID) {
        arts_db_destroy((arts_guid_t)paramv[i]);
      }
    }
    arts_shutdown();
  }
}

/* Steps over b on the given ranks and modes, each ordered after the previous
 * step's release; the last also destroys c and d. */
static void two_chain(arts_guid_t b, arts_guid_t c, arts_guid_t d, int steps,
                      const unsigned int *rank_of,
                      const arts_db_access_mode_t *mode_of) {
  arts_guid_t gate = NULL_GUID;
  for (int s = steps - 1; s >= 0; s--) {
    arts_guid_t prev_done = (s == 0) ? NULL_GUID : latch(1);
    uint64_t pv[4] = {(uint64_t)b, (s == steps - 1) ? 1u : 0u, (uint64_t)c,
                      (uint64_t)d};
    arts_guid_t e =
        edt_on(two_step, rank_of[s], 4, pv, (s == 0) ? 1u : 2u, gate);
    arts_add_dependence(b, e, 0, mode_of[s]);
    if (prev_done != NULL_GUID) {
      arts_add_dependence(prev_done, e, 1, DB_MODE_NULL);
    }
    gate = prev_done;
  }
}

static bool two_ranks(const char *shape) {
  if (arts_get_total_ranks() == 2u) {
    return true;
  }
  arts_printf("FAIL: counter_exact %s needs exactly two ranks\n", shape);
  arts_test_fail();
  arts_shutdown();
  return false;
}

static void two_main(void) {
  if (!two_ranks("two")) {
    return;
  }
  arts_db_hint_t home0 = {.rank = 0u};
  arts_db_hint_t home1 = {.rank = 1u};
  void *p = NULL;
  arts_guid_t b =
      arts_db_create(&p, S_BYTES, ARTS_DB, ARTS_DB_PROP_NONE, &home0);
  memset(p, 0, S_BYTES);
  arts_db_release(b, DB_MODE_RW);
  /* Created here, homed on rank 1: counted on this rank, never acquired. */
  arts_guid_t c =
      arts_db_create(&p, S_BYTES, ARTS_DB, ARTS_DB_PROP_NONE, &home1);
  memset(p, 0, S_BYTES);
  arts_db_release(c, DB_MODE_RW);
  arts_guid_t d =
      arts_db_create(&p, S_BYTES, ARTS_DB, ARTS_DB_PROP_NO_ACQUIRE, &home1);

  enum { STEPS = N_READERS + 3 };
  static const unsigned int rank_of[STEPS] = {1, 1, 1, 1, 1, 0};
  static const arts_db_access_mode_t mode_of[STEPS] = {
      DB_MODE_RO, DB_MODE_RO, DB_MODE_RO, DB_MODE_RW, DB_MODE_RO, DB_MODE_RO};
  two_chain(b, c, d, STEPS, rank_of, mode_of);
}

/* ---- home --------------------------------------------------------------- */

static void home_main(void) {
  if (!two_ranks("home")) {
    return;
  }
  arts_db_hint_t home0 = {.rank = 0u};
  void *p = NULL;
  arts_guid_t b =
      arts_db_create(&p, S_BYTES, ARTS_DB, ARTS_DB_PROP_NONE, &home0);
  memset(p, 0, S_BYTES);
  arts_db_release(b, DB_MODE_RW);
  static const unsigned int rank_of[3] = {1, 0, 0};
  static const arts_db_access_mode_t mode_of[3] = {DB_MODE_RW, DB_MODE_RO,
                                                   DB_MODE_RW};
  two_chain(b, NULL_GUID, NULL_GUID, 3, rank_of, mode_of);
}

/* ---- recall ------------------------------------------------------------- */

static void recall_main(void) {
  if (!two_ranks("recall")) {
    return;
  }
  arts_db_hint_t home0 = {.rank = 0u};
  void *p = NULL;
  arts_guid_t b =
      arts_db_create(&p, S_BYTES, ARTS_DB, ARTS_DB_PROP_NONE, &home0);
  memset(p, 0, S_BYTES);
  arts_db_release(b, DB_MODE_RW);
  static const unsigned int rank_of[2] = {1, 0};
  static const arts_db_access_mode_t mode_of[2] = {DB_MODE_RO, DB_MODE_RW};
  two_chain(b, NULL_GUID, NULL_GUID, 2, rank_of, mode_of);
}

/* ---- claim -------------------------------------------------------------- */

/* paramv: the block */
static void claim_end(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_db_destroy((arts_guid_t)paramv[0]);
  arts_shutdown();
}

/* paramv: the block, and the event both holders' ends join */
static void claim_create(uint32_t paramc, const uint64_t *paramv,
                         uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  arts_guid_t r = edt_on(one_reader, 1u, 0, NULL, 1, (arts_guid_t)paramv[1]);
  arts_add_dependence(g, r, 0, DB_MODE_RO);
  void *p = arts_db_create_with_guid(g, S_BYTES, ARTS_DB, ARTS_DB_PROP_NONE,
                                     NULL);
  memset(p, 0, S_BYTES);
  arts_db_release(g, DB_MODE_RW);
}

static void claim_main(void) {
  if (!two_ranks("claim")) {
    return;
  }
  arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0u);
  arts_guid_t done = latch(2);
  uint64_t pv[2] = {(uint64_t)g, (uint64_t)done};
  arts_guid_t t = edt_on(claim_end, 0u, 1, pv, 1, NULL_GUID);
  arts_add_dependence(done, t, 0, DB_MODE_NULL);
  (void)edt_on(claim_create, 1u, 2, pv, 0, done);
}

/* ---- pub ----------------------------------------------------------------- */

/* paramv: the block */
static void pub_create(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  (void)arts_db_create_with_guid(g, S_BYTES, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  arts_db_release(g, DB_MODE_RW);
}

static void pub_main(void) {
  if (!two_ranks("pub")) {
    return;
  }
  arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0u);
  arts_guid_t done = latch(2);
  uint64_t pv[1] = {(uint64_t)g};
  arts_guid_t t = edt_on(claim_end, 0u, 1, pv, 1, NULL_GUID);
  arts_add_dependence(done, t, 0, DB_MODE_NULL);
  arts_guid_t r = edt_on(one_reader, 0u, 0, NULL, 1, done);
  arts_add_dependence(g, r, 0, DB_MODE_RO);
  (void)edt_on(pub_create, 1u, 1, pv, 0, done);
}

/* ---- join, gate ---------------------------------------------------------- */

static void gate_write(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  /* The shape's two writers share their rank's write turn and run
   * unordered, each beside the other's release, so neither writes: the shape
   * counts acquisitions, not bytes. */
  if (depv[0].ptr == NULL) {
    arts_printf("FAIL: counter_exact got no storage\n");
    arts_test_fail();
  }
}

/* An EDT on this rank, ending into `done`, that acquires `g` in `mode` at
 * once: its only dependence is the block. */
static void acquire_here(arts_edt_t fn, arts_guid_t g, arts_guid_t done,
                         arts_db_access_mode_t mode) {
  uint64_t pv[2] = {(uint64_t)g, (uint64_t)done};
  arts_guid_t e = edt_on(fn, arts_get_current_rank(), 2, pv, 1, done);
  arts_add_dependence(g, e, 0, mode);
}

/* paramv: the block and the end event */
static void join_first(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  acquire_here(one_reader, (arts_guid_t)paramv[0], (arts_guid_t)paramv[1],
               DB_MODE_RO);
}

static void gate_first(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  acquire_here(gate_write, (arts_guid_t)paramv[0], (arts_guid_t)paramv[1],
               DB_MODE_RW);
}

/* paramv: the block and the end event */
static void join_start(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_guid_t g = (arts_guid_t)paramv[0];
  arts_guid_t done = (arts_guid_t)paramv[1];
  acquire_here(g_shape == SHAPE_GATE ? gate_first : join_first, g, done,
               DB_MODE_RO);
  acquire_here(one_reader, g, done, DB_MODE_RO);
  if (g_shape == SHAPE_GATE) {
    acquire_here(gate_write, g, done, DB_MODE_RW);
  }
}

static void join_main(const char *shape, int32_t acquisitions) {
  if (!two_ranks(shape)) {
    return;
  }
  arts_db_hint_t home0 = {.rank = 0u};
  void *p = NULL;
  arts_guid_t b =
      arts_db_create(&p, S_BYTES, ARTS_DB, ARTS_DB_PROP_NONE, &home0);
  memset(p, 0, S_BYTES);
  arts_db_release(b, DB_MODE_RW);
  arts_guid_t done = latch(acquisitions);
  uint64_t pv[2] = {(uint64_t)b, (uint64_t)done};
  arts_guid_t t = edt_on(claim_end, 0u, 1, pv, 1, NULL_GUID);
  arts_add_dependence(done, t, 0, DB_MODE_NULL);
  (void)edt_on(join_start, 1u, 2, pv, 0, NULL_GUID);
}

/* ---- ping ---------------------------------------------------------------- */

#define P_BYTES 65536u

static void ping_read(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  if (depv[0].ptr == NULL ||
      ((const unsigned char *)depv[0].ptr)[P_BYTES - 1u] != 0x77u) {
    arts_printf("FAIL: counter_exact ping read the wrong bytes\n");
    arts_test_fail();
  }
  arts_shutdown();
}

static void ping_main(void) {
  if (!two_ranks("ping")) {
    return;
  }
  arts_db_hint_t home0 = {.rank = 0u};
  void *p = NULL;
  arts_guid_t b =
      arts_db_create(&p, P_BYTES, ARTS_DB, ARTS_DB_PROP_NONE, &home0);
  memset(p, 0x77, P_BYTES);
  arts_db_release(b, DB_MODE_RW);
  arts_guid_t r = edt_on(ping_read, 1u, 0, NULL, 1, NULL_GUID);
  arts_add_dependence(b, r, 0, DB_MODE_RO);
}

/* -------------------------------------------------------------------------- */

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (g_shape == SHAPE_ONE) {
    one_main();
  } else if (g_shape == SHAPE_TWO) {
    two_main();
  } else if (g_shape == SHAPE_HOME) {
    home_main();
  } else if (g_shape == SHAPE_RECALL) {
    recall_main();
  } else if (g_shape == SHAPE_CLAIM) {
    claim_main();
  } else if (g_shape == SHAPE_PUB) {
    pub_main();
  } else if (g_shape == SHAPE_JOIN) {
    join_main("join", 3);
  } else if (g_shape == SHAPE_GATE) {
    join_main("gate", 4);
  } else {
    ping_main();
  }
}

static char *read_file(const char *path) {
  FILE *fp = fopen(path, "r");
  if (fp == NULL) {
    return NULL;
  }
  (void)fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  (void)fseek(fp, 0, SEEK_SET);
  if (sz <= 0) {
    (void)fclose(fp);
    return NULL;
  }
  char *buf = (char *)malloc((size_t)sz + 1);
  size_t got = fread(buf, 1, (size_t)sz, fp);
  (void)fclose(fp);
  buf[got] = '\0';
  return buf;
}

/* The "value" of counter object `name`; false when the file lacks it. */
static bool counter_value(const char *json, const char *name, uint64_t *out) {
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
  char *end = NULL;
  *out = strtoull(v + 1, &end, 10);
  return end != v + 1;
}

#define MAX_RANKS 2

static char *g_json[MAX_RANKS];

/* The value of `rank`'s counter `name`; false, with the reason printed, when
 * it cannot be read. */
static bool rank_value(unsigned int rank, const char *name, uint64_t *out) {
  if (rank >= MAX_RANKS) {
    printf("FAIL: counter_exact: no rank %u in this run\n", rank);
    return false;
  }
  if (g_json[rank] == NULL) {
    char path[64];
    (void)snprintf(path, sizeof(path), "./counters/n%u.json", rank);
    g_json[rank] = read_file(path);
    if (g_json[rank] == NULL) {
      printf("FAIL: counter_exact: %s not found or empty\n", path);
      return false;
    }
  }
  if (!counter_value(g_json[rank], name, out)) {
    printf("FAIL: counter_exact: rank %u's file carries no %s\n", rank, name);
    return false;
  }
  return true;
}

/* One expectation: <rank>:<COUNTER>=<value> or <rank>:<COUNTER>=<r>:<NAME>. */
static bool check(const char *shape, const char *expect) {
  unsigned int rank = 0;
  unsigned int other = 0;
  char name[96];
  char other_name[96];
  unsigned long long want = 0;
  int n = sscanf(expect, "%u:%95[A-Z0-9_]=%u:%95[A-Z0-9_]", &rank, name, &other,
                 other_name);
  uint64_t got = 0;
  if (n == 4) {
    uint64_t ref = 0;
    if (!rank_value(rank, name, &got) || !rank_value(other, other_name, &ref)) {
      return false;
    }
    if (got != ref) {
      printf("FAIL: counter_exact %s: rank %u %s = %llu, rank %u %s = %llu\n",
             shape, rank, name, (unsigned long long)got, other, other_name,
             (unsigned long long)ref);
      return false;
    }
    return true;
  }
  if (sscanf(expect, "%u:%95[A-Z0-9_]=%llu", &rank, name, &want) != 3) {
    printf("FAIL: counter_exact: bad expectation %s\n", expect);
    return false;
  }
  if (!rank_value(rank, name, &got)) {
    return false;
  }
  if (got != (uint64_t)want) {
    printf("FAIL: counter_exact %s: rank %u %s = %llu, expected %llu\n", shape,
           rank, name, (unsigned long long)got, want);
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("FAIL: counter_exact needs a shape\n");
    return 1;
  }
  if (strcmp(argv[1], "one") == 0) {
    g_shape = SHAPE_ONE;
  } else if (strcmp(argv[1], "two") == 0) {
    g_shape = SHAPE_TWO;
  } else if (strcmp(argv[1], "home") == 0) {
    g_shape = SHAPE_HOME;
  } else if (strcmp(argv[1], "recall") == 0) {
    g_shape = SHAPE_RECALL;
  } else if (strcmp(argv[1], "claim") == 0) {
    g_shape = SHAPE_CLAIM;
  } else if (strcmp(argv[1], "ping") == 0) {
    g_shape = SHAPE_PING;
  } else if (strcmp(argv[1], "pub") == 0) {
    g_shape = SHAPE_PUB;
  } else if (strcmp(argv[1], "join") == 0) {
    g_shape = SHAPE_JOIN;
  } else if (strcmp(argv[1], "gate") == 0) {
    g_shape = SHAPE_GATE;
  } else {
    printf("FAIL: counter_exact: unknown shape %s\n", argv[1]);
    return 1;
  }
  /* Files an earlier run left must not stand in for this run's. */
  for (int r = 0; r < MAX_RANKS; r++) {
    char path[64];
    (void)snprintf(path, sizeof(path), "./counters/n%d.json", r);
    (void)remove(path);
  }
  int rc = arts_rt(argc, argv);
  if (arts_get_current_rank() != 0u) {
    return rc ? 1 : arts_test_status();
  }
  if (rc != 0 || arts_test_status() != 0) {
    printf("FAIL: counter_exact %s: the runtime run failed\n", argv[1]);
    return 1;
  }
  int failures = 0;
  int checked = 0;
  for (int i = 2; i < argc; i++) {
    if (!check(argv[1], argv[i])) {
      failures++;
    }
    checked++;
  }
  for (int r = 0; r < MAX_RANKS; r++) {
    free(g_json[r]);
  }
  if (failures != 0) {
    return 1;
  }
  printf("PASS: counter_exact %s %d values\n", argv[1], checked);
  return 0;
}
