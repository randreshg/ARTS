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

/* Control-plane send facade.
 *
 * This translation unit holds the public send names the rest of the runtime
 * calls and the outbound-size census.  A remote target is forwarded to the
 * fabric core (arts_net_send_core); a single-node run never presents one. */

#include "arts/transport/net.h" /* public API + arts_net_send_core */

#include "arts/counter/counter.h" /* NET_MSG_* census */
#include "arts/system/identity.h" /* arts_global_rank_id / arts_global_rank_count */
#include "arts/system/print.h"

/* ===== Outbound census + guards =========================================== */

/* Outbound message size census: one switch-free arithmetic bucket on the total
 * wire size (header + payload), taken at the async send entry points.  A
 * diagnostic distribution only — no transport decision branches on size. */
static inline void arts_net_msg_census(uint64_t total_size) {
  if (total_size <= 64) {
    INCREMENT_NET_MSG_LE64_BY(1);
  } else if (total_size <= 512) {
    INCREMENT_NET_MSG_LE512_BY(1);
  } else if (total_size <= 4096) {
    INCREMENT_NET_MSG_LE4K_BY(1);
  } else if (total_size <= 65536) {
    INCREMENT_NET_MSG_LE64K_BY(1);
  } else {
    INCREMENT_NET_MSG_GT64K_BY(1);
  }
  INCREMENT_NET_MSG_TOTAL_BY(1);
}

static inline void size_send_check(uint64_t size) {
  if (size == 0) {
    ARTS_ERROR("Cannot send zero-size message");
  }
}

/* Reject a self-addressed or out-of-range target.  A same-rank message never
 * goes through a send (its sender calls the handler body directly or through
 * the OoO engine), so a self target reaching a public send wrapper is a bug,
 * warned and dropped (this also covers single-node, where every rank is
 * self). */
static inline bool net_target_ok(int rank) {
  if ((unsigned int)rank == arts_global_rank_id ||
      (unsigned int)rank >= arts_global_rank_count) {
    ARTS_WARN("Cannot send to rank %u (self=%u, total=%u)", (unsigned int)rank,
              arts_global_rank_id, arts_global_rank_count);
    return false;
  }
  return true;
}

/* ===== Public send API ==================================================== */

void arts_transport_send_async(int rank, char *message, unsigned int length) {
  if (!net_target_ok(rank)) {
    return;
  }
  arts_net_msg_census(length);
  arts_net_send_core(rank, message, length, NULL, 0, 0, NULL);
}

void arts_transport_send_payload_async_free(int rank, char *message,
                                            unsigned int length, char *payload,
                                            unsigned int offset, uint64_t size,
                                            void (*free_method)(void *)) {
  if (!net_target_ok(rank)) {
    return;
  }
  size_send_check(length);
  size_send_check(size);
  arts_net_msg_census((uint64_t)length + size);
  arts_net_send_core(rank, message, length, payload, offset, size, free_method);
}
