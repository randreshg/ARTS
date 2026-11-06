/* SPDX-License-Identifier: Apache-2.0
 *
 * The runtime functions outside the transport that transport/net.c calls,
 * stubbed for a test that compiles net.c into its own translation unit and
 * links no other runtime unit: the bootstrap mesh is never armed, so no peer
 * reads as gone and no sentinel probe is due, the rank's shutdown point is
 * never taken, and a take that a test still reaches returns false.
 *
 * Including this header DEFINES them: include it once, after net.c.
 */
#ifndef ARTS_TESTS_NET_RUNTIME_STUBS_H
#define ARTS_TESTS_NET_RUNTIME_STUBS_H

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include "arts/system/shutdown.h"
#include "arts/transport/socket.h"

bool arts_socket_armed(void) { return false; }

bool arts_socket_peer_gone(unsigned int rank) {
  (void)rank;
  return false;
}

bool arts_socket_probe_due(uint64_t *next_ns) {
  (void)next_ns;
  return false;
}

bool arts_socket_sentinel_check(void) { return false; }

bool arts_exit_take(int rc, enum arts_exit_origin origin) {
  (void)rc;
  (void)origin;
  return false;
}

_Noreturn void arts_exit_resume(void) { _exit(70); }

bool arts_shutdown_taken(void) { return false; }

#endif /* ARTS_TESTS_NET_RUNTIME_STUBS_H */
