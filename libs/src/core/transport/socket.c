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
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h> /* TCP_NODELAY, TCP_KEEP* */
#include <poll.h>
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

/* Disable Nagle on a connected TCP data socket.  ARTS's cross-rank protocol is
 * dominated by small synchronous request/ACK round-trips (DB publish ACK,
 * group signals, lock requests); Nagle's coalescing delay compounds with
 * the
 * peer's delayed-ACK to inflate every such round-trip. */
static inline void arts_socket_set_nodelay(int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

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
unsigned int arts_socket_ports;
// SOCKETS!
int *arts_socket_send_list;
static struct sockaddr_in *remote_server_send_list;
static bool *remote_connection_alive;

static int *local_socket_receive;
int *arts_socket_receive_list;
static struct pollfd *poll_incoming;

static char *ip_list;

/* Set once, by the main thread as the last act of arts_socket_sentinel_arm
 * (release), and read by any thread through arts_socket_armed (acquire).  Its
 * store publishes everything the main thread wrote before the arm: the
 * fabric's peer table and endpoint, this rank's id and the mesh's arrays and
 * descriptors. */
static _Atomic(bool) g_socket_armed;

void arts_transport_set_config(struct arts_config_s *config) {
  arts_global_message_table = config;
  arts_socket_ports = config->port_count;
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
     * row per host and every rank binds the same listen-port set, so a
     * launcher that starts more tasks than nodes hands the extras a rank
     * with no table row — and any same-host pair a guaranteed port
     * collision.  Fail with the contract spelled out instead of indexing
     * the table out of bounds. */
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
  // Always initialize ip_list - it's used by arts_transport_setup_outgoing()
  ip_list = (char *)arts_malloc(100 * sizeof(char) * config->table_length);
  for (int i = 0; i < (int)config->table_length; i++) {
    int rc = hostname_to_ip(config->table[i].ip_address,
                            ip_list + ((ptrdiff_t)100 * i));
    if (rc != 0) {
      ARTS_ERROR("cannot resolve node '%s': %s", config->table[i].ip_address,
                 gai_strerror(rc));
    }
  }
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
 * environment and must not choose again — two ranks choosing separately would
 * disagree about each other's ports. */
static bool this_rank_chooses_ports(const struct arts_config_s *config) {
  return config->launcher != NULL && strcmp(config->launcher, "local") == 0 &&
         config->table_length > 1 && config->table != NULL &&
         config->ports != NULL && config->port_count > 0 &&
         getenv(ARTS_RESOLVED_PORTS_ENV) == NULL;
}

bool arts_transport_select_local_ports(struct arts_config_s *config) {
  if (!this_rank_chooses_ports(config)) {
    return true;
  }

  const unsigned int span = config->table_length * config->port_count;
  const unsigned int seed = config->ports[0];

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
    for (unsigned int rank = 0; rank < config->table_length && all_free;
         rank++) {
      for (unsigned int slot = 0; slot < config->port_count; slot++) {
        if (!port_is_bindable(base + slot + (rank * config->port_count))) {
          all_free = false;
          break;
        }
      }
    }

    if (all_free) {
      if (base != seed) {
        ARTS_WARN("Local multi-node: port block at %u is occupied, taking %u "
                  "instead (%u ports)",
                  seed, base, span);
      }
      for (unsigned int slot = 0; slot < config->port_count; slot++) {
        config->ports[slot] = base + slot;
      }
      for (unsigned int rank = 0; rank < config->table_length; rank++) {
        for (unsigned int slot = 0; slot < config->port_count; slot++) {
          config->table[rank].ports[slot] =
              base + slot + (rank * config->port_count);
        }
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
  /* Called once the bootstrap fi-address exchange has finished: no payload will
   * ever ride the TCP mesh again, but the connections themselves are kept open
   * as payload-free LIVENESS SENTINELS — connection lifetime doubles as peer
   * liveness, and an orderly peer exit and a peer process death both surface as
   * HUP/EOF on the accept-side socket.  Closing any established socket here
   * would instead signal a false death to its peer, so only the listening
   * sockets (whose accept duty is complete) are closed. */
  if (!arts_global_message_table) {
    return;
  }
  if (local_socket_receive) {
    for (int z = 0; z < (int)arts_socket_ports; z++) {
      if (local_socket_receive[z] > 0) {
        close(local_socket_receive[z]);
        local_socket_receive[z] = 0;
      }
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
  int fd = arts_socket_send_list[(size_t)rank * arts_socket_ports];
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

/* Single prober at a time: two threads polling the shared pollfd array would
 * race on its revents fields.  A try-lock: the winning CAS acquires and the
 * return stores release, so each prober sees the entries the previous one
 * retired; a loser returns, and its next due probe tries again. */
static _Atomic(int) g_sentinel_probing;

bool arts_socket_sentinel_check(void) {
  /* Probe the accept-side mesh for peer death (POLLHUP/POLLERR/EOF), with a
   * zero timeout — never blocking, and never reading data beyond draining
   * the EOF condition (no payload ever rides these sockets after bootstrap).
   * Once this rank's shutdown is in progress the probe stands down.  Nothing
   * orders any rank's close after every other rank has taken in a SHUTDOWN
   * post (the post is best effort and reaches only the peers its rank
   * contacted, and no rank waits for anyone before it tears down), so an
   * orderly teardown can reach this probe first too, and for a peer the post
   * skipped it is the only way to learn of the end; the caller enters the
   * same shutdown for both, so the probe need not tell them apart. */
  if (!arts_global_message_table || poll_incoming == NULL ||
      arts_shutdown_taken()) {
    return false;
  }
  int n = (int)(arts_global_message_table->table_length - 1) *
          (int)arts_socket_ports;
  if (n <= 0) {
    return false;
  }
  int expected = 0;
  if (!atomic_compare_exchange_strong_explicit(&g_sentinel_probing, &expected,
                                               1, memory_order_acq_rel,
                                               memory_order_relaxed)) {
    return false;
  }
  bool dead_peer = false;
  if (poll(poll_incoming, (nfds_t)n, 0) > 0) {
    for (int i = 0; i < n && !dead_peer; i++) {
      short re = poll_incoming[i].revents;
      if (re == 0) {
        continue;
      }
      if (re & (POLLHUP | POLLERR | POLLNVAL)) {
        dead_peer = true;
      } else if (re & POLLIN) {
        char scratch[256];
        ssize_t r =
            recv(poll_incoming[i].fd, scratch, sizeof(scratch), MSG_DONTWAIT);
        if (r == 0) {
          dead_peer = true; /* EOF: peer closed its end */
        } else if (r > 0) {
          ARTS_WARN("liveness sentinel %d received %zd unexpected bytes "
                    "(no data should ride the bootstrap mesh)",
                    i, (ssize_t)r);
        }
        /* r < 0 (EAGAIN/EINTR): spurious wakeup, ignore. */
      }
      if (dead_peer) {
        ARTS_INFO("liveness sentinel %d: peer connection closed before "
                  "this rank's shutdown began",
                  i);
        poll_incoming[i].fd = -1; /* negative fd: poll ignores this entry */
      }
    }
  }
  atomic_store_explicit(&g_sentinel_probing, 0, memory_order_release);
  return dead_peer;
}

#ifdef ARTS_DEBUG_TEARDOWN
void arts_socket_cleanup() {
  /* Close of the liveness sentinels, in this rank's teardown.  Nothing
   * orders it after every other rank has taken in a SHUTDOWN post
   * (the post is best effort and reaches only the peers its rank contacted,
   * and no rank waits for anyone before it tears down), so a peer may still
   * observe the HUP before its own shutdown began; the thread whose probe
   * sees it enters the same idempotent shutdown. */
  if (arts_global_message_table != NULL) {
    int count = (int)arts_global_message_table->table_length;
    if (arts_socket_receive_list) {
      for (int i = 0; i < (count - 1) * (int)arts_socket_ports; i++) {
        if (arts_socket_receive_list[i] >= 0) {
          close(arts_socket_receive_list[i]);
        }
      }
    }
    if (arts_socket_send_list) {
      for (int i = 0; i < count * (int)arts_socket_ports; i++) {
        if (i / (int)arts_socket_ports != (int)arts_global_rank_id &&
            arts_socket_send_list[i] >= 0) {
          close(arts_socket_send_list[i]);
        }
      }
    }
  }
  arts_free(ip_list);
  arts_free(arts_socket_send_list);
  arts_free(remote_server_send_list);
  arts_free(remote_connection_alive);
  arts_free(arts_socket_receive_list);
  arts_free(poll_incoming);
  arts_free(local_socket_receive);
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
  const struct arts_config_s *config = arts_global_message_table;
  if (config == NULL) {
    return;
  }
  const size_t ports = arts_socket_ports;
  const size_t count = config->table_length;
  for (size_t z = 0; local_socket_receive != NULL && z < ports; z++) {
    if (local_socket_receive[z] > 0) {
      close(local_socket_receive[z]);
      local_socket_receive[z] = 0;
    }
  }
  for (size_t i = 0; arts_socket_send_list != NULL && i < count * ports; i++) {
    if (arts_socket_send_list[i] >= 0) {
      close(arts_socket_send_list[i]);
      arts_socket_send_list[i] = -1;
    }
  }
  for (size_t i = 0;
       arts_socket_receive_list != NULL && count > 0 && i < (count - 1) * ports;
       i++) {
    if (arts_socket_receive_list[i] >= 0) {
      close(arts_socket_receive_list[i]);
      arts_socket_receive_list[i] = -1;
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
  if (arts_exit_began()) {
    /* The end in progress is another thread's, bounded by its watchdog. */
    for (;;) {
      (void)pause();
    }
  }
  mesh_write_line(line,
                  arts_mesh_format_line(line, sizeof(line), -1, g_startup_node,
                                        phase, missing, n, table));
  arts_exit(ARTS_MESH_EXIT_STATUS, ARTS_EXIT_RUNTIME_ERROR);
}

static inline bool arts_transport_connect(int rank, unsigned int port) {
  size_t idx = ((size_t)rank * arts_socket_ports) + port;
  if (remote_connection_alive[idx]) {
    return true;
  }
  struct sockaddr_in *addr = remote_server_send_list + idx;
  if (connect(arts_socket_send_list[idx], (struct sockaddr *)addr,
              sizeof(struct sockaddr_in)) < 0) {
    close(arts_socket_send_list[idx]);
    arts_socket_send_list[idx] = arts_get_new_socket();

    /* The peer may not be listening yet: ranks of one job start with a
     * skew the launcher does not bound, so retry until the startup budget
     * is spent. */
    int max_retries = (int)(arts_startup_budget_s() * 1000u / 100u);
    int retry_count = 0;
    while (connect(arts_socket_send_list[idx], (struct sockaddr *)addr,
                   sizeof(struct sockaddr_in)) < 0) {
      /* Stop retrying once this rank's shutdown has begun.  During startup
       * only the signal watcher begins one, and it reports the signal. */
      if (arts_shutdown_taken()) {
        return false;
      }
      if (++retry_count >= max_retries) {
        /* Loud: the caller exits the rank on this, and a rank that dies
         * before its fabric is up leaves no other trace of why. */
        ARTS_WARN("arts_transport_connect: rank %u could not connect to "
                  "rank %d port %d in %d retries 100 ms apart (target "
                  "%s:%d, errno=%d: %s)",
                  arts_global_rank_id, rank, port, max_retries,
                  inet_ntoa(addr->sin_addr), ntohs(addr->sin_port), errno,
                  strerror(errno));
        return false;
      }
      close(arts_socket_send_list[idx]);
      arts_socket_send_list[idx] = arts_get_new_socket();
      usleep(100000);
    }
  }
  socket_set_keepalive(arts_socket_send_list[idx]);
  remote_connection_alive[idx] = true;
  return true;
}

bool arts_transport_setup_incoming() {
  // ARTS_INFO("%d", FD_SETSIZE);
  int i;
  int j;
  int k;
  int pos;
  unsigned int *my_ports =
      arts_global_message_table->table[arts_global_message_table->my_rank]
          .ports;
  socklen_t s_length = sizeof(struct sockaddr);
  int count = (int)(arts_global_message_table->table_length - 1);

  arts_socket_receive_list =
      (int *)arts_malloc(sizeof(int) * (size_t)(count + 1) * arts_socket_ports);
  for (size_t r = 0; r < (size_t)(count + 1) * arts_socket_ports; r++) {
    arts_socket_receive_list[r] = -1;
  }
  poll_incoming = (struct pollfd *)arts_malloc(sizeof(struct pollfd) *
                                               (count + 1) * arts_socket_ports);

  struct sockaddr_in test;

  struct sockaddr_in *local_server_addr = (struct sockaddr_in *)arts_calloc(
      arts_socket_ports, sizeof(struct sockaddr_in));
  local_socket_receive = (int *)arts_calloc(arts_socket_ports, sizeof(int));

  int i_set_option;
  for (i = 0; i < (int)arts_global_message_table->port_count; i++) {
    local_socket_receive[i] =
        arts_get_socket_listening(&local_server_addr[i], my_ports[i]);

    i_set_option = 1;
    setsockopt(local_socket_receive[i], SOL_SOCKET, SO_REUSEADDR,
               (char *)&i_set_option, sizeof(i_set_option));

    /* Bind with retry-on-EADDRINUSE, mirroring the connect-side retry below.
     * SO_REUSEADDR (set above) clears the TIME_WAIT case, but a previous
     * process still actively holding this fixed port — a peer rank
     * mid-shutdown, or a rapid restart on the same port set — fails the first
     * bind with EADDRINUSE, which SO_REUSEADDR cannot override.  Wait for the
     * port to be released rather than failing startup outright.  Bounded so a
     * genuinely persistent conflict still fails, and aborted promptly on
     * shutdown so we never spin for the full ceiling during teardown.  A failed
     * bind leaves the socket unbound, so the same fd is retried directly (no
     * recreate needed). */
    int res;
    int bind_retries = 0;
    const int max_bind_retries = 100; /* ~10s ceiling @ 100ms */
    while ((res = bind(local_socket_receive[i],
                       (struct sockaddr *)&local_server_addr[i],
                       sizeof(local_server_addr[i]))) < 0) {
      if (errno != EADDRINUSE) {
        break; /* a non-contention error — fail fast */
      }
      if (arts_shutdown_taken()) {
        return false;
      }
      if (++bind_retries >= max_bind_retries) {
        break; /* port never freed — give up and report below */
      }
      usleep(100000);
    }

    if (res < 0) {
      /* Loud and terminal: a rank that cannot open its listen socket can never
       * join the mesh, and the peers waiting to connect to it have no way to
       * learn that.  Reporting this below the default log level once made an
       * occupied port look like a clean, silent, zero-output exit. */
      ARTS_ERROR("Could not bind listen port %u: %s", my_ports[i],
                 strerror(errno));
      return false;
    }

    res = listen(local_socket_receive[i], 2 * count);

    if (res < 0) {
      ARTS_ERROR("Could not listen on port %u: %s", my_ports[i],
                 strerror(errno));
      return false;
    }
  }

  arts_free(local_server_addr);

  for (i = 0; i < (int)arts_global_message_table->table_length; i++) {
    if (arts_global_message_table->my_rank ==
        arts_global_message_table->table[i].rank) {
      for (j = 0; j < count; j++) {
        for (int z = 0; z < (int)arts_socket_ports; z++) {
          s_length = sizeof(struct sockaddr_in);

          /* Poll with the startup budget before the blocking accept, so a
           * peer that never starts (or a port a stale process still holds)
           * ends the rank instead of hanging it. */
          struct pollfd accept_pfd = {.fd = local_socket_receive[z],
                                      .events = POLLIN};
          int poll_res =
              poll(&accept_pfd, 1, (int)(arts_startup_budget_s() * 1000u));
          if (poll_res <= 0) {
            ARTS_WARN("arts_transport_setup_incoming: rank %u timed out "
                      "after %u s waiting for peer connection %d of %d on "
                      "port index %d (poll=%d, errno=%d: %s)",
                      arts_global_rank_id, arts_startup_budget_s(), j + 1,
                      count, z, poll_res, errno, strerror(errno));
            return false;
          }

          arts_socket_receive_list[z + (j * arts_socket_ports)] =
              accept4(local_socket_receive[z], (struct sockaddr *)&test,
                      &s_length, SOCK_CLOEXEC);
          if (arts_socket_receive_list[z + (j * arts_socket_ports)] < 0) {
            ARTS_WARN("arts_transport_setup_incoming: rank %u accept failed "
                      "on port index %d: %s",
                      arts_global_rank_id, z, strerror(errno));
            return false;
          }
          arts_socket_set_nodelay(
              arts_socket_receive_list[z + (j * arts_socket_ports)]);
          socket_set_keepalive(
              arts_socket_receive_list[z + (j * arts_socket_ports)]);

          poll_incoming[z + (j * arts_socket_ports)].fd =
              arts_socket_receive_list[z + (j * arts_socket_ports)];
          poll_incoming[z + (j * arts_socket_ports)].events = POLLIN;
        }
      }
    } else {
      for (int z = 0; z < (int)arts_socket_ports; z++) {
        if (!arts_transport_connect(i, z)) {
          return false;
        }
      }
    }
  }

  return true;
}

void arts_transport_setup_outgoing() {
  int i;
  int j;
  int count = (int)arts_global_message_table->table_length;

  arts_socket_send_list =
      (int *)arts_malloc(sizeof(int) * (size_t)count * arts_socket_ports);
  remote_server_send_list = (struct sockaddr_in *)arts_calloc(
      (size_t)count * arts_socket_ports, sizeof(struct sockaddr_in));
  remote_connection_alive =
      (bool *)arts_calloc((size_t)count * arts_socket_ports, sizeof(bool));

  for (i = 0; i < count; i++) {
    unsigned int *target_ports = arts_global_message_table->table[i].ports;

    ARTS_INFO("arts_transport_setup_outgoing: node %d ip_list='%s' port=%u", i,
              ip_list + ((ptrdiff_t)100 * i), target_ports[0]);

    for (j = 0; j < (int)arts_socket_ports; j++) {
      arts_socket_send_list[(i * arts_socket_ports) + j] =
          arts_get_socket_outgoing(
              remote_server_send_list + ((size_t)i * arts_socket_ports) + j,
              target_ports[j], inet_addr(ip_list + ((ptrdiff_t)100 * i)));
    }
  }
}

int arts_get_new_socket() {
  int socket_out = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (socket_out < 0) {
    ARTS_ERROR("socket() failed: %s", strerror(errno));
  }
  arts_socket_set_nodelay(socket_out);
  return socket_out;
}

int arts_get_socket_listening(struct sockaddr_in *listening_socket,
                              unsigned int port) {
  memset((char *)listening_socket, 0, sizeof(*listening_socket));
  int socket_out = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (socket_out < 0) {
    ARTS_ERROR("socket() failed: %s", strerror(errno));
  }
  listening_socket->sin_family = AF_INET;
  listening_socket->sin_addr.s_addr = htonl(INADDR_ANY);
  listening_socket->sin_port = htons(port);
  return socket_out;
}

int arts_get_socket_outgoing(struct sockaddr_in *outgoing_socket,
                             unsigned int port, in_addr_t s_addr) {
  memset((char *)outgoing_socket, 0, sizeof(*outgoing_socket));
  int socket_out = socket(PF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (socket_out < 0) {
    ARTS_ERROR("socket() failed: %s", strerror(errno));
  }
  outgoing_socket->sin_family = AF_INET;
  outgoing_socket->sin_addr.s_addr = s_addr;
  outgoing_socket->sin_port = htons(port);
  return socket_out;
}
