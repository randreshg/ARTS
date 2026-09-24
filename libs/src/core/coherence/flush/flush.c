/* SPDX-License-Identifier: Apache-2.0
 *
 * FLUSH: the one protocol of the DB_WRF memory model.  The home holds a
 * block's single payload line.  A non-home acquire, in either mode, lands a
 * private copy of the line for the acquiring EDT alone; a non-home RW release
 * writes that copy back into the line and returns once the home has seen it
 * land.  Nothing is validated, combined, versioned or invalidated: the program
 * keeps a block's write acquisitions exclusive, so the last write-back is the
 * block's value and a reader's copy is whatever the line held when it was
 * fetched.
 */
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "arts/coherence/buffer.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/handlers.h"
#include "arts/counter/Preamble.h"
#include "arts/db.h"
#include "arts/edt.h"
#include "arts/gas/guid.h"
#include "arts/gas/route_table.h"
#include "arts/memory/regpool.h"
#include "arts/ooo.h"
#include "arts/runtime_state.h"
#include "arts/runtime_types.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/transport/net.h"
#include "arts/transport/protocol.h"
#include "arts/utils/atomics.h"
#include "arts/utils/malloc.h"

#define SHARER_BITS (8u * (unsigned)sizeof(unsigned long))

/* ===== sharer roster: who must hear the destroy ========================= */

static void sharers_mark(struct arts_db_s *db, unsigned int rank) {
  if (db->sharers == NULL || rank == arts_global_rank_id ||
      rank / SHARER_BITS >= db->sharer_words) {
    return;
  }
  __atomic_fetch_or(&db->sharers[rank / SHARER_BITS], 1UL << (rank % SHARER_BITS),
                    __ATOMIC_RELAXED);
}

static bool sharers_test(const struct arts_db_s *db, unsigned int rank) {
  if (db->sharers == NULL || rank / SHARER_BITS >= db->sharer_words) {
    return false;
  }
  return (__atomic_load_n(&db->sharers[rank / SHARER_BITS], __ATOMIC_RELAXED) &
          (1UL << (rank % SHARER_BITS))) != 0;
}

/* ===== home directory ==================================================== */

void arts_db_home_init(struct arts_db_s *db, unsigned int creator_rank,
                       unsigned int nranks) {
  db->sharer_words = (nranks + SHARER_BITS - 1) / SHARER_BITS;
  db->sharers = (unsigned long *)arts_calloc(db->sharer_words, sizeof(unsigned long));
  /* A remote creator holds a stub from the moment it made the block. */
  if (creator_rank < nranks) {
    sharers_mark(db, creator_rank);
  }
}

void arts_db_home_teardown(struct arts_db_s *db) {
  if (db == NULL) {
    return;
  }
  arts_free(db->sharers);
  db->sharers = NULL;
  db->sharer_words = 0;
}

/* ===== cache lifecycle =================================================== */

void arts_db_cache_init(struct arts_db_cache_s *c, arts_guid_t db_guid,
                        uint64_t db_size, arts_db_init_kind_t kind,
                        unsigned int creator_rank) {
  c->db_guid = db_guid;
  c->db_size = db_size;
  __atomic_store_n(&c->payload_pending, (uint8_t)1, __ATOMIC_RELAXED);
  arts_db_create_hold_seed(c, kind);
  arts_lf_pool_init(&c->buf_freelist, 0);
  c->home_line_addr = 0;
  c->home_line_rkey = 0;
  c->flush_txid = 0;
  unsigned int n = arts_global_rank_count ? arts_global_rank_count : 1;
  struct arts_db_s *db = arts_db_of_cache(c);
  if (kind == ARTS_DB_INIT_HOME_RECV) {
    arts_db_home_init(db, creator_rank, n);
    db->home_initialized = true;
  } else if (kind == ARTS_DB_INIT_CREATOR_HOME) {
    arts_db_home_init(db, arts_global_rank_id, n);
    db->home_initialized = true;
  }
  /* CREATOR_REMOTE and STUB are cache-only: nothing else to arm. */
}

void arts_db_cache_destructor(struct arts_db_cache_s *cache) {
  if (cache == NULL) {
    return;
  }
  arts_atomic_shared_store(&cache->buffer, NULL);
  __atomic_store_n(&cache->payload_pending, (uint8_t)1, __ATOMIC_RELEASE);
  arts_lf_pool_destroy_with(&cache->buf_freelist, arts_regpool_free);
  struct arts_db_s *db = arts_db_of_cache(cache);
  if (db->home_initialized) {
    arts_db_home_teardown(db);
    db->home_initialized = false;
  }
}

/* ===== acquire: the home reads the line in place, everyone else fetches == */

/* Nothing is serialized.  An acquire here waits on no other EDT's release —
 * there is no grant to hand over, no lock to queue behind — so one EDT's
 * fetches can never deadlock against each other and need no global order. */
bool arts_db_acquire_is_serialized(arts_db_access_mode_t mode) {
  (void)mode;
  return false;
}

static bool is_home(const struct arts_db_cache_s *cache) {
  return arts_guid_get_rank(cache->db_guid) == arts_global_rank_id;
}

/* One request per parked EDT: the private copy is allocated here and
 * advertised as the landing, so the home's PUT lands straight in it.  A
 * block whose size this rank cannot bound sends no landing and is answered
 * size-only; the re-request then carries one. */
static void fetch_send(struct arts_db_cache_s *cache, arts_guid_t edt_guid,
                       uint32_t slot) {
  struct arts_msg_fetch_request_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_FETCH_REQUEST);
  p.header.rank = arts_global_rank_id;
  p.db_guid = cache->db_guid;
  p.edt_guid = edt_guid;
  p.slot = slot;
  memset(p.pad, 0, sizeof(p.pad));
  p.rdzv.addr = 0;
  p.rdzv.key = 0;
  p.rdzv.txid = 0;
  p.rdzv.cookie = 0;
  uint64_t size = arts_db_first_fetch_size(cache);
  if (size > 0) {
    struct arts_db_buffer_s *copy = NULL;
    (void)arts_db_buf_detached(size, &copy);
    uint64_t addr = 0, key = 0;
    if (!arts_net_rdzv_local(copy->data, size, &addr, &key)) {
      ARTS_ERROR("coherence: private copy is not fabric-registered — "
                 "one-sided payloads require the registered pool "
                 "(ARTS_MALLOC=mimalloc)");
    }
    p.rdzv.addr = addr;
    p.rdzv.key = key;
    p.rdzv.txid = arts_net_rdzv_txid_next();
    p.rdzv.cookie = (uint64_t)(uintptr_t)copy; /* copy->cb carries the ref */
  }
  arts_transport_send_async((int)arts_guid_get_rank(cache->db_guid), (char *)&p,
                            sizeof(p));
}

void arts_handler_db_acquire(void *item, void *args) {
  struct arts_db_s *db = (struct arts_db_s *)item;
  struct arts_ooo_args_db_acquire_s *a = (struct arts_ooo_args_db_acquire_s *)args;
  struct arts_edt_s *edt = a->edt;
  unsigned int slot = a->slot;
  struct arts_db_cache_s *cache = &db->cache;
  arts_edt_dep_t *dep = &((arts_edt_dep_t *)arts_get_depv(edt))[slot];
  /* Every slot the engine hands an arm owns its block's single acquisition:
   * the engine classifies the EDT's whole dependence vector before anything
   * fires and resolves the aliases itself, from this acquisition's payload. */
  if (is_home(cache)) {
    /* The line is materialized at its first use when the create took no
     * hold; every later use finds it. */
    (void)arts_db_buf_ensure(cache, cache->db_size);
    dep->ptr = arts_db_acquire_local(cache);
    arts_db_acquire_resolved(edt, slot);
    return;
  }
  /* Counted where the acquire is decided, not where a request leaves: the
   * size-only round re-enters the sender, and one acquire is one acquire. */
  INCREMENT_NUM_DB_ACQUIRE_REMOTE_BY(1);
  fetch_send(cache, edt->guid, slot); /* parks; the response resumes it */
}

/* ===== the home serves a fetch =========================================== */

void arts_handler_db_fetch_request(void *item_v, void *args_v) {
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  struct arts_db_cache_s *cache = &db->cache;
  struct arts_ooo_args_db_fetch_request_s *a =
      (struct arts_ooo_args_db_fetch_request_s *)args_v;
  struct arts_msg_fetch_response_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_FETCH_RESPONSE);
  p.header.rank = arts_global_rank_id;
  p.db_guid = cache->db_guid;
  p.edt_guid = a->edt_guid;
  p.slot = a->slot;
  p.db_size = cache->db_size;
  p.rdzv_txid = 0;
  p.rdzv_cookie = a->rdzv.cookie;
  p.line_addr = 0;
  p.line_rkey = 0;
  p.flush_txid = 0;
  sharers_mark(db, a->requester);
  (void)arts_db_buf_ensure(cache, cache->db_size);
  arts_shared_ptr_t line_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *line = (struct arts_db_buffer_s *)arts_shared_get(line_h);
  if (cache->db_size == 0 || line == NULL) {
    p.kind = 0; /* a zero-size block: NULL is its value */
    arts_db_buf_release(&line_h);
  } else if (a->rdzv.txid == 0) {
    p.kind = 2; /* the requester could not size a landing */
    arts_db_buf_release(&line_h);
  } else {
    p.kind = 1;
    uint64_t addr = 0, rkey = 0;
    if (!arts_net_rdzv_local(line->data, cache->db_size, &addr, &rkey)) {
      /* The line lies in no fabric-registered slab, so it can neither be read
       * by a peer nor named in a credit; answering anyway would advertise a
       * zero address and drop every write-back aimed at it. */
      ARTS_ERROR("coherence: home line is not fabric-registered — "
                 "one-sided payloads require the registered pool "
                 "(ARTS_MALLOC=mimalloc)");
    }
    /* A credit for the requester's next flush: no expectation is registered
     * until its commit arrives, so an unused one is a burned counter value. */
    p.flush_txid = arts_net_rdzv_txid_next();
    p.line_addr = addr;
    p.line_rkey = rkey;
    p.rdzv_txid = a->rdzv.txid;
    /* The line ref transfers to the PUT's local completion. */
    arts_net_put_payload((int)a->requester, a->rdzv.addr, a->rdzv.key,
                         a->rdzv.txid, line->data, cache->db_size,
                         arts_db_buf_ref_release_cb, (void *)line_h);
  }
  arts_transport_send_async((int)a->requester, (char *)&p, sizeof(p));
}

/* ===== the requester lands its copy ====================================== */

static void copy_drop(struct arts_db_buffer_s *copy) {
  if (copy != NULL) {
    arts_shared_ptr_t h = copy->cb;
    arts_shared_release(&h);
  }
}

static void discard_cb(void *arg) { copy_drop((struct arts_db_buffer_s *)arg); }

void arts_db_flush_discard_landing(uint64_t txid, uint64_t cookie) {
  struct arts_db_buffer_s *copy = (struct arts_db_buffer_s *)(uintptr_t)cookie;
  if (copy == NULL) {
    return;
  }
  if (txid == 0) {
    copy_drop(copy);
    return;
  }
  arts_net_rdzv_expect(txid, discard_cb, copy);
}

struct fetch_landed_ctx_s {
  arts_shared_ptr_t db_h; /* consumed by the resume */
  struct arts_db_buffer_s *copy;
  arts_guid_t edt_guid;
  uint32_t slot;
};

static void fetch_landed_cb(void *arg) {
  struct fetch_landed_ctx_s *ctx = (struct fetch_landed_ctx_s *)arg;
  if (arts_shared_get(ctx->db_h) == NULL) {
    /* The block went away under a pending acquire, which the model leaves
     * undefined — but the slot still resolves, to NULL, the value a block
     * with no storage reads as, so the EDT is not left parked forever.  The
     * landed bytes have no descriptor to belong to and die here. */
    (void)arts_db_resume_parked(ctx->edt_guid, ctx->slot, NULL, ctx->db_h);
    copy_drop(ctx->copy);
    arts_free(ctx);
    return;
  }
  arts_shared_ptr_t copy_h = ctx->copy->cb;
  /* The ref becomes the EDT's hold on success, and the descriptor pin the
   * handler took is consumed by the resume.  A duplicate wake keeps the first
   * copy, so this copy dies here either way. */
  if (!arts_db_resume_parked(ctx->edt_guid, ctx->slot, copy_h, ctx->db_h)) {
    arts_shared_release(&copy_h);
  }
  arts_free(ctx);
}

static void learn_line(struct arts_db_cache_s *cache, uint64_t addr,
                       uint64_t rkey, uint64_t txid) {
  if (addr == 0) {
    return;
  }
  __atomic_store_n(&cache->home_line_addr, addr, __ATOMIC_RELAXED);
  __atomic_store_n(&cache->home_line_rkey, rkey, __ATOMIC_RELAXED);
  if (txid != 0) {
    /* Published last, with release order: a release that takes the credit
     * sees the address it belongs with. */
    __atomic_store_n(&cache->flush_txid, txid, __ATOMIC_RELEASE);
  }
}

void arts_handler_db_fetch_response(void *item_v, void *args_v) {
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_db_fetch_response_args_s *a =
      (struct arts_db_fetch_response_args_s *)args_v;
  if (cache->db_size == 0 && a->db_size > 0) {
    cache->db_size = a->db_size;
  }
  learn_line(cache, a->line_addr, a->line_rkey, a->flush_txid);
  struct arts_db_buffer_s *copy = (struct arts_db_buffer_s *)(uintptr_t)a->rdzv_cookie;
  if (a->kind == 2) {
    copy_drop(copy);
    fetch_send(cache, a->edt_guid, a->slot);
    return;
  }
  if (a->kind == 0) {
    copy_drop(copy);
    (void)arts_db_resume_parked(a->edt_guid, a->slot, NULL,
                                arts_route_table_lookup_db(cache->db_guid));
    return;
  }
  struct fetch_landed_ctx_s *ctx =
      (struct fetch_landed_ctx_s *)arts_malloc(sizeof(*ctx));
  ctx->db_h = arts_route_table_lookup_db(cache->db_guid);
  ctx->copy = copy;
  ctx->edt_guid = a->edt_guid;
  ctx->slot = a->slot;
  arts_net_rdzv_expect(a->rdzv_txid, fetch_landed_cb, ctx);
}

/* ===== release: the home does nothing, everyone else writes back ======== */

void arts_db_release_ro(struct arts_db_cache_s *cache) { (void)cache; }

struct flush_wait_s {
  sem_t sem;
  uint64_t line_addr;
  uint64_t line_rkey;
  uint64_t txid;
};

static void send_flush_announce(unsigned int home, arts_guid_t db_guid,
                                struct flush_wait_s *w) {
  struct arts_msg_flush_announce_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_FLUSH_ANNOUNCE);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  p.sem = (uint64_t)(uintptr_t)w;
  arts_transport_send_async((int)home, (char *)&p, sizeof(p));
}

static void send_flush_commit(unsigned int home, arts_guid_t db_guid,
                              uint64_t txid, struct flush_wait_s *w) {
  struct arts_msg_flush_commit_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_FLUSH_COMMIT);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  p.txid = txid;
  p.sem = (uint64_t)(uintptr_t)w;
  arts_transport_send_async((int)home, (char *)&p, sizeof(p));
}

static bool shutting_down(void) {
  return arts_atomic_read(&arts_node_info.shutdown_state) != 0;
}

/* The write-back round every non-home RW release runs: the bytes at `payload`
 * into the home's line, then the wait for the home's ACK.  The credit is
 * consumed with one exchange; a release that finds none asks for one first.
 *
 * The source must stay valid for the whole call — the caller's hold on the
 * bytes is what keeps it so.  Answers false only under shutdown, the one
 * exit that leaves the round unfinished and its wait state leaked for a
 * reply that may still arrive; a home that no longer has the block is a
 * finished round with nothing to write into. */
static bool flush_write_back(struct arts_db_cache_s *cache, void *payload) {
  unsigned int home = arts_guid_get_rank(cache->db_guid);
  /* The wait lives on the heap: a shutdown-escaped release leaks it, and a
   * late ACK may still post into it. */
  struct flush_wait_s *w = (struct flush_wait_s *)arts_malloc(sizeof(*w));
  sem_init(&w->sem, 0, 0);
  w->txid = __atomic_exchange_n(&cache->flush_txid, 0, __ATOMIC_ACQ_REL);
  if (w->txid != 0) {
    w->line_addr = __atomic_load_n(&cache->home_line_addr, __ATOMIC_RELAXED);
    w->line_rkey = __atomic_load_n(&cache->home_line_rkey, __ATOMIC_RELAXED);
  } else {
    send_flush_announce(home, cache->db_guid, w);
    if (!arts_db_await_ack(&w->sem)) {
      return false; /* the escape is recorded by the await */
    }
    if (shutting_down()) {
      /* The reply came, but the write-back it credits is dropped: its own
       * wait is the one left unawaited. */
      __atomic_fetch_add(&arts_shutdown_abandon.waits, 1u, __ATOMIC_RELAXED);
      return false;
    }
    if (w->txid == 0) {
      /* The home no longer has the block: nothing to write back to. */
      sem_destroy(&w->sem);
      arts_free(w);
      return true;
    }
  }
  arts_net_put_payload((int)home, w->line_addr, w->line_rkey, w->txid, payload,
                       cache->db_size, NULL, NULL);
  send_flush_commit(home, cache->db_guid, w->txid, w);
  if (!arts_db_await_ack(&w->sem)) {
    return false;
  }
  sem_destroy(&w->sem);
  arts_free(w);
  return true;
}

void arts_db_release_rw(struct arts_db_cache_s *cache, void *payload) {
  if (payload == NULL) {
    return; /* a dependence that resolved NULL holds no bytes */
  }
  if (is_home(cache) || cache->db_size == 0) {
    return; /* the line was written in place, or there are no bytes */
  }
  (void)flush_write_back(cache, payload);
}

/* The create's own hold.  Its bytes are the copy the create installed in this
 * rank's slot, so the release writes that copy back and empties the slot: a
 * non-home rank keeps no resident copy of a block it is not holding.  The
 * create mark stays set — the rank has no image left for a later create of
 * the same label to be handed. */
void arts_db_release_created(struct arts_db_cache_s *cache) {
  if (is_home(cache)) {
    return; /* the creator wrote the line in place */
  }
  arts_shared_ptr_t slot_h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *b = (struct arts_db_buffer_s *)arts_shared_get(slot_h);
  if (b != NULL && cache->db_size > 0 && !flush_write_back(cache, b->data)) {
    /* Shutdown escape: the copy stays in the slot under the hold that still
     * names it, because the round that would have retired it never ended. */
    arts_db_buf_release(&slot_h);
    return;
  }
  arts_atomic_shared_store(&cache->buffer, NULL);
  __atomic_store_n(&cache->payload_pending, (uint8_t)1, __ATOMIC_RELEASE);
  arts_db_buf_release(&slot_h);
}

/* ===== the home takes a flush ============================================ */

static void send_flush_ack(unsigned int releaser, arts_guid_t db_guid,
                           uint64_t sem, uint64_t next_txid) {
  struct arts_msg_flush_ack_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_FLUSH_ACK);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  p.sem = sem;
  p.next_txid = next_txid;
  arts_transport_send_async((int)releaser, (char *)&p, sizeof(p));
}

/* The line's wire address, or false when the block has no line yet. */
static bool line_credential(struct arts_db_cache_s *cache, uint64_t *addr,
                            uint64_t *rkey) {
  (void)arts_db_buf_ensure(cache, cache->db_size);
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *line = (struct arts_db_buffer_s *)arts_shared_get(h);
  bool ok = line != NULL && cache->db_size > 0 &&
            arts_net_rdzv_local(line->data, cache->db_size, addr, rkey);
  arts_db_buf_release(&h);
  return ok;
}

struct flush_landed_ctx_s {
  arts_shared_ptr_t db_h;
  arts_guid_t db_guid;
  unsigned int releaser;
  uint64_t sem;
};

static void flush_landed_cb(void *arg) {
  struct flush_landed_ctx_s *ctx = (struct flush_landed_ctx_s *)arg;
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(ctx->db_h);
  uint64_t next = 0;
  uint64_t addr, rkey;
  if (db != NULL && line_credential(&db->cache, &addr, &rkey)) {
    next = arts_net_rdzv_txid_next();
  }
  send_flush_ack(ctx->releaser, ctx->db_guid, ctx->sem, next);
  arts_shared_release(&ctx->db_h);
  arts_free(ctx);
}

void arts_handler_db_flush_commit(void *item_v, void *args_v) {
  struct arts_db_flush_commit_args_s *a = (struct arts_db_flush_commit_args_s *)args_v;
  if (item_v == NULL) {
    /* The block is gone (a destroy overtook a live write acquisition, which
     * the model leaves undefined); the releaser must still be unblocked. */
    send_flush_ack(a->releaser, a->db_guid, a->sem, 0);
    return;
  }
  struct flush_landed_ctx_s *ctx =
      (struct flush_landed_ctx_s *)arts_malloc(sizeof(*ctx));
  ctx->db_h = arts_route_table_lookup_db(a->db_guid);
  ctx->db_guid = a->db_guid;
  ctx->releaser = a->releaser;
  ctx->sem = a->sem;
  arts_net_rdzv_expect(a->txid, flush_landed_cb, ctx);
}

void arts_handler_db_flush_ack(void *item_v, void *args_v) {
  struct arts_db_flush_ack_args_s *a = (struct arts_db_flush_ack_args_s *)args_v;
  if (item_v != NULL && a->next_txid != 0) {
    __atomic_store_n(&((struct arts_db_s *)item_v)->cache.flush_txid, a->next_txid,
                     __ATOMIC_RELEASE);
  }
  sem_post(&((struct flush_wait_s *)(uintptr_t)a->sem)->sem);
}

/* The credit-less release's first leg.  It is the one message of this arm a
 * rank can send before the home has installed the block — a creator writes its
 * copy and releases without waiting for the create's return — so it is
 * deferred and replayed on the install's drain.  The engine pins the
 * descriptor, so the announce always finds the block.  A deferred announce for
 * a block that was destroyed first stays deferred — its releaser leaves the
 * wait only at shutdown, the same way a deferred publish on the validation
 * arms does — which the model leaves undefined (a destroy under a live write
 * acquisition). */
void arts_handler_db_flush_announce(void *item_v, void *args_v) {
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  struct arts_ooo_args_db_flush_announce_s *a =
      (struct arts_ooo_args_db_flush_announce_s *)args_v;
  struct arts_msg_flush_cts_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_FLUSH_CTS);
  p.header.rank = arts_global_rank_id;
  p.db_guid = a->db_guid;
  p.sem = a->sem;
  p.line_addr = 0;
  p.line_rkey = 0;
  /* A zero-size block never announces (its release has no bytes to move), so
   * the credential fails here only if the home cannot allocate or register the
   * line; the zero txid then releases the waiter instead of stranding it. */
  p.txid = 0;
  sharers_mark(db, a->releaser);
  uint64_t addr = 0, rkey = 0;
  if (line_credential(&db->cache, &addr, &rkey)) {
    p.txid = arts_net_rdzv_txid_next();
  }
  p.line_addr = addr;
  p.line_rkey = rkey;
  arts_transport_send_async((int)a->releaser, (char *)&p, sizeof(p));
}

void arts_handler_db_flush_cts(void *item_v, void *args_v) {
  struct arts_db_flush_cts_args_s *a = (struct arts_db_flush_cts_args_s *)args_v;
  struct flush_wait_s *w = (struct flush_wait_s *)(uintptr_t)a->sem;
  w->line_addr = a->line_addr;
  w->line_rkey = a->line_rkey;
  w->txid = a->txid;
  if (item_v != NULL) {
    learn_line(&((struct arts_db_s *)item_v)->cache, a->line_addr, a->line_rkey, 0);
  }
  sem_post(&w->sem);
}

/* ===== create leaves ===================================================== */

/* The home's line exists from the create when the create takes a hold, so a
 * write-back can land before anything else touches the block. */
void arts_db_create_install_home_buffer(struct arts_db_cache_s *cache,
                                        uint64_t db_size) {
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  bool absent = (arts_shared_get(h) == NULL);
  arts_db_buf_release(&h);
  if (db_size > 0 && absent) {
    arts_db_buf_install(cache, /*new_version=*/1, /*data_payload=*/NULL, db_size);
  }
}

/* This arm keeps no permission word for a create to step: the mark the create
 * already took is the whole of its state, and the bytes under the hold are
 * the line the create installed in this rank's slot. */
bool arts_db_create_take_hold(struct arts_db_cache_s *cache) {
  (void)cache;
  return true;
}

void arts_db_create_retract_creator_copy(struct arts_db_s *db) { (void)db; }
void arts_db_create_claim_creator_copy(struct arts_db_s *db) { (void)db; }

/* ===== destroy =========================================================== */

void arts_handler_db_destroy(void *item_v, void *args_v) {
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  struct arts_ooo_args_db_destroy_s *a = (struct arts_ooo_args_db_destroy_s *)args_v;
  if (db == NULL) {
    return;
  }
  unsigned int n = arts_global_rank_count;
  for (unsigned int r = 0; r < n; r++) {
    if (r != arts_global_rank_id && sharers_test(db, r)) {
      arts_send_db_cache_destroy(r, a->db_guid);
    }
  }
  (void)arts_route_table_set_destroyed(a->db_guid);
}
