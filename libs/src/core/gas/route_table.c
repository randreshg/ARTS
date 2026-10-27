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

#include "arts/gas/route_table.h"

#include "arts/utils/stdatomic_compat.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "arts.h"
#include "arts/gas/guid.h"
#include "arts/ooo.h"           /* arts_ooo_drain / arts_ooo_redrive_all */
#include "arts/runtime_state.h" /* arts_node_info */
#include "arts/system/print.h"
#include "arts/system/schedfuzz.h"
#include "arts/utils/lockfree_lifo.h"
#include "arts/utils/malloc.h"
#include "arts/utils/shared.h"

#define HASH64(x, y) ((uint64_t)(x) * (y))

static inline uint64_t get_route_table_key(uint64_t x, unsigned int shift) {
  uint64_t hash = 14695981039346656037U;
  switch (shift) {
  case 10:
    hash *= 1021;
    /* fall through */
  case 11:
    hash *= 2039;
    /* fall through */
  case 12:
    hash *= 4093;
    /* fall through */
  case 13:
    hash *= 8191;
    /* fall through */
  case 14:
    hash *= 16381;
    /* fall through */
  case 15:
    hash *= 32749;
    /* fall through */
  case 16:
    hash *= 65521;
    /* fall through */
  case 17:
    hash *= 131071;
    /* fall through */
  case 18:
    hash *= 262139;
    /* fall through */
  case 19:
    hash *= 524287;
    /* fall through */
  case 20:
    hash *= 1048573;
    /* fall through */
  case 21:
    hash *= 2097143;
    /* fall through */
  case 22:
    hash *= 4194301;
    /* fall through */
  case 31:
    hash *= 2147483647;
    /* fall through */
  case 32:
    hash *= 4294967291;
    /* fall through */
  default:
    break;
  }

  return (HASH64(x, hash) >> (64 - shift)) * COLLISION_RESOLVES;
}

static inline arts_route_table_t *arts_get_route_table(arts_guid_t guid) {
  uint64_t key = ARTS_GUID_GET_KEY(guid);
  if (ARTS_GUID_GET_TYPE(guid) == ARTS_GUID_DB && arts_db_seq_budget) {
    /* DB keys are [szhint | seq]; the creating rank is arithmetic on seq
     * (slice width = arts_db_seq_budget), so the local/remote decision is a
     * pure function of the key — identical on every thread, install and
     * lookup alike.  Self-created keys (reserved-range members included:
     * both reserve paths claim from this rank's own slice) spread
     * chunk-granular over the local per-thread tables; foreign creators and
     * the never-minted seqs above the last slice sit outside the slice and
     * take the shared remote shards. */
    uint64_t seq = key & ARTS_GUID_DB_SEQ_MASK;
    if (seq / arts_db_seq_budget == arts_global_rank_id) {
      return arts_node_info.route_table[(seq >> ARTS_GUID_DB_CHUNK_BITS) %
                                        arts_guid_table_count];
    }
    return arts_node_info
        .remote_route_table[key & (ARTS_REMOTE_ROUTE_SHARDS - 1)];
  }
  /* A flat key minted on this rank lies in its threads' blocks; any other key
   * (another rank's, the never-minted rest of this rank's slice, or one below
   * the base, which wraps above the span) takes a remote shard and never
   * indexes past route_table[].  A span of 0 sends every key remote. */
  uint64_t off = key - arts_guid_flat_base;
  if (off < arts_guid_flat_span) {
    return arts_node_info.route_table[off / arts_guid_keys_per_thread];
  }
  return arts_node_info
      .remote_route_table[key & (ARTS_REMOTE_ROUTE_SHARDS - 1)];
}

/* ── Per-kind deleter dispatch (registration) ───────────────────────────────
 * Install wraps the object in a shared cb; the cb's deleter is chosen by GUID
 * kind so create call sites keep the (obj, key, ...) signature.  Each object
 * type REGISTERS its deleter here at startup (a constructor in db.c / edt.c /
 * sync/event.c).  route_table does NOT reference the per-type
 * deleter symbols by name — that would create a backward cross-object-library
 * link dependency (arts_gas → arts_memory/arts_compute) that breaks the CUDA
 * lib's separate-object-library structure.  Registration decouples it: each
 * deleter pointer is published into this table from the deleter's own TU. */
typedef void (*arts_deleter_fn_t)(void *);
static arts_deleter_fn_t g_deleter_by_kind[ARTS_GUID_LAST];

void arts_route_table_register_deleter(arts_guid_kind_t kind,
                                       void (*deleter)(void *)) {
  if ((unsigned int)kind < (unsigned int)ARTS_GUID_LAST) {
    g_deleter_by_kind[kind] = deleter;
  }
}

static inline void (*deleter_for_kind(arts_guid_kind_t k))(void *) {
  return ((unsigned int)k < (unsigned int)ARTS_GUID_LAST) ? g_deleter_by_kind[k]
                                                          : NULL;
}

void arts_route_table_delete_unpublished(arts_guid_t key, void *obj) {
  void (*deleter)(void *) = deleter_for_kind(arts_guid_get_kind(key));
  if (deleter != NULL) {
    deleter(obj);
  }
}

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
/* A leak checker scans the heap and registered roots, not an anonymous
 * mapping, and a slot may hold the only reference to its object. */
#include <sanitizer/lsan_interface.h>
#define ROUTE_TABLE_LEAK_ROOT(p, n) __lsan_register_root_region((p), (n))
#define ROUTE_TABLE_LEAK_UNROOT(p, n) __lsan_unregister_root_region((p), (n))
#else
#define ROUTE_TABLE_LEAK_ROOT(p, n) ((void)0)
#define ROUTE_TABLE_LEAK_UNROOT(p, n) ((void)0)
#endif

arts_route_item_t *arts_route_table_try_map_slots(uint64_t slots) {
  size_t bytes = (size_t)slots * sizeof(arts_route_item_t);
  void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) {
    return NULL;
  }
  ROUTE_TABLE_LEAK_ROOT(p, bytes);
  return (arts_route_item_t *)p;
}

void arts_route_table_unmap_slots(arts_route_item_t *data, uint64_t slots) {
  size_t bytes = (size_t)slots * sizeof(arts_route_item_t);
  ROUTE_TABLE_LEAK_UNROOT(data, bytes);
  munmap(data, bytes);
}

static void route_table_map_failed(uint64_t slots, int err) {
  ARTS_ERROR("route table: mapping a segment of %lu slots (%zu bytes) "
             "failed: %s; route_table_size (gpu_route_table_size for a GPU "
             "table) sets the first segment, each growth doubles it",
             (unsigned long)slots, (size_t)slots * sizeof(arts_route_item_t),
             strerror(err));
}

static arts_route_table_t *route_segment_build(unsigned int windows,
                                               unsigned int shift);
static void route_segment_discard(arts_route_table_t *t);

static const arts_route_segment_ops_t route_segment_cpu_ops = {
    .build = route_segment_build, .discard = route_segment_discard};

static arts_route_table_t *route_segment_build(unsigned int windows,
                                               unsigned int shift) {
  assert(windows == (uint64_t)1 << shift);
  arts_route_table_t *t =
      (arts_route_table_t *)arts_calloc(1, sizeof(arts_route_table_t));
  t->data =
      arts_route_table_try_map_slots((uint64_t)COLLISION_RESOLVES * windows);
  if (t->data == NULL) {
    int err = errno;
    arts_free(t);
    errno = err;
    return NULL;
  }
  t->size = windows;
  t->shift = shift;
  atomic_init(&t->next, NULL);
  t->ops = &route_segment_cpu_ops;
  return t;
}

static void route_segment_discard(arts_route_table_t *t) {
  arts_route_table_unmap_slots(t->data, (uint64_t)t->size * COLLISION_RESOLVES);
  arts_free(t);
}

arts_route_table_t *
arts_route_table_new_root(const arts_route_segment_ops_t *ops,
                          unsigned int windows, unsigned int shift) {
  arts_route_table_t *t = ops->build(windows, shift);
  if (t == NULL) {
    route_table_map_failed((uint64_t)COLLISION_RESOLVES * windows, errno);
  }
  return t;
}

arts_route_table_t *arts_new_route_table(unsigned int route_table_size,
                                         unsigned int shift) {
  return arts_route_table_new_root(&route_segment_cpu_ops, route_table_size,
                                   shift);
}

/* Bound on waiting for another thread's step.  The one step any route-table
 * wait waits out is a retire's RETIRING window, a few atomics with no
 * allocation, system call, lock or wait inside it, so anything near the bound
 * is a lost step. */
#define ROUTE_TABLE_WAIT_NS (5ull * 1000000000ull)

typedef struct {
  struct timespec t0;
  unsigned int spins;
} route_wait_t;

static void route_wait_tick(route_wait_t *w, arts_guid_t key, const char *what,
                            arts_guid_t seen) {
  if (w->spins++ == 0) {
    clock_gettime(CLOCK_MONOTONIC, &w->t0);
    return;
  }
  if ((w->spins & 1023u) != 0) {
    return;
  }
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  uint64_t ns = (uint64_t)(t.tv_sec - w->t0.tv_sec) * 1000000000ull +
                (uint64_t)t.tv_nsec - (uint64_t)w->t0.tv_nsec;
  if (ns > ROUTE_TABLE_WAIT_NS) {
    if (seen != 0) {
      ARTS_ERROR("GUID %lu: %s (slot key seen 0x%lx) for %.3f s",
                 (unsigned long)key, what, (unsigned long)seen,
                 (double)ns / 1e9);
    } else {
      ARTS_ERROR("GUID %lu: %s for %.3f s", (unsigned long)key, what,
                 (double)ns / 1e9);
    }
  }
  sched_yield();
}

/* The segment after `t`, or NULL at the end of the chain. */
static inline arts_route_table_t *
route_table_next(const arts_route_table_t *t) {
  return atomic_load_explicit(&t->next, memory_order_acquire);
}

#ifndef ROUTE_TABLE_BEFORE_LINK
/* Runs between a racer's build of the next segment and its link CAS. */
#define ROUTE_TABLE_BEFORE_LINK(key) ((void)(key), arts_sched_fuzz_point())
#endif

/* The segment after `t`, grown if the chain ends at `t`.  Every thread that
 * finds the end builds a segment of its own and CASes it in; the losers
 * discard theirs, which no other thread saw, and go on in the winner's.  No
 * racer waits on another's build. */
static arts_route_table_t *route_table_next_or_grow(arts_route_table_t *t,
                                                    arts_guid_t key) {
  arts_route_table_t *next =
      atomic_load_explicit(&t->next, memory_order_acquire);
  if (next != NULL) {
    return next;
  }
  if (t->size > UINT_MAX / 2) {
    ARTS_ERROR("GUID %lu: its probe window is full in every segment, and "
               "the last holds %u windows, the most a segment can; the "
               "route table cannot grow (route_table_size sets the first "
               "segment, each growth doubles it)",
               (unsigned long)key, t->size);
  }
  arts_route_table_t *mine = t->ops->build(2 * t->size, t->shift + 1);
  if (mine == NULL) {
    /* One re-read: a segment another racer published serves the caller;
     * otherwise the failed mapping ends the process, as a single builder's
     * would. */
    int err = errno;
    next = atomic_load_explicit(&t->next, memory_order_acquire);
    if (next != NULL) {
      return next;
    }
    route_table_map_failed((uint64_t)COLLISION_RESOLVES * 2 * t->size, err);
  }
  ROUTE_TABLE_BEFORE_LINK(key);
  if (atomic_compare_exchange_strong_explicit(
          &t->next, &next, mine, memory_order_acq_rel, memory_order_acquire)) {
    return mine;
  }
  mine->ops->discard(mine);
  return next;
}

/* Slot is empty when key == 0 (ARTS GUIDs never have key value 0).  A slot
 * keyed by a GUID publishes it until a retire returns the slot. */
arts_route_item_t *
arts_route_table_search_for_key(arts_route_table_t *route_table,
                                arts_guid_t key) {
  arts_route_table_t *current = route_table;
  uint64_t key_val;
  while (current) {
    key_val = get_route_table_key((uint64_t)key, current->shift);
    for (int i = 0; i < COLLISION_RESOLVES; i++) {
      arts_guid_t slot_key = atomic_load_explicit(&current->data[key_val].key,
                                                  memory_order_acquire);
      if (slot_key == key) {
        return &current->data[key_val];
      }
      key_val++;
    }
    current = route_table_next(current);
  }
  return NULL;
}

/* Linearly scan for an empty slot (key == 0) and atomically claim it for
 * `key` via CAS.  Concurrent reservers race here directly (no per-GUID
 * lock) -- the key-CAS is the single point of serialization.  Accepting a
 * slot whose key already equals `key` makes two concurrent reservers agree
 * on the same canonical slot (no orphan-slot leak on the common path). */
arts_route_item_t *
arts_route_table_search_for_empty(arts_route_table_t *route_table,
                                  arts_guid_t key) {
  arts_route_table_t *current = route_table;
  uint64_t key_val;
  while (current != NULL) {
    key_val = get_route_table_key((uint64_t)key, current->shift);
    for (int i = 0; i < COLLISION_RESOLVES; i++) {
      arts_guid_t expected = (arts_guid_t)0;
      if (atomic_compare_exchange_strong_explicit(
              &current->data[key_val].key, &expected, key, memory_order_acq_rel,
              memory_order_acquire)) {
        return &current->data[key_val];
      }
      if (expected == key) {
        return &current->data[key_val];
      }
      key_val++;
    }
    current = route_table_next_or_grow(current, key);
  }
  ARTS_ERROR("Route table search failed: impossible state (table=%p)",
             (void *)route_table);
}

/* Claim states of the key word, beside 0 (free), a GUID (published) and
 * ARTS_ROUTE_KEY_RETIRING.  Bits 63-60 carry a tag no GUID has (a GUID's kind
 * bits 63-62 are never 0), so none equals a GUID and no key search matches
 * one.  The predicates compare the whole tag: one bit of it alone can be set
 * in a GUID's rank field.
 *   BUSY      a claimer owns the slot and is filling claim_key;
 *   CLAIM(v)  claim v of the key in claim_key is live;
 *   LOST(v)   claim v lost and never publishes.
 * v numbers the slot's claims (claim_ver) and never recurs in a slot, so a
 * CAS that names v acts on that claim alone. */
#define ROUTE_KEY_BUSY ((arts_guid_t)2)
#define ROUTE_KEY_VERSION_MASK (((uint64_t)1 << 60) - 1)
#define ROUTE_KEY_CLAIM_TAG ((uint64_t)2 << 60)
#define ROUTE_KEY_LOST_TAG ((uint64_t)1 << 60)
#define ROUTE_KEY_CLAIM(v) ((arts_guid_t)(ROUTE_KEY_CLAIM_TAG | (uint64_t)(v)))
#define ROUTE_KEY_LOST(v) ((arts_guid_t)(ROUTE_KEY_LOST_TAG | (uint64_t)(v)))
#define ROUTE_KEY_IS_CLAIM(w)                                                  \
  (((uint64_t)(w) & ~ROUTE_KEY_VERSION_MASK) == ROUTE_KEY_CLAIM_TAG)
#define ROUTE_KEY_IS_LOST(w)                                                   \
  (((uint64_t)(w) & ~ROUTE_KEY_VERSION_MASK) == ROUTE_KEY_LOST_TAG)
#define ROUTE_KEY_VERSION(w) ((uint64_t)(w) & ROUTE_KEY_VERSION_MASK)

_Static_assert((ROUTE_KEY_CLAIM_TAG >> ARTS_GUID_TYPE_SHIFT) ==
                       ARTS_GUID_RESERVED &&
                   (ROUTE_KEY_LOST_TAG >> ARTS_GUID_TYPE_SHIFT) ==
                       ARTS_GUID_RESERVED,
               "a claim word carries kind ARTS_GUID_RESERVED");
_Static_assert(ROUTE_KEY_CLAIM_TAG != ROUTE_KEY_LOST_TAG,
               "a live claim and a lost one carry different tags");
_Static_assert(((uint64_t)ARTS_ROUTE_KEY_RETIRING & ~ROUTE_KEY_VERSION_MASK) ==
                       0 &&
                   ((uint64_t)ROUTE_KEY_BUSY & ~ROUTE_KEY_VERSION_MASK) == 0 &&
                   ROUTE_KEY_BUSY != ARTS_ROUTE_KEY_RETIRING,
               "RETIRING and BUSY match neither claim tag");

/* An RMW of a slot's key word, on the exact word it leaves. */
static inline bool route_key_cas(_Atomic(arts_guid_t) *word,
                                 arts_guid_t *expect, arts_guid_t desired) {
  return atomic_compare_exchange_strong_explicit(
      word, expect, desired, memory_order_acq_rel, memory_order_acquire);
}

#ifndef ROUTE_TABLE_AFTER_MISS
/* Runs between a reservation's missed search and its claim.  Empty in the
 * runtime; a whitebox test that compiles this file defines it to drive another
 * reserver's claim into exactly that window. */
#define ROUTE_TABLE_AFTER_MISS(key) ((void)(key))
#endif

#ifndef ROUTE_TABLE_AFTER_CLAIM
/* Runs between a claim's fence and the scan that decides it. */
#define ROUTE_TABLE_AFTER_CLAIM(key) ((void)(key), arts_sched_fuzz_point())
#endif

#ifndef ROUTE_TABLE_BEFORE_HELP
/* Runs between a scanner's read of an earlier live claim and its help. */
#define ROUTE_TABLE_BEFORE_HELP(key) ((void)(key), arts_sched_fuzz_point())
#endif

/* Claim the first slot of `key`'s probe sequence whose word is 0 or LOST(u),
 * taking it to BUSY, and grow the chain when the sequence has none.  A word
 * is read before it is CASed, so an occupied slot costs a load, not an RMW.
 * NULL, having claimed nothing, when the sequence already publishes `key`. */
static arts_route_item_t *route_table_claim(arts_route_table_t *table,
                                            arts_guid_t key) {
  for (arts_route_table_t *t = table;; t = route_table_next_or_grow(t, key)) {
    uint64_t pos = get_route_table_key((uint64_t)key, t->shift);
    for (int i = 0; i < COLLISION_RESOLVES; i++, pos++) {
      arts_route_item_t *slot = &t->data[pos];
      arts_guid_t w = atomic_load_explicit(&slot->key, memory_order_acquire);
      while (w == 0 || ROUTE_KEY_IS_LOST(w)) {
        if (route_key_cas(&slot->key, &w, ROUTE_KEY_BUSY)) {
          return slot;
        }
      }
      if (w == key) {
        return NULL;
      }
    }
  }
}

/* Publish `key` at `target`, whose claim is CLAIM(tv), or find where it is
 * published.  Probe order is one global order (segment, then position, since
 * windows are aligned blocks): the earliest live claim of a key wins and is
 * helped by every later one, and a later live claim is killed.  A RETIRING
 * slot is waited out, since its hand-back re-publishes the key there.
 * Returns the slot that publishes `key`, or NULL when the claim helped last
 * died (the caller reserves again). */
static arts_route_item_t *route_table_assist(arts_route_table_t *table,
                                             arts_guid_t key,
                                             arts_route_item_t *target,
                                             uint64_t tv) {
restart:;
  bool passed = false;
  for (arts_route_table_t *t = table; t != NULL; t = route_table_next(t)) {
    uint64_t pos = get_route_table_key((uint64_t)key, t->shift);
    for (int i = 0; i < COLLISION_RESOLVES; i++, pos++) {
      arts_route_item_t *slot = &t->data[pos];
      if (slot == target) {
        passed = true;
        continue;
      }
      route_wait_t w = {0};
      for (;;) {
        arts_guid_t k = atomic_load_explicit(&slot->key, memory_order_acquire);
        if (k == key) {
          /* Published here: the target loses, unless it was published
           * itself after this slot's publication ended. */
          arts_guid_t e = ROUTE_KEY_CLAIM(tv);
          if (!route_key_cas(&target->key, &e, ROUTE_KEY_LOST(tv)) &&
              e == key) {
            return target;
          }
          return slot;
        }
        if (k == ARTS_ROUTE_KEY_RETIRING) {
          route_wait_tick(&w, key,
                          "a retiring slot of its window never settled", k);
          continue;
        }
        if (!ROUTE_KEY_IS_CLAIM(k) ||
            atomic_load_explicit(&slot->claim_key, memory_order_relaxed) !=
                key) {
          break; /* free, BUSY, LOST, or another key's */
        }
        uint64_t v = ROUTE_KEY_VERSION(k);
        if (!passed) {
          /* An earlier live claim: the target yields to it, and this thread
           * helps it to publication instead of waiting for it. */
          arts_guid_t e = ROUTE_KEY_CLAIM(tv);
          if (!route_key_cas(&target->key, &e, ROUTE_KEY_LOST(tv)) &&
              e == key) {
            return target;
          }
          ROUTE_TABLE_BEFORE_HELP(key);
          target = slot;
          tv = v;
          /* The CLAIM just read against the scan of whichever reservation
           * publishes an earlier claim of the key: this fence pairs with
           * that reservation's own fence before its scan (the claim fence in
           * arts_route_table_reserve_or_lookup, or this one in its helper),
           * so of the two scans at least one sees the other's claim. */
          atomic_thread_fence(memory_order_seq_cst);
          goto restart;
        }
        if (atomic_load_explicit(&target->key, memory_order_acquire) !=
            ROUTE_KEY_CLAIM(tv)) {
          goto settle; /* the target is decided already */
        }
        /* A later live claim loses; the slot is examined again. */
        arts_guid_t e = k;
        (void)route_key_cas(&slot->key, &e, ROUTE_KEY_LOST(v));
      }
    }
  }
settle:;
  arts_guid_t e = ROUTE_KEY_CLAIM(tv);
  if (route_key_cas(&target->key, &e, key) || e == key) {
    return target;
  }
  return NULL;
}

/* Reserve a slot for `key` (or look it up if already present).  No
 * reservation waits on another; the one wait is a retire's window.  On
 * return, *out is the one slot that publishes `key`.  The cb (value) may be
 * NULL.
 *
 * Invariant: at most one slot publishes a key.  Slots are returned on destroy,
 * so the first free slot of a probe sequence is not stable, and two reservers
 * that each missed the key can claim two different slots.  A claim is made
 * under a versioned CLAIM word that no search matches, and it is published
 * only after a fence and a scan of the whole probe sequence
 * (route_table_assist).  The earliest live claim of a key in probe order
 * wins; a later one is killed; a reserver that finds an earlier one helps it
 * to publication rather than wait for it.  Of two claimers at least one sees
 * the other (the fences), and every CAS names the claim's version, so no
 * decision acts on a claim other than the one it read.  A RETIRING slot of the
 * sequence is waited out, since its hand-back re-publishes the key there. */
void arts_route_table_reserve_or_lookup(arts_guid_t key,
                                        arts_route_item_t **out) {
  arts_route_table_t *table = arts_get_route_table(key);
  for (;;) {
    arts_route_item_t *item = arts_route_table_search_for_key(table, key);
    if (item != NULL) {
      *out = item;
      return;
    }
    ROUTE_TABLE_AFTER_MISS(key);
    arts_route_item_t *mine = route_table_claim(table, key);
    if (mine == NULL) {
      continue;
    }
    uint64_t v = ++mine->claim_ver;
    assert(v <= ROUTE_KEY_VERSION_MASK);
    atomic_store_explicit(&mine->claim_key, key, memory_order_relaxed);
    atomic_store_explicit(&mine->key, ROUTE_KEY_CLAIM(v), memory_order_release);
    /* This CLAIM store against the scan of every other reservation of the
     * key: pairs with the same fence in a concurrent
     * arts_route_table_reserve_or_lookup, or with the helper's fence in
     * route_table_assist, each before its slot and link loads. */
    atomic_thread_fence(memory_order_seq_cst);
    ROUTE_TABLE_AFTER_CLAIM(key);
    arts_route_item_t *at = route_table_assist(table, key, mine, v);
    if (at != mine) {
      /* Free only claim v's own LOST word.  A failure means a helper
       * published claim v and the word now belongs to that publication's
       * life, or a claimer took the LOST word: either way it is not this
       * thread's.  Weaker than the key word's acq_rel RMWs: the release
       * orders claim_ver for the slot's next claimer, and a failure writes
       * nothing and reads nothing this thread uses. */
      arts_guid_t e = ROUTE_KEY_LOST(v);
      (void)atomic_compare_exchange_strong_explicit(
          &mine->key, &e, (arts_guid_t)0, memory_order_release,
          memory_order_relaxed);
    }
    if (at != NULL) {
      *out = at;
      return;
    }
  }
}

#ifndef ROUTE_TABLE_AFTER_FRESH
/* Runs between a fresh publication and the return to its create. */
#define ROUTE_TABLE_AFTER_FRESH(key) ((void)(key), arts_sched_fuzz_point())
#endif

#ifndef NDEBUG
/* A fresh GUID is named by nothing else, so no other slot of its probe
 * sequence publishes it or holds a live claim of it.  Best effort: it adds
 * no fence, so a general claim of the GUID racing the fresh CAS can go unseen
 * by both sides. */
static void route_table_fresh_check(arts_route_table_t *table, arts_guid_t key,
                                    const arts_route_item_t *mine) {
  for (arts_route_table_t *t = table; t != NULL; t = route_table_next(t)) {
    uint64_t pos = get_route_table_key((uint64_t)key, t->shift);
    for (int i = 0; i < COLLISION_RESOLVES; i++, pos++) {
      arts_route_item_t *slot = &t->data[pos];
      if (slot == mine) {
        continue;
      }
      arts_guid_t w = atomic_load_explicit(&slot->key, memory_order_acquire);
      if (w == key || (ROUTE_KEY_IS_CLAIM(w) &&
                       atomic_load_explicit(&slot->claim_key,
                                            memory_order_relaxed) == key)) {
        ARTS_ERROR("GUID %lu took the fresh path but another reservation "
                   "holds it",
                   (unsigned long)key);
      }
    }
  }
}
#endif

void arts_route_table_reserve_fresh(arts_guid_t key, arts_route_item_t **out) {
  arts_route_table_t *table = arts_get_route_table(key);
  for (arts_route_table_t *t = table;; t = route_table_next_or_grow(t, key)) {
    uint64_t pos = get_route_table_key((uint64_t)key, t->shift);
    for (int i = 0; i < COLLISION_RESOLVES; i++, pos++) {
      arts_route_item_t *slot = &t->data[pos];
      arts_guid_t w = atomic_load_explicit(&slot->key, memory_order_acquire);
      while (w == 0 || ROUTE_KEY_IS_LOST(w)) {
        if (route_key_cas(&slot->key, &w, key)) {
#ifndef NDEBUG
          route_table_fresh_check(table, key, slot);
#endif
          ROUTE_TABLE_AFTER_FRESH(key);
          *out = slot;
          return;
        }
      }
      if (w == key) {
#ifndef NDEBUG
        ARTS_ERROR("GUID %lu took the fresh path but is already published",
                   (unsigned long)key);
#endif
        *out = slot;
        return;
      }
    }
  }
}

/* ── cb-based lifecycle ─────────────────────────────────────────────────── */

/* Unbracketed on purpose: this takes a slot the CALLER located, in a mirror
 * table (the GPU per-device tables) that no destroy returns.  Its slots are
 * bound to their key for the table's life, so there is no identity to lose.  Do
 * not point it at the global table, whose slots are reclaimed. */
arts_shared_ptr_t arts_route_item_acquire(arts_route_item_t *item) {
  return item ? arts_shared_slot_load(&item->value) : NULL;
}

bool arts_route_item_install_data(arts_route_item_t *item, void *obj,
                                  void (*deleter)(void *)) {
  arts_shared_ptr_t cur = arts_shared_slot_load(&item->value);
  if (cur) {
    arts_shared_release(&cur);
    return false;
  }
  arts_shared_ptr_t cb = arts_shared_make(obj, deleter);
  if (arts_shared_slot_compare_exchange(&item->value, NULL, cb)) {
    return true;
  }
  /* Lost the install race — abandon our cb (object stays the caller's). */
  arts_shared_abandon(&cb);
  return false;
}

/* Race install: returns a caller-owned handle only if this caller CAS'd its cb
 * into the slot, NULL otherwise.
 *
 * The slot's identity and its emptiness are decided by one atom.  The empty
 * value word is snapshotted first, the key is checked after it, and the
 * install is a CAS on that exact snapshot.  A teardown retires the key before
 * it exchanges the value, and that exchange moves the word, so a resolver that
 * saw the slot as the key's before a retire can no longer install into it:
 * its CAS fails and it resolves the key again.  An object therefore only ever
 * sits in a slot that named its key when it was installed. */
arts_shared_ptr_t arts_route_table_install_if_absent(void *obj,
                                                     arts_guid_t key) {
  arts_shared_ptr_t cb = arts_route_table_make_handle(obj, key);
  /* The caller's ref, taken before the cb can be published and retired. */
  arts_shared_ptr_t mine = arts_shared_copy(cb);
  if (arts_route_table_install_handle_if_absent(cb, key)) {
    return mine;
  }
  /* Occupied under this key: another install won.  The object stays the
   * caller's. */
  arts_shared_release(&mine);
  arts_shared_abandon(&cb);
  return NULL;
}

arts_shared_ptr_t arts_route_table_make_handle(void *obj, arts_guid_t key) {
  arts_shared_ptr_t cb =
      arts_shared_make(obj, deleter_for_kind(arts_guid_get_kind(key)));
  arts_shared_set_tag(cb, (uint64_t)key);
  return cb;
}

bool arts_route_table_install_handle_if_absent(arts_shared_ptr_t cb,
                                               arts_guid_t key) {
  for (;;) {
    arts_route_item_t *item;
    arts_route_table_reserve_or_lookup(key, &item);
    uint64_t ext = 0;
    bool empty = arts_shared_slot_peek_empty(&item->value, &ext);
    if (atomic_load_explicit(&item->key, memory_order_acquire) != key) {
      continue;
    }
    if (!empty) {
      return false;
    }
    if (arts_shared_slot_install_empty(&item->value, ext, cb)) {
      /* Same-word pairing on ooo_list with a parker's push and re-read in
       * arts_ooo_dispatch_or_defer: the drain's exchange is acq_rel and
       * unconditional, so either it takes the node or the parker's push
       * reads from it or a later RMW and its re-read sees the install or a
       * later write. */
      arts_ooo_drain(item);
      return true;
    }
  }
}

bool arts_route_table_install_handle_fresh(arts_shared_ptr_t cb,
                                           arts_guid_t key) {
  /* For a GUID that is fresh, no object of it exists to install or retire,
   * so the slot stays the key's and its empty value word changes only by
   * this install.  The key is still checked after the snapshot, as every
   * install does, so a GUID that was not fresh never lands in a slot that no
   * longer names it. */
  arts_route_item_t *item;
  arts_route_table_reserve_fresh(key, &item);
  uint64_t ext = 0;
  if (!arts_shared_slot_peek_empty(&item->value, &ext) ||
      atomic_load_explicit(&item->key, memory_order_acquire) != key ||
      !arts_shared_slot_install_empty(&item->value, ext, cb)) {
    return false;
  }
  /* A returned slot can still carry a node a previous occupant's parker
   * pushed; the drain's unconditional exchange sends it on by its own GUID,
   * as install_handle_if_absent's does. */
  arts_ooo_drain(item);
  return true;
}

#ifndef ROUTE_TABLE_BEFORE_RETIRE_CLAIM
/* Runs in an identity retire between its holds() peek and its key claim, and
 * after a won claim, before the value exchange.  Empty in the runtime; a
 * whitebox test defines them to act inside those windows. */
#define ROUTE_TABLE_BEFORE_RETIRE_CLAIM(key) ((void)(key))
#endif
#ifndef ROUTE_TABLE_AFTER_RETIRE_CLAIM
#define ROUTE_TABLE_AFTER_RETIRE_CLAIM(key) ((void)(key))
#endif

/* The object `key` publishes, pinned, looking through a retire in flight: a
 * slot of the key's window that is RETIRING hides the key from a search, and
 * if that retire hands the key back, the object it hid is still the key's.
 * The value is taken only while the slot stays RETIRING across the load, so it
 * was in the slot during that retire: either the object being retired (the
 * dispatched-item form then sees its tag cleared) or a hand-back's occupant. */
static arts_shared_ptr_t route_table_pin_published(arts_guid_t key) {
  arts_shared_ptr_t h = arts_route_table_lookup(key);
  if (h != NULL) {
    return h;
  }
  for (arts_route_table_t *t = arts_get_route_table(key); t != NULL;
       t = route_table_next(t)) {
    uint64_t pos = get_route_table_key((uint64_t)key, t->shift);
    for (int i = 0; i < COLLISION_RESOLVES; i++, pos++) {
      arts_route_item_t *slot = &t->data[pos];
      if (atomic_load_explicit(&slot->key, memory_order_acquire) !=
          ARTS_ROUTE_KEY_RETIRING) {
        continue;
      }
      h = arts_shared_slot_load(&slot->value);
      if (h != NULL && arts_shared_tag(h) == (uint64_t)key &&
          atomic_load_explicit(&slot->key, memory_order_acquire) ==
              ARTS_ROUTE_KEY_RETIRING) {
        return h;
      }
      arts_shared_release(&h);
    }
  }
  return NULL;
}

bool arts_route_table_set_destroyed_if(arts_guid_t key, arts_shared_ptr_t cb) {
  arts_route_table_t *route_table = arts_get_route_table(key);
  arts_route_item_t *item = arts_route_table_search_for_key(route_table, key);
  if (item == NULL || cb == NULL) {
    return false;
  }
  /* The caller's ref keeps `cb` from being recycled, so these compares are
   * identity. */
  if (!arts_shared_slot_holds(&item->value, cb)) {
    return false;
  }
  ROUTE_TABLE_BEFORE_RETIRE_CLAIM(key);
  /* From this claim of RETIRING to the store that leaves it, reservers and
   * other retires of the slot wait this window out (route_wait_tick).  It is
   * tolerated only because it is a few atomics: nothing inside it may
   * allocate, make a system call, take a lock or wait for another thread. */
  arts_guid_t expect = key;
  if (!atomic_compare_exchange_strong_explicit(
          &item->key, &expect, ARTS_ROUTE_KEY_RETIRING, memory_order_acq_rel,
          memory_order_acquire)) {
    return false;
  }
  ROUTE_TABLE_AFTER_RETIRE_CLAIM(key);
  if (!arts_shared_slot_compare_exchange(&item->value, cb, NULL)) {
    /* Between the peek and the claim another retire took `cb` and the key's
     * next generation was installed in this slot.  Nothing was taken: the
     * slot stays the key's.  Reservers wait out RETIRING, so none of them
     * publishes a second slot meanwhile, and a retire of the next generation
     * that arrives meanwhile finds it through route_table_pin_published. */
    atomic_store_explicit(&item->key, key, memory_order_release);
    return false;
  }
  arts_shared_set_tag(cb, 0);
  atomic_store_explicit(&item->key, (arts_guid_t)0, memory_order_release);
  /* Same-word pairing on ooo_list with a parker's push and re-read in
   * arts_ooo_dispatch_or_defer and ooo_dispatch_create: the re-drive's
   * exchange is acq_rel and unconditional, so either it takes the node or
   * the parker's push reads from it or a later RMW and its re-read sees the
   * slot emptied and returned, or a later write. */
  arts_ooo_redrive_all(item);
  return true;
}

bool arts_route_table_set_destroyed_item(arts_guid_t key,
                                         arts_shared_ptr_t cb) {
  if (cb == NULL) {
    return false;
  }
  route_wait_t w = {0};
  for (;;) {
    /* A retire clears the tag: this object no longer answers for `key`. */
    if (arts_shared_tag(cb) != (uint64_t)key) {
      return false;
    }
    if (arts_route_table_set_destroyed_if(key, cb)) {
      return true;
    }
    /* Still tagged `key`, so still installed in the key's one slot: a
     * concurrent retire holds that slot, or has taken `cb` and not yet
     * cleared its tag. */
    arts_route_item_t *slot =
        arts_route_table_search_for_key(arts_get_route_table(key), key);
    const char *why =
        slot == NULL ? "retire: no slot publishes the GUID"
        : arts_shared_slot_holds(&slot->value, cb)
            ? "retire: its slot holds the object"
        : arts_shared_slot_empty(&slot->value)
            ? "retire: its slot is empty"
            : "retire: its slot holds another object";
    arts_guid_t seen =
        slot == NULL ? (arts_guid_t)0
                     : atomic_load_explicit(&slot->key, memory_order_acquire);
    route_wait_tick(&w, key, why, seen);
  }
}

bool arts_route_table_set_destroyed_object(arts_guid_t key, const void *obj) {
  arts_shared_ptr_t h = route_table_pin_published(key);
  if (h == NULL) {
    return false;
  }
  bool retired =
      arts_shared_get(h) == obj && arts_route_table_set_destroyed_item(key, h);
  arts_shared_release(&h);
  return retired;
}

/* Acquire a slot's object, verified by the OBJECT's identity.
 *
 * search_for_key proves the slot held `key` BEFORE the value load; a slot is
 * returned on destroy and re-claimed by another GUID, so that alone no longer
 * proves the value we loaded is the one we asked for — the object could belong
 * to a different GUID, and because DB, EVENT and EDT keys share these tables it
 * could be a different KIND, which a caller would then use through the wrong
 * struct.  Re-reading the slot's key word is not a proof either: the word is
 * not monotone (claimed, retired, returned, re-claimed), and a late message
 * for a destroyed GUID legally re-reserves that GUID into a freed slot — so
 * the word can return to a previously observed key while the value belongs to
 * an identity that held the slot in between.  The pinned cb's own tag cannot:
 * it is stamped with the key the object was published under and the ref taken
 * by the load keeps that cb from being recycled, so tag == key is a property
 * of the object in hand, not of a word that may have changed twice since. */
static inline arts_shared_ptr_t
route_item_acquire_checked(arts_route_item_t *item, arts_guid_t key) {
  arts_shared_ptr_t h = arts_shared_slot_load(&item->value);
  if (h != NULL && arts_shared_tag(h) != (uint64_t)key) {
    arts_shared_release(&h);
    return NULL;
  }
  return h;
}

/* ── Typed handle lookups (caller-owned ref) ────────────────────────────── */

static inline arts_shared_ptr_t
arts_route_table_lookup_typed(arts_guid_t guid, arts_guid_kind_t expected) {
  if (arts_guid_get_kind(guid) != expected) {
    return NULL;
  }
  arts_route_table_t *route_table = arts_get_route_table(guid);
  arts_route_item_t *item = arts_route_table_search_for_key(route_table, guid);
  if (item == NULL) {
    return NULL;
  }
  return route_item_acquire_checked(item, guid);
}

arts_shared_ptr_t arts_route_table_lookup(arts_guid_t key) {
  arts_route_table_t *route_table = arts_get_route_table(key);
  arts_route_item_t *item = arts_route_table_search_for_key(route_table, key);
  if (item == NULL) {
    return NULL;
  }
  return route_item_acquire_checked(item, key);
}

arts_shared_ptr_t arts_route_table_lookup_event(arts_guid_t guid) {
  return arts_route_table_lookup_typed(guid, ARTS_GUID_EVENT);
}

arts_shared_ptr_t arts_route_table_lookup_db(arts_guid_t guid) {
  return arts_route_table_lookup_typed(guid, ARTS_GUID_DB);
}

arts_shared_ptr_t arts_route_table_lookup_edt(arts_guid_t guid) {
  return arts_route_table_lookup_typed(guid, ARTS_GUID_EDT);
}

void arts_reset_route_table_iterator(arts_route_table_iterator_t *iter,
                                     arts_route_table_t *table) {
  iter->table = table;
  iter->index = 0;
}

arts_route_item_t *arts_route_table_iterate(arts_route_table_iterator_t *iter) {
  arts_route_table_t *current = iter->table;
  while (current != NULL) {
    for (uint64_t i = iter->index;
         i < (uint64_t)current->size * COLLISION_RESOLVES; i++) {
      arts_guid_t slot_key =
          atomic_load_explicit(&current->data[i].key, memory_order_acquire);
      if (slot_key != 0) {
        iter->index = i + 1;
        iter->table = current;
        return &current->data[i];
      }
    }
    iter->index = 0;
    current = route_table_next(current);
  }
  return NULL;
}

uint64_t arts_clean_up_route_table(arts_route_table_t *route_table,
                                   bool (*keep)(arts_shared_ptr_t)) {
  arts_route_table_iterator_t iter;
  arts_reset_route_table_iterator(&iter, route_table);

  uint64_t kept = 0;
  arts_route_item_t *item = arts_route_table_iterate(&iter);
  while (item) {
    bool stays = false;
    if (keep != NULL) {
      arts_shared_ptr_t h = arts_shared_slot_load(&item->value);
      stays = h != NULL && keep(h);
      arts_shared_release(&h);
    }
    if (stays) {
      kept++;
    } else {
      /* Detach + release the install ref; the cb's per-kind deleter performs
       * the single teardown once the last ref drops.  No per-type special
       * casing — the deleter is the sole owner. */
      arts_shared_ptr_t old = arts_shared_slot_exchange(&item->value, NULL);
      if (old) {
        arts_shared_release(&old);
      }
    }
    arts_ooo_free_all(item);
    item = arts_route_table_iterate(&iter);
  }
  return kept;
}

unsigned int arts_route_table_parked_count(arts_route_table_t *route_table) {
  unsigned int n = 0;
  for (arts_route_table_t *t = route_table; t != NULL;
       t = route_table_next(t)) {
    for (uint64_t i = 0; i < (uint64_t)t->size * COLLISION_RESOLVES; i++) {
      /* Relaxed: read after every thread that pushes or drains has joined. */
      arts_lf_link_t *l =
          atomic_load_explicit(&t->data[i].ooo_list.head, memory_order_relaxed);
      for (; l != NULL;
           l = atomic_load_explicit(&l->next, memory_order_relaxed)) {
        n++;
      }
    }
  }
  return n;
}

void arts_delete_route_table(arts_route_table_t *route_table) {
  while (route_table != NULL) {
    arts_route_table_t *next = route_table_next(route_table);
    uint64_t slots = (uint64_t)route_table->size * COLLISION_RESOLVES;
    for (uint64_t i = 0; i < slots; i++) {
      /* Read before freeing: a write to every slot would populate every page
       * the run never touched. */
      if (!arts_lf_stack_empty(&route_table->data[i].ooo_list)) {
        arts_ooo_free_all(&route_table->data[i]);
      }
    }
    route_table->ops->discard(route_table);
    route_table = next;
  }
}

uint64_t arts_clean_up_dbs(bool (*keep)(arts_shared_ptr_t)) {
  uint64_t kept = 0;
  for (unsigned int i = 0; i < arts_node_info.total_thread_count; i++) {
    kept += arts_clean_up_route_table(arts_node_info.route_table[i], keep);
  }
  for (int s = 0; s < ARTS_REMOTE_ROUTE_SHARDS; s++) {
    kept +=
        arts_clean_up_route_table(arts_node_info.remote_route_table[s], keep);
  }
  return kept;
}
