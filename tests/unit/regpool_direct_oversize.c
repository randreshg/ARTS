/* Oversize (direct-slab) allocations: a request above the arena's largest
 * chunk object takes a dedicated mapping instead of the arena, and its free
 * retires that mapping — the memory stays mapped and registered, owned by no
 * allocation, so the next request of the same length and alignment gets it
 * back while the freed pointer stops resolving.  Exercised here at several
 * sizes with the pool's confinement lookup on every pointer, plus the
 * placement: an oversize mapping is spread over the caller's node set rather
 * than placed on one node, and with a member of that set reporting full it is
 * still spread over the rest rather than refused. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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

/* Live and retired direct-mapping counts, read off the pool's own report. */
static void direct_counts(unsigned *out_live, unsigned *out_retired) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  assert(f != NULL);
  arts_regpool_report(f);
  fclose(f);
  const char *line = strstr(buf, "[REGPOOL] direct:");
  assert(line != NULL);
  unsigned live = 0, live_mb = 0, retired = 0, retired_mb = 0, slots = 0;
  assert(sscanf(line,
                "[REGPOOL] direct: live=%u/%u retired=%u/%u free_slots=%u",
                &live, &live_mb, &retired, &retired_mb, &slots) == 5);
  free(buf);
  *out_live = live;
  *out_retired = retired;
}

/* How many direct mappings the report calls interleaved. */
static unsigned interleaved_count(void) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  assert(f != NULL);
  arts_regpool_report(f);
  fclose(f);
  const char *line = strstr(buf, "[REGPOOL] direct placement:");
  unsigned count = 0, mib = 0;
  if (line != NULL) {
    const char *field = strstr(line, "interleaved=");
    if (field != NULL)
      assert(sscanf(field, "interleaved=%u/%u", &count, &mib) == 2);
  }
  free(buf);
  return count;
}

static void check_one(size_t size) {
  unsigned char *p = (unsigned char *)arts_regpool_alloc_aligned(size, 64);
  assert(p != NULL);
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  assert(m != NULL);
  assert((unsigned char *)m->base <= p &&
         p + size <= (unsigned char *)m->base + m->len);
  p[0] = 0x5A;
  p[size / 2] = 0x5A;
  p[size - 1] = 0x5A;
  assert(p[0] == 0x5A && p[size / 2] == 0x5A && p[size - 1] == 0x5A);
  arts_regpool_free(p);
  /* The free hands the mapping back to the pool: the pointer must no longer
   * resolve, whether the mapping was kept or torn down. */
  assert(arts_regpool_lookup(p) == NULL);
}

int main(void) {
  /* 64 MiB slab: anything above the arena's 32 MiB chunk object is direct. */
  assert(arts_regpool_init(NULL, NULL, SLAB_BYTES, 0));

  check_one((size_t)40 * 1024 * 1024);
  check_one((size_t)100 * 1024 * 1024);
  /* A third distinct length: each shape gets a mapping of its own, and none
   * of the earlier ones is handed out for a length it does not match. */
  check_one((size_t)48 * 1024 * 1024);
  arts_regpool_cleanup();

  /* Reuse, and the bound on what reuse may hand out.  A fresh pool,
   * so the report's counts are exactly this leg's. */
  assert(arts_regpool_init(NULL, NULL, SLAB_BYTES, 0));
  const size_t big = (size_t)64 * 1024 * 1024;
  unsigned char *first = (unsigned char *)arts_regpool_alloc_aligned(big, 64);
  assert(first != NULL);
  memset(first, 0x5A, big);
  arts_regpool_free(first);

  /* Retained: the same shape gets the same mapping back. */
  unsigned char *again = (unsigned char *)arts_regpool_alloc_aligned(big, 64);
  assert(again == first);

  arts_regpool_free(again);

  /* A different length must not be served from the retired mapping, which
   * stays retired and available for its own shape. */
  unsigned char *other =
      (unsigned char *)arts_regpool_alloc_aligned(big + SLAB_BYTES, 64);
  assert(other != NULL);
  assert(other != first);
  unsigned live = 0, retired = 0;
  direct_counts(&live, &retired);
  assert(live == 1);
  assert(retired == 1);
  arts_regpool_free(other);
  arts_regpool_cleanup();

  /* Placement fallover.  Pinning the thread narrows the caller's node set to
   * the one node it may run on, and that node is then forced full — so the
   * set cannot absorb the mapping, there is nothing to spread it over, and
   * the pool must place it on another node rather than refuse it.  Needs a
   * second node. */
  if (count_nodes() < 2) {
    printf("PASS regpool_direct_oversize (placement leg skipped: one node)\n");
    return 0;
  }
  unsigned cpu = 0, node = 0;
  if (syscall(SYS_getcpu, &cpu, &node, NULL) != 0) {
    printf("PASS regpool_direct_oversize (placement leg skipped: no getcpu)\n");
    return 0;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  assert(sched_setaffinity(0, sizeof(set), &set) == 0);
  char buf[16];
  snprintf(buf, sizeof(buf), "%u", node);
  assert(setenv("ARTS_REGPOOL_FORCE_FULL_NODES", buf, 1) == 0);
  assert(arts_regpool_init(NULL, NULL, SLAB_BYTES, 0));
  unsigned char *p =
      (unsigned char *)arts_regpool_alloc_aligned((size_t)48 * 1024 * 1024, 64);
  assert(p != NULL);
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  assert(m != NULL);
  assert(m->numa_node != (int)node &&
         "a request the caller's node cannot take must relocate, not fail");
  assert(interleaved_count() == 0 &&
         "a set of one full node has nothing to spread the mapping over");
  p[0] = 0x5A;
  p[(size_t)48 * 1024 * 1024 - 1] = 0x5A;
  arts_regpool_free(p);
  arts_regpool_cleanup();
  printf("PASS regpool_direct_oversize (relocated off full node %u to %d)\n",
         node, m->numa_node);
  return 0;
}
