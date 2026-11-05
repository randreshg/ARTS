/* SPDX-License-Identifier: Apache-2.0
 *
 * The runtime functions outside the transport that transport/net.c calls,
 * stubbed for a test that compiles net.c into its own translation unit and
 * links no other runtime unit: the bootstrap mesh is never armed, so no peer
 * reads as gone.
 *
 * Including this header DEFINES them: include it once, after net.c.
 */
#ifndef ARTS_TESTS_NET_RUNTIME_STUBS_H
#define ARTS_TESTS_NET_RUNTIME_STUBS_H

#include <stdbool.h>

#include "arts/transport/socket.h"

bool arts_socket_armed(void) { return false; }

bool arts_socket_peer_gone(unsigned int rank) {
  (void)rank;
  return false;
}

#endif /* ARTS_TESTS_NET_RUNTIME_STUBS_H */
