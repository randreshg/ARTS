/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* db_two_block_readers — two data blocks B and C on two ranks, each rank
 * writing one block while reading the other.
 *
 * Each round makes K EDTs of each of four kinds:
 *   X0 on rank 0: B read-write, C read-only;  Z0 on rank 0: C read-write;
 *   X1 on rank 1: C read-write, B read-only;  Z1 on rank 1: B read-write.
 * A writer writes a range of its read-write block that no other EDT touches;
 * a reader reads a range of its read-only block that no EDT writes, so the
 * program has no data race.  Every EDT's completion decrements one latch,
 * after its blocks are released, and the latch's firing runs the end, which
 * checks the blocks and shuts the run down.  Under VAL and INV a read
 * acquisition waits for no release, so an X0 holding B's write turn and an
 * X1 holding C's both get their reads whatever writers wait behind those
 * turns; under EXCL a read waits behind another node's write turn, and the
 * one order in which every EDT acquires its blocks keeps two such EDTs from
 * waiting on each other.
 *
 * Arguments: [rounds] [k] (20, 4).  Needs two ranks.  Its writers of a block
 * are unordered, which CDAG does not admit.
 */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "arts.h"
#include "../test_failure_status.h"

#define WORDS 32768u /* 256 KiB a block */
#define SPAN 16u     /* words a writer writes */
#define READ_BASE (WORDS - 2048u)
#define READ_WORDS 64u
#define READ_FILL 7u /* every word of the read window, from the create on */

enum { P_ROUND, P_INDEX, P_KIND, P_LATCH, P_B, P_C, P_ROUNDS, P_K, P_COUNT };
enum { KIND_X0, KIND_Z0, KIND_X1, KIND_Z1, KINDS };

/* The range a writer of `kind`, round `round`, index `index` writes: X0 and
 * Z1 share B, X1 and Z0 share C, each writer its own range. */
static uint64_t first_word(uint64_t kind, uint64_t round, uint64_t index,
                           uint64_t k) {
  uint64_t half = (kind == KIND_X0 || kind == KIND_X1) ? 0 : 1;
  return ((round * k + index) * 2 + half) * SPAN;
}

static void write_range(uint64_t *words, const uint64_t *p) {
  uint64_t at = first_word(p[P_KIND], p[P_ROUND], p[P_INDEX], p[P_K]);
  for (uint64_t i = 0; i < SPAN; ++i)
    words[at + i] =
        (p[P_KIND] << 56) | (p[P_ROUND] << 24) | (p[P_INDEX] << 8) | i;
}

static uint64_t read_range(const uint64_t *words) {
  uint64_t s = 0;
  for (uint64_t i = 0; i < READ_WORDS; ++i)
    s += words[READ_BASE + i];
  return s;
}

/* A slot's payload; NULL, reported, when the slot resolved to none. */
static uint64_t *storage(arts_edt_dep_t *dep, const uint64_t *p,
                         const char *slot) {
  if (dep->ptr == NULL) {
    arts_printf("FAIL: db_two_block_readers kind %" PRIu64 " round %" PRIu64
                " index %" PRIu64 " got no storage for its %s block\n",
                p[P_KIND], p[P_ROUND], p[P_INDEX], slot);
    arts_test_fail();
  }
  return (uint64_t *)dep->ptr;
}

static arts_edt_hint_t on_rank(unsigned int rank) {
  arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
  h.rank = rank;
  return h;
}

/* X0 and X1: slot 0 the block written, slot 1 the block read. */
static void x_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t *mine = storage(&depv[0], paramv, "written");
  const uint64_t *other = storage(&depv[1], paramv, "read");
  if (mine != NULL)
    write_range(mine, paramv);
  if (other == NULL)
    return;
  uint64_t s = read_range(other);
  if (s != READ_WORDS * READ_FILL) {
    arts_printf("FAIL: db_two_block_readers kind %" PRIu64 " round %" PRIu64
                " index %" PRIu64 " read sum %" PRIu64 ", expected %u\n",
                paramv[P_KIND], paramv[P_ROUND], paramv[P_INDEX], s,
                READ_WORDS * READ_FILL);
    arts_test_fail();
  }
}

/* Z0 and Z1: slot 0 the block written. */
static void z_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t *mine = storage(&depv[0], paramv, "written");
  if (mine != NULL)
    write_range(mine, paramv);
}

static void make(uint64_t *p, uint64_t kind, uint64_t index) {
  p[P_KIND] = kind;
  p[P_INDEX] = index;
  unsigned int rank =
      ((kind == KIND_X0 || kind == KIND_Z0) ? 0u : 1u) % arts_get_total_ranks();
  arts_guid_t b = (arts_guid_t)p[P_B], c = (arts_guid_t)p[P_C];
  arts_edt_hint_t h = on_rank(rank);
  h.completion_event = (arts_guid_t)p[P_LATCH];
  if (kind == KIND_X0 || kind == KIND_X1) {
    arts_guid_t e = arts_edt_create(x_edt, P_COUNT, p, 2, &h);
    arts_guid_t mine = kind == KIND_X0 ? b : c;
    arts_guid_t other = kind == KIND_X0 ? c : b;
    arts_add_dependence(mine, e, 0, DB_MODE_RW);
    arts_add_dependence(other, e, 1, DB_MODE_RO);
  } else {
    arts_guid_t e = arts_edt_create(z_edt, P_COUNT, p, 1, &h);
    arts_add_dependence(kind == KIND_Z0 ? c : b, e, 0, DB_MODE_RW);
  }
}

/* One round's EDTs, then the next round's driver. */
static void round_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  uint64_t p[P_COUNT];
  memcpy(p, paramv, sizeof p);
  for (uint64_t i = 0; i < p[P_K]; ++i) {
    make(p, KIND_X0, i);
    make(p, KIND_X1, i);
    make(p, KIND_Z0, i);
    make(p, KIND_Z1, i);
  }
  p[P_ROUND] += 1;
  if (p[P_ROUND] < p[P_ROUNDS]) {
    arts_edt_hint_t h = on_rank(0u);
    arts_edt_create(round_edt, P_COUNT, p, 0, &h);
  }
}

static void end_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const uint64_t *b = depv[1].ptr, *c = depv[2].ptr;
  if (b == NULL || c == NULL) {
    arts_printf("FAIL: db_two_block_readers the end got no storage for "
                "%s\n",
                b == NULL ? (c == NULL ? "B and C" : "B") : "C");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  uint64_t bad = 0;
  for (uint64_t r = 0; r < paramv[P_ROUNDS]; ++r)
    for (uint64_t i = 0; i < paramv[P_K]; ++i)
      for (uint64_t kind = 0; kind < KINDS; ++kind) {
        const uint64_t *w = (kind == KIND_X0 || kind == KIND_Z1) ? b : c;
        uint64_t at = first_word(kind, r, i, paramv[P_K]);
        for (uint64_t j = 0; j < SPAN; ++j)
          bad += w[at + j] != ((kind << 56) | (r << 24) | (i << 8) | j);
      }
  if (bad != 0) {
    arts_printf("FAIL: db_two_block_readers %" PRIu64 " words wrong\n", bad);
    arts_test_fail();
  }
  if (arts_test_status() == 0)
    arts_printf("PASS: db_two_block_readers\n");
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  uint64_t p[P_COUNT] = {0};
  p[P_ROUNDS] = argc > 1 ? strtoull(argv[1], NULL, 10) : 20;
  p[P_K] = argc > 2 ? strtoull(argv[2], NULL, 10) : 4;
  if (arts_get_total_ranks() < 2u) {
    arts_printf("FAIL: db_two_block_readers needs two ranks\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  /* Each factor is bounded first, so the fit's arithmetic cannot wrap; the
   * fit then bounds rounds x k, and with it the latch's int32_t count. */
  if (p[P_ROUNDS] == 0 || p[P_K] == 0 || p[P_ROUNDS] > READ_BASE ||
      p[P_K] > READ_BASE ||
      first_word(KIND_Z1, p[P_ROUNDS] - 1, p[P_K] - 1, p[P_K]) + SPAN >
          READ_BASE) {
    arts_printf("FAIL: db_two_block_readers rounds %" PRIu64 " x k %" PRIu64
                " do not fit the blocks\n",
                p[P_ROUNDS], p[P_K]);
    arts_test_fail();
    arts_shutdown();
    return;
  }
  uint64_t *b, *c;
  arts_guid_t bg = arts_db_create((void **)&b, WORDS * sizeof(uint64_t),
                                  ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  arts_guid_t cg = arts_db_create((void **)&c, WORDS * sizeof(uint64_t),
                                  ARTS_DB, ARTS_DB_PROP_NONE, NULL);
  for (uint32_t i = 0; i < WORDS; ++i)
    b[i] = c[i] = i >= READ_BASE ? READ_FILL : 0u;
  arts_db_release(bg);
  arts_db_release(cg);
  arts_event_hint_t lh =
      ARTS_EVENT_HINT_LATCH((int32_t)(KINDS * p[P_K] * p[P_ROUNDS]));
  arts_guid_t latch = arts_event_create(&lh);
  p[P_LATCH] = latch;
  p[P_B] = bg;
  p[P_C] = cg;
  arts_printf("db_two_block_readers: rounds %" PRIu64 " k %" PRIu64
              " B %" PRIu64 " C %" PRIu64 "\n",
              p[P_ROUNDS], p[P_K], (uint64_t)bg, (uint64_t)cg);
  arts_edt_hint_t h = on_rank(0u);
  arts_guid_t end = arts_edt_create(end_edt, P_COUNT, p, 3, &h);
  arts_add_dependence(latch, end, 0, DB_MODE_NULL);
  arts_add_dependence(bg, end, 1, DB_MODE_RO);
  arts_add_dependence(cg, end, 2, DB_MODE_RO);
  arts_edt_create(round_edt, P_COUNT, p, 0, &h);
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
