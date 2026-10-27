/* A machine a test describes to the registered pool, in place of the one it
 * runs on, so the pool's placement and sizing decisions can be driven and
 * observed identically on any host.
 *
 * Including this header defines the link-time wrappers of `sched_getcpu`,
 * `syscall` and `fopen`, which the pool calls for every fact it takes from
 * the machine but the process's CPU affinity: the CPU a thread runs on
 * (sched_getcpu) and its node (getcpu, through syscall), the node a range is
 * placed on (mbind, through syscall), what the machine can still give
 * (/proc/meminfo's MemAvailable), a node's free pages (its sysfs meminfo) and
 * a node's CPUs (its sysfs cpulist).  Include it in exactly one translation
 * unit of a test program and link that program with
 * -Wl,--wrap=sched_getcpu,--wrap=syscall,--wrap=fopen: the link sends every
 * call the program's own objects make (the pool's and the allocator's
 * included) to the wrapper, and the C library's own definition stays its
 * __real_ name.  Until regpool_sim_declare() is called every call goes to the
 * C library unchanged.
 *
 * Once a machine of n nodes is declared:
 *   - getcpu reports the node set for the calling thread: node 0 unless
 *     regpool_sim_set_thread_node said otherwise.  A declaration sets the
 *     declaring thread back to node 0; any other live thread keeps the node
 *     it set, and a thread created later starts on node 0.  As the CPU,
 *     getcpu reports a number that stands for that node in this declaration
 *     and is no real CPU's, and sched_getcpu reports the same number: a
 *     caller that keeps the node it read for a CPU reads it again once the
 *     thread is moved or the machine declared again, and never takes a real
 *     CPU it saw before for a declared one;
 *   - mbind is recorded and reported successful without reaching the
 *     kernel, so a policy naming a node the host lacks is still observable
 *     (regpool_sim_mbind_at);
 *   - /proc/meminfo reads as one line, MemAvailable, carrying the declared
 *     machine availability (regpool_sim_set_machine_available, default
 *     REGPOOL_SIM_MACHINE_DEFAULT);
 *   - in a node's sysfs directory only meminfo can exist, and only for a
 *     node below n; every other file — the cpulist included, so a pool given
 *     no node set takes every declared node as the caller's — and every
 *     path of a node at or above n does not exist;
 *   - a node's meminfo follows its mode (the declared one, or what
 *     regpool_sim_set_node_mode set for that node):
 *       REGPOOL_SIM_UNKNOWN    it does not exist: the pool treats unknown
 *                              availability as room;
 *       REGPOOL_SIM_FREE       it reads MemFree as the byte count
 *                              regpool_sim_set_node_free declared for the
 *                              node (0 until declared), in whole KiB, at
 *                              every read — what the pool maps does not
 *                              lower it;
 *       REGPOOL_SIM_SELF_FULL  it reads as empty when it is the calling
 *                              thread's next fopen after a /proc/meminfo
 *                              read — the grow sizing that node's own slab —
 *                              and as REGPOOL_SIM_SELF_FULL_OTHER otherwise,
 *                              so every node is full to itself and has room
 *                              to every other reader.  A node the pool is
 *                              told to treat as full is never read, so the
 *                              read after a grow's machine read is then not
 *                              that node's: do not combine the two.
 *
 * Declaring a machine also clears ARTS_REGPOOL_FORCE_FULL_NODES from the
 * environment, so a test that wants nodes forced full sets it afterwards
 * and an inherited value never changes what a test asserts.  The process's
 * CPU affinity is still the host's; with every cpulist absent it never
 * narrows the node set.
 *
 * Every mbind is logged with all its arguments and with what the range held
 * at the call (regpool_sim_policy_ok states what a policy the pool sets must
 * satisfy), and the getcpu calls and the machine and node meminfo reads the
 * sim answers are counted (regpool_sim_consulted), so a pool that stops
 * consulting the declared machine fails the test rather than passing on the
 * host's facts.
 *
 * The node COUNT is not simulated: a test passes it to arts_regpool_init or
 * arts_regpool_set_topology, since the pool's own count lists the host's
 * sysfs directory.
 *
 * Threads: the calling thread's node and each kind of stream (machine,
 * node) are per thread — a stream has one buffer per thread, which that
 * thread's next open of the same kind overwrites, and the pool reads and
 * closes each stream before it opens the next.  The declared values are plain
 * variables: declare them before a second thread runs.  The counters are
 * atomic and count every call whatever thread makes it, and the policy log is
 * appended under a lock; read the log once the threads that use the pool are
 * done. */
#ifndef REGPOOL_NUMA_SIM_H
#define REGPOOL_NUMA_SIM_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "arts/utils/stdatomic_compat.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef enum {
  REGPOOL_SIM_UNKNOWN,
  REGPOOL_SIM_FREE,
  REGPOOL_SIM_SELF_FULL,
} regpool_sim_avail_t;

typedef struct {
  uintptr_t base;
  unsigned long len;
  int mode;
  unsigned long mask;    /* first word of the nodemask */
  unsigned long maxnode; /* the kernel reads maxnode - 1 bits of the mask */
  unsigned long flags;
  bool mapped;   /* every page of the range was mapped at the call */
  bool resident; /* some page of the range was resident at the call */
} regpool_sim_mbind_t;

#define REGPOOL_SIM_MAX_NODES 64
/* Above every real CPU number (the kernel's limit is 8192). */
#define REGPOOL_SIM_CPU_BASE (1 << 20)
#define REGPOOL_SIM_MAX_MBINDS 1024
#define REGPOOL_SIM_MPOL_PREFERRED 1
#define REGPOOL_SIM_MPOL_INTERLEAVE 3
#define REGPOOL_SIM_MACHINE_DEFAULT ((size_t)64 << 30)
#define REGPOOL_SIM_SELF_FULL_OTHER ((size_t)1 << 40)

static int regpool_sim_nodes; /* 0: not declared, the real machine answers */
static unsigned regpool_sim_declarations; /* one per regpool_sim_declare */
static regpool_sim_avail_t regpool_sim_avail[REGPOOL_SIM_MAX_NODES];
static size_t regpool_sim_machine_avail;
static size_t regpool_sim_node_free[REGPOOL_SIM_MAX_NODES];
static __thread int regpool_sim_thread_node = -1;
static __thread bool regpool_sim_after_machine_read;
/* The policy log and its length: appended under the lock. */
static pthread_mutex_t regpool_sim_mbind_lock = PTHREAD_MUTEX_INITIALIZER;
static regpool_sim_mbind_t regpool_sim_mbinds[REGPOOL_SIM_MAX_MBINDS];
static unsigned regpool_sim_nmbinds; /* logged, at most the log's size */
/* Call counters, bumped by whichever thread calls: only their own values. */
static _Atomic(unsigned) regpool_sim_mbind_calls;
static _Atomic(unsigned) regpool_sim_getcpu_calls;
static _Atomic(unsigned) regpool_sim_machine_reads;
/* Meminfo reads of a declared node. */
static _Atomic(unsigned) regpool_sim_node_reads;
/* SELF_FULL reads: a node read as empty by its own grow, and as having room
 * by anyone else. */
static _Atomic(unsigned) regpool_sim_self_reads[REGPOOL_SIM_MAX_NODES];
static _Atomic(unsigned) regpool_sim_other_reads[REGPOOL_SIM_MAX_NODES];

static inline void regpool_sim_declare(int nodes, regpool_sim_avail_t avail) {
  if (nodes < 1 || nodes > REGPOOL_SIM_MAX_NODES)
    abort();
  regpool_sim_nodes = nodes;
  regpool_sim_declarations++;
  for (int i = 0; i < REGPOOL_SIM_MAX_NODES; i++)
    regpool_sim_avail[i] = avail;
  regpool_sim_after_machine_read = false;
  regpool_sim_thread_node = -1;
  unsetenv("ARTS_REGPOOL_FORCE_FULL_NODES");
  regpool_sim_machine_avail = REGPOOL_SIM_MACHINE_DEFAULT;
  memset(regpool_sim_node_free, 0, sizeof regpool_sim_node_free);
  regpool_sim_nmbinds = 0;
  atomic_store_explicit(&regpool_sim_mbind_calls, 0, memory_order_relaxed);
  atomic_store_explicit(&regpool_sim_getcpu_calls, 0, memory_order_relaxed);
  atomic_store_explicit(&regpool_sim_machine_reads, 0, memory_order_relaxed);
  atomic_store_explicit(&regpool_sim_node_reads, 0, memory_order_relaxed);
  for (int i = 0; i < REGPOOL_SIM_MAX_NODES; i++) {
    atomic_store_explicit(&regpool_sim_self_reads[i], 0, memory_order_relaxed);
    atomic_store_explicit(&regpool_sim_other_reads[i], 0, memory_order_relaxed);
  }
}

static inline void regpool_sim_set_thread_node(int node) {
  regpool_sim_thread_node = node;
}

static inline void regpool_sim_set_machine_available(size_t bytes) {
  regpool_sim_machine_avail = bytes;
}

static inline void regpool_sim_set_node_mode(int node,
                                             regpool_sim_avail_t avail) {
  if (node < 0 || node >= REGPOOL_SIM_MAX_NODES)
    abort();
  regpool_sim_avail[node] = avail;
}

static inline void regpool_sim_set_node_free(int node, size_t bytes) {
  if (node < 0 || node >= REGPOOL_SIM_MAX_NODES)
    abort();
  regpool_sim_node_free[node] = bytes;
}

/* The latest policy recorded for a range starting at `base`, or NULL. */
static inline const regpool_sim_mbind_t *
regpool_sim_mbind_at(const void *base) {
  for (unsigned i = regpool_sim_nmbinds; i-- > 0;) {
    if (regpool_sim_mbinds[i].base == (uintptr_t)base)
      return &regpool_sim_mbinds[i];
  }
  return NULL;
}

/* What every policy the pool sets must satisfy: set on a mapped range none of
 * whose pages is resident yet (a policy governs only later faults, so one
 * set after population places nothing), with no MPOL_MF_* flag (there is
 * nothing resident to move, and a preference must not fail a mapping), and
 * with a one-word mask that names only declared nodes, at least one, all
 * within the maxnode - 1 bits the kernel reads — and no bit past the word,
 * which the kernel would read from beyond the mask. */
static inline bool regpool_sim_policy_ok(const regpool_sim_mbind_t *r) {
  int n = regpool_sim_nodes;
  unsigned long declared = n >= 64 ? ~0ul : (1ul << n) - 1;
  return r != NULL && r->mapped && !r->resident && r->flags == 0 &&
         r->maxnode > (unsigned long)n &&
         r->maxnode <= 8 * sizeof(unsigned long) + 1 && r->mask != 0 &&
         (r->mask & ~declared) == 0;
}

/* Whether every logged policy satisfies regpool_sim_policy_ok. */
static inline bool regpool_sim_all_policies_ok(void) {
  for (unsigned i = 0; i < regpool_sim_nmbinds; i++) {
    if (!regpool_sim_policy_ok(&regpool_sim_mbinds[i]))
      return false;
  }
  return regpool_sim_nmbinds ==
         atomic_load_explicit(&regpool_sim_mbind_calls, memory_order_relaxed);
}

/* Whether the pool asked the declared machine for each kind of fact it
 * decides on: the calling thread's node, the machine's availability and a
 * node's. */
static inline bool regpool_sim_consulted(void) {
  unsigned getcpu =
      atomic_load_explicit(&regpool_sim_getcpu_calls, memory_order_relaxed);
  unsigned machine =
      atomic_load_explicit(&regpool_sim_machine_reads, memory_order_relaxed);
  unsigned node =
      atomic_load_explicit(&regpool_sim_node_reads, memory_order_relaxed);
  return getcpu > 0 && machine > 0 && node > 0;
}

/* Record, for [base, base + len), whether every page is mapped and whether
 * any is resident. */
static inline void regpool_sim_probe_range(uintptr_t base, unsigned long len,
                                           regpool_sim_mbind_t *r) {
  unsigned long page = (unsigned long)sysconf(_SC_PAGESIZE);
  unsigned char vec[4096];
  r->mapped = true;
  r->resident = false;
  for (unsigned long off = 0; off < len; off += sizeof vec * page) {
    unsigned long chunk =
        len - off < sizeof vec * page ? len - off : sizeof vec * page;
    if (mincore((void *)(base + off), chunk, vec) != 0) {
      r->mapped = false;
      return;
    }
    for (unsigned long i = 0; i < (chunk + page - 1) / page; i++) {
      if (vec[i] & 1)
        r->resident = true;
    }
  }
}

static inline int regpool_sim_node_of_thread(void) {
  return regpool_sim_thread_node >= 0 ? regpool_sim_thread_node : 0;
}

/* The CPU that stands for the calling thread's node in this declaration. */
static inline int regpool_sim_cpu_of_thread(void) {
  return REGPOOL_SIM_CPU_BASE +
         (int)regpool_sim_declarations * REGPOOL_SIM_MAX_NODES +
         regpool_sim_node_of_thread();
}

long __real_syscall(long nr, ...);
int __real_sched_getcpu(void);
FILE *__real_fopen(const char *path, const char *mode);

long __wrap_syscall(long nr, ...) {
  va_list ap;
  va_start(ap, nr);
  long a0 = va_arg(ap, long), a1 = va_arg(ap, long), a2 = va_arg(ap, long);
  long a3 = va_arg(ap, long), a4 = va_arg(ap, long), a5 = va_arg(ap, long);
  va_end(ap);
  if (regpool_sim_nodes > 0) {
    if (nr == SYS_getcpu) {
      atomic_fetch_add_explicit(&regpool_sim_getcpu_calls, 1u,
                                memory_order_relaxed);
      unsigned *cpu = (unsigned *)a0, *node = (unsigned *)a1;
      if (cpu != NULL)
        *cpu = (unsigned)regpool_sim_cpu_of_thread();
      if (node != NULL)
        *node = (unsigned)regpool_sim_node_of_thread();
      return 0;
    }
    if (nr == SYS_mbind) {
      pthread_mutex_lock(&regpool_sim_mbind_lock);
      atomic_fetch_add_explicit(&regpool_sim_mbind_calls, 1u,
                                memory_order_relaxed);
      if (regpool_sim_nmbinds < REGPOOL_SIM_MAX_MBINDS) {
        regpool_sim_mbind_t *r = &regpool_sim_mbinds[regpool_sim_nmbinds++];
        r->base = (uintptr_t)a0;
        r->len = (unsigned long)a1;
        r->mode = (int)a2;
        r->mask = a3 != 0 ? *(const unsigned long *)a3 : 0;
        r->maxnode = (unsigned long)a4;
        r->flags = (unsigned long)a5;
        regpool_sim_probe_range(r->base, r->len, r);
      }
      pthread_mutex_unlock(&regpool_sim_mbind_lock);
      return 0;
    }
  }
  return __real_syscall(nr, a0, a1, a2, a3, a4, a5);
}

int __wrap_sched_getcpu(void) {
  if (regpool_sim_nodes > 0)
    return regpool_sim_cpu_of_thread();
  return __real_sched_getcpu();
}

/* A read-only stream over `text`, which must outlive the stream. */
static inline FILE *regpool_sim_stream(const char *text) {
  return fmemopen((void *)text, strlen(text), "r");
}

FILE *__wrap_fopen(const char *path, const char *mode) {
  if (regpool_sim_nodes <= 0 || path == NULL)
    return __real_fopen(path, mode);
  bool own = regpool_sim_after_machine_read;
  regpool_sim_after_machine_read = false;
  if (strcmp(path, "/proc/meminfo") == 0) {
    atomic_fetch_add_explicit(&regpool_sim_machine_reads, 1u,
                              memory_order_relaxed);
    static __thread char machine[64];
    snprintf(machine, sizeof machine, "MemAvailable:   %zu kB\n",
             regpool_sim_machine_avail >> 10);
    regpool_sim_after_machine_read = true;
    return regpool_sim_stream(machine);
  }
  static const char node_dir[] = "/sys/devices/system/node/node";
  if (strncmp(path, node_dir, sizeof node_dir - 1) != 0)
    return __real_fopen(path, mode);
  char *file = NULL;
  long n = strtol(path + sizeof node_dir - 1, &file, 10);
  if (file == path + sizeof node_dir - 1 || *file != '/')
    return __real_fopen(path, mode);
  file++;
  bool meminfo = strcmp(file, "meminfo") == 0;
  if (meminfo && n >= 0 && n < regpool_sim_nodes)
    atomic_fetch_add_explicit(&regpool_sim_node_reads, 1u,
                              memory_order_relaxed);
  if (n < 0 || n >= regpool_sim_nodes || !meminfo ||
      regpool_sim_avail[n] == REGPOOL_SIM_UNKNOWN) {
    errno = ENOENT;
    return NULL;
  }
  size_t bytes;
  if (regpool_sim_avail[n] == REGPOOL_SIM_FREE) {
    bytes = regpool_sim_node_free[n];
  } else {
    bytes = own ? 0 : REGPOOL_SIM_SELF_FULL_OTHER;
    if (own)
      atomic_fetch_add_explicit(&regpool_sim_self_reads[n], 1u,
                                memory_order_relaxed);
    else
      atomic_fetch_add_explicit(&regpool_sim_other_reads[n], 1u,
                                memory_order_relaxed);
  }
  static __thread char node[96];
  snprintf(node, sizeof node, "Node %ld MemFree: %zu kB\n", n, bytes >> 10);
  return regpool_sim_stream(node);
}

#endif /* REGPOOL_NUMA_SIM_H */
