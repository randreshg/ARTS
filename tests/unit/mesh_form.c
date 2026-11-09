// SPDX-License-Identifier: Apache-2.0
/*
 * mesh_form — pure_unit (loopback ranks + fork)
 *
 * TARGET: the bootstrap mesh's formation, arts_mesh_form in
 * libs/src/core/transport/socket.c: one listen port per rank, one
 * connection per rank pair opened by the higher rank with an 8-byte hello,
 * everything bounded by the startup deadline.
 *
 * PROPERTIES UNDER TEST (one case each):
 *   FORM3        three ranks form; every peer fd is non-blocking,
 *                close-on-exec and kept alive, and a formed fd holds no byte
 *                past the hellos.
 *   LATE_LISTEN  a lowest rank that listens 0.3 s late is reached: its
 *                peers' refused connects are retried, and all three form.
 *   REFUSED      a rank that never runs ends its peers at the deadline, each
 *                with exactly one line: the connect line naming it, or a
 *                hangup line for a peer that gave up first.
 *   BLACKHOLE    a connect that never completes is re-issued after 1 s, and
 *                the deadline ends it with a connect line.
 *   STRAY        a connection whose hello has another magic is closed at once
 *                and ignored.
 *   NOT_ABOVE    a hello naming a rank not above the acceptor's is closed at
 *                once and ignored.
 *   HELD_RANK    a hello naming a rank the acceptor already holds is closed
 *                at once and ignored.
 *   STRAY_SILENT a connection that writes nothing does not delay the
 *                formation and is closed when the formation completes.
 *   EOF_BEFORE_HELLO  a connection that ends before its hello is ignored.
 *   EARLY        bytes queued behind a hello stay unread by the formation.
 *   HANGUP       a formed peer's end during the formation ends this rank at
 *                once with a hangup line naming it.
 *   HELD         a listen port another process holds until the deadline ends
 *                the rank with a listen line.
 *   NOFILE_RAISE  a soft descriptor limit below the mesh's want is raised to
 *                 the hard limit.
 *   NOFILE_REFUSE a hard limit below the mesh's need is refused.
 *   SIGNALLED    a rank whose end began elsewhere ends with no line.
 *   TAKE_LOST    a give-up that loses the take of the end ends with no line.
 *
 * HARNESS: each rank is a forked child with its own stderr pipe; the parent
 * checks every child's status, end time and captured stderr.  The ranks'
 * port block is chosen by arts_transport_select_local_port, as the spawner
 * does, and the deadline is set directly.  connect is interposed to count
 * the calls to one port, forwarding through the system call.  The stubbed
 * arts_exit and arts_exit_resume end the process with 70 + rc: a give-up
 * reads 145 and the ARTS_ERROR path 71.
 *
 * The real socket.c is compiled into this TU; externs are stubbed locally.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "arts/counter/counter.h"
#include "arts/runtime_state.h"
#include "arts/system/config.h"
#include "arts/system/shutdown.h"
#include "arts/transport/protocol.h"
#include "arts/utils/stdatomic_compat.h"

#include "counter_stubs.h"

/* ----- extern stubs the included socket.c needs ----- */
struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL_IE struct arts_runtime_private_s arts_thread_info;
unsigned int arts_global_rank_id;
unsigned int arts_global_rank_count;

static int g_exit_rc;
static bool g_take = true;
/* 0: never taken; else the CLOCK_MONOTONIC ns from which it reads taken. */
static uint64_t g_taken_from_ns;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  (void)origin;
  _exit(70 + rc);
}
bool arts_exit_take(int rc, enum arts_exit_origin origin) {
  (void)origin;
  g_exit_rc = rc;
  return g_take;
}
_Noreturn void arts_exit_resume(void) { _exit(70 + g_exit_rc); }
void arts_exit_startup_arm(uint64_t at_ns) { (void)at_ns; }
void arts_exit_startup_disarm(void) {}
unsigned int arts_signal_handlers_rank(void) { return 0; }
bool arts_exit_rank_known(void) { return true; }
bool arts_shutdown_taken(void) {
  return g_taken_from_ns != 0 && now_ns() >= g_taken_from_ns;
}
bool arts_exit_began(void) { return arts_shutdown_taken(); }
void *arts_malloc(size_t s) { return malloc(s); }
void *arts_calloc(size_t n, size_t s) { return calloc(n, s); }
void *arts_realloc(void *p, size_t s) { return realloc(p, s); }
void arts_free(void *p) { free(p); }
void arts_transport_dispatch_packet(struct arts_msg_header_s *packet) {
  (void)packet;
}

#define MAX_RANKS 4

/* Shared with the children: each writes its own slots only. */
struct shared_s {
  int connects[MAX_RANKS];
  _Atomic(int) formed;
  _Atomic(int) release;
};
static struct shared_s *g_shm;

/* ----- connect interposition: count the calls to one port ----- */
static unsigned int g_count_port;
static int g_count_slot = -1;

int connect(int fd, __CONST_SOCKADDR_ARG addr, socklen_t len) {
  const struct sockaddr_in *in = addr.__sockaddr_in__;
  if (g_count_slot >= 0 && in != NULL && len >= (socklen_t)sizeof(*in) &&
      in->sin_family == AF_INET && ntohs(in->sin_port) == g_count_port) {
    g_shm->connects[g_count_slot]++;
  }
  return (int)syscall(SYS_connect, fd, addr.__sockaddr__, len);
}

/* pull in the real implementation */
#include "../../libs/src/core/transport/socket.c"

static unsigned int g_base;
static int g_failed;

#define FAIL(...)                                                              \
  do {                                                                         \
    fprintf(stdout, "FAIL mesh_form: " __VA_ARGS__);                           \
    fputc('\n', stdout);                                                       \
    g_failed++;                                                                \
  } while (0)

/* What a case asks of one child rank. */
struct rank_opts_s {
  unsigned int n;
  unsigned int me;
  uint64_t deadline_ns;
  unsigned int port[MAX_RANKS]; /* each row's port; 0 = base + row */
  int nofile;                   /* 0, 1 = raise case, 2 = refuse case */
  bool read_early;              /* read 32 bytes 0..31 from peer 1 */
  bool check_fds;               /* the FORM3 fd checks */
  bool take_lost;
  uint64_t taken_from_ns;
};

static unsigned int open_fds(void) {
  DIR *d = opendir("/proc/self/fd");
  unsigned int n = 0;
  if (d == NULL) {
    return 0;
  }
  for (struct dirent *e = readdir(d); e != NULL; e = readdir(d)) {
    if (e->d_name[0] != '.') {
      n++;
    }
  }
  closedir(d);
  return n - 1; /* the directory's own descriptor */
}

static int sockopt_int(int fd, int level, int name) {
  int v = -1;
  socklen_t len = sizeof(v);
  if (getsockopt(fd, level, name, &v, &len) != 0) {
    return -1;
  }
  return v;
}

/* The child's checks after a formation; returns its exit status. */
static int after_form(const struct rank_opts_s *o) {
  if (o->check_fds) {
    for (unsigned int p = 0; p < o->n; p++) {
      int fd = arts_socket_fd[p];
      if (p == o->me) {
        if (fd != -1) {
          fprintf(stderr, "CHILD: own entry is %d\n", fd);
          return 3;
        }
        continue;
      }
      int fl = fcntl(fd, F_GETFL);
      int fdf = fcntl(fd, F_GETFD);
      if (fd < 0 || fl < 0 || (fl & O_NONBLOCK) == 0 || fdf < 0 ||
          (fdf & FD_CLOEXEC) == 0 ||
          sockopt_int(fd, SOL_SOCKET, SO_KEEPALIVE) != 1) {
        fprintf(stderr,
                "CHILD: peer %u's fd %d is not non-blocking, "
                "close-on-exec and kept alive\n",
                p, fd);
        return 3;
      }
      char b;
      if (o->me == 0 && (recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT) != -1 ||
                         (errno != EAGAIN && errno != EWOULDBLOCK))) {
        fprintf(stderr, "CHILD: peer %u's fd holds a byte past the hello\n", p);
        return 3;
      }
    }
  }
  if (o->read_early) {
    unsigned char got[32];
    size_t have = 0;
    uint64_t until = now_ns() + 1000000000ull;
    while (have < sizeof(got) && now_ns() < until) {
      struct pollfd pfd = {.fd = arts_socket_fd[1], .events = POLLIN};
      (void)poll(&pfd, 1, 100);
      ssize_t r =
          recv(arts_socket_fd[1], got + have, sizeof(got) - have, MSG_DONTWAIT);
      if (r > 0) {
        have += (size_t)r;
      } else if (r == 0) {
        break;
      }
    }
    for (size_t i = 0; i < sizeof(got); i++) {
      if (have != sizeof(got) || got[i] != (unsigned char)i) {
        fprintf(stderr, "CHILD: the early bytes read back wrong (%zu)\n", have);
        return 3;
      }
    }
  }
  if (o->nofile == 1) {
    struct rlimit lim;
    getrlimit(RLIMIT_NOFILE, &lim);
    if (lim.rlim_cur != lim.rlim_max) {
      fprintf(stderr, "CHILD: the soft limit %llu was not raised to %llu\n",
              (unsigned long long)lim.rlim_cur,
              (unsigned long long)lim.rlim_max);
      return 3;
    }
  }
  return 0;
}

static _Noreturn void run_rank(const struct rank_opts_s *o) {
  static struct arts_config_table_s rows[MAX_RANKS];
  static struct arts_config_s cfg;
  memset(&cfg, 0, sizeof(cfg));
  for (unsigned int i = 0; i < o->n; i++) {
    rows[i].rank = i;
    rows[i].ip_address = (char *)"127.0.0.1";
    rows[i].port = o->port[i] != 0 ? o->port[i] : g_base + i;
  }
  cfg.launcher = (char *)"local";
  cfg.table_length = o->n;
  cfg.nodes = o->n;
  cfg.my_rank = o->me;
  cfg.table = rows;
  cfg.port = g_base;
  arts_global_rank_id = o->me;
  arts_global_rank_count = o->n;
  arts_transport_set_config(&cfg);
  ip_list = (char *)malloc(100u * o->n);
  for (unsigned int i = 0; i < o->n; i++) {
    strcpy(ip_list + (100u * i), "127.0.0.1");
  }
  g_take = !o->take_lost;
  g_taken_from_ns = o->taken_from_ns;
  g_startup_deadline_ns = o->deadline_ns;
  if (o->nofile != 0) {
    struct rlimit lim;
    getrlimit(RLIMIT_NOFILE, &lim);
    lim.rlim_cur = open_fds() + 2;
    if (o->nofile == 2) {
      lim.rlim_max = lim.rlim_cur;
    }
    if (setrlimit(RLIMIT_NOFILE, &lim) != 0) {
      fprintf(stderr, "CHILD: setrlimit: %s\n", strerror(errno));
      _exit(4);
    }
  }
  arts_mesh_form(&cfg);
  int rc = after_form(o);
  atomic_fetch_add_explicit(&g_shm->formed, 1, memory_order_relaxed);
  uint64_t until = now_ns() + 3000000000ull;
  while (atomic_load_explicit(&g_shm->release, memory_order_relaxed) == 0 &&
         now_ns() < until) {
    struct timespec tick = {0, 1000000L};
    nanosleep(&tick, NULL);
  }
  _exit(rc);
}

struct child_s {
  pid_t pid;
  int err;           /* read end of its stderr */
  int status;        /* -1 while running */
  uint64_t ended_ns; /* first poll that saw it gone */
  char text[16384];
  size_t len;
};

static void spawn(struct child_s *c, const struct rank_opts_s *o) {
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
    close(p[1]);
    g_count_port = o->port[0] != 0 ? o->port[0] : g_base;
    g_count_slot = (int)o->me;
    run_rank(o);
  }
  close(p[1]);
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  memset(c, 0, sizeof(*c));
  c->pid = pid;
  c->err = p[0];
  c->status = -1;
}

static void drain(struct child_s *c) {
  for (;;) {
    ssize_t r = read(c->err, c->text + c->len, sizeof(c->text) - 1 - c->len);
    if (r <= 0) {
      break;
    }
    c->len += (size_t)r;
  }
  c->text[c->len] = '\0';
}

/* Waits for every child (up to `limit_ns` from now), releasing the formed
 * ones once `nform` of them formed; kills any still running at the limit. */
static void wait_all(struct child_s *c, int nc, int nform, uint64_t limit_ns) {
  uint64_t until = now_ns() + limit_ns;
  int left = nc;
  while (left > 0 && now_ns() < until) {
    if (atomic_load_explicit(&g_shm->formed, memory_order_relaxed) >= nform) {
      atomic_store_explicit(&g_shm->release, 1, memory_order_relaxed);
    }
    for (int i = 0; i < nc; i++) {
      drain(&c[i]);
      if (c[i].status < 0) {
        int st;
        if (waitpid(c[i].pid, &st, WNOHANG) == c[i].pid) {
          c[i].status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
          c[i].ended_ns = now_ns();
          left--;
        }
      }
    }
    struct timespec tick = {0, 2000000L};
    nanosleep(&tick, NULL);
  }
  for (int i = 0; i < nc; i++) {
    if (c[i].status < 0) {
      kill(c[i].pid, SIGKILL);
      int st;
      waitpid(c[i].pid, &st, 0);
      c[i].status = 999;
      c[i].ended_ns = now_ns();
    }
    drain(&c[i]);
    close(c[i].err);
  }
}

static int count_lines(const char *text, const char *needle) {
  int n = 0;
  for (const char *at = strstr(text, "ARTS_MESH_FAILED"); at != NULL;
       at = strstr(at + 1, "ARTS_MESH_FAILED")) {
    const char *eol = strchr(at, '\n');
    size_t len = eol != NULL ? (size_t)(eol - at) : strlen(at);
    if (needle == NULL || (memmem(at, len, needle, strlen(needle)) != NULL)) {
      n++;
    }
  }
  return n;
}

static void reset_case(void) {
  memset(g_shm, 0, sizeof(*g_shm));
  struct arts_config_table_s rows[MAX_RANKS];
  struct arts_config_s cfg;
  memset(&cfg, 0, sizeof(cfg));
  memset(rows, 0, sizeof(rows));
  for (unsigned int i = 0; i < MAX_RANKS; i++) {
    rows[i].rank = i;
    rows[i].ip_address = (char *)"127.0.0.1";
  }
  cfg.launcher = (char *)"local";
  cfg.table_length = MAX_RANKS;
  cfg.table = rows;
  cfg.port = g_base + 2 * MAX_RANKS;
  unsetenv(ARTS_RESOLVED_PORT_ENV);
  if (!arts_transport_select_local_port(&cfg)) {
    FAIL("no free port block");
    exit(1);
  }
  g_base = cfg.port;
}

static struct rank_opts_s opts(unsigned int n, unsigned int me, uint64_t d) {
  struct rank_opts_s o;
  memset(&o, 0, sizeof(o));
  o.n = n;
  o.me = me;
  o.deadline_ns = d;
  return o;
}

/* A client of this host's `port`, retried until the port listens or 1 s
 * passed; -1 when it never did. */
static int client_of(unsigned int port) {
  uint64_t until = now_ns() + 1000000000ull;
  while (now_ns() < until) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
      return fd;
    }
    close(fd);
    struct timespec tick = {0, 5000000L};
    nanosleep(&tick, NULL);
  }
  return -1;
}

static int listener_on(unsigned int port, int backlog) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons((uint16_t)port);
  if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
      listen(fd, backlog) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static unsigned int port_of(int fd) {
  struct sockaddr_in a;
  socklen_t len = sizeof(a);
  getsockname(fd, (struct sockaddr *)&a, &len);
  return ntohs(a.sin_port);
}

static double ms_since(uint64_t t0, uint64_t t) {
  return (double)(t - t0) / 1e6;
}

static void report(const char *name, struct child_s *c, int nc) {
  for (int i = 0; i < nc; i++) {
    printf("  %s child %d: status %d\n%s", name, i, c[i].status, c[i].text);
  }
}

static void case_form3(void) {
  reset_case();
  struct child_s c[3];
  uint64_t d = now_ns() + 1000000000ull;
  for (unsigned int r = 0; r < 3; r++) {
    struct rank_opts_s o = opts(3, r, d);
    o.check_fds = true;
    spawn(&c[r], &o);
  }
  wait_all(c, 3, 3, 3000000000ull);
  for (int r = 0; r < 3; r++) {
    if (c[r].status != 0) {
      FAIL("FORM3: rank %d ended %d", r, c[r].status);
      report("FORM3", c, 3);
      return;
    }
  }
}

static void case_late_listen(void) {
  reset_case();
  struct child_s c[3];
  uint64_t t0 = now_ns();
  uint64_t d = t0 + 1000000000ull;
  struct rank_opts_s o1 = opts(3, 1, d);
  struct rank_opts_s o2 = opts(3, 2, d);
  spawn(&c[1], &o1);
  spawn(&c[2], &o2);
  struct timespec late = {0, 300000000L};
  nanosleep(&late, NULL);
  struct rank_opts_s o0 = opts(3, 0, d);
  spawn(&c[0], &o0);
  wait_all(c, 3, 3, 3000000000ull);
  for (int r = 0; r < 3; r++) {
    if (c[r].status != 0 || c[r].ended_ns > d + 500000000ull) {
      FAIL("LATE_LISTEN: rank %d ended %d at %.0f ms", r, c[r].status,
           ms_since(t0, c[r].ended_ns));
      report("LATE_LISTEN", c, 3);
      return;
    }
  }
  if (g_shm->connects[1] < 2 || g_shm->connects[2] < 2) {
    FAIL("LATE_LISTEN: connects to rank 0's port: rank 1 %d, rank 2 %d",
         g_shm->connects[1], g_shm->connects[2]);
  }
}

static void case_refused(void) {
  reset_case();
  struct child_s c[2];
  uint64_t t0 = now_ns();
  uint64_t d = t0 + 1000000000ull;
  for (unsigned int r = 0; r < 2; r++) {
    struct rank_opts_s o = opts(3, r, d);
    spawn(&c[r], &o);
  }
  wait_all(c, 2, 3, 3000000000ull);
  int cause = 0;
  for (int r = 0; r < 2; r++) {
    if (c[r].status != 145 || ms_since(t0, c[r].ended_ns) > 1500.0) {
      FAIL("REFUSED: rank %d ended %d at %.0f ms", r, c[r].status,
           ms_since(t0, c[r].ended_ns));
    }
    int connect_lines =
        count_lines(c[r].text, "phase=connect missing=2 "
                               "missing_hosts=127.0.0.1 more=0");
    int hangup_lines = count_lines(c[r].text, "phase=hangup ");
    cause += connect_lines;
    if (count_lines(c[r].text, NULL) != 1 ||
        connect_lines + hangup_lines != 1) {
      FAIL("REFUSED: rank %d printed %d line(s)", r,
           count_lines(c[r].text, NULL));
    }
  }
  if (cause < 1) {
    FAIL("REFUSED: no connect line names rank 2");
  }
  if (g_failed) {
    report("REFUSED", c, 2);
  }
}

static void case_blackhole(void) {
  reset_case();
  /* A listener whose accept queue one unaccepted connection fills. */
  int hole = listener_on(0, 0);
  int filler = hole >= 0 ? client_of(port_of(hole)) : -1;
  if (hole < 0 || filler < 0) {
    FAIL("BLACKHOLE: cannot make the full listener");
    return;
  }
  struct child_s c[1];
  uint64_t t0 = now_ns();
  struct rank_opts_s o = opts(2, 1, t0 + 2500000000ull);
  o.port[0] = port_of(hole);
  spawn(&c[0], &o);
  wait_all(c, 1, 2, 5000000000ull);
  double ms = ms_since(t0, c[0].ended_ns);
  if (c[0].status != 145 || ms < 2400.0 || ms > 3000.0 ||
      count_lines(c[0].text, "phase=connect missing=0 ") != 1) {
    FAIL("BLACKHOLE: ended %d at %.0f ms", c[0].status, ms);
    report("BLACKHOLE", c, 1);
  }
  if (g_shm->connects[1] < 2) {
    FAIL("BLACKHOLE: %d connect(s), want at least 2", g_shm->connects[1]);
  }
  close(filler);
  close(hole);
}

/* Whether `fd` reads end-of-file within `ms`. */
static bool reads_eof(int fd, int ms) {
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  char b;
  return poll(&pfd, 1, ms) == 1 && recv(fd, &b, 1, MSG_DONTWAIT) == 0;
}

/* STRAY, NOT_ABOVE, STRAY_SILENT, EOF_BEFORE_HELLO: a client of rank 0's port
 * before rank 1 starts; at_once: rank 0 closes it before rank 1 starts. */
static void case_stray(const char *name, const void *bytes, size_t len,
                       bool close_it, bool at_once) {
  reset_case();
  struct child_s c[2];
  uint64_t d = now_ns() + 1000000000ull;
  struct rank_opts_s o0 = opts(2, 0, d);
  spawn(&c[0], &o0);
  int stray = client_of(g_base);
  if (stray < 0) {
    FAIL("%s: rank 0 never listened", name);
    wait_all(c, 1, 2, 2000000000ull);
    return;
  }
  if (len > 0 && write(stray, bytes, len) != (ssize_t)len) {
    FAIL("%s: the stray's write", name);
  }
  if (close_it) {
    close(stray);
    stray = -1;
  }
  if (at_once && !reads_eof(stray, 500)) {
    FAIL("%s: rank 0 did not close the stray at once", name);
  }
  struct timespec settle = {0, 50000000L};
  nanosleep(&settle, NULL);
  struct rank_opts_s o1 = opts(2, 1, d);
  spawn(&c[1], &o1);
  if (stray >= 0 && !at_once) {
    /* The formation closes the stray when it completes, before the ranks
     * are released. */
    uint64_t until = now_ns() + 1500000000ull;
    while (atomic_load_explicit(&g_shm->formed, memory_order_relaxed) < 2 &&
           now_ns() < until) {
      struct timespec tick = {0, 2000000L};
      nanosleep(&tick, NULL);
    }
    if (!reads_eof(stray, 500)) {
      FAIL("%s: the stray did not read end-of-file once the ranks formed",
           name);
    }
  }
  wait_all(c, 2, 2, 3000000000ull);
  for (int r = 0; r < 2; r++) {
    if (c[r].status != 0 || count_lines(c[r].text, NULL) != 0) {
      FAIL("%s: rank %d ended %d", name, r, c[r].status);
      report(name, c, 2);
      break;
    }
  }
  if (stray >= 0) {
    close(stray);
  }
}

/* HELD_RANK: a second hello naming rank 1 reaches rank 0 while rank 0, which
 * holds rank 1's connection, still waits for rank 2. */
static void case_held_rank(void) {
  reset_case();
  struct child_s c[3];
  uint64_t d = now_ns() + 1000000000ull;
  for (unsigned int r = 0; r < 2; r++) {
    struct rank_opts_s o = opts(3, r, d);
    spawn(&c[r], &o);
  }
  /* Rank 1 connected to rank 0; its hello follows at once, and rank 0 takes
   * it in its next round. */
  uint64_t until = now_ns() + 500000000ull;
  while (g_shm->connects[1] < 1 && now_ns() < until) {
    struct timespec tick = {0, 1000000L};
    nanosleep(&tick, NULL);
  }
  struct timespec settle = {0, 200000000L};
  nanosleep(&settle, NULL);
  int stray = client_of(g_base);
  struct arts_mesh_hello_s hello = {.magic = ARTS_MESH_HELLO_MAGIC, .rank = 1};
  if (stray < 0 ||
      write(stray, &hello, sizeof(hello)) != (ssize_t)sizeof(hello)) {
    FAIL("HELD_RANK: the stray could not write");
  } else if (!reads_eof(stray, 500)) {
    FAIL("HELD_RANK: rank 0 did not close the second hello naming rank 1");
  }
  struct rank_opts_s o2 = opts(3, 2, d);
  spawn(&c[2], &o2);
  wait_all(c, 3, 3, 3000000000ull);
  for (int r = 0; r < 3; r++) {
    if (c[r].status != 0 || count_lines(c[r].text, NULL) != 0) {
      FAIL("HELD_RANK: rank %d ended %d", r, c[r].status);
      report("HELD_RANK", c, 3);
      break;
    }
  }
  if (stray >= 0) {
    close(stray);
  }
}

static void case_early(void) {
  reset_case();
  struct child_s c[1];
  uint64_t d = now_ns() + 1000000000ull;
  struct rank_opts_s o0 = opts(2, 0, d);
  o0.read_early = true;
  spawn(&c[0], &o0);
  int fake = client_of(g_base);
  unsigned char out[8 + 32];
  struct arts_mesh_hello_s hello = {.magic = ARTS_MESH_HELLO_MAGIC, .rank = 1};
  memcpy(out, &hello, sizeof(hello));
  for (int i = 0; i < 32; i++) {
    out[8 + i] = (unsigned char)i;
  }
  if (fake < 0 || write(fake, out, sizeof(out)) != (ssize_t)sizeof(out)) {
    FAIL("EARLY: the fake rank 1 could not write");
  }
  wait_all(c, 1, 1, 3000000000ull);
  if (c[0].status != 0) {
    FAIL("EARLY: rank 0 ended %d", c[0].status);
    report("EARLY", c, 1);
  }
  if (fake >= 0) {
    close(fake);
  }
}

static void case_hangup(void) {
  reset_case();
  struct child_s c[1];
  uint64_t t0 = now_ns();
  struct rank_opts_s o0 = opts(3, 0, t0 + 1000000000ull);
  spawn(&c[0], &o0);
  int fake = client_of(g_base);
  struct arts_mesh_hello_s hello = {.magic = ARTS_MESH_HELLO_MAGIC, .rank = 1};
  uint64_t closed = now_ns();
  if (fake < 0 ||
      write(fake, &hello, sizeof(hello)) != (ssize_t)sizeof(hello)) {
    FAIL("HANGUP: the fake rank 1 could not write");
  }
  if (fake >= 0) {
    close(fake);
  }
  wait_all(c, 1, 3, 3000000000ull);
  if (c[0].status != 145 || ms_since(closed, c[0].ended_ns) > 500.0 ||
      count_lines(c[0].text, "phase=hangup missing=1 ") != 1) {
    FAIL("HANGUP: rank 0 ended %d %.0f ms after the close", c[0].status,
         ms_since(closed, c[0].ended_ns));
    report("HANGUP", c, 1);
  }
}

static void case_held(void) {
  reset_case();
  int held = listener_on(g_base, 4);
  if (held < 0) {
    FAIL("HELD: cannot hold rank 0's port");
    return;
  }
  struct child_s c[1];
  uint64_t t0 = now_ns();
  struct rank_opts_s o0 = opts(2, 0, t0 + 1000000000ull);
  spawn(&c[0], &o0);
  wait_all(c, 1, 2, 3000000000ull);
  double ms = ms_since(t0, c[0].ended_ns);
  if (c[0].status != 145 || ms < 900.0 || ms > 1500.0 ||
      count_lines(c[0].text, "phase=listen missing=- missing_hosts=- "
                             "more=0") != 1) {
    FAIL("HELD: rank 0 ended %d at %.0f ms", c[0].status, ms);
    report("HELD", c, 1);
  }
  close(held);
}

static void case_nofile(int which) {
  reset_case();
  const char *name = which == 1 ? "NOFILE_RAISE" : "NOFILE_REFUSE";
  int fake[3];
  struct rank_opts_s o3 = opts(4, 3, now_ns() + 1000000000ull);
  for (int i = 0; i < 3; i++) {
    fake[i] = listener_on(0, 8);
    o3.port[i] = fake[i] >= 0 ? port_of(fake[i]) : 1;
  }
  o3.nofile = which;
  struct child_s c[1];
  spawn(&c[0], &o3);
  wait_all(c, 1, 1, 3000000000ull);
  if (which == 1 && c[0].status != 0) {
    FAIL("%s: rank 3 ended %d", name, c[0].status);
    report(name, c, 1);
  }
  if (which == 2 &&
      (c[0].status != 71 ||
       strstr(c[0].text, "the bootstrap mesh needs ") == NULL ||
       strstr(c[0].text, " file descriptors and RLIMIT_NOFILE's hard limit "
                         "is ") == NULL)) {
    FAIL("%s: rank 3 ended %d", name, c[0].status);
    report(name, c, 1);
  }
  for (int i = 0; i < 3; i++) {
    if (fake[i] >= 0) {
      close(fake[i]);
    }
  }
}

static void case_no_line(const char *name, bool signalled) {
  reset_case();
  struct child_s c[1];
  uint64_t t0 = now_ns();
  struct rank_opts_s o0 = opts(2, 0, t0 + 1000000000ull);
  if (signalled) {
    o0.taken_from_ns = t0 + 200000000ull;
  } else {
    o0.take_lost = true;
  }
  spawn(&c[0], &o0);
  wait_all(c, 1, 2, 3000000000ull);
  if (c[0].status != 145 || count_lines(c[0].text, NULL) != 0) {
    FAIL("%s: rank 0 ended %d with %d line(s)", name, c[0].status,
         count_lines(c[0].text, NULL));
    report(name, c, 1);
  }
}

int main(void) {
  g_shm = mmap(NULL, sizeof(*g_shm), PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (g_shm == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  signal(SIGPIPE, SIG_IGN);
  g_base = ARTS_PORT_WINDOW_LO + ((unsigned int)getpid() % 1000u) * 8u;
  case_form3();
  case_late_listen();
  case_refused();
  case_blackhole();
  static const unsigned char ff[8] = {0xff, 0xff, 0xff, 0xff,
                                      0xff, 0xff, 0xff, 0xff};
  case_stray("STRAY", ff, sizeof(ff), false, true);
  struct arts_mesh_hello_s self = {.magic = ARTS_MESH_HELLO_MAGIC, .rank = 0};
  case_stray("NOT_ABOVE", &self, sizeof(self), false, true);
  case_held_rank();
  case_stray("STRAY_SILENT", NULL, 0, false, false);
  static const unsigned char three[3] = {1, 2, 3};
  case_stray("EOF_BEFORE_HELLO", three, sizeof(three), true, false);
  case_early();
  case_hangup();
  case_held();
  case_nofile(1);
  case_nofile(2);
  case_no_line("SIGNALLED", true);
  case_no_line("TAKE_LOST", false);
  if (g_failed) {
    printf("FAIL mesh_form: %d check(s) failed\n", g_failed);
    return 1;
  }
  printf("PASS mesh_form\n");
  return 0;
}
