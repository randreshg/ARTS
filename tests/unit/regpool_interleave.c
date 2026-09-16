/* An oversize payload is INTERLEAVED across the caller's node set.
 *
 * One object that every thread sweeps has no owning node, so the pool spreads
 * it: each member of the set holds a share and no single memory controller
 * serves the whole block.  Two things are asserted, because the pool's own
 * bookkeeping and the kernel's policy are separate facts and either can drift
 * from the other — the report must call the mapping interleaved, and
 * /proc/self/numa_maps must show the range carrying an interleave policy with
 * resident pages on more than one node.
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config.  Needs >= 2 NUMA
 * nodes; prints SKIP otherwise, so an environment that cannot exercise the
 * property does not read as an exercised pass. */
#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arts/memory/regpool.h"

#define SLAB_BYTES ((size_t)64 * 1024 * 1024)
#define BIG_BYTES ((size_t)64 * 1024 * 1024)

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

/* The report's count and MiB for the interleaved direct mappings. */
static void interleaved_counts(unsigned *out_count, unsigned *out_mib) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  assert(f != NULL);
  arts_regpool_report(f);
  fclose(f);
  const char *line = strstr(buf, "[REGPOOL] direct placement:");
  assert(line != NULL);
  const char *field = strstr(line, "interleaved=");
  unsigned count = 0, mib = 0;
  if (field != NULL)
    assert(sscanf(field, "interleaved=%u/%u", &count, &mib) == 2);
  free(buf);
  *out_count = count;
  *out_mib = mib;
}

/* The first two NUMA nodes this process may allocate on, as a mask.  Taken
 * from the process's own permitted set rather than assumed to be 0 and 1: a
 * memory policy naming a node outside it is refused by the kernel, and the
 * test would then be asserting against a mapping that carries no policy at
 * all.  0 when fewer than two are permitted. */
static uint64_t first_two_allowed_nodes(void) {
  FILE *f = fopen("/proc/self/status", "r");
  if (f == NULL)
    return 0;
  char line[4096];
  uint64_t mask = 0;
  unsigned found = 0;
  while (found < 2 && fgets(line, sizeof line, f) != NULL) {
    if (strncmp(line, "Mems_allowed_list:", 18) != 0)
      continue;
    const char *p = line + 18;
    while (found < 2) {
      char *end = NULL;
      long lo = strtol(p, &end, 10);
      if (end == p)
        break;
      long hi = lo;
      if (*end == '-') {
        p = end + 1;
        hi = strtol(p, &end, 10);
      }
      for (long k = lo; k <= hi && found < 2; k++) {
        if (k >= 0 && k < 64) {
          mask |= (uint64_t)1 << k;
          found++;
        }
      }
      if (*end != ',')
        break;
      p = end + 1;
    }
    break;
  }
  fclose(f);
  return (found == 2) ? mask : 0;
}

/* The numa_maps line for the mapping starting at `base`: how many nodes hold
 * pages of it, and whether its policy is an interleave.  Returns false when
 * the line is absent (no numa_maps on this kernel). */
static bool numa_maps_line(const void *base, bool *out_interleave,
                           unsigned *out_nodes_with_pages) {
  char want[32];
  snprintf(want, sizeof want, "%lx ", (unsigned long)(uintptr_t)base);
  FILE *f = fopen("/proc/self/numa_maps", "r");
  if (f == NULL)
    return false;
  char line[8192];
  bool found = false;
  while (fgets(line, sizeof line, f) != NULL) {
    if (strncmp(line, want, strlen(want)) != 0)
      continue;
    found = true;
    *out_interleave = (strstr(line, "interleave:") != NULL);
    unsigned nodes = 0;
    /* Per-node residency appears as " N<k>=<pages>" fields. */
    for (const char *p = line; (p = strstr(p, " N")) != NULL; p++) {
      const char *q = p + 2;
      if (!isdigit((unsigned char)*q))
        continue;
      while (isdigit((unsigned char)*q))
        q++;
      if (*q == '=')
        nodes++;
    }
    *out_nodes_with_pages = nodes;
    break;
  }
  fclose(f);
  return found;
}

int main(void) {
  unsigned nodes = count_nodes();
  if (nodes < 2) {
    printf("SKIP regpool_interleave (one NUMA node)\n");
    return 0;
  }

  /* A two-node set, as a rank whose threads sit on two nodes would have. */
  uint64_t set = first_two_allowed_nodes();
  if (set == 0) {
    printf("SKIP regpool_interleave (fewer than two nodes permitted)\n");
    return 0;
  }
  arts_regpool_set_topology(nodes, set, /*distance=*/NULL);
  assert(arts_regpool_init(NULL, NULL, SLAB_BYTES, 0));

  unsigned char *p = (unsigned char *)arts_regpool_alloc_aligned(BIG_BYTES, 64);
  assert(p != NULL);
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  assert(m != NULL);
  assert(m->base == p && m->len == BIG_BYTES);
  /* An interleaved mapping belongs to no single node. */
  assert(m->numa_node == -1);
  p[0] = 0x5A;
  p[BIG_BYTES / 2] = 0x5A;
  p[BIG_BYTES - 1] = 0x5A;

  unsigned count = 0, mib = 0;
  interleaved_counts(&count, &mib);
  assert(count == 1);
  assert(mib == (unsigned)(BIG_BYTES >> 20));

  bool interleave = false;
  unsigned with_pages = 0;
  if (!numa_maps_line(p, &interleave, &with_pages)) {
    arts_regpool_free(p);
    arts_regpool_cleanup();
    printf("SKIP regpool_interleave (no /proc/self/numa_maps entry)\n");
    return 0;
  }
  assert(interleave && "the mapping must carry an interleave policy");
  assert(with_pages >= 2 &&
         "an interleaved mapping must be resident on more than one node");

  arts_regpool_free(p);
  arts_regpool_cleanup();
  printf("PASS regpool_interleave (%u MiB interleaved over %u nodes)\n", mib,
         with_pages);
  return 0;
}
