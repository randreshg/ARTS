/* Lazy carving: a node's memory is mapped when a thread on that node first
 * allocates, not at init.  Init owes only the proof that the pool can map at
 * all, so exactly one node — the initializing thread's — is carved; a node
 * that no thread has allocated on is UNTRIED, which is memory that exists
 * and has simply not been asked for, and a thread moved onto such a node
 * must get ITS node's memory rather than another node's arena (the fallback
 * path is for a node the pool tried and could not carve).
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config.  Needs two NUMA
 * nodes and, for the second leg, a CPU of another node inside this process's
 * affinity mask; both are announced as SKIP. */
#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "arts/memory/regpool.h"

#define SLAB_BYTES ((size_t)64 * 1024 * 1024)

static unsigned count_nodes(void) {
  DIR *d = opendir("/sys/devices/system/node");
  if (d == NULL)
    return 1;
  unsigned n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "node", 4) == 0 &&
        isdigit((unsigned char)e->d_name[4]))
      n++;
  }
  closedir(d);
  return n ? n : 1;
}

static unsigned carved_nodes(void) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  assert(f != NULL);
  arts_regpool_report(f);
  fclose(f);
  unsigned n = 0;
  for (const char *p = buf; (p = strstr(p, "state=carved")) != NULL;
       p += strlen("state=carved"))
    n++;
  free(buf);
  return n;
}

/* First CPU of `node` this process is allowed to run on, or -1. */
static int cpu_of_node(unsigned node, const cpu_set_t *allowed) {
  char path[64];
  snprintf(path, sizeof path, "/sys/devices/system/node/node%u/cpulist", node);
  FILE *f = fopen(path, "r");
  if (f == NULL)
    return -1;
  char buf[4096];
  char *line = fgets(buf, sizeof buf, f);
  fclose(f);
  if (line == NULL)
    return -1;
  char *save = NULL;
  for (char *tok = strtok_r(buf, ",\n", &save); tok != NULL;
       tok = strtok_r(NULL, ",\n", &save)) {
    int lo = 0, hi = 0;
    if (sscanf(tok, "%d-%d", &lo, &hi) != 2) {
      if (sscanf(tok, "%d", &lo) != 1)
        continue;
      hi = lo;
    }
    for (int c = lo; c <= hi && c < CPU_SETSIZE; c++) {
      if (CPU_ISSET(c, allowed))
        return c;
    }
  }
  return -1;
}

int main(void) {
  unsigned nodes = count_nodes();
  if (nodes < 2) {
    printf("SKIP regpool_lazy_carve: single NUMA node — nothing to carve on "
           "demand\n");
    return 0;
  }
  cpu_set_t allowed;
  if (sched_getaffinity(0, sizeof allowed, &allowed) != 0) {
    printf("SKIP regpool_lazy_carve: affinity mask unavailable\n");
    return 0;
  }
  unsigned cpu = 0, node = 0;
  if (syscall(SYS_getcpu, &cpu, &node, NULL) != 0) {
    printf("SKIP regpool_lazy_carve: getcpu unavailable\n");
    return 0;
  }

  assert(arts_regpool_init(NULL, NULL, SLAB_BYTES, 0));
  assert(carved_nodes() == 1 &&
         "init must carve the initializing thread's node and no other");

  int target = -1, target_cpu = -1;
  for (unsigned k = 0; k < nodes && target < 0; k++) {
    if (k == node)
      continue;
    int c = cpu_of_node(k, &allowed);
    if (c >= 0) {
      target = (int)k;
      target_cpu = c;
    }
  }
  if (target < 0) {
    arts_regpool_cleanup();
    printf("SKIP regpool_lazy_carve: no CPU of a second node in this "
           "process's affinity mask\n");
    return 0;
  }

  cpu_set_t one;
  CPU_ZERO(&one);
  CPU_SET(target_cpu, &one);
  assert(sched_setaffinity(0, sizeof one, &one) == 0);

  void *p = arts_regpool_alloc_aligned(1 << 20, 64);
  assert(p != NULL);
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  assert(m != NULL);
  assert(m->numa_node == target &&
         "a thread on an uncarved node must be served from that node");
  assert(carved_nodes() == 2);
  memset(p, 0xA5, 1 << 20);
  arts_regpool_free(p);

  arts_regpool_cleanup();
  printf("PASS regpool_lazy_carve: init carved node %u, node %d carved by its "
         "first allocation\n",
         node, target);
  return 0;
}
