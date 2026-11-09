// SPDX-License-Identifier: Apache-2.0
/*
 * socket_helpers — pure_unit
 *
 * TARGET: the standalone helpers in libs/src/core/transport/socket.c:
 *   - hostname_to_ip(host, ip): resolves a hostname into a printable IPv4
 *     string via getaddrinfo(AF_INET) + inet_ntop(..., ip, 100) and returns 0,
 *     or getaddrinfo's error code. Caller contract: `ip` MUST be a >=100-byte
 *     buffer.
 *
 * PROPERTIES UNDER TEST:
 *   1. hostname_to_ip on a numeric IPv4 literal round-trips the exact dotted
 *      string back into the caller buffer and returns 0.
 *   2. hostname_to_ip on an unresolvable name returns an error and does not
 *      write past the buffer (we sentinel-guard the 100-byte slot).
 *
 * The real socket.c is compiled into this TU (project precedent: tests
 * #include a runtime .c directly); externs are stubbed locally.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "arts/counter/counter.h"
#include "arts/runtime_state.h"
#include "arts/system/config.h"
#include "arts/transport/protocol.h"

#include "arts/system/shutdown.h"
#include "counter_stubs.h"

/* ----- extern stubs ----- */
struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;
unsigned int arts_global_rank_id;
unsigned int arts_global_rank_count;

static int g_exit_rc;
_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  _exit(70 + rc);
}
bool arts_exit_take(int rc, enum arts_exit_origin origin) {
  (void)origin;
  g_exit_rc = rc;
  return true;
}
_Noreturn void arts_exit_resume(void) { _exit(70 + g_exit_rc); }
void arts_exit_startup_arm(uint64_t at_ns) { (void)at_ns; }
void arts_exit_startup_disarm(void) {}
unsigned int arts_signal_handlers_rank(void) { return 0; }
bool arts_exit_began(void) { return false; }
bool arts_exit_rank_known(void) { return true; }
bool arts_shutdown_taken(void) { return false; }
void *arts_malloc(size_t s) { return malloc(s); }
void *arts_calloc(size_t n, size_t s) { return calloc(n, s); }
void *arts_realloc(void *p, size_t s) { return realloc(p, s); }
void arts_free(void *p) { free(p); }
void arts_transport_dispatch_packet(struct arts_msg_header_s *packet) {
  (void)packet;
}

/* pull in the real implementation */
#include "../../libs/src/core/transport/socket.c"

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL socket_helpers: %s (line %d)\n", msg, __LINE__);   \
      return 1;                                                                \
    }                                                                          \
  } while (0)

int main(void) {
  /* ---- 1. hostname_to_ip round-trips a numeric IPv4 literal ---- */
  {
    char ip[100];
    /* sentinel just past the documented 100-byte slot to catch overflow */
    memset(ip, 0xAB, sizeof(ip));
    bool ok = hostname_to_ip("192.0.2.55", ip) == 0;
    CHECK(ok, "hostname_to_ip(numeric) returned an error");
    CHECK(strcmp(ip, "192.0.2.55") == 0,
          "hostname_to_ip(numeric) wrong dotted string");
  }

  /* ---- 2. hostname_to_ip on an unresolvable name returns an error ---- */
  {
    /* A syntactically-invalid name that getaddrinfo will reject. Use the
     * AI flags path: an empty + clearly-bogus label. */
    char ip[128];
    /* place a guard byte at index 100 (just past the contract slot) */
    memset(ip, 0, sizeof(ip));
    ip[100] = (char)0x7E;
    bool ok =
        hostname_to_ip("this.name.does.not.exist.invalid.arts.test.", ip) == 0;
    /* On failure hostname_to_ip must return an error and must NOT have
     * written the result (the guard byte is untouched because inet_ntop never
     * ran). */
    CHECK(!ok, "hostname_to_ip(bogus) unexpectedly succeeded");
    CHECK(ip[100] == (char)0x7E,
          "hostname_to_ip(bogus) wrote past/inside buffer on failure");
  }

  printf("PASS socket_helpers (hostname_to_ip round-trip + failure)\n");
  return 0;
}
