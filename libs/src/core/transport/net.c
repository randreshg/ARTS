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
#include "arts/transport/net.h"
#include <errno.h>
#include <stdio.h>
#include "arts/runtime_state.h"

#include "arts/utils/stdatomic_compat.h"
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>

#include "arts/counter/Preamble.h" /* INCREMENT_{BYTES,NUM}_REMOTE_* */
#include "arts/counter/counter.h"
#include "arts/defs.h"
#include "arts/utils/malloc.h"
#include "arts/memory/regpool.h"
#include "arts/system/identity.h" /* arts_global_rank_id / arts_global_rank_count */
#include "arts/system/print.h"
#include "arts/system/schedfuzz.h"
#include "arts/system/threads.h"        /* arts_thread_info, ARTS_THREAD_LOCAL */
#include "arts/transport/dispatcher.h"  /* arts_transport_dispatch_packet */
#include "arts/transport/netif.h"       /* arts_netif_ipv4_addr */
#include "arts/transport/protocol.h"    /* arts_msg_header_s, MSG_* */

/* ------------------------------------------------------------------------- */
/* Tunables                                                                    */
/* ------------------------------------------------------------------------- */

/* ARTS_NET_RECV_BUF_SIZE / ARTS_NET_MIN_MULTI_RECV / ARTS_NET_MSG_MAX live in
 * net.h: a sender outside this file checks a control message against the
 * ceiling before it builds one. */
#define ARTS_NET_RECV_BUF_COUNT 2u

/* Per-thread EAGAIN retry ring.  A handful of slots are reserved so
 * the ACK-class messages (PUBLISH_ACK, PUBLISH_CTS, MSG_SHUTDOWN) can always
 * be buffered even when data sends have saturated the ring — otherwise a full
 * ring could wedge the very progress that would drain it. */
#define ARTS_NET_RING_CAP 1024u
#define ARTS_NET_RING_RESERVED 64u
#define ARTS_NET_MAX_RINGS 1024u

#define ARTS_NET_CQ_BURST 16

/* ------------------------------------------------------------------------- */
/* One pending / outstanding transmit                                          */
/* ------------------------------------------------------------------------- */

/* A posted transmit that owns (or borrows) its buffer until local completion.
 * Two operations share the bookkeeping:
 *   NET_TXN_SEND  — two-sided fi_send of a control message.  `bounce`, when
 *                   non-NULL, is a registered staging copy freed on completion;
 *                   `free_method`, when non-NULL, runs on `free_arg` on
 *                   completion (completion-gated free of a caller payload).
 *   NET_TXN_WRITE — one-sided fi_writedata PUT of a payload straight out of
 *                   the caller's registered buffer (zero-copy: bounce is always
 *                   NULL).  `raddr`/`rkey` name the peer's advertised landing,
 *                   `imm` is the rendezvous txid delivered to the peer's CQ.
 *                   `free_method` is the on-local-done hook that releases the
 *                   source buffer once the provider no longer reads it. */
enum net_txn_op {
  NET_TXN_SEND,
  NET_TXN_WRITE,       /* fi_writedata: carries the rendezvous immediate  */
  NET_TXN_WRITE_PLAIN, /* fi_write: a leading piece of a payload the
                        * provider's ceiling cut apart — no immediate, the
                        * last piece's NET_TXN_WRITE carries it            */
};

struct net_txn_s {
  fi_addr_t addr;
  void *buf;
  size_t len;
  void *desc;
  void *bounce;
  void (*free_method)(void *);
  void *free_arg;
  enum net_txn_op op;
  uint64_t raddr; /* NET_TXN_WRITE: peer landing address (per peer mr_mode) */
  uint64_t rkey;  /* NET_TXN_WRITE: peer landing protection key             */
  uint64_t imm;   /* NET_TXN_WRITE: immediate data (rendezvous txid)        */
  uint64_t posted_ns; /* a whole payload PUT's post time; 0 = not timed     */
};

static inline uint64_t net_now_ns(void);

/* A payload PUT's post-to-local-completion time, added on the thread that
 * retires it.  Compiled out with the counter, clock reads included. */
static inline uint64_t net_put_stamp(void) {
#if ENABLE_TIME_DB_PAYLOAD_PUT
  return net_now_ns();
#else
  return 0;
#endif
}

static inline void net_put_retired(uint64_t posted_ns) {
  if (posted_ns != 0) {
    INCREMENT_TIME_DB_PAYLOAD_PUT_BY(net_now_ns() - posted_ns);
  }
}

/* ------------------------------------------------------------------------- */
/* Per-thread EAGAIN retry ring (SPSC: the owning thread is the sole producer; a *
 * single drainer at a time — owner or progress — consumes under a CAS token).   */
/* ------------------------------------------------------------------------- */

struct net_ring_s {
  /* Consumer cursor (monotone), written by the drain-token holder: its
   * release store says the slot was read and its txn posted, and the owner's
   * acquire load orders a slot reuse, or a direct post, after that post.  The
   * owner's direct post stays behind a drainer's post of its queued txns by
   * that acquire alone when the drain gate saw the ring empty while the
   * drainer still held the token. */
  _Atomic(uint32_t) head;
  /* Producer cursor (monotone), written by the owner alone: its release store
   * publishes the slot, which a drainer's acquire load consumes. */
  _Atomic(uint32_t) tail;
  /* Single-drainer token: the winning CAS acquires what the previous holder
   * stored to `head`, and the holder returns it with a release store. */
  _Atomic(int) draining;
  struct net_txn_s *slots[ARTS_NET_RING_CAP];
  /* Transmits the ring's owning thread has posted, whoever's txn it posted:
   * written only by that thread (a relaxed load and a relaxed store), before
   * each post and back after a refused one.  net_tx_outstanding reads it
   * after its acquire of tx_completed. */
  _Alignas(ARTS_CACHE_LINE_SIZE) _Atomic(uint64_t) posted;
};

/* ------------------------------------------------------------------------- */
/* Receive-buffer context                                                      */
/* ------------------------------------------------------------------------- */

struct net_recv_ctx_s {
  unsigned idx; /* which of the ARTS_NET_RECV_BUF_COUNT landing buffers        */
};

/* ------------------------------------------------------------------------- */
/* Module state                                                                */
/* ------------------------------------------------------------------------- */

static struct {
  struct fi_info *info;
  struct fid_fabric *fabric;
  struct fid_domain *domain;
  struct fid_av *av;
  struct fid_cq *cq;
  struct fid_ep *ep;

  int mr_local;         /* FI_MR_LOCAL negotiated -> pass an mr desc           */
  uint32_t mr_mode;     /* negotiated mr_mode bits                             */
  size_t inject_size;   /* fi_inject ceiling                                   */
  size_t max_msg;       /* provider max message size                          */
  uint64_t tx_order;    /* negotiated tx_attr->msg_order (FI_ORDER_* bits)     */
  size_t max_order_waw; /* ep_attr->max_order_waw_size                        */
  bool waw_ordered;     /* RMA writes of up to max_msg bytes are placed in the
                         * order posted: what lets a payload above max_msg go
                         * as pieces paired on the last piece's immediate      */
  bool is_cxi;          /* negotiated provider is the cxi core (selfcheck)     */
  /* Teardown began: refuse new submissions.  Set once every runtime thread
   * has passed the ready_to_clean barrier, joined or not; a sender or reaper
   * that reads it set stands down and reads nothing the store published, so
   * every load is relaxed.  Beside the init-only fields every send reads. */
  _Atomic(bool) quiescing;
  /* First completion error seen while no shutdown was recognized here, as a
   * monotonic stamp (0 = none) and the provider's text.  A lost transfer is
   * fatal on a live run, but a peer already shutting down flushes this rank's
   * transfers before this rank hears of the shutdown, so the verdict waits a
   * grace period for shutdown recognition before it is fatal.  The recorder
   * that moves the stamp from 0 to NET_CQ_ERR_RECORDING alone writes the
   * text, then stores the time stamp with release; a reader acquires the
   * stamp and reads the text only when it holds a time stamp. */
  _Atomic(uint64_t) live_cq_err_ns;
  char live_cq_err_what[160];

  fi_addr_t *peers;     /* rank-indexed AV handles (fi_addr_t rank == rank)    */
  unsigned peer_count;

  /* Multi-recv landing buffers are drawn from the registered pool (so their MR
   * is the enclosing slab's — no module-private fi_mr_reg); `rxdesc` is that
   * slab's local descriptor on FI_MR_LOCAL providers, else NULL. */
  void *rxbuf[ARTS_NET_RECV_BUF_COUNT];
  void *rxdesc[ARTS_NET_RECV_BUF_COUNT];
  struct net_recv_ctx_s rxctx[ARTS_NET_RECV_BUF_COUNT];

  /* Transmits completed, written only by the CQ token holder, or once every
   * runtime thread has passed the ready_to_clean barrier: a reap burst counts
   * its completions and ends with one release store, which the next holder's
   * token CAS carries on.  Its acquire load in net_tx_outstanding makes every
   * counted completion's post, and that post's `posted` store, visible.  It
   * heads the line of the token-owned and refused-post words, apart from the
   * fields every send reads. */
  _Alignas(ARTS_CACHE_LINE_SIZE) _Atomic(uint64_t) tx_completed;
  /* Bitmask of landing buffers whose FI_MULTI_RECV repost returned -FI_EAGAIN
   * (provider queues momentarily full — a legal transient, NOT an error).  The
   * repost is retried at the top of every CQ reap pass: reaping is exactly
   * what frees provider queue slots, so retry-after-reap converges.  Only
   * touched under the CQ consumer token, before any runtime thread exists, or
   * by the teardown reap once every runtime thread has passed the
   * ready_to_clean barrier, so a plain unsigned suffices. */
  unsigned rx_repost_pending;
  /* Posts the provider refused with EAGAIN: only its own value. */
  _Atomic(uint64_t) eagain_count;
} g_net;

/* The stamp's value while its election winner writes the text: a monotonic
 * time stamp is never 0 and never this. */
#define NET_CQ_ERR_RECORDING UINT64_MAX

/* Global registry of per-thread rings so progress can drain rings owned by
 * threads that are not currently sending.  Grow-only: an append, under
 * g_ring_reg_lock, publishes the ring and its g_rings entry with a release
 * store of the count, which a lock-free scan acquires. */
static struct net_ring_s *g_rings[ARTS_NET_MAX_RINGS];
static _Atomic(uint32_t) g_ring_count;
static pthread_mutex_t g_ring_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static ARTS_THREAD_LOCAL struct net_ring_s *t_ring;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                               */
/* ------------------------------------------------------------------------- */

static inline void *net_desc(const void *p) {
  if (!g_net.mr_local) {
    return NULL;
  }
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  if (m == NULL || m->mr == NULL) {
    /* A provider that requires local descriptors cannot be handed a region it
     * has no registration for: naming NULL would leave it to reject or misread
     * the buffer at submit time, far from the caller that chose the memory. */
    ARTS_ERROR("arts_net: buffer %p lies in no fabric-registered slab — the "
               "provider requires a local descriptor, so every send source, "
               "PUT source and landing requires the registered pool",
               p);
  }
  return fi_mr_desc((struct fid_mr *)m->mr);
}

/* A registered staging buffer for a bounce copy.  regpool fails loudly rather
 * than return an unregistered pointer, so a NULL here is a genuine exhaustion. */
static inline void *net_bounce(size_t size) {
  void *b = arts_regpool_alloc_aligned(size, 64);
  if (b == NULL) {
    ARTS_ERROR("arts_net: registered bounce alloc failed (%zu bytes)", size);
  }
  return b;
}

/* Shutdown / ACK-class messages must never be starved by data-send
 * backpressure — a home rank's ACK is often exactly what unblocks the peer
 * that would otherwise keep the ring full. */
static inline bool net_is_ack_class(unsigned int msg_type) {
  switch (msg_type) {
  case MSG_SHUTDOWN:
  case MSG_DB_PUBLISH_ACK:
  case MSG_DB_PUBLISH_CTS:
    return true;
  default:
    return false;
  }
}

/* True when a provider request names the cxi core provider as a list element
 * ("cxi", "cxi,tcp" — FI_PROVIDER accepts a comma list).  A negated filter
 * list ("^shm") never counts: the pre-getinfo adaptations cxi needs (its
 * write-with-immediate environment gate, the FI_MR_ENDPOINT offer) must be
 * committed before matching runs, so selecting cxi requires naming it. */
static bool net_provider_names_cxi(const char *s) {
  if (s == NULL || s[0] == '^') {
    return false;
  }
  const char *p = s;
  for (;;) {
    const char *e = strchr(p, ',');
    size_t n = (e != NULL) ? (size_t)(e - p) : strlen(p);
    if (n == 3 && strncmp(p, "cxi", 3) == 0) {
      return true;
    }
    if (e == NULL) {
      return false;
    }
    p = e + 1;
  }
}

/* True when `s` names the verbs core, alone or under a utility layer
 * ("verbs;ofi_rxm"), as a whole entry of a comma list; an exclusion list never
 * counts.  Naming it is what commits the address-format decision below. */
static bool net_provider_names_verbs(const char *s) {
  if (s == NULL || s[0] == '^') {
    return false;
  }
  const char *p = s;
  for (;;) {
    const char *e = strchr(p, ',');
    size_t n = (e != NULL) ? (size_t)(e - p) : strlen(p);
    if (n >= 5 && strncmp(p, "verbs", 5) == 0 && (n == 5 || p[5] == ';')) {
      return true;
    }
    if (e == NULL) {
      return false;
    }
    p = e + 1;
  }
}

static struct net_ring_s *net_get_ring(void) {
  if (t_ring != NULL) {
    return t_ring;
  }
  struct net_ring_s *r = (struct net_ring_s *)arts_calloc_aligned(
      1, sizeof(struct net_ring_s), ARTS_CACHE_LINE_SIZE);
  pthread_mutex_lock(&g_ring_reg_lock);
  uint32_t idx = atomic_load_explicit(&g_ring_count, memory_order_relaxed);
  if (idx >= ARTS_NET_MAX_RINGS) {
    pthread_mutex_unlock(&g_ring_reg_lock);
    ARTS_ERROR("arts_net: exceeded %u retry rings", ARTS_NET_MAX_RINGS);
  }
  g_rings[idx] = r;
  atomic_store_explicit(&g_ring_count, idx + 1, memory_order_release);
  pthread_mutex_unlock(&g_ring_reg_lock);
  t_ring = r;
  return r;
}

/* Run a transmit's completion-gated releases and free its bookkeeping. */
static inline void net_txn_complete(struct net_txn_s *txn) {
  net_put_retired(txn->posted_ns);
  if (txn->free_method != NULL) {
    txn->free_method(txn->free_arg);
  }
  if (txn->bounce != NULL) {
    arts_regpool_free(txn->bounce);
  }
  arts_free(txn);
}

static struct net_txn_s *net_txn_new(fi_addr_t addr, void *buf, size_t len,
                                     void *desc, void *bounce,
                                     void (*free_method)(void *),
                                     void *free_arg) {
  struct net_txn_s *txn =
      (struct net_txn_s *)arts_malloc(sizeof(struct net_txn_s));
  txn->addr = addr;
  txn->buf = buf;
  txn->len = len;
  txn->desc = desc;
  txn->bounce = bounce;
  txn->free_method = free_method;
  txn->free_arg = free_arg;
  txn->op = NET_TXN_SEND;
  txn->raddr = 0;
  txn->rkey = 0;
  txn->imm = 0;
  txn->posted_ns = 0;
  return txn;
}

/* Post a txn once (fi_send or fi_writedata by op), counted on the posting
 * thread's own ring `mine`.  0 = accepted (now outstanding), -FI_EAGAIN =
 * provider busy, other = fatal. */
static inline ssize_t net_try_post(struct net_txn_s *txn,
                                   struct net_ring_s *mine) {
  /* Counted before the post: once posted, any thread may reap its
   * completion. */
  uint64_t posted = atomic_load_explicit(&mine->posted, memory_order_relaxed);
  atomic_store_explicit(&mine->posted, posted + 1, memory_order_relaxed);
  arts_sched_fuzz_point(); /* widen the count<->post window */
  ssize_t rc;
  if (txn->op == NET_TXN_SEND) {
    rc = fi_send(g_net.ep, txn->buf, txn->len, txn->desc, txn->addr, txn);
  } else {
    /* Never above the provider's ceiling: a verbs scatter-gather entry
     * carries a 32-bit length, and a work request past the port's maximum
     * comes back as EAGAIN — silent truncation or an endless retry, never an
     * error — so the bound is asserted where the length is final. */
    if (txn->len > g_net.max_msg) {
      ARTS_ERROR("arts_net: one-sided transfer of %zu bytes exceeds the "
                 "provider's %zu-byte message ceiling",
                 txn->len, g_net.max_msg);
    }
    if (txn->op == NET_TXN_WRITE) {
      rc = fi_writedata(g_net.ep, txn->buf, txn->len, txn->desc, txn->imm,
                        txn->addr, txn->raddr, txn->rkey, txn);
    } else {
      rc = fi_write(g_net.ep, txn->buf, txn->len, txn->desc, txn->addr,
                    txn->raddr, txn->rkey, txn);
    }
  }
  if (rc != 0) {
    atomic_store_explicit(&mine->posted, posted, memory_order_relaxed);
  }
  if (rc != 0 && rc != -FI_EAGAIN) {
    ARTS_ERROR("arts_net: %s failed: %s",
               txn->op == NET_TXN_SEND    ? "fi_send"
               : txn->op == NET_TXN_WRITE ? "fi_writedata"
                                          : "fi_write",
               fi_strerror((int)-rc));
  }
  return rc;
}

/* Retry queued txns FIFO until one EAGAINs again or the ring empties, posting
 * on the calling thread's ring `mine`.  Single drainer at a time (CAS token);
 * the owner enqueues concurrently (SPSC). */
static bool net_ring_drain(struct net_ring_s *r, struct net_ring_s *mine) {
  /* A read-only gate: an empty ring stays unwritten, so its line stays shared
   * with its owner.  The owner's own gate is exact (it alone writes tail, and
   * head only grows up to it); another thread's stale read only defers the
   * ring to the next poll, as a lost token CAS does, and the loop below
   * re-reads both cursors. */
  if (atomic_load_explicit(&r->head, memory_order_relaxed) ==
      atomic_load_explicit(&r->tail, memory_order_relaxed)) {
    return false;
  }
  arts_sched_fuzz_point(); /* widen the gate<->token window */
  int expected = 0;
  if (!atomic_compare_exchange_strong_explicit(
          &r->draining, &expected, 1, memory_order_acq_rel,
          memory_order_relaxed)) {
    return false;
  }
  bool did = false;
  for (;;) {
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (h == t) {
      break;
    }
    struct net_txn_s *txn = r->slots[h % ARTS_NET_RING_CAP];
    ssize_t rc = net_try_post(txn, mine);
    if (rc == -FI_EAGAIN) {
      break; /* still blocked — leave it (and the rest) for the next pass */
    }
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
    did = true;
  }
  atomic_store_explicit(&r->draining, 0, memory_order_release);
  return did;
}

/* Drain every registered retry ring once (this thread's and any other thread's
 * whose owner is not currently sending).  Pure TX progress: it posts queued
 * fi_sends and touches neither the CQ nor RX dispatch, so a producer thread
 * blocked in send backpressure makes forward progress WITHOUT reaping the CQ or
 * running an inbound handler on its own stack (which would nest coherence
 * dispatch on a thread that may hold a DB lock).  Returns true if it posted
 * anything. */
static bool net_drain_all_rings(void) {
  /* Resolved before any token is taken: a first call registers the ring. */
  struct net_ring_s *mine = net_get_ring();
  bool did = false;
  uint32_t rc = atomic_load_explicit(&g_ring_count, memory_order_acquire);
  for (uint32_t k = 0; k < rc; k++) {
    if (g_rings[k] != NULL && net_ring_drain(g_rings[k], mine)) {
      did = true;
    }
  }
  return did;
}

/* Enqueue a materialized txn for later retry; false if this class has no room
 * (non-ACK caps out ARTS_NET_RING_RESERVED slots below capacity). */
static bool net_ring_try_enqueue(struct net_ring_s *r, struct net_txn_s *txn,
                                 bool is_ack) {
  uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
  uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
  uint32_t used = t - h;
  uint32_t cap =
      is_ack ? ARTS_NET_RING_CAP : (ARTS_NET_RING_CAP - ARTS_NET_RING_RESERVED);
  if (used >= cap) {
    return false;
  }
  r->slots[t % ARTS_NET_RING_CAP] = txn;
  atomic_store_explicit(&r->tail, t + 1, memory_order_release);
  return true;
}

/* Reap a bounded CQ burst WITHOUT dispatching (defined below): completes TX,
 * copies inbound messages onto the pending-dispatch list, reposts released
 * landing buffers — but runs no handler.  Used by the backpressure spin so a
 * saturating producer keeps the CQ moving (its own TX completions free ring
 * slots; draining RX un-sticks a mutually-saturating peer) without re-entering
 * dispatch on a stack that may already hold a coherence lock. */
static bool net_reap_no_dispatch(void);

/* Hand an already-materialized txn to the fabric.  Drains any earlier queued
 * txns first (per-thread FIFO), tries a direct post when the ring is empty, and
 * on EAGAIN buffers the txn — or, when the ring is full for this class, applies
 * deliberate producer backpressure by spinning progress until the provider
 * accepts. */
static void net_submit(struct net_txn_s *txn, bool is_ack) {
  struct net_ring_s *r = net_get_ring();
  net_ring_drain(r, r);

  uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
  uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
  if (h == t) {
    ssize_t rc = net_try_post(txn, r);
    if (rc == 0) {
      return;
    }
    atomic_fetch_add_explicit(&g_net.eagain_count, 1, memory_order_relaxed);
  }
  if (net_ring_try_enqueue(r, txn, is_ack)) {
    return;
  }
  /* Ring saturated for this class: block the producer, assisting progress until
   * a slot opens, then enqueue BEHIND the existing entries — never direct-post
   * ahead of them, which would break this thread's FIFO ordering.
   *
   * Assisting progress here must be liveness-complete but dispatch-free.  A
   * saturating send may itself be issued from inside an inbound handler on the
   * progress thread, so we must NOT re-enter RX dispatch on this stack (it could
   * nest coherence on a thread already holding a DB lock).  But draining rings
   * ALONE is not enough: if the peer is also saturated and stalled in its own
   * backpressure spin, neither side reaps its CQ, kernel socket buffers fill,
   * the provider's autonomous TX progress wedges, and the two ranks deadlock.
   * So we also take the CQ token (safe: this thread never holds it — dispatch
   * always runs after the token is released) and reap WITHOUT dispatching:
   * completing our TX frees the bounces/slots the ring is waiting on, and
   * draining the peer's inbound messages (onto the pending list, handled later
   * by the progress loop) advances the peer's socket so its TX un-wedges.  This
   * breaks the mutual-saturation cycle while keeping dispatch off this stack. */
  for (;;) {
    net_drain_all_rings();
    net_reap_no_dispatch();
    if (net_ring_try_enqueue(r, txn, is_ack)) {
      return;
    }
  }
}

/* Header-only inject (no completion, buffer reusable on return).  A direct
 * fi_inject is only taken when this thread's retry ring is EMPTY: if earlier
 * sends from this thread are still queued, a fresh inject would jump ahead of
 * them and break same-thread->same-peer FIFO (the ordering FI_ORDER_SAS is meant
 * to give).  When the ring is non-empty, or the inject EAGAINs, the header is
 * copied into a registered bounce and submitted BEHIND the queue as a
 * completion-gated fi_send. */
static void net_inject_header(fi_addr_t addr, const void *buf, size_t len,
                              bool is_ack) {
  struct net_ring_s *r = net_get_ring();
  uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
  uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
  if (h == t) {
    ssize_t rc = fi_inject(g_net.ep, buf, len, addr);
    if (rc == 0) {
      return;
    }
    if (rc != -FI_EAGAIN) {
      ARTS_ERROR("arts_net: fi_inject failed: %s", fi_strerror((int)-rc));
    }
    atomic_fetch_add_explicit(&g_net.eagain_count, 1, memory_order_relaxed);
  }
  void *b = net_bounce(len);
  memcpy(b, buf, len);
  net_submit(net_txn_new(addr, b, len, net_desc(b), b, NULL, NULL), is_ack);
}

/* ------------------------------------------------------------------------- */
/* Send core                                                                   */
/* ------------------------------------------------------------------------- */

void arts_net_send_core(int rank, char *message, unsigned int length,
                        char *payload, unsigned int offset, uint64_t size,
                        void (*free_method)(void *)) {
  /* Teardown has begun (every runtime thread is past the ready_to_clean
   * barrier): drop the send, but still run its completion-gated free so the
   * caller payload is not leaked. */
  if (atomic_load_explicit(&g_net.quiescing, memory_order_relaxed)) {
    if (free_method != NULL) {
      free_method(payload);
    }
    return;
  }
  /* A single message must fit one RX landing buffer.  Control-plane ceiling:
   * bulk payloads travel one-sided (arts_net_put_payload) and never come
   * through here, so an oversized total is a control message that outgrew the
   * landing capacity — a protocol bug, rejected loudly rather than split into
   * frames the connectionless RX cannot reassemble. */
  uint64_t wire_total = (uint64_t)length + size;
  if (wire_total > ARTS_NET_MSG_MAX) {
    ARTS_ERROR("arts_net: control message to rank %d is %llu bytes, exceeds RX "
               "landing capacity %zu — bulk payloads must use the one-sided "
               "rendezvous path",
               rank, (unsigned long long)wire_total, (size_t)ARTS_NET_MSG_MAX);
  }
  fi_addr_t addr = g_net.peers[rank];
  bool is_ack =
      net_is_ack_class(((struct arts_msg_header_s *)message)->message_type);
  /* One logical wire message accepted (inject and bounce paths both deliver). */
  INCREMENT_BYTES_REMOTE_SENT_BY(wire_total);
  INCREMENT_NUM_REMOTE_SEND_BY(1);

  if (payload == NULL) {
    if (length <= g_net.inject_size) {
      net_inject_header(addr, message, length, is_ack);
    } else {
      void *b = net_bounce(length);
      memcpy(b, message, length);
      net_submit(net_txn_new(addr, b, length, net_desc(b), b, NULL, NULL),
                 is_ack);
    }
    return;
  }

  /* Header + payload ship as ONE fabric message.  The connectionless RX lands a
   * whole fi_send contiguously in one landing buffer and does no reassembly, so
   * a split header/payload pair would arrive as two independent frames the
   * receiver cannot rejoin (the header frame's declared size would run past the
   * bytes that actually landed, and the raw payload frame carries no header).
   * `wire_total` was already bounded to ARTS_NET_MSG_MAX above, so the whole
   * message is guaranteed to fit; gather it into one registered bounce and
   * submit a single completion-gated send.  The caller payload is released only
   * once that send completes (free_method rides the txn). */
  uint64_t total = (uint64_t)length + size;
  void *b = net_bounce((size_t)total);
  memcpy(b, message, length);
  memcpy((char *)b + length, payload + offset, size);
  net_submit(
      net_txn_new(addr, b, (size_t)total, net_desc(b), b, free_method, payload),
      is_ack);
}

/* ------------------------------------------------------------------------- */
/* Receive posting + progress                                                  */
/* ------------------------------------------------------------------------- */

static void net_post_recv(unsigned idx) {
  struct iovec iov = {.iov_base = g_net.rxbuf[idx],
                      .iov_len = ARTS_NET_RECV_BUF_SIZE};
  struct fi_msg msg = {
      .msg_iov = &iov,
      .desc = g_net.mr_local ? &g_net.rxdesc[idx] : NULL,
      .iov_count = 1,
      .addr = FI_ADDR_UNSPEC,
      .context = &g_net.rxctx[idx],
      .data = 0,
  };
  ssize_t rc = fi_recvmsg(g_net.ep, &msg, FI_MULTI_RECV);
  if (rc == -FI_EAGAIN) {
    /* Transient: provider queues are full right now.  Defer — the CQ reap
     * pass retries pending reposts after draining completions (which is what
     * frees the queue slots).  Fatal only for real errors below. */
    g_net.rx_repost_pending |= (1u << idx);
    return;
  }
  if (rc != 0) {
    ARTS_ERROR("arts_net: fi_recvmsg(buf %u) failed: %s", idx,
               fi_strerror((int)-rc));
  }
  g_net.rx_repost_pending &= ~(1u << idx);
}

/* The CQ is a single-consumer resource: at most one thread reaps completions
 * and reposts landing buffers at a time.  A sending thread that spins progress
 * under backpressure must not race a progress thread on the CQ — a
 * concurrent FI_MULTI_RECV repost could hand a landing buffer back to the
 * provider while another thread is still copying a just-received message out of
 * it.  Ring draining stays lock-free (each ring is token-serialized).  A
 * try-lock: the winning CAS acquires what the previous holder wrote under it,
 * and the holder releases it with a release store. */
static _Atomic(int) g_net_cq_busy;

static inline bool net_cq_trylock(void) {
  /* A read-only gate: a held token is not taken exclusive by a CAS bound to
   * fail.  A stale "busy" only skips one pass; no caller needs the try to
   * succeed. */
  if (atomic_load_explicit(&g_net_cq_busy, memory_order_relaxed) != 0) {
    return false;
  }
  int expected = 0;
  return atomic_compare_exchange_strong_explicit(
      &g_net_cq_busy, &expected, 1, memory_order_acq_rel, memory_order_relaxed);
}
static inline void net_cq_unlock(void) {
  atomic_store_explicit(&g_net_cq_busy, 0, memory_order_release);
}

/* Pending inbound-dispatch list.  Inbound completions are recorded under the
 * CQ token and appended here; the progress loop (net_reap_and_dispatch)
 * splices the whole list out — also under the token — and dispatches it AFTER
 * releasing the token, so a handler that sends (and may block in backpressure)
 * never runs under the CQ consumer lock.  Two node kinds share the list:
 *   - a MESSAGE node (rdzv_txid == 0): the fi_send bytes were COPIED off the
 *     multi-recv landing buffer (len bytes follow the node inline) and are
 *     dispatched through arts_transport_dispatch_packet;
 *   - a RENDEZVOUS-DATA node (rdzv_txid != 0, len == 0): a remote-write
 *     completion (FI_REMOTE_CQ_DATA) whose immediate is the rendezvous txid;
 *     dispatch runs the txid pairing (net_rdzv_data_arrived) — the pairing
 *     callback may install a buffer and send, so it too must run only after
 *     the token is released.
 * A producer stalled in net_submit's backpressure spin also reaps the CQ
 * (net_reap_no_dispatch) and appends here, but never dispatches (its stack may
 * hold a coherence lock) — the completion it drained off the wire to un-stick
 * a saturated peer is still delivered, in arrival order, by the progress loop.
 * Every mutation happens under the CQ token, so plain pointers suffice.
 * Per-peer FIFO holds per splice: the token-serialized CQ consumer reaps in
 * arrival order and each spliced batch dispatches head-first.  With MULTIPLE
 * progress threads (io configs run two) two batches can dispatch concurrently,
 * so cross-batch per-peer FIFO is NOT total order — no coherence path relies
 * on it (all wire pairs are reorder-tolerant by version/txid pairing). */
struct net_pending_s {
  struct net_pending_s *next;
  unsigned len; /* message byte count; the packet bytes follow the node inline */
  uint64_t rdzv_txid; /* != 0: rendezvous-data node (no inline bytes) */
};
static struct net_pending_s *g_pending_head; /* FIFO head (oldest undispatched) */
static struct net_pending_s *g_pending_tail; /* FIFO tail (newest)              */

/* ------------------------------------------------------------------------- */
/* Rendezvous txid pairing                                                     */
/* ------------------------------------------------------------------------- */

/* A rendezvous transfer is TWO independent arrivals with no inter-message
 * ordering assumed: the metadata packet (fi_send, dispatched to a handler that
 * registers an expectation) and the payload write completion (imm == txid).
 * Whichever comes first parks a record; the second takes it and fires the
 * expectation's callback on its own thread.  A txid is allocated by the rank
 * that pairs it — the receiver advertises the landing buffer and the txid
 * together — so uniqueness is a local property, and a 32-bit value suffices:
 * it must only be unique among this rank's in-flight transfers, and it fits
 * the narrowest immediate a real fabric grants (InfiniBand write-with-imm
 * carries 4 bytes).
 *
 * A record lives in a slot of a chain of levels, at an index a per-level
 * multiplicative hash of the txid picks, and the slot's word is its whole
 * state:
 *
 *   [txid:32 | state:3 | pass:20 | gen:9]
 *
 * EMPTY; XP (an expectation claimed the slot and is writing its fields); XPA
 * (XP, and the arrival came meanwhile: the expectation's thread fires); X (an
 * expectation, ready); A (an arrival marker).  `pass` counts records whose
 * chain passes the slot and that are parked deeper; `gen` changes at every
 * claim and every take.  Both events of a txid walk the same slots, and every
 * decision about a slot is one RMW of its word (net_rdzv_decide): an event
 * takes its partner's record, marks an XP, claims an EMPTY slot no record
 * passes, or else descends a level and adds its pass.  So the two events meet
 * in one slot without a lock or a fence: an event that claims at a level
 * finds, at every level above, either its partner's descend already counted
 * (a pass, which only the pair's own fix-up removes) or a slot its partner
 * descended past.  A take subtracts the pair's two passes from each level
 * above it.  The claim, mark, publish and take CASes are acq_rel: they carry
 * the fields (written by the XP claimant before its publish, read by the
 * arrival after an acquire load of the X it then takes) and order each pass
 * before the fix-up that removes it; a descend carries nothing and is
 * relaxed.  Nothing on the pairing path waits, and it allocates only to add a
 * level, which happens only when every slot of a txid's chain so far is busy.
 *
 * Both sides run on the dispatch path of a progress thread or on a worker
 * replaying a parked handler, never under the CQ token. */
#ifndef ARTS_NET_RDZV_LEVEL0_LOG2
#define ARTS_NET_RDZV_LEVEL0_LOG2 10 /* slots at level 0, log2 */
#endif
#ifndef ARTS_NET_RDZV_LEVEL_LOG2
#define ARTS_NET_RDZV_LEVEL_LOG2 10 /* slots at every deeper level, log2 */
#endif

#define NET_RDZV_GEN_BITS 9
#define NET_RDZV_PASS_SHIFT NET_RDZV_GEN_BITS
#define NET_RDZV_PASS_BITS 20
#define NET_RDZV_STATE_SHIFT (NET_RDZV_PASS_SHIFT + NET_RDZV_PASS_BITS)
#define NET_RDZV_TXID_SHIFT 32
#define NET_RDZV_GEN_MASK ((UINT64_C(1) << NET_RDZV_GEN_BITS) - 1)
#define NET_RDZV_PASS_MAX ((UINT64_C(1) << NET_RDZV_PASS_BITS) - 1)
#define NET_RDZV_PASS_ONE (UINT64_C(1) << NET_RDZV_PASS_SHIFT)
#define NET_RDZV_STATE_MASK (UINT64_C(7) << NET_RDZV_STATE_SHIFT)

enum net_rdzv_state {
  NET_RDZV_EMPTY,
  NET_RDZV_XP,
  NET_RDZV_XPA,
  NET_RDZV_X,
  NET_RDZV_A,
};

static inline uint32_t net_rdzv_word_txid(uint64_t w) {
  return (uint32_t)(w >> NET_RDZV_TXID_SHIFT);
}
static inline unsigned net_rdzv_word_state(uint64_t w) {
  return (unsigned)((w & NET_RDZV_STATE_MASK) >> NET_RDZV_STATE_SHIFT);
}
static inline uint64_t net_rdzv_word_pass(uint64_t w) {
  return (w >> NET_RDZV_PASS_SHIFT) & NET_RDZV_PASS_MAX;
}
/* The word holding `txid` in `state`, with w's pass and the next gen. */
static inline uint64_t net_rdzv_word_claimed(uint64_t w, uint32_t txid,
                                             unsigned state) {
  return (uint64_t)txid << NET_RDZV_TXID_SHIFT |
         (uint64_t)state << NET_RDZV_STATE_SHIFT |
         (w & (NET_RDZV_PASS_MAX << NET_RDZV_PASS_SHIFT)) |
         ((w + 1) & NET_RDZV_GEN_MASK);
}
/* w taken back to EMPTY: its pass kept, the next gen. */
static inline uint64_t net_rdzv_word_taken(uint64_t w) {
  return (w & (NET_RDZV_PASS_MAX << NET_RDZV_PASS_SHIFT)) |
         ((w + 1) & NET_RDZV_GEN_MASK);
}

/* One slot.  `word` decides; cb, arg and len are an expectation's, written by
 * its XP claimant before the publish CAS releases them and read by the
 * arrival after an acquire load of the X word it then takes by a CAS against
 * that exact word (relaxed atomics, so that a duplicate's read is defined and
 * its CAS fails). */
struct net_rdzv_slot_s {
  /* Aligned to its size, so no slot of a line-aligned level straddles two
   * lines. */
  _Alignas(32) _Atomic(uint64_t) word;
  _Atomic(uintptr_t) cb;
  _Atomic(uintptr_t) arg;
  _Atomic(uint64_t) len;
};

/* One level of the chain.  `next` is grow-only: every thread that finds it
 * NULL builds a zeroed level and CASes it in (acq_rel; an acquire load reads
 * it), and a loser frees its copy, which no other thread saw. */
struct net_rdzv_level_s {
  _Atomic(struct net_rdzv_level_s *) next;
  struct net_rdzv_slot_s slot[];
};

/* Level 0, made by arts_net_init before any runtime thread exists and freed
 * by quiesce once every runtime thread has passed the ready_to_clean barrier,
 * so a plain pointer: thread creation and the barrier order every use. */
static struct net_rdzv_level_s *g_rdzv_level0;

/* txid counter — starts at 1 so a full txid is never 0 (0 is the wire
 * sentinel for "no landing advertised / no payload moved").  A ticket: only
 * its own value. */
static _Atomic(uint64_t) g_rdzv_txid_ctr = 1;

static inline unsigned net_rdzv_level_log2(unsigned level) {
  return level == 0 ? ARTS_NET_RDZV_LEVEL0_LOG2 : ARTS_NET_RDZV_LEVEL_LOG2;
}

static struct net_rdzv_level_s *net_rdzv_level_new(unsigned level) {
  return (struct net_rdzv_level_s *)arts_calloc_aligned(
      1,
      sizeof(struct net_rdzv_level_s) +
          ((size_t)1 << net_rdzv_level_log2(level)) *
              sizeof(struct net_rdzv_slot_s),
      ARTS_CACHE_LINE_SIZE);
}

/* txid's slot at `level`: a multiplicative hash with a distinct odd
 * multiplier per level, so a receiver's consecutive txids scatter and two
 * txids that share a slot at one level rarely share one at the next.  The
 * shift is taken in 64 bits, so a one-slot level is index 0. */
static inline struct net_rdzv_slot_s *
net_rdzv_slot(struct net_rdzv_level_s *lv, unsigned level, uint32_t txid) {
  uint32_t h = txid * (UINT32_C(0x9E3779B1) * (2u * level + 1u));
  return &lv->slot[(uint64_t)h >> (32u - net_rdzv_level_log2(level))];
}

/* The level below lv, made on the first descend past lv's last level. */
static struct net_rdzv_level_s *net_rdzv_deeper(struct net_rdzv_level_s *lv,
                                                unsigned level) {
  struct net_rdzv_level_s *next =
      atomic_load_explicit(&lv->next, memory_order_acquire);
  if (next != NULL) {
    return next;
  }
  struct net_rdzv_level_s *mine = net_rdzv_level_new(level + 1);
  if (atomic_compare_exchange_strong_explicit(
          &lv->next, &next, mine, memory_order_acq_rel, memory_order_acquire)) {
    return mine;
  }
  arts_free(mine);
  return next;
}

enum net_rdzv_act {
  NET_RDZV_ACT_CLAIM,   /* EMPTY, no pass: park this event's record */
  NET_RDZV_ACT_TAKE,    /* the partner's ready record: take it and fire */
  NET_RDZV_ACT_MARK,    /* the partner's XP: mark it, its thread fires */
  NET_RDZV_ACT_DESCEND, /* not this txid's slot to take: pass it */
  NET_RDZV_ACT_DUP,     /* a second event of this event's kind */
};

/* What an event of `txid` (an expectation or an arrival) does at a slot whose
 * word is w, and the word it leaves. */
static enum net_rdzv_act net_rdzv_decide(uint64_t w, uint32_t txid, bool expect,
                                         uint64_t *next) {
  unsigned state = net_rdzv_word_state(w);
  if (state != NET_RDZV_EMPTY && net_rdzv_word_txid(w) == txid) {
    if (state == NET_RDZV_A || state == NET_RDZV_X) {
      if (expect != (state == NET_RDZV_A)) {
        return NET_RDZV_ACT_DUP;
      }
      *next = net_rdzv_word_taken(w);
      return NET_RDZV_ACT_TAKE;
    }
    if (expect || state == NET_RDZV_XPA) {
      return NET_RDZV_ACT_DUP;
    }
    *next = (w & ~NET_RDZV_STATE_MASK) | (uint64_t)NET_RDZV_XPA
                                             << NET_RDZV_STATE_SHIFT;
    return NET_RDZV_ACT_MARK;
  }
  if (state == NET_RDZV_EMPTY && net_rdzv_word_pass(w) == 0) {
    *next = net_rdzv_word_claimed(w, txid, expect ? NET_RDZV_XP : NET_RDZV_A);
    return NET_RDZV_ACT_CLAIM;
  }
  if (net_rdzv_word_pass(w) == NET_RDZV_PASS_MAX) {
    ARTS_ERROR("arts_net: %llu rendezvous records parked behind one slot",
               (unsigned long long)NET_RDZV_PASS_MAX);
  }
  *next = w + NET_RDZV_PASS_ONE;
  return NET_RDZV_ACT_DESCEND;
}

/* Add `delta` passes (a negative multiple of NET_RDZV_PASS_ONE) at txid's
 * slots of levels 0 .. depth - 1: the pair's own passes come back after a
 * take, or a dropped duplicate's own.  Relaxed: the take or the duplicate's
 * acquire already follows every pass removed here. */
static void net_rdzv_unpass(uint32_t txid, unsigned depth, uint64_t delta) {
  struct net_rdzv_level_s *lv = g_rdzv_level0;
  for (unsigned level = 0; level < depth; level++) {
    atomic_fetch_sub_explicit(&net_rdzv_slot(lv, level, txid)->word, delta,
                              memory_order_relaxed);
    lv = atomic_load_explicit(&lv->next, memory_order_acquire);
  }
}

/* The callback a completed pairing runs. */
struct net_rdzv_fire_s {
  void (*cb)(void *);
  void *arg;
  uint64_t len;
};

/* Raise one event of `txid`: an expectation when `exp` is non-NULL (its
 * callback), an arrival otherwise.  True when this event completed the
 * pairing: *out is then the callback to run, on this thread. */
static bool net_rdzv_event(uint64_t txid64, const struct net_rdzv_fire_s *exp,
                           struct net_rdzv_fire_s *out) {
  if (txid64 == 0 || txid64 > UINT32_MAX) {
    ARTS_ERROR("arts_net: rendezvous txid %llx is not a 32-bit draw",
               (unsigned long long)txid64);
  }
  const uint32_t txid = (uint32_t)txid64;
  const bool expect = exp != NULL;
  struct net_rdzv_level_s *lv = g_rdzv_level0;
  for (unsigned level = 0;; level++) {
    struct net_rdzv_slot_s *sl = net_rdzv_slot(lv, level, txid);
    uint64_t w = atomic_load_explicit(&sl->word, memory_order_acquire);
    for (;;) {
      uint64_t next = 0;
      enum net_rdzv_act act = net_rdzv_decide(w, txid, expect, &next);
      if (act == NET_RDZV_ACT_DESCEND) {
        if (atomic_compare_exchange_weak_explicit(&sl->word, &w, next,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
          break;
        }
        w = atomic_load_explicit(&sl->word, memory_order_acquire);
        continue;
      }
      if (act == NET_RDZV_ACT_DUP) {
        if (expect) {
          /* Each transfer registers exactly one continuation: two are a
           * protocol bug, never merged silently. */
          ARTS_ERROR("arts_net: duplicate rdzv expectation for txid %llx",
                     (unsigned long long)txid64);
        }
        ARTS_WARN("arts_net: duplicate rdzv data arrival for txid %llx "
                  "(dropped)",
                  (unsigned long long)txid64);
        net_rdzv_unpass(txid, level, NET_RDZV_PASS_ONE);
        return false;
      }
      struct net_rdzv_fire_s rec = {0};
      if (act == NET_RDZV_ACT_TAKE && !expect) {
        /* w was read by an acquire load: the fields it published are
         * visible, and the CAS against that exact word validates them. */
        rec.cb = (void (*)(void *))atomic_load_explicit(&sl->cb,
                                                        memory_order_relaxed);
        rec.arg = (void *)atomic_load_explicit(&sl->arg, memory_order_relaxed);
        rec.len = atomic_load_explicit(&sl->len, memory_order_relaxed);
        arts_sched_fuzz_point(); /* widen the read<->take window */
      }
      if (!atomic_compare_exchange_weak_explicit(&sl->word, &w, next,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        continue;
      }
      if (act == NET_RDZV_ACT_MARK) {
        return false; /* the expectation's thread fires */
      }
      if (act == NET_RDZV_ACT_TAKE) {
        net_rdzv_unpass(txid, level, 2 * NET_RDZV_PASS_ONE);
        *out = expect ? *exp : rec;
        return true;
      }
      /* Claimed. */
      if (!expect) {
        return false;
      }
      atomic_store_explicit(&sl->cb, (uintptr_t)exp->cb, memory_order_relaxed);
      atomic_store_explicit(&sl->arg, (uintptr_t)exp->arg,
                            memory_order_relaxed);
      atomic_store_explicit(&sl->len, exp->len, memory_order_relaxed);
      arts_sched_fuzz_point(); /* widen the claim<->publish window */
      w = atomic_load_explicit(&sl->word, memory_order_acquire);
      for (;;) {
        /* A descend of another txid may move the pass meanwhile; an arrival
         * of this one may mark the slot. */
        bool marked = net_rdzv_word_state(w) == NET_RDZV_XPA;
        uint64_t to = marked ? net_rdzv_word_taken(w)
                             : (w & ~NET_RDZV_STATE_MASK) |
                                   (uint64_t)NET_RDZV_X << NET_RDZV_STATE_SHIFT;
        if (atomic_compare_exchange_weak_explicit(&sl->word, &w, to,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
          if (!marked) {
            return false;
          }
          net_rdzv_unpass(txid, level, 2 * NET_RDZV_PASS_ONE);
          *out = *exp;
          return true;
        }
      }
    }
    lv = net_rdzv_deeper(lv, level);
  }
}

uint64_t arts_net_rdzv_txid_next(void) {
  uint64_t ctr =
      atomic_fetch_add_explicit(&g_rdzv_txid_ctr, 1, memory_order_relaxed);
  /* [1, 2^32-1]: never the wire sentinel 0, and a wrap collision would need
   * one transfer to stay in flight across 2^32-2 subsequent allocations by
   * this same rank — its landing buffer alone forbids that. */
  return 1u + (uint32_t)(ctr % 0xFFFFFFFFULL);
}

uint64_t arts_net_rdzv_txid_peek(void) {
  return atomic_load_explicit(&g_rdzv_txid_ctr, memory_order_relaxed);
}

void arts_net_rdzv_expect(uint64_t txid, uint64_t len,
                          void (*on_data)(void *), void *arg) {
  const struct net_rdzv_fire_s exp = {.cb = on_data, .arg = arg, .len = len};
  struct net_rdzv_fire_s fire;
  if (net_rdzv_event(txid, &exp, &fire)) {
    INCREMENT_BYTES_REMOTE_RECEIVED_BY(fire.len);
    fire.cb(fire.arg); /* data already landed: fire inline */
  }
}

/* Dispatch-side processing of a remote-write completion (imm == txid).  Runs
 * with the CQ token RELEASED (pending-list dispatch), so the paired callback —
 * which may install a coherence buffer and send — is safe here. */
static void net_rdzv_data_arrived(uint64_t txid) {
  struct net_rdzv_fire_s fire;
  if (net_rdzv_event(txid, NULL, &fire)) {
    INCREMENT_BYTES_REMOTE_RECEIVED_BY(fire.len);
    fire.cb(fire.arg);
  }
}

bool arts_net_rdzv_local(const void *p, uint64_t len, uint64_t *raddr,
                         uint64_t *rkey) {
  const arts_regpool_mr_t *m = arts_regpool_lookup(p);
  if (m == NULL || m->mr == NULL) {
    return false; /* unregistered (regpool off / no domain): no RDMA landing */
  }
  if ((const char *)p + len > (const char *)m->base + m->len) {
    ARTS_ERROR("arts_net: rdzv landing [%p +%llu) exceeds its registered slab",
               p, (unsigned long long)len);
  }
  /* Target-address semantics follow OUR negotiated mr_mode (homogeneous
   * build/provider across ranks): with FI_MR_VIRT_ADDR the peer targets our
   * virtual address; without it, the offset from the registered base. */
  *raddr = (g_net.mr_mode & FI_MR_VIRT_ADDR)
               ? (uint64_t)(uintptr_t)p
               : (uint64_t)((const char *)p - (const char *)m->base);
  *rkey = m->rkey;
  return true;
}

/* The pieces of one payload that the provider's ceiling cut apart.  Pieces
 * complete on whichever thread reaps the CQ, so the count is atomic; the
 * caller's hook runs on the thread that retires the last piece.  Each
 * completion's decrement is acq_rel, so the last one runs the hook and frees
 * the group after every other piece's completion. */
struct net_put_group_s {
  _Atomic(uint64_t) remaining;
  uint64_t posted_ns; /* the whole payload's post time; 0 = not timed */
  void (*on_local_done)(void *);
  void *arg;
};

static void net_put_piece_done(void *arg) {
  struct net_put_group_s *group = (struct net_put_group_s *)arg;
  if (atomic_fetch_sub_explicit(&group->remaining, 1,
                                memory_order_acq_rel) == 1) {
    net_put_retired(group->posted_ns);
    if (group->on_local_done != NULL) {
      group->on_local_done(group->arg);
    }
    arts_free(group);
  }
}

/* The one-sided PUT itself, counted by its caller and timed when `timed`;
 * false when teardown dropped it (the local-done hook has then run). */
static bool net_put(int rank, uint64_t raddr, uint64_t rkey, uint64_t txid,
                    const void *src, uint64_t len,
                    void (*on_local_done)(void *), void *arg, bool timed) {
  if (len == 0 || txid == 0) {
    ARTS_ERROR("arts_net: put_payload requires a payload and a txid "
               "(len=%llu txid=%llx)",
               (unsigned long long)len, (unsigned long long)txid);
  }
  /* Teardown has begun: drop the PUT but still run the local-done hook so the
   * source buffer's completion-gated release is not leaked (mirrors
   * arts_net_send_core's quiescing contract). */
  if (atomic_load_explicit(&g_net.quiescing, memory_order_relaxed)) {
    if (on_local_done != NULL) {
      on_local_done(arg);
    }
    return false;
  }
  fi_addr_t addr = g_net.peers[rank];
  size_t ceiling = g_net.max_msg;
  if (len <= ceiling) {
    struct net_txn_s *txn =
        net_txn_new(addr, (void *)src, (size_t)len, net_desc(src),
                    /*bounce=*/NULL, on_local_done, arg);
    txn->op = NET_TXN_WRITE;
    txn->raddr = raddr;
    txn->rkey = rkey;
    txn->imm = txid;
    txn->posted_ns = timed ? net_put_stamp() : 0;
    net_submit(txn, /*is_ack=*/false);
    return true;
  }
  /* Above the ceiling the payload goes as consecutive pieces of at most the
   * ceiling each, submitted in order from this thread to one endpoint, so
   * the peer's completion for the last piece — the only one carrying the
   * immediate — is generated after every earlier piece has been placed.
   * The caller's release hook runs once, when the last piece has left the
   * source. */
  if (!g_net.waw_ordered) {
    ARTS_ERROR("arts_net: a %llu-byte payload exceeds the provider's %zu-byte "
               "message ceiling, and the provider orders no write after "
               "write across that size (msg_order=0x%llx "
               "max_order_waw_size=%zu) — its pieces could land out of order",
               (unsigned long long)len, ceiling,
               (unsigned long long)g_net.tx_order, g_net.max_order_waw);
  }
  uint64_t pieces = (len + ceiling - 1) / ceiling;
  struct net_put_group_s *group =
      (struct net_put_group_s *)arts_malloc(sizeof(struct net_put_group_s));
  atomic_store_explicit(&group->remaining, pieces, memory_order_relaxed);
  group->posted_ns = timed ? net_put_stamp() : 0;
  group->on_local_done = on_local_done;
  group->arg = arg;
  for (uint64_t off = 0; off < len; off += ceiling) {
    size_t plen = (size_t)(len - off < ceiling ? len - off : ceiling);
    const char *piece = (const char *)src + off;
    bool last = off + plen == len;
    struct net_txn_s *txn =
        net_txn_new(addr, (void *)piece, plen, net_desc(piece),
                    /*bounce=*/NULL, net_put_piece_done, group);
    txn->op = last ? NET_TXN_WRITE : NET_TXN_WRITE_PLAIN;
    txn->raddr = raddr + off;
    txn->rkey = rkey;
    txn->imm = last ? txid : 0;
    net_submit(txn, /*is_ack=*/false);
  }
  return true;
}

void arts_net_put_payload(int rank, uint64_t raddr, uint64_t rkey,
                          uint64_t txid, const void *src, uint64_t len,
                          void (*on_local_done)(void *), void *arg) {
  /* Every datablock payload leaves through this one call, so it is where the
   * payload half of the traffic can be told apart from the control half that
   * BYTES_REMOTE_SENT counts with it. */
  if (net_put(rank, raddr, rkey, txid, src, len, on_local_done, arg,
              /*timed=*/true)) {
    INCREMENT_BYTES_DB_PAYLOAD_SENT_BY(len);
    INCREMENT_NUM_DB_PAYLOAD_SENT_BY(1);
    INCREMENT_BYTES_REMOTE_SENT_BY(len);
  }
}

static inline uint64_t net_now_ns(void) {
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* How long a live run may carry a completion error before it is judged lost
 * rather than a peer's teardown this rank has not heard of yet.  Shutdown
 * reaches every rank within a message hop of its recognition, so the window
 * only has to cover a slow hop, not a computation. */
#define ARTS_NET_LIVE_CQ_ERR_GRACE_NS (10ull * 1000000000ull)

/* Polled by every progress pass: a completion error recorded on a live run
 * becomes fatal once the grace period passes with no shutdown recognized. */
static inline void net_live_cq_err_check(void) {
  uint64_t t = atomic_load_explicit(&g_net.live_cq_err_ns, memory_order_acquire);
  if (t == 0 || t == NET_CQ_ERR_RECORDING) {
    return;
  }
  if (atomic_load_explicit(&arts_node_info.shutdown_state,
                           memory_order_acquire) != 0 ||
      atomic_load_explicit(&g_net.quiescing, memory_order_relaxed)) {
    return;
  }
  if (net_now_ns() - t < ARTS_NET_LIVE_CQ_ERR_GRACE_NS) {
    return;
  }
  ARTS_ERROR("arts_net: cq error on a live run, and no shutdown recognized "
             "within %llu s — a transfer was lost and nothing retransmits it: %s",
             (unsigned long long)(ARTS_NET_LIVE_CQ_ERR_GRACE_NS / 1000000000ull),
             g_net.live_cq_err_what);
}

/* End a reap burst that completed `n` transmits.  Only the CQ token holder,
 * or the teardown once every runtime thread has passed the ready_to_clean
 * barrier, writes tx_completed, so its own last value is current. */
static inline void net_count_completed(uint64_t n) {
  if (n != 0) {
    atomic_store_explicit(
        &g_net.tx_completed,
        atomic_load_explicit(&g_net.tx_completed, memory_order_relaxed) + n,
        memory_order_release);
  }
}

/* Transmits posted and not yet completed: never 0 while a counted post that
 * happens-before the call is outstanding.  tx_completed is read first: its
 * acquire makes every counted completion's post, its ring's registration and
 * its `posted` store visible, so the sum is at least the count; a refused
 * post seen without its give-back only overcounts. */
static uint64_t net_tx_outstanding(void) {
  uint64_t completed =
      atomic_load_explicit(&g_net.tx_completed, memory_order_acquire);
  uint32_t nr = atomic_load_explicit(&g_ring_count, memory_order_acquire);
  uint64_t posted = 0;
  for (uint32_t k = 0; k < nr; k++) {
    if (g_rings[k] != NULL) {
      posted += atomic_load_explicit(&g_rings[k]->posted, memory_order_relaxed);
    }
  }
  return posted - completed;
}

/* Reclaim a failed transmit's txn from a CQ error entry (both live and teardown
 * paths).  A failed send still owns its bookkeeping; reclaim it so it does not
 * leak.  Returns the transmits it completed, for its caller's burst count. */
static uint64_t net_reap_cq_err(void) {
  struct fi_cq_err_entry err;
  memset(&err, 0, sizeof(err));
  if (fi_cq_readerr(g_net.cq, &err, 0) == 1) {
    const char *what =
        fi_cq_strerror(g_net.cq, err.prov_errno, err.err_data, NULL, 0);
    ARTS_WARN("arts_net: cq error: %s (flags=0x%llx err=%d prov_errno=%d)",
              what, (unsigned long long)err.flags, err.err, err.prov_errno);
    /* A truncated receive is a control message the landing buffer could not
     * hold whole: its bytes are gone and the sender will not repeat them, so
     * no later shutdown recognition can make the run whole.  Fail now, with
     * the sizes, rather than let the grace period below turn it into a wedge
     * of unknown cause. */
    if ((err.flags & FI_RECV) && err.err == FI_ETRUNC) {
      ARTS_ERROR("arts_net: a %zu-byte inbound message was truncated by %zu "
                 "bytes (landing buffer too full to hold it whole): the "
                 "provider's multi-recv threshold is below the control "
                 "ceiling of %zu bytes",
                 err.len + err.olen, err.olen, (size_t)ARTS_NET_MSG_MAX);
    }
    /* An error completion that carries FI_MULTI_RECV retired its landing
     * buffer exactly as a successful one would have; without a repost the
     * rank runs on one buffer, then none. */
    if ((err.flags & FI_MULTI_RECV) && err.op_context != NULL) {
      net_post_recv(((struct net_recv_ctx_s *)err.op_context)->idx);
    }
    /* A failed transfer is a lost message, and nothing above the transport
     * retransmits: on a live run the outcome is a wedge that only the cell's
     * wall budget ends.  Once shutdown is recognized peers tear their
     * endpoints down in no agreed order, and a transfer flushed by that is
     * the expected residue — and a peer can be that far ahead of this rank,
     * so the first live error only starts the clock (net_live_cq_err_check). */
    if (atomic_load_explicit(&arts_node_info.shutdown_state,
                             memory_order_acquire) == 0 &&
        !atomic_load_explicit(&g_net.quiescing, memory_order_relaxed)) {
      uint64_t none = 0;
      if (atomic_compare_exchange_strong_explicit(
              &g_net.live_cq_err_ns, &none, NET_CQ_ERR_RECORDING,
              memory_order_acq_rel, memory_order_relaxed)) {
        snprintf(g_net.live_cq_err_what, sizeof(g_net.live_cq_err_what),
                 "%s (flags=0x%llx err=%d prov_errno=%d)", what,
                 (unsigned long long)err.flags, err.err, err.prov_errno);
        atomic_store_explicit(&g_net.live_cq_err_ns, net_now_ns(),
                              memory_order_release);
      }
    }
    if ((err.flags & (FI_SEND | FI_WRITE)) && err.op_context != NULL) {
      net_txn_complete((struct net_txn_s *)err.op_context);
      return 1;
    }
  }
  return 0;
}

/* Teardown-only reap: run once every worker/progress thread has passed the
 * ready_to_clean barrier, joined or not (no CQ token needed — none of them
 * touches the CQ again, and the barrier orders each one's last access before
 * this).  Completes
 * outstanding TX so completion-gated frees run; inbound completions are ignored
 * (no dispatch, no re-arm — the recvs are about to be cancelled).  Returns true
 * if any completion or error entry was seen. */
static bool net_reap_tx_only(void) {
  struct fi_cq_data_entry ents[ARTS_NET_CQ_BURST];
  ssize_t n = fi_cq_read(g_net.cq, ents, ARTS_NET_CQ_BURST);
  if (n > 0) {
    uint64_t done = 0;
    for (ssize_t i = 0; i < n; i++) {
      /* FI_SEND and (local) FI_WRITE completions both carry our txn as
       * op_context; a target-side FI_REMOTE_WRITE completion does not set
       * FI_WRITE, so this cannot mistake an inbound RMA event for a txn. */
      if (ents[i].flags & (FI_SEND | FI_WRITE)) {
        net_txn_complete((struct net_txn_s *)ents[i].op_context);
        done++;
      }
    }
    net_count_completed(done);
    return true;
  }
  if (n == -FI_EAVAIL) {
    net_count_completed(net_reap_cq_err());
    return true;
  }
  if (n != -FI_EAGAIN) {
    ARTS_ERROR("arts_net: fi_cq_read failed: %s", fi_strerror((int)-n));
  }
  return false;
}

#ifdef ARTS_DEBUG_INJECTION
/* ARTS_WRITE_COMPLETION_DELAY_MS=<n> holds each one-sided write's local
 * completion for a random 0..n ms before running it.  The fabric orders nothing
 * between a write's local completion and the messages its target sends once
 * the bytes have landed, so whatever a completion releases must stay valid
 * however late it runs; a provider that reports completions promptly never
 * shows that order.  A held completion stays outstanding until it runs, and
 * quiesce runs every one still held before the endpoint closes. */
struct net_held_write_s {
  struct net_held_write_s *next;
  struct net_txn_s *txn;
  uint64_t due_ns;
};
/* Touched under the CQ token, or at quiesce once every runtime thread has
 * passed the ready_to_clean barrier. */
static struct net_held_write_s *g_held_writes;
static uint64_t g_write_delay_rng;
/* Set when the fabric comes up, before any thread exists to reap; 0 = off. */
static int64_t g_write_delay_ns;

static void net_write_delay_init(void) {
  const char *env = getenv("ARTS_WRITE_COMPLETION_DELAY_MS");
  long ms = 0;
  if (env != NULL && env[0] != '\0') {
    char *end = NULL;
    ms = strtol(env, &end, 10);
    if (end == env || *end != '\0' || ms < 0 || ms > 60000) {
      ARTS_ERROR("arts_net: ARTS_WRITE_COMPLETION_DELAY_MS=\"%s\" is not a "
                 "delay of 0-60000 ms",
                 env);
    }
  }
  g_write_delay_ns = (int64_t)ms * 1000000;
  g_write_delay_rng = net_now_ns() | 1u;
}

static bool net_hold_write(struct net_txn_s *txn) {
  if (g_write_delay_ns == 0) {
    return false;
  }
  struct net_held_write_s *h =
      (struct net_held_write_s *)arts_malloc(sizeof(struct net_held_write_s));
  h->txn = txn;
  g_write_delay_rng ^= g_write_delay_rng << 13;
  g_write_delay_rng ^= g_write_delay_rng >> 7;
  g_write_delay_rng ^= g_write_delay_rng << 17;
  h->due_ns =
      net_now_ns() + g_write_delay_rng % (uint64_t)(g_write_delay_ns + 1);
  h->next = g_held_writes;
  g_held_writes = h;
  return true;
}

/* Run the held completions that are due, or every one with `all`; returns
 * how many ran, for its caller's burst count. */
static uint64_t net_run_held_writes(bool all) {
  if (g_held_writes == NULL) {
    return 0;
  }
  uint64_t done = 0;
  uint64_t now = all ? UINT64_MAX : net_now_ns();
  for (struct net_held_write_s **pp = &g_held_writes; *pp != NULL;) {
    struct net_held_write_s *h = *pp;
    if (h->due_ns > now) {
      pp = &h->next;
      continue;
    }
    *pp = h->next;
    net_txn_complete(h->txn);
    done++;
    arts_free(h);
  }
  return done;
}
/* Quiesce's turn: run every held completion.  Every runtime thread has
 * passed the ready_to_clean barrier, joined or not, so none reaps again. */
static void net_quiesce_held_writes(void) {
  if (g_write_delay_ns == 0) {
    return;
  }
  net_count_completed(net_run_held_writes(true));
}
#else
#define net_write_delay_init() ((void)0)
#define net_hold_write(txn) false
#define net_run_held_writes(all) ((uint64_t)0)
#define net_quiesce_held_writes() ((void)0)
#endif

#ifdef ARTS_DEBUG_INJECTION
/* ARTS_REQUEST_DELAY_MS=<n> holds each coherence request a rank sends to
 * another rank for a random 0..n ms, so two requests of one rank reach their
 * home in either order and a request can trail everything its rank sends
 * after it.  ARTS_REQUEST_DELAY_MS=<n>:ro holds only read requests, each for
 * exactly n ms, so a write turn always answers first; <n>:hop holds only
 * hops, each for exactly n ms, so a hop can cross the block's teardown;
 * <n>:trail holds each read request until its rank's write request for the
 * same block to the same rank leaves, and sends it right behind that one
 * from the same thread, so a target that dispatches with one progress thread
 * takes the write request in first whatever the timing, given the
 * send-after-send order the provider grants (a read request that no write
 * request follows leaves after n ms).  <n>:drain holds each read request
 * until as many EDT bodies have returned on its rank as EDTs had been made
 * runnable there when it was held (or n ms pass), so an acquire made after it
 * in the body that sent it, or in a body runnable by then, is decided before
 * it leaves whatever the timing.  The trigger is a count, not an identity:
 * it names those bodies only while nothing else runs on the rank.
 * Producers (any thread, a progress thread in dispatch included) only copy
 * and push; a progress thread that wins the try-lock moves the
 * pushed packets onto the not-yet-due list, detaches what is due, drops the
 * try-lock and only then sends, so no send runs under the CQ token or the
 * try-lock.  A held packet is not counted outstanding until it is sent, and
 * quiesce frees what is still held (an undelivered message at shutdown is
 * legal): every runtime thread has passed the ready_to_clean barrier by then,
 * so none holds or pushes one again. */
struct net_held_msg_s {
  _Atomic(struct net_held_msg_s *) next; /* the producers' stack */
  struct net_held_msg_s *wnext; /* the wait and due lists: the lock's own */
  uint64_t due_ns;
  uint64_t runnable; /* the drain form: g_held_runnable at the hold */
  arts_guid_t guid;
  int rank;
  int kind;
  unsigned int len;
  char data[];
};
/* Producers' stack: pushed by CAS (release), taken whole by exchange
 * (acquire) under the try-lock. */
static _Atomic(struct net_held_msg_s *) g_held_push;
/* The not-yet-due list: touched only by the try-lock's holder. */
static struct net_held_msg_s *g_held_wait;
/* The try-lock: acquired by CAS 0->1, dropped by a release store. */
static _Atomic(unsigned int) g_held_lock;
/* Set before any thread exists to send; 0 = off. */
static int64_t g_req_delay_ns;
/* Which kind is held: every request (randomly) or one kind exactly; or, with
 * the trail form, read requests until their write request leaves, or with the
 * drain form, read requests until the bodies counted at their hold returned. */
static int g_req_delay_mode = -1;
static bool g_req_delay_trail;
static bool g_req_delay_drain;
static ARTS_THREAD_LOCAL uint64_t t_req_delay_rng;
/* The drain form's counts, only while it is set.  runnable: EDTs this rank
 * made runnable, bumped relaxed by the thread that schedules one (before the
 * push that hands it on, so the body that sends a held request is counted)
 * and read relaxed at the hold, a value only.  returned: bodies that
 * returned, bumped with release by the thread that ran one, right after the
 * body; the try-lock holder loads it with acquire before it sends what is
 * due, so the send follows every acquire those bodies made. */
static _Atomic(uint64_t) g_held_runnable;
static _Atomic(uint64_t) g_held_returned;

void arts_net_held_edt_runnable(void) {
  if (g_req_delay_drain) {
    atomic_fetch_add_explicit(&g_held_runnable, 1u, memory_order_relaxed);
  }
}

void arts_net_held_edt_returned(void) {
  if (g_req_delay_drain) {
    atomic_fetch_add_explicit(&g_held_returned, 1u, memory_order_release);
  }
}

static void net_req_delay_init(void) {
  const char *env = getenv("ARTS_REQUEST_DELAY_MS");
  long ms = 0;
  if (env != NULL && env[0] != '\0') {
    char *end = NULL;
    ms = strtol(env, &end, 10);
    if (end != env && strcmp(end, ":ro") == 0) {
      g_req_delay_mode = ARTS_HELD_READ;
      end += 3;
    } else if (end != env && strcmp(end, ":hop") == 0) {
      g_req_delay_mode = ARTS_HELD_HOP;
      end += 4;
    } else if (end != env && strcmp(end, ":trail") == 0) {
      g_req_delay_trail = true;
      end += 6;
    } else if (end != env && strcmp(end, ":drain") == 0) {
      g_req_delay_mode = ARTS_HELD_READ;
      g_req_delay_drain = true;
      end += 6;
    }
    if (end == env || *end != '\0' || ms < 0 || ms > 60000) {
      ARTS_ERROR("arts_net: ARTS_REQUEST_DELAY_MS=\"%s\" is not a delay of "
                 "0-60000 ms, optionally followed by :ro, :hop, :trail or "
                 ":drain",
                 env);
    }
  }
  g_req_delay_ns = (int64_t)ms * 1000000;
}

bool arts_net_send_held(int rank, const char *message, unsigned int length,
                        int kind, arts_guid_t guid) {
  bool trail = g_req_delay_trail;
  bool exact = g_req_delay_mode >= 0;
  if (g_req_delay_ns == 0 || (exact && kind != g_req_delay_mode) ||
      (!exact && kind == ARTS_HELD_HOP)) {
    return false;
  }
  struct net_held_msg_s *h =
      (struct net_held_msg_s *)arts_malloc(sizeof(*h) + length);
  memcpy(h->data, message, length);
  h->guid = guid;
  h->rank = rank;
  h->kind = kind;
  h->len = length;
  if (t_req_delay_rng == 0) {
    t_req_delay_rng = (net_now_ns() ^ ((uint64_t)(uintptr_t)&h)) | 1u;
  }
  t_req_delay_rng ^= t_req_delay_rng << 13;
  t_req_delay_rng ^= t_req_delay_rng >> 7;
  t_req_delay_rng ^= t_req_delay_rng << 17;
  uint64_t hold = (uint64_t)g_req_delay_ns;
  if (trail) {
    hold = kind == ARTS_HELD_WRITE ? 0u : hold;
  } else if (!exact) {
    hold = t_req_delay_rng % (hold + 1u);
  }
  h->due_ns = net_now_ns() + hold;
  h->runnable = g_req_delay_drain ? atomic_load_explicit(&g_held_runnable,
                                                         memory_order_relaxed)
                                  : 0u;
  struct net_held_msg_s *top =
      atomic_load_explicit(&g_held_push, memory_order_relaxed);
  do {
    atomic_store_explicit(&h->next, top, memory_order_relaxed);
  } while (!atomic_compare_exchange_weak_explicit(
      &g_held_push, &top, h, memory_order_release, memory_order_relaxed));
  return true;
}

/* Send the held packets that are due. */
static void net_release_held(void) {
  if (g_req_delay_ns == 0) {
    return;
  }
  unsigned int free_lock = 0u;
  if (!atomic_compare_exchange_strong_explicit(&g_held_lock, &free_lock, 1u,
                                               memory_order_acquire,
                                               memory_order_relaxed)) {
    return;
  }
  struct net_held_msg_s *in = atomic_exchange_explicit(
      &g_held_push, (struct net_held_msg_s *)NULL, memory_order_acquire);
  while (in != NULL) {
    struct net_held_msg_s *nx =
        atomic_load_explicit(&in->next, memory_order_relaxed);
    in->wnext = g_held_wait;
    g_held_wait = in;
    in = nx;
  }
  uint64_t now = net_now_ns();
  uint64_t returned =
      g_req_delay_drain
          ? atomic_load_explicit(&g_held_returned, memory_order_acquire)
          : 0u;
  struct net_held_msg_s *due = NULL;
  for (struct net_held_msg_s **pp = &g_held_wait; *pp != NULL;) {
    struct net_held_msg_s *h = *pp;
    if (h->due_ns > now && !(g_req_delay_drain && returned >= h->runnable)) {
      pp = &h->wnext;
      continue;
    }
    *pp = h->wnext;
    h->wnext = due;
    due = h;
  }
  if (g_req_delay_trail) {
    /* A read request was pushed before its rank's write request, so by the
     * time the write request is taken the read request is on the wait list. */
    for (struct net_held_msg_s *w = due; w != NULL; w = w->wnext) {
      if (w->kind != ARTS_HELD_WRITE) {
        continue;
      }
      for (struct net_held_msg_s **pp = &g_held_wait; *pp != NULL;) {
        struct net_held_msg_s *h = *pp;
        if (h->kind != ARTS_HELD_READ || h->guid != w->guid ||
            h->rank != w->rank) {
          pp = &h->wnext;
          continue;
        }
        *pp = h->wnext;
        h->wnext = w->wnext;
        w->wnext = h;
        w = h;
      }
    }
  }
  atomic_store_explicit(&g_held_lock, 0u, memory_order_release);
  while (due != NULL) {
    struct net_held_msg_s *nx = due->wnext;
    /* Only a remote branch holds a message, so the target is another rank. */
    arts_net_send_core(due->rank, due->data, due->len, NULL, 0, 0, NULL);
    arts_free(due);
    due = nx;
  }
}

static void net_free_held_push(struct net_held_msg_s *h) {
  while (h != NULL) {
    struct net_held_msg_s *nx =
        atomic_load_explicit(&h->next, memory_order_relaxed);
    arts_free(h);
    h = nx;
  }
}

static void net_free_held_wait(struct net_held_msg_s *h) {
  while (h != NULL) {
    struct net_held_msg_s *nx = h->wnext;
    arts_free(h);
    h = nx;
  }
}

static void net_quiesce_held_requests(void) {
  if (g_req_delay_ns == 0) {
    return;
  }
  /* After the ready_to_clean barrier, whose arrivals order every push and
   * every try-lock holder's list before these accesses. */
  net_free_held_push(atomic_exchange_explicit(
      &g_held_push, (struct net_held_msg_s *)NULL, memory_order_relaxed));
  net_free_held_wait(g_held_wait);
  g_held_wait = NULL;
}
#else
#define net_req_delay_init() ((void)0)
#define net_release_held() ((void)0)
#define net_quiesce_held_requests() ((void)0)
#endif

/* Reap ONE bounded CQ burst under the CQ token (caller MUST hold it): validate
 * and copy each inbound message onto the pending-dispatch list, repost released
 * landing buffers, and complete TX.  Does NOT dispatch — the caller decides
 * when (net_reap_and_dispatch dispatches after releasing the token; net_reap_no_
 * dispatch never dispatches).  The RX bytes are copied out while the token is
 * held so the multi-recv buffer can be reposted immediately with no borrow
 * outliving the reap.  Returns true if any completion or error entry was seen. */
static bool net_reap_locked(void) {
  uint64_t done = net_run_held_writes(false);
  /* Retry any landing-buffer repost the provider EAGAIN-deferred: this runs
   * under the CQ token on every reap pass, and each pass drains completions —
   * exactly the action that frees the provider queue slots the repost needs.
   * net_post_recv re-marks the bit if the provider is still saturated. */
  if (g_net.rx_repost_pending != 0) {
    unsigned pending = g_net.rx_repost_pending;
    g_net.rx_repost_pending = 0;
    for (unsigned i = 0; i < ARTS_NET_RECV_BUF_COUNT; i++) {
      if (pending & (1u << i)) {
        net_post_recv(i);
      }
    }
  }
  struct fi_cq_data_entry ents[ARTS_NET_CQ_BURST];
  ssize_t n = fi_cq_read(g_net.cq, ents, ARTS_NET_CQ_BURST);
  if (n > 0) {
    for (ssize_t i = 0; i < n; i++) {
      struct fi_cq_data_entry *e = &ents[i];
      /* Copy the message out BEFORE the buffer can be reposted (an entry can
       * carry both FI_RECV and FI_MULTI_RECV, and the multi-recv completion for
       * a buffer always follows the FI_RECV completions that landed in it, so
       * copy-then-repost order holds within and across entries in this burst). */
      if (e->flags & FI_RECV) {
        /* Framing guard (PERMANENT).  A whole fi_send lands contiguously in one
         * landing buffer, so the peer-declared header.size MUST equal the byte
         * count the fabric actually landed.  A mismatch means a framing bug —
         * e.g. a payload split across frames the connectionless RX cannot rejoin
         * — so fail loud rather than dispatch garbage (a handler reading
         * header.size would run off the end of this copy). */
        if (e->len < sizeof(struct arts_msg_header_s)) {
          ARTS_ERROR("arts_net: RX runt frame — landed %zu bytes < header %zu",
                     e->len, sizeof(struct arts_msg_header_s));
        }
        const struct arts_msg_header_s *hdr =
            (const struct arts_msg_header_s *)e->buf;
        if (hdr->size != e->len) {
          ARTS_ERROR("arts_net: RX framing mismatch — header.size=%llu but "
                     "fabric landed %zu bytes (type=%u from rank=%u)",
                     (unsigned long long)hdr->size, e->len, hdr->message_type,
                     hdr->rank);
        }
        struct net_pending_s *pn = (struct net_pending_s *)arts_malloc(
            sizeof(struct net_pending_s) + e->len);
        pn->next = NULL;
        pn->len = (unsigned)e->len;
        pn->rdzv_txid = 0;
        memcpy(pn + 1, e->buf, e->len);
        if (g_pending_tail != NULL) {
          g_pending_tail->next = pn;
        } else {
          g_pending_head = pn;
        }
        g_pending_tail = pn;
      }
      if (e->flags & FI_MULTI_RECV) {
        net_post_recv(((struct net_recv_ctx_s *)e->op_context)->idx);
      }
      /* Target-side completion of a peer's fi_writedata: the payload bytes are
       * already fully placed in the advertised landing buffer ("imm seen =>
       * landing valid" — the ONLY transport ordering this plane relies on).
       * Queue a rendezvous-data node carrying the immediate (txid); the txid
       * pairing — which may run an install callback that sends — happens on
       * the dispatch side, after the token is released.  A remote write
       * without immediate data is a leading piece of a payload the sender's
       * ceiling cut apart (where the provider reports one at all): nothing
       * pairs on it, since only the last piece's immediate says the whole
       * landing is valid, so it is ignored rather than paired on a zero
       * txid. */
      if ((e->flags & FI_REMOTE_WRITE) && (e->flags & FI_REMOTE_CQ_DATA)) {
        /* The bytes are counted where the pairing completes, from the length
         * its metadata names: this completion reports the last piece only. */
        struct net_pending_s *pn =
            (struct net_pending_s *)arts_malloc(sizeof(struct net_pending_s));
        pn->next = NULL;
        pn->len = 0;
        pn->rdzv_txid = e->data;
        if (g_pending_tail != NULL) {
          g_pending_tail->next = pn;
        } else {
          g_pending_head = pn;
        }
        g_pending_tail = pn;
      }
      if ((e->flags & (FI_SEND | FI_WRITE)) &&
          !((e->flags & FI_WRITE) &&
            net_hold_write((struct net_txn_s *)e->op_context))) {
        net_txn_complete((struct net_txn_s *)e->op_context);
        done++;
      }
    }
    net_count_completed(done);
    return true;
  }
  if (n == -FI_EAVAIL) {
    net_count_completed(done + net_reap_cq_err());
    return true;
  }
  net_count_completed(done);
  if (n != -FI_EAGAIN) {
    ARTS_ERROR("arts_net: fi_cq_read failed: %s", fi_strerror((int)-n));
  }
  return false;
}

/* Live reap-and-dispatch — the progress loop's inbound driver.
 *
 * MANDATORY INVARIANT — no send runs under the CQ consumer lock.  An inbound
 * coherence handler may itself send (e.g. a snapshot response), and a send can
 * block in net_submit's backpressure spin; running that under the CQ token would
 * self-deadlock the very reap that frees the ring / re-arms the landing buffers.
 * So the token guards ONLY the CQ read plus the copy-out, repost, and pending-
 * list bookkeeping; dispatch happens AFTER the token is released.
 *
 * The whole pending list is spliced out under the token (this burst's messages
 * at the tail, plus anything a backpressured producer's net_reap_no_dispatch
 * left at the head) and dispatched FIFO with the token released, preserving
 * per-peer arrival order (oldest first).  Returns true if it reaped or
 * dispatched anything. */
static bool net_reap_and_dispatch(void) {
  if (!net_cq_trylock()) {
    return false; /* another progress thread is consuming the CQ */
  }
  bool progressed = net_reap_locked();
  struct net_pending_s *head = g_pending_head;
  g_pending_head = NULL;
  g_pending_tail = NULL;
  net_cq_unlock();

  while (head != NULL) {
    struct net_pending_s *next = head->next;
    if (head->rdzv_txid != 0) {
      net_rdzv_data_arrived(head->rdzv_txid);
    } else {
      INCREMENT_BYTES_REMOTE_RECEIVED_BY(head->len);
      INCREMENT_NUM_REMOTE_RECEIVE_BY(1);
      arts_transport_dispatch_packet((struct arts_msg_header_s *)(head + 1));
    }
    arts_free(head);
    head = next;
    progressed = true;
  }
  return progressed;
}

/* Backpressure-assist reap: reap a bounded CQ burst under the token but do NOT
 * dispatch — leave the inbound messages on the pending list for the progress
 * loop.  Safe from any thread that does not already hold the CQ token (dispatch
 * always runs with the token released, so a producer stalled in net_submit never
 * holds it, and a worker never touches the CQ except here).  Completing TX frees
 * the ring slots the caller is spinning on; draining RX advances a mutually
 * saturated peer's socket so its wedged TX progresses.  Returns true if it
 * reaped anything. */
static bool net_reap_no_dispatch(void) {
  if (!net_cq_trylock()) {
    return false; /* the progress thread (or another producer) has the CQ */
  }
  bool progressed = net_reap_locked();
  net_cq_unlock();
  return progressed;
}

bool arts_net_progress(void) {
  /* Reap the CQ (single-consumer; copy-out RX under the token, dispatch after
   * release), then drain every registered retry ring (this thread's and any
   * other thread's whose owner is not currently sending).  Non-blocking by
   * contract: idle pacing is the caller's policy (the dedicated progress
   * thread busy-polls with the architectural pause hint — a sleeping poll here
   * would put a fixed per-hop latency floor under every message an idle rank
   * receives, which multiplies across latency-bound message chains). */
  net_live_cq_err_check();
  bool did = net_reap_and_dispatch();
  net_release_held();
  did |= net_drain_all_rings();
  return did;
}

/* ------------------------------------------------------------------------- */
/* Init-time one-sided selfcheck                                               */
/* ------------------------------------------------------------------------- */

/* Remove and free THIS txid's rendezvous-data node from the pending list, if
 * present.  Token-guarded like every pending-list mutation; any other node
 * (none should exist this early) is left for the progress loop. */
static bool net_pending_take_rdzv(uint64_t txid) {
  if (!net_cq_trylock()) {
    return false;
  }
  bool found = false;
  struct net_pending_s *prev = NULL;
  for (struct net_pending_s *n = g_pending_head; n != NULL;
       prev = n, n = n->next) {
    if (n->rdzv_txid == txid) {
      if (prev != NULL) {
        prev->next = n->next;
      } else {
        g_pending_head = n->next;
      }
      if (g_pending_tail == n) {
        g_pending_tail = prev;
      }
      arts_free(n);
      found = true;
      break;
    }
  }
  net_cq_unlock();
  return found;
}

static void net_selfcheck_arrived(void *arg) { *(bool *)arg = true; }

void arts_net_selfcheck(void) {
  if (!g_net.is_cxi) {
    return;
  }
  /* A provider can advertise an immediate-data size and still have its
   * write-with-immediate op configured out at runtime (an environment gate),
   * which nothing before the first real transfer would surface.  One
   * self-addressed PUT through the ordinary rendezvous plane proves both op
   * availability and the plane's one ordering axiom ("imm seen => landing
   * valid") at init, where the failure carries a name instead of killing the
   * first payload mid-application.  Runs single-threaded, after the address
   * exchange and before any runtime thread exists, so it reaps the CQ
   * without dispatching (a dispatch would run peer handlers against a
   * runtime not yet built) and consumes its own rendezvous node directly. */
  enum { SELFCHECK_LEN = 64 };
  uint8_t *dst = (uint8_t *)arts_regpool_alloc_aligned(SELFCHECK_LEN, 64);
  uint8_t *src = (uint8_t *)arts_regpool_alloc_aligned(SELFCHECK_LEN, 64);
  if (dst == NULL || src == NULL) {
    ARTS_ERROR("arts_net: selfcheck buffer allocation failed");
  }
  memset(src, 0x5A, SELFCHECK_LEN);
  memset(dst, 0, SELFCHECK_LEN);
  uint64_t raddr = 0;
  uint64_t rkey = 0;
  if (!arts_net_rdzv_local(dst, SELFCHECK_LEN, &raddr, &rkey)) {
    ARTS_ERROR("arts_net: selfcheck landing is not fabric-registered");
  }
  uint64_t txid = arts_net_rdzv_txid_next();
  /* Set by the expectation's callback on this thread: the selfcheck runs
   * before any other thread can reap or pair. */
  bool done = false;
  /* A probe of the plane, not a datablock payload: no counter sees it. */
  arts_net_rdzv_expect(txid, 0, net_selfcheck_arrived, &done);
  (void)net_put((int)arts_global_rank_id, raddr, rkey, txid, src,
                SELFCHECK_LEN, NULL, NULL, /*timed=*/false);
  struct timespec start;
  (void)clock_gettime(CLOCK_MONOTONIC, &start);
  while (!done) {
    net_reap_no_dispatch();
    if (net_pending_take_rdzv(txid)) {
      net_rdzv_data_arrived(txid); /* fires the expectation on this stack */
    }
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - start.tv_sec >= 5) {
      ARTS_ERROR("arts_net: one-sided write-with-immediate against this "
                 "rank's own memory did not complete in 5s — the provider's "
                 "immediate path is unavailable or unpaired (on cxi: the "
                 "FI_CXI_ENABLE_WRITEDATA gate, or no FI_MR_PROV_KEY "
                 "instance)");
    }
  }
  for (unsigned i = 0; i < SELFCHECK_LEN; i++) {
    if (dst[i] != 0x5A) {
      ARTS_ERROR("arts_net: selfcheck landing byte %u is %02x, not the "
                 "written pattern — the immediate outran the write's bytes",
                 i, dst[i]);
    }
  }
  arts_regpool_free(dst);
  arts_regpool_free(src);
  ARTS_INFO("arts_net: one-sided immediate selfcheck passed");
}

/* ------------------------------------------------------------------------- */
/* Bootstrap glue                                                              */
/* ------------------------------------------------------------------------- */

unsigned arts_net_own_address(void *buf, unsigned buflen) {
  size_t len = buflen;
  int rc = fi_getname(&g_net.ep->fid, buf, &len);
  if (rc != 0) {
    ARTS_ERROR("arts_net: fi_getname failed: %s", fi_strerror(-rc));
  }
  return (unsigned)len;
}

void arts_net_av_insert_table(const void *addrs, unsigned addrlen,
                              unsigned count) {
  fi_addr_t *fiaddrs = (fi_addr_t *)arts_malloc(sizeof(fi_addr_t) * count);
  int inserted = fi_av_insert(g_net.av, addrs, count, fiaddrs, 0, NULL);
  if (inserted != (int)count) {
    ARTS_ERROR("arts_net: fi_av_insert inserted %d of %u addresses", inserted,
               count);
  }
  g_net.peers = (fi_addr_t *)arts_malloc(sizeof(fi_addr_t) * count);
  for (unsigned i = 0; i < count; i++) {
    /* FI_AV_TABLE assigns sequential handles from 0, so inserting in rank order
     * yields fi_addr_t == rank; assert rather than silently mis-route. */
    if (fiaddrs[i] != (fi_addr_t)i) {
      ARTS_ERROR("arts_net: AV handle %u != rank %u (FI_AV_TABLE assumption "
                 "violated)",
                 (unsigned)fiaddrs[i], i);
    }
    g_net.peers[i] = fiaddrs[i];
  }
  g_net.peer_count = count;
  arts_free(fiaddrs);
  /* fi_av_insert strides `addrs` by the provider's own address length; the
   * caller must have laid the table out at exactly that stride. */
  (void)addrlen;
}

struct fid_domain *arts_net_domain(void) { return g_net.domain; }

/* ------------------------------------------------------------------------- */
/* Init / teardown                                                             */
/* ------------------------------------------------------------------------- */

void arts_net_init(const char *provider, const char *fabric_domain,
                   const char *net_interface, bool one_host) {
  net_write_delay_init();
  net_req_delay_init();
  if (g_rdzv_level0 == NULL) {
    g_rdzv_level0 = net_rdzv_level_new(0);
  }
  bool provider_pinned = provider && provider[0] != '\0';
  if (provider_pinned) {
    /* Config is the deliberate artifact, the env var is ambient: overwrite
     * rather than merely default, so an explicit choice here always wins
     * even if FI_PROVIDER was already set to something else. */
    setenv("FI_PROVIDER", provider, 1);
  }
  /* Provider precedence: cfg > FI_PROVIDER env > tcp-first-auto.  With no
   * explicit choice anywhere, fi_getinfo's unconstrained pick is whatever
   * provider happens to sort first on the host (e.g. a datagram emulation
   * stack) — an unpredictable default for a transport with delivery-robustness
   * differences between providers.  Prefer the connection-oriented core "tcp"
   * provider first; fall back to unconstrained auto-selection only when tcp
   * yields no usable RDM info on this host. */
  const char *env_provider = getenv("FI_PROVIDER");
  bool env_pinned =
      !provider_pinned && env_provider != NULL && env_provider[0] != '\0';
  bool tcp_first = !provider_pinned && !env_pinned;

  /* The provider the request names — needed BEFORE the first libfabric call:
   * cxi requires two adaptations committed ahead of matching (an environment
   * gate read when the provider library initializes, and an mr_mode offer),
   * and the interface-bind decision below keys on the same string. */
  const char *effective_provider =
      provider_pinned ? provider : (env_pinned ? env_provider : NULL);
  bool cxi_effective = net_provider_names_cxi(effective_provider);
  if (cxi_effective) {
    /* The data plane is one-sided fi_writedata; the cxi provider compiles
     * that op out of its RMA ops unless this variable is set when the
     * provider library initializes (first fi_getinfo).  An ambient 0 is
     * always wrong for this runtime, so overwrite deliberately. */
    setenv("FI_CXI_ENABLE_WRITEDATA", "1", 1);
    ARTS_INFO("arts_net: cxi requested — FI_CXI_ENABLE_WRITEDATA=1");
  }

  struct fi_info *hints = fi_allocinfo();
  if (hints == NULL) {
    ARTS_ERROR("arts_net: fi_allocinfo failed");
  }
  /* FI_RMA_EVENT: the rendezvous data plane's target side needs a CQ
   * completion (with the immediate txid) when a peer's fi_writedata lands —
   * "imm seen => landing buffer valid" is the plane's one ordering axiom. */
  hints->caps = FI_MSG | FI_RMA | FI_MULTI_RECV | FI_RMA_EVENT;
  hints->mode = 0;
  hints->ep_attr->type = FI_EP_RDM;
  hints->domain_attr->threading = FI_THREAD_SAFE;
  /* Request send-after-send ordering (FI_ORDER_SAS): two messages this rank
   * sends to a given peer are delivered to that peer's endpoint in send order
   * (per-EP-pair FIFO).  The coherence rounds that ship a state update and
   * then its acknowledgement to the same peer from the same thread rely on
   * it.  The value the provider actually grants is stored and logged below; a
   * provider that does not honor it would surface here rather than silently.
   */
  hints->tx_attr->msg_order = FI_ORDER_SAS;
  hints->rx_attr->msg_order = FI_ORDER_SAS;
  /* Offer the common mr_mode bits and accept whatever subset the provider
   * requires (the tcp provider requires none).  FI_MR_ENDPOINT is offered
   * only when the request names a provider that requires it: a layering
   * provider (rxm) propagates the offered set into what it asks its core
   * for, so an unconditional offer would perturb stacks this change has no
   * way to regression-test. */
  hints->domain_attr->mr_mode =
      FI_MR_LOCAL | FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_VIRT_ADDR |
      (cxi_effective ? FI_MR_ENDPOINT : 0);
  if (provider_pinned) {
    /* fi_freeinfo() below frees fabric_attr->prov_name itself, so it must be
     * a heap string fi_getinfo/fi_freeinfo owns from here on, not the cfg's
     * storage. */
    hints->fabric_attr->prov_name = strdup(provider);
  } else if (tcp_first) {
    hints->fabric_attr->prov_name = strdup("tcp");
  }
  if (fabric_domain && fabric_domain[0] != '\0') {
    /* Same ownership rule as prov_name: fi_freeinfo frees domain_attr->name. */
    hints->domain_attr->name = strdup(fabric_domain);
  }

  /* Source-interface bind.  The effective provider addresses by IP when it
   * was explicitly named tcp/sockets, or when the tcp-first default is in
   * play; the verbs stack does when its connections are to be resolved over
   * an IP interface (IPoIB, RoCE).  For anything else (cxi, ...) an interface
   * name is meaningless — addressing there is by fabric domain. */
  bool verbs_effective = net_provider_names_verbs(effective_provider);
  bool ip_provider =
      tcp_first ||
      (effective_provider != NULL && (strcmp(effective_provider, "tcp") == 0 ||
                                      strcmp(effective_provider, "sockets") == 0));
  bool bind_interface =
      net_interface != NULL && (ip_provider || verbs_effective);
  /* Every rank must open the same interface, and the provider's own pick is
   * each process's view of the interfaces up at that instant, which a bridge
   * coming or going changes between two ranks' starts; a pair that splits
   * across interfaces can reach each other only through address translation,
   * which hides the peer's address from the provider.  All ranks of a
   * one-host run carry loopback, and nothing rewrites it. */
  bool bind_loopback = !bind_interface && one_host && ip_provider &&
                       (fabric_domain == NULL || fabric_domain[0] == '\0');
  if (bind_interface || bind_loopback) {
    /* fi_freeinfo frees hints->src_addr, so it must be heap-owned. */
    struct sockaddr_in *src =
        (struct sockaddr_in *)calloc(1, sizeof(struct sockaddr_in));
    if (src == NULL) {
      ARTS_ERROR("arts_net: out of memory for the source-address hint");
    }
    if (bind_interface) {
      if (!arts_netif_ipv4_addr(net_interface, src)) {
        ARTS_ERROR("arts_net: net_interface=%s has no usable IPv4 address — "
                   "refusing to fall back to the default route",
                   net_interface);
      }
      src->sin_port = 0; /* any port; only the address constrains the bind */
    } else {
      src->sin_family = AF_INET;
      src->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    hints->addr_format = FI_SOCKADDR_IN;
    hints->src_addr = src;
    hints->src_addrlen = sizeof(struct sockaddr_in);
  } else if (verbs_effective) {
    /* An address vector holds one format, so every rank must open the same
     * one, and the verbs provider offers several under its one name: per
     * device, the IP addresses of whatever interfaces resolve to it, then
     * its native address — in an order that follows each node's own
     * interfaces.  Left unasked, a node without a usable IP interface leads
     * with the native format while its neighbours lead with IPv4.  The
     * native format is the one a fabric node offers whatever its interfaces
     * carry, so it is the choice that is the same on every rank. */
    hints->addr_format = FI_SOCKADDR_IB;
  }

  int rc = fi_getinfo(FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION), NULL, NULL,
                      0, hints, &g_net.info);
  if ((rc != 0 || g_net.info == NULL) && provider_pinned) {
    /* A layering provider can deliver remote-write completions without
     * advertising FI_RMA_EVENT: rxm forwards the core provider's
     * FI_REMOTE_WRITE completion — immediate included — yet never names the
     * bit in its caps, so a capability filter refuses a stack that in fact
     * provides the event.  Ask again without the bit, on the pinned path
     * only: a provider that does advertise it (tcp) keeps being asked, since
     * there the request is what arms remote events. */
    hints->caps &= ~(uint64_t)FI_RMA_EVENT;
    rc = fi_getinfo(FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION), NULL, NULL,
                    0, hints, &g_net.info);
    if (rc == 0 && g_net.info != NULL) {
      ARTS_INFO("arts_net: provider %s matched without FI_RMA_EVENT; relying "
                "on its layered remote-write completion delivery",
                provider);
    }
  }
  if ((rc != 0 || g_net.info == NULL) && tcp_first) {
    /* tcp-first probe found nothing usable on this host — retry without a
     * provider constraint and take fi_getinfo's own pick. */
    free(hints->fabric_attr->prov_name);
    hints->fabric_attr->prov_name = NULL;
    rc = fi_getinfo(FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION), NULL, NULL,
                    0, hints, &g_net.info);
  }
  fi_freeinfo(hints);
  if (rc != 0 || g_net.info == NULL) {
    ARTS_ERROR("arts_net: fi_getinfo found no FI_EP_RDM provider%s%s: %s",
               provider_pinned ? " matching requested provider=" : "",
               provider_pinned ? provider : "", fi_strerror(-rc));
  }

  if (tcp_first) {
    /* Auto-selection can never reach a provider that needs pre-getinfo
     * adaptations (the cxi_effective gates above), so on a host that HAS
     * such a fabric an unpinned run would silently measure tcp over it.
     * One extra probe makes that loud. */
    struct fi_info *cxi_hints = fi_allocinfo();
    if (cxi_hints != NULL) {
      cxi_hints->ep_attr->type = FI_EP_RDM;
      cxi_hints->fabric_attr->prov_name = strdup("cxi");
      cxi_hints->domain_attr->mr_mode =
          FI_MR_LOCAL | FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_VIRT_ADDR |
          FI_MR_ENDPOINT;
      struct fi_info *cxi_avail = NULL;
      if (fi_getinfo(FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION), NULL,
                     NULL, 0, cxi_hints, &cxi_avail) == 0 &&
          cxi_avail != NULL) {
        ARTS_WARN("arts_net: a cxi fabric is present but tcp was "
                  "auto-selected — set provider=cxi to use it");
        fi_freeinfo(cxi_avail);
      }
      fi_freeinfo(cxi_hints);
    }
  }

  /* Instance selection.  fi_getinfo returns instances in the provider's own
   * order, and a provider may expose both provider-assigned-key and
   * client-key instances that each satisfy the offered mr_mode superset.
   * The one-sided immediate path needs provider-assigned keys where the
   * provider ties its immediate support to them (cxi does), so on that path
   * take the first FI_MR_PROV_KEY instance rather than trusting the head's
   * ordering. */
  struct fi_info *use = g_net.info;
  if (cxi_effective) {
    while (use != NULL &&
           !(use->domain_attr->mr_mode & FI_MR_PROV_KEY)) {
      use = use->next;
    }
    if (use == NULL) {
      ARTS_ERROR("arts_net: provider %s matched only client-key MR "
                 "instances — one-sided immediates need FI_MR_PROV_KEY "
                 "(check FI_CXI_ODP / provider defaults)",
                 effective_provider);
    }
  }
  {
    const char *pn = use->fabric_attr->prov_name;
    /* Exact core name, tolerating a hook suffix ("cxi;ofi_hook_perf"). */
    g_net.is_cxi = pn != NULL && strncmp(pn, "cxi", 3) == 0 &&
                   (pn[3] == '\0' || pn[3] == ';');
  }
  /* ofi_rxm in passthru mode answers the multi-recv threshold for a receive
   * queue its messages do not land in, so the threshold check below could
   * not hold for it. */
  if (use->ep_attr->protocol == FI_PROTO_RXM_TCP) {
    ARTS_ERROR("arts_net: provider %s runs ofi_rxm in passthru mode "
               "(FI_OFI_RXM_ENABLE_PASSTHRU), whose multi-recv threshold "
               "is not the one its messages land under",
               use->fabric_attr->prov_name);
  }

  g_net.mr_mode = (uint32_t)use->domain_attr->mr_mode;
  g_net.mr_local = (g_net.mr_mode & FI_MR_LOCAL) != 0;
  g_net.inject_size = use->tx_attr->inject_size;
  g_net.max_msg = use->ep_attr->max_msg_size;
#ifdef ARTS_DEBUG_INJECTION
  {
    /* A one-sided transfer is never posted above the provider's ceiling
     * (arts_net_put_payload splits at it), so lowering the ceiling from the
     * environment is what lets a test drive the split path on a provider
     * whose own ceiling no test payload reaches.  It can only lower it. */
    const char *cap = getenv("ARTS_NET_MAX_MSG");
    if (cap != NULL && *cap != '\0') {
      char *end = NULL;
      errno = 0;
      unsigned long long v =
          (*cap >= '0' && *cap <= '9') ? strtoull(cap, &end, 10) : 0;
      if (end == NULL || end == cap || *end != '\0' || v == 0 ||
          errno == ERANGE) {
        ARTS_ERROR("arts_net: ARTS_NET_MAX_MSG=\"%s\" is not a positive byte "
                   "count",
                   cap);
      }
      if ((size_t)v < g_net.max_msg) {
        g_net.max_msg = (size_t)v;
      }
    }
  }
#endif
  g_net.tx_order = use->tx_attr->msg_order;
  g_net.max_order_waw = use->ep_attr->max_order_waw_size;
  g_net.waw_ordered =
      (g_net.tx_order & (FI_ORDER_WAW | FI_ORDER_RMA_WAW)) != 0 &&
      g_net.max_order_waw >= g_net.max_msg;

  /* The rendezvous txid is receiver-allocated and 32-bit by design, sized to
   * the narrowest immediate a real fabric grants (InfiniBand write-with-imm
   * carries 4 bytes; tcp grants 8).  A provider below even that would
   * silently truncate the immediate and pair the wrong transfers. */
  if (use->domain_attr->cq_data_size < sizeof(uint32_t)) {
    ARTS_ERROR("arts_net: provider cq_data_size %zu < 4 — rendezvous txids "
               "need at least a 32-bit immediate",
               use->domain_attr->cq_data_size);
  }

  rc = fi_fabric(use->fabric_attr, &g_net.fabric, NULL);
  if (rc != 0) {
    ARTS_ERROR("arts_net: fi_fabric failed: %s", fi_strerror(-rc));
  }
  rc = fi_domain(g_net.fabric, use, &g_net.domain, NULL);
  if (rc != 0) {
    ARTS_ERROR("arts_net: fi_domain failed: %s", fi_strerror(-rc));
  }

  struct fi_av_attr av_attr;
  memset(&av_attr, 0, sizeof(av_attr));
  av_attr.type = FI_AV_TABLE; /* fi_addr_t == insertion index == rank */
  rc = fi_av_open(g_net.domain, &av_attr, &g_net.av, NULL);
  if (rc != 0) {
    ARTS_ERROR("arts_net: fi_av_open failed: %s", fi_strerror(-rc));
  }

  struct fi_cq_attr cq_attr;
  memset(&cq_attr, 0, sizeof(cq_attr));
  cq_attr.format = FI_CQ_FORMAT_DATA; /* need buf/len/flags for RX + data slot */
  cq_attr.wait_obj = FI_WAIT_NONE;
  rc = fi_cq_open(g_net.domain, &cq_attr, &g_net.cq, NULL);
  if (rc != 0) {
    ARTS_ERROR("arts_net: fi_cq_open failed: %s", fi_strerror(-rc));
  }

  rc = fi_endpoint(g_net.domain, use, &g_net.ep, NULL);
  if (rc != 0) {
    ARTS_ERROR("arts_net: fi_endpoint failed: %s", fi_strerror(-rc));
  }
  rc = fi_ep_bind(g_net.ep, &g_net.av->fid, 0);
  if (rc != 0) {
    ARTS_ERROR("arts_net: fi_ep_bind(av) failed: %s", fi_strerror(-rc));
  }
  rc = fi_ep_bind(g_net.ep, &g_net.cq->fid, FI_TRANSMIT | FI_RECV);
  if (rc != 0) {
    ARTS_ERROR("arts_net: fi_ep_bind(cq) failed: %s", fi_strerror(-rc));
  }
  /* The multi-recv threshold is what makes ARTS_NET_MSG_MAX a guarantee: a
   * landing buffer is retired once less than this remains, so a message no
   * larger than it always finds room.  It must be set BEFORE the endpoint is
   * enabled: a layered provider builds its receive context at enable time
   * from the value it holds then, and a later setopt reaches only the
   * endpoint's own copy while the receive side keeps its default, a fraction
   * of the ceiling, and truncates any larger message. */
  size_t min_mr = ARTS_NET_MIN_MULTI_RECV;
  rc = fi_setopt(&g_net.ep->fid, FI_OPT_ENDPOINT, FI_OPT_MIN_MULTI_RECV,
                 &min_mr, sizeof(min_mr));
  if (rc != 0) {
    ARTS_ERROR("arts_net: fi_setopt(MIN_MULTI_RECV) failed: %s",
               fi_strerror(-rc));
  }
  rc = fi_enable(g_net.ep);
  if (rc != 0) {
    ARTS_ERROR("arts_net: fi_enable failed: %s", fi_strerror(-rc));
  }
  /* Read the threshold back from the enabled endpoint, which answers for its
   * receive side.  The control-message ceiling holds only if the threshold
   * that side applies is at least the ceiling, so a provider whose receive
   * side applies less, or that cannot report what it applies, cannot carry
   * this transport. */
  const char *prov_name = use->fabric_attr->prov_name;
  size_t got_mr = 0;
  size_t got_len = sizeof(got_mr);
  rc = fi_getopt(&g_net.ep->fid, FI_OPT_ENDPOINT, FI_OPT_MIN_MULTI_RECV,
                 &got_mr, &got_len);
  if (rc != 0) {
    ARTS_ERROR("arts_net: provider %s cannot report its multi-recv "
               "threshold (%s); the %zu-byte control-message ceiling cannot "
               "be guaranteed",
               prov_name, fi_strerror(-rc), (size_t)ARTS_NET_MSG_MAX);
  }
  if (got_mr < ARTS_NET_MSG_MAX) {
    ARTS_ERROR("arts_net: provider %s applies a %zu-byte multi-recv "
               "threshold, below the %zu-byte control-message ceiling "
               "(%zu was requested)",
               prov_name, got_mr, (size_t)ARTS_NET_MSG_MAX,
               (size_t)ARTS_NET_MIN_MULTI_RECV);
  }

  ARTS_INFO("arts_net: fabric up provider=%s domain=%s addr_format=%s "
            "inject_size=%zu mr_mode=0x%x "
            "mr_local=%d multi_recv=%uMiB max_msg=%zu msg_order=0x%llx sas=%d "
            "waw=%d(max %zu) mr_cnt=%zu",
            use->fabric_attr->prov_name, use->domain_attr->name,
            fi_tostr(&use->addr_format, FI_TYPE_ADDR_FORMAT),
            g_net.inject_size, g_net.mr_mode, g_net.mr_local,
            (unsigned)(ARTS_NET_RECV_BUF_SIZE / (1024 * 1024)), g_net.max_msg,
            (unsigned long long)g_net.tx_order,
            (g_net.tx_order & FI_ORDER_SAS) == FI_ORDER_SAS,
            (int)g_net.waw_ordered, g_net.max_order_waw,
            use->domain_attr->mr_cnt);
}

struct fid_ep *arts_net_mr_endpoint(void) {
  /* Endpoint-bound registration is a negotiated property, not a request:
   * non-NULL exactly when the matched instance demands FI_MR_ENDPOINT. */
  return (g_net.mr_mode & FI_MR_ENDPOINT) != 0 ? g_net.ep : NULL;
}

void arts_net_rx_arm(void) {
  /* Landing buffers come from the registered pool (initialized just before this
   * call), so recv memory is pinned/registered exactly like send bounces — one
   * MR-key namespace, and no duplicate requested_key on a provider that needs
   * FI_MR_LOCAL without FI_MR_PROV_KEY (a class of bug the pool already avoids
   * with a monotone key counter).  When the provider requires a local
   * descriptor, it is the enclosing slab's MR handle; otherwise it is NULL. */
  for (unsigned i = 0; i < ARTS_NET_RECV_BUF_COUNT; i++) {
    g_net.rxbuf[i] = arts_regpool_alloc_aligned(ARTS_NET_RECV_BUF_SIZE, 4096);
    if (g_net.rxbuf[i] == NULL) {
      ARTS_ERROR("arts_net: recv buffer %u allocation failed", i);
    }
    g_net.rxctx[i].idx = i;
    g_net.rxdesc[i] = net_desc(g_net.rxbuf[i]);
  }
  for (unsigned i = 0; i < ARTS_NET_RECV_BUF_COUNT; i++) {
    net_post_recv(i);
  }
}

void arts_net_quiesce(void) {
  /* Phase 1 of teardown, run BEFORE the registered pool is torn down, once
   * every runtime thread has passed the ready_to_clean barrier: none touches
   * the fabric again, joined or not, and the barrier's arrivals order each
   * one's last access before this.  Discarding sends that were
   * never accepted by the provider at final teardown is deliberate — it mirrors
   * a bounded drain-then-exit and must never block shutdown on a peer that has
   * already gone.  The ordering matters: everything that references a slab MR
   * (send bounces still queued, and the recv buffers themselves) is released
   * here, while the pool is still live, so the pool cleanup that follows can
   * close those MRs without any outstanding reference — and no desc ever names
   * a closed MR. */
  atomic_store_explicit(&g_net.quiescing, true, memory_order_relaxed);

  /* Discard queued-but-unposted retry txns.  These never reached the provider,
   * so they are not outstanding; run their completion-gated frees (caller
   * payload + bounce back to the still-live pool) and drop them.  The rings
   * themselves outlive the reap below, whose bound sums their post counts,
   * and nothing reads their slots again. */
  uint32_t nr = atomic_load_explicit(&g_ring_count, memory_order_acquire);
  for (uint32_t k = 0; k < nr; k++) {
    struct net_ring_s *r = g_rings[k];
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    for (; h != t; h++) {
      net_txn_complete(r->slots[h % ARTS_NET_RING_CAP]);
    }
  }

  /* Free any inbound messages reaped but not yet dispatched (a producer's
   * net_reap_no_dispatch can leave entries on the pending list).  Quiescent
   * teardown: drop them without dispatch — handlers must not run against
   * torn-down state at shutdown. */
  while (g_pending_head != NULL) {
    struct net_pending_s *next = g_pending_head->next;
    arts_free(g_pending_head);
    g_pending_head = next;
  }
  g_pending_tail = NULL;

  /* Drop unpaired rendezvous records (an expectation whose data never landed,
   * or data whose metadata never arrived — both only possible when a peer died
   * or shutdown cut a transfer mid-flight) with the levels that hold them.
   * Quiescent: no callback runs. */
  for (struct net_rdzv_level_s *lv = g_rdzv_level0; lv != NULL;) {
    struct net_rdzv_level_s *next =
        atomic_load_explicit(&lv->next, memory_order_relaxed);
    arts_free(lv);
    lv = next;
  }
  g_rdzv_level0 = NULL;

  /* Reap already-posted sends so their completion-gated frees run (bounces back
   * to the still-live pool).  TX-only: inbound completions are ignored and no
   * landing buffer is re-armed — they are about to be cancelled.  Bounded — the
   * bound guards against a completion that never arrives on a wedged provider.
   * Every worker/progress thread has passed the ready_to_clean barrier, so
   * net_reap_tx_only needs no CQ token — no other thread contends the CQ. */
  net_quiesce_held_writes();
  net_quiesce_held_requests();
  for (int i = 0; i < 100000 && net_tx_outstanding() != 0; i++) {
    net_reap_tx_only();
  }
  for (uint32_t k = 0; k < nr; k++) {
    arts_free(g_rings[k]);
    g_rings[k] = NULL;
  }
  atomic_store_explicit(&g_ring_count, 0, memory_order_release);
  atomic_store_explicit(&g_net.tx_completed, 0, memory_order_relaxed);
  t_ring = NULL;

  /* Endpoint-bound registrations hold references the endpoint cannot close
   * under (fi_close would return -FI_EBUSY and the endpoint would leak, then
   * the domain behind it).  The pool's MRs serve remote access only — no
   * local descriptor references them on such a provider (no FI_MR_LOCAL) —
   * so closing them first, once no runtime thread touches the fabric, only
   * makes a peer's still-in-flight PUT fail at the NIC, the same exposure the
   * endpoint close below already creates.  Providers WITH FI_MR_LOCAL keep
   * today's order: their posted recvs hold MR descriptors until the endpoint
   * close cancels them, so their MRs must outlive the endpoint. */
  if ((g_net.mr_mode & FI_MR_ENDPOINT) != 0) {
    arts_regpool_unregister();
  }

  /* Close the endpoint (cancels the posted recvs), then the CQ and AV.  The
   * returns are checked: a -FI_EBUSY here means a close-ordering invariant
   * broke and the object (and everything behind it) leaks. */
  if (g_net.ep != NULL) {
    int crc = fi_close(&g_net.ep->fid);
    if (crc != 0) {
      ARTS_WARN("arts_net: fi_close(ep) failed: %s", fi_strerror(-crc));
    }
    g_net.ep = NULL;
  }
  if (g_net.cq != NULL) {
    int crc = fi_close(&g_net.cq->fid);
    if (crc != 0) {
      ARTS_WARN("arts_net: fi_close(cq) failed: %s", fi_strerror(-crc));
    }
    g_net.cq = NULL;
  }
  if (g_net.av != NULL) {
    int crc = fi_close(&g_net.av->fid);
    if (crc != 0) {
      ARTS_WARN("arts_net: fi_close(av) failed: %s", fi_strerror(-crc));
    }
    g_net.av = NULL;
  }

  /* Return the recv landing buffers to the pool while it is still live; their
   * slab MR is closed by the pool cleanup that follows. */
  for (unsigned i = 0; i < ARTS_NET_RECV_BUF_COUNT; i++) {
    if (g_net.rxbuf[i] != NULL) {
      arts_regpool_free(g_net.rxbuf[i]);
      g_net.rxbuf[i] = NULL;
    }
    g_net.rxdesc[i] = NULL;
  }
}

void arts_net_teardown(void) {
  /* Phase 2 of teardown, run AFTER arts_regpool_cleanup: the pool has closed
   * every slab MR (which backed both sends and the recv buffers), so nothing
   * registered outlives its domain — close the domain, then the fabric. */
  if (g_net.domain != NULL) {
    int crc = fi_close(&g_net.domain->fid);
    if (crc != 0) {
      ARTS_WARN("arts_net: fi_close(domain) failed: %s", fi_strerror(-crc));
    }
    g_net.domain = NULL;
  }
  if (g_net.fabric != NULL) {
    int crc = fi_close(&g_net.fabric->fid);
    if (crc != 0) {
      ARTS_WARN("arts_net: fi_close(fabric) failed: %s", fi_strerror(-crc));
    }
    g_net.fabric = NULL;
  }
  if (g_net.info != NULL) {
    fi_freeinfo(g_net.info);
    g_net.info = NULL;
  }
  if (g_net.peers != NULL) {
    arts_free(g_net.peers);
    g_net.peers = NULL;
  }
  g_net.peer_count = 0;
}

void arts_net_drain_outstanding(unsigned int deadline_ms) {
  /* Bounded wait for accepted fabric sends to complete, so the shutdown
   * broadcast's frames leave this node before teardown.  The progress threads
   * are still running: they reap completions and dispatch what arrives.  This
   * assists with the transmit side only -- it posts queued sends, reaps the
   * completion queue without dispatching and polls the outstanding count --
   * since a handler run here could ready an EDT on a thread that owns no
   * deque.  Single-node never brought the fabric up. */
  if (arts_global_rank_count <= 1) {
    return;
  }
  uint64_t deadline_ns =
      (uint64_t)deadline_ms * 1000000ULL; /* ms -> ns budget */
  struct timespec start;
  (void)clock_gettime(CLOCK_MONOTONIC, &start);
  for (;;) {
    if (net_tx_outstanding() == 0) {
      ARTS_INFO("arts_net: shutdown TX drain complete");
      return;
    }
    net_drain_all_rings();
    net_reap_no_dispatch();
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t elapsed = (uint64_t)(now.tv_sec - start.tv_sec) * 1000000000ULL +
                       (uint64_t)(now.tv_nsec - start.tv_nsec);
    if (elapsed >= deadline_ns) {
      ARTS_WARN("arts_net: shutdown TX drain timeout (%llu sends still "
                "outstanding)",
                (unsigned long long)net_tx_outstanding());
      return;
    }
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 200000L /* 0.2 ms */};
    nanosleep(&ts, NULL);
  }
}
