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
#include "arts/gas/guid.h"

#include "arts.h"
#include "arts/runtime_state.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/utils/malloc.h"
#include "arts/utils/stdatomic_compat.h"

/* Written once by arts_guid_node_geometry_init, before the runtime threads
 * are created; thread creation publishes them. */
uint64_t num_tables = 0;
uint64_t keys_per_thread = 0;
uint64_t arts_guid_flat_base = 0;
uint64_t arts_guid_flat_span = 0;

/* ── DB-kind seq allocator ──────────────────────────────────────────────────
 * DB keys are [szhint:10 | seq:38] (see guid.h).  seq is unique per
 * (home rank, creating rank) with zero communication: every home's seq space
 * is statically sliced by creating rank (arts_db_seq_budget wide), and within
 * this rank's slice a SHARED per-home counter hands out chunks that threads
 * consume through a private cursor — the fast path is a plain thread-local
 * increment, the shared counter is touched once per chunk.  The seqs above
 * the last slice are never minted and classify as "no creator" on every
 * rank. */
#define ARTS_GUID_DB_CHUNK ((uint64_t)1 << ARTS_GUID_DB_CHUNK_BITS)

uint64_t arts_db_seq_budget = 0;
static uint64_t db_seq_creator_base = 0;
/* [nrank] per-home counters, shared by this rank's minters.  The array is
 * allocated by arts_guid_node_geometry_init before the runtime threads are
 * created, and thread creation publishes the pointer.  Each counter is an ID
 * counter: its accesses carry only its own value, since disjointness comes
 * from its RMWs alone. */
static _Atomic(uint64_t) *db_seq_next = NULL;

struct db_seq_cursor_s {
  uint64_t cur;
  uint64_t end;
};
static ARTS_THREAD_LOCAL struct db_seq_cursor_s *t_db_cursor = NULL;
/* Every thread parks its cursor array here (own slot, written once at lazy
 * alloc) so global cleanup can free what thread-local storage would strand. */
static struct db_seq_cursor_s **db_cursor_registry = NULL;

/* Claim `count` consecutive seqs on `home`'s counter.  One atom is both the
 * count and the decision: concurrent claimants receive disjoint spans by the
 * definition of fetch-add, so contiguity and exclusivity need no lock and no
 * observe-then-claim step.  The bound is checked on the RETURNED base — a
 * pre-read check would itself be an observe-then-claim race. */
static uint64_t db_seq_claim(unsigned int home, uint64_t count) {
  if (db_seq_next == NULL) {
    /* DB mints require the shared counters the node geometry opens; reaching
     * here first is an initialization-order bug, not a recoverable state. */
    ARTS_ERROR("GUID generation: DB seq counters not initialized");
  }
  uint64_t base = atomic_fetch_add_explicit(&db_seq_next[home], count,
                                            memory_order_relaxed);
  if (base + count > db_seq_creator_base + arts_db_seq_budget) {
    ARTS_ERROR("GUID generation failed: DB seq slice exhausted "
               "(home %u, budget %lu)",
               home, (unsigned long)arts_db_seq_budget);
  }
  return base;
}

static uint64_t db_seq_mint_one(unsigned int home) {
  if (t_db_cursor == NULL) {
    t_db_cursor = (struct db_seq_cursor_s *)arts_calloc(
        arts_global_rank_count, sizeof(struct db_seq_cursor_s));
    if (db_cursor_registry != NULL) {
      db_cursor_registry[arts_thread_info.thread_id] = t_db_cursor;
    }
  }
  struct db_seq_cursor_s *c = &t_db_cursor[home];
  if (c->cur == c->end) {
    c->cur = db_seq_claim(home, ARTS_GUID_DB_CHUNK);
    c->end = c->cur + ARTS_GUID_DB_CHUNK;
  }
  return c->cur++;
}

uint64_t *arts_guid_generator_get_key(unsigned int rank, unsigned int type) {
  return &arts_node_info
              .keys[arts_thread_info.thread_id][(rank * ARTS_GUID_LAST) + type];
}

/* The first key of the calling thread's flat-kind block. */
static inline uint64_t thread_block_base(void) {
  return arts_guid_flat_base +
         (keys_per_thread * (uint64_t)arts_thread_info.thread_id);
}

arts_guid_t arts_guid_create_for_rank_internal(unsigned int rank,
                                               unsigned int type,
                                               unsigned int guid_count) {
  /* Sentinel rank values (ARTS_HINT_CURRENT_RANK, ARTS_HINT_ROUND_ROBIN)
   * must be resolved to a real rank by the caller before reaching the
   * encoder.  They are stored in 32-bit hint fields and would not fit in
   * the 14-bit GUID rank field (silent truncation).  Guard explicitly so
   * mis-routed sentinels fail loudly. */
  if (rank > ARTS_GUID_RANK_MASK) {
    ARTS_ERROR("GUID encode: rank %u exceeds 14-bit field "
               "(max %lu) — caller must resolve sentinel ranks first",
               rank, (unsigned long)ARTS_GUID_RANK_MASK);
  }
  if (rank >= arts_global_rank_count) {
    /* Both arms index per-home arrays sized to the live rank count (the DB
     * counters this rank's minters share, each thread's flat counter row) —
     * an out-of-range home would be a heap write, not a bad key.  Reject
     * loudly at the boundary instead. */
    ARTS_ERROR("GUID encode: home rank %u out of range (ranks %u)", rank,
               arts_global_rank_count);
  }
  uint64_t key = 0;
  if (type == ARTS_GUID_DB) {
    /* Chunked fast path for single mints; a multi-GUID reservation needs
     * CONSECUTIVE seqs, which no chunk cursor can promise — it claims its
     * span straight from the shared counter instead. */
    uint64_t seq = (guid_count == 1) ? db_seq_mint_one(rank)
                                     : db_seq_claim(rank, guid_count);
    key = ARTS_GUID_DB_KEY(ARTS_GUID_DB_SZHINT_NONE, seq);
  } else {
    uint64_t *key_ptr = arts_guid_generator_get_key(rank, type);
    uint64_t value = *key_ptr;
    if (value + guid_count < keys_per_thread) {
      key = value + thread_block_base();
      (*key_ptr) += guid_count;
    } else {
      ARTS_ERROR("GUID generation failed: out of keys");
    }
  }
  return ARTS_GUID_MAKE(type, rank, key);
}

arts_guid_t arts_guid_create_for_rank(unsigned int rank, unsigned int type) {
  return arts_guid_create_for_rank_internal(rank, type, 1);
}

void arts_guid_generator_cleanup() {
  if (db_cursor_registry != NULL) {
    for (uint64_t t = 0; t < num_tables; t++) {
      arts_free(db_cursor_registry[t]);
    }
    arts_free((void *)db_cursor_registry);
    db_cursor_registry = NULL;
  }
  /* The calling thread's cursor was in the registry. */
  t_db_cursor = NULL;
  arts_free((void *)db_seq_next);
  db_seq_next = NULL;
}

void arts_guid_node_geometry_init(void) {
  /* Every key a thread mints lies in a range only it draws from, fixed by
   * values every rank shares (the key widths, the rank count) and its own
   * rank and thread ids.  Flat keys: rank r owns [r*S, (r+1)*S) with
   * S = 2^48 / N, and its thread t the block of keys_per_thread keys from
   * arts_guid_flat_base + t * keys_per_thread, so a flat key's local table is
   * one division and no rank's thread count matters to another's.  DB seqs:
   * creator r owns [r*B, (r+1)*B) of every home's seq space, B = 2^38 / N. */
  uint64_t nrank = arts_global_rank_count ? arts_global_rank_count : 1;
  num_tables = arts_node_info.total_thread_count;
  uint64_t slice = ((uint64_t)1 << ARTS_GUID_KEY_BITS) / nrank;
  keys_per_thread = num_tables ? slice / num_tables : 0;
  arts_guid_flat_base = slice * arts_global_rank_id;
  arts_guid_flat_span = keys_per_thread * num_tables;

  arts_db_seq_budget = (ARTS_GUID_DB_SEQ_MASK + 1) / nrank;
  db_seq_creator_base = arts_db_seq_budget * arts_global_rank_id;
  db_seq_next =
      (_Atomic(uint64_t) *)arts_malloc(sizeof(*db_seq_next) * (size_t)nrank);
  for (uint64_t r = 0; r < nrank; r++) {
    /* Seq 0 stays unused on rank 0 (counter convention: first value is 1). */
    atomic_init(&db_seq_next[r], db_seq_creator_base + 1);
  }
  db_cursor_registry = (struct db_seq_cursor_s **)arts_calloc(
      num_tables, sizeof(struct db_seq_cursor_s *));
}

void arts_guid_key_generator_init() {
  /* One counter per (rank, kind) in this thread's block, owned by this thread
   * alone. */
  arts_node_info.keys[arts_thread_info.thread_id] = (uint64_t *)arts_malloc(
      sizeof(uint64_t) * ARTS_GUID_LAST * arts_global_rank_count);
  for (unsigned int i = 0; i < ARTS_GUID_LAST * arts_global_rank_count; i++) {
    arts_node_info.keys[arts_thread_info.thread_id][i] = 1;
  }
}

arts_guid_kind_t arts_guid_get_kind(arts_guid_t guid) {
  return (arts_guid_kind_t)ARTS_GUID_GET_TYPE(guid);
}

unsigned int arts_guid_get_rank(arts_guid_t guid) {
  return (unsigned int)ARTS_GUID_GET_RANK(guid);
}

bool arts_guid_is_local(arts_guid_t guid) {
  return (arts_global_rank_id == arts_guid_get_rank(guid));
}

uint64_t arts_guid_get_key(arts_guid_t guid) { return ARTS_GUID_GET_KEY(guid); }

arts_guid_t arts_guid_reserve(arts_guid_kind_t kind, unsigned int rank) {
  arts_guid_t guid = NULL_GUID;
  if (rank == ARTS_HINT_CURRENT_RANK) {
    rank = arts_global_rank_id;
  }
  rank = rank % arts_global_rank_count;
  if ((unsigned int)kind < ARTS_GUID_LAST) {
    guid = arts_guid_create_for_rank_internal(rank, (unsigned int)kind, 1);
    // ARTS_INFO("Allocation Guid %u", guid);
  } else {
    ARTS_WARN("arts_guid_reserve: %u is not a GUID kind; no GUID reserved",
              (unsigned int)kind);
  }
  return guid;
}

arts_guid_t arts_guid_reserve_range(arts_guid_kind_t kind, unsigned int size,
                                    unsigned int rank) {
  if (!size || kind >= ARTS_GUID_LAST) {
    return NULL_GUID;
  }
  if (rank == ARTS_HINT_ROUND_ROBIN) {
    /* arts_guid_from_index plants labeled GUIDs at (idx%nrank,
     * base + idx/nrank) on every rank, so the SAME span [base, base+stride)
     * must be claimed on EVERY home's counter. */
    unsigned int nrank = arts_global_rank_count ? arts_global_rank_count : 1;
    /* ceil(size / nrank), with no sum that can wrap. */
    unsigned int stride = size / nrank + (size % nrank != 0);
    if (kind == ARTS_GUID_DB) {
      if (db_seq_next == NULL) {
        /* The shared counters do not exist yet: fail loudly, never fall into
         * a NULL counter array. */
        ARTS_ERROR("GUID range reservation failed: DB ranges require the "
                   "node geometry's counters");
      }
      /* The counters are SHARED with the wait-free mint fast path, so the
       * base must be claimed, never observed-then-stored: a plain store
       * computed from a stale read would rewind a counter below a chunk
       * another thread already leased, and two live cursors would mint the
       * same seq.  CAS-claim with global restart: a successful CAS is the
       * statement "no allocation landed between the observed value and
       * base+stride on this counter"; any interleaving fails the CAS and
       * restarts with base raised past the intruder.  Lock-free by
       * construction — a restart happens only because ANOTHER claimant
       * (a lease or a competing reserve) completed progress on that
       * counter, and counters are monotone, so some claimant always
       * finishes.  Stranded seqs below a raised base stay unused; each
       * restart strands at most stride per counter. */
      uint64_t base = 0;
      for (unsigned int r = 0; r < nrank; r++) {
        uint64_t v =
            atomic_load_explicit(&db_seq_next[r], memory_order_relaxed);
        if (v > base) {
          base = v;
        }
      }
      for (unsigned int r = 0; r < nrank;) {
        uint64_t v =
            atomic_load_explicit(&db_seq_next[r], memory_order_relaxed);
        if (v > base) {
          base = v; /* someone claimed under us — re-raise every counter */
          r = 0;
          continue;
        }
        if (atomic_compare_exchange_strong_explicit(
                &db_seq_next[r], &v, base + stride, memory_order_relaxed,
                memory_order_relaxed)) {
          r++;
        }
      }
      if (base + stride > db_seq_creator_base + arts_db_seq_budget) {
        ARTS_ERROR("GUID range reservation failed: DB seq slice exhausted");
      }
      uint64_t key = ARTS_GUID_DB_KEY(ARTS_GUID_DB_SZHINT_NONE, base);
      return ARTS_GUID_MAKE((unsigned int)kind, ARTS_DISTRIBUTED_RANK, key);
    }
    /* Non-DB kinds: per-thread counters, no concurrent writer on this row —
     * the read-max-store sequence is single-threaded by construction. */
    uint64_t base_value = 1;
    for (unsigned int r = 0; r < nrank; r++) {
      uint64_t v = *arts_guid_generator_get_key(r, (unsigned int)kind);
      if (v > base_value) {
        base_value = v;
      }
    }
    if (base_value + stride >= keys_per_thread) {
      ARTS_ERROR("GUID range reservation failed: thread key space exhausted");
    }
    for (unsigned int r = 0; r < nrank; r++) {
      *arts_guid_generator_get_key(r, (unsigned int)kind) = base_value + stride;
    }
    uint64_t encoded_key = base_value + thread_block_base();
    return ARTS_GUID_MAKE((unsigned int)kind, ARTS_DISTRIBUTED_RANK,
                          encoded_key);
  }
  if (rank == ARTS_HINT_CURRENT_RANK) {
    rank = arts_global_rank_id;
  }
  return arts_guid_create_for_rank_internal(rank, (unsigned int)kind, size);
}

/* A DB key is [szhint | seq]: range arithmetic must stay INSIDE the seq
 * field.  A carry out of seq would flow through the szhint bits into the
 * rank field (a labeled range's sentinel szhint is all-ones, so a single
 * overflowing index silently names a DIFFERENT home rank).  The guard fails
 * loudly instead; every member of a guarded range then inherits the range's
 * exact szhint bits, which is also what keeps arts_guid_index_from's
 * key-difference arithmetic valid for DB ranges. */
static inline void db_range_index_check(uint64_t base_key, uint64_t offset) {
  if ((base_key & ARTS_GUID_DB_SEQ_MASK) + offset > ARTS_GUID_DB_SEQ_MASK) {
    ARTS_ERROR("GUID range index overflows the DB seq field "
               "(base seq %lu + offset %lu)",
               (unsigned long)(base_key & ARTS_GUID_DB_SEQ_MASK),
               (unsigned long)offset);
  }
}

arts_guid_t arts_guid_from_index(arts_guid_t range_guid, unsigned int idx) {
  if (ARTS_GUID_GET_RANK(range_guid) == ARTS_DISTRIBUTED_RANK) {
    /* Round-robin distribution: home = idx % nrank, key offset = idx / nrank.
     * Same (range, idx) on every rank yields the same GUID. */
    unsigned int nrank = arts_global_rank_count ? arts_global_rank_count : 1;
    unsigned int home = idx % nrank;
    uint64_t base_key = ARTS_GUID_GET_KEY(range_guid);
    if (ARTS_GUID_GET_TYPE(range_guid) == ARTS_GUID_DB) {
      db_range_index_check(base_key, idx / nrank);
    }
    uint64_t key = base_key + (idx / nrank);
    return ARTS_GUID_MAKE(ARTS_GUID_GET_TYPE(range_guid), home, key);
  }
  if (ARTS_GUID_GET_TYPE(range_guid) == ARTS_GUID_DB) {
    db_range_index_check(ARTS_GUID_GET_KEY(range_guid), idx);
  }
  return range_guid + idx;
}

int64_t arts_guid_index_from(arts_guid_t range_guid, arts_guid_t guid) {
  if (ARTS_GUID_GET_TYPE(range_guid) != ARTS_GUID_GET_TYPE(guid)) {
    return -1;
  }
  /* For DB kinds a member carries the range's exact szhint bits (see
   * db_range_index_check); differing szhint therefore means "not from this
   * range", and the subtraction below must only ever see seq deltas. */
  bool db_kind = (ARTS_GUID_GET_TYPE(range_guid) == ARTS_GUID_DB);
  if (db_kind && (ARTS_GUID_DB_GET_SZHINT(ARTS_GUID_GET_KEY(range_guid)) !=
                  ARTS_GUID_DB_GET_SZHINT(ARTS_GUID_GET_KEY(guid)))) {
    return -1;
  }
  if (ARTS_GUID_GET_RANK(range_guid) == ARTS_DISTRIBUTED_RANK) {
    /* Inverse of round-robin: idx = key_offset * nrank + home. */
    unsigned int nrank = arts_global_rank_count ? arts_global_rank_count : 1;
    uint64_t base_key = ARTS_GUID_GET_KEY(range_guid);
    uint64_t guid_key = ARTS_GUID_GET_KEY(guid);
    if (guid_key < base_key) {
      return -1;
    }
    unsigned int home = (unsigned int)ARTS_GUID_GET_RANK(guid);
    if (home >= nrank) {
      return -1;
    }
    return (int64_t)(((guid_key - base_key) * nrank) + home);
  }
  if (ARTS_GUID_GET_RANK(range_guid) != ARTS_GUID_GET_RANK(guid)) {
    return -1;
  }
  uint64_t start_key = ARTS_GUID_GET_KEY(range_guid);
  uint64_t check_key = ARTS_GUID_GET_KEY(guid);
  if (check_key < start_key) {
    return -1;
  }
  return (int64_t)(check_key - start_key);
}

uint64_t arts_guid_hash_key(arts_guid_t guid) {
  uint64_t key = arts_guid_get_key(guid);
  return key % (uint64_t)arts_node_info.gpu;
}

/* ── szhint encode/decode ───────────────────────────────────────────────────
 * [exp:5 | man:5] over 64-byte granules (see guid.h).  Encoding rounds UP so
 * the decoded bound always covers the size; the all-ones value stays
 * unreachable from any size (it is the "no information" sentinel), so a
 * sentinel can never alias a real bound. */

uint64_t arts_db_szhint_encode(uint64_t size_bytes) {
  uint64_t g = (size_bytes + 63) >> 6; /* ceil to granules */
  if (g < 32) {
    return g; /* exp 0: exact */
  }
  /* Normalize to (32+man) << (exp-1), man in [0,31], rounding the mantissa
   * up.  A carry out of the mantissa bumps the exponent (g was a power-step
   * boundary). */
  unsigned exp = 64 - (unsigned)__builtin_clzll(g) - 5; /* g >= 32 => >= 1 */
  uint64_t man = ((g + (((uint64_t)1 << (exp - 1)) - 1)) >> (exp - 1)) - 32;
  if (man > 31) {
    exp++;
    man = ((g + (((uint64_t)1 << (exp - 1)) - 1)) >> (exp - 1)) - 32;
  }
  uint64_t hint = ((uint64_t)exp << 5) | man;
  if (hint >= ARTS_GUID_DB_SZHINT_NONE) {
    return ARTS_GUID_DB_SZHINT_NONE; /* beyond the range: no information */
  }
  return hint;
}

arts_guid_t arts_db_guid_stamp_szhint(arts_guid_t guid, uint64_t size_bytes) {
  uint64_t hint = arts_db_szhint_encode(size_bytes);
  uint64_t cleared =
      (uint64_t)guid &
      ~(ARTS_GUID_DB_SZHINT_MASK << ARTS_GUID_DB_SZHINT_SHIFT);
  return (arts_guid_t)(cleared | (hint << ARTS_GUID_DB_SZHINT_SHIFT));
}
