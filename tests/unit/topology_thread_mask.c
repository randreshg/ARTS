/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_get_thread_mask against a DECLARED hwloc topology
 * (libs/src/core/system/topology.c).  This TU #includes topology.c to drive
 * the (public, non-static) arts_get_thread_mask plus the file-static helpers it
 * uses; the runtime symbols that ARTS_ERROR / ARTS_INFO reference are stubbed
 * so the unit links standalone.  The machine is declared through hwloc's own
 * synthetic-topology input (HWLOC_SYNTHETIC, read at every topology load), so
 * every case runs and asserts the same on any host.  Two machines: one whose
 * description has no NUMA level (hwloc then gives it a single NUMA node
 * holding every PU) and one of two packages of two NUMA nodes each.  A
 * synthetic machine numbers its PUs, cores and NUMA nodes in logical order,
 * so the exact placement of every thread is known in advance.
 *
 * Invariants asserted on each machine
 * -----------------------------------
 *  - flat[t].pu_id and flat[t].numa_domain_id are exactly the placement rule's
 *    (first PU of every core before any core's second, NUMA-compact within a
 *    round, PU index ascending within a node), slice element t for a rank
 *    whose slice starts at its PU offset;
 *  - every flat[t].pu_id < total_pus (in range);
 *  - all flat[t].pu_id are DISTINCT (no two threads pinned to one PU);
 *  - role counts equal the config split (worker/progress);
 *  - group_pos is 0..count-1 CONTIGUOUS within each role group;
 *  - flat[t].id == t;
 *  - arts_numa_domain_count is the declared NUMA node count after the call;
 *  - local-multinode (shared_pu_pool=true) gives rank 0 and rank 1 DISJOINT
 *    pu_id slices (the structural oversubscription invariant);
 *  - pu_offset overflow (a slice exceeding total_pus) hits the bounds-check =>
 *    ARTS_ERROR => arts_abort (death test via fork, expect nonzero exit).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Standalone stubs for runtime symbols referenced by ARTS_ERROR/ARTS_INFO. */
unsigned int arts_global_rank_id = 0;
_Noreturn void arts_abort(uint8_t code);
_Noreturn void arts_abort(uint8_t code) {
  fprintf(stderr, "arts_abort(%u)\n", (unsigned)code);
  _exit(code ? code : 1);
}
void *arts_malloc(size_t size) { return malloc(size); }
void arts_free(void *ptr) { free(ptr); }

#include "../../libs/src/core/system/topology.c"

/* Storage for the thread-local arts_thread_info that print.h inline macros
 * reference (declared extern in runtime_state.h via the include above). */
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;

/* ------------------------------------------------------------------------- */

/* The declared machines: hwloc synthetic descriptions, what each holds, and
 * where the placement rule puts each thread of the cases below.  Core k
 * holds PUs 2k and 2k+1, and on the second machine NUMA node j holds PUs
 * 8j..8j+7, so the first PU of each core comes in ascending order: 0, 2, 4,
 * ..., then the second PUs: 1, 3, 5, ... */
#define SINGLE_TC 4 /* case 1: four threads, the last of them progress */
#define SLICE_TC 3  /* case 2: three threads per rank, the last progress */
static const struct {
  const char *desc;
  unsigned int pus;
  unsigned int numa;
  unsigned int single_pu[SINGLE_TC], single_numa[SINGLE_TC];
  unsigned int rank0_pu[SLICE_TC], rank0_numa[SLICE_TC];
  unsigned int rank1_pu[SLICE_TC], rank1_numa[SLICE_TC];
} machines[] = {
    {"core:4 pu:2",
     8,
     1,
     {0, 2, 4, 6},
     {0, 0, 0, 0},
     {0, 2, 4},
     {0, 0, 0},
     {6, 1, 3},
     {0, 0, 0}},
    {"pack:2 numa:2 core:4 pu:2",
     32,
     4,
     {0, 2, 4, 6},
     {0, 0, 0, 0},
     {0, 2, 4},
     {0, 0, 0},
     {6, 8, 10},
     {0, 1, 1}},
};

/* Declare `desc` as the machine every later topology load reads, and return
 * its PU count as hwloc reports it. */
static unsigned int declare_machine(const char *desc) {
  unsetenv("HWLOC_COMPONENTS");
  unsetenv("HWLOC_FSROOT");
  unsetenv("HWLOC_CPUID_PATH");
  if (setenv("HWLOC_SYNTHETIC", desc, 1) != 0) {
    fprintf(stderr, "FAIL topology_thread_mask: setenv\n");
    exit(1);
  }
  hwloc_topology_t t;
  if (hwloc_topology_init(&t) < 0 || hwloc_topology_load(t) < 0) {
    fprintf(stderr, "FAIL topology_thread_mask: hwloc init/load failed\n");
    exit(1);
  }
  int n = hwloc_get_nbobjs_by_type(t, HWLOC_OBJ_PU);
  hwloc_topology_destroy(t);
  if (n <= 0) {
    fprintf(stderr, "FAIL topology_thread_mask: declared machine has 0 PUs\n");
    exit(1);
  }
  return (unsigned int)n;
}

static void fail(const char *msg) {
  fprintf(stderr, "FAIL topology_thread_mask: %s\n", msg);
  exit(1);
}

/* A config whose thread count is its worker and progress counts together:
 * arts_get_thread_mask places that many threads, workers in the low slots
 * [0, worker_thread_count) and progress threads in the rest. */
static struct arts_config_s make_config(unsigned int thread_count,
                                        unsigned int progress, bool shared_pool,
                                        unsigned int my_rank) {
  struct arts_config_s c;
  memset(&c, 0, sizeof(c));
  c.thread_count = thread_count;
  c.progress_thread_count = progress;
  c.worker_thread_count = thread_count - progress;
  c.shared_pu_pool = shared_pool;
  c.my_rank = my_rank;
  c.pin_threads = false;
  return c;
}

static void check_mask(struct thread_mask_s *flat, struct arts_config_s *c,
                       unsigned int total_pus, const unsigned int *want_pu,
                       const unsigned int *want_numa) {
  unsigned int tc = c->thread_count;
  unsigned int role_seen[ARTS_ROLE_MAX] = {0};

  /* track distinct pu_ids via a small set */
  for (unsigned int t = 0; t < tc; t++) {
    if (flat[t].id != t) {
      fail("flat[t].id != t");
    }
    if (flat[t].pu_id >= total_pus) {
      fail("pu_id out of range");
    }
    if (flat[t].role >= ARTS_ROLE_MAX) {
      fail("role out of range");
    }
    /* group_pos must equal the count of same-role threads seen so far. */
    if (flat[t].group_pos != role_seen[flat[t].role]) {
      fail("group_pos not contiguous within role group");
    }
    role_seen[flat[t].role]++;
    if (flat[t].pu_id != want_pu[t]) {
      fprintf(stderr, "  thread %u: pu_id %u, expected %u\n", t, flat[t].pu_id,
              want_pu[t]);
      fail("pu_id is not the placement rule's slice element");
    }
    if (flat[t].numa_domain_id != want_numa[t]) {
      fprintf(stderr, "  thread %u: numa_domain_id %u, expected %u\n", t,
              flat[t].numa_domain_id, want_numa[t]);
      fail("numa_domain_id is not the node holding the thread's PU");
    }
  }

  /* distinctness */
  for (unsigned int i = 0; i < tc; i++) {
    for (unsigned int j = i + 1; j < tc; j++) {
      if (flat[i].pu_id == flat[j].pu_id) {
        fail("duplicate pu_id across threads");
      }
    }
  }

  /* role counts == config split (workers + progress == thread_count) */
  if (role_seen[ARTS_ROLE_WORKER] != c->worker_thread_count) {
    fail("worker count mismatch");
  }
  if (role_seen[ARTS_ROLE_PROGRESS] != tc - c->worker_thread_count) {
    fail("progress count mismatch");
  }

  /* Role layout must be [workers | progress] by index. */
  for (unsigned int t = 0; t < tc; t++) {
    enum arts_thread_role want =
        (t < c->worker_thread_count) ? ARTS_ROLE_WORKER : ARTS_ROLE_PROGRESS;
    if (flat[t].role != want) {
      fail("role layout not [workers|progress]");
    }
  }
}

/* Death test: a slice exceeding total_pus must abort.  Run in a child. */
static void death_test_offset_overflow(unsigned int total_pus) {
  pid_t pid = fork();
  if (pid < 0) {
    fail("fork failed");
  }
  if (pid == 0) {
    /* child: request a slice that overflows.  thread_count = total_pus, but
     * shared_pu_pool with my_rank=1 => offset = total_pus => slice exceeds. */
    struct arts_config_s c =
        make_config(total_pus, 0, /*shared_pool=*/true, /*my_rank=*/1);
    struct thread_mask_s *flat = calloc(total_pus, sizeof(*flat));
    /* expected to ARTS_ERROR -> arts_abort */
    arts_get_thread_mask(&c, flat, NULL);
    /* If we get here, the bounds check did NOT fire — fail loudly. */
    fprintf(stderr, "CHILD: arts_get_thread_mask returned without abort\n");
    _exit(0); /* exit 0 signals the death test FAILED to abort */
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    fail("waitpid failed");
  }
  if (WIFSIGNALED(status)) {
    fprintf(stderr, "  child died by signal %d\n", WTERMSIG(status));
    fail("offset-overflow crashed instead of reaching arts_abort");
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) == 0) {
    fail("offset-overflow did NOT trigger the bounds-check abort");
  }
}

static void run_machine(unsigned int m) {
  const char *desc = machines[m].desc;
  unsigned int total_pus = declare_machine(desc);
  if (total_pus != machines[m].pus) {
    fprintf(stderr,
            "FAIL topology_thread_mask: declared \"%s\" loads %u PUs, "
            "expected %u\n",
            desc, total_pus, machines[m].pus);
    exit(1);
  }

  /* Case 1: single-rank (no shared pool). */
  {
    struct arts_config_s c =
        make_config(SINGLE_TC, 1, /*shared_pool=*/false, /*rank=*/0);
    struct thread_mask_s *flat = calloc(SINGLE_TC, sizeof(*flat));
    if (!flat) {
      fail("calloc");
    }
    arts_get_thread_mask(&c, flat, NULL);
    check_mask(flat, &c, total_pus, machines[m].single_pu,
               machines[m].single_numa);
    if (arts_numa_domain_count != machines[m].numa) {
      fail("arts_numa_domain_count is not the declared NUMA node count");
    }
    free(flat);
  }

  /* Case 2: local-multinode disjoint slices (shared_pu_pool=true).  Rank r's
   * slice starts at PU offset r * SLICE_TC of the base order. */
  {
    struct arts_config_s c0 =
        make_config(SLICE_TC, 1, /*shared_pool=*/true, /*rank=*/0);
    struct arts_config_s c1 =
        make_config(SLICE_TC, 1, /*shared_pool=*/true, /*rank=*/1);
    struct thread_mask_s *f0 = calloc(SLICE_TC, sizeof(*f0));
    struct thread_mask_s *f1 = calloc(SLICE_TC, sizeof(*f1));
    if (!f0 || !f1) {
      fail("calloc");
    }
    arts_get_thread_mask(&c0, f0, NULL);
    check_mask(f0, &c0, total_pus, machines[m].rank0_pu,
               machines[m].rank0_numa);
    arts_get_thread_mask(&c1, f1, NULL);
    check_mask(f1, &c1, total_pus, machines[m].rank1_pu,
               machines[m].rank1_numa);

    /* Disjointness: no pu_id from rank 0's slice appears in rank 1's slice. */
    for (unsigned int i = 0; i < SLICE_TC; i++) {
      for (unsigned int j = 0; j < SLICE_TC; j++) {
        if (f0[i].pu_id == f1[j].pu_id) {
          fail("local-multinode slices NOT disjoint");
        }
      }
    }
    free(f0);
    free(f1);
  }

  /* Case 3: offset-overflow death test. */
  death_test_offset_overflow(total_pus);

  printf("  machine \"%s\": exact placement + in-range + distinct + "
         "role-split + group_pos contiguous + disjoint MN slices + "
         "offset-overflow abort (total_pus=%u num_numa=%u)\n",
         desc, total_pus, arts_numa_domain_count);
}

int main(void) {
  for (unsigned int m = 0; m < sizeof machines / sizeof machines[0]; m++) {
    run_machine(m);
  }
  printf("PASS topology_thread_mask: %zu declared machines\n",
         sizeof machines / sizeof machines[0]);
  return 0;
}
