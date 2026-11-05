// SPDX-License-Identifier: Apache-2.0
/*
 * socket_mesh_liveness — pure_unit (loopback mesh + fork)
 *
 * TARGET: the bootstrap mesh of libs/src/core/transport/socket.c.
 *
 * PROPERTIES UNDER TEST:
 *   1. Every socket the mesh creates (the three factories) or accepts is
 *      close-on-exec, so no process a rank execs keeps a mesh connection
 *      open after the rank ends.
 *   2. Both ends of every established mesh connection carry keepalive:
 *      SO_KEEPALIVE on, idle 30 s, interval 10 s, 4 probes.
 *   3. arts_socket_peer_gone(r) reads this rank's connection to r: false for
 *      this rank and before the arm whatever the connection's state; after
 *      the arm, false while the far end is open and true once it closed (FIN)
 *      or reset (RST), and every later call reports the same, a call after
 *      the reset's pending error was taken included.
 *   4. The peek decides alone when the poll fails (EINTR, a signal pending
 *      at a zero-timeout poll): an open peer reads alive, a FIN or an RST
 *      reads gone, and so does the RST once the peek took its error; each
 *      errno of the peek's list reads gone, an empty or interrupted peek
 *      does not.
 *   5. arts_socket_probe_due is false before the arm whatever the cadence;
 *      after it, false until the coarse monotonic clock reaches *next_ns,
 *      and once it has, true with *next_ns moved 1 ms past the clock's
 *      reading.  arts_socket_sentinel_check reports the accepted connection
 *      a peer closed, and enters nothing (this test links no shutdown
 *      entry).
 *
 * HARNESS: a two-rank mesh on loopback, one port per rank.  The port block is
 * chosen by arts_transport_select_local_ports, as a spawning rank does; the
 * process then forks, and parent (rank 0) and child (rank 1) each run
 * arts_transport_setup_outgoing and arts_transport_setup_incoming, so every
 * connection is made by arts_transport_connect and taken by the accept path.
 * Each process checks the fds it holds; the child reports by exit status.
 * Properties 3 and 4 run in the parent on connections it builds itself and
 * installs as rank 1's send socket.  socket.c's poll and recv go through
 * switchable shims: by default each passes through; poll can be made to fail
 * with EINTR and recv with a chosen errno.
 *
 * The real socket.c is compiled into this TU; externs are stubbed locally.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <ifaddrs.h>
#include <inttypes.h>
#include <net/if.h>
#include <netdb.h>

#include "arts.h"
#include "arts/counter/counter.h"
#include "arts/runtime_state.h"
#include "arts/system/config.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/transport/dispatcher.h"
#include "arts/transport/netif.h"
#include "arts/transport/protocol.h"
#include "arts/transport/socket.h"
#include "arts/utils/malloc.h"
#include "arts/utils/stdatomic_compat.h"

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
void arts_transport_dispatch_packet(struct arts_msg_header_s *packet) {
  (void)packet;
}

/* Every header socket.c includes is in already, so the two macros below
 * rename only the calls in socket.c's own body. */
static bool g_poll_eintr;
static int g_recv_errno;

static int socket_test_poll(struct pollfd *fds, nfds_t n, int timeout) {
  if (g_poll_eintr) {
    errno = EINTR;
    return -1;
  }
  return poll(fds, n, timeout);
}

static ssize_t socket_test_recv(int fd, void *buf, size_t len, int flags) {
  if (g_recv_errno != 0) {
    errno = g_recv_errno;
    return -1;
  }
  return recv(fd, buf, len, flags);
}

/* pull in the real implementation */
#define poll socket_test_poll
#define recv socket_test_recv
#include "../../libs/src/core/transport/socket.c"
#undef recv
#undef poll

static const char *g_who = "rank 0";

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL socket_mesh_liveness (%s): %s (line %d)\n", g_who, \
              msg, __LINE__);                                                  \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static bool is_cloexec(int fd) {
  int flags = fcntl(fd, F_GETFD);
  return flags >= 0 && (flags & FD_CLOEXEC) != 0;
}

static int sockopt_int(int fd, int level, int name) {
  int v = -1;
  socklen_t len = sizeof(v);
  if (getsockopt(fd, level, name, &v, &len) != 0) {
    return -1;
  }
  return v;
}

static bool has_mesh_keepalive(int fd) {
  return sockopt_int(fd, SOL_SOCKET, SO_KEEPALIVE) == 1 &&
         sockopt_int(fd, IPPROTO_TCP, TCP_KEEPIDLE) == 30 &&
         sockopt_int(fd, IPPROTO_TCP, TCP_KEEPINTVL) == 10 &&
         sockopt_int(fd, IPPROTO_TCP, TCP_KEEPCNT) == 4;
}

/* The fds one rank of the mesh holds: its listener, its connection to the
 * peer and the connection it accepted from the peer. */
static int check_mesh_ends(unsigned int peer) {
  int listener = local_socket_receive[0];
  int sent = arts_socket_send_list[peer * arts_socket_ports];
  int taken = arts_socket_receive_list[0];
  CHECK(is_cloexec(listener), "listening socket is not close-on-exec");
  CHECK(is_cloexec(sent), "connected socket is not close-on-exec");
  CHECK(is_cloexec(taken), "accepted socket is not close-on-exec");
  CHECK(has_mesh_keepalive(sent), "connected socket lacks keepalive 1/30/10/4");
  CHECK(has_mesh_keepalive(taken), "accepted socket lacks keepalive 1/30/10/4");
  return 0;
}

/* A loopback connection outside the mesh: *near is the connecting end. */
static bool loopback_pair(int *near, int *far) {
  int l = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(a);
  if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof(a)) != 0 ||
      listen(l, 1) != 0 || getsockname(l, (struct sockaddr *)&a, &len) != 0) {
    return false;
  }
  *near = socket(AF_INET, SOCK_STREAM, 0);
  if (*near < 0 || connect(*near, (struct sockaddr *)&a, sizeof(a)) != 0) {
    return false;
  }
  *far = accept(l, NULL, NULL);
  close(l);
  return *far >= 0;
}

static uint64_t coarse_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Wait until the kernel delivered the far end's FIN or RST to fd. */
static bool await_hangup(int fd) {
  struct pollfd p = {.fd = fd, .events = POLLRDHUP};
  return poll(&p, 1, 5000) == 1 &&
         (p.revents & (POLLRDHUP | POLLHUP | POLLERR)) != 0;
}

static int run_rank(struct arts_config_s *config, unsigned int me) {
  config->my_rank = me;
  arts_global_rank_id = me;
  arts_global_rank_count = 2;
  arts_transport_set_config(config);
  ip_list = (char *)arts_malloc(200);
  strcpy(ip_list, "127.0.0.1");
  strcpy(ip_list + 100, "127.0.0.1");
  arts_transport_setup_outgoing();
  CHECK(arts_transport_setup_incoming(), "mesh setup failed");
  return check_mesh_ends(1 - me);
}

int main(void) {
  /* ---- 1. the factories ---- */
  {
    struct sockaddr_in sa;
    int fd = arts_get_new_socket();
    CHECK(fd >= 0 && is_cloexec(fd), "arts_get_new_socket: not close-on-exec");
    close(fd);
    fd = arts_get_socket_listening(&sa, 25123);
    CHECK(fd >= 0 && is_cloexec(fd),
          "arts_get_socket_listening: not close-on-exec");
    close(fd);
    fd = arts_get_socket_outgoing(&sa, 25124, inet_addr("127.0.0.1"));
    CHECK(fd >= 0 && is_cloexec(fd),
          "arts_get_socket_outgoing: not close-on-exec");
    close(fd);
  }

  /* ---- 2. a two-rank loopback mesh ---- */
  unsigned int base_ports[1] = {ARTS_PORT_WINDOW_LO +
                                ((unsigned int)getpid() % 5000u) * 2u};
  unsigned int row_ports[2][1] = {{0}, {0}};
  struct arts_config_table_s rows[2] = {
      {.rank = 0, .ip_address = "127.0.0.1", .ports = row_ports[0]},
      {.rank = 1, .ip_address = "127.0.0.1", .ports = row_ports[1]}};
  struct arts_config_s config;
  memset(&config, 0, sizeof(config));
  config.launcher = "local";
  config.master_boot = true;
  config.port_count = 1;
  config.ports = base_ports;
  config.table_length = 2;
  config.nodes = 2;
  config.table = rows;
  unsetenv(ARTS_RESOLVED_PORTS_ENV);
  CHECK(arts_transport_select_local_ports(&config), "no free port block");
  setenv("ARTS_STARTUP_TIMEOUT_S", "20", 1);

  pid_t child = fork();
  CHECK(child >= 0, "fork failed");
  if (child == 0) {
    g_who = "rank 1";
    _exit(run_rank(&config, 1));
  }
  int rc = run_rank(&config, 0);
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child, "waitpid failed");
  CHECK(rc == 0, "rank 0's mesh ends");
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "rank 1's mesh ends");

  /* ---- 3. arts_socket_peer_gone ---- */
  int near_a, far_a, near_b, far_b, near_c, far_c;
  CHECK(loopback_pair(&near_a, &far_a), "loopback pair A");
  close(far_a);
  CHECK(await_hangup(near_a), "pair A's FIN never arrived");
  close(arts_socket_send_list[1 * arts_socket_ports]);
  arts_socket_send_list[1 * arts_socket_ports] = near_a;
  CHECK(!arts_socket_armed(), "armed before the arm");
  CHECK(!arts_socket_peer_gone(1), "a peer reported gone before the arm");
  uint64_t next_ns = 0;
  CHECK(!arts_socket_probe_due(&next_ns) && next_ns == 0,
        "a probe due before the arm");

  arts_socket_sentinel_arm();
  CHECK(arts_socket_armed(), "not armed after the arm");
  CHECK(local_socket_receive[0] == 0, "the arm left the listener open");

  close(arts_socket_send_list[0]);
  arts_socket_send_list[0] = near_a;
  CHECK(!arts_socket_peer_gone(0), "this rank reported gone");

  CHECK(loopback_pair(&near_b, &far_b), "loopback pair B");
  arts_socket_send_list[1 * arts_socket_ports] = near_b;
  CHECK(!arts_socket_peer_gone(1), "an open peer reported gone");
  g_poll_eintr = true;
  CHECK(!arts_socket_peer_gone(1), "an open peer reported gone (peek)");
  g_poll_eintr = false;
  close(far_b);
  CHECK(await_hangup(near_b), "pair B's FIN never arrived");
  CHECK(arts_socket_peer_gone(1), "a closed peer not reported gone");
  CHECK(arts_socket_peer_gone(1), "a closed peer's report was consumed");
  g_poll_eintr = true;
  CHECK(arts_socket_peer_gone(1), "a closed peer not reported gone (peek)");
  CHECK(arts_socket_peer_gone(1), "a closed peer's report was consumed (peek)");
  g_poll_eintr = false;

  CHECK(loopback_pair(&near_c, &far_c), "loopback pair C");
  arts_socket_send_list[1 * arts_socket_ports] = near_c;
  CHECK(!arts_socket_peer_gone(1), "an open peer reported gone");
  struct linger reset = {.l_onoff = 1, .l_linger = 0};
  CHECK(setsockopt(far_c, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0,
        "SO_LINGER");
  close(far_c);
  CHECK(await_hangup(near_c), "pair C's RST never arrived");
  CHECK(arts_socket_peer_gone(1), "a reset peer not reported gone");
  CHECK(arts_socket_peer_gone(1), "a reset peer's report was consumed");
  g_poll_eintr = true;
  CHECK(arts_socket_peer_gone(1), "a reset peer not reported gone (peek)");
  CHECK(arts_socket_peer_gone(1), "a reset peer read as alive once the peek "
                                  "took its pending error");
  g_poll_eintr = false;
  CHECK(arts_socket_peer_gone(1), "a reset peer read as alive once its "
                                  "pending error was taken");

  /* ---- 4. each errno the peek can return ---- */
  int near_d, far_d;
  CHECK(loopback_pair(&near_d, &far_d), "loopback pair D");
  arts_socket_send_list[1 * arts_socket_ports] = near_d;
  g_poll_eintr = true;
  static const int gone[] = {ECONNRESET, ETIMEDOUT, EPIPE, EHOSTUNREACH,
                             ENETUNREACH};
  for (size_t i = 0; i < sizeof(gone) / sizeof(gone[0]); i++) {
    g_recv_errno = gone[i];
    if (!arts_socket_peer_gone(1)) {
      fprintf(stderr,
              "FAIL socket_mesh_liveness: a peek failing with %s "
              "did not read gone\n",
              strerror(gone[i]));
      return 1;
    }
  }
  static const int alive[] = {EAGAIN, EINTR};
  for (size_t i = 0; i < sizeof(alive) / sizeof(alive[0]); i++) {
    g_recv_errno = alive[i];
    if (arts_socket_peer_gone(1)) {
      fprintf(stderr,
              "FAIL socket_mesh_liveness: a peek failing with %s "
              "read gone\n",
              strerror(alive[i]));
      return 1;
    }
  }
  g_recv_errno = 0;
  g_poll_eintr = false;
  CHECK(!arts_socket_peer_gone(1), "an open peer reported gone");

  /* ---- 5. the probe's cadence and the sentinel check ---- */
  uint64_t now = coarse_now_ns();
  next_ns = now + 3600ull * 1000000000ull;
  uint64_t later = next_ns;
  CHECK(!arts_socket_probe_due(&next_ns) && next_ns == later,
        "a probe due before its time");
  next_ns = now;
  uint64_t before = coarse_now_ns();
  CHECK(arts_socket_probe_due(&next_ns), "a due probe not reported");
  uint64_t after = coarse_now_ns();
  CHECK(next_ns >= before + 1000000ull && next_ns <= after + 1000000ull,
        "a due probe did not move the cadence 1 ms past the clock");
  CHECK(await_hangup(arts_socket_receive_list[0]),
        "rank 1's end never reached its accepted connection");
  CHECK(arts_socket_sentinel_check(), "a closed peer's sentinel not reported");

  printf("PASS socket_mesh_liveness\n");
  return 0;
}
