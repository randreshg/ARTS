/* The registered pool's request classes against the allocator it links.
 *
 * The pool keeps one binding per (thread, request class) on the premise that
 * an arena serves or refuses two requests of one class alike: requests of one
 * class are decided by one of the allocator's page queues.  For every
 * alignment the arena path takes (64 B to 64 KiB), every size up to twice the
 * alignment plus 4 KiB, then every size at and around each point where the
 * block size of the size or of the over-allocated size changes, every 4 KiB
 * multiple to 1 MiB and a stride to the arena path's largest object (32 MiB):
 * a fresh heap confined to an exclusive arena, as the pool's heaps are,
 * serves the request alone, so its only page's block size names the queue
 * that decided.  Two requests of one class key must name one queue (or both
 * a singleton page).  Guarded sampling sends a sampled request elsewhere, so
 * the premise and this test assume it off.
 *
 * Whitebox unit test: links the allocator alone; no pool, no runtime. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <mimalloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "arts/memory/regpool_class.h"

#define ARENA_OBJ_MAX ((size_t)32 << 20)
#define ARENA_BYTES ((size_t)128 << 20)
#define SLOTS (1u << 16)

static size_t g_block;
static unsigned g_pages;
static bool visit(const mi_heap_t *h, const mi_heap_area_t *a, void *b,
                  size_t bs, void *arg) {
  (void)h; (void)b; (void)bs; (void)arg;
  if (a != NULL && a->used > 0) {
    g_block = a->full_block_size;
    g_pages++;
  }
  return true;
}

static uint64_t g_key[SLOTS], g_queue[SLOTS];
static size_t g_first[SLOTS];
static unsigned long g_requests, g_classes, g_bad;
static mi_arena_id_t g_arena;

static void one(size_t size, size_t align) {
  if (size == 0 || size > ARENA_OBJ_MAX)
    return;
  mi_heap_t *h = mi_heap_new_in_arena(g_arena);
  if (h == NULL) {
    printf("FAIL regpool_request_classes: no heap in the arena\n");
    exit(1);
  }
  void *p = mi_heap_malloc_aligned(h, size, align);
  g_block = 0;
  g_pages = 0;
  if (p != NULL)
    mi_heap_visit_blocks(h, false, visit, NULL);
  mi_heap_destroy(h);
  if (p == NULL || g_pages != 1) {
    if (g_bad++ < 10)
      printf("FAIL regpool_request_classes: size %zu align %zu: %s\n", size,
             align, p == NULL ? "refused" : "not one page");
    return;
  }
  uint64_t queue = g_block > MI_LARGE_MAX_OBJ_SIZE ? ARTS_REGPOOL_CLASS_SINGLETON
                                                   : (uint64_t)g_block;
  uint64_t key = arts_regpool_class_key(size, align);
  g_requests++;
  unsigned i = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 40) & (SLOTS - 1);
  while (g_key[i] != 0 && g_key[i] != key)
    i = (i + 1) & (SLOTS - 1);
  if (g_key[i] == 0) {
    g_key[i] = key;
    g_queue[i] = queue;
    g_first[i] = size;
    g_classes++;
  } else if (g_queue[i] != queue && g_bad++ < 10) {
    printf("FAIL regpool_request_classes: class %#llx: size %zu -> block "
           "%llu, size %zu -> block %llu (align %zu)\n",
           (unsigned long long)key, g_first[i], (unsigned long long)g_queue[i],
           size, (unsigned long long)queue, align);
  }
}

static void around(size_t s, size_t align) {
  for (long d = -2; d <= 2; d++)
    if ((long)s + d > 0)
      one((size_t)((long)s + d), align);
}

int main(void) {
  if (mi_option_get(mi_option_guarded_sample_rate) != 0) {
    printf("FAIL regpool_request_classes: guarded sampling is on "
           "(MIMALLOC_GUARDED_SAMPLE_RATE); the classes assume it off\n");
    return 1;
  }
  size_t align32 = (size_t)32 << 20;
  void *raw = mmap(NULL, ARENA_BYTES + align32, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (raw == MAP_FAILED) {
    printf("FAIL regpool_request_classes: mmap\n");
    return 1;
  }
  uintptr_t base = ((uintptr_t)raw + align32 - 1) & ~(uintptr_t)(align32 - 1);
  if (!mi_manage_os_memory_ex((void *)base, ARENA_BYTES, true, false, true, -1,
                              true, &g_arena)) {
    printf("FAIL regpool_request_classes: no exclusive arena\n");
    return 1;
  }
  /* Every size where the block size changes, from the allocator itself. */
  static size_t edges[4096];
  unsigned ne = 0;
  size_t prev = 0;
  for (size_t s = 1; s <= MI_LARGE_MAX_OBJ_SIZE + 64 && ne < 4096; s++) {
    size_t g = mi_good_size(s);
    if (g != prev) {
      edges[ne++] = s;
      prev = g;
    }
  }
  for (size_t align = 64; align <= 65536; align <<= 1) {
    for (size_t s = 1; s <= 2 * align + 4096; s++)
      one(s, align);
    for (unsigned e = 0; e < ne; e++) {
      around(edges[e], align);             /* the size's block size changes */
      if (edges[e] > align)                /* the over-allocated size's does */
        around(edges[e] - align + 1, align);
    }
    for (size_t m = 4096; m <= ((size_t)1 << 20); m += 4096)
      around(m, align);
    around(MI_LARGE_MAX_OBJ_SIZE, align);
    around(MI_LARGE_MAX_OBJ_SIZE - align + 1, align);
    for (size_t s = (size_t)1 << 20; s <= ARENA_OBJ_MAX; s += 65536 + 4093)
      one(s, align);
    around(ARENA_OBJ_MAX - 65536, align);
    around(ARENA_OBJ_MAX - 2, align);
  }
  if (g_bad != 0)
    return 1;
  printf("PASS regpool_request_classes: %lu requests, %lu classes, each "
         "decided by one queue\n",
         g_requests, g_classes);
  return 0;
}
