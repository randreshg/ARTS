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
#include "arts/remote/handler.h"

#include <string.h>

#include "arts.h"
#include "arts/compute/edt.h"
#include "arts/gas/out_of_order.h"
#include "arts/gas/route_table.h"
#include "arts/memory/db.h"
#include "arts/memory/frontier.h"
#include "arts/runtime_state.h"
#include "arts/sync/event.h"
#include "arts/sync/termination.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/transport/protocol.h"
#include "arts/utils/atomics.h"
#include "arts/utils/malloc.h"

static void arts_clear_exclusive_request(struct arts_db_s *db, int rank,
                                         arts_guid_t edt_guid) {
  if (!db || !db->db_list) {
    return;
  }

  struct arts_db_list_s *db_list = (struct arts_db_list_s *)db->db_list;
  arts_writer_lock(&db_list->reader, &db_list->writer);
  for (struct arts_db_frontier_s *frontier = db_list->head; frontier;
       frontier = frontier->next) {
    if (frontier->exNode == (unsigned int)rank &&
        frontier->exEdtGuid == edt_guid) {
      frontier->exEdtGuid = NULL_GUID;
      frontier->exEdt = NULL;
      frontier->exSlot = 0;
      frontier->exMode = DB_MODE_NULL;
      break;
    }
  }
  arts_writer_unlock(&db_list->writer);
}

static void send_remote_add_dependence_packet(unsigned int message_type,
                                              arts_guid_t source,
                                              arts_guid_t destination,
                                              uint32_t slot, unsigned int rank,
                                              arts_db_access_mode_t mode,
                                              uint32_t flags, bool ordered,
                                              uint64_t order) {
  struct arts_remote_add_dependence_packet_s packet;
  packet.source = source;
  packet.destination = destination;
  packet.slot = slot;
  packet.mode = mode;
  packet.flags = flags;
  packet.order = order;
  packet.ordered = ordered ? 1U : 0U;
  packet.reserved = 0U;
  arts_fill_packet_header(&packet.header, sizeof(packet), message_type);
  arts_remote_send_request_async((int)rank, (char *)&packet, sizeof(packet));
}

static uint64_t arts_next_ordered_add_dependence(unsigned int rank) {
  static volatile uint64_t *orders = NULL;
  static volatile unsigned int init_lock = 0;
  if (!orders) {
    arts_lock(&init_lock);
    if (!orders) {
      orders = (volatile uint64_t *)arts_calloc(arts_global_rank_count,
                                                sizeof(uint64_t));
    }
    arts_unlock(&init_lock);
  }
  if (!orders || rank >= arts_global_rank_count) {
    return 0;
  }
  return arts_atomic_fetch_add_u64(&orders[rank], 1U);
}

void arts_remote_add_dependence(arts_guid_t source, arts_guid_t destination,
                                uint32_t slot, unsigned int rank,
                                arts_db_access_mode_t mode, uint32_t flags) {
  ARTS_DEBUG("Remote Add dependence sent %d", rank);
  send_remote_add_dependence_packet(ARTS_REMOTE_ADD_DEPENDENCE_MSG, source,
                                    destination, slot, rank, mode, flags, false,
                                    0);
}

void arts_remote_add_dependence_ordered(arts_guid_t source,
                                        arts_guid_t destination, uint32_t slot,
                                        unsigned int rank,
                                        arts_db_access_mode_t mode,
                                        uint32_t flags) {
  uint64_t order = arts_next_ordered_add_dependence(rank);
  ARTS_DEBUG("Ordered remote Add dependence sent %d order=%lu", rank, order);
  send_remote_add_dependence_packet(ARTS_REMOTE_ADD_DEPENDENCE_MSG, source,
                                    destination, slot, rank, mode, flags, true,
                                    order);
}

void arts_remote_add_dependence_with_hints(arts_guid_t source,
                                           arts_guid_t destination,
                                           uint32_t slot, unsigned int rank,
                                           arts_db_access_mode_t mode,
                                           uint32_t flags) {
  ARTS_DEBUG("Remote Add dependence (mode=%u) sent %d", mode, rank);
  send_remote_add_dependence_packet(ARTS_REMOTE_ADD_DEPENDENCE_MSG, source,
                                    destination, slot, rank, mode, flags, false,
                                    0);
}

void arts_remote_set_dep_mode(arts_guid_t edt_guid, uint32_t slot,
                              arts_db_access_mode_t mode, uint32_t flags) {
  arts_remote_set_dep_metadata_ext(edt_guid, slot, mode, flags, 0, 0);
}

void arts_remote_set_dep_metadata_ext(arts_guid_t edt_guid, uint32_t slot,
                                      arts_db_access_mode_t mode,
                                      uint32_t flags, uint64_t slice_offset,
                                      uint64_t slice_size) {
  unsigned int rank = arts_guid_get_rank(edt_guid);
  struct arts_remote_set_dep_mode_packet_s packet;
  packet.edt = edt_guid;
  packet.slot = slot;
  packet.mode = mode;
  packet.flags = flags;
  packet.slice_offset = slice_offset;
  packet.slice_size = slice_size;
  arts_fill_packet_header(&packet.header, sizeof(packet),
                          ARTS_REMOTE_SET_DEP_MODE_MSG);
  arts_remote_send_request_async((int)rank, (char *)&packet, sizeof(packet));
}

void arts_remote_update_route_table(arts_guid_t guid, unsigned int rank) {
  unsigned int owner = arts_guid_get_rank(guid);
  if (owner == arts_global_rank_id) {
    struct arts_db_frontier_iterator_s iter;
    if (arts_route_table_get_rank_duplicates(guid, rank, &iter)) {
      unsigned int node;
      while (arts_db_frontier_iter_next(&iter, &node)) {
        if (node != arts_global_rank_id && node != rank) {
          struct arts_remote_guid_only_packet_s out_packet;
          out_packet.guid = guid;
          arts_fill_packet_header(&out_packet.header, sizeof(out_packet),
                                  ARTS_REMOTE_INVALIDATE_DB_MSG);
          arts_remote_send_request_async((int)node, (char *)&out_packet,
                                         sizeof(out_packet));
        }
      }
    }
  } else {
    struct arts_remote_guid_only_packet_s packet;
    arts_fill_packet_header(&packet.header, sizeof(packet),
                            ARTS_REMOTE_DB_UPDATE_GUID_MSG);
    packet.guid = guid;
    arts_remote_send_request_async((int)owner, (char *)&packet, sizeof(packet));
  }
}

static void arts_remote_send_db_snapshot(int rank, char *packet,
                                         unsigned int packet_size,
                                         struct arts_db_s *db) {
  uint64_t db_size = db->header.size;
  struct arts_db_s *snapshot =
      (struct arts_db_s *)arts_malloc_align(db_size, 16);
  memcpy(snapshot, db, db_size);
  arts_remote_send_request_payload_async_free(
      rank, packet, packet_size, (char *)snapshot, 0, db_size, arts_free);
}

void arts_remote_handle_update_db_guid(void *ptr) {
  struct arts_remote_guid_only_packet_s *packet =
      (struct arts_remote_guid_only_packet_s *)ptr;
  ARTS_DEBUG("Updated %ld to %d", packet->guid, packet->header.rank);
  arts_remote_update_route_table(packet->guid, packet->header.rank);
}

void arts_remote_handle_invalidate_db(void *ptr) {
  struct arts_remote_guid_only_packet_s *packet =
      (struct arts_remote_guid_only_packet_s *)ptr;
  arts_route_table_invalidate_item(packet->guid);
}

void arts_remote_db_destroy(arts_guid_t guid, unsigned int origin_rank) {
  unsigned int owner = arts_guid_get_rank(guid);
  if (owner == arts_global_rank_id) {
    // Owner: iterate frontier, send DESTROY to all copy holders
    struct arts_db_frontier_iterator_s iter;
    if (arts_route_table_get_rank_duplicates(guid, (unsigned int)-1, &iter)) {
      unsigned int node;
      while (arts_db_frontier_iter_next(&iter, &node)) {
        if (node != arts_global_rank_id && node != origin_rank) {
          struct arts_remote_guid_only_packet_s out_packet;
          out_packet.guid = guid;
          arts_fill_packet_header(&out_packet.header, sizeof(out_packet),
                                  ARTS_REMOTE_DB_DESTROY_MSG);
          arts_remote_send_request_async((int)node, (char *)&out_packet,
                                         sizeof(out_packet));
        }
      }
    }
  } else {
    // Non-owner: forward destroy request to owner
    struct arts_remote_guid_only_packet_s packet;
    arts_fill_packet_header(&packet.header, sizeof(packet),
                            ARTS_REMOTE_DB_DESTROY_FORWARD_MSG);
    packet.guid = guid;
    arts_remote_send_request_async((int)owner, (char *)&packet, sizeof(packet));
  }
}

void arts_remote_handle_db_destroy_forward(void *ptr) {
  struct arts_remote_guid_only_packet_s *packet =
      (struct arts_remote_guid_only_packet_s *)ptr;
  arts_remote_db_destroy(packet->guid, packet->header.rank);
  arts_db_destroy_safe(packet->guid, false);
}

void arts_remote_handle_db_destroy(void *ptr) {
  struct arts_remote_guid_only_packet_s *packet =
      (struct arts_remote_guid_only_packet_s *)ptr;
  arts_db_destroy_safe(packet->guid, false);
}

void arts_remote_update_db(arts_guid_t guid, arts_guid_t edt_guid,
                           bool send_db) {
  unsigned int rank = arts_guid_get_rank(guid);
  if (rank != arts_global_rank_id) {
    struct arts_remote_db_update_packet_s packet;
    packet.guid = guid;
    packet.edt_guid = edt_guid;
    struct arts_db_s *db = NULL;
    if (send_db && (db = (struct arts_db_s *)arts_route_table_lookup_db(
                        guid, NULL, false))) {
      uint64_t size =
          sizeof(struct arts_remote_db_update_packet_s) + db->header.size;
      arts_fill_packet_header(&packet.header, size, ARTS_REMOTE_DB_UPDATE_MSG);
      arts_remote_send_db_snapshot((int)rank, (char *)&packet, sizeof(packet),
                                   db);
      arts_route_table_return_db(guid, false);
    } else {
      if (send_db) {
        ARTS_INFO("RemoteUpdateDb missing local DB for Guid:%lu on rank %u",
                  guid, arts_global_rank_id);
      }
      arts_fill_packet_header(&packet.header,
                              sizeof(struct arts_remote_db_update_packet_s),
                              ARTS_REMOTE_DB_UPDATE_MSG);
      arts_remote_send_request_async((int)rank, (char *)&packet,
                                     sizeof(packet));
    }
  }
}

void arts_remote_handle_update_db(void *ptr) {
  struct arts_remote_db_update_packet_s *packet =
      (struct arts_remote_db_update_packet_s *)ptr;
  void *packet_payload = (char *)(packet + 1) + sizeof(struct arts_db_s);
  unsigned int rank = arts_guid_get_rank(packet->guid);
  if (rank == arts_global_rank_id) {
    struct arts_db_s **data_ptr;
    bool write =
        packet->header.size > sizeof(struct arts_remote_db_update_packet_s);
    item_state_t state = arts_route_table_lookup_item_with_state(
        packet->guid, (void ***)&data_ptr, ALLOCATED_KEY, write);
    struct arts_db_s *db = (data_ptr) ? *data_ptr : NULL;
    if (db) {
      if (write) {
        uint64_t data_size = db->header.size - sizeof(struct arts_db_s);
        uint64_t expected_size =
            sizeof(struct arts_remote_db_update_packet_s) + db->header.size;
        if (packet->header.size != expected_size) {
          ARTS_ERROR("Remote DB update packet size mismatch DB[Guid:%lu] "
                     "expected=%lu received=%lu",
                     packet->guid, expected_size, packet->header.size);
        }
        uint64_t trace_value = 0;
        if (data_size >= sizeof(trace_value)) {
          memcpy(&trace_value, packet_payload, sizeof(trace_value));
        }
        ARTS_TRACE_RDMA("remote db_update apply rank=%u from=%u db=%lu "
                        "bytes=%lu value=%lu",
                        arts_global_rank_id, packet->header.rank, packet->guid,
                        data_size, trace_value);
        if (db->db_list && db->db_list != (void *)1 &&
            packet->edt_guid != NULL_GUID) {
          if (!arts_apply_remote_writer_update(db, packet->header.rank,
                                               packet->edt_guid, packet_payload,
                                               data_size)) {
            ARTS_ERROR("Remote DB update for DB[Guid:%lu] writer EDT[Guid:%lu] "
                       "from rank %u has no matching CDAG frontier",
                       packet->guid, packet->edt_guid, packet->header.rank);
          }
        } else {
          void *dest = (void *)(db + 1);
          memcpy(dest, packet_payload, data_size);
          arts_route_table_set_rank(packet->guid, (int)arts_global_rank_id);
          arts_progress_frontier(db, arts_global_rank_id);
        }
      } else {
        arts_progress_frontier(db, packet->header.rank);
      }
    }
  }
}

void arts_remote_memory_move(unsigned int route, arts_guid_t guid, void *ptr,
                             unsigned int mem_size, unsigned message_type,
                             void (*free_method)(void *)) {
  TIME_REMOTE_MOVE_START();
  struct arts_remote_guid_only_packet_s packet;
  arts_fill_packet_header(&packet.header, sizeof(packet) + mem_size,
                          message_type);
  packet.guid = guid;
  arts_remote_send_request_payload_async_free((int)route, (char *)&packet,
                                              sizeof(packet), (char *)ptr, 0,
                                              mem_size, free_method);
  arts_route_table_remove_item(guid);
  TIME_REMOTE_MOVE_STOP();
}

void arts_remote_memory_move_no_free(unsigned int route, arts_guid_t guid,
                                     void *ptr, unsigned int mem_size,
                                     unsigned message_type) {
  struct arts_remote_guid_only_packet_s packet;
  arts_fill_packet_header(&packet.header, sizeof(packet) + mem_size,
                          message_type);
  packet.guid = guid;
  arts_remote_send_request_payload_async((int)route, (char *)&packet,
                                         sizeof(packet), (char *)ptr, mem_size);
}

void arts_remote_handle_edt_move(void *ptr) {
  struct arts_remote_guid_only_packet_s *packet =
      (struct arts_remote_guid_only_packet_s *)ptr;
  uint64_t size =
      packet->header.size - sizeof(struct arts_remote_guid_only_packet_s);
  struct arts_edt_s *edt = (struct arts_edt_s *)arts_malloc_align(size, 16);

  memcpy(edt, packet + 1, size);
  arts_route_table_add_item_race(edt, packet->guid, arts_global_rank_id, false);
  ARTS_INFO("EDT[Guid:%lu] Moved to Rank: %d", packet->guid,
            arts_global_rank_id);
  ARTS_TRACE_RDMA("remote edt_move recv rank=%u edt=%lu depc=%u needed=%u "
                  "epoch=%lu",
                  arts_global_rank_id, packet->guid, edt->depc,
                  edt->depc_needed, edt->epoch_guid);
  if (edt->depc_needed == 0) {
    arts_handle_ready_edt(edt);
  } else {
    arts_route_table_fire_oo(packet->guid, arts_out_of_order_handler);
  }
}

void arts_remote_handle_db_move(void *ptr) {
  struct arts_remote_guid_only_packet_s *packet =
      (struct arts_remote_guid_only_packet_s *)ptr;
  uint64_t size =
      packet->header.size - sizeof(struct arts_remote_guid_only_packet_s);

  struct arts_db_s db_header_buf;
  memcpy(&db_header_buf, (packet + 1), sizeof(struct arts_db_s));
  uint64_t db_size = db_header_buf.header.size;

  struct arts_header_s *mem_packet =
      (struct arts_header_s *)arts_malloc_align(db_size, 16);

  if (size == db_size) {
    memcpy(mem_packet, packet + 1, size);
  } else {
    mem_packet->type = (unsigned int)arts_guid_get_type(packet->guid);
    mem_packet->size = db_size;
  }
  // We need a local pointer for this node
  if (db_header_buf.db_list) {
    struct arts_db_s *new_db = (struct arts_db_s *)mem_packet;
    new_db->db_list = arts_new_db_list();
  }

  ARTS_INFO("DB[Guid:%lu] Moved to Rank: %d", packet->guid,
            arts_global_rank_id);
  if (arts_route_table_add_item_race(mem_packet, packet->guid,
                                     arts_global_rank_id, false)) {
    arts_route_table_fire_oo(packet->guid, arts_out_of_order_handler);
  }
}

void arts_remote_handle_event_move(void *ptr) {
  struct arts_remote_guid_only_packet_s *packet =
      (struct arts_remote_guid_only_packet_s *)ptr;
  uint64_t size =
      packet->header.size - sizeof(struct arts_remote_guid_only_packet_s);

  struct arts_header_s *mem_packet =
      (struct arts_header_s *)arts_malloc_align(size, 16);

  memcpy(mem_packet, packet + 1, size);
  arts_route_table_add_item_race(mem_packet, packet->guid, arts_global_rank_id,
                                 false);
  arts_route_table_fire_oo(packet->guid, arts_out_of_order_handler);
}

static void send_remote_edt_signal_packet(arts_guid_t edt, arts_guid_t db,
                                          uint32_t slot,
                                          arts_db_access_mode_t mode,
                                          uint32_t flags) {
  struct arts_remote_edt_signal_packet_s packet;
  unsigned int rank = arts_guid_get_rank(edt);

  if (rank == arts_global_rank_id) {
    rank = arts_route_table_lookup_rank(edt);
  }

  packet.db = db;
  packet.edt = edt;
  packet.slot = slot;
  packet.mode = mode;
  packet.flags = flags;
  packet.db_route = arts_guid_get_rank(db);
  arts_fill_packet_header(&packet.header, sizeof(packet),
                          ARTS_REMOTE_EDT_SIGNAL_MSG);
  arts_remote_send_request_async((int)rank, (char *)&packet, sizeof(packet));
}

void arts_remote_signal_edt(arts_guid_t edt, arts_guid_t db, uint32_t slot,
                            arts_db_access_mode_t mode, uint32_t flags) {
  ARTS_INFO("Remote Signal from DB[Guid:%lu] to EDT[Guid:%lu, Slot:%d, Rank: "
            "%d]",
            db, edt, slot, arts_guid_get_rank(edt));
  send_remote_edt_signal_packet(edt, db, slot, mode, flags);
}

void arts_remote_event_satisfy_slot(arts_guid_t event_guid,
                                    arts_guid_t data_guid, uint32_t slot) {
  struct arts_remote_event_satisfy_slot_packet_s packet;
  packet.event = event_guid;
  packet.db = data_guid;
  packet.slot = slot;
  arts_fill_packet_header(&packet.header, sizeof(packet),
                          ARTS_REMOTE_EVENT_SATISFY_SLOT_MSG);
  arts_remote_send_request_async((int)arts_guid_get_rank(event_guid),
                                 (char *)&packet, sizeof(packet));
}

void arts_db_request_callback(struct arts_edt_s *edt, unsigned int slot,
                              struct arts_db_s *db_res) {
  arts_edt_dep_t *depv = (arts_edt_dep_t *)arts_get_depv(edt);
  ARTS_TRACE_RDMA("db_request_callback rank=%u edt=%lu slot=%u db=%lu ptr=%p "
                  "needed_before=%u",
                  arts_global_rank_id, edt->current_edt, slot,
                  db_res ? db_res->guid : NULL_GUID,
                  db_res ? (void *)(db_res + 1) : NULL, edt->depc_needed);
  if (db_res) {
    /* Acquire a route table ref for this dep slot — matched by
     * return_db in release_dbs after EDT execution. */
    arts_route_table_lookup_db(db_res->guid, NULL, false);
    depv[slot].ptr = db_res + 1;
  } else {
    /* DB was destroyed between the OO check and the lookup (DELETE_ITEM
     * race).  Treat this slot as a NULL dependency — the data is gone. */
    ARTS_WARN("arts_db_request_callback: db_res is NULL for EDT[Guid:%lu] "
              "slot=%u (DB destroyed during OO resolution)",
              edt->current_edt, slot);
    depv[slot].guid = NULL_GUID;
    depv[slot].ptr = NULL;
  }
  unsigned int temp = arts_atomic_sub(&edt->depc_needed, 1U);
  ARTS_TRACE_RDMA("db_request_callback rank=%u edt=%lu slot=%u "
                  "needed_after=%u",
                  arts_global_rank_id, edt->current_edt, slot, temp);
  if (temp == 0) {
    arts_handle_remote_stolen_edt(edt);
  }
}

bool arts_remote_db_request(arts_guid_t data_guid, int rank,
                            struct arts_edt_s *edt, int pos,
                            arts_db_access_mode_t mode, uint32_t flags,
                            bool agg_request) {
  bool send_request =
      arts_route_table_add_sent(data_guid, edt, pos, agg_request);
  ARTS_TRACE_RDMA("remote db_request rank=%u to=%d edt=%lu slot=%d db=%lu "
                  "mode=%u flags=%u aggregate=%u send=%u",
                  arts_global_rank_id, rank, edt ? edt->current_edt : NULL_GUID,
                  pos, data_guid, mode, flags, agg_request ? 1U : 0U,
                  send_request ? 1U : 0U);
  if (send_request) {
    struct arts_remote_db_request_packet_s packet;
    packet.db_guid = data_guid;
    packet.mode = mode;
    packet.flags = flags;
    arts_fill_packet_header(&packet.header, sizeof(packet),
                            ARTS_REMOTE_DB_REQUEST_MSG);
    ARTS_DEBUG("Rank %u requesting DB[Guid:%lu] from rank %d (slot=%d)",
               arts_global_rank_id, data_guid, rank, pos);
    arts_remote_send_request_async(rank, (char *)&packet, sizeof(packet));
    return true;
  }
  return false;
}

void arts_remote_db_forward(int dest_rank, int source_rank,
                            arts_guid_t data_guid, arts_db_access_mode_t mode,
                            uint32_t flags) {
  struct arts_remote_db_request_packet_s packet;
  packet.header.size = sizeof(packet);
  packet.header.message_type = ARTS_REMOTE_DB_REQUEST_MSG;
  packet.header.rank = dest_rank;
  packet.db_guid = data_guid;
  packet.mode = mode;
  packet.flags = flags;
  arts_remote_send_request_async(source_rank, (char *)&packet, sizeof(packet));
}

void arts_remote_db_send_now(int rank, struct arts_db_s *db) {
  struct arts_remote_db_send_packet_s packet;
  uint64_t size = sizeof(struct arts_remote_db_send_packet_s) + db->header.size;
  arts_fill_packet_header(&packet.header, size, ARTS_REMOTE_DB_SEND_MSG);
  ARTS_TRACE_RDMA("remote db_send_now rank=%u to=%d db=%lu bytes=%lu",
                  arts_global_rank_id, rank, db->guid, db->header.size);
  arts_remote_send_db_snapshot(rank, (char *)&packet, sizeof(packet), db);
}

void arts_remote_db_send_check(int rank, struct arts_db_s *db,
                               arts_db_access_mode_t mode, uint32_t flags) {
  if (!arts_guid_is_local(db->guid)) {
    arts_route_table_return_db(db->guid, false);
    arts_remote_db_send_now(rank, db);
    return;
  }
  /*
   * Remote RO reader ordering. If this reader's node has a pre-registered RO
   * reader pending on some frontier generation (arts_register_remote_ro_reader
   * at arts_add_dependence time, in CDAG order), the owner does NOT ship the DB
   * here: that reader is served a TARGETED per-generation snapshot
   * (arts_remote_signal_edt_with_ptr) when its generation reaches head. This
   * delivers a private post-write copy straight to the EDT slot, bypassing the
   * route-table cache so repeated reads of the same GUID across timesteps each
   * get the correct generation's value (the seed's node-granular send_now is
   * cached on the reader and goes stale, or is served pre-write — the bug).
   */
  if ((mode == DB_MODE_RO) && db->db_list && db->db_list != (void *)1) {
    /*
     * Owner-local CDAG DB: every cross-node RO reader of it is pre-registered
     * on the frontier at arts_add_dependence time and served by a targeted
     * snapshot at promotion. The owner therefore never ships via the legacy
     * node-granular send_now path — whether the matching pre-registration is
     * still present (request arrived before serve) or already served+retired
     * (a late request arriving after the reader has been satisfied). Shipping
     * in the latter case would double-deliver into the EDT slot and leave a
     * dangling frontier generation that stalls the epoch (the observed hang).
     */
    ARTS_TRACE_RDMA("remote db_send_check ro-prereg-defer rank=%u to=%d db=%lu "
                    "mode=%u flags=%u prereg=%u",
                    arts_global_rank_id, rank, db->guid, mode, flags,
                    arts_remote_ro_reader_preregistered(db, (unsigned int)rank,
                                                        NULL_GUID)
                        ? 1U
                        : 0U);
    return;
  }
  bool on_head = false;
  bool added = arts_add_db_duplicate_ex(db, rank, NULL, NULL_GUID, 0, mode,
                                        flags, &on_head);
  if (added && on_head) {
    ARTS_TRACE_RDMA("remote db_send_check head rank=%u to=%d db=%lu mode=%u "
                    "flags=%u",
                    arts_global_rank_id, rank, db->guid, mode, flags);
    /*
     * Remote RO halo reader served on a live PURE-RO head: send the snapshot
     * now. Head lifetime is governed by the unified roReaders pre-registration
     * path (arts_serve_ro_readers) and owner-local roOutstanding, so this
     * late-serve path needs no extra pin/retire of its own.
     */
    arts_remote_db_send_now(rank, db);
  } else {
    ARTS_TRACE_RDMA("remote db_send_check defer rank=%u to=%d db=%lu mode=%u "
                    "flags=%u on_head=%u added=%u",
                    arts_global_rank_id, rank, db->guid, mode, flags,
                    on_head ? 1U : 0U, added ? 1U : 0U);
  }
}

void arts_remote_db_send(struct arts_remote_db_request_packet_s *pack) {
  unsigned int redirected = arts_route_table_lookup_rank(pack->db_guid);
  ARTS_INFO("Remote DB Send [Guid:%lu] [Rank: %d] [Mode:%d]", pack->db_guid,
            pack->header.rank, pack->mode);
  if (redirected != arts_global_rank_id && redirected != -1) {
    arts_remote_send_request_async((int)redirected, (char *)pack,
                                   pack->header.size);
  } else {
    struct arts_db_s *db = (struct arts_db_s *)arts_route_table_lookup_db(
        pack->db_guid, NULL, false);
    if (db == NULL) {
      arts_out_of_order_handle_remote_db_send((int)pack->header.rank,
                                              pack->db_guid, pack->mode,
                                              pack->flags);
    } else if (!arts_guid_is_local(db->guid) &&
               pack->header.rank == arts_global_rank_id) {
      // This is when the memory model sends a CDAG write after CDAG write to
      // the same node The arts_guid_is_local should be an extra check, maybe
      // not required
      arts_route_table_fire_oo(pack->db_guid, arts_out_of_order_handler);
      arts_route_table_return_db(pack->db_guid, false);
    } else {
      arts_remote_db_send_check((int)pack->header.rank, db, pack->mode,
                                pack->flags);
      arts_route_table_return_db(pack->db_guid, false);
    }
  }
}

void arts_remote_handle_db_received(
    struct arts_remote_db_send_packet_s *packet) {
  struct arts_db_s pdb;
  memcpy(&pdb, (packet + 1), sizeof(struct arts_db_s));
  // Actual DB bytes in the packet (may be a stub or a full DB).
  uint64_t received_bytes =
      packet->header.size - sizeof(struct arts_remote_db_send_packet_s);
  void *packet_payload = (char *)(packet + 1) + sizeof(struct arts_db_s);
  ARTS_DEBUG("Handle DB Received [Guid:%lu, Size:%lu, Received:%lu] on rank %u",
             pdb.guid, pdb.header.size, received_bytes, arts_global_rank_id);
  ARTS_TRACE_RDMA("remote db_recv rank=%u from=%u db=%lu size=%lu received=%lu",
                  arts_global_rank_id, packet->header.rank, pdb.guid,
                  pdb.header.size, received_bytes);
  struct arts_db_s *db_res = NULL;
  struct arts_db_s **data_ptr = NULL;
  item_state_t state = arts_route_table_lookup_item_with_state(
      pdb.guid, (void ***)&data_ptr, ALLOCATED_KEY, true);

  struct arts_db_s *t_ptr = (data_ptr) ? *data_ptr : NULL;
  struct arts_db_list_s *db_list = NULL;
  bool needs_frontier =
      arts_guid_is_local(pdb.guid) && pdb.db_type != ARTS_DB_LOCAL;
  if (t_ptr && needs_frontier) {
    db_list = (struct arts_db_list_s *)t_ptr->db_list;
  }
  ARTS_DEBUG("Rec DB State: %u", state);
  switch (state) {
  case REQUESTED_KEY: {
    if (t_ptr && pdb.header.size == t_ptr->header.size) {
      uint64_t payload_bytes = received_bytes - sizeof(struct arts_db_s);
      if (payload_bytes > 0) {
        void *dest = (void *)(t_ptr + 1);
        memcpy(dest, packet_payload, payload_bytes);
      }
      t_ptr->db_list = db_list;
      db_res = t_ptr;
    } else {
      ARTS_INFO("Did the DB do a remote resize...");
    }
  } break;

  case RESERVED_KEY: {
    db_res = (struct arts_db_s *)arts_malloc_align(pdb.header.size, 16);
    memcpy(db_res, (packet + 1), received_bytes);
    if (needs_frontier) {
      db_res->db_list = arts_new_db_list();
    } else {
      db_res->db_list = NULL;
    }
  } break;

  default: {
    // Fresh remote DB creation: GUID not previously in this node's route
    // table.  Allocate the full DB, copy the received stub, and register.
    db_res = (struct arts_db_s *)arts_malloc_align(pdb.header.size, 16);
    memcpy(db_res, (packet + 1), received_bytes);
    if (needs_frontier) {
      db_res->db_list = arts_new_db_list();
    } else {
      db_res->db_list = NULL;
    }
    db_res->copy_count = 1;
    if (arts_route_table_add_item_race(db_res, pdb.guid, arts_global_rank_id,
                                       false)) {
      arts_route_table_fire_oo(pdb.guid, arts_out_of_order_handler);
    }
    return;
  } break;
  }

  if (db_res && arts_route_table_update_item(pdb.guid, (void *)db_res,
                                             arts_global_rank_id, state)) {
    ARTS_TRACE_RDMA("remote db_recv fire rank=%u db=%lu state=%u",
                    arts_global_rank_id, pdb.guid, state);
    arts_route_table_fire_oo(pdb.guid, arts_out_of_order_handler);
  }
}

void arts_remote_db_full_request(arts_guid_t data_guid, int rank,
                                 arts_guid_t edt_guid, int pos,
                                 arts_db_access_mode_t mode) {
  // Do not try to reduce full requests since they are unique
  struct arts_remote_db_full_request_packet_s packet;
  packet.db_guid = data_guid;
  packet.edt_guid = edt_guid;
  packet.slot = pos;
  packet.mode = mode;
  arts_fill_packet_header(&packet.header, sizeof(packet),
                          ARTS_REMOTE_DB_FULL_REQUEST_MSG);
  ARTS_TRACE_RDMA("remote db_full_request rank=%u to=%d edt=%lu slot=%d "
                  "db=%lu mode=%u",
                  arts_global_rank_id, rank, edt_guid, pos, data_guid, mode);
  arts_remote_send_request_async(rank, (char *)&packet, sizeof(packet));
  ARTS_INFO("Full DB request sent [DbGuid:%lu, EdtGuid:%lu, Slot:%d, Mode:%u] "
            "from rank %u to rank %u",
            data_guid, edt_guid, pos, mode, arts_global_rank_id, rank);
  ARTS_DEBUG("Request Full DB[Guid:%lu] from rank %u to rank %u, mode: %u",
             data_guid, rank, packet.header.rank, mode);
}

void arts_remote_db_forward_full(int dest_rank, int source_rank,
                                 arts_guid_t data_guid, arts_guid_t edt_guid,
                                 int pos, arts_db_access_mode_t mode) {
  struct arts_remote_db_full_request_packet_s packet;
  packet.header.size = sizeof(packet);
  packet.header.message_type = ARTS_REMOTE_DB_FULL_REQUEST_MSG;
  packet.header.rank = dest_rank;
  packet.db_guid = data_guid;
  packet.edt_guid = edt_guid;
  packet.slot = pos;
  packet.mode = mode;
  arts_remote_send_request_async(source_rank, (char *)&packet, sizeof(packet));
}

void arts_remote_db_full_send_now(int rank, struct arts_db_s *db,
                                  arts_guid_t edt_guid, unsigned int slot,
                                  arts_db_access_mode_t mode) {
  struct arts_remote_db_full_send_packet_s packet;
  packet.edt_guid = edt_guid;
  packet.slot = slot;
  packet.mode = mode;
  uint64_t size =
      sizeof(struct arts_remote_db_full_send_packet_s) + db->header.size;
  arts_fill_packet_header(&packet.header, size, ARTS_REMOTE_DB_FULL_SEND_MSG);
  ARTS_TRACE_RDMA("remote db_full_send_now rank=%u to=%d edt=%lu slot=%u "
                  "db=%lu mode=%u bytes=%lu",
                  arts_global_rank_id, rank, edt_guid, slot, db->guid, mode,
                  db->header.size);
  arts_remote_send_db_snapshot(rank, (char *)&packet, sizeof(packet), db);
  ARTS_INFO("Full DB send [DbGuid:%lu, EdtGuid:%lu, Slot:%u, Mode:%u, Size:%u] "
            "from rank %u to rank %u",
            db->guid, edt_guid, slot, mode, db->header.size,
            arts_global_rank_id, rank);
}

void arts_remote_db_full_send_check(int rank, struct arts_db_s *db,
                                    arts_guid_t edt_guid, unsigned int slot,
                                    arts_db_access_mode_t mode) {
  if (!arts_guid_is_local(db->guid)) {
    arts_route_table_return_db(db->guid, false);
    arts_remote_db_full_send_now(rank, db, edt_guid, slot, mode);
  } else if ((mode == DB_MODE_EW || mode == DB_MODE_MEMSET)) {
    /*
     * Remote EW writer: it may have been pre-registered in CDAG order at
     * arts_add_dependence time (arts_register_remote_ew_writer). If so, do NOT
     * create a second frontier generation — that would both mis-order the
     * reader and leave a writer slot that never progresses (the hang). Instead
     * bind to the existing generation: ship the DB now iff it is the head and
     * has not been delivered yet; otherwise arts_progress_frontier ships it
     * when its generation reaches the head.
     */
    bool on_head = false;
    bool deliver = false;
    if (arts_claim_remote_ew_writer(db, (unsigned int)rank, edt_guid, &on_head,
                                    &deliver)) {
      if (deliver) {
        ARTS_TRACE_RDMA("remote db_full_send_check prereg-head rank=%u to=%d "
                        "edt=%lu slot=%u db=%lu mode=%u",
                        arts_global_rank_id, rank, edt_guid, slot, db->guid,
                        mode);
        arts_remote_db_full_send_now(rank, db, edt_guid, slot, mode);
      } else {
        ARTS_TRACE_RDMA("remote db_full_send_check prereg-defer rank=%u to=%d "
                        "edt=%lu slot=%u db=%lu mode=%u on_head=%u",
                        arts_global_rank_id, rank, edt_guid, slot, db->guid,
                        mode, on_head ? 1U : 0U);
      }
    } else {
      bool added_on_head = false;
      if (arts_add_db_duplicate(db, rank, NULL, edt_guid, slot, mode,
                                &added_on_head)) {
        if (added_on_head) {
          arts_remote_db_full_send_now(rank, db, edt_guid, slot, mode);
          arts_clear_exclusive_request(db, rank, edt_guid);
        }
      }
    }
  } else if (mode == DB_MODE_RO && db->db_list && db->db_list != (void *)1) {
    /*
     * Remote RO reader of an owner-managed CDAG DB. The preferred path is
     * eager pre-registration at arts_add_dependence time, which preserves
     * program order before the reader EDT runs. That only happens when the
     * dependence is issued on the DB owner. If a non-owner rank records a
     * dependence on a remote-owned DB, the owner's first chance to see the
     * reader is this full request. In that case, lazily register the reader on
     * the owner frontier and serve a targeted snapshot immediately when it is
     * already at the head; otherwise promotion will serve it later.
     */
    bool prereg = arts_remote_ro_reader_preregistered(
        db, (unsigned int)rank, edt_guid);
    ARTS_TRACE_RDMA("remote db_full_send_check ro-request rank=%u to=%d "
                    "edt=%lu slot=%u db=%lu prereg=%u",
                    arts_global_rank_id, rank, edt_guid, slot, db->guid,
                    prereg ? 1U : 0U);
    if (!prereg) {
      bool registered =
          arts_register_remote_ro_reader(db, (unsigned int)rank, edt_guid,
                                         slot);
      ARTS_TRACE_RDMA("remote db_full_send_check ro-lazy-register rank=%u "
                      "to=%d edt=%lu slot=%u db=%lu registered=%u",
                      arts_global_rank_id, rank, edt_guid, slot, db->guid,
                      registered ? 1U : 0U);
      if (!registered) {
        arts_remote_db_full_send_now(rank, db, edt_guid, slot, mode);
      }
    }
  } else {
    bool on_head = false;
    if (arts_add_db_duplicate(db, rank, NULL, edt_guid, slot, mode, &on_head)) {
      if (on_head) {
        ARTS_TRACE_RDMA("remote db_full_send_check head rank=%u to=%d "
                        "edt=%lu slot=%u db=%lu mode=%u",
                        arts_global_rank_id, rank, edt_guid, slot, db->guid,
                        mode);
        arts_remote_db_full_send_now(rank, db, edt_guid, slot, mode);
        arts_clear_exclusive_request(db, rank, edt_guid);
      }
      /* Non-head: request is stored in the frontier (exNode/exEdtGuid/
       * exSlot/exMode).  arts_progress_frontier will send the updated DB
       * copy when this frontier becomes the head. */
    }
  }
}

void arts_remote_db_full_send(
    struct arts_remote_db_full_request_packet_s *pack) {
  unsigned int redirected = arts_route_table_lookup_rank(pack->db_guid);
  if (redirected != arts_global_rank_id && redirected != -1) {
    arts_remote_send_request_async((int)redirected, (char *)pack,
                                   pack->header.size);
  } else {
    struct arts_db_s *db = (struct arts_db_s *)arts_route_table_lookup_db(
        pack->db_guid, NULL, false);
    if (db == NULL) {
      arts_out_of_order_handle_remote_db_full_send(
          pack->db_guid, (int)pack->header.rank, pack->edt_guid, pack->slot,
          pack->mode);
    } else {
      arts_remote_db_full_send_check((int)pack->header.rank, db, pack->edt_guid,
                                     pack->slot, pack->mode);
      arts_route_table_return_db(pack->db_guid, false);
    }
  }
}

void arts_remote_handle_db_full_recieved(
    struct arts_remote_db_full_send_packet_s *packet) {
  bool dec;
  item_state_t state;
  struct arts_db_s pdb;
  memcpy(&pdb, (packet + 1), sizeof(struct arts_db_s));
  void *packet_payload = (char *)(packet + 1) + sizeof(struct arts_db_s);
  ARTS_DEBUG("Handle Full DB Received [Guid:%lu, Slot:%u, Mode:%u]", pdb.guid,
             packet->slot, packet->mode);
  ARTS_TRACE_RDMA("remote db_full_recv rank=%u from=%u edt=%lu slot=%u "
                  "db=%lu mode=%u size=%lu",
                  arts_global_rank_id, packet->header.rank, packet->edt_guid,
                  packet->slot, pdb.guid, packet->mode, pdb.header.size);
  void **data_ptr = arts_route_table_reserve(pdb.guid, &dec, &state);
  struct arts_db_s *db_res = (data_ptr) ? (struct arts_db_s *)*data_ptr : NULL;
  if (db_res) {
    if (pdb.header.size == db_res->header.size) {
      struct arts_db_list_s *db_list = (struct arts_db_list_s *)db_res->db_list;
      void *dest = (void *)(db_res + 1);
      memcpy(dest, packet_payload, pdb.header.size - sizeof(struct arts_db_s));
      db_res->db_list = db_list;
    } else {
      ARTS_INFO("Did the DB do a remote resize...");
    }
  } else {
    db_res = (struct arts_db_s *)arts_malloc_align(pdb.header.size, 16);
    memcpy(db_res, (packet + 1), pdb.header.size);
    if (arts_guid_is_local(pdb.guid) && pdb.db_type != ARTS_DB_LOCAL) {
      db_res->db_list = arts_new_db_list();
    } else {
      db_res->db_list = NULL;
    }
  }
  if (arts_route_table_update_item(pdb.guid, (void *)db_res,
                                   arts_global_rank_id, state)) {
    /*
     * A full DB send for an EW/MEMSET writer is that writer's PRIVATE working
     * copy: the writer is about to mutate it in place before shipping the
     * update back to the owner. Firing the out-of-order waiters here would let
     * an unrelated pending RO reader on this same node latch the pre-write
     * value (the cross-node-reader / same-node-writer staleness). Only the
     * targeted writer EDT is satisfied below (arts_db_request_callback); RO
     * readers are served the post-write value by the owner's frontier forward
     * at promotion. RO/other sends still fire OO normally.
     */
    if (packet->mode != DB_MODE_EW && packet->mode != DB_MODE_MEMSET) {
      arts_route_table_fire_oo(pdb.guid, arts_out_of_order_handler);
    }
  }
  struct arts_edt_s *edt =
      (struct arts_edt_s *)arts_route_table_lookup_item(packet->edt_guid);
  if (!edt) {
    void **edt_data = NULL;
    item_state_t edt_state = arts_route_table_lookup_item_with_state(
        packet->edt_guid, &edt_data, ANY_KEY, false);
    ARTS_INFO("Full DB received for missing EDT[Guid:%lu] on rank %u "
              "(state=%u, data=%p) [DbGuid:%lu, Slot:%u, Mode:%u]",
              packet->edt_guid, arts_global_rank_id, edt_state,
              edt_data ? *edt_data : NULL, pdb.guid, packet->slot,
              packet->mode);
    ARTS_TRACE_RDMA("remote db_full_recv missing_edt rank=%u edt=%lu "
                    "state=%u data=%p db=%lu slot=%u mode=%u",
                    arts_global_rank_id, packet->edt_guid, edt_state,
                    edt_data ? *edt_data : NULL, pdb.guid, packet->slot,
                    packet->mode);
    return;
  }
  arts_db_request_callback(edt, packet->slot, db_res);
}

void arts_remote_send_already_local(int rank, arts_guid_t guid,
                                    arts_guid_t edt_guid, unsigned int slot,
                                    arts_db_access_mode_t mode) {
  struct arts_remote_db_full_request_packet_s packet;
  packet.db_guid = guid;
  packet.edt_guid = edt_guid;
  packet.slot = slot;
  packet.mode = mode;
  arts_fill_packet_header(&packet.header, sizeof(packet),
                          ARTS_REMOTE_DB_FULL_SEND_ALREADY_LOCAL_MSG);
  arts_remote_send_request_async(rank, (char *)&packet, sizeof(packet));
}

void arts_remote_handle_send_already_local(void *pack) {
  struct arts_remote_db_full_request_packet_s *packet =
      (struct arts_remote_db_full_request_packet_s *)pack;
  int rank;
  struct arts_db_s *db_res = (struct arts_db_s *)arts_route_table_lookup_db(
      packet->db_guid, &rank, true);
  struct arts_edt_s *edt =
      (struct arts_edt_s *)arts_route_table_lookup_item(packet->edt_guid);
  if (!edt) {
    void **edt_data = NULL;
    item_state_t edt_state = arts_route_table_lookup_item_with_state(
        packet->edt_guid, &edt_data, ANY_KEY, false);
    ARTS_INFO("Already-local DB received for missing EDT[Guid:%lu] on rank %u "
              "(state=%u, data=%p) [DbGuid:%lu, Slot:%u, Mode:%u]",
              packet->edt_guid, arts_global_rank_id, edt_state,
              edt_data ? *edt_data : NULL, packet->db_guid, packet->slot,
              packet->mode);
    return;
  }
  arts_db_request_callback(edt, packet->slot, db_res);
}

void arts_remote_get_from_db(arts_guid_t edt_guid, arts_guid_t db_guid,
                             unsigned int slot, uint64_t offset,
                             uint64_t len, uint32_t flags,
                             unsigned int rank) {
  struct arts_remote_get_put_packet_s packet;
  packet.edt_guid = edt_guid;
  packet.db_guid = db_guid;
  packet.epoch_guid = NULL_GUID;
  packet.slot = slot;
  packet.flags = flags;
  packet.offset = offset;
  packet.size = len;
  arts_fill_packet_header(&packet.header, sizeof(packet),
                          ARTS_REMOTE_GET_FROM_DB_MSG);
  arts_remote_send_request_async((int)rank, (char *)&packet, sizeof(packet));
}

void arts_remote_handle_get_from_db(void *pack) {
  struct arts_remote_get_put_packet_s *packet =
      (struct arts_remote_get_put_packet_s *)pack;
  arts_get_from_db_at_ex(packet->edt_guid, packet->db_guid, packet->slot,
                         packet->offset, packet->size, packet->flags,
                         arts_global_rank_id);
}

void arts_remote_put_in_db(void *ptr, arts_guid_t edt_guid, arts_guid_t db_guid,
                           unsigned int slot, uint64_t offset,
                           uint64_t len, arts_guid_t epoch_guid,
                           unsigned int rank) {
  struct arts_remote_get_put_packet_s packet;
  packet.edt_guid = edt_guid;
  packet.db_guid = db_guid;
  packet.epoch_guid = epoch_guid;
  packet.slot = slot;
  packet.flags = 0;
  packet.offset = offset;
  packet.size = len;
  uint64_t total_size = sizeof(struct arts_remote_get_put_packet_s) + len;
  arts_fill_packet_header(&packet.header, total_size,
                          ARTS_REMOTE_PUT_IN_DB_MSG);
  //    arts_remote_send_request_payload_async(rank, (char *)&packet,
  //    sizeof(packet), (char *)ptr, len);
  arts_remote_send_request_payload_async_free((int)rank, (char *)&packet,
                                              sizeof(packet), (char *)ptr, 0,
                                              len, arts_free);
}

void arts_remote_handle_put_in_db(void *pack) {
  struct arts_remote_get_put_packet_s *packet =
      (struct arts_remote_get_put_packet_s *)pack;
  void *data = (void *)(packet + 1);
  internal_put_in_db(data, packet->edt_guid, packet->db_guid, packet->slot,
                     packet->offset, packet->size, packet->epoch_guid,
                     arts_global_rank_id);
}

void arts_remote_signal_edt_with_ptr(arts_guid_t edt_guid, arts_guid_t db_guid,
                                     void *ptr, unsigned int size,
                                     unsigned int slot) {
  unsigned int rank = arts_guid_get_rank(edt_guid);
  if (rank == arts_global_rank_id) {
    arts_signal_edt_ptr_with_guid(edt_guid, slot, db_guid, ptr, size);
    return;
  }
  ARTS_DEBUG("SEND NOW: %u -> %u", arts_global_rank_id, rank);
  uint64_t total_size =
      sizeof(struct arts_remote_signal_edt_with_ptr_packet_s) + size;
  char *buf = (char *)arts_malloc((size_t)total_size);
  struct arts_remote_signal_edt_with_ptr_packet_s *packet =
      (struct arts_remote_signal_edt_with_ptr_packet_s *)buf;
  packet->edt_guid = edt_guid;
  packet->db_guid = db_guid;
  packet->size = size;
  packet->slot = slot;
  arts_fill_packet_header(&packet->header, total_size,
                          ARTS_REMOTE_SIGNAL_EDT_WITH_PTR_MSG);
  memcpy(buf + sizeof(*packet), ptr, size);
  arts_remote_send_request_async((int)rank, buf, (unsigned int)total_size);
  arts_free(buf);
}

void arts_remote_handle_signal_edt_with_ptr(void *pack) {
  struct arts_remote_signal_edt_with_ptr_packet_s *packet =
      (struct arts_remote_signal_edt_with_ptr_packet_s *)pack;
  void *source = (void *)(packet + 1);
  arts_signal_edt_ptr_with_guid(packet->edt_guid, packet->slot, packet->db_guid,
                                source, packet->size);
}

void arts_remote_send(unsigned int rank, send_handler_t fun_ptr, void *args,
                      unsigned int size, bool free) {
  if (rank == arts_global_rank_id) {
    fun_ptr(args);
    if (free) {
      arts_free(args);
    }
    return;
  }
  struct arts_remote_send_s packet;
  packet.fun_ptr = fun_ptr;
  int total_size = (int)(sizeof(struct arts_remote_send_s) + size);
  arts_fill_packet_header(&packet.header, total_size, ARTS_REMOTE_SEND_MSG);

  if (free) {
    arts_remote_send_request_payload_async_free((int)rank, (char *)&packet,
                                                sizeof(packet), (char *)args, 0,
                                                size, arts_free);
  } else {
    arts_remote_send_request_payload_async((int)rank, (char *)&packet,
                                           sizeof(packet), (char *)args, size);
  }
}

void arts_remote_handle_send(void *pack) {
  struct arts_remote_send_s *packet = (struct arts_remote_send_s *)pack;
  void *args = (void *)(packet + 1);
  packet->fun_ptr(args);
}

void arts_remote_epoch_init_send(unsigned int rank, arts_guid_t epoch_guid,
                                 arts_guid_t edt_guid, unsigned int slot) {
  struct arts_remote_epoch_init_packet_s packet;
  packet.epoch_guid = epoch_guid;
  packet.edt_guid = edt_guid;
  packet.slot = slot;
  arts_fill_packet_header(&packet.header, sizeof(packet), ARTS_EPOCH_INIT_MSG);
  ARTS_TRACE_RDMA("remote epoch_init enqueue to=%u guid=%lu edt=%lu slot=%u",
                  rank, epoch_guid, edt_guid, slot);
  arts_remote_send_request_async((int)rank, (char *)&packet, sizeof(packet));
}

void arts_remote_handle_epoch_init_send(void *pack) {
  ARTS_DEBUG("Net Epoch Init Rec");
  struct arts_remote_epoch_init_packet_s *packet =
      (struct arts_remote_epoch_init_packet_s *)pack;
  arts_guid_t local_epoch_guid = packet->epoch_guid;
  ARTS_TRACE_RDMA("remote epoch_init recv from=%u guid=%lu edt=%lu slot=%u",
                  packet->header.rank, packet->epoch_guid, packet->edt_guid,
                  packet->slot);
  create_epoch(&local_epoch_guid, packet->edt_guid, packet->slot);
  packet->epoch_guid = local_epoch_guid;
}

void arts_remote_epoch_init_pool_send(unsigned int rank, unsigned int pool_size,
                                      arts_guid_t start_guid,
                                      arts_guid_t pool_guid) {
  //    ARTS_INFO("Net Epoch Init Pool Send: %u %lu %lu", rank, start_guid,
  //    pool_guid);
  struct arts_remote_epoch_init_pool_packet_s packet;
  packet.pool_size = pool_size;
  packet.start_guid = start_guid;
  packet.pool_guid = pool_guid;
  arts_fill_packet_header(&packet.header, sizeof(packet),
                          ARTS_EPOCH_INIT_POOL_MSG);
  ARTS_TRACE_RDMA("remote epoch_pool enqueue to=%u pool=%lu start=%lu size=%u",
                  rank, pool_guid, start_guid, pool_size);
  arts_remote_send_request_async((int)rank, (char *)&packet, sizeof(packet));
}

void arts_remote_handle_epoch_init_pool_send(void *pack) {
  struct arts_remote_epoch_init_pool_packet_s *packet =
      (struct arts_remote_epoch_init_pool_packet_s *)pack;
  arts_guid_t local_pool_guid = packet->pool_guid;
  arts_guid_t local_start_guid = packet->start_guid;
  ARTS_TRACE_RDMA("remote epoch_pool recv from=%u pool=%lu start=%lu size=%u",
                  packet->header.rank, packet->pool_guid, packet->start_guid,
                  packet->pool_size);
  arts_epoch_pool_t *pool =
      create_epoch_pool(&local_pool_guid, packet->pool_size, &local_start_guid);
  arts_link_epoch_pool_to_tls(pool);
  packet->pool_guid = local_pool_guid;
  packet->start_guid = local_start_guid;
}

void arts_remote_epoch_req(unsigned int rank, arts_guid_t guid) {
  struct arts_remote_guid_only_packet_s packet;
  packet.guid = guid;
  arts_fill_packet_header(&packet.header, sizeof(packet), ARTS_EPOCH_REQ_MSG);
  ARTS_TRACE_RDMA("remote epoch_req enqueue to=%u guid=%lu", rank, guid);
  arts_remote_send_request_async((int)rank, (char *)&packet, sizeof(packet));
}

void arts_remote_handle_epoch_req(void *pack) {
  struct arts_remote_guid_only_packet_s *packet =
      (struct arts_remote_guid_only_packet_s *)pack;
  // For now the source and dest are the same...
  ARTS_TRACE_RDMA("remote epoch_req recv from=%u guid=%lu",
                  packet->header.rank, packet->guid);
  send_epoch(packet->guid, packet->header.rank, packet->header.rank);
}

void arts_remote_epoch_send(unsigned int rank, arts_guid_t guid,
                            unsigned int active, unsigned int finish) {
  struct arts_remote_epoch_send_packet_s packet;
  packet.epoch_guid = guid;
  packet.active = active;
  packet.finish = finish;
  arts_fill_packet_header(&packet.header, sizeof(packet), ARTS_EPOCH_SEND_MSG);
  ARTS_TRACE_RDMA("remote epoch_send enqueue to=%u guid=%lu active=%u "
                  "finish=%u",
                  rank, guid, active, finish);
  arts_remote_send_request_async((int)rank, (char *)&packet, sizeof(packet));
}

void arts_remote_handle_epoch_send(void *pack) {
  struct arts_remote_epoch_send_packet_s *packet =
      (struct arts_remote_epoch_send_packet_s *)pack;
  ARTS_TRACE_RDMA("remote epoch_send recv from=%u guid=%lu active=%u "
                  "finish=%u",
                  packet->header.rank, packet->epoch_guid, packet->active,
                  packet->finish);
  reduce_epoch(packet->epoch_guid, packet->active, packet->finish);
}

void arts_remote_epoch_delete(unsigned int rank, arts_guid_t epoch_guid) {
  struct arts_remote_guid_only_packet_s packet;
  packet.guid = epoch_guid;
  arts_fill_packet_header(&packet.header, sizeof(packet),
                          ARTS_EPOCH_DELETE_MSG);
  arts_remote_send_request_async((int)rank, (char *)&packet, sizeof(packet));
}

void arts_remote_handle_epoch_delete(void *pack) {
  struct arts_remote_guid_only_packet_s *packet =
      (struct arts_remote_guid_only_packet_s *)pack;
  delete_epoch(packet->guid, NULL);
}

void arts_remote_handle_buffer_send(void *pack) {
  struct arts_remote_guid_only_packet_s *packet =
      (struct arts_remote_guid_only_packet_s *)pack;
  uint64_t size =
      packet->header.size - sizeof(struct arts_remote_guid_only_packet_s);
  void *buffer = (void *)(packet + 1);
  arts_set_buffer(packet->guid, buffer, size);
}

void arts_remote_db_rename(arts_guid_t new_guid, arts_guid_t old_guid) {
  unsigned int dest_rank = arts_guid_get_rank(old_guid);
  struct arts_remote_db_rename_s packet;
  packet.old_guid = old_guid;
  packet.new_guid = new_guid;
  packet.header.size = sizeof(packet);
  packet.header.message_type = ARTS_REMOTE_DB_RENAME_MSG;
  packet.header.rank = dest_rank;
  arts_remote_send_request_async((int)dest_rank, (char *)&packet,
                                 sizeof(packet));
}

void arts_remote_handle_db_rename(void *pack) {
  struct arts_remote_db_rename_s *packet =
      (struct arts_remote_db_rename_s *)pack;
  arts_db_rename_with_guid(packet->new_guid, packet->old_guid);
}

// RTT-based time synchronization for counter capture alignment
// External declarations for time sync state (defined in Counter.c)
extern volatile int64_t arts_counter_time_offset;
extern volatile bool arts_counter_time_sync_received;

// Worker sends sync request to master with its current timestamp (T1)
void arts_remote_time_sync_request(void) {
  struct arts_remote_time_sync_req_packet_s packet;
  packet.worker_send_time = arts_get_time_stamp(); // T1
  arts_fill_packet_header(&packet.header, sizeof(packet),
                          ARTS_REMOTE_TIME_SYNC_REQ_MSG);

  // Send to master
  arts_remote_send_request_async((int)arts_global_master_rank_id,
                                 (char *)&packet, sizeof(packet));
  ARTS_INFO("Time sync: Worker %u sent request to master %u at T1=%lu",
            arts_global_rank_id, arts_global_master_rank_id,
            packet.worker_send_time);
}

// Master handles sync request: records T2 and sends response with T1, T2
void arts_remote_handle_time_sync_req(void *pack) {
  struct arts_remote_time_sync_req_packet_s *req =
      (struct arts_remote_time_sync_req_packet_s *)pack;
  uint64_t master_recv_time = arts_get_time_stamp(); // T2

  struct arts_remote_time_sync_resp_packet_s resp;
  resp.worker_send_time = req->worker_send_time; // Echo T1
  resp.master_recv_time = master_recv_time;      // T2
  arts_fill_packet_header(&resp.header, sizeof(resp),
                          ARTS_REMOTE_TIME_SYNC_RESP_MSG);

  // Send response back to the requesting worker
  arts_remote_send_request_async((int)req->header.rank, (char *)&resp,
                                 sizeof(resp));
  ARTS_INFO("Time sync: Master received request from rank %u, T1=%lu, T2=%lu",
            req->header.rank, req->worker_send_time, master_recv_time);
}

// Worker handles sync response: calculates offset using RTT
void arts_remote_handle_time_sync_resp(void *pack) {
  struct arts_remote_time_sync_resp_packet_s *resp =
      (struct arts_remote_time_sync_resp_packet_s *)pack;
  uint64_t worker_recv_time = arts_get_time_stamp(); // T3

  uint64_t ntp_t1 = resp->worker_send_time;
  uint64_t ntp_t2 = resp->master_recv_time;
  uint64_t ntp_t3 = worker_recv_time;

  // RTT = T3 - T1 (round-trip time in worker's clock)
  // One-way delay estimate = RTT / 2 (assuming symmetric network)
  // At T2 (master clock), worker clock was approximately T1 + RTT/2
  // offset = workerTime - masterTime = (T1 + RTT/2) - T2 = (T1 + T3)/2 - T2
  int64_t offset = (int64_t)((ntp_t1 + ntp_t3) / 2) - (int64_t)ntp_t2;

  __atomic_store_n(&arts_counter_time_offset, offset, __ATOMIC_RELAXED);
  __atomic_store_n(&arts_counter_time_sync_received, true, __ATOMIC_RELEASE);

  uint64_t rtt = ntp_t3 - ntp_t1;
  ARTS_INFO("Time sync: Worker %u received response, T1=%lu, T2=%lu, T3=%lu, "
            "RTT=%lu ns (%.3f ms), offset=%ld ns (%.3f ms)",
            arts_global_rank_id, ntp_t1, ntp_t2, ntp_t3, rtt,
            (double)rtt / 1000000.0, offset, (double)offset / 1000000.0);
}
