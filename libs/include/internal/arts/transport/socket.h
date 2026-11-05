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

/* The bootstrap mesh's sockets, set up by arts_transport_setup_outgoing /
 * _incoming on the main thread and read by the address exchange on that same
 * thread, and by any thread from arts_socket_armed returning true until
 * arts_socket_cleanup:
 *   arts_socket_send_list[peer * arts_socket_ports + port]  this rank's
 *     connection to peer;
 *   arts_socket_receive_list[port + conn * arts_socket_ports]  accepted peer
 *     connections, in acceptance order.
 * arts_socket_ports is the configuration's port count. */
extern unsigned int arts_socket_ports;
extern int *arts_socket_send_list;
extern int *arts_socket_receive_list;

int arts_get_new_socket();
int arts_get_socket_listening(struct sockaddr_in *listening_socket,
                              unsigned int port);
int arts_get_socket_outgoing(struct sockaddr_in *outgoing_socket,
                             unsigned int port, in_addr_t s_addr);

void arts_transport_set_config(struct arts_config_s *config);
/* Slide a local multi-node run's port block past whatever already holds it, by
 * probing the exact set of ports the run is about to claim; rewrites
 * config->ports on success.  Callable only from the process that spawns
 * the other ranks — they inherit the chosen base and must not probe again.
 * Returns false when no free block fits below the ephemeral range, leaving the
 * configured ports in place. */
bool arts_transport_select_local_ports(struct arts_config_s *config);
void arts_transport_setup_outgoing();
bool arts_transport_setup_incoming();
/* Convert the bootstrap TCP mesh into payload-free liveness sentinels once the
 * fi-address exchange has finished: every established connection stays open
 * (connection lifetime doubles as peer liveness — orderly close and process
 * death both surface as HUP on the peer's accept-side socket); only the
 * listening sockets are closed.  The launcher / stdio sockets are untouched.
 * Its last act arms the mesh (arts_socket_armed). */
void arts_socket_sentinel_arm(void);

/* Whether the mesh is armed.  True only after arts_socket_sentinel_arm, whose
 * arming publishes to the caller, whatever its thread, the fabric's peer
 * table and endpoint, this rank's id and the mesh's sockets.  Callable from
 * any thread and from a signal handler (one lock-free atomic load).  What it
 * publishes stays valid until arts_socket_cleanup, which must run only once
 * every thread that may call this or arts_socket_peer_gone has stopped. */
bool arts_socket_armed(void);

/* Whether this rank's connection to `rank` (< arts_global_rank_count) shows
 * the peer gone: closed or reset by it, or dropped by keepalive.  False for
 * this rank and before the mesh is armed.  Non-blocking, callable from any
 * thread and async-signal-safe (an atomic load, poll and recv; it may change
 * errno), until arts_socket_cleanup.  It never clears what it reports: once
 * true, it stays true. */
bool arts_socket_peer_gone(unsigned int rank);

/* Non-blocking probe of the accept-side sentinels for a closed peer
 * (POLLHUP/POLLERR/EOF).  Called from a progress thread's idle cycle; on a
 * closed peer it enters the same idempotent shutdown entry an inbound
 * shutdown message does, as a rank that learned of the end, and returns true.
 * Stands down (false) once this rank's shutdown is in progress.  Nothing
 * orders any rank's close after every other rank has taken in a SHUTDOWN
 * post — the post is best effort and reaches only the peers its rank
 * contacted, and no rank waits for anyone before it tears down — so an
 * orderly teardown can reach the probe first; it joins the same shutdown.
 * Serialized internally to a single prober. */
bool arts_socket_sentinel_check(void);
#ifdef __cplusplus
}
#endif

#endif
