/* SPDX-License-Identifier: Apache-2.0
 *
 * Segment probes for the route-table whitebox tests that include
 * route_table.c: the CPU segment ops wrapped to count and record every build
 * and discard, and route_table.c's own mmap/munmap interposed at link time
 * (-Wl,--wrap=mmap,--wrap=munmap).  --wrap rewrites only the undefined
 * references of the objects it links, and the test object is the only one
 * that includes route_table.c, so the C library's, the allocator's and the
 * sanitizers' own mappings are untouched.  Include after route_table.c.
 */
#ifndef ROUTE_TABLE_SEGMENT_PROBE_H
#define ROUTE_TABLE_SEGMENT_PROBE_H

#include <errno.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/types.h>

void *__real_mmap(void *addr, size_t len, int prot, int flags, int fd,
                  off_t off);
int __real_munmap(void *addr, size_t len);

/* mmap failures to inject, with ENOMEM: the calling thread's next
 * probe_fail_mine calls, and every call while probe_fail_all is set. */
static __thread unsigned probe_fail_mine;
static _Atomic(int) probe_fail_all;
static _Atomic(unsigned) probe_mmap_failed;

void *__wrap_mmap(void *addr, size_t len, int prot, int flags, int fd,
                  off_t off) {
  if (probe_fail_mine > 0 ||
      atomic_load_explicit(&probe_fail_all, memory_order_relaxed)) {
    if (probe_fail_mine > 0) {
      probe_fail_mine--;
    }
    atomic_fetch_add_explicit(&probe_mmap_failed, 1u, memory_order_relaxed);
    errno = ENOMEM;
    return MAP_FAILED;
  }
  return __real_mmap(addr, len, prot, flags, fd, off);
}

#define PROBE_MAX 4096u
typedef struct {
  void *addr;
  size_t len;
  int rc;
} probe_range_t;

/* Each entry is written once, by the thread that drew its index, and read
 * only after that thread was joined. */
static probe_range_t probe_unmaps[PROBE_MAX];
static _Atomic(unsigned) probe_unmap_count;

int __wrap_munmap(void *addr, size_t len) {
  int rc = __real_munmap(addr, len);
  unsigned i =
      atomic_fetch_add_explicit(&probe_unmap_count, 1u, memory_order_relaxed);
  if (i < PROBE_MAX) {
    probe_unmaps[i] = (probe_range_t){addr, len, rc};
  }
  return rc;
}

/* ── the CPU segment ops, counted ──────────────────────────────────────── */

static _Atomic(unsigned) probe_builds;
static _Atomic(unsigned) probe_build_failures;
static _Atomic(unsigned) probe_discards;
static probe_range_t probe_discarded[PROBE_MAX];
/* errno right after the calling thread's last failed build returned. */
static __thread int probe_build_errno;
/* Runs on entry to every build, before the real one: where a test holds its
 * racers.  Set while no build runs. */
static void (*probe_build_hook)(void);

static size_t probe_seg_bytes(const arts_route_table_t *t) {
  return (size_t)t->size * COLLISION_RESOLVES * sizeof(arts_route_item_t);
}

static arts_route_table_t *probe_build(unsigned int windows,
                                       unsigned int shift);
static void probe_discard(arts_route_table_t *t);
static const arts_route_segment_ops_t probe_ops = {probe_build, probe_discard};

static arts_route_table_t *probe_build(unsigned int windows,
                                       unsigned int shift) {
  if (probe_build_hook != NULL) {
    probe_build_hook();
  }
  arts_route_table_t *t = route_segment_cpu_ops.build(windows, shift);
  if (t == NULL) {
    probe_build_errno = errno;
    atomic_fetch_add_explicit(&probe_build_failures, 1u, memory_order_relaxed);
    errno = probe_build_errno;
    return NULL;
  }
  atomic_fetch_add_explicit(&probe_builds, 1u, memory_order_relaxed);
  t->ops = &probe_ops;
  return t;
}

static void probe_discard(arts_route_table_t *t) {
  unsigned i =
      atomic_fetch_add_explicit(&probe_discards, 1u, memory_order_relaxed);
  if (i < PROBE_MAX) {
    probe_discarded[i] = (probe_range_t){t->data, probe_seg_bytes(t), 0};
  }
  route_segment_cpu_ops.discard(t);
}

/* Counters back to zero; call with no build, discard or unmap in flight. */
static void probe_reset(void) {
  atomic_store_explicit(&probe_builds, 0u, memory_order_relaxed);
  atomic_store_explicit(&probe_build_failures, 0u, memory_order_relaxed);
  atomic_store_explicit(&probe_discards, 0u, memory_order_relaxed);
  atomic_store_explicit(&probe_unmap_count, 0u, memory_order_relaxed);
  atomic_store_explicit(&probe_mmap_failed, 0u, memory_order_relaxed);
}

/* A root whose growth goes through the probe. */
static arts_route_table_t *probe_root(unsigned int windows,
                                      unsigned int shift) {
  arts_route_table_t *t = arts_new_route_table(windows, shift);
  t->ops = &probe_ops;
  return t;
}

/* Whether munmap was called for exactly [addr, addr + len) and returned 0.
 * An exact record of the call is immune to a later mapping reusing the range,
 * which a residency probe of the range is not. */
static int probe_unmapped(const void *addr, size_t len) {
  unsigned n = atomic_load_explicit(&probe_unmap_count, memory_order_relaxed);
  for (unsigned i = 0; i < n && i < PROBE_MAX; i++) {
    if (probe_unmaps[i].addr == addr && probe_unmaps[i].len == len &&
        probe_unmaps[i].rc == 0) {
      return 1;
    }
  }
  return 0;
}

/* Discards from index `from` on whose exact range no successful munmap was
 * recorded. */
static unsigned probe_discards_not_unmapped(unsigned from) {
  unsigned n = atomic_load_explicit(&probe_discards, memory_order_relaxed);
  unsigned bad = 0;
  for (unsigned i = from; i < n && i < PROBE_MAX; i++) {
    bad += !probe_unmapped(probe_discarded[i].addr, probe_discarded[i].len);
  }
  return bad;
}

/* Claim words (BUSY, CLAIM, LOST) and RETIRING words left in a chain, and
 * slots publishing `key` (0: none counted). */
static void probe_scan_chain(arts_route_table_t *root, arts_guid_t key,
                             unsigned *claim_words, unsigned *published) {
  *claim_words = 0;
  *published = 0;
  for (arts_route_table_t *t = root; t != NULL;
       t = atomic_load_explicit(&t->next, memory_order_acquire)) {
    for (uint64_t i = 0; i < (uint64_t)t->size * COLLISION_RESOLVES; i++) {
      arts_guid_t k =
          atomic_load_explicit(&t->data[i].key, memory_order_acquire);
      *claim_words += k == ROUTE_KEY_BUSY || ROUTE_KEY_IS_CLAIM(k) ||
                      ROUTE_KEY_IS_LOST(k) || k == ARTS_ROUTE_KEY_RETIRING;
      *published += key != 0 && k == key;
    }
  }
}

#endif
