// SPDX-License-Identifier: Apache-2.0
/*
 * mesh_exchange — pure_unit (loopback pairs + fork)
 *
 * TARGET: the address exchange over the formed mesh,
 * arts_net_exchange_addresses in libs/src/core/transport/bootstrap.c, run
 * under the startup deadline.
 *
 * PROPERTIES UNDER TEST (rank 0 of three; peers 1 and 2 are the far ends of
 * loopback TCP pairs this test drives):
 *   PEER_DONE        a peer that writes its frame, reads rank 0's and closes
 *                    while the other peer's frame is still pending is not a
 *                    failure: the exchange returns with both blobs.
 *   PEER_GONE_EARLY  a peer that ends with its frame incomplete ends rank 0
 *                    at once with a hangup line naming it.
 *   STUCK            a peer whose frame never comes ends rank 0 at the
 *                    deadline with an exchange line naming it.
 *   WRONG_RANK       a frame naming another rank than its connection's peer
 *                    is refused (ARTS_ERROR).
 *
 * The real socket.c and bootstrap.c are compiled into this TU; the fabric's
 * two bootstrap calls and the runtime's externs are stubbed locally.  The
 * stubbed arts_exit and arts_exit_resume end the process with 70 + rc.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
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

#include "arts/counter/counter.h"
#include "arts/runtime_state.h"
#include "arts/system/config.h"
#include "arts/system/shutdown.h"
#include "arts/transport/net.h"
#include "arts/transport/protocol.h"

#include "counter_stubs.h"

/* ----- extern stubs the included socket.c and bootstrap.c need ----- */
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

#define BLOB 16u

/* A 16-byte address naming its rank. */
static void blob_of(unsigned int rank, uint8_t *out) {
  memset(out, (int)(0x40u + rank), BLOB);
}

unsigned arts_net_own_address(void *buf, unsigned buflen) {
  if (buflen < BLOB) {
    return 0;
  }
  blob_of(arts_global_rank_id, (uint8_t *)buf);
  return BLOB;
}

static uint8_t g_table[3 * BLOB];
static unsigned g_table_count;
void arts_net_av_insert_table(const void *addrs, unsigned addrlen,
                              unsigned count) {
  if (addrlen == BLOB && count <= 3) {
    memcpy(g_table, addrs, (size_t)addrlen * count);
    g_table_count = count;
  }
}

/* pull in the real implementation */
#include "../../libs/src/core/transport/bootstrap.c"
#include "../../libs/src/core/transport/socket.c"

static int g_failed;

#define FAIL(...)                                                              \
  do {                                                                         \
    fprintf(stdout, "FAIL mesh_exchange: " __VA_ARGS__);                       \
    fputc('\n', stdout);                                                       \
    g_failed++;                                                                \
  } while (0)

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_ms(long ms) {
  struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
  while (nanosleep(&t, &t) != 0 && errno == EINTR) {
  }
}

/* A loopback connection: *near is rank 0's end, *far the peer's. */
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

/* The frame `rank` sends, naming `named`. */
static size_t frame_of(unsigned int rank, unsigned int named, uint8_t *out) {
  struct arts_net_addr_frame_s f;
  memset(&f, 0, sizeof(f));
  f.rank = named;
  f.len = BLOB;
  f.run_id = rank == 0 ? 77u : 0u;
  blob_of(rank, f.addr);
  size_t n = offsetof(struct arts_net_addr_frame_s, addr) + BLOB;
  memcpy(out, &f, n);
  return n;
}

static bool read_full(int fd, uint8_t *buf, size_t n) {
  size_t have = 0;
  while (have < n) {
    ssize_t r = recv(fd, buf + have, n - have, 0);
    if (r <= 0) {
      return false;
    }
    have += (size_t)r;
  }
  return true;
}

/* Rank 0's exchange in a child over near[1], near[2]; exits 0 when it
 * returns with the table holding every rank's blob. */
static pid_t run_rank0(const int near[3], int far_to_close[3], int *err) {
  int p[2];
  if (pipe(p) != 0) {
    perror("pipe");
    exit(1);
  }
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    close(p[0]);
    dup2(p[1], STDERR_FILENO);
    for (int i = 1; i < 3; i++) {
      close(far_to_close[i]);
    }
    static struct arts_config_table_s rows[3];
    static struct arts_config_s cfg;
    static char names[3][8];
    for (unsigned int i = 0; i < 3; i++) {
      snprintf(names[i], sizeof(names[i]), "h%u", i);
      rows[i].rank = i;
      rows[i].ip_address = names[i];
      rows[i].port = 25000;
    }
    cfg.table_length = 3;
    cfg.nodes = 3;
    cfg.my_rank = 0;
    cfg.table = rows;
    arts_transport_set_config(&cfg);
    arts_global_rank_id = 0;
    arts_global_rank_count = 3;
    arts_socket_fd = (int *)calloc(3, sizeof(int));
    arts_socket_fd[0] = -1;
    arts_socket_fd[1] = near[1];
    arts_socket_fd[2] = near[2];
    g_startup_deadline_ns = now_ns() + 1000000000ull;
    uint64_t run = arts_net_exchange_addresses(77u);
    uint8_t want[3 * BLOB];
    for (unsigned int r = 0; r < 3; r++) {
      blob_of(r, want + r * BLOB);
    }
    _exit(run == 77u && g_table_count == 3 &&
                  memcmp(g_table, want, sizeof(want)) == 0
              ? 0
              : 3);
  }
  close(p[1]);
  *err = p[0];
  return pid;
}

struct end_s {
  int status;
  uint64_t ended_ns;
  char text[8192];
};

static void wait_rank0(pid_t pid, int err, struct end_s *e, uint64_t limit_ns) {
  uint64_t until = now_ns() + limit_ns;
  e->status = -1;
  while (now_ns() < until) {
    int st;
    if (waitpid(pid, &st, WNOHANG) == pid) {
      e->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
      break;
    }
    sleep_ms(2);
  }
  e->ended_ns = now_ns();
  if (e->status < 0) {
    kill(pid, SIGKILL);
    int st;
    waitpid(pid, &st, 0);
    e->status = 999;
  }
  ssize_t n = read(err, e->text, sizeof(e->text) - 1);
  e->text[n > 0 ? n : 0] = '\0';
  close(err);
}

static bool pairs(int near[3], int far[3]) {
  near[0] = far[0] = -1;
  return loopback_pair(&near[1], &far[1]) && loopback_pair(&near[2], &far[2]);
}

static void close_all(int near[3], int far[3]) {
  for (int i = 1; i < 3; i++) {
    close(near[i]);
    if (far[i] >= 0) {
      close(far[i]);
    }
  }
}

static void case_peer_done(void) {
  int near[3], far[3];
  if (!pairs(near, far)) {
    FAIL("PEER_DONE: loopback pairs");
    return;
  }
  int err;
  pid_t pid = run_rank0(near, far, &err);
  uint8_t out[512], in[512];
  size_t n = frame_of(1, 1, out);
  if (write(far[1], out, n) != (ssize_t)n || !read_full(far[1], in, n)) {
    FAIL("PEER_DONE: peer 1's exchange");
  }
  close(far[1]);
  far[1] = -1;
  sleep_ms(300);
  n = frame_of(2, 2, out);
  if (write(far[2], out, n) != (ssize_t)n) {
    FAIL("PEER_DONE: peer 2's frame");
  }
  struct end_s e;
  wait_rank0(pid, err, &e, 3000000000ull);
  if (e.status != 0) {
    FAIL("PEER_DONE: rank 0 ended %d\n%s", e.status, e.text);
  }
  close_all(near, far);
}

static void case_peer_gone_early(void) {
  int near[3], far[3];
  if (!pairs(near, far)) {
    FAIL("PEER_GONE_EARLY: loopback pairs");
    return;
  }
  int err;
  pid_t pid = run_rank0(near, far, &err);
  uint8_t out[512];
  frame_of(1, 1, out);
  uint64_t closed = now_ns();
  if (write(far[1], out, 8) != 8) {
    FAIL("PEER_GONE_EARLY: peer 1's partial header");
  }
  close(far[1]);
  far[1] = -1;
  struct end_s e;
  wait_rank0(pid, err, &e, 3000000000ull);
  double ms = (double)(e.ended_ns - closed) / 1e6;
  if (e.status != 145 || ms > 300.0 ||
      strstr(e.text, "ARTS_MESH_FAILED rank=0 host=h0 phase=hangup "
                     "missing=1 missing_hosts=h1 more=0\n") == NULL) {
    FAIL("PEER_GONE_EARLY: rank 0 ended %d %.0f ms after the close\n%s",
         e.status, ms, e.text);
  }
  close_all(near, far);
}

static void case_stuck(void) {
  int near[3], far[3];
  if (!pairs(near, far)) {
    FAIL("STUCK: loopback pairs");
    return;
  }
  int err;
  uint64_t t0 = now_ns();
  pid_t pid = run_rank0(near, far, &err);
  uint8_t out[512];
  size_t n = frame_of(1, 1, out);
  if (write(far[1], out, n) != (ssize_t)n) {
    FAIL("STUCK: peer 1's frame");
  }
  struct end_s e;
  wait_rank0(pid, err, &e, 3000000000ull);
  double ms = (double)(e.ended_ns - t0) / 1e6;
  if (e.status != 145 || ms < 900.0 || ms > 1500.0 ||
      strstr(e.text, "ARTS_MESH_FAILED rank=0 host=h0 phase=exchange "
                     "missing=2 missing_hosts=h2 more=0\n") == NULL) {
    FAIL("STUCK: rank 0 ended %d at %.0f ms\n%s", e.status, ms, e.text);
  }
  close_all(near, far);
}

static void case_wrong_rank(void) {
  int near[3], far[3];
  if (!pairs(near, far)) {
    FAIL("WRONG_RANK: loopback pairs");
    return;
  }
  int err;
  pid_t pid = run_rank0(near, far, &err);
  uint8_t out[512];
  size_t n = frame_of(1, 2, out);
  if (write(far[1], out, n) != (ssize_t)n) {
    FAIL("WRONG_RANK: peer 1's frame");
  }
  struct end_s e;
  wait_rank0(pid, err, &e, 3000000000ull);
  if (e.status != 71) {
    FAIL("WRONG_RANK: rank 0 ended %d\n%s", e.status, e.text);
  }
  close_all(near, far);
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  case_peer_done();
  case_peer_gone_early();
  case_stuck();
  case_wrong_rank();
  if (g_failed) {
    printf("FAIL mesh_exchange: %d check(s) failed\n", g_failed);
    return 1;
  }
  printf("PASS mesh_exchange\n");
  return 0;
}
