// SPDX-License-Identifier: Apache-2.0
/*
 * socket_net_interface_remap — pure_unit (getifaddrs interposition + fork)
 *
 * TARGET: arts_transport_set_ip() in libs/src/core/transport/socket.c with
 * net_interface set.  A rank finds its routing-table row (the launcher's
 * ARTS_RANK, else the row a local interface carries), then moves the
 * bootstrap mesh onto the named interface by shifting every row by the one
 * offset from its own row to the interface's IPv4 address.
 *
 * PROPERTIES UNDER TEST (two rows, two hosts A and B, each with lo, a
 * "fakeeth0" carrying the rows' network and a "fakeib0" on another one):
 *   - the shift is taken from this rank's own row, on either rank, so both
 *     ranks compute the same table;
 *   - rows that already name the interface's network are left as they are;
 *   - a name prefix selects the interface as an exact name does;
 *   - launcher=local's all-127.0.0.1 table becomes the interface's address,
 *     and stays as it is when the interface is lo;
 *   - with no launcher rank, the rank is the row an interface carries;
 *   - three refusals, each one report and ARTS_ERROR's exit status 1: a
 *     net_interface that names no interface, an own row no local interface
 *     carries, and a loopback own row in a table with a row that is not
 *     loopback.
 *
 * HARNESS: getifaddrs/freeifaddrs are interposed (these definitions win over
 * libc at link time) to return the case's interface list.  The rows are
 * numeric IPs, which getaddrinfo resolves offline.  Each case runs
 * arts_transport_set_ip in a forked child whose stdout and stderr go to one
 * pipe; the child prints the rank and rows it ended with, and the parent
 * judges that line, or the refusal's exit status and text.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "arts/counter/counter.h"
#include "arts/runtime_state.h"
#include "arts/system/config.h"
#include "arts/transport/protocol.h"

#include "counter_stubs.h"

/* ----- extern stubs the included socket.c needs ----- */
struct arts_runtime_shared_s arts_node_info;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;
unsigned int arts_global_rank_id;
unsigned int arts_global_rank_count;

/* The runtime's abort exits with the code it is given. */
void arts_abort(uint8_t code) {
  (void)fflush(stdout);
  (void)fflush(stderr);
  _exit(code);
}
void *arts_malloc(size_t s) { return malloc(s); }
void *arts_calloc(size_t n, size_t s) { return calloc(n, s); }
void arts_free(void *p) { free(p); }
void arts_enter_shutdown_state(bool initiator) { (void)initiator; }
void arts_runtime_stop(void) {}
void arts_transport_dispatch_packet(struct arts_msg_header_s *packet) {
  (void)packet;
}

/* ----- getifaddrs interposition: the case's "name=a.b.c.d" list ----- */
#define MAX_IFS 4
static const char *const *g_ifs;
static struct ifaddrs g_ifa[MAX_IFS];
static struct sockaddr_in g_sa[MAX_IFS];
static char g_names[MAX_IFS][16];

int getifaddrs(struct ifaddrs **ifap) {
  int n = 0;
  for (; g_ifs[n] != NULL; n++) {
    const char *eq = strchr(g_ifs[n], '=');
    memset(&g_ifa[n], 0, sizeof(g_ifa[n]));
    memset(&g_sa[n], 0, sizeof(g_sa[n]));
    snprintf(g_names[n], sizeof(g_names[n]), "%.*s", (int)(eq - g_ifs[n]),
             g_ifs[n]);
    g_sa[n].sin_family = AF_INET;
    inet_pton(AF_INET, eq + 1, &g_sa[n].sin_addr);
    g_ifa[n].ifa_name = g_names[n];
    g_ifa[n].ifa_addr = (struct sockaddr *)&g_sa[n];
    if (n > 0) {
      g_ifa[n - 1].ifa_next = &g_ifa[n];
    }
  }
  *ifap = (n > 0) ? &g_ifa[0] : NULL;
  return 0;
}

void freeifaddrs(struct ifaddrs *ifa) { (void)ifa; /* static storage */ }

/* pull in the real implementation */
#include "../../libs/src/core/transport/socket.c"

static const char *const HOST_A[] = {"lo=127.0.0.1", "fakeeth0=10.99.0.1",
                                     "fakeib0=10.98.0.1", NULL};
static const char *const HOST_B[] = {"lo=127.0.0.1", "fakeeth0=10.99.0.2",
                                     "fakeib0=10.98.0.2", NULL};

struct remap_case_s {
  const char *name;
  const char *const *ifs;
  const char *row0;
  const char *row1;
  const char *arts_rank; /* NULL: the rank comes from the interface walk */
  const char *net_interface;
  /* Accepted: the line the child prints.  Refused: NULL. */
  const char *want_result;
  /* Refused: a fragment of the one report. */
  const char *want_refusal;
};

static const struct remap_case_s CASES[] = {
    {"own-row offset, rank 0", HOST_A, "10.99.0.1", "10.99.0.2", "0", "fakeib0",
     "RESULT ok=1 rank=0 rows=10.98.0.1,10.98.0.2", NULL},
    {"own-row offset, rank 1", HOST_B, "10.99.0.1", "10.99.0.2", "1", "fakeib0",
     "RESULT ok=1 rank=1 rows=10.98.0.1,10.98.0.2", NULL},
    {"rows already on the interface's network", HOST_A, "10.98.0.1",
     "10.98.0.2", "0", "fakeib0", "RESULT ok=1 rank=0 rows=10.98.0.1,10.98.0.2",
     NULL},
    {"interface named by prefix", HOST_A, "10.99.0.1", "10.99.0.2", "0",
     "fakeib", "RESULT ok=1 rank=0 rows=10.98.0.1,10.98.0.2", NULL},
    {"launcher=local all-loopback table", HOST_A, "127.0.0.1", "127.0.0.1", "1",
     "fakeib0", "RESULT ok=1 rank=1 rows=10.98.0.1,10.98.0.1", NULL},
    {"all-loopback table onto lo", HOST_A, "127.0.0.1", "127.0.0.1", "0", "lo",
     "RESULT ok=1 rank=0 rows=127.0.0.1,127.0.0.1", NULL},
    {"no launcher rank: the interface walk", HOST_B, "10.99.0.1", "10.99.0.2",
     NULL, "fakeib0", "RESULT ok=1 rank=1 rows=10.98.0.1,10.98.0.2", NULL},
    {"no interface of that name", HOST_A, "10.99.0.1", "10.99.0.2", "0",
     "nosuch0", NULL, "no interface of that name or prefix has an IPv4"},
    {"own row no interface carries", HOST_A, "10.97.0.1", "10.97.0.2", "0",
     "fakeib0", NULL, "no local interface carries this rank's routing-table"},
    {"loopback own row, peer row not loopback", HOST_A, "127.0.0.1",
     "10.99.0.2", "0", "fakeib0", NULL,
     "is a loopback address while node 1's (10.99.0.2) is not"},
};

static void run_child(const struct remap_case_s *c) {
  g_ifs = c->ifs;
  if (c->arts_rank != NULL) {
    setenv("ARTS_RANK", c->arts_rank, 1);
  } else {
    unsetenv("ARTS_RANK");
  }
  unsetenv("SLURM_PROCID");

  static struct arts_config_table_s table[2];
  static unsigned int p0[1] = {25000};
  static unsigned int p1[1] = {25001};
  memset(table, 0, sizeof(table));
  table[0].rank = 0;
  table[0].ip_address = (char *)c->row0;
  table[0].ports = p0;
  table[1].rank = 1;
  table[1].ip_address = (char *)c->row1;
  table[1].ports = p1;

  static struct arts_config_s cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.table_length = 2;
  cfg.port_count = 1;
  cfg.nodes = 2;
  cfg.net_interface = (char *)c->net_interface;
  cfg.table = table;

  arts_transport_set_config(&cfg);
  bool ok = arts_transport_set_ip(&cfg);
  printf("RESULT ok=%d rank=%u rows=%s,%s\n", ok ? 1 : 0, cfg.my_rank, ip_list,
         ip_list + 100);
  (void)fflush(stdout);
  _exit(0);
}

static size_t count_of(const char *hay, const char *needle) {
  size_t n = 0;
  for (const char *p = strstr(hay, needle); p != NULL;
       p = strstr(p + 1, needle)) {
    n++;
  }
  return n;
}

/* Returns true when the case behaved as specified. */
static bool run_case(const struct remap_case_s *c) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    perror("pipe");
    return false;
  }
  (void)fflush(stdout);
  (void)fflush(stderr);
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return false;
  }
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    run_child(c);
    _exit(99); /* unreachable */
  }
  close(pipefd[1]);

  static char out[16384];
  size_t len = 0;
  for (;;) {
    ssize_t r = read(pipefd[0], out + len, sizeof(out) - 1 - len);
    if (r < 0 && errno == EINTR) {
      continue;
    }
    if (r <= 0 || len + (size_t)r >= sizeof(out) - 1) {
      if (r > 0) {
        len += (size_t)r;
      }
      break;
    }
    len += (size_t)r;
  }
  out[len] = '\0';
  close(pipefd[0]);

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid");
    return false;
  }
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

  bool pass;
  if (c->want_result != NULL) {
    pass = code == 0 && strstr(out, c->want_result) != NULL &&
           count_of(out, "[ERROR]") == 0;
  } else {
    pass = code == 1 && strstr(out, "RESULT") == NULL &&
           count_of(out, "[ERROR]") == 1 &&
           strstr(out, c->want_refusal) != NULL;
  }
  if (!pass) {
    fprintf(stderr,
            "FAIL socket_net_interface_remap: %s: expected %s \"%s\", got "
            "%s %d with output:\n%s\n",
            c->name, c->want_result != NULL ? "exit 0 and" : "exit 1 and",
            c->want_result != NULL ? c->want_result : c->want_refusal,
            WIFEXITED(status) ? "exit" : "signal",
            WIFEXITED(status) ? code : WTERMSIG(status), out);
  }
  return pass;
}

int main(void) {
  size_t n = sizeof(CASES) / sizeof(CASES[0]);
  size_t failed = 0;
  for (size_t i = 0; i < n; i++) {
    if (!run_case(&CASES[i])) {
      failed++;
    }
  }
  if (failed != 0) {
    fprintf(stderr, "FAIL socket_net_interface_remap: %zu of %zu cases\n",
            failed, n);
    return 1;
  }
  printf("PASS socket_net_interface_remap (%zu cases)\n", n);
  return 0;
}
