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
#include "arts/transport/dispatcher.h"

#include <semaphore.h> /* sem_post (PUBLISH_CTS inline wake) */
#include <unistd.h>

#include "arts.h"
#include "arts/coherence/census.h"
#include "arts/coherence/coherence.h" /* arts_handler_db_grant_* bodies */
#include "arts/coherence/handlers.h"
#include "arts/counter/counter.h" /* arts_handler_time_sync_* */
#include "arts/db.h"
#include "arts/edt.h"
#include "arts/event.h"
#include "arts/gas/route_table.h" /* arts_route_table_lookup_db (Cat-C lookup-acquire) */
#include "arts/memory/regpool.h" /* push-rendezvous landing alloc */
#include "arts/ooo.h"
#include "arts/runtime_state.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/transport/net.h"
#include "arts/transport/protocol.h"
#include "arts/utils/malloc.h"
#include "arts/utils/shared.h" /* arts_shared_get / arts_shared_release */

/*
 * arts_transport_broadcast_shutdown — First step of the shutdown protocol.
 *
 * Inject a header-only MSG_SHUTDOWN onto the fabric to every other rank (same
 * path as every other message).  The caller then waits (bounded) on
 * arts_net_drain_outstanding for those sends to complete before proceeding to
 * local shutdown.
 */
void arts_transport_broadcast_shutdown(void) {
  if (arts_global_rank_count <= 1) {
    return;
  }
  for (unsigned int r = 0; r < arts_global_rank_count; r++) {
    if (r == arts_global_rank_id) {
      continue;
    }
    struct arts_msg_header_s packet;
    arts_fill_packet_header(&packet, sizeof(packet), MSG_SHUTDOWN);
    arts_transport_send_async((int)r, (char *)&packet, sizeof(packet));
  }
}

void arts_transport_setup(struct arts_config_s *config) {
  /* Establish the bootstrap TCP mesh; each producing thread injects its own
   * sends directly onto the fabric. */
  arts_socket_setup(config);
}

void arts_transport_dispatch_packet(struct arts_msg_header_s *packet) {
  switch (packet->message_type) {
  case MSG_SHUTDOWN: {
    ARTS_INFO("Node %u: Received shutdown message from node %u",
              arts_global_rank_id, packet->rank);
    /* Passive shutdown entry: we received MSG_SHUTDOWN from
     * another rank, so the dispatcher calls the lightweight RX handler
     * directly — idempotent CAS gate + worker stop, no re-broadcast, no
     * drain-wait.  The main thread handles network stop and bounded join
     * after the worker loop exits. */
    arts_handler_shutdown();
    break;
  }
  case MSG_EDT_SATISFY_SLOT: {
    struct arts_msg_edt_satisfy_slot_packet_s *pack =
        (struct arts_msg_edt_satisfy_slot_packet_s *)(packet);
    /* RX is at the EDT's home — route straight into the OoO engine, same as
     * arts_edt_satisfy_slot's home==self branch. */
    struct arts_ooo_args_edt_satisfy_s a = {.edt_guid = pack->edt,
                                            .data_guid = pack->db,
                                            .slot = pack->slot,
                                            .mode = pack->mode};
    arts_ooo_dispatch_or_defer_guid(pack->edt, OOO_EDT_SATISFY_SLOT, &a,
                                    sizeof(a));
    break;
  }
  case MSG_EVENT_SATISFY_SLOT: {
    struct arts_msg_event_satisfy_slot_packet_s *pack =
        (struct arts_msg_event_satisfy_slot_packet_s *)(packet);
    /* RX is at the event's home — route straight into the OoO engine, same as
     * arts_event_satisfy_slot's home==self branch. */
    struct arts_ooo_args_event_satisfy_s a = {
        .event_guid = pack->event, .data_guid = pack->db, .slot = pack->slot};
    arts_ooo_dispatch_or_defer_guid(pack->event, OOO_EVENT_SATISFY_SLOT, &a,
                                    sizeof(a));
    break;
  }
  case MSG_EVENT_ADD_DEPENDENCE: {
    ARTS_DEBUG("Dependence Received");
    struct arts_msg_add_dependence_packet_s *pack =
        (struct arts_msg_add_dependence_packet_s *)(packet);
    /* The wire message means source == event (an EDT/DB source satisfies
     * immediately and is never shipped as ADD_DEPENDENCE), and RX is at the
     * source event's home — route straight into the OoO engine, same as
     * arts_event_add_dependence's home==self branch.  No generic src-kind
     * re-derivation. */
    struct arts_ooo_args_event_add_dep_s a = {.source = pack->source,
                                              .destination = pack->destination,
                                              .slot = pack->slot,
                                              .mode = pack->mode};
    arts_ooo_dispatch_or_defer_guid(pack->source, OOO_EVENT_ADD_DEPENDENCE, &a,
                                    sizeof(a));
    break;
  }
  case MSG_EDT_CREATE: {
    ARTS_DEBUG("EDT Create Received");
    struct arts_msg_object_blob_packet_s *pack =
        (struct arts_msg_object_blob_packet_s *)(packet);
    if (pack->header.size < sizeof(*pack) + sizeof(struct arts_edt_s)) {
      ARTS_ERROR("EDT create blob of %lu bytes, below the minimum %lu bytes",
                 (unsigned long)pack->header.size,
                 (unsigned long)(sizeof(*pack) + sizeof(struct arts_edt_s)));
    }
    arts_edt_create_enter(pack, pack + 1,
                          (uint32_t)(pack->header.size - sizeof(*pack)));
    break;
  }
  case MSG_EVENT_CREATE: {
    ARTS_DEBUG("Event Create Received");
    struct arts_msg_object_blob_packet_s *pack =
        (struct arts_msg_object_blob_packet_s *)(packet);
    if (pack->header.size !=
        sizeof(*pack) + sizeof(struct arts_event_s)) {
      ARTS_ERROR("event create blob of %lu bytes, expected %lu",
                 (unsigned long)pack->header.size,
                 (unsigned long)(sizeof(*pack) + sizeof(struct arts_event_s)));
    }
    arts_event_create_enter(pack, pack + 1);
    break;
  }
  case MSG_TIME_SYNC_REQUEST: {
    ARTS_DEBUG("Time Sync Request Received");
    arts_handler_time_sync_request(packet);
    break;
  }
  case MSG_TIME_SYNC_RESPONSE: {
    ARTS_DEBUG("Time Sync Response Received");
    arts_handler_time_sync_response(packet);
    break;
  }
  /* ===== coherence wire-message dispatch =============
   * No coherence message carries a block's payload on the wire: payloads
   * travel one-sided and pair with their packet by rdzv_txid inside the
   * handler.  Only GRANT_RESPONSE has a trailer, its dedup map.
   *
   * GRANT_REQUEST: shared by the arms with a migrating grant (VAL, INV)
   * under both write policies, but EXCL has no such concept.  Fatal in
   * EXCL builds to catch binary mode mismatch.
   * GRANT_INVALIDATE is handled in its own block below: a direct call under
   * both write policies of a RETAIN build, never deferred, and refused by
   * every other build.
   */
#if defined(ARTS_PROTOCOL_FLUSH)
/* Refused in one place below: this arm has none of these rounds. */
#elif defined(ARTS_PROTOCOL_EXCL)
  case MSG_DB_GRANT_REQUEST: {
    ARTS_ERROR("EXCL build received exclusivity message type %d from rank "
               "%u — this protocol has no migrating grant; binary mode mismatch?",
               packet->message_type, packet->rank);
    break;
  }
#else  /* VAL, INV: full handlers */
  case MSG_DB_GRANT_REQUEST: {
    ARTS_DEBUG("Coh GRANT_REQUEST Received");
    struct arts_msg_grant_request_packet_s *pack =
        (struct arts_msg_grant_request_packet_s *)(packet);
    struct arts_ooo_args_db_grant_request_s args = {
        .requester = pack->header.rank,
        .db_guid = pack->db_guid,
        .rdzv = {.addr = pack->rdzv.addr,
                 .key = pack->rdzv.key,
                 .txid = pack->rdzv.txid,
                 .cookie = pack->rdzv.cookie},
        .have_version = pack->have_version & ARTS_GRANT_REQ_VERSION_MASK,
        .cover = (uint32_t)(pack->have_version >> ARTS_GRANT_REQ_VERSION_BITS),
    };
    arts_ooo_dispatch_or_defer_guid(pack->db_guid, OOO_DB_GRANT_REQUEST,
                                    &args, sizeof(args));
    break;
  }
#endif /* model dispatch for MSG_DB_GRANT_REQUEST */
  /* GRANT_RETURN — the voluntary hand-back of a migrating grant, which
   * exists only where VAL or INV run the PURGE release policy.  Under RETAIN
   * the home revokes the grant, and EXCL has no migrating grant, so receiving
   * one is a mode mismatch. */
#if defined(ARTS_PROTOCOL_FLUSH)
/* Refused in one place below: this arm has none of these rounds. */
#elif (defined(ARTS_PROTOCOL_VAL) || defined(ARTS_PROTOCOL_INV)) &&              \
    defined(ARTS_RELEASE_PURGE)
  case MSG_DB_GRANT_RETURN: {
    ARTS_DEBUG("Coh GRANT_RETURN Received");
    struct arts_msg_grant_return_packet_s *pack =
        (struct arts_msg_grant_return_packet_s *)(packet);
    struct arts_ooo_args_db_grant_return_s args = {
        .returner = pack->header.rank,
        .db_guid = pack->db_guid,
        .cv = pack->cv,
    };
    arts_ooo_dispatch_or_defer_guid(pack->db_guid, OOO_DB_GRANT_RETURN, &args,
                                    sizeof(args));
    break;
  }
#else
  case MSG_DB_GRANT_RETURN: {
    ARTS_ERROR("this build received a voluntary grant return from rank %u — "
               "only a PURGE build of VAL or INV hands a migrating grant "
               "back; binary mode mismatch?",
               packet->rank);
    break;
  }
#endif /* grant-arm PURGE */
  /* GRANT_INVALIDATE — protocol-split.
   *   VAL, INV under RETAIN (WT and WB): NOT deferred.  Home publishes the
   *           invalidate target (rw_holder) only after that rank's cache
   *           install — the CONFIRM owner-swap (post-install in both write
   *           policies) or the DB_CREATE on the creator — so the target's
   *           cache is provably already installed when INVALIDATE arrives.
   *           Call the pure handler body directly with the looked-up cache.
   *   EXCL: no migrating grant; PURGE: the holder returns the grant itself.
   *           Both refuse the message below. */
#if defined(ARTS_PROTOCOL_FLUSH)
/* Refused in one place below: this arm has none of these rounds. */
#elif defined(ARTS_PROTOCOL_EXCL)
  case MSG_DB_GRANT_INVALIDATE: {
    ARTS_ERROR("EXCL build received INVALIDATE from rank %u — "
               "this protocol has no migrating grant; binary mode mismatch?",
               packet->rank);
    break;
  }
#elif defined(ARTS_RELEASE_PURGE)
  case MSG_DB_GRANT_INVALIDATE: {
    ARTS_ERROR("voluntary-return build received INVALIDATE from rank %u — "
               "under this release policy the grant comes back at the "
               "holder's own idle edge and the home never asks for it; "
               "binary mode mismatch?",
               packet->rank);
    break;
  }
#else /* VAL and INV under RETAIN share the direct-call body */
  case MSG_DB_GRANT_INVALIDATE: {
    ARTS_DEBUG("Coh GRANT_INVALIDATE Received");
    struct arts_msg_grant_invalidate_packet_s *pack =
        (struct arts_msg_grant_invalidate_packet_s *)(packet);
    struct arts_ooo_args_db_grant_invalidate_s args = {
        .db_guid = pack->db_guid,
        .new_owner_rank = pack->new_owner_rank,
        .new_owner_rdzv = {.addr = pack->new_owner_rdzv.addr,
                           .key = pack->new_owner_rdzv.key,
                           .txid = pack->new_owner_rdzv.txid,
                           .cookie = pack->new_owner_rdzv.cookie},
        .new_owner_have = pack->new_owner_have,
    };
    /* Pin the db_s for the handler's duration (the embedded cache is its FIRST
     * member, offset 0) so a concurrent DESTROY on another progress thread
     * cannot free it mid-handler.  The home publishes the rw_holder target only
     * after that rank's cache install, so the db_s is present for every
     * program within the destroy contract.  A destroy issued while the
     * requester's acquire was pending may have NULLed the route slot first;
     * the teardown frees the waiters, so an orphaned INVALIDATE for a
     * torn-down DB is simply dropped. */
    arts_shared_ptr_t db_h = arts_route_table_lookup_db(pack->db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
    if (db != NULL) {
      arts_handler_db_grant_invalidate(db, &args);
    }
    arts_shared_release(&db_h);
    break;
  }
#endif /* model dispatch for MSG_DB_GRANT_INVALIDATE */
#if defined(ARTS_PROTOCOL_FLUSH)
/* Refused in one place below: this arm has none of these rounds. */
#elif defined(ARTS_PROTOCOL_EXCL) || defined(ARTS_PROTOCOL_INV)
  case MSG_DB_SNAPSHOT_REQUEST:
  case MSG_DB_SNAPSHOT_RESPONSE: {
    ARTS_ERROR("EXCL/INV build received snapshot message type %d from rank %u — "
               "this protocol has no RO snapshot round; binary mode mismatch?",
               packet->message_type, packet->rank);
    break;
  }
#else  /* VAL: snapshot handlers */
  case MSG_DB_SNAPSHOT_REQUEST: {
    ARTS_DEBUG("Coh SNAPSHOT_REQUEST Received");
    struct arts_msg_snapshot_request_packet_s *pack =
        (struct arts_msg_snapshot_request_packet_s *)(packet);
    struct arts_ooo_args_db_snapshot_request_s args = {
        .requester = pack->header.rank,
        .db_guid = pack->db_guid,
        .cover = pack->cover,
        .rdzv = {.addr = pack->rdzv.addr,
                 .key = pack->rdzv.key,
                 .txid = pack->rdzv.txid,
                 .cookie = pack->rdzv.cookie},
    };
    arts_ooo_dispatch_or_defer_guid(pack->db_guid, OOO_DB_SNAPSHOT_REQUEST,
                                    &args, sizeof(args));
    break;
  }
  case MSG_DB_SNAPSHOT_RESPONSE: {
    ARTS_DEBUG("Coh SNAPSHOT_RESPONSE Received");
    struct arts_msg_snapshot_response_packet_s *pack =
        (struct arts_msg_snapshot_response_packet_s *)(packet);
    /* No inline payload rides the wire anymore (the snapshot payload travels
     * one-sided and pairs by rdzv_txid inside the handler); a data-bearing
     * response is metadata-only.  Cat-C lookup-acquire-or-drop: HIT runs the
     * pure body against the ref-pinned db_s; MISS (DB destroyed / slot
     * NULL-stored) resumes nothing — the parked EDT this 1:1 response would
     * resume was torn down — and only ends the landing. */
    struct arts_db_snapshot_response_args_s args = {
        .cover = pack->cover,
        .data_present = pack->data_present,
        .version = pack->version,
        .db_size = pack->db_size,
        .rdzv_txid = pack->rdzv_txid,
        .rdzv_cookie = pack->rdzv_cookie,
    };
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      /* A read answer on the wire comes from another rank. */
      uint8_t census = arts_db_census_serving(true);
      arts_handler_db_snapshot_response(db, &args);
      arts_db_census_served(census);
    } else {
      /* MISS: still consume the txid of any one-sided payload in flight so
       * the pairing table stays leak-free; the landing ends when that payload
       * arrives, or at once when none was sent. */
      arts_db_rdzv_discard_landing(pack->rdzv_txid, pack->db_size,
                                   pack->rdzv_cookie);
    }
    arts_shared_release(&h);
    break;
  }
#endif /* model dispatch for MSG_DB_SNAPSHOT_REQUEST and _RESPONSE */
  case MSG_DB_CREATE: {
    ARTS_DEBUG("Coh DB_CREATE_COHERENT Received");
    arts_db_create_announce_enter(
        (struct arts_msg_db_create_coherent_packet_s *)(packet));
    break;
  }
  case MSG_DB_DESTROY: {
    ARTS_DEBUG("Coh DESTROY_REQ Received");
    struct arts_msg_destroy_packet_s *pack =
        (struct arts_msg_destroy_packet_s *)(packet);
    struct arts_ooo_args_db_destroy_s args = {
        .requester = pack->header.rank,
        .db_guid = pack->db_guid,
    };
    arts_ooo_dispatch_or_defer_guid(pack->db_guid, OOO_DB_DESTROY, &args,
                                    sizeof(args));
    break;
  }
  case MSG_DB_CACHE_DESTROY: {
    ARTS_DEBUG("Coh CACHE_DESTROY Received");
    struct arts_msg_cache_destroy_packet_s *pack =
        (struct arts_msg_cache_destroy_packet_s *)(packet);
    /* Cat-C lookup-acquire-or-drop: HIT retires this rank's cache (parked
     * acquirers are freed with it, never resumed); MISS (a reused GUID whose
     * other lifetime's notice already retired it) silently drops. */
    struct arts_db_cache_destroy_args_s args = {.db_guid = pack->db_guid};
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_cache_destroy(db, &args);
    }
    arts_shared_release(&h);
    break;
  }
  /* The FLUSH arm's own rounds: the fetch that serves an acquire, and the
   * write-back round that serves an RW release. */
#if defined(ARTS_PROTOCOL_FLUSH)
  case MSG_DB_FETCH_REQUEST: {
    struct arts_msg_fetch_request_packet_s *pack =
        (struct arts_msg_fetch_request_packet_s *)(packet);
    struct arts_ooo_args_db_fetch_request_s args = {
        .requester = pack->header.rank,
        .db_guid = pack->db_guid,
        .edt_guid = pack->edt_guid,
        .slot = pack->slot,
        .rdzv = {.addr = pack->rdzv.addr, .key = pack->rdzv.key,
                 .txid = pack->rdzv.txid, .cookie = pack->rdzv.cookie},
    };
    arts_ooo_dispatch_or_defer_guid(pack->db_guid, OOO_DB_FETCH_REQUEST, &args,
                                    sizeof(args));
    break;
  }
  case MSG_DB_FETCH_RESPONSE: {
    struct arts_msg_fetch_response_packet_s *pack =
        (struct arts_msg_fetch_response_packet_s *)(packet);
    struct arts_db_fetch_response_args_s args = {
        .edt_guid = pack->edt_guid, .slot = pack->slot, .kind = pack->kind,
        .db_size = pack->db_size, .rdzv_txid = pack->rdzv_txid,
        .rdzv_cookie = pack->rdzv_cookie, .line_addr = pack->line_addr,
        .line_rkey = pack->line_rkey, .flush_txid = pack->flush_txid};
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_fetch_response(db, &args);
    } else {
      arts_db_flush_discard_landing(pack->rdzv_txid, pack->db_size,
                                    pack->rdzv_cookie);
    }
    arts_shared_release(&h);
    break;
  }
  case MSG_DB_FLUSH_COMMIT: {
    struct arts_msg_flush_commit_packet_s *pack =
        (struct arts_msg_flush_commit_packet_s *)(packet);
    struct arts_db_flush_commit_args_s args = {
        .releaser = pack->header.rank, .db_guid = pack->db_guid,
        .txid = pack->txid, .sem = pack->sem};
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    arts_handler_db_flush_commit(arts_shared_get(h), &args); /* NULL = gone: still ACK */
    arts_shared_release(&h);
    break;
  }
  case MSG_DB_FLUSH_ACK: {
    struct arts_msg_flush_ack_packet_s *pack =
        (struct arts_msg_flush_ack_packet_s *)(packet);
    struct arts_db_flush_ack_args_s args = {.sem = pack->sem,
                                            .next_txid = pack->next_txid};
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    arts_handler_db_flush_ack(arts_shared_get(h), &args); /* NULL = no refill, still post */
    arts_shared_release(&h);
    break;
  }
  case MSG_DB_FLUSH_ANNOUNCE: {
    struct arts_msg_flush_announce_packet_s *pack =
        (struct arts_msg_flush_announce_packet_s *)(packet);
    struct arts_ooo_args_db_flush_announce_s args = {
        .releaser = pack->header.rank, .db_guid = pack->db_guid, .sem = pack->sem};
    /* A remote creator releases as soon as it has written its copy, which can
     * be before its own CREATE has been handled here; a route-table miss
     * cannot tell that from a destroy, so the announce defers and replays on
     * the install's drain. */
    arts_ooo_dispatch_or_defer_guid(pack->db_guid, OOO_DB_FLUSH_ANNOUNCE, &args,
                                    sizeof(args));
    break;
  }
  case MSG_DB_FLUSH_CTS: {
    struct arts_msg_flush_cts_packet_s *pack =
        (struct arts_msg_flush_cts_packet_s *)(packet);
    struct arts_db_flush_cts_args_s args = {
        .sem = pack->sem, .line_addr = pack->line_addr,
        .line_rkey = pack->line_rkey, .txid = pack->txid};
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    arts_handler_db_flush_cts(arts_shared_get(h), &args);
    arts_shared_release(&h);
    break;
  }
#else
  case MSG_DB_FETCH_REQUEST:
  case MSG_DB_FETCH_RESPONSE:
  case MSG_DB_FLUSH_COMMIT:
  case MSG_DB_FLUSH_ACK:
  case MSG_DB_FLUSH_ANNOUNCE:
  case MSG_DB_FLUSH_CTS: {
    ARTS_ERROR("this build received a FLUSH-arm message (type %d) from rank %u "
               "— binary mode mismatch?", packet->message_type, packet->rank);
    break;
  }
#endif /* ARTS_PROTOCOL_FLUSH */
  /* GRANT_RESPONSE: the single ownership-transfer wire message.  One layout
   * carries the dedup map, which only VAL WB fills (every other arm sends
   * map_entry_count=0); the payload travels one-sided.  EXCL has no
   * ownership transfer and fatals to catch a binary mode mismatch. */
#if defined(ARTS_PROTOCOL_FLUSH)
/* Refused in one place below: this arm has none of these rounds. */
#elif defined(ARTS_PROTOCOL_EXCL)
  case MSG_DB_GRANT_RESPONSE:
  case MSG_DB_GRANT_CTS: {
    ARTS_ERROR("EXCL build received ownership-transfer message type %d "
               "from rank %u — this protocol has no migrating grant; binary "
               "mode mismatch?",
               packet->message_type, packet->rank);
    break;
  }
#else  /* VAL, INV: one layout */
  case MSG_DB_GRANT_RESPONSE: {
    ARTS_DEBUG("Coh GRANT_RESPONSE Received");
    /* The (small) serialized map immediately follows the header and nothing
     * trails it; the buffer payload travels one-sided and pairs by rdzv_txid
     * inside the handler. */
    arts_handler_db_grant_response((void *)packet, (size_t)packet->size);
    break;
  }
  case MSG_DB_GRANT_CTS: {
    ARTS_DEBUG("Coh GRANT_CTS Received");
    struct arts_msg_grant_cts_packet_s *pack =
        (struct arts_msg_grant_cts_packet_s *)(packet);
    /* Cat-C lookup-acquire-or-drop: HIT learns db_size + re-issues the
     * in-flight request with a landing; MISS (DB destroyed) silently drops:
     * the requester's parked waiters are acquires the destroy contract
     * excludes, freed with its cache and never resumed. */
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_grant_cts(db, pack);
    }
    arts_shared_release(&h);
    break;
  }
#endif /* model dispatch for MSG_DB_GRANT_RESPONSE */
/* The CTS leg exists only when a publish carries payload — i.e. under WT
 * (and EXCL's purge policy).  INV's WB publish is control-only, so it never
 * announces. */
#if defined(ARTS_PROTOCOL_FLUSH)
/* Refused in one place below: this arm has none of these rounds. */
#elif (defined(ARTS_PROTOCOL_EXCL) && defined(ARTS_RELEASE_RETAIN)) ||          \
    (!defined(ARTS_PROTOCOL_EXCL) && defined(ARTS_WRITE_POLICY_WB))
  case MSG_DB_PUBLISH_CTS: {
    ARTS_ERROR("this build received PUBLISH_CTS from rank %u — none of its "
               "publishes carries a payload; binary mode mismatch?",
               packet->rank);
    break;
  }
#else
  /* PUBLISH_CTS: the announce leg of a WT payload publish (VAL, INV), and
   * EXCL PURGE's landing-less RW release (a creator-seeded hold that never
   * received a grant). */
  case MSG_DB_PUBLISH_CTS: {
    ARTS_DEBUG("Coh PUBLISH_CTS Received");
    struct arts_msg_publish_cts_packet_s *pack =
        (struct arts_msg_publish_cts_packet_s *)(packet);
    /* Cat-C SPECIAL — pointer-identity wake of the blocked releaser's heap
     * rendezvous (sem first member).  Cache-independent: write the landing
     * fields, THEN post (sem_post is the release/acquire edge), even if the
     * DB is being torn down — a stranded releaser must never hang. */
    struct arts_db_pub_rendezvous_s *wr =
        (struct arts_db_pub_rendezvous_s *)(uintptr_t)pack->cv;
    if (wr != NULL) {
      wr->landing.addr = pack->landing.addr;
      wr->landing.key = pack->landing.key;
      wr->landing.txid = pack->landing.txid;
      wr->landing.cookie = pack->landing.cookie;
      sem_post(&wr->sem);
    }
    break;
  }
#endif /* arm dispatch for MSG_DB_PUBLISH_CTS */
/* Every arm that publishes at a release: WT write policies (payload write-through)
 * and INV under either write policy (its release always asks the home for an
 * invalidation round, carrying payload only under WT). */
#if defined(ARTS_PROTOCOL_FLUSH)
  /* PUBLISH / PUBLISH_ACK are refused in one place below; the create's line
   * credential is this arm's too, and lands in its own cache fields. */
  case MSG_DB_CREATE_RETURN: {
    ARTS_DEBUG("Coh CREATE_RETURN Received");
    struct arts_msg_db_create_return_packet_s *pack =
        (struct arts_msg_db_create_return_packet_s *)(packet);
    /* Cat-C lookup-or-drop: record the creator's first flush credit.  The
     * creator always installed its descriptor before sending the create this
     * answers, so a MISS means the DB is already destroyed — the credit dies
     * with it (a credit is a hint, never a precondition). */
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL && pack->credit_txid != 0) {
      struct arts_db_cache_s *cache = &db->cache;
      atomic_store_explicit(&cache->home_line_addr, pack->credit_addr,
                            memory_order_relaxed);
      atomic_store_explicit(&cache->home_line_rkey, pack->credit_rkey,
                            memory_order_relaxed);
      atomic_store_explicit(&cache->flush_txid, pack->credit_txid,
                            memory_order_release);
    }
    arts_shared_release(&h);
    break;
  }
#elif defined(ARTS_PROTOCOL_EXCL) ||                                         \
    (defined(ARTS_WRITE_POLICY_WB) && !defined(ARTS_PROTOCOL_INV))
  case MSG_DB_PUBLISH:
  case MSG_DB_PUBLISH_ACK:
  case MSG_DB_CREATE_RETURN: {
    ARTS_ERROR("this build received publish message type %d from rank %u — "
               "its releases publish nothing; binary mode mismatch?",
               packet->message_type, packet->rank);
    break;
  }
#else  /* full handlers */
  case MSG_DB_PUBLISH: {
    ARTS_DEBUG("Coh PUBLISH Received");
    struct arts_msg_publish_packet_s *pack =
        (struct arts_msg_publish_packet_s *)(packet);
    /* Control-only in every phase (announce / commit / data-less) — the dirty
     * payload travels one-sided and pairs by rdzv_txid inside the handler. */
    struct arts_ooo_args_db_publish_s args = {
        .releaser = pack->header.rank,
        .db_guid = pack->db_guid,
        .version = pack->version,
        .cv = pack->cv,
        .data_size = pack->data_size,
        .rdzv_txid = pack->rdzv_txid,
        .rdzv_cookie = pack->rdzv_cookie,
        .returns_grant =
            (pack->flags & ARTS_PUBLISH_FLAG_GRANT_RETURN) ? 1u : 0u,
    };
    arts_ooo_dispatch_or_defer_guid(pack->db_guid, OOO_DB_PUBLISH, &args,
                                    sizeof(args));
    break;
  }
  case MSG_DB_PUBLISH_ACK: {
    ARTS_DEBUG("Coh PUBLISH_ACK Received");
    struct arts_msg_publish_ack_packet_s *pack =
        (struct arts_msg_publish_ack_packet_s *)(packet);
    /* Cat-C SPECIAL — call the body on BOTH HIT and MISS.  A nonzero cv is a
     * cache-independent pointer-identity sem-post (the heap rendezvous of a
     * grant return sent on its own); a torn-down cache must NOT drop that ACK
     * or the blocked returner hangs.  cv == 0 is flight completion, which needs
     * the live releaser-side cache — the body tolerates a MISS (a destroy while
     * a publish is outstanding: an ordering violation by the program, or a
     * destroy that a shutdown escape let follow the release; a waiter still
     * blocked unblocks through the shutdown path). */
    struct arts_db_publish_ack_args_s args = {.cv = pack->cv,
                                              .version = pack->version,
                                              .credit_addr = pack->credit_addr,
                                              .credit_rkey = pack->credit_rkey,
                                              .credit_txid = pack->credit_txid};
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    arts_handler_db_publish_ack(db, &args);
    arts_shared_release(&h);
    break;
  }
#ifdef ARTS_WRITE_POLICY_WT
  case MSG_DB_CREATE_RETURN: {
    ARTS_DEBUG("Coh CREATE_RETURN Received");
    struct arts_msg_db_create_return_packet_s *pack =
        (struct arts_msg_db_create_return_packet_s *)(packet);
    /* Cat-C lookup-or-drop: record the creator's first publish credit.  The
     * creator always installed its descriptor before sending the create this
     * answers, so a MISS means the DB is already destroyed — the credit dies
     * with it (a credit is a hint, never a precondition). */
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL && pack->credit_txid != 0) {
      struct arts_db_cache_s *cache = &db->cache;
      atomic_store_explicit(&cache->home_pub_addr, pack->credit_addr,
                            memory_order_relaxed);
      atomic_store_explicit(&cache->home_pub_rkey, pack->credit_rkey,
                            memory_order_relaxed);
      atomic_store_explicit(&cache->home_pub_txid, pack->credit_txid,
                            memory_order_release);
    }
    arts_shared_release(&h);
    break;
  }
#else  /* INV WB: its creates mint no credit */
  case MSG_DB_CREATE_RETURN: {
    ARTS_ERROR("this build received a create credit from rank %u — its "
               "creates mint none; binary mode mismatch?",
               packet->rank);
    break;
  }
#endif /* ARTS_WRITE_POLICY_WT */
#endif /* arm dispatch for MSG_DB_PUBLISH, _PUBLISH_ACK and _CREATE_RETURN */
  case MSG_EVENT_DESTROY: {
    ARTS_DEBUG("Event Destroy Received");
    /* Decode the GUID and route into the OoO engine.  A DESTROY that races
     * ahead of the event's CREATE (before-create wire reorder) defers on the
     * slot and replays on the create handler's drain; otherwise the pure body
     * (arts_handler_event_destroy) runs inline on the live event. */
    struct arts_msg_guid_only_packet_s *pack =
        (struct arts_msg_guid_only_packet_s *)(packet);
    struct arts_ooo_args_event_destroy_s args = {.guid = pack->guid};
    arts_ooo_dispatch_or_defer_guid(pack->guid, OOO_EVENT_DESTROY, &args,
                                    sizeof(args));
    break;
  }
  case MSG_EDT_DESTROY: {
    ARTS_DEBUG("EDT Destroy Received");
    /* Decode the GUID and route into the OoO engine (symmetric with
     * MSG_EVENT_DESTROY).  Before-create reorder defers; otherwise the pure
     * body (arts_handler_edt_destroy) runs inline on the live EDT. */
    struct arts_msg_guid_only_packet_s *pack =
        (struct arts_msg_guid_only_packet_s *)(packet);
    struct arts_ooo_args_edt_destroy_s args = {.guid = pack->guid};
    arts_ooo_dispatch_or_defer_guid(pack->guid, OOO_EDT_DESTROY, &args,
                                    sizeof(args));
    break;
  }
  /* The versioned-snapshot redirect: VAL's read path under WB, where the home
 * holds no bytes and forwards the read to the owner.  INV has its own reader
 * plane (INV_REDIRECT) and EXCL its own FORWARD, so both are excluded. */
#if defined(ARTS_PROTOCOL_FLUSH)
/* Refused in one place below: this arm has none of these rounds. */
#elif defined(ARTS_WRITE_POLICY_WB) && !defined(ARTS_PROTOCOL_EXCL) &&         \
    !defined(ARTS_PROTOCOL_INV)
  case MSG_DB_SNAPSHOT_REDIRECT: {
    ARTS_DEBUG("Coh SNAPSHOT_REDIRECT Received");
    struct arts_msg_snapshot_redirect_packet_s *pack =
        (struct arts_msg_snapshot_redirect_packet_s *)(packet);
    /* Cat-C lookup-or-dismiss: HIT serves SNAPSHOT_RESPONSE from the
     * ref-pinned owner-side db_s; MISS is a block torn down after the home
     * took the read in (the home names only an installed owner), answered
     * with a dismiss that returns the reader's landing. */
    struct arts_db_snapshot_redirect_args_s args = {
        .db_guid = pack->db_guid,
        .requester_rank = pack->requester_rank,
        .cover = pack->cover,
        .rdzv = {.addr = pack->rdzv.addr,
                 .key = pack->rdzv.key,
                 .txid = pack->rdzv.txid,
                 .cookie = pack->rdzv.cookie},
    };
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_snapshot_redirect(db, &args);
    } else {
      arts_send_db_snapshot_response(pack->requester_rank, NULL,
                                     pack->db_guid, /*version=*/0, pack->cover,
                                     /*kind=*/2, /*db_size=*/0, &args.rdzv,
                                     NULL);
    }
    arts_shared_release(&h);
    break;
  }
#else
  case MSG_DB_SNAPSHOT_REDIRECT: {
    ARTS_ERROR("this build received a snapshot redirect from rank %u — it has "
               "no versioned-snapshot read path; binary mode mismatch?",
               packet->rank);
    break;
  }
#endif /* VAL + WB snapshot redirect */

/* CONFIRM_ACK is the WB write policy's confirm gate: a new owner may not run
 * until the home has published the directory flip.  Every grant-bearing arm
 * needs it under WB — the gate belongs to the write policy, not the protocol. */
#if defined(ARTS_PROTOCOL_FLUSH)
/* Refused in one place below: this arm has none of these rounds. */
#elif defined(ARTS_WRITE_POLICY_WB) && !defined(ARTS_PROTOCOL_EXCL)
  case MSG_DB_GRANT_CONFIRM_ACK: {
    ARTS_DEBUG("Coh GRANT_CONFIRM_ACK Received");
    struct arts_msg_grant_confirm_ack_packet_s *pack =
        (struct arts_msg_grant_confirm_ack_packet_s *)(packet);
    /* Cat-C lookup-acquire-or-drop: HIT runs the confirm_ack body on the
     * ref-pinned db_s.  MISS (DB destroyed) silently drops: the gated waiters
     * are acquires the destroy contract excludes, freed with the cache and
     * never resumed. */
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_grant_confirm_ack(db, pack);
    }
    arts_shared_release(&h);
    break;
  }
#else
  case MSG_DB_GRANT_CONFIRM_ACK: {
    ARTS_ERROR("this build received GRANT_CONFIRM_ACK from rank %u — it "
               "has no WB-write-policy confirm gate; binary mode mismatch?",
               packet->rank);
    break;
  }
#endif /* WB-write-policy confirm gate */
  /* GRANT_CONFIRM: both write policies of the revoking release policy (new
   * owner C → home A flips rw_holder + advances the round).  WB additionally
   * replies with CONFIRM_ACK; WT's home handler does not (the new owner
   * already drained at GRANT_RESPONSE).  EXCL has no ownership transfer and a
   * voluntarily returned grant names no holder, so both fatal. */
#if defined(ARTS_PROTOCOL_FLUSH)
/* Refused in one place below: this arm has none of these rounds. */
#elif defined(ARTS_PROTOCOL_EXCL)
  case MSG_DB_GRANT_CONFIRM: {
    ARTS_ERROR("EXCL build received GRANT_CONFIRM from rank %u — "
               "this protocol has no migrating grant; binary mode mismatch?",
               packet->rank);
    break;
  }
#elif defined(ARTS_RELEASE_PURGE)
  case MSG_DB_GRANT_CONFIRM: {
    ARTS_ERROR("voluntary-return build received GRANT_CONFIRM from rank %u — "
               "under this release policy the hand-back is the only message "
               "a holder sends the home; binary mode mismatch?",
               packet->rank);
    break;
  }
#else /* VAL, INV under the revoking release policy */
  case MSG_DB_GRANT_CONFIRM: {
    ARTS_DEBUG("Coh GRANT_CONFIRM Received");
    struct arts_msg_grant_confirm_packet_s *pack =
        (struct arts_msg_grant_confirm_packet_s *)(packet);
    /* Cat-C lookup-acquire-or-drop: HIT advances the transfer round on the
     * ref-pinned home db_s; MISS (DB destroyed) silently drops.  The new owner
     * is pending_install_owner, so the handler takes no args. */
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_grant_confirm(db, NULL);
    }
    arts_shared_release(&h);
    break;
  }
#endif /* GRANT_CONFIRM model dispatch */
  /* EXCL has its own REQUEST wire (both release policies) and policy-specific grant/
   * release messages.  In an EXCL build each shared grant, snapshot and publish
   * case above is a refusal (guarded there). */
#ifdef ARTS_PROTOCOL_EXCL
  /* MSG_DB_EXCL_REQUEST is shared: both PURGE and RETAIN home-dispatch via OoO
   * (a REQUEST can arrive before the home db_s is installed on a remote-create
   * stub-install path). */
  case MSG_DB_EXCL_REQUEST: {
    ARTS_DEBUG("Coh EXCL_REQUEST Received");
    struct arts_msg_excl_request_packet_s *pack =
        (struct arts_msg_excl_request_packet_s *)(packet);
    struct arts_ooo_args_db_excl_request_s args = {
        .requester = pack->header.rank,
        .db_guid = pack->db_guid,
        .mode = pack->mode,
        .cover = pack->cover,
        .rdzv = {.addr = pack->rdzv.addr,
                 .key = pack->rdzv.key,
                 .txid = pack->rdzv.txid,
                 .cookie = pack->rdzv.cookie},
    };
    arts_ooo_dispatch_or_defer_guid(pack->db_guid, OOO_DB_EXCL_REQUEST, &args,
                                    sizeof(args));
    break;
  }
  case MSG_DB_EXCL_CTS: {
    ARTS_DEBUG("Coh EXCL_CTS Received");
    struct arts_msg_excl_cts_packet_s *pack =
        (struct arts_msg_excl_cts_packet_s *)(packet);
    /* Cat-C lookup-acquire-or-drop: HIT learns db_size + re-issues the
     * request with a landing; MISS (DB destroyed) silently drops. */
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_excl_cts(db, pack);
    }
    arts_shared_release(&h);
    break;
  }
#ifdef ARTS_RELEASE_PURGE
  /* PURGE-only: the grant and the release that returns it; nothing
   * acknowledges a release. */
  case MSG_DB_EXCL_GRANT: {
    ARTS_DEBUG("Coh EXCL_GRANT Received");
    /* Cat-C: the grant receiver always sent its own REQUEST first, so its cache
     * exists.  Payload (data) follows the header in the contiguous buffer; the
     * handler parses it from the full packet. */
    arts_handler_db_excl_grant((void *)packet, (size_t)packet->size);
    break;
  }
  case MSG_DB_EXCL_RELEASE: {
    ARTS_DEBUG("Coh EXCL_RELEASE Received");
    struct arts_msg_excl_release_packet_s *pack =
        (struct arts_msg_excl_release_packet_s *)(packet);
    /* Control-only: a dirty RW release PUT its bytes into the grant's home
     * landing and pairs by rdzv_txid inside the handler. */
    struct arts_ooo_args_db_excl_release_s args = {
        .releaser = pack->header.rank,
        .db_guid = pack->db_guid,
        .mode = pack->mode,
        .data_size = pack->data_size,
        .cv = pack->cv,
        .version = pack->version,
        .rdzv_txid = pack->rdzv_txid,
        .rdzv_cookie = pack->rdzv_cookie,
    };
    /* An absent home slot means the create has not landed yet: a
     * creator-remote seeded RW hold releases via the announce leg, ordered
     * only after its own DB_CREATE_COHERENT, and with >= 2 progress threads
     * the two can dispatch out of per-peer order.  Defer, and the
     * install-time drain replays it.
     *
     * A release cannot outlive its DB: every release still owed is counted in
     * the home's lock_state, and a destroy only MARKS that word — the teardown
     * that detaches the slot runs at the zero edge, after the last owed
     * release has landed and decremented.  So a release message that arrives
     * after the destroy (the program released first, but nothing orders the
     * two deliveries) still finds the marked home, and a release that finds
     * the slot absent is waiting for the create, never for a teardown.
     * When the slot is present, run the release inline on the pinned db_s. */
    arts_shared_ptr_t db_h = arts_route_table_lookup_db(pack->db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(db_h);
    if (db != NULL) {
      arts_handler_db_excl_release(db, &args);
      arts_shared_release(&db_h);
    } else {
      arts_shared_release(&db_h);
      arts_ooo_dispatch_or_defer_guid(pack->db_guid, OOO_DB_EXCL_RELEASE,
                                      &args, sizeof(args));
    }
    break;
  }

#endif /* ARTS_RELEASE_PURGE */
#ifdef ARTS_RELEASE_RETAIN
  /* RETAIN-only: async migration/serve protocol.
   * All four are Cat-C (direct): their target was installed before the
   * message was sent — FORWARD/DELIVER targets a cache built at acquire;
   * CONFIRM/RORET reach home only after home sent FORWARD.  A CONFIRM or
   * RORET is fire-and-forget, so one can arrive after the block's destroy has
   * torn the home down; its handler then finds no home and drops it.
   */
  case MSG_DB_EXCL_FORWARD: {
    ARTS_DEBUG("Coh EXCL_FORWARD Received");
    arts_handler_db_excl_forward((void *)packet);
    break;
  }
  case MSG_DB_EXCL_DELIVER: {
    ARTS_DEBUG("Coh EXCL_DELIVER Received");
    arts_handler_db_excl_deliver((void *)packet, (size_t)packet->size);
    break;
  }
  case MSG_DB_EXCL_CONFIRM: {
    ARTS_DEBUG("Coh EXCL_CONFIRM Received");
    arts_handler_db_excl_confirm((void *)packet);
    break;
  }
  case MSG_DB_EXCL_RORET: {
    ARTS_DEBUG("Coh EXCL_RORET Received");
    arts_handler_db_excl_roret((void *)packet);
    break;
  }
  case MSG_DB_EXCL_RECALL: {
    ARTS_DEBUG("Coh EXCL_RECALL Received");
    arts_handler_db_excl_recall((void *)packet);
    break;
  }
#endif /* ARTS_RELEASE_RETAIN */
#endif /* ARTS_PROTOCOL_EXCL */
#ifdef ARTS_PROTOCOL_INV
  /* INV wire arm.  REQUEST is Cat-B (a message can reach home before the home
   * db_s installs on a remote-create stub-install path); the rest are Cat-C
   * with per-message MISS actions that must never strand a round, a blocked
   * releaser, or a parked requester. */
  case MSG_DB_INV_REQUEST: {
    ARTS_DEBUG("Coh INV_REQUEST Received");
    struct arts_msg_inv_request_packet_s *pack =
        (struct arts_msg_inv_request_packet_s *)(packet);
    struct arts_ooo_args_db_inv_request_s args = {
#ifdef ARTS_WRITE_POLICY_WB
        /* The subject, not the sender: a holder that cannot serve re-sends a
         * read request on the reader's behalf. */
        .requester = pack->requester,
        .bounce = (pack->flags & ARTS_INV_REQ_BOUNCE) != 0u,
#else
        .requester = pack->header.rank,
#endif
        .db_guid = pack->db_guid,
        .mode = (arts_db_access_mode_t)pack->mode,
        .cover = pack->cover,
        .rdzv = {.addr = pack->rdzv.addr,
                 .key = pack->rdzv.key,
                 .txid = pack->rdzv.txid,
                 .cookie = pack->rdzv.cookie},
    };
    if (args.bounce) {
      /* A holder's re-send of a read its home had already taken in: the home
       * was installed, so an empty slot is a block torn down since.  Never an
       * early arrival: answer it with no bytes, which gives the reader its
       * landing back, rather than park it. */
      arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
      struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
      if (db != NULL) {
        arts_handler_db_inv_request(db, &args);
      } else {
        arts_send_db_inv_cts(args.requester, NULL, args.db_guid, 0u,
                             args.cover, args.rdzv.cookie);
      }
      arts_shared_release(&h);
      break;
    }
    arts_ooo_dispatch_or_defer_guid(pack->db_guid, OOO_DB_INV_REQUEST, &args,
                                    sizeof(args));
    break;
  }
  case MSG_DB_INV_CTS: {
    ARTS_DEBUG("Coh INV_CTS Received");
    struct arts_msg_inv_cts_packet_s *pack =
        (struct arts_msg_inv_cts_packet_s *)(packet);
    /* Cat-C lookup-acquire-or-drop: HIT learns db_size, returns any landing
     * and re-sends or closes the fetch; MISS (DB destroyed) only returns the
     * landing. */
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_inv_cts(db, pack);
    } else {
      arts_db_rdzv_discard_landing(0u, 0u, pack->rdzv_cookie);
    }
    arts_shared_release(&h);
    break;
  }
  case MSG_DB_INV_DELIVER: {
    ARTS_DEBUG("Coh INV_DELIVER Received");
    /* Cat-C: the receiver sent its own REQUEST first, so its cache exists;
     * a destroyed cache discards the landing inside the handler. */
    arts_handler_db_inv_deliver((void *)packet, (size_t)packet->size);
    break;
  }
  case MSG_DB_INV_INVALIDATE: {
    ARTS_DEBUG("Coh INV_INVALIDATE Received");
    struct arts_msg_inv_invalidate_packet_s *pack =
        (struct arts_msg_inv_invalidate_packet_s *)(packet);
    /* Cat-C; MISS (cache already destroyed) MUST still ack — the round's
     * close is gated on every roster member answering. */
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_inv_invalidate(db);
      arts_shared_release(&h);
    } else {
      arts_shared_release(&h);
      arts_send_db_inv_invalidate_ack(pack->header.rank, pack->db_guid);
    }
    break;
  }
  case MSG_DB_INV_INVALIDATE_ACK: {
    ARTS_DEBUG("Coh INV_INVALIDATE_ACK Received");
    struct arts_msg_inv_invalidate_ack_packet_s *pack =
        (struct arts_msg_inv_invalidate_ack_packet_s *)(packet);
    /* Cat-C at home; a MISS means the DB was destroyed with a round open —
     * outside the destroy contract — drop. */
    arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
    struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
    if (db != NULL) {
      arts_handler_db_inv_invalidate_ack(db, pack->header.rank);
    }
    arts_shared_release(&h);
    break;
  }
#ifdef ARTS_WRITE_POLICY_WB
  /* The home holds no bytes, so a read request it cannot answer is forwarded
   * to the rank the directory names. */
  case MSG_DB_INV_REDIRECT: {
    ARTS_DEBUG("Coh INV_REDIRECT Received");
    struct arts_msg_inv_redirect_packet_s *pack =
        (struct arts_msg_inv_redirect_packet_s *)(packet);
    struct arts_ooo_args_db_inv_redirect_s args = {
        .requester = pack->requester,
        .cover = pack->cover,
        .db_guid = pack->db_guid,
        .rdzv = {.addr = pack->rdzv.addr,
                 .key = pack->rdzv.key,
                 .txid = pack->rdzv.txid,
                 .cookie = pack->rdzv.cookie},
    };
    /* The home names a holder only after that holder's install (its CONFIRM,
     * or the creator's own announce), so an empty slot here is a cache
     * destroyed since.  Never an early arrival: answer the reader with no
     * bytes, which gives its landing back, rather than park it. */
    {
      arts_shared_ptr_t h = arts_route_table_lookup_db(pack->db_guid);
      struct arts_db_s *db = (struct arts_db_s *)arts_shared_get(h);
      if (db != NULL) {
        arts_handler_db_inv_redirect(db, &args);
      } else {
        arts_send_db_inv_cts(args.requester, NULL, args.db_guid, 0u,
                             args.cover, args.rdzv.cookie);
      }
      arts_shared_release(&h);
    }
    break;
  }
#else
  case MSG_DB_INV_REDIRECT: {
    ARTS_ERROR("write-through build received a redirect from rank %u — the "
               "home serves reads itself under this policy; binary mode "
               "mismatch",
               ((struct arts_msg_header_s *)packet)->rank);
    break;
  }
#endif /* ARTS_WRITE_POLICY_WB */
#endif /* ARTS_PROTOCOL_INV */
  /* Every coherence round this arm does not run.  The FLUSH arm fetches a
   * private copy at each remote acquire and writes it back at each remote RW
   * release: it has no grant to migrate, no snapshot to validate, no
   * invalidation round and no exclusion queue, so any of those messages on
   * the wire means the ranks were not built alike. */
#ifdef ARTS_PROTOCOL_FLUSH
  case MSG_DB_GRANT_REQUEST:
  case MSG_DB_GRANT_RESPONSE:
  case MSG_DB_GRANT_CTS:
  case MSG_DB_GRANT_INVALIDATE:
  case MSG_DB_GRANT_CONFIRM:
  case MSG_DB_GRANT_CONFIRM_ACK:
  case MSG_DB_GRANT_RETURN:
  case MSG_DB_PUBLISH:
  case MSG_DB_PUBLISH_ACK:
  case MSG_DB_PUBLISH_CTS:
  case MSG_DB_SNAPSHOT_REQUEST:
  case MSG_DB_SNAPSHOT_RESPONSE:
  case MSG_DB_SNAPSHOT_REDIRECT:
  case MSG_DB_INV_REQUEST:
  case MSG_DB_INV_CTS:
  case MSG_DB_INV_DELIVER:
  case MSG_DB_INV_INVALIDATE:
  case MSG_DB_INV_INVALIDATE_ACK:
  case MSG_DB_INV_REDIRECT:
  case MSG_DB_EXCL_REQUEST:
  case MSG_DB_EXCL_CTS:
  case MSG_DB_EXCL_GRANT:
  case MSG_DB_EXCL_RELEASE:
  case MSG_DB_EXCL_FORWARD:
  case MSG_DB_EXCL_DELIVER:
  case MSG_DB_EXCL_CONFIRM:
  case MSG_DB_EXCL_RORET:
  case MSG_DB_EXCL_RECALL: {
    ARTS_ERROR("FLUSH build received an OCR-model coherence message (type %d) "
               "from rank %u — binary mode mismatch?",
               packet->message_type, packet->rank);
    break;
  }
#endif /* ARTS_PROTOCOL_FLUSH */
  default: {
    ARTS_ERROR("received a message of unknown type %u (%lu bytes) from rank "
               "%u — binary mode mismatch or a corrupt packet?",
               packet->message_type, (unsigned long)packet->size, packet->rank);
    break;
  }
  }
}
