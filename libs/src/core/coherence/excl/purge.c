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

#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arts/coherence/buffer.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/handlers.h"
#include "arts/coherence/directory.h"
#include "arts/db.h"
#include "arts/edt.h"
#include "arts/fam/pool.h"
#include "arts/gas/route_table.h"
#include "arts/ooo.h"
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/system/identity.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/transport/net.h"
#include "arts/transport/protocol.h"
#include "arts/utils/atomics.h"
#include "arts/utils/malloc.h"

/* ===== RO waiter node (rank-granular RO queue entry) ===================
 * Mirrors the definition in retain.c (both TUs include excl/types.h which does
 * not define this node — it is a file-local type used only by home-side grant
 * and teardown logic in this TU, and by arts_send_db_excl_grant here which
 * calls arts_handler_db_excl_grant as a local-hit self-send). */
struct arts_lock_ro_node_s {
  arts_lf_link_t link; /* FIRST */
  unsigned int rank;
  struct arts_rdzv_landing_s rdzv; /* requester's grant landing */
};

/* ===== Pure state-transition arbiters ==================================
 * excl_compute_next and cache_compute_next are pure functions (no external
 * calls, no global state).  They live in arbiters.c so the unit test
 * (tests/unit/excl_compute_next.c) can #include just the arbiters without
 * pulling in the full handler bodies. */
#include "arbiters.c"
#include "arts/counter/Preamble.h"

/* ===== Grant dispatcher (shared by request + release handlers) ========= */

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
#ifdef ARTS_FAM
  /* The grant carries the block's store, never its bytes: the home holds no
   * canonical copy on this arm, and every grantee reads the store itself.
   * The landing's address field names it — the only thing that travels, since
   * the owner is a function of the address — and txid 0 says nothing is in
   * flight. */
  struct arts_rdzv_landing_s pub = {arts_db_fam_slot_addr(cache), 0, 0, 0};
  uint64_t data_size = cache->db_size;
  if (grant == LOCK_GRANT_ONE_RW) {
    unsigned int rank;
    struct arts_rdzv_landing_s rdzv;
    if (arts_home_grantreq_queue_pop(&db->rw_waiters, &rank, &rdzv, NULL)) {
      arts_send_db_excl_grant(rank, cache->db_guid, DB_MODE_RW, /*version=*/0u,
                              &rdzv, &pub, /*src_h=*/NULL, data_size);
    }
  } else { /* LOCK_GRANT_ALL_RO */
    arts_lf_link_t *node = arts_lf_stack_drain(&db->ro_waiters);
    while (node != NULL) {
      arts_lf_link_t *nx =
          atomic_load_explicit(&node->next, memory_order_relaxed);
      struct arts_lock_ro_node_s *rn =
          ARTS_CONTAINER_OF(node, struct arts_lock_ro_node_s, link);
      arts_send_db_excl_grant(rn->rank, cache->db_guid, DB_MODE_RO,
                              /*version=*/0u, &rn->rdzv, &pub, /*src_h=*/NULL,
                              data_size);
      arts_free(rn);
      node = nx;
    }
  }
#else
  /* A first use of a block whose create took no hold: the grant about to
   * leave carries the block's bytes, and under this release policy the home
   * is the one rank entitled to say what they are — so materialize them here
   * when the block has no storage yet.  A no-op for every other block. */
  (void)arts_db_buf_ensure(cache, cache->db_size);
  arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf =
      (struct arts_db_buffer_s *)arts_shared_get(buf_h);
  /* The size is descriptor state — known even before any payload exists;
   * the grant carries it so a hinted (CTS-skipping) first touch learns it. */
  uint64_t data_size = cache->db_size;
  /* The grant carries the home buffer's own version.  No separate counter is
   * needed: home commits (RW publish installs) are sequentialized by the
   * protocol (RW single-owner inter-node + ACK-gated release), so buf->version
   * is monotone across rounds. */
  uint64_t version = buf ? buf->version : 0;
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
      arts_send_db_excl_grant(rank, cache->db_guid, DB_MODE_RW, version, &rdzv,
                              &pub, buf_h, data_size);
      buf_h = NULL; /* consumed by the grant sender */
    }
  } else { /* LOCK_GRANT_ALL_RO: drain ro_waiters, fan-out to each rank. */
    arts_lf_link_t *node = arts_lf_stack_drain(&db->ro_waiters);
    while (node != NULL) {
      arts_lf_link_t *nx =
          atomic_load_explicit(&node->next, memory_order_relaxed);
      struct arts_lock_ro_node_s *rn =
          ARTS_CONTAINER_OF(node, struct arts_lock_ro_node_s, link);
      /* Each fan-out PUT pins the source with its own strong ref. */
      arts_send_db_excl_grant(rn->rank, cache->db_guid, DB_MODE_RO, version,
                              &rn->rdzv, NULL,
                              (buf != NULL) ? arts_shared_copy(buf_h) : NULL,
                              data_size);
      arts_free(rn);
      node = nx;
    }
  }
  if (buf_h != NULL) {
    arts_db_buf_release(&buf_h);
  }
#endif /* ARTS_FAM */
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
     * buffer.  The requester's advertised landing goes unused — for EXCL it is
     * the stable buffer itself, so there is nothing to recycle. */
    if (src != NULL) {
      arts_db_buf_release(&src_h);
    }
    p.header.size = sizeof(p);
    arts_handler_db_excl_grant(&p, sizeof(p));
    return;
  }
#ifdef ARTS_FAM
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
   * completion, so a concurrent publish recycling the buffer cannot free
   * the bytes mid-read. */
  uint64_t ds = data_size;
  p.data_size = ds;
  p.rdzv_txid = req_rdzv->txid;
  arts_net_put_payload((int)requester_rank, req_rdzv->addr, req_rdzv->key,
                       req_rdzv->txid, src->data, ds,
                       arts_db_buf_ref_release_cb, (void *)src_h);
  arts_transport_send_async((int)requester_rank, (char *)&p, sizeof(p));
#endif /* ARTS_FAM */
}

/* ===== arts_handler_db_excl_request ==================================== */

void arts_handler_db_excl_request(void *item_v, void *args_v) {
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  struct arts_db_cache_s *cache = &db->cache;
  struct arts_ooo_args_db_excl_request_s *a =
      (struct arts_ooo_args_db_excl_request_s *)args_v;
  unsigned int requester = a->requester;
  arts_db_access_mode_t mode = (arts_db_access_mode_t)a->mode;

#ifndef ARTS_FAM
  if (a->rdzv.txid == 0 && cache->db_size > 0 && arts_global_rank_count > 1) {
    /* First-touch request without a landing: the requester did not know
     * db_size.  Answer with the size (CTS) and do NOT enqueue — the grant
     * plane requires a landing.  The requester re-issues with one. */
    arts_send_db_excl_cts(requester, cache->db_guid, cache->db_size,
                          (uint32_t)mode);
    return;
  }
#else
  /* On the arm whose grants carry no payload a request advertises no landing,
   * so its absence says nothing about what the requester knows: the size
   * travels in the grant, which the grantee records before it reads. */
#endif

  /* (1) push-before-CAS: enqueue this requester rank in its mode's queue
   * BEFORE reading lock_state, so the CAS transition already sees this
   * participant counted. */
  if (mode == DB_MODE_RW) {
    arts_home_grantreq_queue_push(&db->rw_waiters, requester, &a->rdzv,
                                  ARTS_GRANT_VERSION_NONE);
  } else {
    struct arts_lock_ro_node_s *n =
        (struct arts_lock_ro_node_s *)arts_malloc(sizeof(*n));
    n->rank = requester;
    n->rdzv = a->rdzv;
    arts_lf_stack_push(&db->ro_waiters, &n->link);
  }
  arts_rank_bitset_set(&db->cached_ranks,
                       requester); /* destroy fan-out roster */

  /* (2) read -> compute next -> CAS retry on contention. */
  int op = (mode == DB_MODE_RW) ? EXCL_OP_RW_ACQ : EXCL_OP_RO_ACQ;
  uint32_t grant;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&db->lock_state, memory_order_acquire);
    next = excl_compute_next(cur, op, &grant);
  } while (!atomic_compare_exchange_weak_explicit(
      &db->lock_state, &cur, next, memory_order_acq_rel, memory_order_acquire));

  /* (3) grant per the transition case. */
  lock_home_grant(db, cache, grant);
}

/* ===== arts_handler_db_excl_release ==================================== */

/* Steps (2)+(3) of the release: the lock_state transition + next grant.
 * Factored out so the rendezvous continuation (dirty bytes landed) runs the
 * identical commit. */
static void lock_release_commit(struct arts_db_s *db,
                                struct arts_db_cache_s *cache,
                                arts_db_access_mode_t mode) {
  int op = (mode == DB_MODE_RW) ? EXCL_OP_RW_REL : EXCL_OP_RO_REL;
  uint32_t grant;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&db->lock_state, memory_order_acquire);
    next = excl_compute_next(cur, op, &grant);
  } while (!atomic_compare_exchange_weak_explicit(
      &db->lock_state, &cur, next, memory_order_release, memory_order_acquire));
  lock_home_grant(db, cache, grant);
}

#ifndef ARTS_FAM
/* Rendezvous continuation for a committed remote RW release: the dirty bytes
 * have fully landed IN PLACE in home's stable buffer ("imm seen => buffer
 * valid"; the global RW lock excluded every reader while they flew), so there
 * is nothing to install — ACK the blocked releaser, then transition + grant. */
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
#endif /* ARTS_FAM */

void arts_handler_db_excl_release(void *item_v, void *args_v) {
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  struct arts_db_cache_s *cache = &db->cache;
  struct arts_ooo_args_db_excl_release_s *a =
      (struct arts_ooo_args_db_excl_release_s *)args_v;
  arts_db_access_mode_t mode = (arts_db_access_mode_t)a->mode;

#ifdef ARTS_FAM
  /* A release returns a right and nothing else: the turn's bytes reached the
   * block's store before the message left the releaser, so there is nothing
   * here to announce, to pair with a write completion, or to install. */
  if (a->data_size != 0u) {
    ARTS_ERROR("fam: a release from rank %u carries a payload; on this arm a "
               "release carries the right it returns and nothing else",
               a->releaser);
  }
#else
  if (mode == DB_MODE_RW && a->data_size > 0 && a->data_inline == 0 &&
      a->rdzv_txid == 0) {
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
  if (mode == DB_MODE_RW && a->data_size > 0 && a->data_inline == 0 &&
      a->rdzv_txid != 0) {
    /* Remote dirty release: the publish PUT straight into home's stable
     * buffer (the landing advertised in this grant).  Pair the release packet
     * with the write completion; ACK + transition + grant run only once the
     * bytes are fully placed. */
    struct lock_release_landed_ctx_s *ctx =
        (struct lock_release_landed_ctx_s *)arts_malloc(sizeof(*ctx));
    ctx->db_h = arts_route_table_lookup_db(cache->db_guid);
    ctx->db_guid = a->db_guid;
    arts_net_rdzv_expect(a->rdzv_txid, lock_release_landed_cb, ctx);
    return;
  }

  /* (1) RW same-rank: write the publish into home's stable buffer in place,
   * then ACK.  Under exclusive-lock serialization the releaser held the sole
   * RW grant and home grants the next holder only after this publish
   * completes, so no reader is touching the buffer here — the in-place
   * overwrite is safe and the buffer address stays fixed (preserving DBs with
   * internal self-pointers).  No versioning: home commits are already
   * sequentialized by the protocol (RW single-owner + ACK-gated release). */
  if (mode == DB_MODE_RW && a->data_size > 0 && a->data_inline != 0) {
    const void *data = (const char *)a + sizeof(*a);
    arts_db_buf_write_inplace(cache, data, a->data_size);
  }
#endif /* ARTS_FAM */

  /* (2)+(3) transition + grant. */
  lock_release_commit(db, cache, mode);
}

/* A destroy does not tear the home down on arrival: under exclusion the home
 * may still hold the lock for a holder whose release is in flight, and that
 * release carries both a counter decrement and a payload PUT aimed at this
 * generation.  Tearing down first is what lets a stale release land on the
 * NEXT generation of a labeled GUID.  So the destroy MARKS the state word and
 * whichever of it and the last release reaches the zero edge performs the
 * teardown; marking and reading the counts commit in one CAS, so neither can
 * conclude the other will do it.
 *
 * The mark delays the slot's return, and no legitimate create can collide
 * with that window: an ordinary GUID is minted from a monotonic sequence and
 * never comes back, and re-creating a labeled GUID across a lifetime
 * boundary is the reuse case ARTS does not support
 * (docs/programming_model/guids.rst). */
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
 * pending_snapshot, home-directory init).
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
  }
  atomic_store_explicit(&c->cache_state, seed, memory_order_relaxed);
  arts_lf_stack_init(&c->ro_pending);
  arts_lf_stack_init(&c->rw_pending);
#ifndef ARTS_FAM
  c->home_pub_rdzv = (struct arts_rdzv_landing_s){0, 0, 0, 0};
#endif
  arts_db_cache_common_init(c, db_guid, db_size, kind, creator_rank);
}

/* ===== arts_db_cache_destructor =========================================
 * free parked nodes only — destroying an in-use DB is undefined (OCR), no
 * wake.
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
#ifndef ARTS_FAM
/* Materialize this rank's stable buffer (EXCL's fixed-address backing store)
 * and advertise it as the grant landing: the grant PUT installs IN PLACE,
 * preserving the address across the DB's whole lifetime.  A fresh txid is
 * drawn per request (each request is served by at most one grant). */
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
    arts_db_buf_release(&h);
    return false;
  }
  out->txid = arts_net_rdzv_txid_next();
  out->cookie = 0; /* in-place: this rank finds the buffer via its cache */
  arts_db_buf_release(&h);
  return true;
}
#endif /* ARTS_FAM */

void arts_send_db_excl_request(struct arts_db_cache_s *cache,
                               arts_db_access_mode_t mode) {
  arts_guid_t db_guid = cache->db_guid;
  unsigned int home_rank = arts_guid_get_rank(db_guid);
  /* Zeroed outside every conditional: no arm may reach the send with stack
   * garbage where a landing would be. */
  struct arts_rdzv_landing_s rdzv = {0, 0, 0, 0};
#ifndef ARTS_FAM
  (void)lock_stable_landing(cache, &rdzv);
#endif
  if (home_rank == arts_global_rank_id) {
    /* Self-send: route through the OoO engine so before-create reorders are
     * handled correctly (the engine defers when the slot is absent). */
    struct arts_ooo_args_db_excl_request_s args = {
        .requester = arts_global_rank_id,
        .db_guid = db_guid,
        .mode = (uint32_t)mode,
        .rdzv = rdzv,
    };
    arts_ooo_dispatch_or_defer_guid(db_guid, OOO_DB_EXCL_REQUEST, &args,
                                    sizeof(args));
    return;
  }
  struct arts_msg_excl_request_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_EXCL_REQUEST);
  p.db_guid = db_guid;
  p.mode = (uint32_t)mode;
  p.pad = 0;
  p.rdzv.addr = rdzv.addr;
  p.rdzv.key = rdzv.key;
  p.rdzv.txid = rdzv.txid;
  p.rdzv.cookie = rdzv.cookie;
  arts_transport_send_async((int)home_rank, (char *)&p, sizeof(p));
}

/* EXCL_CTS sender (home → first-touch requester) + requester-side body. */
void arts_send_db_excl_cts(unsigned int requester_rank, arts_guid_t db_guid,
                           uint64_t db_size, uint32_t mode) {
  INCREMENT_NUM_EXCL_SIZE_CTS_BY(1);
  struct arts_msg_excl_cts_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_EXCL_CTS);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  p.db_size = db_size;
  p.mode = mode;
  p.pad = 0;
  if (requester_rank == arts_global_rank_id) {
    arts_shared_ptr_t h = arts_route_table_lookup_db(db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_excl_cts(db, &p);
    }
    arts_shared_release(&h);
    return;
  }
  arts_transport_send_async((int)requester_rank, (char *)&p, sizeof(p));
}

void arts_handler_db_excl_cts(void *item_v, void *args_v) {
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_msg_excl_cts_packet_s *p =
      (struct arts_msg_excl_cts_packet_s *)args_v;
  arts_db_cache_note_answered(cache); /* before the answer takes effect */
  if (cache->db_size == 0) {
    cache->db_size = p->db_size;
  }
  arts_send_db_excl_request(cache, (arts_db_access_mode_t)p->mode);
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
 * that order and in one call; it does NOT touch the count (the count was
 * carried by the waiter's acquire CAS — "counted ⟹ will park"). */
static void lock_drain_pending(arts_lf_stack_t *q, uint32_t expected) {
  uint32_t served = 0;
  while (served < expected) {
    arts_lf_link_t *node = arts_lf_stack_drain(q);
    while (node != NULL) {
      arts_lf_link_t *nx =
          atomic_load_explicit(&node->next, memory_order_relaxed);
      struct arts_db_excl_waiter_s *w =
          ARTS_CONTAINER_OF(node, struct arts_db_excl_waiter_s, link);
      /* Re-derive dep->ptr from the installed buffer, let the serialized walk
       * past this slot, and account (which may schedule the EDT when
       * acquire_remaining reaches 0). */
      mark_edt_ready_by_guid(w->edt_guid, w->slot);
      arts_free(w);
      served++;
      node = nx;
    }
  }
}

/* The hold is this arm's own: one CAS of the cache word, through the arbiter
 * like every other transition of it, and then the actions that transition
 * owes.  A read this rank had already asked for is covered by the turn the
 * create takes (RW ⊇ RO) and is served here, exactly as a granted RW phase
 * serves its RO cohort.  The home's directory is not touched from here — it
 * comes from the announce, where arts_db_home_init names the creator. */
bool arts_db_create_take_hold(struct arts_db_cache_s *cache) {
  uint32_t act;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    next = cache_compute_next(cur, CACHE_OP_CREATE_HOLD, &act);
    if (next == cur) {
      return false; /* the word carries the block already */
    }
  } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                  next, memory_order_acq_rel,
                                                  memory_order_acquire));
  if (act == CACHE_ACT_DRAIN_BOTH) {
    /* The counts are the pre-image's: the population this transition owes,
     * exactly as a GRANT_RW landing drains it (the creator's own +1 is not
     * parked). */
    lock_drain_pending(&cache->rw_pending, CACHE_RW_CNT(cur));
    lock_drain_pending(&cache->ro_pending, CACHE_RO_CNT(cur));
  }
  return true;
}

#ifdef ARTS_FAM
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
uint64_t arts_fam_fetches;
uint64_t arts_fam_purges;

/* The residency seam: declared once here, defined exactly once per residency,
 * and called only from the two shared funnels below — which have already
 * validated the store, taken the buffer ref and read the size. */
static void fam_fetch_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf);
static void fam_purge_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf);
#if !defined(ARTS_FAM_STAGED) && !defined(ARTS_FAM_DIRECT)
#error "a FAM build names a residency"
#endif

/* A SIZED block must know its store address.  A create records the
 * store before the block can be asked for and every grant names it, so a zero
 * here is a create that skipped its allocation or a grant that carried none —
 * the same defect, with the same answer, on either residency.  It lives in the
 * shared part for that reason: the slot rule is what it checks, not a funnel.
 * A block whose declared size is 0 is exempt: a sentinel has no bytes, so it
 * has no store and a grant for it carries address 0 legitimately. */
static void fam_slot_required(const struct arts_db_cache_s *cache,
                              const char *edge) {
  if (arts_db_fam_slot_addr(cache) == 0 && cache->db_size != 0) {
    ARTS_ERROR("fam: guid %lu has no slot at its %s edge",
               (unsigned long)cache->db_guid, edge);
  }
}

/* Bring the block's canonical bytes under this rank's grant.  Runs before the
 * grant's admitting CAS, while the granted axis still rests at REQ, so every
 * acquire of it is parked and nothing reads or writes the working copy being
 * filled.  The buffer ref and the block's handle are held across it, and the
 * store cannot be freed under it because this rank has been counted at the
 * home from the moment it asked, and the home frees only at a zero edge.
 * The block's handle keeps the descriptor's destructor away, so a working copy
 * is missing here only when the ensure could not allocate one — and a grant
 * whose cohort cannot be served is a hang, never a soft drop. */
static void fam_fetch_working_copy(struct arts_db_cache_s *cache) {
  uint64_t n = cache->db_size;
  fam_slot_required(cache, "grant");
  if (n == 0) {
    return; /* a sentinel-sized block has no bytes and no store */
  }
  (void)arts_db_buf_ensure(cache, n);
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf = (struct arts_db_buffer_s *)arts_shared_get(h);
  if (buf == NULL) {
    ARTS_ERROR("fam: guid %lu has no working copy of %llu bytes to fetch its "
               "grant into",
               (unsigned long)cache->db_guid, (unsigned long long)n);
  }
  fam_fetch_body(cache, buf);
  arts_db_buf_release(&h);
  __atomic_fetch_add(&arts_fam_fetches, 1u, __ATOMIC_RELAXED);
}

/* Return this rank's turn's bytes to the block's canonical store.
 * Synchronous on the releasing worker: the zero edge and the cache word's
 * return to IDLE commit in one CAS before this runs, so a local acquire
 * arriving meanwhile parks or requests and cannot self-serve out of a working
 * copy that is being drained — a protection that holds only while the copy
 * completes before the release is committed or sent.
 *
 * PRECONDITION, enforced rather than assumed: the caller holds the turn whose
 * zero edge this is, and the store is the one this block's creator allocated.
 * One label has one store because a second address for one GUID aborts where
 * it is recorded. */
static void fam_purge_working_copy(struct arts_db_cache_s *cache) {
  uint64_t n = cache->db_size;
  fam_slot_required(cache, "release");
  if (n == 0) {
    return;
  }
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf = (struct arts_db_buffer_s *)arts_shared_get(h);
  if (buf == NULL) {
    /* The route slot is gone under a holder that still owed its release: the
     * documented boundary.  Nothing is written. */
    arts_db_buf_release(&h);
    return;
  }
  fam_purge_body(cache, buf);
  arts_db_buf_release(&h);
  __atomic_fetch_add(&arts_fam_purges, 1u, __ATOMIC_RELAXED);
}

#ifdef ARTS_FAM_STAGED
/* Staged residency: the working copy is DRAM, so the bytes are reloaded out of
 * the block's store and copied in.  The consumer flush precedes the read: this
 * rank's private lines may be clean stale copies of the store. */
static void fam_fetch_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf) {
  uint64_t n = cache->db_size;
  const void *slot = (const void *)(uintptr_t)arts_db_fam_slot_addr(cache);
  arts_fam_flush_consumer(slot, (size_t)n);
  memcpy(buf->data, slot, (size_t)n);
}

/* Staged residency: the turn's bytes are copied out of the DRAM working copy
 * and the producer flush follows the write. */
static void fam_purge_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf) {
  uint64_t n = cache->db_size;
  void *slot = (void *)(uintptr_t)arts_db_fam_slot_addr(cache);
  memcpy(slot, buf->data, (size_t)n);
  arts_fam_flush_producer(slot, (size_t)n);
}
#endif /* ARTS_FAM_STAGED */

#ifdef ARTS_FAM_DIRECT
/* The working bytes ARE the block's store here: the fetch's arts_db_buf_ensure
 * adopted the slot, so this descriptor names it and there is nothing to copy.
 * A fetch is the consumer flush that makes this rank's private lines the
 * store's. */
static void fam_fetch_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf) {
  arts_fam_flush_consumer(buf->data, (size_t)cache->db_size);
}

/* A purge is the producer flush that publishes this turn's writes to the
 * store.  It runs before the release is committed or sent, which is what makes
 * it precede every onward admission. */
static void fam_purge_body(struct arts_db_cache_s *cache,
                           struct arts_db_buffer_s *buf) {
  arts_fam_flush_producer(buf->data, (size_t)cache->db_size);
}
#endif /* ARTS_FAM_DIRECT */
#endif /* ARTS_FAM */

/* ===== arts_handler_db_acquire =========================================
 * OOO_DB_ACQUIRE Cat-B body — protocol-agnostic signature.
 *
 * item is the pre-pinned home db_s; args is {edt, db_guid, slot}; mode is read
 * from depv[slot].mode.
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

#ifndef ARTS_FAM
  if (arts_guid_get_rank(cache->db_guid) == arts_global_rank_id) {
    /* First use of a block whose create took no hold: its storage was left
     * for whoever uses it first, and under this release policy only the home
     * may hold the canonical copy — so if the block has none, this acquiring
     * thread allocates it, on its own node.  A no-op for every other block:
     * the home's buffer exists from create and a requester materializes its
     * own before it asks. */
    (void)arts_db_buf_ensure(cache, cache->db_size);
  }
#else
  /* The home materializes nothing here: it is an ordinary participant, and a
   * working copy is placed by the grant's fetch that fills it — this arm's
   * single materialization point. */
#endif /* ARTS_FAM */

  int op = (mode == DB_MODE_RW) ? CACHE_OP_ACQ_RW : CACHE_OP_ACQ_RO;
  uint32_t act;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    next = cache_compute_next(cur, op, &act);
  } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                  next, memory_order_acq_rel,
                                                  memory_order_acquire));

  if (act == CACHE_ACT_SELF_SERVE) {
    /* Served exactly as a drained waiter is.  The turn was answered from what
     * this rank already holds, so it joins the same census the other arms
     * feed. */
    INCREMENT_NUM_DB_ACQUIRE_LOCAL_HIT_BY(1);
    mark_edt_ready_by_guid(edt->guid, slot);
    return;
  }
  INCREMENT_NUM_DB_ACQUIRE_REMOTE_BY(1);

  /* SEND_* / PARK: park the waiter.  Push BEFORE any send so the node is
   * drainable before a grant for this round can arrive. */
  struct arts_db_excl_waiter_s *w =
      (struct arts_db_excl_waiter_s *)arts_malloc(sizeof(*w));
  w->edt_guid = edt->guid;
  w->slot = slot;
  arts_lf_stack_push(
      (mode == DB_MODE_RW) ? &cache->rw_pending : &cache->ro_pending, &w->link);

  if (act == CACHE_ACT_SEND_RW) {
    arts_send_db_excl_request(cache, DB_MODE_RW);
  } else if (act == CACHE_ACT_SEND_RO) {
    arts_send_db_excl_request(cache, DB_MODE_RO);
  }
}

/* ===== arts_handler_db_excl_grant =======================================
 * Cat-C pure body — grant arrived at the requester rank.  payload is the full
 * contiguous wire buffer (header + db_size data bytes); size is the byte count.
 *
 *   (1) Cat-C route-table lookup (NULL ⇒ DB destroyed concurrently ⇒ drop).
 *   (2) Install the grant data BEFORE the CAS so drained waiters observe it.
 *   (3) CAS the single word: RW grant Q→GRANT (action DRAIN_BOTH); RO grant
 *       Q→GRANT (DRAIN_RO) or, when rc==0 (the RO waiters were already served
 *       by an RW grant — RW⊇RO), a phantom Q→IDLE that is returned to home at
 *       once (REL_RO).
 *   (4) Run the action after the CAS commits (mirrors the home grant path:
 *       compute decides, the pop/drain/send runs after).
 *
 * The grant committer is the SOLE drainer of the pend stacks: the counts its
 * CAS observed are exactly the parked population (arrivals under the held
 * grant self-serve and never park), and lock_drain_pending consumes exactly
 * that many nodes — serving is exactly-once with no other drain path that
 * could run against a later round. */
#ifdef ARTS_FAM
/* Apply a grant: fetch, then the one CAS that admits the cohort.  The fetch
 * must precede that CAS — once the axis reads GRANT an arriving acquire
 * self-serves out of the working copy — and it is taken only for a grant the
 * CAS will admit under, decided on the word the CAS then compares.  A word
 * that moved meanwhile is re-decided, and a fetch already taken stands.  That
 * holds inside the labeled-GUID contract only: there the granted axis stays at
 * REQ until this CAS, so nothing admits a holder of the copy in between; a
 * create hold of the same GUID racing a grant of an earlier generation is
 * undefined, and may write the copy before a CAS that then re-decides.
 * A grant that admits nobody goes back to the home with nothing fetched.
 * Consumes db_h. */
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
      fam_fetch_working_copy(cache);
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
  case CACHE_ACT_REL_RW_EMPTY:
    if (CACHE_RW_ST(cur) == CACHE_ST_REQ && CACHE_RW_ST(next) == CACHE_ST_IDLE &&
        CACHE_RW_CNT(cur) > 0u) {
      /* A write request cleared while writers are still parked on it: nothing
       * will wake them, since the home has no second grant to send for a
       * request it has already answered.  A write axis already held keeps its
       * word, and its writers are served by that hold. */
      ARTS_ERROR("fam: guid %lu clears its write request with %u writers "
                 "parked on it (word=%llx)",
                 (unsigned long)db_guid, (unsigned)CACHE_RW_CNT(cur),
                 (unsigned long long)cur);
    }
    arts_send_db_excl_release(arts_guid_get_rank(db_guid), db_guid, DB_MODE_RW,
                              /*version=*/0u, /*cv=*/0u, NULL, 0u,
                              /*rdzv_txid=*/0u, /*rdzv_cookie=*/0u);
    break;
  case CACHE_ACT_REL_RO: /* phantom RO grant: nothing to serve, return home */
    arts_send_db_excl_release(arts_guid_get_rank(db_guid), db_guid, DB_MODE_RO,
                              /*version=*/0u, /*cv=*/0u, NULL, 0u,
                              /*rdzv_txid=*/0u, /*rdzv_cookie=*/0u);
    break;
  default:
    break;
  }
  arts_shared_release(&db_h);
}
#else
/* Steps (3)+(4) of the grant: the cache_state transition + resulting drain /
 * phantom return.  Factored out so the rendezvous continuation (grant bytes
 * landed) runs the identical commit.  Consumes db_h. */
static void lock_grant_commit(arts_shared_ptr_t db_h, arts_guid_t db_guid,
                              arts_db_access_mode_t mode,
                              const struct arts_rdzv_landing_s *pub) {
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  struct arts_db_cache_s *cache = &db->cache;

  /* Stash home's publish landing for this grant's eventual RW release —
   * BEFORE the CAS that lets local writers run (the single ACK-gated releaser
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
  case CACHE_ACT_REL_RO: /* phantom RO grant: nothing to serve, return home */
    arts_send_db_excl_release(arts_guid_get_rank(db_guid), db_guid, DB_MODE_RO,
                              /*version=*/0u, /*cv=*/0u, NULL, 0u,
                              /*rdzv_txid=*/0u, /*rdzv_cookie=*/0u);
    break;
  case CACHE_ACT_REL_RW_EMPTY:
    /* The RW mirror of the line above: a grant whose cohort was already
     * served under this rank's own create holds nothing, and nothing was
     * written under it, so the write right goes back with no payload — the
     * home's release handler takes the same no-data path an RO release
     * takes. */
    arts_send_db_excl_release(arts_guid_get_rank(db_guid), db_guid, DB_MODE_RW,
                              /*version=*/0u, /*cv=*/0u, NULL, 0u,
                              /*rdzv_txid=*/0u, /*rdzv_cookie=*/0u);
    break;
  default:
    break;
  }

  arts_shared_release(&db_h);
}
#endif /* ARTS_FAM */

#ifndef ARTS_FAM
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
    lock_grant_commit(ctx->db_h, ctx->db_guid, ctx->mode, &ctx->pub);
  } else {
    arts_shared_release(&ctx->db_h); /* destroyed mid-flight — drop */
  }
  arts_free(ctx);
}
#endif /* ARTS_FAM */

#ifdef ARTS_FAM
void arts_handler_db_excl_grant(void *payload, size_t size) {
  struct arts_msg_excl_grant_packet_s *p =
      (struct arts_msg_excl_grant_packet_s *)payload;
  arts_db_access_mode_t mode = (arts_db_access_mode_t)p->mode;
  if (size != sizeof(*p) || p->rdzv_txid != 0) {
    ARTS_ERROR("fam: a grant from rank %u carries a payload; on this arm a "
               "grant carries the block's store and nothing else",
               p->header.rank);
  }
  arts_shared_ptr_t db_h = arts_route_table_lookup_db(p->db_guid);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  if (db == NULL) {
    arts_shared_release(&db_h); /* destroyed mid-flight: drop, by design */
    return;
  }
  arts_db_cache_note_answered(&db->cache); /* before it takes effect */
  struct arts_db_cache_s *cache = &db->cache;
  /* Hinted first touch: the size may still be unlearned here, and the fetch
   * needs it. */
  if (cache->db_size == 0 && p->data_size > 0) {
    cache->db_size = p->data_size;
  }
  /* The grant names the block's store in the landing's address field, and
   * nothing else: the owner is the address's slice, and the landing's key,
   * txid and the version are unread here.  A rank that created the block
   * already knows the store and records nothing.  A zero address is legal for
   * a sentinel-sized block, which has none. */
  if (p->pub.addr != 0) {
    (void)arts_db_fam_slot_record(cache, p->pub.addr);
  }
  lock_grant_commit(db_h, p->db_guid, mode); /* consumes db_h */
}
#else
void arts_handler_db_excl_grant(void *payload, size_t size) {
  struct arts_msg_excl_grant_packet_s *p =
      (struct arts_msg_excl_grant_packet_s *)payload;
  arts_db_access_mode_t mode = (arts_db_access_mode_t)p->mode;
  const void *data = (const char *)p + sizeof(*p);
  uint64_t data_size = (uint64_t)size - (uint64_t)sizeof(*p);

  arts_shared_ptr_t db_h = arts_route_table_lookup_db(p->db_guid);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  if (db == NULL) {
    /* Destroyed mid-flight: still consume any pairing so the txid table stays
     * leak-free (EXCL landings are in-place, cookie 0 — nothing to free). */
    arts_db_rdzv_discard_landing(p->rdzv_txid, p->rdzv_cookie);
    arts_shared_release(&db_h);
    return;
  }
  arts_db_cache_note_answered(&db->cache); /* before it takes effect */
  struct arts_db_cache_s *cache = &db->cache;

  /* Hinted first touch: the size may still be unlearned here. */
  if (cache->db_size == 0 && p->data_size > 0) {
    cache->db_size = p->data_size;
  }

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
    arts_net_rdzv_expect(p->rdzv_txid, lock_grant_landed_cb, ctx);
    return;
  }

  /* Same-rank / data-less grant: install any inline bytes in place (exclusive-
   * lock serialization — no local holder; duplicate bytes are identical), then
   * commit. */
  if (data_size > 0u) {
    arts_db_buf_write_inplace(cache, data, data_size);
  }
  lock_grant_commit(db_h, p->db_guid, mode, &pub); /* consumes db_h */
}
#endif /* ARTS_FAM */

/* ===== arts_send_db_excl_release ========================================
 * Send EXCL_RELEASE to the home.  RW carries publish data, version, and cv
 * (the releaser's stack-local sem_t address so home can echo it in the ACK);
 * RO carries none and passes version=0 / cv=0.
 * Self-send (home == this rank) routes through the OoO engine so reordering
 * against DB_CREATE is handled identically to the wire path. */
void arts_send_db_excl_release(unsigned int home_rank, arts_guid_t db_guid,
                               arts_db_access_mode_t mode, uint64_t version,
                               uint64_t cv, const void *data,
                               uint64_t data_size, uint64_t rdzv_txid,
                               uint64_t rdzv_cookie) {
  uint64_t ds = (mode == DB_MODE_RW) ? data_size : 0u;

  if (home_rank == arts_global_rank_id) {
    /* Self-send: route through the OoO engine exactly as the wire RX
     * dispatcher does — HIT runs arts_handler_db_excl_release inline (which
     * posts cv); MISS defers the args until the home db_s is installed.  A
     * data-less release (the common self case) carries stack args directly;
     * only an inline-data release builds the contiguous args buffer. */
    if (data == NULL || ds == 0u) {
      struct arts_ooo_args_db_excl_release_s args = {
          .releaser = arts_global_rank_id,
          .db_guid = db_guid,
          .mode = (uint32_t)mode,
          .data_size = 0,
          .cv = cv,
          .version = version,
          .rdzv_txid = 0,
          .rdzv_cookie = 0,
          .data_inline = 0,
      };
      arts_ooo_dispatch_or_defer_guid(db_guid, OOO_DB_EXCL_RELEASE, &args,
                                      sizeof(args));
      return;
    }
#ifdef ARTS_FAM
    /* Every release on this arm is data-less: the turn's bytes reached the
     * block's store before this call, so the arm above is the only one. */
    ARTS_ERROR("fam: a release of guid %lu carries %lu payload bytes",
               (unsigned long)db_guid, (unsigned long)ds);
#else
    uint32_t asz =
        (uint32_t)(sizeof(struct arts_ooo_args_db_excl_release_s) + ds);
    char *abuf = (char *)arts_malloc(asz);
    struct arts_ooo_args_db_excl_release_s *args =
        (struct arts_ooo_args_db_excl_release_s *)abuf;
    args->releaser = arts_global_rank_id;
    args->db_guid = db_guid;
    args->mode = (uint32_t)mode;
    args->data_size = ds;
    args->cv = cv;
    args->version = version;
    args->rdzv_txid = 0;
    args->rdzv_cookie = 0;
    args->data_inline = 1u;
    memcpy(abuf + sizeof(*args), data, (size_t)ds);
    arts_ooo_dispatch_or_defer_guid(db_guid, OOO_DB_EXCL_RELEASE, abuf, asz);
    arts_free(abuf);
    return;
#endif /* ARTS_FAM */
  }

  /* Remote send: control-only — a dirty RW release PUT its bytes into the
   * grant's home landing before this packet; {rdzv_txid, rdzv_cookie} echo it
   * for pairing. */
  (void)data;
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
#ifdef ARTS_FAM
  /* The turn's bytes go back to the block's store BEFORE the home hears
   * anything: the home's next act is to grant somebody, and that somebody
   * reads the store.  The home's own turn takes the same two copies every
   * other rank's does, which is why the purge precedes the home-local branch
   * below — a write-back edge that holds only when the home happens to be the
   * writer is not one worth having. */
  fam_purge_working_copy(cache);
  if (home == arts_global_rank_id) {
    lock_release_commit(arts_db_of_cache(cache), cache, DB_MODE_RW);
    return;
  }
  arts_send_db_excl_release(home, cache->db_guid, DB_MODE_RW, /*version=*/0u,
                            /*cv=*/0u, /*data=*/NULL, /*data_size=*/0u,
                            /*rdzv_txid=*/0u, /*rdzv_cookie=*/0u);
  return;
#else
  if (home == arts_global_rank_id) {
    /* Home-local RW release: this rank IS home, so the releaser already holds
     * home's authoritative buffer (its in-place writes have landed) and the
     * caller keeps the descriptor pinned across this call.  There is nothing to
     * ship and no ACK to await — apply the home lock_state commit + onward
     * grant directly on the live cache.  Routing this through a GUID-keyed
     * self-send would re-resolve the DB via its route slot, which a concurrent
     * (legal) destroy may have already detached while this holder still owed
     * its release; the self-send would then MISS and defer on the OoO list
     * forever, losing the release outright.  A release provably follows a
     * successful acquire, so it never needs the OoO before-create deferral
     * that the request path relies on. */
    lock_release_commit(arts_db_of_cache(cache), cache, DB_MODE_RW);
    return;
  }
  arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf =
      (struct arts_db_buffer_s *)arts_shared_get(buf_h);
  const void *data = (buf != NULL) ? buf->data : NULL;
  uint64_t ds = (buf != NULL) ? cache->db_size : 0u;
  /* Consume this grant's home publish landing (1:1 grant:release). */
  struct arts_rdzv_landing_s pub = cache->home_pub_rdzv;
  cache->home_pub_rdzv = (struct arts_rdzv_landing_s){0, 0, 0, 0};
  if (home != arts_global_rank_id && ds > 0u && pub.txid == 0) {
    /* Dirty release from a hold that never received a grant (the
     * creator-seeded RW grant): no cached landing — run the announce leg.
     * Heap rendezvous, deliberately leaked on the shutdown escape (a late
     * CTS/ACK writes/posts through the echoed cv; see the HOME-placement
     * publish sync for the same discipline). */
    struct arts_db_pub_rendezvous_s *wr =
        (struct arts_db_pub_rendezvous_s *)arts_malloc(sizeof(*wr));
    sem_init(&wr->sem, 0, 0);
    wr->landing = (struct arts_rdzv_landing_s){0, 0, 0, 0};
    arts_send_db_excl_release(home, cache->db_guid, DB_MODE_RW,
                              /*version=*/0u, (uint64_t)(uintptr_t)wr,
                              /*data=*/NULL, ds, /*rdzv_txid=*/0u,
                              /*rdzv_cookie=*/0u);
    arts_db_await_ack(&wr->sem); /* CTS wake — or the shutdown escape */
    if (wr->landing.txid == 0) {
      /* This read of wr->landing.txid is UNSYNCHRONIZED on the shutdown-
       * escape path — sem_timedwait returned via the shutdown timeout, not a
       * real post, so there is no happens-before edge against a CTS reply
       * that races in concurrently.  That is precisely why wr is leaked
       * instead of freed: do not "tighten" this into an immediate free, or a
       * late racing write turns it into a use-after-free. */
      arts_db_buf_release(&buf_h);
      return; /* shutdown escape: round abandoned with the runtime — leak wr */
    }
    /* buf_h transfers into the PUT's local completion, which pins the source
     * bytes until the fabric drains them.  cv=0 asks for no ACK: the home
     * pairs {packet, write completion} on its own side and commits + grants
     * from there, so waiting here would only serve the pin the callback
     * already holds. */
    arts_net_put_payload((int)home, wr->landing.addr, wr->landing.key,
                         wr->landing.txid, data, ds, arts_db_buf_ref_release_cb,
                         (void *)buf_h);
    arts_send_db_excl_release(home, cache->db_guid, DB_MODE_RW,
                              /*version=*/0u, /*cv=*/0u,
                              /*data=*/NULL, ds, wr->landing.txid,
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
                              /*cv=*/0u, /*data=*/NULL, ds, pub.txid,
                              pub.cookie);
    return; /* buf_h transferred to the PUT */
  }
  /* Data-less release: nothing is in flight from this buffer, so the send
   * carries the whole release and the local ref drops here. */
  arts_send_db_excl_release(home, cache->db_guid, DB_MODE_RW, /*version=*/0u,
                            /*cv=*/0u, data, ds, /*rdzv_txid=*/0u,
                            /*rdzv_cookie=*/0u);
  arts_db_buf_release(&buf_h);
#endif /* ARTS_FAM */
}

static void lock_send_release_ro(struct arts_db_cache_s *cache) {
  unsigned int home = (unsigned int)arts_guid_get_rank(cache->db_guid);
  if (home == arts_global_rank_id) {
    /* Home-local RO release: commit the home lock_state transition + onward
     * grant directly, for the same reason the RW path does — a destroy that
     * detached the route slot must not be able to defer this release forever. */
    lock_release_commit(arts_db_of_cache(cache), cache, DB_MODE_RO);
    return;
  }
  arts_send_db_excl_release(home, cache->db_guid, DB_MODE_RO, /*version=*/0u,
                            /*cv=*/0u, NULL, 0u, /*rdzv_txid=*/0u,
                            /*rdzv_cookie=*/0u);
}

/* ===== arts_db_release_rw / arts_db_release_ro =========================
 * Single-word release: CAS the count down (+ the 0-edge state transition,
 * atomic together) via cache_compute_next, then run the release action it
 * returns.  CACHE_ACT_REL_RW → publish (the last holder of an RW grant, incl.
 * the last RO joiner under it); CACHE_ACT_REL_RO → notify (last RO holder).
 *
 * Destroyed-guard: a dep NULL-woken at destroy never really held the lock; a
 * count of 0 means there is nothing to release — skip rather than underflow.
 * (Full destroy reconciliation under the acquire-time count is a separate
 * subtask.) */
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
      return; /* destroyed-guard / already released */
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
      return; /* destroyed-guard / already released */
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
