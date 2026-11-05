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

/// @file launcher_orphan_prevention.c
/// @brief The spawner starts exactly the configured ranks, each once, as its
///        own children, with each rank's identity handed down.
///
/// Every rank, rank 0 included, is a child of the spawner, which is no rank:
/// it hands each its rank through ARTS_RANK, and a rank whose spawner ended
/// before the rank armed its parent-death signal leaves at startup, so no rank
/// outlives the spawner.  Asserted per rank, by a probe placed on it:
///   1. it runs on the rank it was placed on, and sees the configured total;
///   2. ARTS_RANK is set and names that rank;
///   3. its parent is a live process other than init that runs this program
///      (the spawner, not a subreaper that adopted an orphan), and the same
///      process on every rank (each probe records its parent, the verifier
///      compares);
///   4. it stamps its own slot of a marker block, which no probe stamped
///      before, and the verifier then reads every slot 0..total-1 holding its
///      own rank's marker, so the live ranks are exactly 0..total-1 with no
///      gap and no duplicate.
/// A rank that never came up leaves its probe unrun (the run times out).
/// Protocol-agnostic: the spawner carries no coherence state.

#include "arts.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../test_failure_status.h"

#define MARKER_BASE 0x5A5A0000u

/* The block: total marks, then total parent pids. */
static uint32_t *marks_of(void *block) { return (uint32_t *)block; }
static int32_t *parents_of(void *block, unsigned int total) {
  return (int32_t *)((uint32_t *)block + total);
}

/* Whether process `pid` runs the executable this process runs. */
static int runs_this_program(pid_t pid) {
  char exe[64];
  (void)snprintf(exe, sizeof(exe), "/proc/%d/exe", (int)pid);
  struct stat mine;
  struct stat theirs;
  if (stat("/proc/self/exe", &mine) != 0 || stat(exe, &theirs) != 0) {
    return 0;
  }
  return mine.st_dev == theirs.st_dev && mine.st_ino == theirs.st_ino;
}

void rank_probe_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int expected_rank = (unsigned int)paramv[0];
  unsigned int total = (unsigned int)paramv[1];
  void *block = depv[0].ptr;

  unsigned int me = arts_get_current_rank();
  unsigned int seen_total = arts_get_total_ranks();

  if (me != expected_rank) {
    arts_printf("FAIL: launcher_orphan_prevention probe ran on %u, expected "
                "%u\n",
                me, expected_rank);
    arts_test_fail();
    return;
  }
  if (seen_total != total) {
    arts_printf("FAIL: launcher_orphan_prevention rank %u sees %u ranks, "
                "expected %u\n",
                me, seen_total, total);
    arts_test_fail();
    return;
  }
  const char *rank_env = getenv("ARTS_RANK");
  if (rank_env == NULL || (unsigned int)strtoul(rank_env, NULL, 10) != me) {
    arts_printf("FAIL: launcher_orphan_prevention rank %u has ARTS_RANK=%s\n",
                me, rank_env ? rank_env : "(unset)");
    arts_test_fail();
    return;
  }
  pid_t ppid = getppid();
  if (ppid <= 1 || (kill(ppid, 0) != 0 && errno != EPERM)) {
    arts_printf("FAIL: launcher_orphan_prevention rank %u has parent %d, "
                "which is init or gone\n",
                me, (int)ppid);
    arts_test_fail();
    return;
  }
  if (!runs_this_program(ppid)) {
    arts_printf("FAIL: launcher_orphan_prevention rank %u has parent %d, "
                "which does not run this program\n",
                me, (int)ppid);
    arts_test_fail();
    return;
  }
  if (block == NULL) {
    arts_printf("FAIL: launcher_orphan_prevention rank %u got a NULL marker "
                "block\n",
                me);
    arts_test_fail();
    return;
  }
  if (marks_of(block)[me] != 0u) {
    arts_printf("FAIL: launcher_orphan_prevention slot %u was stamped before "
                "(0x%x)\n",
                me, marks_of(block)[me]);
    arts_test_fail();
    return;
  }
  marks_of(block)[me] = MARKER_BASE | me;
  parents_of(block, total)[me] = (int32_t)ppid;
}

void verify_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int total = (unsigned int)paramv[0];
  void *block = depv[0].ptr;
  if (block == NULL) {
    arts_printf("FAIL: launcher_orphan_prevention verifier got a NULL marker "
                "block\n");
    arts_test_fail();
    return;
  }
  const uint32_t *marks = marks_of(block);
  const int32_t *parents = parents_of(block, total);
  for (unsigned int r = 0; r < total; r++) {
    if (marks[r] != (MARKER_BASE | r)) {
      arts_printf("FAIL: launcher_orphan_prevention slot %u = 0x%x, expected "
                  "0x%x\n",
                  r, marks[r], MARKER_BASE | r);
      arts_test_fail();
    }
    if (parents[r] != parents[0]) {
      arts_printf("FAIL: launcher_orphan_prevention rank %u has parent %d, "
                  "rank 0 has %d\n",
                  r, (int)parents[r], (int)parents[0]);
      arts_test_fail();
    }
  }
}

static void finale_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("PASS: launcher_orphan_prevention (%u ranks, each once, one "
              "live parent)\n",
              arts_get_total_ranks());
  arts_shutdown();
}

static void probe_step_edt(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]);

/* paramv: the next probe's rank, the marker block.  One probe at a time,
 * each in a group of its own and the next gated on it, so each write
 * turn is ordered before the next; then the verifier, and the end of the run
 * gated on it. */
static void probe_step(const uint64_t *paramv) {
  unsigned int total = arts_get_total_ranks();
  unsigned int r = (unsigned int)paramv[0];
  arts_guid_t db = (arts_guid_t)paramv[1];
  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&gh);
  arts_guid_t n;
  if (r < total) {
    uint64_t params[2] = {(uint64_t)r, (uint64_t)total};
    arts_guid_t p =
        arts_edt_create(rank_probe_edt, 2, params, 1,
                        &(arts_edt_hint_t){.rank = r, .group = group});
    arts_add_dependence(db, p, 0, DB_MODE_RW);
    uint64_t next[2] = {(uint64_t)r + 1u, (uint64_t)db};
    n = arts_edt_create(probe_step_edt, 2, next, 1,
                        &(arts_edt_hint_t){.rank = 0});
  } else {
    uint64_t vparams[1] = {(uint64_t)total};
    arts_guid_t v =
        arts_edt_create(verify_edt, 1, vparams, 1,
                        &(arts_edt_hint_t){.rank = 0, .group = group});
    arts_add_dependence(db, v, 0, DB_MODE_RO);
    n = arts_edt_create(finale_edt, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  }
  arts_add_dependence(group, n, 0, DB_MODE_NULL);
  arts_event_satisfy_slot(group, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

static void probe_step_edt(uint32_t paramc, const uint64_t *paramv,
                           uint32_t depc, arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  probe_step(paramv);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  unsigned int total = arts_get_total_ranks();

  void *ptr = NULL;
  arts_guid_t db =
      arts_db_create(&ptr, total * (sizeof(uint32_t) + sizeof(int32_t)),
                     ARTS_DB, ARTS_DB_PROP_NONE, &(arts_db_hint_t){.rank = 0});
  for (unsigned int r = 0; r < total; r++) {
    marks_of(ptr)[r] = 0u;
    parents_of(ptr, total)[r] = 0;
  }
  arts_db_release(db);

  probe_step((const uint64_t[2]){0u, (uint64_t)db});
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
