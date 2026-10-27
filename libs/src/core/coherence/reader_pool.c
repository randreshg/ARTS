/* SPDX-License-Identifier: Apache-2.0
 *
 * The parked-reader pool of the arms that chain a cache's parked readers in
 * its request word (VAL, INV).
 *
 * Alloc pops the tagged free list, or takes a fresh index and installs the
 * chunk that holds it; a free pushes one node; a served chain goes back whole
 * with one push.  Who writes and reads a node's fields, and under which CAS,
 * is stated at struct arts_db_reader_node_s.
 */
#include "arts/coherence/types.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "arts/coherence/coherence.h"
#include "arts/system/print.h" /* ARTS_ERROR */
#include "arts/utils/malloc.h"
#include "arts/utils/stdatomic_compat.h"

static_assert(ARTS_DB_READER_IDX_MAX < (1ULL << ARTS_DB_READER_IDX_BITS),
              "the indices the chunks cover must fit the head field");

#define FREE_IDX(h) ((uint32_t)((h) & 0xffffffffULL))
#define FREE_TAG(h) ((uint32_t)((h) >> 32))
#define FREE_MAKE(tag, idx) ((((uint64_t)(tag)) << 32) | (uint64_t)(idx))

struct reader_dir_s {
  _Atomic(struct arts_db_reader_node_s *) chunk[ARTS_DB_READER_NCHUNKS];
};

/* Index i >= 1 lives in chunk k at offset o, where i + 255 = 2^(k+8) + o. */
static inline uint32_t reader_chunk_of(uint32_t idx, uint32_t *off) {
  uint32_t j = idx + 255u;
  uint32_t k = (uint32_t)(31 - __builtin_clz(j)) - 8u;
  *off = j - (1u << (k + 8u));
  return k;
}

void arts_db_reader_pool_init(struct arts_db_reader_pool_s *p) {
  atomic_init(&p->chunks, (uintptr_t)0);
  atomic_init(&p->next_fresh, 1u);
  atomic_init(&p->free_head, (uint64_t)0);
}

/* An index's allocator installed its chunk or acquired the install, and
 * every other thread reaches the index after an acquire that follows that
 * allocator: the chain grab's acquire of the park, or the pop's acquire of
 * the push. */
struct arts_db_reader_node_s *
arts_db_reader_node(struct arts_db_reader_pool_s *p, uint32_t idx) {
  assert(idx != 0u && idx <= ARTS_DB_READER_IDX_MAX);
  uint32_t off;
  uint32_t k = reader_chunk_of(idx, &off);
  struct reader_dir_s *dir = (struct reader_dir_s *)atomic_load_explicit(
      &p->chunks, memory_order_acquire);
  struct arts_db_reader_node_s *chunk =
      atomic_load_explicit(&dir->chunk[k], memory_order_acquire);
  return &chunk[off];
}

static struct reader_dir_s *reader_dir_install(struct arts_db_reader_pool_s *p) {
  uintptr_t d = atomic_load_explicit(&p->chunks, memory_order_acquire);
  if (d != 0) {
    return (struct reader_dir_s *)d;
  }
  struct reader_dir_s *fresh =
      (struct reader_dir_s *)arts_calloc(1, sizeof(*fresh));
  if (atomic_compare_exchange_strong_explicit(&p->chunks, &d, (uintptr_t)fresh,
                                              memory_order_acq_rel,
                                              memory_order_acquire)) {
    return fresh;
  }
  arts_free(fresh);
  return (struct reader_dir_s *)d;
}

uint32_t arts_db_reader_alloc(struct arts_db_reader_pool_s *p) {
  uint64_t h = atomic_load_explicit(&p->free_head, memory_order_acquire);
  while (FREE_IDX(h) != 0u) {
    uint32_t idx = FREE_IDX(h);
    uint32_t next = atomic_load_explicit(&arts_db_reader_node(p, idx)->next,
                                         memory_order_relaxed);
    if (atomic_compare_exchange_weak_explicit(
            &p->free_head, &h, FREE_MAKE(FREE_TAG(h) + 1u, next),
            memory_order_acq_rel, memory_order_acquire)) {
      return idx;
    }
  }
  uint32_t idx =
      atomic_fetch_add_explicit(&p->next_fresh, 1u, memory_order_relaxed);
  if (idx > ARTS_DB_READER_IDX_MAX) {
    ARTS_ERROR("parked-reader pool exhausted (index space)");
  }
  uint32_t off;
  uint32_t k = reader_chunk_of(idx, &off);
  struct reader_dir_s *dir = reader_dir_install(p);
  _Atomic(struct arts_db_reader_node_s *) *slotp = &dir->chunk[k];
  struct arts_db_reader_node_s *chunk =
      atomic_load_explicit(slotp, memory_order_acquire);
  if (chunk == NULL) {
    struct arts_db_reader_node_s *fresh = (struct arts_db_reader_node_s *)
        arts_calloc((size_t)1 << (k + 8u), sizeof(*fresh));
    if (!atomic_compare_exchange_strong_explicit(slotp, &chunk, fresh,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
      arts_free(fresh);
    }
  }
  return idx;
}

void arts_db_reader_free_chain(struct arts_db_reader_pool_s *p, uint32_t head,
                               uint32_t tail) {
  struct arts_db_reader_node_s *t = arts_db_reader_node(p, tail);
  /* Nothing is read through the old head: its index only becomes the tail's
   * link, and the CAS re-validates it. */
  uint64_t h = atomic_load_explicit(&p->free_head, memory_order_relaxed);
  for (;;) {
    atomic_store_explicit(&t->next, FREE_IDX(h), memory_order_relaxed);
    if (atomic_compare_exchange_weak_explicit(
            &p->free_head, &h, FREE_MAKE(FREE_TAG(h) + 1u, head),
            memory_order_acq_rel, memory_order_relaxed)) {
      return;
    }
  }
}

void arts_db_reader_free(struct arts_db_reader_pool_s *p, uint32_t idx) {
  arts_db_reader_free_chain(p, idx, idx);
}

/* The chain stays private to this server until its push, so a wake that
 * parks a reader on the same cache takes another node. */
void arts_db_reader_serve_chain(struct arts_db_reader_pool_s *p,
                                uint32_t head) {
  if (head == 0u) {
    return;
  }
  uint32_t tail = head;
  uint32_t i = head;
  do {
    struct arts_db_reader_node_s *r = arts_db_reader_node(p, i);
    tail = i;
    i = atomic_load_explicit(&r->next, memory_order_relaxed);
    mark_edt_ready_by_guid(r->edt_guid, r->slot);
  } while (i != 0u);
  arts_db_reader_free_chain(p, head, tail);
}

/* The cache's last reference is gone, and its edge orders every pool access
 * before this one: relaxed. */
void arts_db_reader_pool_destroy(struct arts_db_reader_pool_s *p) {
  uintptr_t d = atomic_load_explicit(&p->chunks, memory_order_relaxed);
  if (d == 0) {
    return;
  }
  struct reader_dir_s *dir = (struct reader_dir_s *)d;
  for (uint32_t k = 0; k < ARTS_DB_READER_NCHUNKS; k++) {
    arts_free(atomic_load_explicit(&dir->chunk[k], memory_order_relaxed));
  }
  arts_free(dir);
  atomic_store_explicit(&p->chunks, (uintptr_t)0, memory_order_relaxed);
}
