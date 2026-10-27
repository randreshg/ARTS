/******************************************************************************
** This material was prepared as an account of work sponsored by an agency   **
** of the United States Government.  Neither the United States Government    **
** nor the United States Department of Energy, nor Battelle, nor any of      **
** their employees, nor any jurisdiction or organization that has cooperated **
** in the development of these materials, makes any warranty, express or     **
** implied, or assumes any legal liability or responsibility for the accuracy,*
** completeness, or usefulness or any information, apparatus, product,       **
** software, or process disclosed, or represents that its use would not      **
** infringe privately owned rights.                                          **
**                                                                           **
** Reference herein to any specific commercial product, process, or service  **
** by trade name, trademark, manufacturer, or otherwise does not necessarily **
** constitute or imply its endorsement, recommendation, or favoring by the   **
** United States Government or any agency thereof, or Battelle Memorial      **
** Institute. The views and opinions of authors expressed herein do not      **
** necessarily state or reflect those of the United States Government or     **
** any agency thereof.                                                       **
**                                                                           **
**                      PACIFIC NORTHWEST NATIONAL LABORATORY                **
**                                  operated by                              **
**                                    BATTELLE                               **
**                                     for the                               **
**                      UNITED STATES DEPARTMENT OF ENERGY                   **
**                         under Contract DE-AC05-76RL01830                  **
**                                                                           **
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License");           **
** you may not use this file except in compliance with the License.          **
** You may obtain a copy of the License at                                   **
**                                                                           **
**    https://www.apache.org/licenses/LICENSE-2.0                            **
**                                                                           **
** Unless required by applicable law or agreed to in writing, software       **
** distributed under the License is distributed on an "AS IS" BASIS, WITHOUT **
** WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the  **
** License for the specific language governing permissions and limitations   **
******************************************************************************/

/// @file argv_params.c
/// @brief An argument vector packed into EDT parameters reads back the same on
///        another rank, and the parser refuses every malformed block without
///        reading a word past it.
///
/// main_edt packs fixed argument lists, checks the word count of each against
/// the format's (one argc word, then per argument a length word and its bytes,
/// NUL-terminated and padded to a word), and sends each block as an EDT's
/// parameters to the last rank, which unpacks it and compares it with the same
/// list.  The lists cover argc 0 and 1, an empty argument, and lengths that
/// fill their last word (len % 8 == 7) and that spill a word holding only the
/// NUL (len % 8 == 0).
///
/// main_edt also hands the parser malformed blocks, each in a heap copy of
/// exactly its own size, so a read past the block is a heap overflow that
/// AddressSanitizer reports.  Every one is refused by both readers, and
/// arts_argv_from_params leaves its output untouched; a capacity equal to argc
/// is refused by arts_argv_from_params alone, since arts_argv_count takes none.

#include "arts.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../test_failure_status.h"

#define MAX_ARGS 6

typedef struct {
  int argc;
  const char *argv[MAX_ARGS];
  /* The block's words, counted by hand from the format. */
  uint32_t paramc;
} arg_list_t;

static const arg_list_t lists[] = {
    {.argc = 0, .paramc = 1},
    {.argc = 1, .argv = {"prog"}, .paramc = 3},
    {.argc = 1, .argv = {""}, .paramc = 3},
    {.argc = 1, .argv = {"1234567"}, .paramc = 3},
    {.argc = 1, .argv = {"12345678"}, .paramc = 4},
    {.argc = 1, .argv = {"abcdefghijklmnop"}, .paramc = 5},
    {.argc = 2, .argv = {"", ""}, .paramc = 5},
    {.argc = 2, .argv = {"0123456789abcdef0123456", "x"}, .paramc = 7},
    {.argc = 3, .argv = {"prog", "--file", "/path/to/input.mtx"}, .paramc = 9},
    {.argc = 4, .argv = {"a", "", "abcdefg", "abcdefgh"}, .paramc = 10},
    {.argc = 5,
     .argv = {"prog", "--num-vertices", "1024", "--num-edges", "8192"},
     .paramc = 13},
};

#define NLISTS (sizeof(lists) / sizeof(lists[0]))

/* What arts_argv_from_params must leave in every slot it refuses to fill. */
static const char untouched[] = "untouched";

static void fail(const char *what, unsigned int which) {
  arts_printf("FAIL argv_params: %s (%u)\n", what, which);
  arts_test_fail();
}

static uint64_t *pack(const arg_list_t *l) {
  uint32_t paramc = arts_argv_paramc(l->argc, l->argv);
  if (paramc != l->paramc) {
    return NULL;
  }
  uint64_t *block = (uint64_t *)malloc(paramc * sizeof(uint64_t));
  if (block != NULL) {
    arts_argv_to_params(l->argc, l->argv, block);
  }
  return block;
}

/* Checks that the block of l, at paramv[0, paramc), reads back as l. */
static void check_unpacks(const arg_list_t *l, unsigned int which,
                          const uint64_t *paramv, uint32_t paramc) {
  if (arts_argv_count(paramv, paramc) != l->argc) {
    fail("arts_argv_count of a packed list", which);
    return;
  }
  const char *argv[MAX_ARGS + 1];
  if (arts_argv_from_params(paramv, paramc, argv, l->argc + 1) != l->argc) {
    fail("arts_argv_from_params of a packed list", which);
    return;
  }
  const char *lo = (const char *)paramv;
  const char *hi = (const char *)(paramv + paramc);
  for (int i = 0; i < l->argc; i++) {
    if (argv[i] < lo || argv[i] >= hi || strcmp(argv[i], l->argv[i]) != 0) {
      fail("an unpacked argument differs from the packed one", which);
      return;
    }
  }
  if (argv[l->argc] != NULL) {
    fail("the unpacked vector is not NULL-terminated", which);
  }
}

/* On the last rank.  The parameters are the block of list depv[0].guid. */
static void unpack_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)depc;
  unsigned int which = (unsigned int)depv[0].guid;
  if (arts_get_current_rank() != arts_get_total_ranks() - 1) {
    fail("the unpacking EDT runs on another rank", which);
  }
  if (which >= NLISTS || paramc != lists[which].paramc) {
    fail("the EDT's parameters are not the packed block", which);
    return;
  }
  check_unpacks(&lists[which], which, paramv, paramc);
}

/* `block` is handed to both readers as given.  Both must refuse it, except
 * that arts_argv_count returns `count` (-1 unless only the capacity is short),
 * and arts_argv_from_params must leave its output untouched. */
static void expect_refused(const char *what, const uint64_t *block,
                           uint32_t paramc, int max, int count) {
  if (arts_argv_count(block, paramc) != count) {
    arts_printf("FAIL argv_params: arts_argv_count accepted %s\n", what);
    arts_test_fail();
  }
  const char *argv[MAX_ARGS + 1];
  for (int i = 0; i <= MAX_ARGS; i++) {
    argv[i] = untouched;
  }
  if (arts_argv_from_params(block, paramc, argv, max) != -1) {
    arts_printf("FAIL argv_params: arts_argv_from_params accepted %s\n", what);
    arts_test_fail();
  }
  for (int i = 0; i <= MAX_ARGS; i++) {
    if (argv[i] != untouched) {
      arts_printf("FAIL argv_params: arts_argv_from_params refused %s but "
                  "wrote its output\n",
                  what);
      arts_test_fail();
      break;
    }
  }
}

/* Hands the readers an exactly sized heap copy of words[0, paramc). */
static void expect_copy_refused(const char *what, const uint64_t *words,
                                uint32_t paramc, int max, int count) {
  uint64_t *copy = (uint64_t *)malloc(paramc * sizeof(uint64_t));
  if (copy == NULL) {
    fail("cannot allocate a block", paramc);
    return;
  }
  memcpy(copy, words, paramc * sizeof(uint64_t));
  expect_refused(what, copy, paramc, max, count);
  free(copy);
}

static void check_refusals(void) {
  /* The empty block.  A copy of no words has no storage: NULL, whose first
   * word faults in every build, and a heap byte, too short for a word. */
  expect_refused("the empty block at NULL", NULL, 0, MAX_ARGS + 1, -1);
  void *byte = malloc(1);
  if (byte == NULL) {
    fail("cannot allocate a block", 0);
  } else {
    expect_refused("the empty block at a heap byte", byte, 0, MAX_ARGS + 1, -1);
    free(byte);
  }

  /* Every packed list truncated by one word, and read with a capacity equal
   * to its argc. */
  for (unsigned int k = 0; k < NLISTS; k++) {
    uint64_t *block = pack(&lists[k]);
    if (block == NULL) {
      fail("cannot pack a list", k);
      continue;
    }
    if (lists[k].paramc > 1) {
      expect_copy_refused("a block truncated by one word", block,
                          lists[k].paramc - 1, MAX_ARGS + 1, -1);
    }
    expect_copy_refused("a capacity equal to argc", block, lists[k].paramc,
                        lists[k].argc, lists[k].argc);
    free(block);
  }

  /* A length word of UINT64_MAX. */
  uint64_t huge_len[] = {1, UINT64_MAX, 0};
  expect_copy_refused("a length word of UINT64_MAX", huge_len, 3, MAX_ARGS + 1,
                      -1);

  /* A length whose bytes and NUL need three words where two are left. */
  uint64_t long_len[] = {1, 16, 0, 0};
  expect_copy_refused("a length that runs past the block", long_len, 4,
                      MAX_ARGS + 1, -1);

  /* "abcdef" with its length word rewritten to 3: byte 3 is 'd'. */
  static const char *abcdef[] = {"abcdef"};
  uint64_t nul[3];
  arts_argv_to_params(1, abcdef, nul);
  nul[1] = 3;
  expect_copy_refused("a NUL not at the length", nul, 3, MAX_ARGS + 1, -1);

  /* argc 2 over one 8-byte argument: [2][8][bytes][NUL word]. */
  static const char *eight[] = {"12345678"};
  uint64_t short_argc[4];
  arts_argv_to_params(1, eight, short_argc);
  short_argc[0] = 2;
  expect_copy_refused("an argc word larger than the arguments present",
                      short_argc, 4, MAX_ARGS + 1, -1);

  /* argc 2 within the two-words-per-argument bound, but the first argument
   * takes every word left. */
  static const char *sixteen[] = {"abcdefghijklmnop"};
  uint64_t run_out[5];
  arts_argv_to_params(1, sixteen, run_out);
  run_out[0] = 2;
  expect_copy_refused("arguments that run out before argc", run_out, 5,
                      MAX_ARGS + 1, -1);

  /* argc words above INT_MAX. */
  uint64_t big_argc[] = {(uint64_t)INT_MAX + 1};
  expect_copy_refused("an argc word above INT_MAX", big_argc, 1, MAX_ARGS + 1,
                      -1);
  uint64_t max_argc[] = {UINT64_MAX};
  expect_copy_refused("an argc word of UINT64_MAX", max_argc, 1, MAX_ARGS + 1,
                      -1);
}

/* The layout itself, for one list: the argc word, the length word, then the
 * bytes, NUL-terminated and zero-padded. */
static void check_layout(void) {
  static const char *eight[] = {"12345678"};
  uint64_t block[4];
  memset(block, 0xff, sizeof(block));
  arts_argv_to_params(1, eight, block);
  if (block[0] != 1 || block[1] != 8 || memcmp(&block[2], "12345678", 8) != 0 ||
      block[3] != 0) {
    fail("the packed layout of one 8-byte argument", 0);
  }
}

static void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_test_status() == 0) {
    arts_printf("PASS argv_params: %u lists unpacked on rank %u\n",
                (unsigned int)NLISTS, arts_get_total_ranks() - 1);
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  check_layout();
  check_refusals();

  arts_guid_t unpacked = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t done =
      arts_edt_create(done_edt, 0, NULL, 1, &ARTS_EDT_HINT_DEFAULTS);
  arts_add_dependence(unpacked, done, 0, DB_MODE_NULL);

  arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
  h.rank = arts_get_total_ranks() - 1;
  h.finish_event = unpacked;
  for (unsigned int k = 0; k < NLISTS; k++) {
    uint64_t *block = pack(&lists[k]);
    if (block == NULL) {
      fail("arts_argv_paramc differs from the format's word count", k);
      continue;
    }
    check_unpacks(&lists[k], k, block, lists[k].paramc);
    arts_guid_t edt =
        arts_edt_create(unpack_edt, lists[k].paramc, block, 1, &h);
    arts_edt_satisfy_slot(edt, 0, (arts_guid_t)k, DB_MODE_NULL);
    free(block);
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
