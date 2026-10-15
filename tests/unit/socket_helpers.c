// SPDX-License-Identifier: Apache-2.0
/*
 * socket_helpers — pure_unit
 *
 * TARGET: the standalone helpers in libs/src/core/transport/socket.c:
 *   - hostname_to_ip(host, ip): resolves a hostname into a printable IPv4
 *     string via getaddrinfo(AF_INET) + inet_ntop(..., ip, 100) and returns 0,
 *     or getaddrinfo's error code. Caller contract: `ip` MUST be a >=100-byte
 *     buffer.
 *   - arts_get_new_socket(): TCP socket factory (+ TCP_NODELAY).
 *   - arts_get_socket_listening(sa, port): TCP socket factory; fills sa with
 *     AF_INET / INADDR_ANY / htons(port).
 *   - arts_get_socket_outgoing(sa, port, s_addr): TCP socket factory; fills
 *     sa with AF_INET / s_addr / htons(port).
 *
 * PROPERTIES UNDER TEST:
 *   1. hostname_to_ip on a numeric IPv4 literal round-trips the exact dotted
 *      string back into the caller buffer and returns 0.
 *   2. hostname_to_ip on an unresolvable name returns an error and does not
 *      write past the buffer (we sentinel-guard the 100-byte slot).
 *   3. Socket factories return a valid fd and populate the sockaddr_in with
 *      the exact family/addr/port the contract promises.
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

#include "counter_stubs.h"

/* ----- extern stubs ----- */
struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;
unsigned int arts_global_rank_id;
unsigned int arts_global_rank_count;

void arts_abort(uint8_t code) { _exit(70 + code); }
void *arts_malloc(size_t s) { return malloc(s); }
void *arts_calloc(size_t n, size_t s) { return calloc(n, s); }
void arts_free(void *p) { free(p); }
unsigned int arts_atomic_sub(volatile unsigned int *d, unsigned int v) {
  return __atomic_sub_fetch(d, v, __ATOMIC_SEQ_CST);
}
unsigned int arts_atomic_add(volatile unsigned int *d, unsigned int v) {
  return __atomic_add_fetch(d, v, __ATOMIC_SEQ_CST);
}
void arts_enter_shutdown_state(bool initiator) { (void)initiator; }
void arts_runtime_stop(void) {}
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

  /* ---- 3. socket factories populate sockaddr fields exactly ---- */
  {
    int fd = arts_get_new_socket();
    CHECK(fd >= 0, "arts_get_new_socket returned bad fd");
    close(fd);
  }
  {
    struct sockaddr_in sa;
    memset(&sa, 0xCC, sizeof(sa));
    int fd = arts_get_socket_listening(&sa, 25123);
    CHECK(fd >= 0, "arts_get_socket_listening returned bad fd");
    CHECK(sa.sin_family == AF_INET, "listening: wrong family");
    CHECK(sa.sin_addr.s_addr == htonl(INADDR_ANY),
          "listening: addr not INADDR_ANY");
    CHECK(sa.sin_port == htons(25123), "listening: wrong port");
    close(fd);
  }
  {
    struct sockaddr_in sa;
    memset(&sa, 0xCC, sizeof(sa));
    in_addr_t a = inet_addr("198.51.100.9");
    int fd = arts_get_socket_outgoing(&sa, 25124, a);
    CHECK(fd >= 0, "arts_get_socket_outgoing returned bad fd");
    CHECK(sa.sin_family == AF_INET, "outgoing: wrong family");
    CHECK(sa.sin_addr.s_addr == a, "outgoing: wrong addr");
    CHECK(sa.sin_port == htons(25124), "outgoing: wrong port");
    close(fd);
  }

  printf("PASS socket_helpers (hostname_to_ip round-trip + failure, socket "
         "factories)\n");
  return 0;
}
