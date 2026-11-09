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
#include "arts/transport/socket.h"

#include "arts/utils/stdatomic_compat.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h> /* TCP_KEEP* */
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "arts.h"
#include "arts/runtime_state.h"
#include "arts/system/config.h"
#include "arts/system/print.h"
#include "arts/system/shutdown.h"
#include "arts/system/signals.h"
#include "arts/system/threads.h"
#include "arts/transport/dispatcher.h"
#include "arts/transport/netif.h"
#include "arts/transport/protocol.h"
#include "arts/utils/malloc.h"

/* Keepalive on both ends of every established mesh connection.  An
 * acknowledgement from the peer, its answer to this end's own probe included,
 * restarts the idle clock, and four unanswered probes 10 s apart drop the
 * connection 70 s after the peer's last answer, which arts_socket_peer_gone
 * then reports: a host that died silently is a peer gone in bounded time. */
static void socket_set_keepalive(int fd) {
  static const struct {
    int level;
    int name;
    int value;
    const char *what;
  } opts[] = {{SOL_SOCKET, SO_KEEPALIVE, 1, "SO_KEEPALIVE"},
              {IPPROTO_TCP, TCP_KEEPIDLE, 30, "TCP_KEEPIDLE"},
              {IPPROTO_TCP, TCP_KEEPINTVL, 10, "TCP_KEEPINTVL"},
              {IPPROTO_TCP, TCP_KEEPCNT, 4, "TCP_KEEPCNT"}};
  for (size_t i = 0; i < sizeof(opts) / sizeof(opts[0]); i++) {
    if (setsockopt(fd, opts[i].level, opts[i].name, &opts[i].value,
                   sizeof(opts[i].value)) != 0) {
      ARTS_ERROR("setsockopt(%s) on a mesh connection: %s", opts[i].what,
                 strerror(errno));
    }
  }
}

struct arts_config_s *arts_global_message_table;
int *arts_socket_fd;

/* The formation's listener and the accepted connections whose hello is not
 * whole yet: the main thread's alone, and closed when the formation ends. */
struct mesh_pending_s {
  int fd;
  unsigned int got;
  unsigned char hello[sizeof(struct arts_mesh_hello_s)];
};
static int g_mesh_listener = -1;
static struct mesh_pending_s *g_mesh_pending;
static unsigned int g_mesh_pending_n;
static unsigned int g_mesh_pending_cap;

/* The sentinels' poll set, one entry per peer: built by the main thread before
 * the arming store that publishes it, then touched by one prober at a time
 * (g_sentinel_probing). */
static struct pollfd *g_sentinel_pfd;
static unsigned int g_sentinel_n;

static char *ip_list;

/* Set once, by the main thread as the last act of arts_socket_sentinel_arm
 * (release), and read by any thread through arts_socket_armed (acquire).  Its
 * store publishes everything the main thread wrote before the arm: the
 * fabric's peer table and endpoint, this rank's id and the mesh's arrays and
 * descriptors. */
static _Atomic(bool) g_socket_armed;

void arts_transport_set_config(struct arts_config_s *config) {
  arts_global_message_table = config;
}
/* Resolve `host_name` to a printable IPv4 address in `ip` (>= 100 bytes).
 * Returns 0, or getaddrinfo's error code; the caller decides what a failure
 * means. */
static int hostname_to_ip(const char *host_name, char *ip) {
  struct addrinfo *result;
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET; // Force IPv4 - inet_addr() doesn't handle IPv6
  int error = getaddrinfo(host_name, NULL, &hints, &result);
  if (error == 0) {
    struct sockaddr_in *res = (struct sockaddr_in *)result->ai_addr;
    inet_ntop(AF_INET, &res->sin_addr, ip, 100);
    freeaddrinfo(result);
  }
  return error;
}

/* The routing-table row whose resolved address one of this host's IPv4
 * interfaces carries: `prefer` when an interface carries it, else the first
 * row one does (interfaces in the kernel's order), else -1. */
static int local_row(const struct arts_config_s *config, int prefer) {
  struct ifaddrs *ifap = NULL;
  if (getifaddrs(&ifap) != 0) {
    ARTS_ERROR("getifaddrs: %s", strerror(errno));
  }
  int first = -1;
  for (struct ifaddrs *ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
    /* An interface with no address has no ifa_addr. */
    if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    char addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, addr,
              sizeof(addr));
    for (int i = 0; i < (int)config->table_length; i++) {
      if (strcmp(addr, ip_list + ((ptrdiff_t)100 * i)) != 0) {
        continue;
      }
      if (i == prefer) {
        freeifaddrs(ifap);
        return i;
      }
      if (first < 0) {
        first = i;
      }
    }
  }
  freeifaddrs(ifap);
  return first;
}

/* This rank's row: the launcher's word where it gives one (the rank the
 * spawner passes in ARTS_RANK, a flux or Slurm task rank), else the row whose
 * resolved address a local interface carries. */
static bool set_my_rank(struct arts_config_s *config) {
  char *arts_rank_env = getenv("ARTS_RANK");
  if (arts_rank_env) {
    arts_global_rank_id = (unsigned int)strtol(arts_rank_env, NULL, 10);
    if (arts_global_rank_id >= config->table_length) {
      ARTS_ERROR("ARTS_RANK=%u exceeds the %u-node routing table",
                 arts_global_rank_id, config->table_length);
    }
    config->my_rank = arts_global_rank_id;
    arts_global_rank_count = config->table_length;
    return true;
  }

  /* flux: the declared launcher wins over any leaked wrapper-scheduler
   * variables (a flux instance can itself run inside another scheduler's
   * allocation, where every task inherits its broker's rank variables),
   * so this branch is gated on the launcher and sits ahead of the SLURM
   * one.  The launcher setup already required the flux task environment. */
  if (config->launcher != NULL && strcmp(config->launcher, "flux") == 0) {
    char *flux_rank = getenv("FLUX_TASK_RANK");
    if (flux_rank) {
      arts_global_rank_id = (unsigned int)strtol(flux_rank, NULL, 10);
      /* One rank per node is the launch contract (see the SLURM branch
       * below for why extra tasks are unusable). */
      if (arts_global_rank_id >= config->table_length) {
        ARTS_ERROR("task rank %u exceeds the %u-node routing table — the "
                   "launcher must start exactly one task per node "
                   "(flux run -N<n> -n<n>)",
                   arts_global_rank_id, config->table_length);
      }
      /* The routing table was built from the job hostlist in list order,
       * and the mesh treats row r as rank r — which assumes the launcher
       * assigns task ranks in that same order (block task map, one task
       * per node).  Verify by address: this host must carry row r's
       * address.  Carrying a DIFFERENT row's address instead is an
       * unambiguous ordering violation and fails now, rather than as an
       * unattributable connect timeout; carrying none of them is only
       * ambiguity (NAT, a host name that resolves to an address no
       * interface carries) and warns, except with net_interface set, where
       * the shift onto the interface refuses a rank whose own row this host
       * does not carry. */
      unsigned int r = arts_global_rank_id;
      int row = local_row(config, (int)r);
      if (row != (int)r) {
        if (row >= 0) {
          ARTS_ERROR("flux task rank %u runs on the host of routing-table "
                     "row %d — task ranks do not follow the job hostlist "
                     "order this table was built in",
                     r, row);
        }
        if (config->net_interface == NULL) {
          ARTS_WARN("flux: no local interface carries the address of "
                    "routing-table row %u — task placement cannot be "
                    "verified on this host",
                    r);
        }
      }
      config->my_rank = arts_global_rank_id;
      arts_global_rank_count = config->table_length;
      return true;
    }
  }

  // SLURM: use SLURM_PROCID for rank (srun sets this per task)
  // IP matching fails when all nodes resolve to the same address (e.g., WSL2)
  char *task_rank = getenv("SLURM_PROCID");
  if (task_rank) {
    arts_global_rank_id = (unsigned int)strtol(task_rank, NULL, 10);
    /* One rank per node is the launch contract: the routing table has one
     * row per host and every rank listens on the same port, so a launcher
     * that starts more tasks than nodes hands the extras a rank with no
     * table row — and any same-host pair a guaranteed port collision.  Fail
     * with the contract spelled out instead of indexing the table out of
     * bounds. */
    if (arts_global_rank_id >= config->table_length) {
      ARTS_ERROR("task rank %u exceeds the %u-node routing table — the "
                 "launcher must start exactly one task per node "
                 "(--ntasks-per-node=1)",
                 arts_global_rank_id, config->table_length);
    }
    config->my_rank = arts_global_rank_id;
    arts_global_rank_count = config->table_length;
    return true;
  }

  int row = local_row(config, -1);
  if (row < 0) {
    return false;
  }
  config->my_rank = (unsigned int)row;
  arts_global_rank_id = (unsigned int)row;
  arts_global_rank_count = config->table_length;
  return true;
}

static bool row_is_loopback(int row) {
  struct in_addr addr;
  inet_pton(AF_INET, ip_list + ((ptrdiff_t)100 * row), &addr);
  return (ntohl(addr.s_addr) >> IN_CLASSA_NSHIFT) == IN_LOOPBACKNET;
}

/* Move the bootstrap mesh onto net_interface by shifting every row by one
 * offset, from this rank's own row to the interface's address.  That holds
 * when the interface's network numbers the hosts as the resolved rows do: a
 * row that already names that network stays put, and under launcher=local,
 * where every row is 127.0.0.1, every row becomes the interface's address. */
static void remap_rows_to_interface(struct arts_config_s *config) {
  struct sockaddr_in iface_sa;
  if (!arts_netif_ipv4_addr(config->net_interface, &iface_sa)) {
    ARTS_ERROR("net_interface=%s: no interface of that name or prefix has an "
               "IPv4 address — refusing to fall back to the default route",
               config->net_interface);
  }
  /* The offset is taken from this rank's own row, so that row must be an
   * address this host carries; a host name that resolves elsewhere (a
   * loopback alias, NAT) would shift every peer by a meaningless amount. */
  if (local_row(config, (int)config->my_rank) != (int)config->my_rank) {
    ARTS_ERROR("net_interface=%s: no local interface carries this rank's "
               "routing-table address %s ('%s'), so the offset onto the "
               "interface cannot be computed — name the nodes by an "
               "address this host carries",
               config->net_interface,
               ip_list + ((ptrdiff_t)100 * config->my_rank),
               config->table[config->my_rank].ip_address);
  }
  char my_ip[INET_ADDRSTRLEN];
  char iface_ip[INET_ADDRSTRLEN];
  struct in_addr my_addr;
  inet_pton(AF_INET, ip_list + ((ptrdiff_t)100 * config->my_rank), &my_addr);
  inet_ntop(AF_INET, &my_addr, my_ip, sizeof(my_ip));
  inet_ntop(AF_INET, &iface_sa.sin_addr, iface_ip, sizeof(iface_ip));
  uint32_t offset = ntohl(iface_sa.sin_addr.s_addr) - ntohl(my_addr.s_addr);
  if (offset == 0) {
    ARTS_INFO("net_interface=%s: this rank's row already names it (%s)",
              config->net_interface, iface_ip);
    return;
  }
  /* A loopback address names this host on no network, so an offset taken
   * from one moves only rows that are loopback too: the all-loopback table
   * of a one-host run. */
  if (row_is_loopback((int)config->my_rank)) {
    for (int i = 0; i < (int)config->table_length; i++) {
      if (!row_is_loopback(i)) {
        ARTS_ERROR("net_interface=%s: this rank's routing-table address %s "
                   "('%s') is a loopback address while node %d's (%s) is "
                   "not, so the offset onto the interface says nothing "
                   "about where the other hosts are — name the nodes by "
                   "their addresses on the network",
                   config->net_interface, my_ip,
                   config->table[config->my_rank].ip_address, i,
                   ip_list + ((ptrdiff_t)100 * i));
      }
    }
  }
  ARTS_INFO("net_interface=%s: remapping IPs (%s -> %s)",
            config->net_interface, my_ip, iface_ip);
  for (int i = 0; i < (int)config->table_length; i++) {
    char *row_ip = ip_list + ((ptrdiff_t)100 * i);
    struct in_addr addr;
    char old_ip[INET_ADDRSTRLEN];
    inet_pton(AF_INET, row_ip, &addr);
    inet_ntop(AF_INET, &addr, old_ip, sizeof(old_ip));
    addr.s_addr = htonl(ntohl(addr.s_addr) + offset);
    inet_ntop(AF_INET, &addr, row_ip, 100);
    ARTS_INFO("  node %d: %s -> %s", i, old_ip, row_ip);
  }
}

bool arts_transport_set_ip(struct arts_config_s *config) {
  ip_list = (char *)arts_malloc(100 * sizeof(char) * config->table_length);
  bool *failed = (bool *)arts_calloc(config->table_length, sizeof(bool));
  bool any = false;
  for (int i = 0; i < (int)config->table_length; i++) {
    int rc = hostname_to_ip(config->table[i].ip_address,
                            ip_list + ((ptrdiff_t)100 * i));
    if (rc != 0) {
      ARTS_WARN("cannot resolve node '%s': %s", config->table[i].ip_address,
                gai_strerror(rc));
      failed[i] = true;
      any = true;
    }
  }
  if (any) {
    arts_mesh_fail(ARTS_MESH_PHASE_RESOLVE, failed);
  }
  arts_free(failed);
  /* The rank comes from the rows as resolved: the net_interface shift is
   * computed from this rank's own row. */
  if (!set_my_rank(config)) {
    return false;
  }
  if (config->net_interface != NULL) {
    remap_rows_to_interface(config);
  }
  return true;
}

void arts_socket_setup(struct arts_config_s *config) {
  arts_transport_set_config(config);

  if (!arts_transport_set_ip(config) && config->nodes > 1) {
    ARTS_ERROR("no local IPv4 interface carries the address of any "
               "routing-table row, and the launcher named no rank — cannot "
               "tell which row is this rank");
  }
}

/* Claim-and-release probe of one wildcard listen port, with the same options
 * the real listen socket is created with, so its verdict matches what bind()
 * will do there.  Availability is a point-in-time observation, never a
 * reservation: the port is released again before this returns. */
static bool port_is_bindable(unsigned int port) {
  int fd = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return false;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);

  bool free_port = bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
  close(fd);
  return free_port;
}

/* Lowest port the kernel hands out as an outgoing connection's source port.  A
 * listen port at or above it collides at random with any connection the machine
 * makes, so the search below never crosses it. */
static unsigned int ephemeral_port_floor(void) {
  unsigned int low = 32768;
  unsigned int high;
  FILE *f = fopen("/proc/sys/net/ipv4/ip_local_port_range", "r");
  if (f) {
    if (fscanf(f, "%u %u", &low, &high) != 2) {
      low = 32768;
    }
    fclose(f);
  }
  return low;
}

/* Sliding the port block is the spawner's job, and only on a local run: a
 * bind probe here observes this machine, which for a local run is where every
 * rank lives.  A rank the spawner started already carries the answer in its
 * environment and must not choose again: two ranks choosing separately would
 * disagree about each other's ports. */
static bool this_rank_chooses_port(const struct arts_config_s *config) {
  return config->launcher != NULL && strcmp(config->launcher, "local") == 0 &&
         config->table_length > 1 && config->table != NULL &&
         config->port != 0 && getenv(ARTS_RESOLVED_PORT_ENV) == NULL;
}

bool arts_transport_select_local_port(struct arts_config_s *config) {
  if (!this_rank_chooses_port(config)) {
    return true;
  }

  const unsigned int span = config->table_length;
  const unsigned int seed = config->port;

  /* Candidates walk up from the seed and wrap back to the bottom of the
   * window, so a seed near the top still sees the whole window.  Stepping by
   * the span keeps successive candidates from overlapping. */
  unsigned int ceiling = ephemeral_port_floor();
  if (ceiling > ARTS_PORT_WINDOW_HI) {
    ceiling = ARTS_PORT_WINDOW_HI;
  }
  if (ceiling < ARTS_PORT_WINDOW_LO + span) {
    ARTS_WARN("Local multi-node: the port window %u-%u cannot hold %u ports; "
              "keeping %u and letting bind contend",
              ARTS_PORT_WINDOW_LO, ceiling, span, seed);
    return false;
  }
  const unsigned int last_start = ceiling - span;
  const unsigned int candidates = (last_start - ARTS_PORT_WINDOW_LO) / span + 1;

  unsigned int base = seed > last_start ? ARTS_PORT_WINDOW_LO : seed;
  for (unsigned int tried = 0; tried < candidates; tried++) {
    bool all_free = true;
    for (unsigned int rank = 0; rank < span && all_free; rank++) {
      all_free = port_is_bindable(base + rank);
    }

    if (all_free) {
      if (base != seed) {
        ARTS_WARN("Local multi-node: port block at %u is occupied, taking %u "
                  "instead (%u ports)",
                  seed, base, span);
      }
      config->port = base;
      for (unsigned int rank = 0; rank < span; rank++) {
        config->table[rank].port = base + rank;
      }
      return true;
    }

    base = base + span > last_start ? ARTS_PORT_WINDOW_LO : base + span;
  }

  ARTS_WARN("Local multi-node: no free block of %u ports in %u-%u; keeping %u "
            "and letting bind contend",
            span, ARTS_PORT_WINDOW_LO, ceiling, seed);
  return false;
}

void arts_socket_sentinel_arm(void) {
  /* No payload rides the mesh again, but its connections stay open as
   * payload-free liveness sentinels: closing one here would signal a false
   * death to its peer. */
  const struct arts_config_s *config = arts_global_message_table;
  if (config == NULL) {
    return;
  }
  g_sentinel_pfd =
      (struct pollfd *)arts_calloc(config->table_length, sizeof(struct pollfd));
  g_sentinel_n = 0;
  for (unsigned int p = 0; p < config->table_length; p++) {
    if (p != config->my_rank) {
      g_sentinel_pfd[g_sentinel_n].fd = arts_socket_fd[p];
      g_sentinel_pfd[g_sentinel_n].events = POLLIN;
      g_sentinel_n++;
    }
  }
  atomic_store_explicit(&g_socket_armed, true, memory_order_release);
}

bool arts_socket_armed(void) {
  return atomic_load_explicit(&g_socket_armed, memory_order_acquire);
}

bool arts_socket_peer_gone(unsigned int rank) {
  if (!arts_socket_armed() || rank == arts_global_rank_id) {
    return false;
  }
  int fd = arts_socket_fd[rank];
  struct pollfd pfd = {.fd = fd, .events = POLLRDHUP};
  if (poll(&pfd, 1, 0) > 0 &&
      (pfd.revents & (POLLRDHUP | POLLHUP | POLLERR)) != 0) {
    return true;
  }
  /* No byte rides this connection toward this rank, so none can hide an end
   * of file from the peek.  A pending error the peek returns is cleared, but
   * the end that raised it left the connection closed, so every later poll
   * reports POLLHUP and every later peek reads 0. */
  char b;
  ssize_t r = recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
  return r == 0 || (r < 0 && (errno == ECONNRESET || errno == ETIMEDOUT ||
                              errno == EPIPE || errno == EHOSTUNREACH ||
                              errno == ENETUNREACH));
}

/* The sentinels' probe cadence on the coarse monotonic clock, whose
 * resolution may be coarser still: a due probe then waits for its tick. */
#define SOCKET_PROBE_INTERVAL_NS 1000000ull

bool arts_socket_probe_due(uint64_t *next_ns) {
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  if (now < *next_ns || !arts_socket_armed()) {
    return false;
  }
  *next_ns = now + SOCKET_PROBE_INTERVAL_NS;
  return true;
}

/* The peer whose connection entry i of the sentinels' poll set holds: the
 * set lists every peer in rank order, this rank left out. */
static unsigned int sentinel_rank(unsigned int i) {
  return i < arts_global_rank_id ? i : i + 1u;
}

/* Single prober at a time: two threads polling the shared pollfd array would
 * race on its revents fields.  A try-lock: the winning CAS acquires and the
 * return stores release, so each prober sees the entries the previous one
 * retired; a loser returns, and its next due probe tries again. */
static _Atomic(int) g_sentinel_probing;

bool arts_socket_sentinel_check(void) {
  /* Probe every peer's connection for its end (POLLHUP/POLLERR/EOF), with a
   * zero timeout: never blocking, and never reading data beyond draining the
   * end of file (no payload rides these sockets after the exchange).  Once
   * this rank's shutdown is in progress the probe stands down.  Nothing
   * orders any rank's close after every other rank has taken in a SHUTDOWN
   * post (the post is best effort and reaches only the peers its rank
   * contacted, and no rank waits for anyone before it tears down), so an
   * orderly teardown can reach this probe first too, and for a peer the post
   * skipped it is the only way to learn of the end; the caller enters the
   * same shutdown for both, so the probe need not tell them apart. */
  if (!atomic_load_explicit(&g_socket_armed, memory_order_acquire) ||
      g_sentinel_n == 0 || arts_shutdown_taken()) {
    return false;
  }
  int expected = 0;
  if (!atomic_compare_exchange_strong_explicit(&g_sentinel_probing, &expected,
                                               1, memory_order_acq_rel,
                                               memory_order_relaxed)) {
    return false;
  }
  bool dead_peer = false;
  if (poll(g_sentinel_pfd, (nfds_t)g_sentinel_n, 0) > 0) {
    for (unsigned int i = 0; i < g_sentinel_n && !dead_peer; i++) {
      short re = g_sentinel_pfd[i].revents;
      if (re == 0) {
        continue;
      }
      if (re & (POLLHUP | POLLERR | POLLNVAL)) {
        dead_peer = true;
      } else if (re & POLLIN) {
        char scratch[256];
        ssize_t r =
            recv(g_sentinel_pfd[i].fd, scratch, sizeof(scratch), MSG_DONTWAIT);
        if (r == 0) {
          dead_peer = true; /* EOF: peer closed its end */
        } else if (r > 0) {
          ARTS_WARN("liveness sentinel of rank %u received %zd unexpected "
                    "bytes (no data should ride the bootstrap mesh)",
                    sentinel_rank(i), (ssize_t)r);
        }
        /* r < 0 (EAGAIN/EINTR): spurious wakeup, ignore. */
      }
      if (dead_peer) {
        ARTS_INFO("liveness sentinel of rank %u: peer connection closed "
                  "before this rank's shutdown began",
                  sentinel_rank(i));
        g_sentinel_pfd[i].fd = -1; /* negative fd: poll ignores this entry */
      }
    }
  }
  atomic_store_explicit(&g_sentinel_probing, 0, memory_order_release);
  return dead_peer;
}

#ifdef ARTS_DEBUG_TEARDOWN
void arts_socket_cleanup(void) {
  /* Close of the liveness sentinels, in this rank's teardown.  Nothing
   * orders it after every other rank has taken in a SHUTDOWN post
   * (the post is best effort and reaches only the peers its rank contacted,
   * and no rank waits for anyone before it tears down), so a peer may still
   * observe the HUP before its own shutdown began; the thread whose probe
   * sees it enters the same idempotent shutdown. */
  const struct arts_config_s *config = arts_global_message_table;
  for (unsigned int p = 0;
       config != NULL && arts_socket_fd != NULL && p < config->table_length;
       p++) {
    if (arts_socket_fd[p] >= 0) {
      close(arts_socket_fd[p]);
    }
  }
  arts_free(ip_list);
  arts_free(arts_socket_fd);
  arts_free(g_sentinel_pfd);
}
#endif

/* The startup bound (socket.h), written by the main thread.  The budget, the
 * deadline and the node name are written by arts_startup_begin before the
 * startup timer's arming, which publishes them to its handler; the own table
 * name once, before the first store of a step at or past listen, which
 * publishes it. */
static unsigned int g_startup_budget_s;
static uint64_t g_startup_deadline_ns;
static char g_startup_node[256];
static char g_startup_host[256];
static bool g_startup_host_set;
/* The step the process is in: release stores by the main thread, acquire
 * loads by the startup timer's handler.  A store of a step at or past listen
 * publishes g_startup_host and the rank the signal handlers print. */
static_assert(ATOMIC_INT_LOCK_FREE == 2,
              "a signal handler reads an atomic unsigned int");
static _Atomic(unsigned int) g_startup_step;

#define STARTUP_NS_PER_S 1000000000ull

static const char *mesh_phase_name(enum arts_mesh_phase phase) {
  switch (phase) {
  case ARTS_MESH_PHASE_CONFIG:
    return "config";
  case ARTS_MESH_PHASE_RESOLVE:
    return "resolve";
  case ARTS_MESH_PHASE_LISTEN:
    return "listen";
  case ARTS_MESH_PHASE_CONNECT:
    return "connect";
  case ARTS_MESH_PHASE_FABRIC:
    return "fabric";
  case ARTS_MESH_PHASE_EXCHANGE:
    return "exchange";
  case ARTS_MESH_PHASE_HANGUP:
    return "hangup";
  }
  return "?";
}

/* A line under construction: a byte that would pass lim is not written, and
 * marks the line cut. */
struct mesh_line_s {
  char *buf;
  size_t n;
  size_t lim;
  bool cut;
};

static void mesh_put(struct mesh_line_s *l, const char *s) {
  for (; *s != '\0'; s++) {
    if (l->n == l->lim) {
      l->cut = true;
      return;
    }
    l->buf[l->n++] = *s;
  }
}

static void mesh_put_u(struct mesh_line_s *l, uint64_t v) {
  char num[24];
  char *d = num + sizeof(num) - 1;
  *d = '\0';
  do {
    *--d = (char)('0' + (v % 10u));
    v /= 10u;
  } while (v != 0u);
  mesh_put(l, d);
}

static void mesh_line_put(struct mesh_line_s *l, int rank, const char *host,
                          enum arts_mesh_phase phase, const bool *missing,
                          unsigned int n,
                          const struct arts_config_table_s *table,
                          unsigned int listed, unsigned int total) {
  mesh_put(l, "ARTS_MESH_FAILED rank=");
  if (rank < 0) {
    mesh_put(l, "-");
  } else {
    mesh_put_u(l, (uint64_t)rank);
  }
  mesh_put(l, " host=");
  mesh_put(l, host);
  mesh_put(l, " phase=");
  mesh_put(l, mesh_phase_name(phase));
  for (int hosts = 0; hosts < 2; hosts++) {
    mesh_put(l, hosts ? " missing_hosts=" : " missing=");
    unsigned int put = 0;
    for (unsigned int i = 0; i < n && put < listed; i++) {
      if (!missing[i]) {
        continue;
      }
      if (put++ > 0) {
        mesh_put(l, ",");
      }
      if (!hosts) {
        mesh_put_u(l, i);
      } else {
        mesh_put(l, table != NULL && table[i].ip_address != NULL
                        ? table[i].ip_address
                        : "?");
      }
    }
    if (put == 0) {
      mesh_put(l, "-");
    }
  }
  mesh_put(l, " more=");
  mesh_put_u(l, total - listed);
}

size_t arts_mesh_format_line(char *buf, size_t cap, int rank, const char *host,
                             enum arts_mesh_phase phase, const bool *missing,
                             unsigned int n,
                             const struct arts_config_table_s *table) {
  if (cap < 2) {
    if (cap == 1) {
      buf[0] = '\0';
    }
    return 0;
  }
  const size_t lim =
      cap - 1 < ARTS_MESH_LINE_MAX ? cap - 1 : ARTS_MESH_LINE_MAX;
  unsigned int total = 0;
  for (unsigned int i = 0; missing != NULL && i < n; i++) {
    total += missing[i] ? 1u : 0u;
  }
  unsigned int listed =
      total < ARTS_MESH_LINE_RANKS ? total : ARTS_MESH_LINE_RANKS;
  struct mesh_line_s l;
  for (;;) {
    l = (struct mesh_line_s){.buf = buf, .n = 0, .lim = lim - 1, .cut = false};
    mesh_line_put(&l, rank, host != NULL ? host : "-", phase, missing, n, table,
                  listed, total);
    if (!l.cut || listed == 0) {
      break;
    }
    listed--;
  }
  buf[l.n++] = '\n';
  buf[l.n] = '\0';
  return l.n;
}

size_t arts_startup_expired_line(char *buf, size_t cap) {
  const unsigned int step =
      atomic_load_explicit(&g_startup_step, memory_order_acquire);
  if (step >= ARTS_MESH_PHASE_LISTEN) {
    return arts_mesh_format_line(buf, cap, (int)arts_signal_handlers_rank(),
                                 g_startup_host, (enum arts_mesh_phase)step,
                                 NULL, 0, NULL);
  }
  return arts_mesh_format_line(buf, cap, -1, g_startup_node,
                               (enum arts_mesh_phase)step, NULL, 0, NULL);
}

static void startup_copy(char *to, size_t cap, const char *from) {
  size_t n = 0;
  for (; from != NULL && from[n] != '\0' && n < cap - 1; n++) {
    to[n] = from[n];
  }
  to[n] = '\0';
}

/* ASCII decimal digits naming a value in lo..hi. */
static bool startup_digits(const char *text, unsigned long lo, unsigned long hi,
                           unsigned long *out) {
  unsigned long v = 0;
  if (*text == '\0') {
    return false;
  }
  for (; *text != '\0'; text++) {
    if (*text < '0' || *text > '9') {
      return false;
    }
    v = (v * 10u) + (unsigned long)(*text - '0');
    if (v > hi) {
      return false;
    }
  }
  if (v < lo) {
    return false;
  }
  *out = v;
  return true;
}

#ifdef ARTS_DEBUG_INJECTION
enum startup_fault_kind {
  STARTUP_FAULT_NONE,
  STARTUP_FAULT_STALL,
  STARTUP_FAULT_EXIT,
  STARTUP_FAULT_DELAY,
};
#define STARTUP_FAULT_MS_MAX 600000ul
/* Parsed by arts_startup_begin and armed by arts_startup_config_done in the
 * rank it names; read by the main thread alone. */
static struct {
  enum startup_fault_kind kind;
  unsigned long rank;
  enum arts_mesh_phase step;
  unsigned long ms;
  bool armed;
} g_startup_fault;

static void startup_fault_parse(void) {
  const char *text = getenv("ARTS_MESH_FAULT");
  if (text == NULL || text[0] == '\0') {
    return;
  }
  char copy[128];
  char *field[5];
  int fields = 0;
  bool ok = strlen(text) < sizeof(copy);
  if (ok) {
    memcpy(copy, text, strlen(text) + 1);
    char *save = NULL;
    for (char *f = strtok_r(copy, ":", &save); f != NULL && fields < 5;
         f = strtok_r(NULL, ":", &save)) {
      field[fields++] = f;
    }
    ok = text[0] != ':' && text[strlen(text) - 1] != ':' &&
         strstr(text, "::") == NULL;
  }
  enum startup_fault_kind kind = STARTUP_FAULT_NONE;
  if (ok && fields == 3 && strcmp(field[1], "stall") == 0) {
    kind = STARTUP_FAULT_STALL;
  } else if (ok && fields == 3 && strcmp(field[1], "exit") == 0) {
    kind = STARTUP_FAULT_EXIT;
  } else if (ok && fields == 4 && strcmp(field[1], "delay") == 0) {
    kind = STARTUP_FAULT_DELAY;
  }
  bool step_ok = false;
  enum arts_mesh_phase step = ARTS_MESH_PHASE_CONFIG;
  for (int p = ARTS_MESH_PHASE_CONFIG;
       kind != STARTUP_FAULT_NONE && p <= ARTS_MESH_PHASE_EXCHANGE; p++) {
    if (strcmp(field[2], mesh_phase_name((enum arts_mesh_phase)p)) == 0) {
      step = (enum arts_mesh_phase)p;
      step_ok = true;
    }
  }
  unsigned long rank = 0;
  unsigned long ms = 0;
  if (!step_ok || !startup_digits(field[0], 0, UINT_MAX, &rank) ||
      (kind == STARTUP_FAULT_DELAY &&
       !startup_digits(field[3], 1, STARTUP_FAULT_MS_MAX, &ms))) {
    ARTS_ERROR("ARTS_MESH_FAULT=\"%s\" is not <rank>:stall:<step>, "
               "<rank>:exit:<step> or <rank>:delay:<step>:<ms> (a step is "
               "config, resolve, listen, connect, fabric or exchange; ms is "
               "1-%lu)",
               text, STARTUP_FAULT_MS_MAX);
  }
  g_startup_fault.kind = kind;
  g_startup_fault.rank = rank;
  g_startup_fault.step = step;
  g_startup_fault.ms = ms;
}

/* A hook the run cannot act on would leave it unchanged, and the run would
 * read as one that exercised the hook. */
static void startup_fault_check(const struct arts_config_s *config) {
  if (g_startup_fault.kind == STARTUP_FAULT_NONE) {
    return;
  }
  if (config->table_length <= 1) {
    ARTS_ERROR("ARTS_MESH_FAULT is set, but a one-rank run has no mesh for it "
               "to act on");
  }
  if (strcmp(config->launcher, "lsf") == 0) {
    ARTS_ERROR("ARTS_MESH_FAULT is set, but launcher=lsf names no rank for it "
               "to act on");
  }
  if (g_startup_fault.rank >= config->table_length) {
    ARTS_ERROR("ARTS_MESH_FAULT names rank %lu, but the run's ranks are 0-%u",
               g_startup_fault.rank, config->table_length - 1u);
  }
}

/* The hook acts only in the rank its launcher's own rank variable names. */
static void startup_fault_arm(const struct arts_config_s *config) {
  if (g_startup_fault.kind == STARTUP_FAULT_NONE) {
    return;
  }
  const char *var = "ARTS_RANK";
  if (strcmp(config->launcher, "flux") == 0) {
    var = "FLUX_TASK_RANK";
  } else if (strcmp(config->launcher, "slurm") == 0) {
    var = "SLURM_PROCID";
  }
  const char *text = getenv(var);
  unsigned long rank = 0;
  g_startup_fault.armed = text != NULL &&
                          startup_digits(text, 0, UINT_MAX, &rank) &&
                          rank == g_startup_fault.rank;
}

static void startup_fault_act(enum arts_mesh_phase step) {
  if (!g_startup_fault.armed || g_startup_fault.step != step) {
    return;
  }
  switch (g_startup_fault.kind) {
  case STARTUP_FAULT_STALL:
    for (;;) {
      (void)pause();
    }
  case STARTUP_FAULT_EXIT:
    arts_print_line("[ARTS] mesh fault: rank %lu exits at %s\n",
                    g_startup_fault.rank, mesh_phase_name(step));
    arts_exit(1, ARTS_EXIT_RUNTIME_ERROR);
  case STARTUP_FAULT_DELAY: {
    struct timespec left = {.tv_sec = (time_t)(g_startup_fault.ms / 1000u),
                            .tv_nsec =
                                (long)(g_startup_fault.ms % 1000u) * 1000000L};
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &left, &left) == EINTR) {
    }
    break;
  }
  case STARTUP_FAULT_NONE:
    break;
  }
}
#else
#ifndef NDEBUG
/* A hook's variable set where the hooks are not compiled would leave the run
 * unchanged, and the run would read as one that exercised the hook. */
static void startup_fault_parse(void) {
  const char *text = getenv("ARTS_MESH_FAULT");
  if (text != NULL && text[0] != '\0') {
    ARTS_ERROR("ARTS_MESH_FAULT is set, but this build compiles no injection "
               "hooks (configure it with -DARTS_DEBUG_INJECTION=ON)");
  }
}
#else
static void startup_fault_parse(void) {}
#endif
static void startup_fault_check(const struct arts_config_s *config) {
  (void)config;
}
static void startup_fault_arm(const struct arts_config_s *config) {
  (void)config;
}
static void startup_fault_act(enum arts_mesh_phase step) { (void)step; }
#endif

void arts_startup_begin(void) {
  const char *text = getenv("ARTS_STARTUP_TIMEOUT_S");
  unsigned long budget = ARTS_STARTUP_BUDGET_DEFAULT_S;
  if (text != NULL && !startup_digits(text, ARTS_STARTUP_BUDGET_MIN_S,
                                      ARTS_STARTUP_BUDGET_MAX_S, &budget)) {
    ARTS_ERROR("ARTS_STARTUP_TIMEOUT_S=\"%s\" is not a whole number of "
               "seconds in %u-%u",
               text, ARTS_STARTUP_BUDGET_MIN_S, ARTS_STARTUP_BUDGET_MAX_S);
  }
  g_startup_budget_s = (unsigned int)budget;
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  g_startup_deadline_ns = ((uint64_t)now.tv_sec * STARTUP_NS_PER_S) +
                          (uint64_t)now.tv_nsec +
                          ((uint64_t)budget * STARTUP_NS_PER_S);
  struct utsname name;
  startup_copy(g_startup_node, sizeof(g_startup_node),
               uname(&name) == 0 ? name.nodename : "?");
  startup_fault_parse();
  atomic_store_explicit(&g_startup_step, (unsigned int)ARTS_MESH_PHASE_CONFIG,
                        memory_order_release);
  arts_exit_startup_arm(g_startup_deadline_ns + STARTUP_NS_PER_S);
}

unsigned int arts_startup_budget_s(void) { return g_startup_budget_s; }

uint64_t arts_startup_deadline_ns(void) { return g_startup_deadline_ns; }

void arts_startup_step(enum arts_mesh_phase step) {
  if (step >= ARTS_MESH_PHASE_LISTEN && !g_startup_host_set &&
      arts_global_message_table != NULL) {
    const struct arts_config_s *config = arts_global_message_table;
    startup_copy(g_startup_host, sizeof(g_startup_host),
                 config->table[config->my_rank].ip_address);
    g_startup_host_set = true;
  }
  atomic_store_explicit(&g_startup_step, (unsigned int)step,
                        memory_order_release);
  startup_fault_act(step);
}

void arts_startup_check_run(const struct arts_config_s *config) {
  startup_fault_check(config);
}

void arts_startup_config_done(const struct arts_config_s *config) {
  startup_fault_arm(config);
  startup_fault_act(ARTS_MESH_PHASE_CONFIG);
  if (config->table_length <= 1) {
    arts_startup_end();
  }
}

void arts_startup_end(void) { arts_exit_startup_disarm(); }

/* Closes every mesh socket this rank holds, the listeners included, so each
 * peer sees its connection end. */
static void mesh_close_all(void) {
  if (g_mesh_listener >= 0) {
    close(g_mesh_listener);
    g_mesh_listener = -1;
  }
  for (unsigned int i = 0; i < g_mesh_pending_n; i++) {
    close(g_mesh_pending[i].fd);
  }
  g_mesh_pending_n = 0;
  const struct arts_config_s *config = arts_global_message_table;
  for (unsigned int p = 0;
       config != NULL && arts_socket_fd != NULL && p < config->table_length;
       p++) {
    if (arts_socket_fd[p] >= 0) {
      close(arts_socket_fd[p]);
      arts_socket_fd[p] = -1;
    }
  }
}

/* One write, made only if stderr takes it within a second: a reader that
 * stopped draining must not hold the rank's end. */
static void mesh_write_line(const char *line, size_t n) {
  struct pollfd err = {.fd = STDERR_FILENO, .events = POLLOUT};
  if (poll(&err, 1, 1000) == 1 && (err.revents & POLLOUT) != 0) {
    (void)write(STDERR_FILENO, line, n);
  }
}

_Noreturn void arts_mesh_fail(enum arts_mesh_phase phase, const bool *missing) {
  /* The give-up owns this rank's line from here on. */
  arts_exit_startup_disarm();
  mesh_close_all();
  const struct arts_config_s *config = arts_global_message_table;
  const unsigned int n = config != NULL ? config->table_length : 0u;
  const struct arts_config_table_s *table =
      config != NULL ? config->table : NULL;
  char line[ARTS_MESH_LINE_MAX + 1];
  if (arts_exit_rank_known()) {
    if (!arts_exit_take(ARTS_MESH_EXIT_STATUS, ARTS_EXIT_RUNTIME_ERROR)) {
      arts_exit(ARTS_MESH_EXIT_STATUS, ARTS_EXIT_RUNTIME_ERROR);
    }
    const char *host =
        config != NULL ? table[config->my_rank].ip_address : g_startup_node;
    mesh_write_line(line, arts_mesh_format_line(line, sizeof(line),
                                                (int)arts_global_rank_id, host,
                                                phase, missing, n, table));
    arts_exit_resume();
  }
  if (!arts_exit_began()) {
    mesh_write_line(line, arts_mesh_format_line(line, sizeof(line), -1,
                                                g_startup_node, phase, missing,
                                                n, table));
  }
  arts_exit(ARTS_MESH_EXIT_STATUS, ARTS_EXIT_RUNTIME_ERROR);
}

/* The descriptors the formation and, past the arm, the fabric hold.  A
 * tcp-based provider (named, or the automatic choice, which tries tcp first)
 * holds one socket per peer it talks to. */
static bool mesh_provider_holds_sockets(const struct arts_config_s *config) {
  const char *name = config->provider;
  if (name == NULL || name[0] == '\0') {
    name = getenv("FI_PROVIDER");
  }
  if (name == NULL || name[0] == '\0') {
    return true;
  }
  for (const char *c = name; *c != '\0';) {
    size_t len = strcspn(c, ";,");
    if (len == 3 && strncmp(c, "tcp", 3) == 0) {
      return true;
    }
    c += len + (c[len] != '\0' ? 1 : 0);
  }
  return false;
}

static unsigned int mesh_open_fds(void) {
  DIR *dir = opendir("/proc/self/fd");
  if (dir == NULL) {
    return 0;
  }
  unsigned int n = 0;
  for (struct dirent *e = readdir(dir); e != NULL; e = readdir(dir)) {
    n += e->d_name[0] != '.' ? 1u : 0u;
  }
  closedir(dir);
  return n > 0 ? n - 1u : 0u; /* the listing's own descriptor */
}

/* Raises the soft descriptor limit to the hard one when it is below what the
 * mesh and the fabric want; it refuses only a limit the mesh's own need
 * cannot fit in (a hard limit below it, or a soft one below it that cannot be
 * raised), so the reserve never refuses a run the mesh itself could form. */
static void mesh_reserve_descriptors(const struct arts_config_s *config) {
  const uint64_t peers = config->table_length - 1u;
  const uint64_t need = peers + 1u + mesh_open_fds();
  const uint64_t want =
      need + (mesh_provider_holds_sockets(config) ? peers : 0u);
  struct rlimit lim;
  if (getrlimit(RLIMIT_NOFILE, &lim) != 0) {
    ARTS_ERROR("getrlimit(RLIMIT_NOFILE): %s", strerror(errno));
  }
  if (lim.rlim_max != RLIM_INFINITY && lim.rlim_max < need) {
    ARTS_ERROR("the bootstrap mesh needs %llu file descriptors and "
               "RLIMIT_NOFILE's hard limit is %llu",
               (unsigned long long)need, (unsigned long long)lim.rlim_max);
  }
  if (lim.rlim_cur != RLIM_INFINITY && lim.rlim_cur < want &&
      lim.rlim_cur != lim.rlim_max) {
    const rlim_t had = lim.rlim_cur;
    lim.rlim_cur = lim.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &lim) != 0) {
      if (had < need) {
        ARTS_ERROR("the bootstrap mesh needs %llu file descriptors and "
                   "RLIMIT_NOFILE's soft limit %llu cannot be raised: %s",
                   (unsigned long long)need, (unsigned long long)had,
                   strerror(errno));
      }
      ARTS_WARN("RLIMIT_NOFILE's soft limit %llu is below the %llu "
                "descriptors the mesh and the fabric want, and cannot be "
                "raised: %s",
                (unsigned long long)had, (unsigned long long)want,
                strerror(errno));
    }
  }
}

/* A descriptor the formation could not get: a full host file table is the
 * host's condition, which another host cures; this process's own limit was
 * reserved, so running out of it is a configuration error. */
static _Noreturn void mesh_descriptor_error(const char *call) {
  const int err = errno;
  if (err == ENFILE) {
    arts_mesh_fail(ARTS_MESH_PHASE_LISTEN, NULL);
  }
  ARTS_ERROR("%s for the bootstrap mesh: %s", call, strerror(err));
}

/* A connect error is retried this long after; a connect still in progress
 * this long is closed and re-issued. */
#define MESH_RETRY_NS 100000000ull
#define MESH_REISSUE_NS 1000000000ull

static uint64_t mesh_now_ns(void) {
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

/* A poll timeout from now to until, in milliseconds rounded up. */
static int mesh_ms_until(uint64_t now, uint64_t until) {
  if (until <= now) {
    return 0;
  }
  const uint64_t ms = (until - now + 999999u) / 1000000u;
  return ms > (uint64_t)INT_MAX ? INT_MAX : (int)ms;
}

/* Binds this rank's listener on every interface.  A port another process
 * holds is retried every 100 ms until the deadline. */
static void mesh_listen(const struct arts_config_s *config, uint64_t deadline) {
  const unsigned int port = config->table[config->my_rank].port;
  g_mesh_listener =
      socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (g_mesh_listener < 0) {
    mesh_descriptor_error("socket");
  }
  int one = 1;
  (void)setsockopt(g_mesh_listener, SOL_SOCKET, SO_REUSEADDR, &one,
                   sizeof(one));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);
  while (bind(g_mesh_listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    if (errno != EADDRINUSE) {
      ARTS_ERROR("Could not bind listen port %u: %s", port, strerror(errno));
    }
    if (arts_exit_began()) {
      arts_exit(ARTS_MESH_EXIT_STATUS, ARTS_EXIT_RUNTIME_ERROR);
    }
    const uint64_t now = mesh_now_ns();
    if (now >= deadline) {
      arts_mesh_fail(ARTS_MESH_PHASE_LISTEN, NULL);
    }
    const uint64_t retry = now + MESH_RETRY_NS;
    (void)poll(NULL, 0,
               mesh_ms_until(now, retry < deadline ? retry : deadline));
  }
  if (listen(g_mesh_listener, SOMAXCONN) != 0) {
    ARTS_ERROR("Could not listen on port %u: %s", port, strerror(errno));
  }
}

/* A peer's link during the formation.  A lower peer is connected to: IDLE
 * until at_ns, then CONNECTING (re-issued at at_ns), then HELLO while the
 * hello is written, then UP.  A higher peer is ACCEPT until a connection
 * brings its hello, then UP. */
enum mesh_link_state {
  MESH_IDLE,
  MESH_CONNECTING,
  MESH_HELLO,
  MESH_ACCEPT,
  MESH_UP,
};
struct mesh_link_s {
  enum mesh_link_state state;
  uint64_t at_ns;
  unsigned int sent;
  struct sockaddr_in addr;
};

static void mesh_link_retry(struct mesh_link_s *l, unsigned int p,
                            uint64_t now) {
  close(arts_socket_fd[p]);
  arts_socket_fd[p] = -1;
  l->state = MESH_IDLE;
  l->at_ns = now + MESH_RETRY_NS;
}

static void mesh_link_up(struct mesh_link_s *l, unsigned int p, int fd) {
  socket_set_keepalive(fd);
  arts_socket_fd[p] = fd;
  l->state = MESH_UP;
}

static void mesh_connect(struct mesh_link_s *l, unsigned int p, uint64_t now) {
  const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    mesh_descriptor_error("socket");
  }
  arts_socket_fd[p] = fd;
  l->sent = 0;
  if (connect(fd, (const struct sockaddr *)&l->addr, sizeof(l->addr)) == 0) {
    l->state = MESH_HELLO;
  } else if (errno == EINPROGRESS || errno == EINTR) {
    l->state = MESH_CONNECTING;
    l->at_ns = now + MESH_REISSUE_NS;
  } else {
    mesh_link_retry(l, p, now);
  }
}

static void mesh_send_hello(struct mesh_link_s *l, unsigned int p,
                            unsigned int me, uint64_t now) {
  const struct arts_mesh_hello_s hello = {.magic = ARTS_MESH_HELLO_MAGIC,
                                          .rank = me};
  const unsigned char *bytes = (const unsigned char *)&hello;
  const ssize_t r = send(arts_socket_fd[p], bytes + l->sent,
                         sizeof(hello) - l->sent, MSG_NOSIGNAL | MSG_DONTWAIT);
  if (r > 0) {
    l->sent += (unsigned int)r;
    if (l->sent == sizeof(hello)) {
      mesh_link_up(l, p, arts_socket_fd[p]);
    }
  } else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
             errno != EINTR) {
    mesh_link_retry(l, p, now);
  }
}

static void mesh_pending_drop(unsigned int i, bool close_it) {
  if (close_it) {
    close(g_mesh_pending[i].fd);
  }
  g_mesh_pending[i] = g_mesh_pending[--g_mesh_pending_n];
}

/* Reads what is left of a pending connection's hello, and not a byte past
 * it.  A whole hello from a higher rank no connection holds yet makes that
 * peer's link; any other connection is closed, unattributed. */
static void mesh_take_hello(unsigned int i, struct mesh_link_s *link,
                            const struct arts_config_s *config) {
  struct mesh_pending_s *c = &g_mesh_pending[i];
  const ssize_t r =
      recv(c->fd, c->hello + c->got, sizeof(c->hello) - c->got, MSG_DONTWAIT);
  if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
    return;
  }
  if (r <= 0) {
    mesh_pending_drop(i, true);
    return;
  }
  c->got += (unsigned int)r;
  if (c->got < sizeof(c->hello)) {
    return;
  }
  struct arts_mesh_hello_s hello;
  memcpy(&hello, c->hello, sizeof(hello));
  if (hello.magic != ARTS_MESH_HELLO_MAGIC || hello.rank <= config->my_rank ||
      hello.rank >= config->table_length ||
      link[hello.rank].state != MESH_ACCEPT) {
    mesh_pending_drop(i, true);
    return;
  }
  mesh_link_up(&link[hello.rank], hello.rank, c->fd);
  mesh_pending_drop(i, false);
}

static void mesh_accept_all(void) {
  for (;;) {
    const int fd =
        accept4(g_mesh_listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd >= 0) {
      if (g_mesh_pending_n == g_mesh_pending_cap) {
        g_mesh_pending_cap = g_mesh_pending_cap ? 2u * g_mesh_pending_cap : 8u;
        g_mesh_pending = (struct mesh_pending_s *)arts_realloc(
            g_mesh_pending, g_mesh_pending_cap * sizeof(*g_mesh_pending));
      }
      g_mesh_pending[g_mesh_pending_n++] =
          (struct mesh_pending_s){.fd = fd, .got = 0};
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    /* A connection that failed before it was taken is the peer's to retry. */
    if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO ||
        errno == ENOPROTOOPT || errno == EOPNOTSUPP || errno == ENETDOWN ||
        errno == ENONET || errno == EHOSTDOWN || errno == EHOSTUNREACH ||
        errno == ENETUNREACH) {
      continue;
    }
    mesh_descriptor_error("accept4");
  }
}

void arts_mesh_form(struct arts_config_s *config) {
  const unsigned int n = config->table_length;
  const unsigned int me = config->my_rank;
  const uint64_t deadline = g_startup_deadline_ns;
  mesh_reserve_descriptors(config);
  arts_socket_fd = (int *)arts_malloc(n * sizeof(int));
  struct mesh_link_s *link =
      (struct mesh_link_s *)arts_calloc(n, sizeof(struct mesh_link_s));
  bool *flagged = (bool *)arts_calloc(n, sizeof(bool));
  for (unsigned int p = 0; p < n; p++) {
    arts_socket_fd[p] = -1;
    link[p].state = p < me ? MESH_IDLE : MESH_ACCEPT;
    link[p].addr.sin_family = AF_INET;
    link[p].addr.sin_port = htons((uint16_t)config->table[p].port);
    link[p].addr.sin_addr.s_addr = inet_addr(ip_list + ((ptrdiff_t)100 * p));
  }
  link[me].state = MESH_UP;

  arts_startup_step(ARTS_MESH_PHASE_LISTEN);
  mesh_listen(config, deadline);
  arts_startup_step(ARTS_MESH_PHASE_CONNECT);

  unsigned int cap = 0;
  struct pollfd *pfd = NULL;
  unsigned int *who = NULL;
  for (;;) {
    if (arts_exit_began()) {
      arts_exit(ARTS_MESH_EXIT_STATUS, ARTS_EXIT_RUNTIME_ERROR);
    }
    unsigned int up = 0;
    for (unsigned int p = 0; p < n; p++) {
      up += link[p].state == MESH_UP ? 1u : 0u;
    }
    if (up == n) {
      break;
    }
    uint64_t now = mesh_now_ns();
    if (now >= deadline) {
      for (unsigned int p = 0; p < n; p++) {
        flagged[p] = link[p].state != MESH_UP;
      }
      arts_mesh_fail(ARTS_MESH_PHASE_CONNECT, flagged);
    }
    uint64_t wake = deadline;
    for (unsigned int p = 0; p < me; p++) {
      struct mesh_link_s *l = &link[p];
      if (l->state == MESH_CONNECTING && l->at_ns <= now) {
        close(arts_socket_fd[p]);
        arts_socket_fd[p] = -1;
        l->state = MESH_IDLE;
      }
      if (l->state == MESH_IDLE && l->at_ns <= now) {
        mesh_connect(l, p, now);
      }
      if ((l->state == MESH_IDLE || l->state == MESH_CONNECTING) &&
          l->at_ns < wake) {
        wake = l->at_ns;
      }
    }

    /* Every round polls the listener, every link in flight, every
     * established link for its end only (its bytes past the hello wait for
     * the exchange) and every pending connection. */
    const unsigned int need = 1u + n + g_mesh_pending_n;
    if (need > cap) {
      cap = need;
      pfd = (struct pollfd *)arts_realloc(pfd, cap * sizeof(*pfd));
      who = (unsigned int *)arts_realloc(who, cap * sizeof(*who));
    }
    unsigned int k = 0;
    pfd[k] = (struct pollfd){.fd = g_mesh_listener, .events = POLLIN};
    who[k++] = UINT_MAX;
    for (unsigned int p = 0; p < n; p++) {
      if (p == me || link[p].state == MESH_IDLE ||
          link[p].state == MESH_ACCEPT) {
        continue;
      }
      const short events = link[p].state == MESH_UP ? POLLRDHUP : POLLOUT;
      pfd[k] = (struct pollfd){.fd = arts_socket_fd[p], .events = events};
      who[k++] = p;
    }
    for (unsigned int i = 0; i < g_mesh_pending_n; i++) {
      pfd[k] = (struct pollfd){.fd = g_mesh_pending[i].fd, .events = POLLIN};
      who[k++] = n + i;
    }
    if (poll(pfd, (nfds_t)k, mesh_ms_until(now, wake)) < 0) {
      if (errno == EINTR) {
        continue;
      }
      ARTS_ERROR("poll on the bootstrap mesh: %s", strerror(errno));
    }

    now = mesh_now_ns();
    bool ended = false;
    memset(flagged, 0, n * sizeof(bool));
    bool accept_due = false;
    /* Entries are serviced from the highest index, pending connections
     * first, so a drop's swap moves in one this round already serviced. */
    for (unsigned int j = k; j-- > 0;) {
      const short re = pfd[j].revents;
      if (re == 0) {
        continue;
      }
      if (who[j] == UINT_MAX) {
        accept_due = true;
      } else if (who[j] >= n) {
        mesh_take_hello(who[j] - n, link, config);
      } else {
        const unsigned int p = who[j];
        struct mesh_link_s *l = &link[p];
        if (l->state == MESH_UP) {
          if (re & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) {
            flagged[p] = true;
            ended = true;
          }
        } else if (l->state == MESH_CONNECTING) {
          int err = 0;
          socklen_t len = sizeof(err);
          if (getsockopt(arts_socket_fd[p], SOL_SOCKET, SO_ERROR, &err, &len) !=
              0) {
            err = errno;
          }
          if (err == 0 && (re & POLLOUT) != 0) {
            l->state = MESH_HELLO;
            mesh_send_hello(l, p, me, now);
          } else {
            mesh_link_retry(l, p, now);
          }
        } else if (l->state == MESH_HELLO) {
          if (re & (POLLERR | POLLHUP | POLLNVAL)) {
            mesh_link_retry(l, p, now);
          } else {
            mesh_send_hello(l, p, me, now);
          }
        }
      }
    }
    if (ended) {
      arts_mesh_fail(ARTS_MESH_PHASE_HANGUP, flagged);
    }
    if (accept_due) {
      mesh_accept_all();
    }
  }

  /* A pending connection never delays the formation; it ends with it. */
  close(g_mesh_listener);
  g_mesh_listener = -1;
  for (unsigned int i = 0; i < g_mesh_pending_n; i++) {
    close(g_mesh_pending[i].fd);
  }
  g_mesh_pending_n = 0;
  arts_free(g_mesh_pending);
  g_mesh_pending = NULL;
  g_mesh_pending_cap = 0;
  arts_free(pfd);
  arts_free(who);
  arts_free(flagged);
  arts_free(link);
}
