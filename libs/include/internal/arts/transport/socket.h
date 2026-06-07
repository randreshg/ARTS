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

#ifndef ARTS_TRANSPORT_SOCKET_H
#define ARTS_TRANSPORT_SOCKET_H
#ifdef __cplusplus
extern "C" {
#endif
#include "arts/system/config.h"
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

int arts_get_new_socket();
void arts_server_set_socket_options_sender(unsigned int socket);
void arts_server_set_socket_options_reciever(unsigned int socket);
int arts_get_socket_listening(struct sockaddr_in *listening_socket,
                              unsigned int port);
int arts_get_socket_outgoing(struct sockaddr_in *outgoing_socket,
                             unsigned int port, in_addr_t s_addr);

void arts_remote_set_message_table(struct arts_config_s *table);
void arts_remote_setup_outgoing();
bool arts_remote_setup_incoming();
void arts_remote_eager_connect_all();
void arts_remote_refresh_startup_connect_grace();
unsigned int arts_remote_get_my_rank();
bool arts_server_try_to_receive(char **in_buffer, const int *in_packet_size,
                                const volatile unsigned int *remote_steal_lock);
uint64_t arts_remote_send_request(int rank, unsigned int queue, char *message,
                                  uint64_t length);
uint64_t arts_remote_send_payload_request(int rank, unsigned int queue,
                                          char *message, unsigned int length,
                                          char *payload, uint64_t length2);
void arts_remote_set_thread_inbound_queues(unsigned int start,
                                           unsigned int stop);
void arts_remote_thread_inbound_queues_cleanup();

// Canonical name of the active data-plane transport: "tcp", "rdma-rsocket", or
// "gasnet". Each backend (socket.c / gasnet_transport.c) provides its own
// definition, so callers and logs can distinguish the three transports instead
// of collapsing every accelerated path to "rdma".
const char *arts_transport_kind_name(void);

// --- One-sided RMA DB-move seam ------------------------------------------
// GASNet implements this seam over gex_RMA_Put. TCP/rsocket stubs report
// "not RMA-capable" so callers use the Medium-AM snapshot path.

// True iff this backend can perform one-sided RMA Puts into a peer's segment.
bool arts_transport_rma_capable(void);

// Base/size of the local RMA-addressable segment region that backs DB storage.
// Returns NULL (and *size_out = 0) when the backend has no usable segment.
// The DB arena (memory/db_arena.c) carves DB storage from this region so that
// DB bodies are valid remote targets for arts_remote_rma_put.
void *arts_transport_segment_base(uint64_t *size_out);

// One-sided blocking Put of nbytes from local_src into remote_addr (an absolute
// address inside dest_rank's registered segment, as advertised by that rank).
// Returns 0 on success (bytes are remotely landed on return), non-zero on
// failure or when the backend is not RMA-capable. Local completion only: the
// caller must still send an explicit completion control message before the
// remote installs the DB or wakes waiting work.
int arts_remote_rma_put(int dest_rank, uint64_t remote_addr,
                        const void *local_src, uint64_t nbytes);
#ifdef __cplusplus
}
#endif

#endif
