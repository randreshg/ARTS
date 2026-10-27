/* SPDX-License-Identifier: Apache-2.0
 *
 * EXCL protocol, RETAIN release policy.
 *
 * The home holds no bytes — only a directory (current owner, phase, waiter
 * queues).  The payload stays with the last read--write holder and moves
 * owner->owner on demand; a release with nothing pending moves nothing at all.
 * Remote requests all go to the home, while an acquire on the rank that already
 * owns the data is served from its own buffer with no home round-trip.
 *
 * What makes that tractable is the protocol's own phase exclusion.  Read--write
 * and read-only phases never overlap, so an ownership migration (read--write
 * phase only) and a reader serve (read-only phase only) can never be in flight
 * together: the migration-versus-in-transit-reader race that a migrating
 * protocol with a concurrent reader plane must solve cannot arise here.  The
 * price is that a remote read waits out the write phase at the home instead of
 * being served from a second copy — which is what phase exclusion means.
 *
 * Compiled only for ARTS_COHERENCE_PROTOCOL=EXCL + ARTS_RELEASE_POLICY=RETAIN;
 * the release-policy variant is a link-time file choice, so this TU contains no
 * release-policy preprocessor guards.
 */

/* excl/types.h must precede all other coherence headers: it defines
 * arts_db_cache_s, arts_db_s, the CACHE_ and LOCK_ macros, the RETAIN arbiter
 * prototypes, and arts_db_excl_waiter_s for the EXCL build. */
#include "arts/coherence/excl/types.h"

#include "arts/utils/stdatomic_compat.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arts/coherence/buffer.h"
#include "arts/coherence/census.h"
#include "arts/coherence/coherence.h"
#include "arts/coherence/handlers.h"
#include "arts/coherence/directory.h"
#include "arts/db.h"
#include "arts/edt.h"
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
/* The pure state-transition arbiters live in arbiters.c, which the unit test
 * (tests/unit/excl_compute_next.c) #includes without the handler bodies. */
#include "arbiters.c"
#include "arts/counter/Preamble.h"

/* ===== RO-serve node (reader rank the owner must serve in the RO phase) =
 * The owner's ro_serve is an arts_lf_stack_t (Treiber); each node carries one
 * reader rank that the home asked the owner to serve via FORWARD(serve).  File-
 * local: only the owner-side FORWARD handler (push) and owner_try_execute /
 * the destructor (drain) touch it. */
struct arts_lock_ro_serve_node_s {
  arts_lf_link_t link; /* FIRST — required by arts_lf_stack_t */
  unsigned int rank;
  /* Relinquish policy the home stamped on this grant.  Per-node, not per-drain:
   * an untagged FORWARD blocked behind a live writer can sit here while a
   * tagged one arrives, so one drain must be able to carry both. */
  uint32_t tag;
  struct arts_rdzv_landing_s rdzv; /* reader's serve landing (its stable buffer) */
};

/* ===== arts_db_acquire_is_serialized ===================================
 * EXCL: both RW and RO are blocking locks — both are GUID-serialized so the
 * engine acquires them in a global order (deadlock-free lock ordering). */
bool arts_db_acquire_is_serialized(arts_db_access_mode_t mode) {
  return mode == DB_MODE_RW || mode == DB_MODE_RO;
}

/* ===== arts_db_cache_init ==============================================
 * Initialize the per-rank cache for the EXCL RETAIN protocol.
 *
 * Creator hold: arts_db_create defaults to an RW acquire (OCR contract; only
 * ARTS_DB_PROP_NO_ACQUIRE skips it).  The creator is the first data owner, so
 * we seed cache_state with owner-bit=1, rw_st=GRANT, wc=1, rc=0,
 * migrate_target=NO_TARGET.  The matching arts_db_release drives wc back to 0
 * (a sticky release — the creator stays the owner).  Every other cache (the
 * home's HOME_RECV, a first-touch STUB) starts with all fields zero/idle (not
 * owner). */
void arts_db_cache_init(struct arts_db_cache_s *c, arts_guid_t db_guid,
                        uint64_t db_size, arts_db_init_kind_t kind,
                        unsigned int creator_rank) {
  uint64_t seed;
  if (kind == ARTS_DB_INIT_CREATOR_HOME ||
      kind == ARTS_DB_INIT_CREATOR_REMOTE) {
    /* Creator holds RW and is the data owner: owner=1, rw_st=GRANT, wc=1,
     * ro_st=IDLE, rc=0, migrate_target=NO_TARGET (no pending migration). */
    seed = CACHE_MAKE_FULL(1u, CACHE_ST_GRANT, CACHE_ST_IDLE,
                           ARTS_EXCL_NO_TARGET, 1u, 0u);
  } else {
    /* Non-creator cache stub: not owner, all state IDLE, counts 0,
     * migrate_target=NO_TARGET. */
    seed = CACHE_MAKE_FULL(0u, CACHE_ST_IDLE, CACHE_ST_IDLE,
                           ARTS_EXCL_NO_TARGET, 0u, 0u);
    if (kind == ARTS_DB_INIT_STUB && ARTS_DB_STUB_CLAIMABLE(db_guid)) {
      seed |= CACHE_OWNER_STUB_BIT;
    }
  }
  atomic_store_explicit(&c->cache_state, seed, memory_order_relaxed);
  arts_lf_stack_init(&c->ro_pending);
  arts_lf_stack_init(&c->rw_pending);
  arts_lf_stack_init(&c->ro_serve);
  c->migrate_rdzv = (struct arts_rdzv_landing_s){0, 0, 0, 0};
  arts_db_cache_common_init(c, db_guid, db_size, kind, creator_rank);
}

/* ===== arts_db_cache_destructor ========================================
 * Runs at refcount 0 as the SOLE owner of the object; no concurrent actor
 * remains, so it is the single safe drainer of the parked-waiter stacks.
 * No waiter is woken: one still parked is an acquire the destroy contract
 * excludes.  Destroy itself is just the route-slot detach (CAS value→NULL +
 * drop the install ref). */
void arts_db_cache_destructor(struct arts_db_cache_s *cache) {
  if (cache == NULL) {
    return;
  }
  arts_db_cache_common_destroy_pre(cache); /* buffer-NULL FIRST */
  /* Drain + free the parked-waiter stacks (ro_pending, rw_pending) and the
   * RO-phase serve list (ro_serve).  All empty in the normal case — a legit
   * destroy has no outstanding acquirers and no pending RO serve. */
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
  {
    arts_lf_link_t *node = arts_lf_stack_drain(&cache->ro_serve);
    while (node != NULL) {
      arts_lf_link_t *nx =
          atomic_load_explicit(&node->next, memory_order_relaxed);
      struct arts_lock_ro_serve_node_s *rn =
          ARTS_CONTAINER_OF(node, struct arts_lock_ro_serve_node_s, link);
      arts_free(rn);
      node = nx;
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

/* ===== arts_db_create_install_home_buffer ===============================
 * The data lives with its owner, the creator at create time; the home holds no
 * canonical buffer.  Nothing to install at create — the creator's own cache
 * buffer is installed by db_create_in_place / the DB_CREATE handler. */
void arts_db_create_install_home_buffer(struct arts_db_cache_s *cache,
                                        uint64_t db_size) {
  (void)cache;
  (void)db_size;
}

/* ===== lock_drain_pending ==============================================
 * Serve exactly `expected` waiters from `q` — the parked population the
 * caller's CAS observed (cache_owner_parked_rw / cache_owner_parked_ro of its
 * pre-image).  That CAS is the one that starts serving the axis, so every
 * later arrival serves itself and never pushes: the counted population is the
 * whole stack, and the caller is its sole drainer.  A counted waiter whose
 * push has not landed yet — the instruction window between its acquire CAS
 * and its push — is awaited by re-draining.  Serving = claim the slot's
 * acquire record with the installed buffer + position-idempotent cursor
 * advance (which is what fires the next serialized dep) + account (may hand
 * the payload to depv and schedule the EDT when acquire_remaining reaches 0).
 * It does NOT touch the count, which the waiter's acquire CAS carried. */
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
    next = cache_owner_compute_next(cur, CACHE_OP_CREATE_HOLD, &act);
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
    lock_drain_pending(&cache->rw_pending, cache_owner_parked_rw(cur));
    lock_drain_pending(&cache->ro_pending, cache_owner_parked_ro(cur));
  }
  return true;
}


/* ===== owner_try_execute ===============================================
 * The serve-RO + RW-migration driver.  Called whenever an event may have made
 * this rank ready to ship: a FORWARD installing a pending action, a DELIVER
 * making this rank the owner, or a release 0-edge.
 *
 * The release path decides its own 0-edge inside one CAS; this routine exists
 * for the case where the FORWARD lands when there is no live holder left to
 * release, so nothing else would re-evaluate the pending action. */
static void owner_try_execute(struct arts_db_cache_s *cache) {
  /* Store->load pair, closed by this one fence because both sides pass it.
   * The FORWARD(serve) arm of arts_handler_db_excl_forward pushes onto
   * ro_serve and then loads cache_state for the serve gate; the callers that
   * move cache_state first (the zero-edge releases arts_db_release_rw and
   * arts_db_release_ro, lock_deliver_commit, the forward's other arms) CAS
   * it and then probe ro_serve.  Without a fence each side's load may be
   * satisfied before its own store is visible to the other, and both miss:
   * the reader strands and the read-only phase never completes.  It covers
   * only the transitions made before the call, not this routine's own CASes
   * below, which close the gate. */
  atomic_thread_fence(memory_order_seq_cst);
  /* (1) A pending migrate_target ships the buffer, in one of two shapes.
   *
   * target != self is a real migration: the bytes leave, so it waits for no live
   * local holder of either kind, clears owner-bit + migrate_target, and DELIVERs
   * to the target, which CONFIRMs the home.
   *
   * target == self means the home granted the write phase to the rank that
   * already holds the data — a resident rank that had relinquished its write
   * permission (serving a reader, §the demote below) and therefore had to ask
   * the home for it back like any other requester.  This arm is an ORDINARY
   * path, not a rare escape: it is also how the first writer of a
   * DB_PROP_NO_ACQUIRE block gets its permission, since those seed as
   * resident-with-no-permission.  Nothing may move; once the readers drain, the
   * parked writers are granted locally and the home is CONFIRMed so it advances
   * the phase.  Here wc is the writer cohort BEING granted, not a blocker, so
   * this shape must NOT wait for wc==0 — that writer's own count can never reach
   * zero on its own, and waiting would deadlock. */
  for (;;) {
    uint64_t cur =
        atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    uint32_t mt = CACHE_MIGRATE_TARGET(cur);
    /* The standalone-RO clause is the reader guard for the self-migration shape,
     * which has no wc/rc gate of its own: a self-grant must not overwrite a
     * live, separately held RO grant.  A pending RO request, or an orphan grant
     * left by a reader that self-served through a write grant and released, is
     * not a live reader and must not block.  The real-ship arm needs none of
     * this — its wc==0 && rc==0 gate is strictly stronger, and additionally
     * catches a reader that joined under a write grant, which is counted in rc
     * while ro_st stays IDLE. */
    if (mt == ARTS_EXCL_NO_TARGET || CACHE_OWNER(cur) == 0u ||
        (CACHE_RO_ST(cur) == CACHE_ST_GRANT && CACHE_RO_CNT(cur) != 0u)) {
      break; /* no eligible migration (or a live standalone RO grant) */
    }
    if (mt == arts_global_rank_id) {
      /* Self-migration → local RW grant.  Keep owner-bit + counts; set
       * rw_st=GRANT, clear migrate_target in one CAS, then drain rw_pending and
       * CONFIRM home.  (No data ship.)  Does NOT require wc==0. */
      uint64_t next = CACHE_MAKE_FULL(1u, CACHE_ST_GRANT, CACHE_RO_ST(cur),
                                      ARTS_EXCL_NO_TARGET, CACHE_RW_CNT(cur),
                                      CACHE_RO_CNT(cur)) |
                      (cur & (CACHE_OWNER_LEASED_MASK |
                              CACHE_OWNER_RO_PAR_BIT));
      if (atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur, next,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        /* The CAS committed this rank as the resident holder of the write
         * permission, so it is now the rank that owes its local writers the
         * block's bytes.  A create that took no hold left the block's storage
         * to its first user, and these writers are it — materialize before
         * the drain hands them a pointer.  A no-op for every other block:
         * residency arrived with a DELIVER that installed the copy. */
        (void)arts_db_buf_ensure(
            cache, atomic_load_explicit(&cache->db_size, memory_order_relaxed));
        /* Only rw_pending: a reader never parks while this rank is resident
         * (the read acquire's owner-bit branch serves it immediately), so
         * ro_pending is empty whenever the owner-bit is already set.  A change
         * that removed the read fast path would strand parked readers here.
         * The write right never left this rank. */
        uint8_t census = arts_db_census_serving(false);
        lock_drain_pending(&cache->rw_pending, cache_owner_parked_rw(cur));
        arts_db_census_served(census);
        arts_send_db_excl_confirm(arts_guid_get_rank(cache->db_guid),
                                  cache->db_guid);
        break;
      }
      continue; /* CAS lost — re-snapshot. */
    }
    /* Real migration to another rank: the buffer LEAVES this rank by one-sided
     * PUT.  Its safety rests on global-lock exclusion — NO local holder may be
     * touching the stable buffer when it ships, because a later migration back
     * to this rank installs IN PLACE and would overwrite the buffer under a
     * still-live holder.  So ship only at the SAME 0-edge REL_RW commits on:
     * wc==0 && rc==0 — no live writer AND no live reader of ANY grant form.
     * Gating on the RW/RO STATE alone is unsound: an RW⊇RO reader is counted in
     * rc while ro_st stays IDLE (it joined under rw_st==GRANT), so a state-only
     * gate would ship out from under it.  No deadlock: whichever flavor
     * releases LAST re-runs owner_try_execute (REL_RW on its wc 0-edge, REL_RO
     * on its rc 0-edge), so a migration parked here ships the instant the final
     * holder releases.
     *
     * The writer half of the gate is a LIVE GRANTED writer, not wc>0: wc also
     * counts a merely PENDING write request, and this rank can hold one
     * (own==1 && rw_st==REQ: a resident rank without the write permission
     * asks the home for it).  A parked writer's EDT has never run, so nothing
     * would ever decrement wc and a wc>0 gate would deadlock here
     * permanently.  The rc half stays unconditional: an owner-local reader is
     * counted ONLY in rc and is invisible to the home, so it is this gate
     * alone that stops ownership leaving under it.
     *
     * The next word CLEARS the write permission (rw_st GRANT -> IDLE: it leaves
     * with the bytes; two ranks must never both hold it) but PRESERVES a parked
     * REQ and, critically, PRESERVES wc.  Zeroing wc would lose the parked
     * writer's count: it would later go live with wc==0, so the next ship gate
     * would pass mid-write and arts_db_release_rw's wc==0 guard would swallow
     * its zero edge. */
    if ((CACHE_RW_ST(cur) == CACHE_ST_GRANT && CACHE_RW_CNT(cur) != 0u) ||
        CACHE_RO_CNT(cur) != 0u) {
      break;
    }
    uint64_t next =
        CACHE_MAKE_FULL(0u,
                        (CACHE_RW_ST(cur) == CACHE_ST_GRANT) ? CACHE_ST_IDLE
                                                             : CACHE_RW_ST(cur),
                        CACHE_RO_ST(cur), ARTS_EXCL_NO_TARGET,
                        CACHE_RW_CNT(cur), CACHE_RO_CNT(cur)) |
        (cur & (CACHE_OWNER_LEASED_MASK |
                CACHE_OWNER_STICKY_MASK)); /* preserve ro_granted */
    if (atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur, next,
                                              memory_order_acq_rel,
                                              memory_order_acquire)) {
      /* owner-bit + migrate_target cleared (published) BEFORE the DELIVER
       * ships — a concurrent acquire now sees not-owner and routes via home.
       * The write permission is cleared in the SAME CAS, which is what keeps
       * "rw_st==GRANT implies the owner-bit" true across every transition:
       * leave it behind and the departed rank would still grant local writes
       * on bytes that now live elsewhere. */
      /* The size is descriptor state and travels whether or not bytes do.
       * A block nobody has written has no buffer here, so the migration is
       * data-less and the new owner's own prepared buffer becomes the first
       * image — but that owner must still learn the size, or every reader it
       * later serves would be handed an empty copy. */
      arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
      uint64_t ds = atomic_load_explicit(&cache->db_size, memory_order_relaxed);
      /* Consume the pending migrate target's landing (single in-flight
       * migration; the FORWARD handler wrote it before publishing the
       * target).  buf_h transfers into the deliver sender (PUT source pin). */
      struct arts_rdzv_landing_s mt_rdzv = cache->migrate_rdzv;
      cache->migrate_rdzv = (struct arts_rdzv_landing_s){0, 0, 0, 0};
      arts_send_db_excl_deliver(mt, cache->db_guid, (uint32_t)DB_MODE_RW,
                                /*tag=*/0u, &mt_rdzv, buf_h, ds);
      break;
    }
    /* CAS lost — re-snapshot. */
  }

  /* (2) Ship an RO copy to each reader the home asked this rank to serve.
   *
   * The gate is a live GRANTED writer, not wc>0: wc also counts a merely
   * PENDING write request, which must not hold readers back.  Readers ARE
   * tolerated (they only read) — that is what separates this from the ship gate
   * above.  A reader pushed after this drain is picked up by the pusher's own
   * recheck or by the next release.
   *
   * Serving is also where this rank RELINQUISHES its write permission: handing
   * out a readable copy and keeping the right to write it are incompatible.
   * The demote is committed BEFORE the drain, and the gate and the emptiness
   * probe are re-evaluated INSIDE the CAS loop:
   *   - before, not after: a local ACQ_RW joining on rw_st==GRANT between the
   *     drain and the PUT would write into the bytes being shipped.
   *   - inside the loop: a writer that went live since the snapshot must close
   *     the gate on the retry rather than be demoted out from under.
   *   - only GRANT -> IDLE: clearing a parked REQ would make the next local
   *     ACQ_RW open a SECOND request, double-counting the home's w.
   *   - ro_granted preserved: CACHE_MAKE_FULL zeroes it, and losing that token
   *     strands the home's r.
   * The emptiness probe is a relaxed load and MUST stay below this function's
   * seq_cst fence — hoisting it recreates the store-load pair that fence
   * closes, and the reader would strand with the writer behind it. */
  uint64_t cur;
  for (;;) {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    if (CACHE_OWNER(cur) != 1u ||
        (CACHE_RW_ST(cur) == CACHE_ST_GRANT && CACHE_RW_CNT(cur) != 0u)) {
      return; /* not resident, or a live writer holds the buffer */
    }
    if (arts_lf_stack_empty(&cache->ro_serve)) {
      return; /* nothing to serve — do NOT relinquish */
    }
    if (CACHE_RW_ST(cur) != CACHE_ST_GRANT) {
      break; /* permission already relinquished */
    }
    uint64_t next = CACHE_MAKE_FULL(CACHE_OWNER(cur), CACHE_ST_IDLE,
                                    CACHE_RO_ST(cur), CACHE_MIGRATE_TARGET(cur),
                                    CACHE_RW_CNT(cur), CACHE_RO_CNT(cur)) |
                    (cur & (CACHE_OWNER_LEASED_MASK | CACHE_OWNER_STICKY_MASK));
    if (atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur, next,
                                              memory_order_acq_rel,
                                              memory_order_acquire)) {
      break;
    }
  }
  {
    /* Residency was observed above, not pinned: the exit that finds the
     * permission already relinquished takes no CAS.  What holds it through
     * the serve is the home's phase arbitration, not this word — a reader's
     * entry is queued here by a FORWARD the home issues inside the read phase
     * it opened on that reader's outstanding count, and the home opens no
     * write phase (so no migration that could clear residency) while that
     * count stands.  Within that guarantee this is the rank that owes these
     * readers the block's bytes.  A create that took no hold left the block's
     * storage to its first user, and a reader served from here is it: a
     * data-less DELIVER would install nothing at the reader and hand its EDT
     * a NULL pointer for a sized block.  Install-if-absent, so even a stale
     * observation could at most fill a hole, never displace bytes. */
    (void)arts_db_buf_ensure(
        cache, atomic_load_explicit(&cache->db_size, memory_order_relaxed));
    arts_lf_link_t *node = arts_lf_stack_drain(&cache->ro_serve);
    if (node != NULL) {
      arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
      struct arts_db_buffer_s *buf =
          (struct arts_db_buffer_s *)arts_shared_get(buf_h);
      uint64_t ds = atomic_load_explicit(&cache->db_size, memory_order_relaxed);
      while (node != NULL) {
        arts_lf_link_t *nx =
            atomic_load_explicit(&node->next, memory_order_relaxed);
        struct arts_lock_ro_serve_node_s *rn =
            ARTS_CONTAINER_OF(node, struct arts_lock_ro_serve_node_s, link);
        /* Each fan-out PUT pins the source with its own strong ref. */
        arts_send_db_excl_deliver(
            rn->rank, cache->db_guid, (uint32_t)DB_MODE_RO, rn->tag, &rn->rdzv,
            (buf != NULL) ? arts_shared_copy(buf_h) : NULL, ds);
        arts_free(rn);
        node = nx;
      }
      arts_db_buf_release(&buf_h);
    }
  }
}

/* ===== arts_send_db_excl_request ========================================
 * Send MSG_DB_EXCL_REQUEST to the home rank.  Self-send (home == this rank)
 * dispatches through the OoO engine (HIT runs inline; MISS defers until the
 * home db_s is installed) — a REQUEST can race the home CREATE on a cross-rank
 * stub-install path.  Remote send goes via the transport. */
/* This rank's deliver landing is its stable buffer (the fixed-address
 * backing store), installed IN PLACE by the DELIVER for either mode.  Phase
 * exclusion keeps the buffer quiescent for the whole flight of a request:
 * this rank's readers park on ro_st==REQ and its writers park at the home
 * behind the outstanding read count, the home opens no write phase until
 * every read grant has come back, and a grant comes back only after its
 * bytes have landed — so neither a migration nor a local access can reach
 * the buffer while a serve is in flight, and the serve leaves an owner only
 * while that owner's write permission is relinquished.  Every advertisement
 * draws a fresh txid and is served by at most one DELIVER. */

/* Bytes a landing must hold; 0 when no payload can move here. */
static uint64_t lock_stable_size(const struct arts_db_cache_s *cache) {
  return (arts_global_rank_count > 1) ? arts_db_first_fetch_size(cache) : 0u;
}

/* Materialize an n-byte stable buffer if this rank has none.  Run by the
 * requester, so the pool places it on the requesting thread's node.  The
 * bytes are unspecified: the PUT fully overwrites them before any drained
 * waiter reads, and a data-less deliver is one for a block nobody has
 * written.  n may be the GUID bound — an ALLOCATION size only; the DB's size
 * is declared exclusively by the wire (prepare never records it). */
static void lock_stable_place(struct arts_db_cache_s *cache, uint64_t n) {
  if (n != 0u) {
    arts_db_buf_prepare_inplace(cache, n);
  }
}

/* Advertise the stable buffer placed with the same n.  False only when no
 * payload can move here (n == 0); a sized block whose buffer cannot be
 * advertised is fatal, since a landing-less request or FORWARD would be
 * answered without its bytes. */
static bool lock_stable_advertise(struct arts_db_cache_s *cache, uint64_t n,
                                  struct arts_rdzv_landing_s *out) {
  *out = (struct arts_rdzv_landing_s){0, 0, 0, 0};
  if (n == 0u) {
    return false;
  }
  arts_shared_ptr_t h = arts_db_buf_acquire(cache);
  struct arts_db_buffer_s *buf = (struct arts_db_buffer_s *)arts_shared_get(h);
  if (buf == NULL ||
      !arts_net_rdzv_local(buf->data, n, &out->addr, &out->key)) {
    ARTS_ERROR("lock: stable buffer is unplaced or not fabric-registered — "
               "one-sided deliver requires the registered pool");
  }
  out->txid = arts_net_rdzv_txid_next();
  out->cookie = 0; /* in-place: this rank finds the buffer via its cache */
  arts_db_buf_release(&h);
  return true;
}

void arts_send_db_excl_request(struct arts_db_cache_s *cache,
                               arts_db_access_mode_t mode, uint32_t cover) {
  arts_guid_t db_guid = cache->db_guid;
  unsigned int home_rank = arts_guid_get_rank(db_guid);
  /* One reading for both steps: a concurrent size answer may record the
   * block's size between them, and the advertisement must name the buffer
   * this reading placed. */
  uint64_t n = lock_stable_size(cache);
  lock_stable_place(cache, n);
  if (home_rank == arts_global_rank_id) {
    /* A home requester advertises no landing: when the owner is another rank
     * the home draws one for the buffer placed above as it forwards (see
     * forward_target_landing), and when the owner is this rank no payload
     * moves at all. */
    struct arts_ooo_args_db_excl_request_s args = {
        .requester = arts_global_rank_id,
        .db_guid = db_guid,
        .mode = mode,
        .cover = cover,
        .rdzv = {0, 0, 0, 0},
    };
    arts_ooo_dispatch_or_defer_guid(db_guid, OOO_DB_EXCL_REQUEST, &args,
                                    sizeof(args));
    return;
  }
  struct arts_rdzv_landing_s rdzv;
  (void)lock_stable_advertise(cache, n, &rdzv);
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
    /* The reply clears the stub bit before the re-send declares again. */
    uint64_t s, n;
    do {
      s = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
      if (CACHE_RW_ST(s) != CACHE_ST_REQ) {
        ARTS_ERROR("lock: guid %lu got a size round for a write request it "
                   "does not have out (word=%llx)",
                   (unsigned long)cache->db_guid, (unsigned long long)s);
      }
      n = s & ~CACHE_OWNER_STUB_BIT;
    } while (n != s && !atomic_compare_exchange_weak_explicit(
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
    next = cache_owner_compute_next(cur, CACHE_OP_CTS_RO, &act);
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

/* ===== arts_send_db_excl_forward ========================================
 * home → current owner.  Direct send (Cat-C): the owner's cache is always
 * installed (it became owner via DELIVER, or is the creator).  Self-send (owner
 * == home) calls the handler body directly (local hit, no wire). */
void arts_send_db_excl_forward(unsigned int owner_rank, arts_guid_t db_guid,
                               uint32_t mode, uint32_t target, uint32_t tag,
                               const struct arts_rdzv_landing_s *target_rdzv) {
  struct arts_msg_excl_forward_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_EXCL_FORWARD);
  p.db_guid = db_guid;
  p.mode = mode;
  p.target = target;
  p.tag = tag;
  p.tag_pad = 0;
  if (target_rdzv != NULL) {
    p.rdzv.addr = target_rdzv->addr;
    p.rdzv.key = target_rdzv->key;
    p.rdzv.txid = target_rdzv->txid;
    p.rdzv.cookie = target_rdzv->cookie;
  } else {
    p.rdzv = (struct arts_msg_rdzv_landing_s){0, 0, 0, 0};
  }
  if (owner_rank == arts_global_rank_id) {
    arts_handler_db_excl_forward(&p);
    return;
  }
  if (!arts_net_send_held((int)owner_rank, (char *)&p, sizeof(p),
                          ARTS_HELD_HOP)) {
    arts_transport_send_async((int)owner_rank, (char *)&p, sizeof(p));
  }
}

/* ===== arts_send_db_excl_deliver ========================================
 * owner → target.  Direct send (Cat-C): the target's cache is always
 * installed (it sent its own REQUEST first, or is the migration target named
 * by home).  Versionless — exclusive-lock serialization guarantees no stale
 * write can race.  The payload travels one-sided into the target's forwarded
 * landing.  The target is never this rank: the home never forwards a rank its
 * own read request, a migration to this rank is owner_try_execute's
 * self-grant, and a release reaching the migration edge holds the write
 * permission, which a pending migration to this rank excludes. */
void arts_send_db_excl_deliver(unsigned int target_rank, arts_guid_t db_guid,
                               uint32_t mode, uint32_t tag,
                               const struct arts_rdzv_landing_s *rdzv,
                               arts_shared_ptr_t src_h, uint64_t data_size) {
  struct arts_db_buffer_s *src =
      (struct arts_db_buffer_s *)arts_shared_get(src_h);
  struct arts_msg_excl_deliver_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_EXCL_DELIVER);
  p.header.rank = arts_global_rank_id;
  p.db_guid = db_guid;
  p.mode = mode;
  p.tag = tag;
  /* Always the DB's size; payload presence is rdzv_txid / trailing bytes. */
  p.data_size = data_size;
  p.rdzv_txid = 0;
  p.rdzv_cookie = (rdzv != NULL) ? rdzv->cookie : 0;
  if (target_rank == arts_global_rank_id) {
    ARTS_ERROR("lock: guid %lu: a deliver addressed to this rank itself",
               (unsigned long)db_guid);
  }
  if (src == NULL || data_size == 0 || rdzv == NULL || rdzv->txid == 0) {
    /* Data-less deliver (sentinel DB / nothing published). */
    if (src != NULL) {
      arts_db_buf_release(&src_h);
    }
    arts_transport_send_async((int)target_rank, (char *)&p, sizeof(p));
    return;
  }
  /* One-sided deliver: PUT straight from the owner buffer into the target's
   * forwarded landing; the strong ref transfers to the PUT's local
   * completion, pinning the source bytes until the fabric drains them. */
  p.data_size = data_size;
  p.rdzv_txid = rdzv->txid;
  arts_net_put_payload((int)target_rank, rdzv->addr, rdzv->key, rdzv->txid,
                       src->data, data_size, arts_db_buf_ref_release_cb,
                       (void *)src_h);
  arts_transport_send_async((int)target_rank, (char *)&p, sizeof(p));
}

/* ===== arts_send_db_excl_confirm ========================================
 * new owner → home: migration complete.  Direct send (Cat-C): home db_s is
 * always installed (home sent the FORWARD that triggered the migration).
 * Self-send (new owner == home) calls the home handler body directly. */
void arts_send_db_excl_confirm(unsigned int home_rank, arts_guid_t db_guid) {
  struct arts_msg_excl_confirm_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_EXCL_CONFIRM);
  p.db_guid = db_guid;
  if (home_rank == arts_global_rank_id) {
    arts_handler_db_excl_confirm(&p);
    return;
  }
  arts_transport_send_async((int)home_rank, (char *)&p, sizeof(p));
}

/* ===== arts_send_db_excl_recall =========================================
 * home → a rank holding an unmarked RO grant: "give it back".  Data-less,
 * fire-and-forget, sharing CONFIRM/RORET's packet.  There is NO ack: the
 * completion signal is the ordinary RO_RETURN this provokes, and the home's
 * r==0 edge is the continuation that fires the read->write flip.
 *
 * The fan-out MUST include the sending rank itself.  A rank that read and
 * retained, then wants to write, has to go through the home like anyone else
 * (there is no owner write fast path), so its own retained read grant is the
 * only thing standing between it and its own write.  Filtering `rank != self`
 * — the idiom the adjacent destroy fan-out uses — deadlocks it against itself,
 * deterministically. */
void arts_send_db_excl_recall(unsigned int target_rank, arts_guid_t db_guid) {
  struct arts_msg_excl_confirm_packet_s p;
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_EXCL_RECALL);
  p.db_guid = db_guid;
  if (target_rank == arts_global_rank_id) {
    arts_handler_db_excl_recall(&p);
    return;
  }
  arts_transport_send_async((int)target_rank, (char *)&p, sizeof(p));
}

/* ===== arts_handler_db_excl_recall ======================================
 * Cat-C.  One CAS; the count test and the transition must read the SAME word,
 * or a reader's acquire (rc++) landing between a separate load and the CAS
 * turns the "readers live" row into the "no readers" row. */
void arts_handler_db_excl_recall(void *item_v) {
  struct arts_msg_excl_confirm_packet_s *p =
      (struct arts_msg_excl_confirm_packet_s *)item_v;
  arts_shared_ptr_t db_h = arts_route_table_lookup_db(p->db_guid);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  if (db == NULL) {
    arts_shared_release(&db_h);
    return;
  }
  struct arts_db_cache_s *cache = &db->cache;
  uint64_t cur, next;
  bool retire = false;
  bool early = false;
  do {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    uint32_t ros = CACHE_RO_ST(cur);
    uint32_t ro_next = ros;
    retire = false;
    early = false;
    if (ros == CACHE_ST_GRANT && CACHE_RO_CNT(cur) == 0u) {
      ro_next = CACHE_ST_IDLE; /* nothing live here — give it back now */
      retire = true;
    } else if (ros == CACHE_ST_GRANT) {
      /* Readers are live.  Mark the grant to return voluntarily at its own zero
       * edge; do NOT retire it here.  Retiring under live readers lets the
       * home's r reach zero and opens a write phase beneath them — the exact
       * claim this protocol makes ("a retained copy is never read across a
       * write") — and in the worst ordering ownership comes back and the
       * install writes the stable buffer in place under them. */
      ro_next = CACHE_ST_GRANT_PURGE;
    } else if (ros == CACHE_ST_REQ) {
      /* The grant has not landed yet.  Remember the recall in bit 63 so the
       * DELIVER that eventually arrives commits GRANT_PURGE: a recall is one
       * hop while its grant is two (home -> owner -> here) and the transport
       * gives no per-peer FIFO, so overtaking is ordinary rather than exotic.
       * REQ must NEVER be retired here — that rank's r is the only pin holding
       * the owner's not-yet-drained serve node in place, and the DELIVER arm is
       * what owes the return for it. */
      if (CACHE_GRANTED(cur)) {
        arts_shared_release(&db_h); /* token already set — idempotent */
        return;
      }
      early = true;
      next = cur | CACHE_OWNER_LEASED_MASK;
      continue; /* fall through to the CAS below via the loop condition */
    } else {
      arts_shared_release(&db_h); /* IDLE / GRANT_PURGE — nothing to do */
      return;
    }
    next = CACHE_MAKE_FULL(CACHE_OWNER(cur), CACHE_RW_ST(cur), ro_next,
                           CACHE_MIGRATE_TARGET(cur), CACHE_RW_CNT(cur),
                           CACHE_RO_CNT(cur)) |
           (cur & CACHE_OWNER_STICKY_MASK);
    if (!retire) {
      next |= (cur & CACHE_OWNER_LEASED_MASK); /* debt still outstanding */
    }
  } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                  next, memory_order_acq_rel,
                                                  memory_order_acquire));
  if (early) {
    INCREMENT_NUM_EXCL_RECALL_EARLY_BY(1);
  }
  if (retire && CACHE_GRANTED(cur)) {
    arts_send_db_excl_roret(arts_guid_get_rank(cache->db_guid), cache->db_guid);
  }
  arts_shared_release(&db_h);
}

/* ===== arts_send_db_excl_roret ==========================================
 * reader → home: RO release (data-less).  Direct send (Cat-C): home db_s is
 * always installed (home sent the FORWARD(serve) that granted this reader its
 * copy).  Self-send (reader == home) calls the home handler body directly. */
void arts_send_db_excl_roret(unsigned int home_rank, arts_guid_t db_guid) {
  struct arts_msg_excl_confirm_packet_s p; /* CONFIRM/RORET share the struct */
  arts_fill_packet_header(&p.header, sizeof(p), MSG_DB_EXCL_RORET);
  p.db_guid = db_guid;
  if (home_rank == arts_global_rank_id) {
    arts_handler_db_excl_roret(&p);
    return;
  }
  arts_transport_send_async((int)home_rank, (char *)&p, sizeof(p));
}

/* ===== arts_handler_db_acquire =========================================
 * The coherent acquire of one dep — protocol-agnostic signature; the driver's
 * acquire_one_dep calls it.  item is this rank's pre-pinned db_s of the
 * block; args is {edt, db_guid, slot}; mode is read from depv[slot].mode.
 *
 * Single-atom acquire, as under PURGE: ONE CAS carries {count++, decision}
 * (see cache_owner_compute_next in arbiters.c for why the atomicity is
 * load-bearing).  The action decides where this acquire is served:
 *   SELF_SERVE  a write grant, a read grant or the resident copy serves the
 *               dep directly.  It never touches the pend stacks, which keeps
 *               the parked population equal to what a later grant's CAS reads.
 *   SEND_*      this acquire opened the request: park the waiter FIRST, then
 *               send — by the time any answer to the request can exist, the
 *               opener's node is already drainable.
 *   PARK        covered by the in-flight request: park.  The transition that
 *               serves it counts it in its CAS and waits for a push that trails
 *               it, so it is awaited, never lost. */
void arts_handler_db_acquire(void *item, void *args) {
  struct arts_db_s *db = (struct arts_db_s *)item;
  struct arts_ooo_args_db_acquire_s *a =
      (struct arts_ooo_args_db_acquire_s *)args;
  struct arts_edt_s *edt = a->edt;
  unsigned int slot = a->slot;
  struct arts_db_cache_s *cache = &db->cache;
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  arts_db_access_mode_t mode = depv[slot].mode;

  int op = (mode == DB_MODE_RW) ? CACHE_OP_ACQ_RW : CACHE_OP_ACQ_RO;
  uint32_t act;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    next = cache_owner_compute_next(cur, op, &act);
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
    /* The permission or the copy was already held here, so the dep is served
     * without asking anyone — the retained grant's whole point.  Where the
     * write grant or the resident copy serves it, this is where a block whose
     * create took no hold gets its storage: on the acquiring thread's node,
     * before the EDT is handed a pointer. */
    (void)arts_db_buf_ensure(
        cache, atomic_load_explicit(&cache->db_size, memory_order_relaxed));
    uint8_t census = arts_db_census_serving(false);
    mark_edt_ready_by_guid(edt->guid, slot);
    arts_db_census_served(census);
    return;
  }

  /* SEND_* / PARK: park the waiter.  Push BEFORE any send so the node is
   * drainable before an answer to the request can arrive. */
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

/* ===== arts_handler_db_excl_forward ====================================
 * Cat-C @owner.  item_v is the full wire packet (arts_msg_excl_forward_packet_s
 * *).  RW(mode) → install migrate_target (CAS into cache_state); RO(mode) →
 * push the reader rank onto ro_serve.  Then owner_try_execute drives the ship
 * (immediate if wc==0; otherwise the eventual release 0-edge does it).
 *
 * Cat-C route-table lookup: a NULL lookup means the DB was destroyed
 * concurrently → drop. */
void arts_handler_db_excl_forward(void *item_v) {
  struct arts_msg_excl_forward_packet_s *p =
      (struct arts_msg_excl_forward_packet_s *)item_v;
  arts_shared_ptr_t db_h = arts_route_table_lookup_db(p->db_guid);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  if (db == NULL) {
    /* Torn down after the home took the request in; the landing is the
     * requester's own buffer, so nothing is owed back. */
    arts_shared_release(&db_h);
    return;
  }
  struct arts_db_cache_s *cache = &db->cache;

  if ((arts_db_access_mode_t)p->mode == DB_MODE_RW) {
    /* Stash the migrate target's landing BEFORE the CAS that publishes the
     * target (single in-flight migration per owner — home serializes by
     * CONFIRM — so this plain store has no concurrent writer; the shipping
     * actor reads it only after observing the published target). */
    cache->migrate_rdzv.addr = p->rdzv.addr;
    cache->migrate_rdzv.key = p->rdzv.key;
    cache->migrate_rdzv.txid = p->rdzv.txid;
    cache->migrate_rdzv.cookie = p->rdzv.cookie;
    /* Install the migrate target into the word (single CAS preserving the
     * rest). push-before-decision is not needed here — there is exactly one
     * in-flight migrate target per owner (home serializes FORWARD-migrate by
     * CONFIRM). */
    uint64_t cur, next;
    do {
      cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
      next =
          CACHE_MAKE_FULL(CACHE_OWNER(cur), CACHE_RW_ST(cur), CACHE_RO_ST(cur),
                          p->target, CACHE_RW_CNT(cur), CACHE_RO_CNT(cur)) |
          (cur & (CACHE_OWNER_LEASED_MASK |
                  CACHE_OWNER_STICKY_MASK)); /* preserve ro_granted */
    } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                    next, memory_order_acq_rel,
                                                    memory_order_acquire));
  } else {
    /* Push the reader rank onto ro_serve, THEN owner_try_execute — push-then-
     * recheck (owner_try_execute re-reads wc==0) so a reader pushed just after
     * a drain is not stranded. */
    if (p->target == arts_global_rank_id) {
      /* A rank sends a read request only while it does not hold the bytes,
       * and becomes the owner only through its own write request, which
       * declares that read request: the home dismisses it (arrived after the
       * declaration), the write request takes it from the read queue
       * (arrived before and still queued), or it was already forwarded to
       * that phase's owner, which this rank becomes only later; a create's
       * claim closes the claimant's read requests instead.  So the home
       * never forwards a rank its own read. */
      ARTS_ERROR("lock: guid %lu: the home forwarded this rank its own read "
                 "request",
                 (unsigned long)cache->db_guid);
    }
    struct arts_lock_ro_serve_node_s *rn =
        (struct arts_lock_ro_serve_node_s *)arts_malloc(sizeof(*rn));
    rn->rank = p->target;
    rn->tag = p->tag;
    rn->rdzv.addr = p->rdzv.addr;
    rn->rdzv.key = p->rdzv.key;
    rn->rdzv.txid = p->rdzv.txid;
    rn->rdzv.cookie = p->rdzv.cookie;
    arts_lf_stack_push(&cache->ro_serve, &rn->link);
  }

  owner_try_execute(cache);
  arts_shared_release(&db_h);
}

/* ===== arts_handler_db_excl_deliver ====================================
 * Cat-C @target.  payload is the full contiguous wire buffer (header + db_size
 * data bytes); size is the byte count.
 *
 *   RW → become owner: install the data, set owner-bit + rw_st=GRANT (CAS),
 *        drain rw_pending (the parked RW writers are secured + ready), send
 *        CONFIRM to home, then owner_try_execute (in case a migrate_target /
 *        ro_serve was already pending — RW chain / RW→RO flip).
 *   RO → install the copy, set ro_st=GRANT (CAS), drain ro_pending (the parked
 *        RO readers are secured + ready).
 *
 * Cat-C route-table lookup: NULL ⇒ DB destroyed concurrently ⇒ drop.
 * Data is installed BEFORE the CAS so drained waiters observe it. */
/* Post-arrival tail of the DELIVER handler: the state transition + drains /
 * CONFIRM / return, run once the payload bytes are available.  `data` is the
 * inline same-rank payload; NULL on the one-sided path, whose bytes already
 * sit in the stable buffer.  Consumes db_h. */
static void lock_deliver_commit(arts_shared_ptr_t db_h, arts_guid_t db_guid,
                                uint32_t tag,
                                arts_db_access_mode_t mode, const void *data,
                                uint64_t data_size);

/* Rendezvous continuation: the deliver payload fully landed, in place in the
 * stable buffer for either mode.  Runs the commit. */
struct lock_deliver_landed_ctx_s {
  arts_shared_ptr_t db_h;
  arts_guid_t db_guid;
  arts_db_access_mode_t mode;
  uint64_t data_size;
  /* A one-sided DELIVER splits the packet frame from the commit frame: the
   * handler hands off to the rendezvous continuation and the packet's lifetime
   * ends there, while the CAS that decides GRANT vs GRANT_PURGE runs later in
   * lock_deliver_commit, which never sees the packet.  Every multinode RO serve
   * takes that path, so the tag MUST ride here. */
  uint32_t tag;
};

static void lock_deliver_landed_cb(void *arg) {
  struct lock_deliver_landed_ctx_s *ctx =
      (struct lock_deliver_landed_ctx_s *)arg;
  if (arts_shared_get(ctx->db_h) != NULL) {
    /* A one-sided payload comes from another rank. */
    uint8_t census = arts_db_census_serving(true);
    lock_deliver_commit(ctx->db_h, ctx->db_guid, ctx->tag, ctx->mode, NULL,
                        ctx->data_size);
    arts_db_census_served(census);
  } else {
    arts_shared_release(&ctx->db_h);
  }
  arts_free(ctx);
}

void arts_handler_db_excl_deliver(void *payload, size_t size) {
  struct arts_msg_excl_deliver_packet_s *p =
      (struct arts_msg_excl_deliver_packet_s *)payload;
  arts_db_access_mode_t mode = (arts_db_access_mode_t)p->mode;
  const void *data = (const char *)p + sizeof(*p);
  uint64_t data_size = (uint64_t)size - (uint64_t)sizeof(*p);

  arts_shared_ptr_t db_h = arts_route_table_lookup_db(p->db_guid);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  if (db == NULL) {
    /* Destroyed mid-flight: consume any pairing (the landing is the stable
     * buffer, cookie 0 — nothing to free). */
    arts_db_rdzv_discard_landing(p->rdzv_txid, p->data_size, p->rdzv_cookie);
    arts_shared_release(&db_h);
    return;
  }

  /* Hinted first touch: the size may still be unlearned here. */
  arts_db_cache_learn_size(&db->cache, p->data_size);

  if (p->rdzv_txid != 0) {
    /* One-sided deliver: pair this packet with the write completion (either
     * order); the commit runs once both are in. */
    struct lock_deliver_landed_ctx_s *ctx =
        (struct lock_deliver_landed_ctx_s *)arts_malloc(sizeof(*ctx));
    ctx->db_h = db_h;
    ctx->db_guid = p->db_guid;
    ctx->mode = mode;
    ctx->data_size = p->data_size;
    ctx->tag = p->tag;
    arts_net_rdzv_expect(p->rdzv_txid, p->data_size, lock_deliver_landed_cb,
                         ctx);
    return;
  }

  /* Same-rank / data-less deliver: inline payload (if any), no landing. */
  uint8_t census = arts_db_census_serving_from(p->header.rank);
  lock_deliver_commit(db_h, p->db_guid, p->tag, mode,
                      (data_size > 0u) ? data : NULL,
                      data_size); /* consumes db_h */
  arts_db_census_served(census);
}

static void lock_deliver_commit(arts_shared_ptr_t db_h, arts_guid_t db_guid,
                                uint32_t tag,
                                arts_db_access_mode_t mode, const void *data,
                                uint64_t data_size) {
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  struct arts_db_cache_s *cache = &db->cache;

  if (mode == DB_MODE_RW) {
    /* Migration is the ONLY thing that moves buffer data (owner→owner).  A
     * one-sided migration already landed IN PLACE in the stable buffer ("imm
     * seen => buffer valid"; no concurrent holder — migration is
     * CONFIRM-gated, single owner, global-lock excluded); a same-rank inline
     * payload installs here.  Then become owner: owner-bit + rw_st=GRANT in
     * one CAS (preserve counts + ro_st; migrate_target stays NO_TARGET — a
     * chained migrate is installed later by a fresh FORWARD). */
    if (data != NULL && data_size > 0u) {
      arts_db_buf_write_inplace(cache, data, data_size);
    }
    uint64_t cur, next;
    do {
      cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
      /* A migration answers this rank's write request: a request a create's
       * hold covered is absorbed at the home and never forwarded. */
      if (CACHE_RW_ST(cur) != CACHE_ST_REQ) {
        ARTS_ERROR("lock: guid %lu got a write grant it has no request out "
                   "for (word=%llx)",
                   (unsigned long)db_guid, (unsigned long long)cur);
      }
      next = CACHE_MAKE_FULL(1u, CACHE_ST_GRANT, CACHE_RO_ST(cur),
                             CACHE_MIGRATE_TARGET(cur), CACHE_RW_CNT(cur),
                             CACHE_RO_CNT(cur)) |
             (cur & (CACHE_OWNER_LEASED_MASK | CACHE_OWNER_RO_PAR_BIT));
    } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                    next, memory_order_acq_rel,
                                                    memory_order_acquire));
    /* RW grant serves this rank's RW + RO cohort (RW⊇RO): drain BOTH the parked
     * writers and any parked readers (a reader that requested RO before this
     * rank became the RW owner is parked in ro_pending and joins the RW grant
     * here).  Without the ro_pending drain those readers strand, rc never
     * reaches zero, and the migration — which waits for it — deadlocks. */
    lock_drain_pending(&cache->rw_pending, cache_owner_parked_rw(cur));
    lock_drain_pending(&cache->ro_pending, cache_owner_parked_ro(cur));
    /* Tell home the migration is complete so it can advance (RW chain / flip).
     */
    arts_send_db_excl_confirm(arts_guid_get_rank(db_guid), db_guid);
    /* A migrate_target / ro_serve may already be pending (home chained the next
     * action); drive it. */
    owner_try_execute(cache);
  } else {
    /* RO grant: a borrower receives a read-only copy from the owner, another
     * rank (the home never forwards a rank its own read).  The one-sided
     * bytes already sit in the stable buffer, an inline payload installs
     * here; set ro_st=GRANT + ro_granted (owes one RO_RETURN when its readers
     * drain), then serve them.  A read grant never meets this rank holding
     * the write grant or ownership: the home counts this grant in its read
     * phase until its RO_RETURN, so no write phase -- no migration to this
     * rank -- can start while it is out, and after a create's claim the owner
     * is this rank itself, whose read requests the claim closed.  Meeting
     * either is a grant for no request: fatal. */
    uint64_t cur, next;
    bool installed = false;
    do {
      cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
      if (CACHE_RW_ST(cur) == CACHE_ST_GRANT || CACHE_OWNER(cur) == 1u) {
        ARTS_ERROR("lock: guid %lu got a read grant while it holds the write "
                   "grant or ownership (word=%llx)",
                   (unsigned long)db_guid, (unsigned long long)cur);
      }
      if (CACHE_RO_ST(cur) != CACHE_ST_REQ) {
        ARTS_ERROR("lock: guid %lu got a read grant it has no read request "
                   "out for (word=%llx)",
                   (unsigned long)db_guid, (unsigned long long)cur);
      }
      if (!installed) {
        /* Deferred commit (universal invariant): a grant may become observable
         * only AFTER its bytes are installed.  A one-sided serve landed in the
         * stable buffer before this continuation ran; an inline payload is
         * installed here, BEFORE the CAS that publishes ros=GRANT: the moment
         * any acquirer observes GRANT (its acquire-load of cache_state pairing
         * with this CAS's release) it can be granted locally and read the
         * buffer, so publishing first would hand out the not-yet-installed
         * (stale) bytes.  Installing pre-publish is safe: while this rank's RO
         * grant is outstanding the home cannot start an RW phase (its r-count
         * includes this grant), so no ownership DELIVER can race this commit;
         * and with ros still REQ every local RO acquire parks, so no reader
         * touches the buffer until the publish. */
        if (data != NULL && data_size > 0u) {
          arts_db_buf_write_inplace(cache, data, data_size);
        }
        installed = true;
      }
      /* The grant this DELIVER installs relinquishes voluntarily if the home
       * stamped it (a writer was already queued when it was served) OR if a
       * recall overtook it: a recall landing on ro_st==REQ cannot retire a
       * grant that has not arrived, so it leaves the early-recall token in
       * bit 63 and this commit consumes it.  RECALL is one hop while the grant
       * is two (home -> owner -> here) and the transport gives no per-peer
       * FIFO, so that overtake is ordinary, not exotic. */
      bool relinquish = (tag != 0u) || CACHE_GRANTED(cur);
      uint32_t ro_next = relinquish ? CACHE_ST_GRANT_PURGE : CACHE_ST_GRANT;
      next = CACHE_MAKE_FULL(CACHE_OWNER(cur), CACHE_RW_ST(cur), ro_next,
                             CACHE_MIGRATE_TARGET(cur), CACHE_RW_CNT(cur),
                             CACHE_RO_CNT(cur)) |
             (cur & CACHE_OWNER_RO_PAR_BIT) | CACHE_OWNER_LEASED_MASK;
    } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                    next, memory_order_acq_rel,
                                                    memory_order_acquire));
    {
      /* Bytes were installed before the publish above; wake the parked
       * readers. */
      lock_drain_pending(&cache->ro_pending, cache_owner_parked_ro(cur));
      /* A grant marked to return while no reader holds it is returned by
       * whichever side observes the complete state second.
       *
       * The mark is a promise to give the grant back at the read count's zero
       * edge, and the release path keeps that promise.  A mark that becomes
       * visible when the count is ALREADY zero promises an edge that is in the
       * past: it cannot recur on its own, because a grant is relinquished only
       * on that edge or on a recall, and a recall declines a grant that is
       * already marked.  The promise would then be owed forever, and the
       * directory's outstanding-reader count never falls to zero — so every
       * writer queued behind this block waits on an event nobody will produce.
       *
       * Settling it here is safe precisely because the count covers waiters as
       * well as holders (it is raised before a waiter is made drainable), so a
       * zero count means no reader holds this grant and none is queued for it.
       *
       * The guard is re-evaluated on every attempt rather than tested once.
       * The mark shares its word with unrelated state — a local write request
       * moves the same word without touching either the mark or the read count
       * — so a failed exchange proves only that the word moved, NEVER that the
       * obligation found another payer.  Treating those two as the same fact is
       * what strands the grant: the unrelated writer's change is read as "the
       * other side has it" and neither side sends.  Re-reading the guard tells
       * them apart, and it is the only thing that has to be re-read.
       *
       * The loop cannot spin against a peer that is not progressing: every
       * retry follows another thread's committed change to the word, and the
       * exit is either this side's own success or a guard that a payer now
       * exists (a reader has joined, or the release edge settled it). */
      if (CACHE_RO_ST(next) == CACHE_ST_GRANT_PURGE) {
        uint64_t seen =
            atomic_load_explicit(&cache->cache_state, memory_order_acquire);
        for (;;) {
          if (CACHE_RO_ST(seen) != CACHE_ST_GRANT_PURGE ||
              CACHE_RO_CNT(seen) != 0u || !CACHE_GRANTED(seen)) {
            break; /* the mark has a payer, or has already been paid */
          }
          /* CACHE_MAKE_FULL yields bit63 == 0, so the debt is discharged in the
           * same word that retires the grant — the release path's rule. */
          uint64_t settled =
              CACHE_MAKE_FULL(CACHE_OWNER(seen), CACHE_RW_ST(seen),
                              CACHE_ST_IDLE, CACHE_MIGRATE_TARGET(seen),
                              CACHE_RW_CNT(seen), CACHE_RO_CNT(seen)) |
              (seen & CACHE_OWNER_STICKY_MASK);
          if (atomic_compare_exchange_weak_explicit(
                  &cache->cache_state, &seen, settled, memory_order_acq_rel,
                  memory_order_acquire)) {
            arts_send_db_excl_roret(arts_guid_get_rank(db_guid), db_guid);
            break;
          }
        }
      }
    }
  }

  arts_shared_release(&db_h);
}

/* ===== arts_db_release_rw / arts_db_release_ro =========================
 * Release — single-word CAS via cache_owner_compute_next, then dispatch the
 * returned action.
 *
 * RW: wc-- (+ 0-edge migration decision) in ONE CAS.  CACHE_ACT_MIGRATE means
 *     the 0-edge cleared owner-bit + migrate_target in the same next-state
 *     (publish-before-send); ship DELIVER(RW) to the target.  No home
 *     publish, no ACK — data is sticky on the owner.  No pending target →
 *     sticky (no-op).
 * RO: rc--; on the 0-edge — if this rank is still the owner,
 *     owner_try_execute (drive any pending RO serve / migration now that
 *     readers are gone); else RORET to home (a remote RO copy holder
 *     returning its grant).
 *
 * A count of 0 means this rank holds nothing to release — skip rather than
 * underflow. */
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
  uint32_t target = ARTS_EXCL_NO_TARGET;
  do {
    cur = atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    if (CACHE_RW_CNT(cur) == 0u) {
      return; /* nothing held */
    }
    target = CACHE_MIGRATE_TARGET(cur); /* captured pre-CAS for the ship */
    next = cache_owner_compute_next(cur, CACHE_OP_REL_RW, &act);
  } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                  next, memory_order_acq_rel,
                                                  memory_order_acquire));
  if (act == CACHE_ACT_MIGRATE) {
    /* owner-bit + migrate_target already cleared (published) inside the CAS;
     * ship the buffer to the target now. */
    arts_shared_ptr_t buf_h = arts_db_buf_acquire(cache);
    struct arts_db_buffer_s *buf =
        (struct arts_db_buffer_s *)arts_shared_get(buf_h);
    uint64_t ds = (buf != NULL) ? atomic_load_explicit(&cache->db_size,
                                                       memory_order_relaxed)
                                : 0u;
    /* Consume the pending migrate target's landing; buf_h transfers into the
     * deliver sender (PUT source pin). */
    struct arts_rdzv_landing_s mt_rdzv = cache->migrate_rdzv;
    cache->migrate_rdzv = (struct arts_rdzv_landing_s){0, 0, 0, 0};
    arts_send_db_excl_deliver(target, cache->db_guid, (uint32_t)DB_MODE_RW,
                              /*tag=*/0u, &mt_rdzv, buf_h, ds);
  } else if (CACHE_RW_CNT(next) == 0u && CACHE_OWNER(next) == 1u) {
    /* Sticky 0-edge (no pending migration): the writer phase just ended and we
     * remain the owner.  Drive any RO readers the home queued via
     * FORWARD(serve) during the write phase: they sit in ro_serve until the last
     * writer leaves.  No-op on a single node, where ro_serve is always empty. */
    owner_try_execute(cache);
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
    next = cache_owner_compute_next(cur, CACHE_OP_REL_RO, &act);
  } while (!atomic_compare_exchange_weak_explicit(&cache->cache_state, &cur,
                                                  next, memory_order_acq_rel,
                                                  memory_order_acquire));
  /* Whether a RO_RETURN goes out is derived from the COMMITTED transition, not
   * from a re-read of the word: ro_st(cur) in {REQ, GRANT, GRANT_PURGE} and
   * ro_st(next) == IDLE.  That CAS is the exactly-once token, so a concurrent
   * recall cannot make two sites send for one grant.  Keying off ro_granted
   * instead would fire on EVERY retaining zero edge — dropping the home's r,
   * letting a writer in while this rank still holds ro_st == GRANT, and serving
   * stale bytes to its next reader. */
  bool relinquished =
      (CACHE_RO_ST(cur) != CACHE_ST_IDLE) && (CACHE_RO_ST(next) == CACHE_ST_IDLE);
  if (relinquished && CACHE_GRANTED(cur)) {
    arts_send_db_excl_roret(arts_guid_get_rank(cache->db_guid), cache->db_guid);
  }
  /* The engine must be driven on EVERY rc zero edge, whether or not the grant
   * was relinquished: it is the only driver for a deferred lend or migration,
   * and under RETAIN the retaining edge is the common one. */
  if (act == CACHE_ACT_REL_RO) {
    uint64_t now =
        atomic_load_explicit(&cache->cache_state, memory_order_acquire);
    if (CACHE_OWNER(now) == 1u) {
      owner_try_execute(cache);
    }
  }
}

/* The cover gate [td:1 (31) | rwo:1 (30) | roo:1 (29) | owed:29].  A destroy
 * marks it and tears the home down when nothing is owed; otherwise the last
 * owed request's arrival does.  No release here owes the home anything it must
 * wait for: the payload legs move owner->owner, a pending migration means a
 * live acquire, which the destroy contract excludes, and a RO_RETURN or
 * CONFIRM that trails the teardown finds no home and is dropped. */
#define EXCL_GATE_TD 0x80000000u
#define EXCL_GATE_RWO 0x40000000u
#define EXCL_GATE_ROO 0x20000000u
#define EXCL_GATE_OWED_MASK 0x1fffffffu

enum gate_op { GATE_OWE, GATE_ARRIVE, GATE_RWO_ARRIVE, GATE_ROO_ARRIVE, GATE_MARK };

/* One RMW of the gate.  Returns true when this step is the one that leaves a
 * marked gate with nothing owed (the caller tears down); *marked reports
 * whether the gate was marked when the step committed. */
static bool gate_step(struct arts_db_s *db, enum gate_op op, bool *marked) {
  uint32_t cur = atomic_load_explicit(&db->cover_gate, memory_order_acquire);
  uint32_t next;
  bool edge;
  do {
    uint32_t owed = cur & EXCL_GATE_OWED_MASK;
    uint32_t flag = (op == GATE_RWO_ARRIVE)   ? EXCL_GATE_RWO
                    : (op == GATE_ROO_ARRIVE) ? EXCL_GATE_ROO
                                              : 0u;
    next = cur;
    edge = false;
    switch (op) {
    case GATE_OWE:
      if (owed == EXCL_GATE_OWED_MASK) {
        ARTS_ERROR("lock: guid %lu owes more requests than its gate holds "
                   "(gate=%x)",
                   (unsigned long)db->cache.db_guid, cur);
      }
      next = cur + 1u;
      break;
    case GATE_ARRIVE:
    case GATE_RWO_ARRIVE:
    case GATE_ROO_ARRIVE:
      if (owed == 0u || (flag != 0u && (cur & flag) == 0u)) {
        ARTS_ERROR("lock: guid %lu settles a request it was never owed "
                   "(gate=%x)",
                   (unsigned long)db->cache.db_guid, cur);
      }
      next = (cur - 1u) & ~flag;
      edge = (cur & EXCL_GATE_TD) != 0u && owed == 1u;
      break;
    case GATE_MARK:
      if ((cur & EXCL_GATE_TD) != 0u) {
        *marked = true;
        return false; /* a duplicate destroy */
      }
      next = cur | EXCL_GATE_TD;
      edge = (owed == 0u);
      break;
    }
  } while (!atomic_compare_exchange_weak_explicit(
      &db->cover_gate, &cur, next, memory_order_acq_rel, memory_order_acquire));
  *marked = (cur & EXCL_GATE_TD) != 0u;
  return edge;
}

/* The declared-arrival rule, before a request is processed.  Returns false
 * when the request is not processed: the claiming creator's pre-claim
 * requests are discarded unanswered, and a declared read request is
 * dismissed (a data-less answer; no forward, no count).  `reads`: a read
 * request that, if not declared, is processed as a read (not answered with a
 * size round). */
static bool lock_request_cover(struct arts_db_s *db, unsigned int requester,
                               arts_db_access_mode_t mode, uint32_t cover,
                               bool reads) {
  bool marked = false;
  if ((cover & EXCL_COVER_PRECLAIM) != 0u && requester == db->claim_rank) {
    if (gate_step(db, mode == DB_MODE_RW ? GATE_RWO_ARRIVE : GATE_ROO_ARRIVE,
                  &marked)) {
      arts_excl_home_teardown(db, db->cache.db_guid);
    }
    return false;
  }
  if (mode == DB_MODE_RW && (cover & EXCL_COVER_RO_OPEN) != 0u) {
    bool par = (cover & EXCL_COVER_RO_PAR) != 0u;
    (void)gate_step(db, GATE_OWE, &marked);
    if (arts_rank_bitset_put(&db->ro_parity, requester, par) == par) {
      if (gate_step(db, GATE_ARRIVE, &marked)) {
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
      /* Declared before it came: a write turn of this rank covers it.
       * Dismissed, never processed as a read. */
      bool edge = gate_step(db, GATE_ARRIVE, &marked);
      arts_send_db_excl_cts(db, requester,
                            atomic_load_explicit(&db->cache.db_size,
                                                 memory_order_relaxed),
                            (uint32_t)DB_MODE_RO, cover);
      if (edge) {
        arts_excl_home_teardown(db, db->cache.db_guid);
      }
      return false;
    }
  }
  return true;
}

void arts_handler_db_destroy(void *item_v, void *args_v) {
  struct arts_db_cache_s *cache = &((struct arts_db_s *)item_v)->cache;
  struct arts_ooo_args_db_destroy_s *a =
      (struct arts_ooo_args_db_destroy_s *)args_v;
  struct arts_db_s *db = arts_db_of_cache(cache);
  if (db == NULL) {
    return;
  }
  bool marked = false;
  if (gate_step(db, GATE_MARK, &marked)) {
    arts_excl_home_teardown(db, a->db_guid);
  }
}

/* ===== Home-side handlers ==============================================
 * The home directory arbiter + action dispatch.  Each handler runs the single-
 * word lock_state CAS via lock_owner_compute_next, then dispatches the returned
 * FORWARD action to the current owner (LOCK_OWNER() of the freshly-CAS'd word),
 * resolving the migrate target / served reader(s) from the lock-free queues.
 *
 * They run CONCURRENTLY across progress threads with no mutex; correctness
 * rests entirely on the single-word CAS plus the lock-free queues.  The
 * protocol tolerates message reorder because the transport guarantees no
 * per-peer FIFO: r counts OUTSTANDING RO grants, so it may transiently exceed
 * the rank count, and the write flip is gated on r reaching zero rather than on
 * any arrival order. */

/* The landing a FORWARD names for its target.  A target on another rank
 * advertised its own with its request.  A home target requested without one
 * but placed its stable buffer then, so the advertisement for it is drawn
 * here, and only when the owner is another rank (an owner that is the target
 * moves no payload).  The home's size is fixed from its create on, so this
 * reading is the one the request placed with. */
static struct arts_rdzv_landing_s
forward_target_landing(struct arts_db_s *db, unsigned int owner,
                       unsigned int target,
                       const struct arts_rdzv_landing_s *advertised) {
  struct arts_rdzv_landing_s out = *advertised;
  if (advertised->txid == 0 && target == arts_global_rank_id &&
      owner != arts_global_rank_id) {
    (void)lock_stable_advertise(&db->cache, lock_stable_size(&db->cache), &out);
  }
  return out;
}

static void lock_owner_home_forward(struct arts_db_s *db, uint32_t action,
                                   uint64_t word);

/* A read count returned at the home itself: the RO_RET transition of a read
 * request the home dismissed instead of serving. */
static void lock_owner_ro_ret_local(struct arts_db_s *db) {
  uint32_t action;
  uint64_t cur, next;
  do {
    cur = atomic_load_explicit(&db->lock_state, memory_order_acquire);
    next =
        lock_owner_compute_next(cur, EXCL_OP_RO_RET, /*new_owner=*/0u, &action);
  } while (!atomic_compare_exchange_weak_explicit(
      &db->lock_state, &cur, next, memory_order_acq_rel, memory_order_acquire));
  lock_owner_home_forward(db, action, next);
}

/* Dispatch a MIGRATE or SERVE_ALL FORWARD action after the lock_state CAS
 * committed.  `word` is the freshly-CAS'd lock_state, whose LOCK_OWNER() names
 * the rank to FORWARD to.  SERVE_ONE is not handled here: its target is the
 * requesting rank, which the word does not carry. */
static void lock_owner_home_forward(struct arts_db_s *db, uint32_t action,
                                   uint64_t word) {
  unsigned int owner = LOCK_OWNER(word);
  if (action == EXCL_ACTION_FORWARD_MIGRATE) {
    /* peek, not pop: the requester stays queued until its own CONFIRM pops it,
     * which is what makes "queue front == the rank that will CONFIRM" hold. */
    unsigned int target;
    struct arts_rdzv_landing_s target_rdzv;
    if (arts_home_grantreq_queue_peek(&db->rw_waiters, &target, &target_rdzv, NULL)) {
      target_rdzv = forward_target_landing(db, owner, target, &target_rdzv);
      arts_send_db_excl_forward(owner, db->cache.db_guid, (uint32_t)DB_MODE_RW,
                                target, /*tag=*/0u, &target_rdzv);
    }
  } else if (action == EXCL_ACTION_FORWARD_SERVE_ALL) {
    /* The relinquish policy stamped on this batch comes from a FRESH load taken
     * AFTER the drain below, never from the CAS word this frame committed.  The
     * drain takes the WHOLE stack, so this frame forwards other readers' nodes
     * too, and its own word can already be stale for them:
     *
     *   R1 commits its CAS (w==0) and is preempted before draining.  W commits
     *   w:0->1 and drains the roster, recalling only R1.  R2 sets its roster bit
     *   and pushes.  R1 resumes, drains BOTH and stamps both from its stale word
     *   -> R2 retains untagged and was never recalled, and w cannot fall to zero
     *   until W is served, which needs r==0, which needs R2's return.
     *
     * The error directions are asymmetric and settle it: over-stamping costs a
     * re-fetch, under-stamping hangs.  Moving the drain ahead of the forward, or
     * reverting to the committed word, silently reopens that hang. */
    /* The owner comes from that same fresh load, for the same reason: a frame
     * preempted between its CAS and its drain can resume after the phase it
     * committed has passed, and then drain readers that arrived in the write
     * phase that followed.  Forwarding those to the owner of its own word
     * would serve them from a rank that has migrated away (the serve node
     * strands there and r never falls) or alongside a migration PUT into the
     * same reader's buffer.  Every queued node is a counted reader, so a
     * post-drain load that shows the read phase pins both the phase and the
     * owner until these readers are served; any other phase means the nodes
     * are held readers that the next flip must serve — put them back and look
     * once more, since that flip may have drained between the drain and the
     * push-back.  The push-back and the flip's drain are acq_rel RMWs of the
     * one stack head: if the flip's drain is the later one it takes the nodes;
     * if not, the last push-back reads its drain and synchronizes with it, so
     * the re-load sees the flip's CAS (and the read phase, which the nodes'
     * own counts hold open).
     *
     * FORWARD-serve each reader UNCONDITIONALLY (the fixed single-target
     * packet, same shape as SERVE_ONE).  No owner identity tracking here: a
     * rank that is (or was) the owner gets the grant like any other, and its
     * cache returns it immediately (RO_RETURN) when it already holds the data
     * — see arts_handler_db_excl_deliver's RO arm. */
    for (;;) {
      arts_sched_fuzz_point();
      arts_lf_link_t *node = arts_lf_stack_drain(&db->ro_waiters);
      if (node == NULL) {
        break;
      }
      uint64_t fresh =
          atomic_load_explicit(&db->lock_state, memory_order_acquire);
      if (LOCK_PHASE(fresh) == EXCL_PHASE_RO) {
        unsigned int serve_owner = LOCK_OWNER(fresh);
        uint32_t tag = (LOCK_W(fresh) > 0u) ? 1u : 0u;
        uint32_t dismissed = 0u;
        while (node != NULL) {
          arts_lf_link_t *nx =
              atomic_load_explicit(&node->next, memory_order_relaxed);
          struct arts_lock_ro_node_s *rn =
              ARTS_CONTAINER_OF(node, struct arts_lock_ro_node_s, link);
          if (!arts_rank_bitset_put(&db->ro_queued, rn->rank, false)) {
            /* The rank's write request took the bit: its write turn covers
             * these readers.  Dismissed; the read count is released below,
             * once the batch is out.  The rank holds no read grant from this
             * request, so it leaves the recall roster before the dismiss
             * (its next request rosters it again). */
            (void)arts_rank_bitset_put(&db->ro_retainers, rn->rank, false);
            arts_send_db_excl_cts(db, rn->rank,
                                  atomic_load_explicit(&db->cache.db_size,
                                                       memory_order_relaxed),
                                  (uint32_t)DB_MODE_RO, rn->cover);
            dismissed++;
            arts_free(rn);
            node = nx;
            continue;
          }
          struct arts_rdzv_landing_s serve_rdzv =
              forward_target_landing(db, serve_owner, rn->rank, &rn->rdzv);
          arts_send_db_excl_forward(serve_owner, db->cache.db_guid,
                                    (uint32_t)DB_MODE_RO, rn->rank, tag,
                                    &serve_rdzv);
          arts_free(rn);
          node = nx;
        }
        while (dismissed-- > 0u) {
          lock_owner_ro_ret_local(db);
        }
        break;
      }
      arts_sched_fuzz_point();
      while (node != NULL) {
        arts_lf_link_t *nx =
            atomic_load_explicit(&node->next, memory_order_relaxed);
        arts_lf_stack_push(&db->ro_waiters, node);
        node = nx;
      }
      /* Same-word pairing with the flipping frame's drain of ro_waiters
       * (this function, after the CAS that opens the read phase): either
       * that drain takes these nodes or this load sees the read phase.  No
       * fence. */
      fresh = atomic_load_explicit(&db->lock_state, memory_order_acquire);
      if (LOCK_PHASE(fresh) != EXCL_PHASE_RO) {
        break; /* still held: the flip that ends this write phase serves them */
      }
    }
  }
}

/* ===== arts_handler_db_excl_request ====================================
 * Cat-B @home (OoO OOO_DB_EXCL_REQUEST).  item_v is the pre-pinned home db_s;
 * args_v is {requester, db_guid, mode}.
 *
 * A write request pushes onto its queue BEFORE the CAS, so "queued ⟹ counted"
 * holds and any transition that observes w also finds the node; a read request
 * pushes AFTER its CAS, so "queued ⟹ counted" holds the other way round — a
 * drain never forwards a serve for a reader the word does not yet count (see
 * the RO arm).  A write request that finds the lock idle FORWARDs the owner to
 * migrate to the queue front; a read request is served at once in the read-only
 * or idle phase and otherwise held for the eventual flip. */
void arts_handler_db_excl_request(void *item_v, void *args_v) {
  struct arts_db_s *db = (struct arts_db_s *)item_v;
  struct arts_ooo_args_db_excl_request_s *a =
      (struct arts_ooo_args_db_excl_request_s *)args_v;
  unsigned int requester = a->requester;
  arts_db_access_mode_t mode = (arts_db_access_mode_t)a->mode;

  bool size_round = a->rdzv.txid == 0 && requester != arts_global_rank_id &&
                    atomic_load_explicit(&db->cache.db_size,
                                         memory_order_relaxed) > 0 &&
                    arts_global_rank_count > 1;
  if (!lock_request_cover(db, requester, mode, a->cover, !size_round)) {
    return;
  }

  if (size_round) {
    /* First-touch request without a landing: the requester did not know
     * db_size.  Answer with the size (CTS) and do NOT enqueue — the deliver
     * plane requires a landing.  The requester re-issues with one. */
    INCREMENT_NUM_EXCL_SIZE_CTS_BY(1);
    arts_send_db_excl_cts(
        db, requester,
        atomic_load_explicit(&db->cache.db_size, memory_order_relaxed),
        (uint32_t)mode, a->cover);
    return;
  }

  arts_rank_bitset_set(&db->cached_ranks,
                       requester); /* destroy fan-out roster */

  uint32_t action;
  uint64_t cur, next;
  if (mode == DB_MODE_RW) {
    /* push-before-CAS: the requester is queued (and thus counted by w) before
     * the transition reads w, so a concurrent CONFIRM/peek sees it. */
    arts_home_grantreq_queue_push(&db->rw_waiters, requester, &a->rdzv,
                                  ARTS_GRANT_VERSION_NONE);
    arts_sched_fuzz_point();
    do {
      cur = atomic_load_explicit(&db->lock_state, memory_order_acquire);
      next = lock_owner_compute_next(cur, EXCL_OP_RW_ACQ, /*new_owner=*/0u,
                                    &action);
    } while (!atomic_compare_exchange_weak_explicit(&db->lock_state, &cur, next,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire));
    if (action != EXCL_ACTION_FORWARD_MIGRATE) {
      INCREMENT_NUM_EXCL_QUEUE_WAIT_BY(1);
    }
    lock_owner_home_forward(db, action, next);
    /* First writer of the round: ask every rank holding an unmarked read grant
     * to give it back.  Placed AFTER the forward so a self-send cannot make the
     * outer frame act on a pre-nesting word.
     *
     * On the w 0->1 edge with r == 0 there is nothing to recall (a retained
     * grant always contributes to r), so the sends are skipped — and that case
     * is exactly `action == MIGRATE`, because phase RW implies w >= 1 and phase
     * IDLE implies r == 0.  The roster word is NOT cleared then: the bit is set
     * before the requester's r++ commits, so a rank can hold a set bit with its
     * r++ still in flight, and erasing it would leave that rank with no future
     * edge able to recall it — its grant would be retained forever and the home
     * would never reach r == 0 again. */
    if (LOCK_W(next) == 1u && LOCK_R(next) > 0u) {
      struct arts_rank_bitset_s *bs = &db->ro_retainers;
      for (unsigned int i = 0; i < bs->nwords; i++) {
        uint64_t w = atomic_exchange_explicit(&bs->words[i], 0ull,
                                              memory_order_acq_rel);
        while (w != 0ull) {
          unsigned int b = (unsigned int)__builtin_ctzll(w);
          w &= (w - 1ull);
          INCREMENT_NUM_EXCL_RECALL_SENT_BY(1);
          arts_send_db_excl_recall((i * 64u) + b, db->cache.db_guid);
        }
      }
    }
  } else {
    /* Serving is ALWAYS a queue drain; there is deliberately no path that
     * serves the requester directly from a snapshot, because such a snapshot
     * can name an owner that has since migrated, stranding both the reader and
     * the home's r. */
    /* Roster the requester BEFORE the lock_state CAS.  That ordering is what
     * makes "served untagged implies eventually recalled" hold: the fetch_or
     * is sequenced before this rank's r++ CAS, and a later writer's w:0->1
     * CAS reads that CAS or a later RMW (every write of lock_state after its
     * init is an acq_rel RMW) and synchronizes with it, so the writer's
     * exchange of the roster, sequenced after its CAS, sees the bit.  The
     * edge is the one word both sides RMW, not the RMWs themselves: an RMW
     * is no fence.  Keep the set a fetch_or: a test-then-store could skip
     * the store on a stale read of a bit a writer has already taken. */
    arts_rank_bitset_set(&db->ro_retainers, requester);
    struct arts_lock_ro_node_s *n =
        (struct arts_lock_ro_node_s *)arts_malloc(sizeof(*n));
    n->rank = requester;
    n->cover = a->cover;
    n->rdzv = a->rdzv;
    do {
      cur = atomic_load_explicit(&db->lock_state, memory_order_acquire);
      next = lock_owner_compute_next(cur, EXCL_OP_RO_ACQ, /*new_owner=*/0u,
                                    &action);
    } while (!atomic_compare_exchange_weak_explicit(&db->lock_state, &cur, next,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire));
    if (action != EXCL_ACTION_FORWARD_SERVE_ONE) {
      INCREMENT_NUM_EXCL_QUEUE_WAIT_BY(1);
    }
    /* Queue AFTER the r++ CAS.  Every serve is a drain of the whole stack, so
     * a node queued before its count commits could be forwarded by another
     * frame while r does not yet hold it against the next write phase — and
     * the serve's PUT lands in the reader's stable buffer, which a migration
     * granted in that phase may then be writing.  Queued after the count, a
     * forwarded reader is always one the word pins the read phase on.
     *
     * The converse gap — a flip that observed this r++ and drained before the
     * push — is closed by the re-check below: the push and the flip's drain
     * are acq_rel RMWs of the one stack head, so either the flip's drain
     * reads the push and finds this node, or the push reads the drain and
     * synchronizes with it and the load finds the phase already read-only
     * (the read phase, never idle: this request's own count is unserved). */
    arts_sched_fuzz_point();
    arts_lf_stack_push(&db->ro_waiters, &n->link);
    /* RO/idle phase (SERVE_ONE): drain ro_waiters and FORWARD each reader to
     * the freshly-CAS'd owner — this serves the just-pushed node (plus any
     * reader a concurrent request pushed; XCHG drain partitions are disjoint).
     * RW phase (NONE): the node stays held for the RW→RO flip's SERVE_ALL,
     * unless that flip already ran. */
    if (action == EXCL_ACTION_FORWARD_SERVE_ONE) {
      lock_owner_home_forward(db, EXCL_ACTION_FORWARD_SERVE_ALL, next);
    } else {
      /* Same-word pairing with the flipping frame's drain of ro_waiters
       * (lock_owner_home_forward, after the CAS that opens the read phase):
       * either that drain takes this node or this load sees the read phase.
       * No fence. */
      arts_sched_fuzz_point();
      uint64_t fresh =
          atomic_load_explicit(&db->lock_state, memory_order_acquire);
      if (LOCK_PHASE(fresh) == EXCL_PHASE_RO) {
        lock_owner_home_forward(db, EXCL_ACTION_FORWARD_SERVE_ALL, fresh);
      }
    }
  }
}

/* ===== arts_handler_db_excl_confirm ====================================
 * Cat-C @home (direct dispatch).  item_v is the full wire packet
 * (arts_msg_excl_confirm_packet_s *); the new owner is packet->header.rank (the
 * sender — both wire and self-send set it via arts_fill_packet_header).
 *
 * Cat-C route-table lookup: NULL ⇒ a CONFIRM that trailed the block's
 * destroy (it is fire-and-forget, sent once the new owner's writers are
 * ready) ⇒ drop, harmlessly.  pop the
 * RW waiter (assert front == src), then CAS lock_owner_compute_next(CONFIRM,
 * new_owner=src) — this sets owner=src + w-- + next phase + action in ONE CAS.
 * Dispatch: MIGRATE → next RW migrate; SERVE_ALL → RW→RO flip drain. */
void arts_handler_db_excl_confirm(void *item_v) {
  struct arts_msg_excl_confirm_packet_s *p =
      (struct arts_msg_excl_confirm_packet_s *)item_v;
  unsigned int new_owner = p->header.rank;
  arts_shared_ptr_t db_h = arts_route_table_lookup_db(p->db_guid);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  if (db == NULL) {
    arts_shared_release(&db_h);
    return;
  }

  /* pop the confirming requester off the RW queue (it was the migrate target).
   * front must equal new_owner (the home FORWARDed migrate to the queue front,
   * and CONFIRM comes only from that owner).  The target was pushed before the
   * FORWARD that made it one, and only its own CONFIRM pops it, so the queue
   * is never empty here; an empty one would wrap w below.  A reused label can
   * reach it, since its generations are not told apart: a CONFIRM meant for
   * the previous generation lands on the next one's home. */
  unsigned int front;
  bool popped =
      arts_home_grantreq_queue_pop(&db->rw_waiters, &front, NULL, NULL);
  assert(popped && "a CONFIRM found no migrate target queued");
  (void)popped;
  (void)front;

  uint32_t action;
  uint64_t cur, next;
  arts_sched_fuzz_point();
  do {
    cur = atomic_load_explicit(&db->lock_state, memory_order_acquire);
    /* A CONFIRM ends a migration this home counted in w. */
    assert(LOCK_W(cur) != 0u);
    next = lock_owner_compute_next(cur, EXCL_OP_CONFIRM, new_owner, &action);
  } while (!atomic_compare_exchange_weak_explicit(
      &db->lock_state, &cur, next, memory_order_acq_rel, memory_order_acquire));
  lock_owner_home_forward(db, action, next);
  arts_shared_release(&db_h);
}

/* ===== arts_handler_db_excl_roret ======================================
 * Cat-C @home (direct dispatch).  item_v is the full wire packet
 * (arts_msg_excl_confirm_packet_s *, shared with CONFIRM); the returning reader
 * is packet->header.rank (informational — RO accounting is rank-agnostic).
 *
 * CAS lock_owner_compute_next(RO_RET) — r--; on r→0 with RW waiting, flip phase
 * to RW and MIGRATE to the next RW waiter (RO→RW flip).  Dispatch the action.
 */
void arts_handler_db_excl_roret(void *item_v) {
  struct arts_msg_excl_confirm_packet_s *p =
      (struct arts_msg_excl_confirm_packet_s *)item_v;
  arts_shared_ptr_t db_h = arts_route_table_lookup_db(p->db_guid);
  struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
  if (db == NULL) {
    arts_shared_release(&db_h);
    return;
  }

  uint32_t action;
  uint64_t cur, next;
  arts_sched_fuzz_point();
  do {
    cur = atomic_load_explicit(&db->lock_state, memory_order_acquire);
    /* r counts outstanding RO grants and a RO_RETURN follows its own grant,
     * so it never finds r at zero; a zero r would wrap and open a spurious
     * flip.  A reused label can reach it, as a CONFIRM can. */
    assert(LOCK_R(cur) != 0u);
    next =
        lock_owner_compute_next(cur, EXCL_OP_RO_RET, /*new_owner=*/0u, &action);
  } while (!atomic_compare_exchange_weak_explicit(
      &db->lock_state, &cur, next, memory_order_acq_rel, memory_order_acquire));
  lock_owner_home_forward(db, action, next);
  arts_shared_release(&db_h);
}
