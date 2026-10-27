/* SPDX-License-Identifier: Apache-2.0
 *
 * EXCL protocol, PURGE release policy.
 *
 * The home holds the canonical payload, so it both arbitrates and serves: a
 * write grant leaves the home carrying the bytes, and a reader phase is fanned
 * out from the home's own buffer.  For that to be sound the bytes must be back
 * before anyone else can be granted, which is why a release carries the payload
 * to the home and hands the grant back in the SAME message — the grant cannot
 * outlive the write-through, and returning it costs nothing once the payload has
 * to travel anyway.  A rank's next write therefore re-requests from the home and
 * receives the payload again.
 *
 * That is the whole difference from the RETAIN release policy, where the payload stays
 * with the last writer and a release with nothing pending sends nothing at all.
 *
 * Compiled only for ARTS_COHERENCE_PROTOCOL=EXCL + ARTS_RELEASE_POLICY=PURGE;
 * the release-policy variant is a link-time file choice, so this TU contains no
 * release-policy preprocessor guards.
 */

/* excl/types.h must precede all other coherence headers: it defines
 * arts_db_cache_s, arts_db_s, arts_db_excl_waiter_s, and the
 * arts_home_grantreq_queue_s for the EXCL build (coherence.h and handlers.h
 * declare functions that take these types by pointer). */
#include "arts/coherence/excl/types.h"

#include "arts/utils/stdatomic_compat.h"
#include <assert.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arts/coherence/buffer.h"
#include "arts/coherence/census.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/handlers.h"
#include "arts/coherence/directory.h"
#include "arts/db.h"
#include "arts/edt.h"
#include "arts/cxl/store.h"
#include "arts/gas/route_table.h"
#include "arts/ooo.h"
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/system/identity.h"
#include "arts/system/print.h"
#include "arts/system/schedfuzz.h"
#include "arts/system/threads.h"
#include "arts/transport/net.h"
#include "arts/transport/protocol.h"
#include "arts/utils/malloc.h"

/* ===== Pure state-transition arbiters ==================================
 * excl_compute_next and cache_compute_next are pure functions (no external
 * calls, no global state).  They live in arbiters.c so the unit test
 * (tests/unit/excl_compute_next.c) can #include just the arbiters without
 * pulling in the full handler bodies. */
#include "arbiters.c"
#include "arts/counter/Preamble.h"

/* ===== Grant dispatcher (shared by request + release handlers) ========= */

/* Whether lock_state admits readers: some are counted and no writer holds the
 * block (no writer counted, or the counted writers all wait behind the read
 * phase). */
static bool lock_read_phase_open(uint64_t s) {
  return EXCL_STATE_R(s) > 0u &&
         (EXCL_STATE_W(s) == 0u || EXCL_STATE_BIT(s) == EXCL_PHASE_BIT_RO);
}

/* Grant every reader queued on ro_waiters, from the home's store (CXL) or
 * from buf_h.  A node is queued only after its reader's count has committed,
 * so a drained reader is always counted.  The drain takes the whole stack,
 * including readers that a later write phase holds back, and the frame running
 * it may have been preempted since the transition that sent it here.  So a
 * batch is granted only against a load taken after its drain: the nodes'
 * counts were committed before their pushes, which the drain reads, so the
 * load is at least as new as every count, and an open read phase stays open
 * until these readers release.  Any other phase means the nodes wait for the
 * flip that ends it.  They are pushed back and the word is read once more,
 * because that flip may have drained between this drain and the push-back.
 * The push-back and the flip's drain are acq_rel RMWs of the one stack head:
 * if the drain is the later one it takes the nodes; if not, the last
 * push-back reads it and synchronizes with it, and the load sees the flip. */
static void lock_release_commit(struct arts_db_s *db,
                                struct arts_db_cache_s *cache,
                                arts_db_access_mode_t mode);

/* A node whose rank's write request took its ro_queued bit is not granted:
 * that write turn covers its readers, so it is dismissed and its read count
 * released here, once the batch is out. */
static void lock_home_serve_ro(struct arts_db_s *db,
                               struct arts_db_cache_s *cache,
                               arts_shared_ptr_t buf_h, uint64_t data_size) {
#ifdef ARTS_USE_CXL
  struct arts_rdzv_landing_s pub = {arts_db_cxl_slot_addr(cache), 0, 0, 0};
  (void)buf_h;
#endif
  for (;;) {
    arts_sched_fuzz_point();
    arts_lf_link_t *node = arts_lf_stack_drain(&db->ro_waiters);
    if (node == NULL) {
      return;
    }
    uint64_t fresh =
        atomic_load_explicit(&db->lock_state, memory_order_acquire);
    if (lock_read_phase_open(fresh)) {
      uint32_t dismissed = 0u;
      while (node != NULL) {
        arts_lf_link_t *nx =
            atomic_load_explicit(&node->next, memory_order_relaxed);
        struct arts_lock_ro_node_s *rn =
            ARTS_CONTAINER_OF(node, struct arts_lock_ro_node_s, link);
        if (!arts_rank_bitset_put(&db->ro_queued, rn->rank, false)) {
          arts_send_db_excl_cts(db, rn->rank, data_size, (uint32_t)DB_MODE_RO,
                                rn->cover);
          dismissed++;
          arts_free(rn);
          node = nx;
          continue;
        }
#ifdef ARTS_USE_CXL
        arts_send_db_excl_grant(rn->rank, cache->db_guid, DB_MODE_RO,
                                /*version=*/0u, &rn->rdzv, &pub,
                                /*src_h=*/NULL, data_size);
#else
        /* Each fan-out PUT pins the source with its own strong ref. */
        arts_send_db_excl_grant(rn->rank, cache->db_guid, DB_MODE_RO,
                                /*version=*/0u, &rn->rdzv, NULL,
                                (arts_shared_get(buf_h) != NULL)
                                    ? arts_shared_copy(buf_h)
                                    : NULL,
                                data_size);
#endif
        arts_free(rn);
        node = nx;
      }
      while (dismissed-- > 0u) {
        lock_release_commit(db, cache, DB_MODE_RO);
      }
      return;
    }
    arts_sched_fuzz_point();
    while (node != NULL) {
      arts_lf_link_t *nx =
          atomic_load_explicit(&node->next, memory_order_relaxed);
      arts_lf_stack_push(&db->ro_waiters, node);
      node = nx;
    }
    fresh = atomic_load_explicit(&db->lock_state, memory_order_acquire);
    if (!lock_read_phase_open(fresh)) {
      return;
    }
  }
}

/* Acquire the home buffer's data for a grant payload and fan-out grants.
 * After pop (rw) / drain (ro), for each target rank: self → direct handler
 * call (local hit, wire 0) / remote → MSG_DB_EXCL_GRANT. */
static void lock_home_grant(struct arts_db_s *db, struct arts_db_cache_s *cache,
                            uint32_t grant) {
  if (grant == LOCK_GRANT_NONE) {
    return;
  }
  if (grant == LOCK_GRANT_DESTROY) {
    /* This release was the last one owed to a home a destroy already marked.
     * Nothing is left to grant — tear the home down instead. */
    arts_excl_home_teardown(db, cache->db_guid);
    return;
  }
#ifdef ARTS_USE_CXL
  /* The grant carries the block's store, never its bytes: the home holds no
   * canonical copy on this arm, and every grantee reads the store itself.
   * The landing's address field names it — the only thing that travels — and
   * txid 0 says nothing is in flight. */
  struct arts_rdzv_landing_s pub = {arts_db_cxl_slot_addr(cache), 0, 0, 0};
  uint64_t data_size =
      atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  if (grant == LOCK_GRANT_ONE_RW) {
    unsigned int rank;
    struct arts_rdzv_landing_s rdzv;
    if (arts_home_grantreq_queue_pop(&db->rw_waiters, &rank, &rdzv, NULL)) {
      arts_send_db_excl_grant(rank, cache->db_guid, DB_MODE_RW, /*version=*/0u,
                              &rdzv, &pub, /*src_h=*/NULL, data_size);
    }
  } else { /* LOCK_GRANT_ALL_RO */
    lock_home_serve_ro(db, cache, /*buf_h=*/NULL, data_size);
  }
#else
  /* A first use of a block whose create took no hold: the grant about to
   * leave carries the block's bytes, and under this release policy the home
   * is the one rank entitled to say what they are — so materialize them here
   * when the block has no storage yet.  A no-op for every other block. */
  (void)arts_db_buf_ensure(
      cache, atomic_load_explicit(&cache->db_size, memory_order_relaxed));
  arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf =
      (struct arts_db_buffer_s *)arts_shared_get(buf_h);
  /* The size is descriptor state — known even before any payload exists;
   * the grant carries it so a hinted (CTS-skipping) first touch learns it. */
  uint64_t data_size =
      atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  if (grant == LOCK_GRANT_ONE_RW) {
    unsigned int rank;
    struct arts_rdzv_landing_s rdzv;
    if (arts_home_grantreq_queue_pop(&db->rw_waiters, &rank, &rdzv, NULL)) {
      /* RW grant: advertise home's stable buffer as THIS grant's publish
       * landing (grants and RW releases pair 1:1); the releaser PUTs its
       * dirty bytes straight into it.  In-place is safe at home for the same
       * reason the install always was: the global RW lock excludes every
       * reader while the publish is in flight. */
      struct arts_rdzv_landing_s pub = {0, 0, 0, 0};
      if (buf != NULL && rank != arts_global_rank_id &&
          arts_net_rdzv_local(buf->data, data_size, &pub.addr, &pub.key)) {
        pub.txid = arts_net_rdzv_txid_next();
        pub.cookie = 0; /* in-place: home finds its buffer via the cache */
      }
      arts_send_db_excl_grant(rank, cache->db_guid, DB_MODE_RW, /*version=*/0u,
                              &rdzv, &pub, buf_h, data_size);
      buf_h = NULL; /* consumed by the grant sender */
    }
  } else { /* LOCK_GRANT_ALL_RO */
    lock_home_serve_ro(db, cache, buf_h, data_size);
  }
  if (buf_h != NULL) {
    arts_db_buf_release(&buf_h);
  }
#endif /* ARTS_USE_CXL */
}

/* ===== arts_send_db_excl_grant ========================================= */

void arts_send_db_excl_grant(unsigned int requester_rank, arts_guid_t db_guid,
                             arts_db_access_mode_t mode, uint64_t version,
                             const struct arts_rdzv_landing_s *req_rdzv,
                             const struct arts_rdzv_landing_s *pub,
                             arts_shared_ptr_t src_h, uint64_t data_size) {
  struct arts_db_buffer_s *src =
      (struct arts_db_buffer_s *)arts_shared_get(src_h);
  struct arts_msg_excl_grant_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_EXCL_GRANT);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  p.mode = (uint32_t)mode;
  p.pad = 0;
  p.version = version;
  /* Always the DB's size; payload presence is rdzv_txid / trailing bytes. */
  p.data_size = data_size;
  p.rdzv_txid = 0;
  p.rdzv_cookie = (req_rdzv != NULL) ? req_rdzv->cookie : 0;
  if (pub != NULL) {
    p.pub.addr = pub->addr;
    p.pub.key = pub->key;
    p.pub.txid = pub->txid;
    p.pub.cookie = pub->cookie;
  } else {
    p.pub = (struct arts_msg_rdzv_landing_s){0, 0, 0, 0};
  }
  if (requester_rank == arts_global_rank_id) {
    /* Self-send (home == requester): the granted buffer already sits in this
     * rank's own cache (home and requester name the same single stable buffer),
     * so there is nothing to serialize or install — copying the whole DB out to
     * a packet and back into the same buffer would be pure waste.  Emit a
     * data-less self-grant: the handler skips the in-place install and runs the
     * grant commit (cache-state transition + waiter drain) against the live
     * buffer.  A requester on the home advertises no landing: nothing would
     * land in it. */
    if (src != NULL) {
      arts_db_buf_release(&src_h);
    }
    p.header.size = sizeof(p);
    arts_handler_db_excl_grant(&p, sizeof(p));
    return;
  }
#ifdef ARTS_USE_CXL
  /* Every grant on this arm is data-less: it names the block's store and the
   * grantee reads it, so there is no source buffer to pin and no landing to
   * write into. */
  if (src != NULL) {
    arts_db_buf_release(&src_h);
  }
  arts_transport_send_async((int)requester_rank, (char *)&p, sizeof(p));
#else
  if (src == NULL || req_rdzv == NULL || req_rdzv->txid == 0) {
    /* Data-less grant (sentinel DB / nothing published). */
    if (src != NULL) {
      arts_db_buf_release(&src_h);
    }
    arts_transport_send_async((int)requester_rank, (char *)&p, sizeof(p));
    return;
  }
  /* One-sided grant: PUT straight from the home buffer into the requester's
   * stable-buffer landing (EXCL's fixed-address install), pairing packet and
   * write completion by txid.  The strong ref transfers to the PUT's local
   * completion, so a concurrent destroy cannot free the bytes mid-read. */
  uint64_t ds = data_size;
  p.data_size = ds;
  p.rdzv_txid = req_rdzv->txid;
  arts_net_put_payload((int)requester_rank, req_rdzv->addr, req_rdzv->key,
                       req_rdzv->txid, src->data, ds,
                       arts_db_buf_ref_release_cb, (void *)src_h);
  arts_transport_send_async((int)requester_rank, (char *)&p, sizeof(p));
#endif /* ARTS_USE_CXL */
}

/* ===== arts_handler_db_excl_request ==================================== */

/* One lock_state transition that settles the owed count; returns its grant
 * code (NONE or DESTROY). */
static uint32_t lock_owed_step(struct arts_db_s *db, int op) {
  uint32_t grant;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&db->lock_state, memory_order_acquire);
    if (op != EXCL_OP_OWE &&
        (EXCL_STATE_OWED(cur) == 0u ||
         (op == EXCL_OP_RWO_ARRIVE && EXCL_STATE_RWO(cur) == 0u) ||
         (op == EXCL_OP_ROO_ARRIVE && EXCL_STATE_ROO(cur) == 0u))) {
      ARTS_ERROR("lock: guid %lu settles a request it was never owed "
                 "(lock_state=%llx)",
                 (unsigned long)db->cache.db_guid, (unsigned long long)cur);
    }
    next = excl_compute_next(cur, op, &grant);
    if (grant == LOCK_GRANT_FULL) {
      ARTS_ERROR("lock: guid %lu owes more requests than its owed field holds "
                 "(lock_state=%llx)",
                 (unsigned long)db->cache.db_guid, (unsigned long long)cur);
    }
  } while (!atomic_compare_exchange_weak_explicit(
      &db->lock_state, &cur, next, memory_order_acq_rel, memory_order_acquire));
  return grant;
}

/* The declared-arrival rule, before a request is processed.  Returns false
 * when the request is not processed: the claiming creator's pre-claim
 * requests are discarded unanswered, and a declared read request is
 * dismissed (a data-less answer; no read turn, no count).  `reads`: a read
 * request that, if not declared, is processed as a read (not answered with a
 * size round). */
static bool lock_request_cover(struct arts_db_s *db, unsigned int requester,
                               arts_db_access_mode_t mode, uint32_t cover,
                               bool reads) {
  if ((cover & EXCL_COVER_PRECLAIM) != 0u && requester == db->claim_rank) {
    /* The claiming creator's pre-claim request: the claim's hold served its
     * cohort, closed the axis, and the announce owes it.  Not counted, no
     * size round, no answer; its declaration and parity are not applied (the
     * announce seeded the parity). */
    if (lock_owed_step(db, mode == DB_MODE_RW ? EXCL_OP_RWO_ARRIVE
                                              : EXCL_OP_ROO_ARRIVE) ==
        LOCK_GRANT_DESTROY) {
      arts_excl_home_teardown(db, db->cache.db_guid);
    }
    return false;
  }
  if (mode == DB_MODE_RW && (cover & EXCL_COVER_RO_OPEN) != 0u) {
    /* The read request this rank has out is covered by this write turn and
     * may arrive after the block's destroy.  Count it before the parity step,
     * so its arrival, which reads that step, always finds the count; undo if
     * it has already arrived. */
    bool par = (cover & EXCL_COVER_RO_PAR) != 0u;
    (void)lock_owed_step(db, EXCL_OP_OWE);
    if (arts_rank_bitset_put(&db->ro_parity, requester, par) == par) {
      /* The undo is a settling step like any: if it leaves a marked home
       * with nothing counted or owed it owns the teardown, and the request,
       * which then meets a destroyed block (outside the contract), is
       * dropped. */
      if (lock_owed_step(db, EXCL_OP_ARRIVE) == LOCK_GRANT_DESTROY) {
        arts_excl_home_teardown(db, db->cache.db_guid);
        return false;
      }
      /* The read request arrived first; if it still waits to be served as a
       * read, this write turn covers it instead. */
      (void)arts_rank_bitset_put(&db->ro_queued, requester, false);
    }
  }
  if (mode == DB_MODE_RO) {
    bool par = (cover & EXCL_COVER_PAR) != 0u;
    if (reads) {
      (void)arts_rank_bitset_put(&db->ro_queued, requester, true);
    }
    if (arts_rank_bitset_put(&db->ro_parity, requester, par) == par) {
      if (reads) {
        (void)arts_rank_bitset_put(&db->ro_queued, requester, false);
      }
      /* Declared before it came: a write turn of this rank covers it.  Never
       * processed as a read: dismissed, whether or not the destroy is
       * marked. */
      uint32_t g = lock_owed_step(db, EXCL_OP_ARRIVE);
      arts_send_db_excl_cts(db, requester,
                            atomic_load_explicit(&db->cache.db_size,
                                                 memory_order_relaxed),
                            (uint32_t)DB_MODE_RO, cover);
      if (g == LOCK_GRANT_DESTROY) {
        arts_excl_home_teardown(db, db->cache.db_guid);
      }
      return false;
    }
  }
  return true;
}

void arts_handler_db_excl_request(void *item_v, void *args_v) {
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  struct arts_db_cache_s *cache = &db->cache;
  struct arts_ooo_args_db_excl_request_s *a =
      (struct arts_ooo_args_db_excl_request_s *)args_v;
  unsigned int requester = a->requester;
  arts_db_access_mode_t mode = (arts_db_access_mode_t)a->mode;

#ifndef ARTS_USE_CXL
  bool size_round = a->rdzv.txid == 0 && requester != arts_global_rank_id &&
                    atomic_load_explicit(&cache->db_size,
                                         memory_order_relaxed) > 0 &&
                    arts_global_rank_count > 1;
#else
  bool size_round = false;
#endif
  if (!lock_request_cover(db, requester, mode, a->cover, !size_round)) {
    return;
  }

#ifndef ARTS_USE_CXL
  if (size_round) {
    /* First-touch request without a landing: the requester did not know
     * db_size.  Answer with the size (CTS) and do NOT enqueue — the grant
     * plane requires a landing.  The requester re-issues with one. */
    INCREMENT_NUM_EXCL_SIZE_CTS_BY(1);
    arts_send_db_excl_cts(
        db, requester,
        atomic_load_explicit(&cache->db_size, memory_order_relaxed),
        (uint32_t)mode, a->cover);
    return;
  }
#else
  /* On the arm whose grants carry no payload a request advertises no landing,
   * so its absence says nothing about what the requester knows: the size
   * travels in the grant, which the grantee records before it reads. */
#endif

  arts_rank_bitset_set(&db->cached_ranks,
                       requester); /* destroy fan-out roster */

  uint32_t grant;
  uint64_t cur, next;
  if (mode == DB_MODE_RW) {
    /* Queued before the count: a transition that counts this writer also
     * finds its node, and a pop that meets the push half done waits for it.
     * A writer popped before its own count lands takes the turn of the
     * counted writer queued behind it, whose count stands in for it until
     * its late transition counts it. */
    arts_home_grantreq_queue_push(&db->rw_waiters, requester, &a->rdzv,
                                  ARTS_GRANT_VERSION_NONE);
    arts_sched_fuzz_point();
    do {
      cur = atomic_load_explicit(&db->lock_state, memory_order_acquire);
      next = excl_compute_next(cur, EXCL_OP_RW_ACQ, &grant);
    } while (!atomic_compare_exchange_weak_explicit(&db->lock_state, &cur,
                                                    next, memory_order_acq_rel,
                                                    memory_order_acquire));
    if (grant != LOCK_GRANT_ONE_RW && next != cur) {
      INCREMENT_NUM_EXCL_QUEUE_WAIT_BY(1);
    }
    lock_home_grant(db, cache, grant);
    return;
  }

  /* A reader is queued only after its count has committed.  Every read grant
   * is a drain of the whole stack, so a node queued first could be granted,
   * served and released before its count lands: the release would take r down
   * for a reader never counted, opening a write phase under a counted reader
   * or wrapping r.  Counted first, the opposite gap is a flip that saw this
   * count and drained before the push; the re-check below closes it. */
  struct arts_lock_ro_node_s *n =
      (struct arts_lock_ro_node_s *)arts_malloc(sizeof(*n));
  n->rank = requester;
  n->cover = a->cover;
  n->rdzv = a->rdzv;
  do {
    cur = atomic_load_explicit(&db->lock_state, memory_order_acquire);
    next = excl_compute_next(cur, EXCL_OP_RO_ACQ, &grant);
  } while (!atomic_compare_exchange_weak_explicit(&db->lock_state, &cur, next,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire));
  if (grant != LOCK_GRANT_ALL_RO && next != cur) {
    INCREMENT_NUM_EXCL_QUEUE_WAIT_BY(1);
  }
  arts_sched_fuzz_point();
  arts_lf_stack_push(&db->ro_waiters, &n->link);
  if (grant == LOCK_GRANT_ALL_RO) {
    lock_home_grant(db, cache, grant);
    return;
  }
  /* Held behind a write phase.  The push and the flip's drain are acq_rel
   * RMWs of the one stack head: either that drain takes this node, or the
   * push reads the drain and synchronizes with it, and this load sees the
   * read phase the flip opened (this reader's own count keeps it open). */
  arts_sched_fuzz_point();
  if (lock_read_phase_open(
          atomic_load_explicit(&db->lock_state, memory_order_acquire))) {
    lock_home_grant(db, cache, LOCK_GRANT_ALL_RO);
  }
}

/* ===== arts_handler_db_excl_release ==================================== */

/* The release's commit: the lock_state transition and the next grant.
 * Factored out so the rendezvous continuation (dirty bytes landed) runs the
 * identical commit. */
static void lock_release_commit(struct arts_db_s *db,
                                struct arts_db_cache_s *cache,
                                arts_db_access_mode_t mode) {
  int op = (mode == DB_MODE_RW) ? EXCL_OP_RW_REL : EXCL_OP_RO_REL;
  uint32_t grant;
  uint64_t cur, next;
  arts_sched_fuzz_point();
  do {
    cur = atomic_load_explicit(&db->lock_state, memory_order_acquire);
    /* A write release returns a turn this home granted, so it never finds
     * the writer count at zero; a zero count would wrap, and the block would
     * never be torn down, so a Debug build stops here instead of hanging.
     * A reused label can reach it, since its generations are not told apart:
     * a release meant for the next generation lands here while the marked
     * previous one still waits for a release owed to it. */
    assert(op != EXCL_OP_RW_REL || EXCL_STATE_W(cur) != 0u);
    /* A read release returns a grant to a reader whose count committed before
     * its node could be drained, so it never finds the reader count at zero.
     * A wrapped r would park writers behind readers that do not exist. */
    assert(op != EXCL_OP_RO_REL || EXCL_STATE_R(cur) != 0u);
    next = excl_compute_next(cur, op, &grant);
  } while (!atomic_compare_exchange_weak_explicit(
      &db->lock_state, &cur, next, memory_order_acq_rel, memory_order_acquire));
  lock_home_grant(db, cache, grant);
}

#ifndef ARTS_USE_CXL
/* Rendezvous continuation for a committed remote RW release: the dirty bytes
 * have fully landed IN PLACE in home's stable buffer ("imm seen => buffer
 * valid"; the global RW lock excluded every reader while they flew), so there
 * is nothing to install — run the transition and the next grant; no releaser
 * is blocked on this. */
struct lock_release_landed_ctx_s {
  arts_shared_ptr_t db_h;
  arts_guid_t db_guid;
};

static void lock_release_landed_cb(void *arg) {
  struct lock_release_landed_ctx_s *ctx =
      (struct lock_release_landed_ctx_s *)arg;
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(ctx->db_h);
  if (db != NULL) {
    lock_release_commit(db, &db->cache, DB_MODE_RW);
  }
  arts_shared_release(&ctx->db_h);
  arts_free(ctx);
}
#endif /* ARTS_USE_CXL */

void arts_handler_db_excl_release(void *item_v, void *args_v) {
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  struct arts_db_cache_s *cache = &db->cache;
  struct arts_ooo_args_db_excl_release_s *a =
      (struct arts_ooo_args_db_excl_release_s *)args_v;
  arts_db_access_mode_t mode = (arts_db_access_mode_t)a->mode;

#ifdef ARTS_USE_CXL
  /* A release returns a right and nothing else: the turn's bytes reached the
   * block's store before the message left the releaser, so there is nothing
   * here to announce, to pair with a write completion, or to install. */
  if (a->data_size != 0u) {
    ARTS_ERROR("cxl: a release from rank %u carries a payload; on this arm a "
               "release carries the right it returns and nothing else",
               a->releaser);
  }
#else
  if (mode == DB_MODE_RW && a->data_size > 0 && a->rdzv_txid == 0) {
    /* Announce: the releaser holds dirty bytes but its RW hold carried no
     * grant-provided publish landing (a creator-seeded hold never received
     * a grant).  Advertise home's stable buffer (fresh txid) and let the
     * releaser PUT + commit; nothing transitions yet.  In-place is safe by
     * the same exclusion argument as the grant-advertised landing: the
     * releaser holds the global RW lock, so no reader anywhere touches the
     * buffer while the publish flies. */
    struct arts_rdzv_landing_s landing = {0, 0, 0, 0};
    arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
    struct arts_db_buffer_s *buf =
        (struct arts_db_buffer_s *)arts_shared_get(buf_h);
    if (buf == NULL) {
      /* No home buffer to land in (never installed): materialize the stable
       * buffer first — the PUT fully overwrites it. */
      arts_db_buf_write_inplace(cache, NULL, a->data_size);
      buf_h = arts_db_buf_acquire(cache);
      buf = (struct arts_db_buffer_s *)arts_shared_get(buf_h);
    }
    if (buf == NULL ||
        !arts_net_rdzv_local(buf->data, a->data_size, &landing.addr,
                             &landing.key)) {
      ARTS_ERROR("lock: home stable buffer is not fabric-registered — "
                 "one-sided publish requires the registered pool");
    }
    landing.txid = arts_net_rdzv_txid_next();
    landing.cookie = 0; /* in-place: home finds its buffer via the cache */
    arts_db_buf_release(&buf_h);
    arts_send_db_publish_cts(a->releaser, a->db_guid, &landing, a->cv);
    return;
  }
  if (mode == DB_MODE_RW && a->data_size > 0 && a->rdzv_txid != 0) {
    /* Remote dirty release: the publish PUT straight into home's stable
     * buffer (the landing advertised in this grant, or in the PUBLISH_CTS that
     * answered the announce).  Pair the release packet with the write
     * completion; the transition and the next grant run only once the bytes
     * are fully placed. */
    struct lock_release_landed_ctx_s *ctx =
        (struct lock_release_landed_ctx_s *)arts_malloc(sizeof(*ctx));
    ctx->db_h = arts_route_table_lookup_db(cache->db_guid);
    ctx->db_guid = a->db_guid;
    arts_net_rdzv_expect(a->rdzv_txid, a->data_size, lock_release_landed_cb,
                         ctx);
    return;
  }
  /* Anything else carries no bytes: the home's own write release commits
   * without a message, so a payload reaches the home only one-sided. */
#endif /* ARTS_USE_CXL */

  lock_release_commit(db, cache, mode);
}

/* A destroy does not tear the home down on arrival.  The program orders it
 * after every hold's release, but a release is fire-and-forget, and nothing
 * orders the delivery of the release and the destroy: the home may still
 * hold the lock for a holder whose release is in flight, carrying a counter
 * decrement and, outside the CXL store, a one-sided PUT into the home's
 * buffer.  Tearing down first would free that buffer under the PUT and leave
 * the release parked on an empty slot for good.  So the destroy MARKS the
 * state word and whichever of it and the last release reaches the zero edge
 * performs the teardown; marking and reading the counts commit in one CAS, so
 * neither can conclude the other will do it.
 *
 * The mark delays the slot's return.  An ordinary GUID is minted from a
 * monotonic sequence and never comes back, so only a reused label meets the
 * delay: its next generation's create parks behind the marked generation as
 * any create does, but the next generation's other messages that arrive
 * meanwhile run against the marked one, the creator's release of its own
 * hold included: a reused label's generations are not told apart. */
void arts_handler_db_destroy(void *item_v, void *args_v) {
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_ooo_args_db_destroy_s *a =
      (struct arts_ooo_args_db_destroy_s *)args_v;
  struct arts_db_s *db = arts_db_of_cache(cache);
  if (db == NULL) {
    return;
  }
  uint32_t grant;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&db->lock_state, memory_order_acquire);
    next = excl_compute_next(cur, EXCL_OP_TEARDOWN, &grant);
  } while (!atomic_compare_exchange_weak_explicit(
      &db->lock_state, &cur, next, memory_order_acq_rel, memory_order_acquire));
  if (grant == LOCK_GRANT_DESTROY) {
    arts_excl_home_teardown(db, a->db_guid);
  }
}

/* ===== arts_db_acquire_is_serialized ===================================
 * EXCL: both RW and RO are blocking locks — both are GUID-serialized so
 * the engine acquires them in a global order (deadlock-free lock ordering). */
bool arts_db_acquire_is_serialized(arts_db_access_mode_t mode) {
  return mode == DB_MODE_RW || mode == DB_MODE_RO;
}

/* ===== arts_db_cache_init ==============================================
 * Initialize the per-rank cache for the EXCL protocol.  For EXCL, the home-rank
 * lock_state and the per-cache cache_state single word are the coherence state;
 * arts_db_cache_common_init handles the shared fields (db_guid, db_size,
 * home-directory init).
 *
 * Creator hold: arts_db_create defaults to an RW acquire (OCR contract; only
 * ARTS_DB_PROP_NO_ACQUIRE skips it), so the creator EDT must hold the lock RW
 * exactly like any granted writer — otherwise its matching arts_db_release (or
 * the EDT-epilogue auto-release) would run against a hold that was never taken.
 * For the creator/home kinds we therefore SEED the cache held in RW
 * (rw_state=GRANT, rw_count=1); the home lock_state is seeded w=1 in
 * arts_db_home_init.  The release path drives both back to 0 — the standard
 * acquire/release pair, no protocol-specific create bookkeeping.  (NO_ACQUIRE
 * resets these to idle in arts_db_create, mirroring the single-owner arms.) */
void arts_db_cache_init(struct arts_db_cache_s *c, arts_guid_t db_guid,
                        uint64_t db_size, arts_db_init_kind_t kind,
                        unsigned int creator_rank) {
  uint64_t seed = 0ULL; /* rw_state=ro_state=IDLE, both counts 0 */
  if (kind == ARTS_DB_INIT_CREATOR_HOME ||
      kind == ARTS_DB_INIT_CREATOR_REMOTE) {
    /* Creator holds RW: GRANT + one writer. */
    seed = CACHE_MAKE(CACHE_ST_GRANT, CACHE_ST_IDLE, 1u, 0u);
  } else if (kind == ARTS_DB_INIT_STUB && ARTS_DB_STUB_CLAIMABLE(db_guid)) {
    seed = CACHE_MAKE_X(CACHE_ST_IDLE, CACHE_ST_IDLE, 0u, 1u, 0u, 0u);
  }
  atomic_store_explicit(&c->cache_state, seed, memory_order_relaxed);
  arts_lf_stack_init(&c->ro_pending);
  arts_lf_stack_init(&c->rw_pending);
#ifndef ARTS_USE_CXL
  c->home_pub_rdzv = (struct arts_rdzv_landing_s){0, 0, 0, 0};
#endif
  arts_db_cache_common_init(c, db_guid, db_size, kind, creator_rank);
}

/* ===== arts_db_cache_destructor =========================================
 * free parked nodes only, no wake: a waiter still parked is an acquire the
 * destroy contract excludes.
 *
 * Destructor runs at refcount 0 as the SOLE owner of the object; no concurrent
 * actor remains, so it is the single safe drainer of the parked-waiter stacks.
 * Destroy itself is just the route-slot detach (CAS value→NULL + drop the
 * install ref). */
void arts_db_cache_destructor(struct arts_db_cache_s *cache) {
  if (cache == NULL) {
    return;
  }
  arts_db_cache_common_destroy_pre(cache); /* buffer-NULL FIRST */
  /* Drain + free the parked-waiter stacks (empty in the normal case — a legit
   * destroy has no outstanding acquirers). */
  for (arts_lf_stack_t *q = &cache->ro_pending;; q = &cache->rw_pending) {
    arts_lf_link_t *node = arts_lf_stack_drain(q);
    while (node != NULL) {
      arts_lf_link_t *nx =
          atomic_load_explicit(&node->next, memory_order_relaxed);
      struct arts_db_excl_waiter_s *w =
          ARTS_CONTAINER_OF(node, struct arts_db_excl_waiter_s, link);
      arts_free(w);
      node = nx;
    }
    if (q == &cache->rw_pending) {
      break;
    }
  }
  arts_db_cache_common_destroy_post(cache); /* snapshot free → home teardown */
}

/* Retract nothing: on this arm a create that takes no hold rewrites the whole
 * cache word to idle already, so no copy claim survives it. */
/* Claim nothing: this arm keeps no reader copy past a write turn, so a
 * create's copy asserts nothing that has to be recorded. */
void arts_db_create_claim_creator_copy(struct arts_db_s *db) { (void)db; }

void arts_db_create_retract_creator_copy(struct arts_db_s *db) { (void)db; }

/* ===== arts_db_create_install_home_buffer ================================
 * EXCL home init: the home leaves a create holding the block's storage.
 *
 * Where the home is the canonical backing store, that storage is its own
 * buffer, its bytes unspecified until a holder writes them: the first GRANT
 * carries whatever it holds to the requester, and the requester's first RW
 * release sends the updated contents back via EXCL_RELEASE publish.
 *
 * Where the block's bytes live in a store of their own, this is the home's
 * handle on that store — a working copy the home's own turns fetch into and
 * purge out of, or the store itself where no copy is kept.  Nothing is
 * invented there: a create's bytes are the creator's, and the handle is made
 * against the store the caller must already have recorded. */
void arts_db_create_install_home_buffer(struct arts_db_cache_s *cache,
                                        uint64_t db_size) {
  if (db_size > 0) {
    arts_db_buf_write_inplace(cache, /*data=*/NULL, db_size);
  }
}

/* ===== arts_send_db_excl_request ========================================
 * Send MSG_DB_EXCL_REQUEST to the home rank.  Self-send (home == this rank)
 * dispatches through the OoO engine (HIT runs inline; MISS defers until the
 * home db_s is installed).  Remote send goes via the transport. */
#ifndef ARTS_USE_CXL
/* Materialize this rank's stable buffer (EXCL's fixed-address backing store)
 * and advertise it as the grant landing: the grant PUT installs IN PLACE,
 * preserving the address across the DB's whole lifetime.  A fresh txid is
 * drawn per request (each request is served by at most one grant).  False
 * only when no payload can move here; a sized block whose buffer cannot be
 * advertised is fatal, since a landing-less request is answered with a size
 * round that would only repeat. */
static bool lock_stable_landing(struct arts_db_cache_s *cache,
                                struct arts_rdzv_landing_s *out) {
  *out = (struct arts_rdzv_landing_s){0, 0, 0, 0};
  uint64_t fetch_size = arts_db_first_fetch_size(cache);
  if (fetch_size == 0 || arts_global_rank_count <= 1) {
    return false;
  }
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf = (struct arts_db_buffer_s *)arts_shared_get(h);
  if (buf == NULL) {
    /* First touch: materialize the one stable buffer (bytes unspecified —
     * the grant PUT fully overwrites it before any drained waiter reads, and
     * a grant with no PUT is one for a block nobody has written).
     * fetch_size may be the GUID bound — an ALLOCATION size only; the DB's
     * size is declared exclusively by the wire (prepare never records it). */
    arts_db_buf_prepare_inplace(cache, fetch_size);
    h = arts_db_buf_acquire(cache);
    buf = (struct arts_db_buffer_s *)arts_shared_get(h);
  }
  if (buf == NULL ||
      !arts_net_rdzv_local(buf->data, fetch_size, &out->addr, &out->key)) {
    ARTS_ERROR("lock: stable buffer is unplaced or not fabric-registered — "
               "one-sided grant requires the registered pool");
  }
  out->txid = arts_net_rdzv_txid_next();
  out->cookie = 0; /* in-place: this rank finds the buffer via its cache */
  arts_db_buf_release(&h);
  return true;
}
#endif /* ARTS_USE_CXL */

void arts_send_db_excl_request(struct arts_db_cache_s *cache,
                               arts_db_access_mode_t mode, uint32_t cover) {
  arts_guid_t db_guid = cache->db_guid;
  unsigned int home_rank = arts_guid_get_rank(db_guid);
  if (home_rank == arts_global_rank_id) {
    /* Self-send: route through the OoO engine so before-create reorders are
     * handled correctly (the engine defers when the slot is absent).  The
     * home is the canonical store, so its own grant is data-less and no
     * landing is advertised. */
    struct arts_ooo_args_db_excl_request_s args = {
        .requester = arts_global_rank_id,
        .db_guid = db_guid,
        .mode = (uint32_t)mode,
        .cover = cover,
        .rdzv = {0, 0, 0, 0},
    };
    arts_ooo_dispatch_or_defer_guid(db_guid, OOO_DB_EXCL_REQUEST, &args,
                                    sizeof(args));
    return;
  }
  /* Zeroed outside every conditional: no arm may reach the send with stack
   * garbage where a landing would be. */
  struct arts_rdzv_landing_s rdzv = {0, 0, 0, 0};
#ifndef ARTS_USE_CXL
  (void)lock_stable_landing(cache, &rdzv);
#endif
  struct arts_msg_excl_request_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_EXCL_REQUEST);
  p.db_guid = db_guid;
  p.mode = (uint32_t)mode;
  p.cover = cover;
  p.rdzv.addr = rdzv.addr;
  p.rdzv.key = rdzv.key;
  p.rdzv.txid = rdzv.txid;
  p.rdzv.cookie = rdzv.cookie;
  if (!arts_net_send_held((int)home_rank, (char *)&p, sizeof(p),
                          mode == DB_MODE_RO ? ARTS_HELD_READ
                                             : ARTS_HELD_WRITE)) {
    arts_transport_send_async((int)home_rank, (char *)&p, sizeof(p));
  }
}

/* EXCL_CTS sender (a size round, or a read request's dismiss) +
 * requester-side body. */
void arts_send_db_excl_cts(struct arts_db_s *db, unsigned int requester_rank,
                           uint64_t db_size, uint32_t mode, uint32_t cover) {
  struct arts_msg_excl_cts_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_EXCL_CTS);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db->cache.db_guid;
  p.db_size = db_size;
  p.mode = mode;
  p.cover = cover;
  if (requester_rank == arts_global_rank_id) {
    /* The home's own read request dismissed: the transport drops a
     * self-send, so run the requester's body here, on the descriptor the
     * caller pins. */
    arts_handler_db_excl_cts(db, &p);
    return;
  }
  arts_transport_send_async((int)requester_rank, (char *)&p, sizeof(p));
}

/* A read request's cover word: its parity, and PRECLAIM while the cache is a
 * claimable first touch. */
static uint32_t excl_ro_cover(uint64_t s) {
  return (CACHE_RO_PAR(s) ? EXCL_COVER_PAR : 0u) |
         (CACHE_STUB(s) ? EXCL_COVER_PRECLAIM : 0u);
}

/* The cover word a write request carries: the read request this rank has out,
 * and whether the cache is still an unclaimed first touch.  `s` is the word of
 * the transition that opened (or re-sends) the write request. */
static uint32_t excl_rw_cover(uint64_t s) {
  uint32_t c = 0u;
  if (CACHE_RO_ST(s) == CACHE_ST_REQ) {
    c |= EXCL_COVER_RO_OPEN;
    if (CACHE_RO_PAR(s)) {
      c |= EXCL_COVER_RO_PAR;
    }
  }
  if (CACHE_STUB(s)) {
    c |= EXCL_COVER_PRECLAIM;
  }
  return c;
}

void arts_handler_db_excl_cts(void *item_v, void *args_v) {
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_msg_excl_cts_packet_s *p =
      (struct arts_msg_excl_cts_packet_s *)args_v;
  arts_db_cache_learn_size(cache, p->db_size);
  if ((arts_db_access_mode_t)p->mode == DB_MODE_RW) {
    /* A write request is answered by its grant alone, and one whose cohort a
     * create served was absorbed at the home: the axis is at REQ.  While it
     * is, no read request opens, so the read state the re-send declares is
     * the one its first leg declared or one its answer has since closed. */
    uint32_t dact;
    uint64_t s, n;
    do {
      s = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
      n = cache_compute_next(s, CACHE_OP_DECLAIM, &dact);
      if (dact == CACHE_ACT_LATE) {
        ARTS_ERROR("lock: guid %lu got a size round for a write request it "
                   "does not have out (word=%llx)",
                   (unsigned long)cache->db_guid, (unsigned long long)s);
      }
    } while (!atomic_compare_exchange_weak_explicit(
        &cache->cache_state, &s, n, memory_order_acq_rel,
        memory_order_acquire));
    arts_send_db_excl_request(cache, DB_MODE_RW, excl_rw_cover(n));
    return;
  }
  /* A size round or a dismiss of the read request; its parity must be the
   * open request's, checked against the word the CAS commits on. */
  uint32_t par = (p->cover & EXCL_COVER_PAR) != 0u ? 1u : 0u;
  uint32_t act;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    next = cache_compute_next(cur, CACHE_OP_CTS_RO, &act);
    if (act == CACHE_ACT_LATE || CACHE_RO_PAR(cur) != par) {
      ARTS_ERROR("lock: guid %lu got a size round or dismiss (parity %u) for "
                 "a read request it does not have out (word=%llx)",
                 (unsigned long)cache->db_guid, par, (unsigned long long)cur);
    }
  } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                  next, memory_order_acq_rel,
                                                  memory_order_acquire));
  if (act == CACHE_ACT_RESEND_RO) {
    arts_send_db_excl_request(cache, DB_MODE_RO, excl_ro_cover(next));
  }
}

/* Serve exactly `expected` waiters from `q` — the parked population the
 * caller's GRANT CAS observed in the counts, which is exact: counts return to
 * zero at every 0-edge, arrivals under a held grant self-serve (never push),
 * so everything counted at the grant is parked (or about to be).  The grant
 * committer is the SOLE drainer; each whole-stack atomic-exchange batch is
 * consumed once (exactly-once serve).  A counted waiter whose push has not
 * landed yet — the instruction window between its acquire CAS and its push —
 * is awaited by re-draining, the same bounded-window retry discipline as the
 * home queue's mid-push pop.  Serving = resolve + cursor-advance + account, in
 * that order; it does NOT touch the count (the count was carried by the
 * waiter's acquire CAS — "counted ⟹ will park"). */
static void lock_drain_pending(arts_lf_stack_t *q, uint32_t expected) {
  uint32_t served = 0;
  while (served < expected) {
    if (arts_lf_stack_empty(q)) {
      continue; /* a counted waiter's push is still on its way */
    }
    arts_lf_link_t *node = arts_lf_stack_drain(q);
    while (node != NULL) {
      arts_lf_link_t *nx =
          atomic_load_explicit(&node->next, memory_order_relaxed);
      struct arts_db_excl_waiter_s *w =
          ARTS_CONTAINER_OF(node, struct arts_db_excl_waiter_s, link);
      /* Claim the slot's acquire record with the installed buffer, let the
       * serialized walk past this slot, and account (which may hand the
       * payload to depv and schedule the EDT when acquire_remaining reaches
       * 0). */
      mark_edt_ready_by_guid(w->edt_guid, w->slot);
      arts_free(w);
      served++;
      node = nx;
    }
  }
  if (served != expected) {
    ARTS_ERROR("lock: a drain served %u waiters where its transition counted "
               "%u",
               served, expected);
  }
}

/* The hold is this arm's own: one CAS of the cache word, through the arbiter
 * like every other transition of it, and then the actions that transition
 * owes.  A read this rank had already asked for is covered by the turn the
 * create takes (RW ⊇ RO) and is served here, exactly as a granted RW phase
 * serves its RO cohort.  The home's directory is not touched from here — it
 * comes from the announce, where arts_db_home_init names the creator. */
bool arts_db_create_take_hold(struct arts_db_cache_s *cache, uint8_t *owed) {
  uint32_t act;
  uint64_t cur, next;
  *owed = 0u;
  do {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    next = cache_compute_next(cur, CACHE_OP_CREATE_HOLD, &act);
    if (act == CACHE_ACT_LATE) {
      ARTS_ERROR("lock: guid %lu has more write acquires counted on one rank "
                 "than its cache word can hold (word=%llx)",
                 (unsigned long)cache->db_guid, (unsigned long long)cur);
    }
    if (next == cur) {
      return false; /* the word carries the block already */
    }
  } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                  next, memory_order_acq_rel,
                                                  memory_order_acquire));
  /* What the claimed first touch had already asked for, read from the word
   * this claim replaced: its requests are the home's to absorb. */
  if (CACHE_RO_ST(cur) == CACHE_ST_REQ) {
    *owed |= ARTS_CREATE_OWED_RO;
    if (CACHE_RO_PAR(cur)) {
      *owed |= ARTS_CREATE_OWED_RO_PAR;
    }
  }
  if (CACHE_RW_ST(cur) == CACHE_ST_REQ) {
    *owed |= ARTS_CREATE_OWED_RW;
  }
  if (act == CACHE_ACT_DRAIN_BOTH) {
    /* The counts are the pre-image's: the population this transition owes,
     * exactly as a GRANT_RW landing drains it (the creator's own +1 is not
     * parked). */
    lock_drain_pending(&cache->rw_pending, CACHE_RW_CNT(cur));
    lock_drain_pending(&cache->ro_pending, CACHE_RO_CNT(cur));
  }
  return true;
}

#ifdef ARTS_USE_CXL
/* ===== The two funnels =================================================
 *
 * The block's bytes live in its store, not on the wire.  The transfer through
 * the store happens at a rank's two ownership edges and nowhere else: a grant
 * names the store and the grantee FETCHES it where the grant is applied,
 * before the CAS that admits anyone, and the last release of a write turn
 * PURGES the working copy back into the store before the right is returned.
 * Each transfer is the flush and the copy, run synchronously on whatever
 * thread reaches the edge; nothing is deferred, and nothing is acknowledged
 * beyond the protocol's own release.  Acquires and releases between the two
 * edges touch the working copy alone.  Both funnels are the same on either
 * residency; what "bring" and "write back" mean is the two bodies at the end
 * of this block. */

/* The arm's own observables. */
_Atomic(uint64_t) arts_cxl_fetches;
_Atomic(uint64_t) arts_cxl_purges;

/* The residency seam: declared once here, defined exactly once per residency,
 * and called only from the two shared funnels below — which have already
 * validated the store, taken the buffer ref and read the size. */
static void cxl_fetch_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf);
static void cxl_purge_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf);
#if !defined(ARTS_CXL_STAGED) && !defined(ARTS_CXL_DIRECT)
#error "a CXL build names a residency"
#endif

/* A SIZED block must know its store address.  A create records the
 * store before the block can be asked for and every grant names it, so a zero
 * here is a create that skipped its allocation or a grant that carried none —
 * the same defect, with the same answer, on either residency.  It lives in the
 * shared part for that reason: the slot rule is what it checks, not a funnel.
 * A block whose declared size is 0 is exempt: a sentinel has no bytes, so it
 * has no store and a grant for it carries address 0 legitimately. */
static void cxl_slot_required(const struct arts_db_cache_s *cache,
                              const char *edge) {
  if (arts_db_cxl_slot_addr(cache) == 0 &&
      atomic_load_explicit(&cache->db_size, memory_order_relaxed) != 0) {
    ARTS_ERROR("cxl: guid %lu has no slot at its %s edge",
               (unsigned long)cache->db_guid, edge);
  }
}

/* Bring the block's canonical bytes under this rank's grant.  Runs before the
 * grant's admitting CAS, while the granted axis still rests at REQ, so every
 * acquire of it is parked and nothing reads or writes the working copy being
 * filled.  The buffer ref and the block's handle are held across it, and the
 * slot outlives every turn: nothing reclaims it within a run.
 * The block's handle keeps the descriptor's destructor away, so a working copy
 * is missing here only when the ensure could not allocate one — and a grant
 * whose cohort cannot be served is a hang, never a soft drop. */
static void cxl_fetch_working_copy(struct arts_db_cache_s *cache) {
  uint64_t n = atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  cxl_slot_required(cache, "grant");
  if (n == 0) {
    return; /* a sentinel-sized block has no bytes and no store */
  }
  (void)arts_db_buf_ensure(cache, n);
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf = (struct arts_db_buffer_s *)arts_shared_get(h);
  if (buf == NULL) {
    ARTS_ERROR("cxl: guid %lu has no working copy of %llu bytes to fetch its "
               "grant into",
               (unsigned long)cache->db_guid, (unsigned long long)n);
  }
  cxl_fetch_body(cache, buf);
  arts_db_buf_release(&h);
  atomic_fetch_add_explicit(&arts_cxl_fetches, 1u, memory_order_relaxed);
}

/* Return this rank's turn's bytes to the block's slot.
 * Synchronous on the releasing worker: the zero edge and the cache word's
 * return to IDLE commit in one CAS before this runs, so a local acquire
 * arriving meanwhile parks or requests and cannot self-serve out of a working
 * copy that is being drained — a protection that holds only while the copy
 * completes before the release is committed or sent.
 *
 * PRECONDITION, enforced rather than assumed: the caller holds the turn whose
 * zero edge this is, and the store is the one this block's creator allocated.
 * One block has one store because its creator mints it once. */
static void cxl_purge_working_copy(struct arts_db_cache_s *cache) {
  uint64_t n = atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  cxl_slot_required(cache, "release");
  if (n == 0) {
    return;
  }
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf = (struct arts_db_buffer_s *)arts_shared_get(h);
  if (buf == NULL) {
    /* A holder never gets here, a destroy notwithstanding: only the
     * descriptor's destructor empties a published cache's slot, and the
     * holder's pin defers it.  Nothing is written. */
    arts_db_buf_release(&h);
    return;
  }
  cxl_purge_body(cache, buf);
  arts_db_buf_release(&h);
  atomic_fetch_add_explicit(&arts_cxl_purges, 1u, memory_order_relaxed);
}

#ifdef ARTS_CXL_STAGED
/* Staged residency: the working copy is DRAM, so the bytes are reloaded out of
 * the block's store and copied in.  The consumer flush precedes the read: this
 * rank's private lines may be clean stale copies of the store. */
static void cxl_fetch_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf) {
  uint64_t n = atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  const void *slot = (const void *)(uintptr_t)arts_db_cxl_slot_addr(cache);
  arts_cxl_flush_consumer(slot, (size_t)n);
  memcpy(buf->data, slot, (size_t)n);
}

/* Staged residency: the turn's bytes are copied out of the DRAM working copy
 * and the producer flush follows the write. */
static void cxl_purge_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf) {
  uint64_t n = atomic_load_explicit(&cache->db_size, memory_order_relaxed);
  void *slot = (void *)(uintptr_t)arts_db_cxl_slot_addr(cache);
  memcpy(slot, buf->data, (size_t)n);
  arts_cxl_flush_producer(slot, (size_t)n);
}
#endif /* ARTS_CXL_STAGED */

#ifdef ARTS_CXL_DIRECT
/* The working bytes ARE the block's store here: the fetch's arts_db_buf_ensure
 * adopted the slot, so this descriptor names it and there is nothing to copy.
 * A fetch is the consumer flush that makes this rank's private lines the
 * store's. */
static void cxl_fetch_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf) {
  arts_cxl_flush_consumer(
      buf->data,
      (size_t)atomic_load_explicit(&cache->db_size, memory_order_relaxed));
}

/* A purge is the producer flush that publishes this turn's writes to the
 * store.  It runs before the release is committed or sent, which is what makes
 * it precede every onward admission. */
static void cxl_purge_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf) {
  arts_cxl_flush_producer(
      buf->data,
      (size_t)atomic_load_explicit(&cache->db_size, memory_order_relaxed));
}
#endif /* ARTS_CXL_DIRECT */
#endif /* ARTS_USE_CXL */

/* ===== arts_handler_db_acquire =========================================
 * The coherent acquire of one dep — protocol-agnostic signature; the driver's
 * acquire_one_dep calls it.
 *
 * item is this rank's pre-pinned db_s of the block; args is {edt, db_guid,
 * slot}; mode is read from depv[slot].mode.
 *
 * Single-atom acquire: ONE CAS carries {count++, decision} together (see
 * cache_compute_next in arbiters.c for why the atomicity is load-bearing).
 * The action decides where this acquire is served:
 *   SELF_SERVE  a covering grant is held — serve the dep directly.  It never
 *               touches the pend stacks, which keeps the parked population
 *               equal to the counts a later grant CAS reads.
 *   SEND_*      this acquire opened the round's request: park the waiter
 *               FIRST, then send — by the time any grant for the request can
 *               exist, the opener's node is already drainable.
 *   PARK        covered by the in-flight request: park.  The grant committer
 *               serves exactly the population counted at its grant CAS, so a
 *               node whose push trails the grant is awaited, never lost. */
void arts_handler_db_acquire(void *item, void *args) {
  struct arts_db_s *db = (struct arts_db_s *)item;
  struct arts_ooo_args_db_acquire_s *a =
      (struct arts_ooo_args_db_acquire_s *)args;
  struct arts_edt_s *edt = a->edt;
  unsigned int slot = a->slot;
  struct arts_db_cache_s *cache = &db->cache;
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  arts_db_access_mode_t mode = depv[slot].mode;

#ifndef ARTS_USE_CXL
  if (arts_guid_get_rank(cache->db_guid) == arts_global_rank_id) {
    /* First use of a block whose create took no hold: its storage was left
     * for whoever uses it first, and under this release policy only the home
     * may hold the canonical copy — so if the block has none, this acquiring
     * thread allocates it, on its own node.  A no-op for every other block:
     * the home's buffer exists from create and a requester materializes its
     * own before it asks. */
    (void)arts_db_buf_ensure(
        cache, atomic_load_explicit(&cache->db_size, memory_order_relaxed));
  }
#else
  /* The home materializes nothing here: it is an ordinary participant, and a
   * working copy is placed by the grant's fetch that fills it — this arm's
   * single materialization point. */
#endif /* ARTS_USE_CXL */

  int op = (mode == DB_MODE_RW) ? CACHE_OP_ACQ_RW : CACHE_OP_ACQ_RO;
  uint32_t act;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    next = cache_compute_next(cur, op, &act);
    if (act == CACHE_ACT_LATE) {
      ARTS_ERROR("lock: guid %lu has more %s acquires counted on one rank "
                 "than its cache word can hold (word=%llx)",
                 (unsigned long)cache->db_guid,
                 mode == DB_MODE_RW ? "write" : "read",
                 (unsigned long long)cur);
    }
  } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                  next, memory_order_acq_rel,
                                                  memory_order_acquire));

  if (act == CACHE_ACT_SELF_SERVE) {
    /* Served exactly as a drained waiter is, from the turn this rank holds. */
    uint8_t census = arts_db_census_serving(false);
    mark_edt_ready_by_guid(edt->guid, slot);
    arts_db_census_served(census);
    return;
  }

  /* SEND_* / PARK: park the waiter.  Push BEFORE any send so the node is
   * drainable before a grant for this round can arrive. */
  struct arts_db_excl_waiter_s *w =
      (struct arts_db_excl_waiter_s *)arts_malloc(sizeof(*w));
  w->edt_guid = edt->guid;
  w->slot = slot;
  arts_lf_stack_push(
      (mode == DB_MODE_RW) ? &cache->rw_pending : &cache->ro_pending, &w->link);

  if (act == CACHE_ACT_SEND_RW) {
    arts_send_db_excl_request(cache, DB_MODE_RW, excl_rw_cover(cur));
  } else if (act == CACHE_ACT_SEND_RO) {
    arts_send_db_excl_request(cache, DB_MODE_RO, excl_ro_cover(next));
  }
}

/* ===== arts_handler_db_excl_grant =======================================
 * Cat-C pure body — grant arrived at the requester rank.  payload is the wire
 * packet alone; size is its byte count.
 *
 *   (1) Cat-C route-table lookup (NULL ⇒ DB destroyed concurrently ⇒ drop).
 *   (2) A payload sent one-sided must land BEFORE the CAS so drained waiters
 *       observe it: the commit waits for the write completion.
 *   (3) CAS the single word: RW grant Q→GRANT (action DRAIN_BOTH); RO grant
 *       Q→GRANT (DRAIN_RO).
 *   (4) Run the action after the CAS commits (mirrors the home grant path:
 *       compute decides, the pop/drain/send runs after).
 *
 * The grant committer is the SOLE drainer of the pend stacks: the counts its
 * CAS observed are exactly the parked population (arrivals under the held
 * grant self-serve and never park), and lock_drain_pending consumes exactly
 * that many nodes — serving is exactly-once with no other drain path that
 * could run against a later round. */
#ifdef ARTS_USE_CXL
/* Apply a grant: fetch, then the one CAS that admits the cohort.  The fetch
 * must precede that CAS — once the axis reads GRANT an arriving acquire
 * self-serves out of the working copy — and it is taken only for a grant the
 * CAS will admit under, decided on the word the CAS then compares.  A word
 * that moved meanwhile is re-decided, and a fetch already taken stands.  That
 * holds because the granted axis stays at REQ until this CAS, so nothing
 * admits a holder of the copy in between.  Only a reused label breaks it,
 * its generations not being told apart: a create hold of the label racing a
 * grant meant for its previous block may write the copy before a CAS that
 * then re-decides.  Consumes db_h. */
static void lock_grant_commit(arts_shared_ptr_t db_h, arts_guid_t db_guid,
                              arts_db_access_mode_t mode) {
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  struct arts_db_cache_s *cache = &db->cache;
  int op = (mode == DB_MODE_RW) ? CACHE_OP_GRANT_RW : CACHE_OP_GRANT_RO;
  bool fetched = false;
  uint32_t act;
  uint64_t cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
  uint64_t next;
  for (;;) {
    next = cache_compute_next(cur, op, &act);
    if (!fetched &&
        (act == CACHE_ACT_DRAIN_BOTH || act == CACHE_ACT_DRAIN_RO)) {
      cxl_fetch_working_copy(cache);
      fetched = true;
      cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
      continue;
    }
    if (atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur, next,
                                              memory_order_acq_rel,
                                              memory_order_acquire)) {
      break;
    }
  }
  switch (act) {
  case CACHE_ACT_DRAIN_BOTH: /* an RW turn serves this rank's RW + RO cohort */
    lock_drain_pending(&cache->rw_pending, CACHE_RW_CNT(cur));
    lock_drain_pending(&cache->ro_pending, CACHE_RO_CNT(cur));
    break;
  case CACHE_ACT_DRAIN_RO:
    lock_drain_pending(&cache->ro_pending, CACHE_RO_CNT(cur));
    break;
  case CACHE_ACT_LATE:
    ARTS_ERROR("lock: guid %lu got a %s grant it has no request out for "
               "(word=%llx)",
               (unsigned long)db_guid, mode == DB_MODE_RW ? "write" : "read",
               (unsigned long long)cur);
    break;
  default:
    break;
  }
  arts_shared_release(&db_h);
}
#else
/* Steps (3)+(4) of the grant: the cache_state transition + resulting drain.
 * Factored out so the rendezvous continuation (grant bytes landed) runs the
 * identical commit.  Consumes db_h. */
static void lock_grant_commit(arts_shared_ptr_t db_h, arts_guid_t db_guid,
                              arts_db_access_mode_t mode,
                              const struct arts_rdzv_landing_s *pub) {
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  struct arts_db_cache_s *cache = &db->cache;

  /* Stash home's publish landing for this grant's eventual RW release —
   * BEFORE the CAS that lets local writers run (the single RW releaser
   * consumes it). */
  if (mode == DB_MODE_RW && pub != NULL) {
    cache->home_pub_rdzv = *pub;
  }

  int op = (mode == DB_MODE_RW) ? CACHE_OP_GRANT_RW : CACHE_OP_GRANT_RO;
  uint32_t act;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    next = cache_compute_next(cur, op, &act);
  } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                  next, memory_order_acq_rel,
                                                  memory_order_acquire));

  switch (act) {
  case CACHE_ACT_DRAIN_BOTH: /* RW grant serves this rank's RW + RO cohort */
    lock_drain_pending(&cache->rw_pending, CACHE_RW_CNT(cur));
    lock_drain_pending(&cache->ro_pending, CACHE_RO_CNT(cur));
    break;
  case CACHE_ACT_DRAIN_RO:
    lock_drain_pending(&cache->ro_pending, CACHE_RO_CNT(cur));
    break;
  case CACHE_ACT_LATE:
    ARTS_ERROR("lock: guid %lu got a %s grant it has no request out for "
               "(word=%llx)",
               (unsigned long)db_guid, mode == DB_MODE_RW ? "write" : "read",
               (unsigned long long)cur);
    break;
  default:
    break;
  }

  arts_shared_release(&db_h);
}
#endif /* ARTS_USE_CXL */

#ifndef ARTS_USE_CXL
/* Rendezvous continuation: the grant bytes have fully landed IN PLACE in this
 * rank's stable buffer (EXCL's fixed-address install; no local holder exists
 * while a grant is in flight — the global lock excluded us).  Nothing to
 * install; run the commit. */
struct lock_grant_landed_ctx_s {
  arts_shared_ptr_t db_h;
  arts_guid_t db_guid;
  arts_db_access_mode_t mode;
  struct arts_rdzv_landing_s pub;
};

static void lock_grant_landed_cb(void *arg) {
  struct lock_grant_landed_ctx_s *ctx = (struct lock_grant_landed_ctx_s *)arg;
  if (arts_shared_get(ctx->db_h) != NULL) {
    /* A one-sided payload comes from another rank. */
    uint8_t census = arts_db_census_serving(true);
    lock_grant_commit(ctx->db_h, ctx->db_guid, ctx->mode, &ctx->pub);
    arts_db_census_served(census);
  } else {
    arts_shared_release(&ctx->db_h); /* destroyed mid-flight — drop */
  }
  arts_free(ctx);
}
#endif /* ARTS_USE_CXL */

#ifdef ARTS_USE_CXL
void arts_handler_db_excl_grant(void *payload, size_t size) {
  struct arts_msg_excl_grant_packet_s *p =
      (struct arts_msg_excl_grant_packet_s *)payload;
  arts_db_access_mode_t mode = (arts_db_access_mode_t)p->mode;
  if (size != sizeof(*p) || p->rdzv_txid != 0) {
    ARTS_ERROR("cxl: a grant from rank %u carries a payload; on this arm a "
               "grant carries the block's slot and nothing else",
               p->header.rank);
  }
  arts_shared_ptr_t db_h = arts_route_table_lookup_db(p->db_guid);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  if (db == NULL) {
    arts_shared_release(&db_h); /* destroyed mid-flight: drop, by design */
    return;
  }
  struct arts_db_cache_s *cache = &db->cache;
  /* Hinted first touch: the size may still be unlearned here, and the fetch
   * needs it. */
  arts_db_cache_learn_size(cache, p->data_size);
  /* The grant names the block's slot in the landing's address field, and
   * nothing else: the landing's key, txid and the version are unread here.
   * A rank that created the block, or learned it from an earlier grant,
   * already knows the slot and records nothing.  A zero address is legal for
   * a sentinel-sized block, which has none. */
  (void)arts_db_cxl_slot_record(cache, p->pub.addr);
  uint8_t census = arts_db_census_serving_from(p->header.rank);
  lock_grant_commit(db_h, p->db_guid, mode); /* consumes db_h */
  arts_db_census_served(census);
}
#else
void arts_handler_db_excl_grant(void *payload, size_t size) {
  struct arts_msg_excl_grant_packet_s *p =
      (struct arts_msg_excl_grant_packet_s *)payload;
  arts_db_access_mode_t mode = (arts_db_access_mode_t)p->mode;
  /* No bytes trail a grant: the self-grant names the home's own buffer and
   * a remote grant's payload travels one-sided. */
  assert(size == sizeof(*p));
  (void)size;

  arts_shared_ptr_t db_h = arts_route_table_lookup_db(p->db_guid);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  if (db == NULL) {
    /* Destroyed mid-flight: still consume any pairing so the txid table stays
     * leak-free (EXCL landings are in-place, cookie 0 — nothing to free). */
    arts_db_rdzv_discard_landing(p->rdzv_txid, p->data_size, p->rdzv_cookie);
    arts_shared_release(&db_h);
    return;
  }
  struct arts_db_cache_s *cache = &db->cache;

  /* Hinted first touch: the size may still be unlearned here. */
  arts_db_cache_learn_size(cache, p->data_size);

  struct arts_rdzv_landing_s pub = {p->pub.addr, p->pub.key, p->pub.txid,
                                   p->pub.cookie};

  if (p->rdzv_txid != 0) {
    /* The grant payload travels one-sided into our stable buffer; pair this
     * packet with the write completion (either order), then commit. */
    struct lock_grant_landed_ctx_s *ctx =
        (struct lock_grant_landed_ctx_s *)arts_malloc(sizeof(*ctx));
    ctx->db_h = db_h;
    ctx->db_guid = p->db_guid;
    ctx->mode = mode;
    ctx->pub = pub;
    arts_net_rdzv_expect(p->rdzv_txid, p->data_size, lock_grant_landed_cb, ctx);
    return;
  }

  /* Same-rank or data-less grant: nothing to land; commit. */
  uint8_t census = arts_db_census_serving_from(p->header.rank);
  lock_grant_commit(db_h, p->db_guid, mode, &pub); /* consumes db_h */
  arts_db_census_served(census);
}
#endif /* ARTS_USE_CXL */

/* ===== arts_send_db_excl_release ========================================
 * Send EXCL_RELEASE to the home.  An RW release's bytes travel by PUT ahead
 * of this packet, which carries their size and the landing's {txid, cookie};
 * cv is nonzero only on the landing-less announce, where it is the address of
 * the releaser's heap rendezvous and the home echoes it in the PUBLISH_CTS.
 * Nothing acknowledges a release, and version is always 0.  RO carries none
 * of these.
 * Self-send (home == this rank) routes through the OoO engine so reordering
 * against DB_CREATE is handled identically to the wire path. */
void arts_send_db_excl_release(unsigned int home_rank, arts_guid_t db_guid,
                               arts_db_access_mode_t mode, uint64_t version,
                               uint64_t cv, uint64_t data_size,
                               uint64_t rdzv_txid, uint64_t rdzv_cookie) {
  uint64_t ds = (mode == DB_MODE_RW) ? data_size : 0u;

  if (home_rank == arts_global_rank_id) {
    /* Self-send: route through the OoO engine exactly as the wire RX
     * dispatcher does — HIT runs arts_handler_db_excl_release inline; MISS
     * defers the args until the home db_s is installed.  It carries no
     * bytes: the home's own write release commits in place
     * (lock_send_release_rw), and every other self-sent release is the
     * data-less return of a grant. */
    if (ds != 0u) {
      ARTS_ERROR("lock: a release of guid %lu to this rank's own home carries "
                 "%lu payload bytes",
                 (unsigned long)db_guid, (unsigned long)ds);
    }
    struct arts_ooo_args_db_excl_release_s args = {
        .releaser = arts_global_rank_id,
        .db_guid = db_guid,
        .mode = (uint32_t)mode,
        .data_size = 0,
        .cv = cv,
        .version = version,
        .rdzv_txid = 0,
        .rdzv_cookie = 0,
    };
    arts_ooo_dispatch_or_defer_guid(db_guid, OOO_DB_EXCL_RELEASE, &args,
                                    sizeof(args));
    return;
  }

  /* Remote send: control-only — a dirty RW release PUT its bytes into the
   * grant's home landing before this packet; {rdzv_txid, rdzv_cookie} echo it
   * for pairing. */
  struct arts_msg_excl_release_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_EXCL_RELEASE);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  p.mode = (uint32_t)mode;
  p.pad = 0;
  p.version = version;
  p.cv = cv;
  p.data_size = ds;
  p.rdzv_txid = rdzv_txid;
  p.rdzv_cookie = rdzv_cookie;
  arts_transport_send_async((int)home_rank, (char *)&p, sizeof(p));
}

/* ===== release-edge senders ============================================
 * Called AFTER the release CAS has already moved the state word to IDLE for
 * this phase (so for a self-send, the inline grant handler that follows will
 * CAS the next phase onto an already-IDLE word — no overwrite).
 *
 * Neither edge waits for an acknowledgement.  Under exclusion the home IS the
 * order: it commits and grants only after it has paired the release packet
 * with the payload's write completion, and a request that arrives first simply
 * stays pending because there is no grant to give.  What the payload leg does
 * need is a pin on the source bytes while the one-sided PUT drains them, and
 * that is the PUT's own local-completion callback, not a remote round trip.
 *
 * RW → PUT the dirty bytes into the landing the grant advertised, then send a
 *      control-only release; the buffer ref rides the PUT.
 * RO → data-less notify.
 *
 * The one wait left on this path is the CTS in the announce leg below, which
 * is not an acknowledgement: a hold that never received a grant has no landing
 * address, and there is nowhere to PUT until the home names one. */
static void lock_send_release_rw(struct arts_db_cache_s *cache) {
  unsigned int home = (unsigned int)arts_guid_get_rank(cache->db_guid);
#ifdef ARTS_USE_CXL
  /* The turn's bytes go back to the block's store BEFORE the home hears
   * anything: the home's next act is to grant somebody, and that somebody
   * reads the store.  The home's own turn takes the same two copies every
   * other rank's does, which is why the purge precedes the home-local branch
   * below — a write-back edge that holds only when the home happens to be the
   * writer is not one worth having. */
  cxl_purge_working_copy(cache);
  if (home == arts_global_rank_id) {
    lock_release_commit(arts_db_of_cache(cache), cache, DB_MODE_RW);
    return;
  }
  INCREMENT_NUM_PURGE_RETURN_BY(1);
  arts_send_db_excl_release(home, cache->db_guid, DB_MODE_RW, /*version=*/0u,
                            /*cv=*/0u, /*data_size=*/0u,
                            /*rdzv_txid=*/0u, /*rdzv_cookie=*/0u);
  return;
#else
  if (home == arts_global_rank_id) {
    /* Home-local RW release: this rank IS home, so the releaser already holds
     * home's authoritative buffer (its in-place writes have landed) and the
     * caller keeps the descriptor pinned across this call.  There is nothing to
     * ship and no ACK to await — apply the home lock_state commit + onward
     * grant directly on the pinned cache, rather than through a GUID-keyed
     * self-send that would re-resolve the DB via its route slot.  A release
     * provably follows a successful acquire, so it never needs the OoO
     * before-create deferral that the request path relies on. */
    lock_release_commit(arts_db_of_cache(cache), cache, DB_MODE_RW);
    return;
  }
  INCREMENT_NUM_PURGE_RETURN_BY(1);
  arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf =
      (struct arts_db_buffer_s *)arts_shared_get(buf_h);
  const void *data = (buf != NULL) ? buf->data : NULL;
  uint64_t ds = (buf != NULL) ? atomic_load_explicit(&cache->db_size,
                                                     memory_order_relaxed)
                              : 0u;
  /* Consume this grant's home publish landing (1:1 grant:release). */
  struct arts_rdzv_landing_s pub = cache->home_pub_rdzv;
  cache->home_pub_rdzv = (struct arts_rdzv_landing_s){0, 0, 0, 0};
  if (home != arts_global_rank_id && ds > 0u && pub.txid == 0) {
    /* Dirty release from a hold that never received a grant (the
     * creator-seeded RW grant): no cached landing — run the announce leg.
     * Heap rendezvous, deliberately leaked on the shutdown escape (a late
     * CTS writes and posts through the echoed cv). */
    struct arts_db_pub_rendezvous_s *wr =
        (struct arts_db_pub_rendezvous_s *)arts_malloc(sizeof(*wr));
    sem_init(&wr->sem, 0, 0);
    wr->landing = (struct arts_rdzv_landing_s){0, 0, 0, 0};
    arts_send_db_excl_release(home, cache->db_guid, DB_MODE_RW,
                              /*version=*/0u, (uint64_t)(uintptr_t)wr, ds,
                              /*rdzv_txid=*/0u, /*rdzv_cookie=*/0u);
    if (!arts_db_await_ack(&wr->sem)) {
      /* Shutdown escape: the round is abandoned with the runtime.  A CTS
       * reply may still be writing the landing and posting through wr, so wr
       * is leaked and nothing in it is read. */
      arts_db_buf_release(&buf_h);
      return;
    }
    /* The post orders the reply's landing stores before these reads, and a
     * CTS reply always carries a landing. */
    assert(wr->landing.txid != 0);
    /* buf_h transfers into the PUT's local completion, which pins the source
     * bytes until the fabric drains them.  The commit carries cv 0 and
     * nothing acknowledges it: the home pairs {packet, write completion} on
     * its own side and commits + grants from there, so waiting here would
     * only serve the pin the callback already holds. */
    arts_net_put_payload((int)home, wr->landing.addr, wr->landing.key,
                         wr->landing.txid, data, ds, arts_db_buf_ref_release_cb,
                         (void *)buf_h);
    arts_send_db_excl_release(home, cache->db_guid, DB_MODE_RW,
                              /*version=*/0u, /*cv=*/0u, ds, wr->landing.txid,
                              wr->landing.cookie);
    sem_destroy(&wr->sem);
    arts_free(wr);
    return;
  }
  if (home != arts_global_rank_id && ds > 0u && pub.txid != 0) {
    /* Remote dirty release: PUT the dirty bytes straight into home's stable
     * buffer (the landing advertised in the grant) — the global RW lock
     * excludes every reader while they fly — then send the control-only
     * release packet; home pairs {packet, write completion} before it commits
     * and grants onward.  The releaser does not wait: ordering is the home's
     * lock, not an acknowledgement, and a request that arrives before the
     * release simply stays pending because there is no grant to give.  buf_h
     * transfers into the PUT's local completion, which is what pins the source
     * bytes until the fabric drains them. */
    arts_net_put_payload((int)home, pub.addr, pub.key, pub.txid, data, ds,
                         arts_db_buf_ref_release_cb, (void *)buf_h);
    arts_send_db_excl_release(home, cache->db_guid, DB_MODE_RW, /*version=*/0u,
                              /*cv=*/0u, ds, pub.txid, pub.cookie);
    return; /* buf_h transferred to the PUT */
  }
  /* Data-less release: nothing is in flight from this buffer, so the send
   * carries the whole release and the local ref drops here. */
  arts_send_db_excl_release(home, cache->db_guid, DB_MODE_RW, /*version=*/0u,
                            /*cv=*/0u, /*data_size=*/0u, /*rdzv_txid=*/0u,
                            /*rdzv_cookie=*/0u);
  arts_db_buf_release(&buf_h);
#endif /* ARTS_USE_CXL */
}

static void lock_send_release_ro(struct arts_db_cache_s *cache) {
  unsigned int home = (unsigned int)arts_guid_get_rank(cache->db_guid);
  if (home == arts_global_rank_id) {
    /* Home-local RO release: commit the home lock_state transition + onward
     * grant directly on the pinned cache, as the RW path does. */
    lock_release_commit(arts_db_of_cache(cache), cache, DB_MODE_RO);
    return;
  }
  INCREMENT_NUM_PURGE_RETURN_BY(1);
  arts_send_db_excl_release(home, cache->db_guid, DB_MODE_RO, /*version=*/0u,
                            /*cv=*/0u, /*data_size=*/0u, /*rdzv_txid=*/0u,
                            /*rdzv_cookie=*/0u);
}

/* ===== arts_db_release_rw / arts_db_release_ro =========================
 * Single-word release: CAS the count down (+ the 0-edge state transition,
 * atomic together) via cache_compute_next, then run the release action it
 * returns.  CACHE_ACT_REL_RW → publish (the last holder of an RW grant, incl.
 * the last RO joiner under it); CACHE_ACT_REL_RO → notify (last RO holder).
 *
 * A count of 0 means this rank holds nothing to release — skip rather than
 * underflow.
 */
/* A create's hold is this arm's ordinary write hold, taken when the block was
 * made; the bytes under it are the cache's own, so the release needs no
 * pointer to them. */
void arts_db_release_created(struct arts_db_cache_s *cache) {
  arts_db_release_rw(cache, NULL);
}

void arts_db_release_rw(struct arts_db_cache_s *cache, void *payload) {
  (void)payload;
  uint32_t act;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    if (CACHE_RW_CNT(cur) == 0u) {
      return; /* nothing held */
    }
    next = cache_compute_next(cur, CACHE_OP_REL_RW, &act);
  } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                  next, memory_order_acq_rel,
                                                  memory_order_acquire));
  if (act == CACHE_ACT_REL_RW) {
    lock_send_release_rw(cache);
  }
}

void arts_db_release_ro(struct arts_db_cache_s *cache) {
  uint32_t act;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    if (CACHE_RO_CNT(cur) == 0u) {
      return; /* nothing held */
    }
    next = cache_compute_next(cur, CACHE_OP_REL_RO, &act);
  } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                  next, memory_order_acq_rel,
                                                  memory_order_acquire));
  if (act == CACHE_ACT_REL_RW) {
    lock_send_release_rw(
        cache); /* last RO joiner under an RW grant → publish */
  } else if (act == CACHE_ACT_REL_RO) {
    lock_send_release_ro(cache);
  }
}
