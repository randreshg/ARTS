/* SPDX-License-Identifier: Apache-2.0
 *
 * A resolution through a waiting entry that outlived its EDT, or that arrives
 * after its EDT's acquire phase ended, stops the run (ARTS_DEBUG_WAKE_GUID).
 *
 * No runtime starts.  Each case builds an EDT by hand as the EDT create lays
 * it out (one read slot, the acquire record allocated as the acquire phase
 * allocates it, no descriptor pin) and runs in a forked child whose output
 * the parent reads to its end before judging how the child ended:
 *
 *   mismatch   a wake through an entry naming the EDT's address with another
 *              EDT's GUID ends with status 1 and the waiter message, before
 *              the resolver reads the pin;
 *   ended      a wake with the EDT's own GUID after its acquire phase ended
 *              ends with status 1 and the phase message;
 *   real site  a parked-reader node naming the EDT's address with another
 *              GUID, served through the runtime's own chain serve, ends with
 *              status 1 (an exit, not a signal) and the waiter message alone;
 *   control    a wake with the EDT's own GUID during its acquire phase passes
 *              both checks silently, and a second resolution of the slot ends
 *              with status 1 and the duplicate message.
 *
 * No case touches freed memory, so the verdicts do not depend on the
 * allocator or the sanitizer.
 */
#include "arts/coherence/coherence.h"
#include "arts/coherence/types.h"
#include "arts/db.h"
#include "arts/edt.h"
#include "arts/gas/guid.h"
#include "arts/runtime_types.h"
#include "arts/system/identity.h"
#include "arts/utils/malloc.h"
#include "arts/utils/stdatomic_compat.h"

#include <signal.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef ARTS_DEBUG_WAKE_GUID
#error "dep_slot_resolved_late is built only where ARTS_DEBUG_WAKE_GUID is on"
#endif

#define MSG_WAITER "no longer holds that EDT"
#define MSG_PHASE "resolved after its acquire phase ended"
#define MSG_TWICE "resolved twice"

/* Another EDT's GUID, which no hand-built EDT carries. */
#define OTHER_GUID ARTS_GUID_MAKE(ARTS_GUID_EDT, 0u, 0x2B5u)

static struct arts_edt_s *hand_edt(unsigned int acquire_remaining) {
  size_t space = sizeof(struct arts_edt_s) + sizeof(arts_edt_dep_t);
  struct arts_edt_s *edt = (struct arts_edt_s *)arts_calloc_aligned(
      1, space, ARTS_CACHE_LINE_SIZE);
  edt->guid = ARTS_GUID_MAKE(ARTS_GUID_EDT, 0u, 0x51u);
  edt->depc = 1u;
  edt->paramc = 0u;
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  depv[0].guid = ARTS_GUID_MAKE(ARTS_GUID_DB, 1u, ARTS_GUID_DB_KEY(1u, 42u));
  depv[0].ptr = NULL;
  depv[0].mode = DB_MODE_RO;
  arts_db_acquire_prepare(edt);
  atomic_store_explicit(&edt->acquire_remaining, acquire_remaining,
                        memory_order_relaxed);
  return edt;
}

static void mismatch(void) {
  struct arts_edt_s *edt = hand_edt(2u);
  ARTS_DEP_WAITER_CHECK(edt, OTHER_GUID, 0u);
  arts_db_resolve_slot_cached(edt, 0u);
}

static void ended(void) {
  struct arts_edt_s *edt = hand_edt(0u);
  ARTS_DEP_WAITER_CHECK(edt, edt->guid, 0u);
  arts_db_resolve_slot_cached(edt, 0u);
}

/* A first-touch cache of a block homed on the other rank, its reader pool
 * holding one parked node that names the EDT's address with another GUID. */
static void real_site(void) {
  arts_global_rank_id = 0u;
  arts_global_rank_count = 2u;
  struct arts_edt_s *edt = hand_edt(2u);
  arts_guid_t g = ARTS_GUID_MAKE(ARTS_GUID_DB, 1u, ARTS_GUID_DB_KEY(1u, 42u));
  size_t sz = (sizeof(struct arts_db_s) + alignof(struct arts_db_s) - 1u) /
              alignof(struct arts_db_s) * alignof(struct arts_db_s);
  struct arts_db_s *db =
      (struct arts_db_s *)aligned_alloc(alignof(struct arts_db_s), sz);
  memset(db, 0, sz);
  db->db_type = ARTS_DB;
  arts_db_cache_init(&db->cache, g, 64u, ARTS_DB_INIT_STUB, 1u);
  uint32_t idx = arts_db_reader_alloc(&db->cache.reader_pool);
  struct arts_db_reader_node_s *n =
      arts_db_reader_node(&db->cache.reader_pool, idx);
  n->edt = edt;
  n->edt_guid = OTHER_GUID;
  n->slot = 0u;
  ARTS_DB_CENSUS_SET_HELD(n, false);
  atomic_store_explicit(&n->next, 0u, memory_order_relaxed);
  arts_db_reader_serve_chain(&db->cache.reader_pool, idx);
}

static void control_once(void) {
  struct arts_edt_s *edt = hand_edt(2u);
  ARTS_DEP_WAITER_CHECK(edt, edt->guid, 0u);
  arts_dep_acq_resolve_once(edt, 0u);
}

static void control_twice(void) {
  struct arts_edt_s *edt = hand_edt(2u);
  ARTS_DEP_WAITER_CHECK(edt, edt->guid, 0u);
  arts_dep_acq_resolve_once(edt, 0u);
  arts_dep_acq_resolve_once(edt, 0u);
}

/* Runs body in a child whose stdout and stderr go to a pipe; returns the
 * child's exit status, or -1 when it did not exit on its own (a signal) or
 * could not be run.  out receives what the child printed. */
static int run_case(void (*body)(void), char *out, size_t cap) {
  int fd[2];
  out[0] = '\0';
  if (pipe(fd) != 0) {
    return -1;
  }
  fflush(stdout);
  fflush(stderr);
  pid_t pid = fork();
  if (pid == 0) {
    close(fd[0]);
    dup2(fd[1], STDOUT_FILENO);
    dup2(fd[1], STDERR_FILENO);
    close(fd[1]);
    body();
    fflush(stdout);
    _exit(0);
  }
  close(fd[1]);
  if (pid < 0) {
    close(fd[0]);
    return -1;
  }
  size_t len = 0;
  for (;;) {
    char buf[512];
    ssize_t r = read(fd[0], buf, sizeof(buf));
    if (r <= 0) {
      break;
    }
    size_t take = (size_t)r;
    if (take > cap - 1u - len) {
      take = cap - 1u - len;
    }
    memcpy(out + len, buf, take);
    len += take;
  }
  out[len] = '\0';
  close(fd[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
    return -1;
  }
  return WEXITSTATUS(status);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  /* A SIGCHLD ignored by whoever started the test would reap the children
   * before waitpid could see how they ended. */
  signal(SIGCHLD, SIG_DFL);
  struct {
    const char *name;
    void (*body)(void);
    int want;
    const char *must;
    const char *never[2];
  } cases[] = {
      {"mismatch", mismatch, 1, MSG_WAITER, {MSG_PHASE, MSG_TWICE}},
      {"ended", ended, 1, MSG_PHASE, {MSG_WAITER, MSG_TWICE}},
      {"real site", real_site, 1, MSG_WAITER, {MSG_PHASE, MSG_TWICE}},
      {"control", control_once, 0, NULL, {NULL, NULL}},
      {"control, resolved again", control_twice, 1, MSG_TWICE,
       {MSG_WAITER, MSG_PHASE}},
  };
  static char out[1 << 16];
  int fail = 0;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    int got = run_case(cases[i].body, out, sizeof(out));
    bool ok = got == cases[i].want;
    if (cases[i].must != NULL) {
      ok = ok && strstr(out, cases[i].must) != NULL;
    } else {
      ok = ok && out[0] == '\0';
    }
    for (size_t k = 0; k < 2u; k++) {
      if (cases[i].never[k] != NULL && strstr(out, cases[i].never[k]) != NULL) {
        ok = false;
      }
    }
    printf("%s %s: exit status %d (want %d)\n", ok ? "ok" : "FAIL:",
           cases[i].name, got, cases[i].want);
    if (!ok) {
      fail = 1;
    }
  }
  printf(fail ? "FAIL dep_slot_resolved_late\n"
              : "PASS dep_slot_resolved_late\n");
  return fail;
}
