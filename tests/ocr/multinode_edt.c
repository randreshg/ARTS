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

/// @file multinode_edt.c
/// @brief Tests EDT creation and signaling across nodes: remote execution,
///        a fan-in of several tasks from every node under one finish scope,
///        multi-hop chains, and paramv delivery.  The finish scope fires once,
///        after every member on every node, and shuts the run down: each
///        check records its verdict in a block that the EDT the scope fires
///        reads, so a scope that fires before a member finished can leave
///        that check unrecorded, which fails the run when it does.
///        Requires multi-node (node_count > 1).

#include "arts.h"

#include <stdint.h>

#include "../test_failure_status.h"

/* One word per check in the verdict block: unreported until the check runs. */
enum { V_REMOTE, V_ALL_NODES, V_CHAIN, V_PARAMV, V_COUNT };
enum { V_UNREPORTED = 0, V_PASS = 1, V_FAIL = 2 };

static const char *const check_names[V_COUNT] = {
    "remote EDT", "fan-in from every node", "multi-hop chain", "remote paramv"};

static void record(arts_edt_dep_t *verdicts, int check, bool ok) {
  ((uint64_t *)verdicts->ptr)[check] = ok ? V_PASS : V_FAIL;
  if (!ok) {
    arts_test_fail();
  }
}

// ---------------------------------------------------------------------------
// Test 1: Remote EDT signals back to master.
// ---------------------------------------------------------------------------

void remote_task(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  (void)paramc;
  arts_guid_t collector = (arts_guid_t)paramv[0];
  unsigned int my_rank = arts_get_current_rank();
  arts_edt_satisfy_slot(collector, 0, (arts_guid_t)((uint64_t)my_rank), DB_MODE_NULL);
}

/// depv[0] = the remote task's rank, depv[1] = the verdict block.
void check_remote_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t remote_rank = (uint64_t)depv[0].guid;
  unsigned int expected = (unsigned int)paramv[0];
  bool ok = ((unsigned int)remote_rank == expected);
  if (ok) {
    arts_printf("  PASS: remote EDT ran on rank %u\n",
                (unsigned int)remote_rank);
  } else {
    arts_printf("  FAIL: expected rank %u got %lu\n", expected,
                (unsigned long)remote_rank);
  }
  record(&depv[1], V_REMOTE, ok);
}

// ---------------------------------------------------------------------------
// Test 2: TASKS_PER_NODE EDTs on every node, fan-in to one collector.
// ---------------------------------------------------------------------------

#define TASKS_PER_NODE 10

void all_nodes_task(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  (void)paramc;
  arts_guid_t collector = (arts_guid_t)paramv[0];
  uint32_t slot = (uint32_t)paramv[1];
  unsigned int my_rank = arts_get_current_rank();
  arts_edt_satisfy_slot(collector, slot, (arts_guid_t)((uint64_t)my_rank), DB_MODE_NULL);
}

/// Slot i < depc - 1 was given to a task placed on rank i / TASKS_PER_NODE;
/// the last slot is the verdict block.
void check_all_nodes(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  uint32_t tasks = depc - 1;
  bool ok = true;
  for (uint32_t i = 0; i < tasks; i++) {
    uint64_t rank = (uint64_t)depv[i].guid;
    if (rank != (uint64_t)(i / TASKS_PER_NODE)) {
      ok = false;
    }
  }
  if (ok) {
    arts_printf("  PASS: %u EDTs ran, %u on each of %u nodes\n", tasks,
                (unsigned int)TASKS_PER_NODE, tasks / TASKS_PER_NODE);
  } else {
    arts_printf("  FAIL: EDT distribution incorrect\n");
  }
  record(&depv[tasks], V_ALL_NODES, ok);
}

// ---------------------------------------------------------------------------
// Test 3: Multi-hop chain: A(node 0) -> B(node 1) -> C(node 0).
// ---------------------------------------------------------------------------

/// Intermediate hop: add 10 to value, signal next.
void chain_hop(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
               arts_edt_dep_t depv[]) {
  (void)depc;
  (void)paramc;
  arts_guid_t next = (arts_guid_t)paramv[0];
  uint64_t value = (uint64_t)depv[0].guid;
  arts_edt_satisfy_slot(next, 0, (arts_guid_t)(value + 10), DB_MODE_NULL);
}

/// Final hop: assert accumulated value.  depv[1] = the verdict block.
void chain_check(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t value = (uint64_t)depv[0].guid;
  uint64_t expected = (uint64_t)paramv[0];
  bool ok = (value == expected);
  if (ok) {
    arts_printf("  PASS: multi-hop chain value=%lu\n", (unsigned long)value);
  } else {
    arts_printf("  FAIL: multi-hop chain value=%lu expected=%lu\n",
                (unsigned long)value, (unsigned long)expected);
  }
  record(&depv[1], V_CHAIN, ok);
}

// ---------------------------------------------------------------------------
// Test 4: Paramv delivery to remote node.
// ---------------------------------------------------------------------------

/// depv[0] = the verdict block.
void check_paramv(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)depc;
  uint64_t expected[] = {0xDEAD, 0xBEEF, 0xCAFE, 0xF00D};
  bool ok = (paramc == 4);
  for (uint32_t i = 0; i < paramc && ok; i++) {
    if (paramv[i] != expected[i]) {
      ok = false;
    }
  }
  if (ok) {
    arts_printf("  PASS: paramv delivered correctly to remote node\n");
  } else {
    arts_printf("  FAIL: paramv mismatch on remote node\n");
  }
  record(&depv[0], V_PARAMV, ok);
}

// ---------------------------------------------------------------------------

/// depv[0] = the finish scope, depv[1] = the verdict block (RO).
void shutdown_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const uint64_t *verdicts = (const uint64_t *)depv[1].ptr;
  bool all = true;
  for (int i = 0; i < V_COUNT; i++) {
    if (verdicts[i] == V_UNREPORTED) {
      arts_printf("  FAIL: the finish scope fired before the %s check "
                  "reported\n",
                  check_names[i]);
      arts_test_fail();
      all = false;
    } else if (verdicts[i] != V_PASS) {
      all = false;
    }
  }
  if (all) {
    arts_printf("PASS: multinode_edt every check reported before the finish "
                "scope fired\n");
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== multinode_edt ===\n");

  unsigned int total = arts_get_total_ranks();
  uint64_t *v = NULL;
  arts_guid_t verdicts =
      arts_db_create((void **)&v, V_COUNT * sizeof(uint64_t), ARTS_DB,
                     ARTS_DB_PROP_NONE, &(arts_db_hint_t){.rank = 0});
  for (int i = 0; i < V_COUNT; i++) {
    v[i] = V_UNREPORTED;
  }
  arts_db_release(verdicts, DB_MODE_RW);

  arts_guid_t shut =
      arts_edt_create(shutdown_edt, 0, NULL, 2, &(arts_edt_hint_t){.rank = 0});
  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(fe, shut, 0, DB_MODE_NULL);
  arts_add_dependence(verdicts, shut, 1, DB_MODE_RO);

  // Test 1: Create EDT on remote node 1, have it signal back.
  {
    uint64_t expected_param = 1;
    arts_guid_t checker =
        arts_edt_create(check_remote_edt, 1, &expected_param, 2,
                        &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
    arts_add_dependence(verdicts, checker, 1, DB_MODE_RW);
    uint64_t coll_param = (uint64_t)checker;
    arts_edt_create(remote_task, 1, &coll_param, 0,
                    &(arts_edt_hint_t){.rank = 1, .finish_event = fe});
  }

  // Test 2: TASKS_PER_NODE EDTs on every node, each reports its rank.
  {
    uint32_t tasks = total * TASKS_PER_NODE;
    arts_guid_t all_coll =
        arts_edt_create(check_all_nodes, 0, NULL, tasks + 1,
                        &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
    arts_add_dependence(verdicts, all_coll, tasks, DB_MODE_RW);
    for (unsigned int i = 0; i < tasks; i++) {
      uint64_t params[2];
      params[0] = (uint64_t)all_coll;
      params[1] = (uint64_t)i;
      arts_edt_create(
          all_nodes_task, 2, params, 0,
          &(arts_edt_hint_t){.rank = i / TASKS_PER_NODE, .finish_event = fe});
    }
  }

  // Test 3: Multi-hop chain A(node 0) -> B(node 1) -> C(node 0).
  // A receives 100, adds 10, sends to B. B adds 10, sends to C.
  // C asserts value == 120.
  {
    uint64_t exp3 = 120;
    arts_guid_t c =
        arts_edt_create(chain_check, 1, &exp3, 2,
                        &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
    arts_add_dependence(verdicts, c, 1, DB_MODE_RW);
    uint64_t c_param = (uint64_t)c;
    arts_guid_t b =
        arts_edt_create(chain_hop, 1, &c_param, 1,
                        &(arts_edt_hint_t){.rank = 1, .finish_event = fe});
    uint64_t b_param = (uint64_t)b;
    arts_guid_t a =
        arts_edt_create(chain_hop, 1, &b_param, 1,
                        &(arts_edt_hint_t){.rank = 0, .finish_event = fe});
    arts_edt_satisfy_slot(a, 0, (arts_guid_t)(100), DB_MODE_NULL);
  }

  // Test 4: Paramv delivery to remote node.
  {
    uint64_t pv[4] = {0xDEAD, 0xBEEF, 0xCAFE, 0xF00D};
    arts_guid_t chk =
        arts_edt_create(check_paramv, 4, pv, 1,
                        &(arts_edt_hint_t){.rank = 1, .finish_event = fe});
    arts_add_dependence(verdicts, chk, 0, DB_MODE_RW);
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
