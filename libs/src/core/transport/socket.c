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

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h> /* TCP_NODELAY */
#include <poll.h>
#include <unistd.h>

#include "arts.h"
#include "arts/runtime_state.h"
#include "arts/system/config.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/transport/dispatcher.h"
#include "arts/transport/netif.h"
#include "arts/transport/protocol.h"
#include "arts/utils/atomics.h"
#include "arts/utils/malloc.h"

/* Disable Nagle on a connected TCP data socket.  ARTS's cross-rank protocol is
 * dominated by small synchronous request/ACK round-trips (DB publish ACK,
 * finish-event signals, lock requests); Nagle's coalescing delay compounds with
 * the
 * peer's delayed-ACK to inflate every such round-trip. */
static inline void arts_socket_set_nodelay(int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

struct arts_config_s *arts_global_message_table;
unsigned int ports;
// SOCKETS!
int *remote_socket_send_list;
volatile unsigned int *volatile remote_socket_send_lock_list;
struct sockaddr_in *remote_server_send_list;
bool *remote_connection_alive;

int *local_socket_receive;
int *remote_socket_receive_list;
fd_set read_set;
int max_fd;
struct sockaddr_in *remote_server_receive_list;
struct pollfd *poll_incoming;

#define PACKET_SIZE 4194304

char *ip_list;

void arts_transport_set_config(struct arts_config_s *config) {
  arts_global_message_table = config;
  ports = config->port_count;
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

/* This rank's row: the launcher's word where it gives one (the rank a spawning
 * launcher passes in ARTS_RANK, a flux or Slurm task rank), else the row whose
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
  int fd = socket(PF_INET, SOCK_STREAM, 0);
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

/* Sliding the port block is the spawning rank's job, and only on a local run:
 * a bind probe here observes this machine, which for a local run is where every
 * rank lives.  A rank that was spawned already carries the answer in its
 * environment and must not choose again — two ranks choosing separately would
 * disagree about each other's ports. */
static bool this_rank_chooses_ports(const struct arts_config_s *config) {
  return config->launcher != NULL && strcmp(config->launcher, "local") == 0 &&
         config->master_boot && config->table_length > 1 &&
         config->table != NULL && config->ports != NULL &&
         config->port_count > 0 && getenv(ARTS_RESOLVED_PORTS_ENV) == NULL;
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
   * as zero-traffic LIVENESS SENTINELS — connection lifetime doubles as peer
   * liveness, and an orderly peer exit and a peer process death both surface as
   * HUP/EOF on the accept-side socket.  Closing any established socket here
   * would instead signal a false death to its peer, so only the listening
   * sockets (whose accept duty is complete) are closed.  The launcher / stdio
   * sockets are separate fds and are untouched. */
  if (!arts_global_message_table) {
    return;
  }
  if (local_socket_receive) {
    for (int z = 0; z < (int)ports; z++) {
      if (local_socket_receive[z] > 0) {
        close(local_socket_receive[z]);
        local_socket_receive[z] = 0;
      }
    }
  }
}

/* Single prober at a time: two progress threads polling the shared pollfd
 * array would race on its revents fields. */
static _Atomic int g_sentinel_probing;

bool arts_socket_sentinel_check(void) {
  /* Probe the accept-side mesh for peer death (POLLHUP/POLLERR/EOF), with a
   * zero timeout — called from a progress thread's idle cycle, never blocking,
   * and never reading data beyond draining the EOF condition (no payload ever
   * rides these sockets after bootstrap).  A dead peer funnels into the same
   * idempotent shutdown entry an inbound shutdown message uses.  Once this
   * rank's shutdown is in progress the probe stands down.  Nothing orders any
   * rank's close after every other rank has dispatched the initiator's
   * MSG_SHUTDOWN (the initiator's drain waits only for its local sends to
   * complete, and a receiver tears down without waiting for anyone), so an
   * orderly teardown can reach this probe first too; both enter the same
   * shutdown, so the probe need not tell them apart. */
  if (!arts_global_message_table || poll_incoming == NULL ||
      __atomic_load_n(&arts_node_info.shutdown_state, __ATOMIC_ACQUIRE)) {
    return false;
  }
  int n = (int)(arts_global_message_table->table_length - 1) * (int)ports;
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
                  "this rank's shutdown began — entering shutdown",
                  i);
        poll_incoming[i].fd = -1; /* negative fd: poll ignores this entry */
      }
    }
  }
  atomic_store_explicit(&g_sentinel_probing, 0, memory_order_release);
  if (dead_peer) {
    /* Same entry the wire shutdown handler uses (idempotent CAS), then stop
     * the local threads — this thread's own loop exits on the cleared flag. */
    arts_enter_shutdown_state(false);
    arts_runtime_stop();
  }
  return dead_peer;
}

void arts_socket_cleanup() {
  /* Close of the liveness sentinels, at the very end of this rank's teardown.
   * Nothing orders it after every other rank has dispatched the initiator's
   * MSG_SHUTDOWN (the initiator's drain waits only for its local sends to
   * complete, and a receiver tears down without waiting for anyone), so a peer
   * may still observe the HUP before its own shutdown began; its probe then
   * enters the same idempotent shutdown. */
  if (arts_global_message_table != NULL) {
    int count = (int)arts_global_message_table->table_length;
    if (remote_socket_receive_list) {
      for (int i = 0; i < (count - 1) * (int)ports; i++) {
        if (remote_socket_receive_list[i] >= 0) {
          close(remote_socket_receive_list[i]);
        }
      }
    }
    if (remote_socket_send_list) {
      for (int i = 0; i < count * (int)ports; i++) {
        if (i / (int)ports != (int)arts_global_rank_id &&
            remote_socket_send_list[i] >= 0) {
          close(remote_socket_send_list[i]);
        }
      }
    }
  }
  arts_free(ip_list);
  arts_free(remote_socket_send_list);
  arts_free((void *)remote_socket_send_lock_list);
  arts_free(remote_server_send_list);
  arts_free(remote_connection_alive);
  arts_free(remote_socket_receive_list);
  arts_free(remote_server_receive_list);
  arts_free(poll_incoming);
  arts_free(local_socket_receive);
}

/* The startup budget of one connection of the socket mesh.  An accept waits
 * up to the budget for its next peer; a connect is retried budget / 100 ms
 * times, 100 ms apart, and each attempt blocks for as long as the kernel's
 * connect does, so the connect side counts attempts, not time.  The skew
 * between the ranks of one job is the launcher's, not the runtime's, so the
 * budget is a knob: ARTS_STARTUP_TIMEOUT_S, in seconds. */
#define ARTS_STARTUP_TIMEOUT_DEFAULT_S 300u
static unsigned int arts_startup_budget_ms(void) {
  static unsigned int cached_ms = 0;
  if (cached_ms == 0) {
    const char *s = getenv("ARTS_STARTUP_TIMEOUT_S");
    char *end = NULL;
    unsigned long v = (s != NULL) ? strtoul(s, &end, 10) : 0;
    if (s == NULL || end == s || v == 0 || v > 86400ul) {
      v = ARTS_STARTUP_TIMEOUT_DEFAULT_S;
    }
    cached_ms = (unsigned int)v * 1000u;
  }
  return cached_ms;
}

static inline bool arts_transport_connect(int rank, unsigned int port) {

  if (!remote_connection_alive[(rank * ports) + port]) {
    int res = connect(remote_socket_send_list[(rank * ports) + port],
                      (struct sockaddr *)(remote_server_send_list +
                                          ((size_t)rank * ports) + port),
                      sizeof(struct sockaddr_in));
    if (res < 0) {
      remote_connection_alive[(rank * ports) + port] = false;

      close(remote_socket_send_list[(rank * ports) + port]);
      remote_socket_send_list[(rank * ports) + port] = arts_get_new_socket();

      /* The peer may not be listening yet: ranks of one job start with a
       * skew the launcher does not bound, so retry until the startup budget
       * is spent. */
      int max_retries = (int)(arts_startup_budget_ms() / 100u);
      int retry_count = 0;
      while (connect(remote_socket_send_list[(rank * ports) + port],
                     (struct sockaddr *)(remote_server_send_list +
                                         ((size_t)rank * ports) + port),
                     sizeof(struct sockaddr_in)) < 0) {
        /* Stop retrying once this rank's shutdown has begun.  During startup
         * only the signal watcher begins one, and it reports the signal. */
        if (__atomic_load_n(&arts_node_info.shutdown_state, __ATOMIC_ACQUIRE)) {
          return false;
        }
        if (++retry_count >= max_retries) {
          struct sockaddr_in *addr =
              remote_server_send_list + ((size_t)rank * ports) + port;
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
        close(remote_socket_send_list[(rank * ports) + port]);
        remote_socket_send_list[(rank * ports) + port] = arts_get_new_socket();
        usleep(100000);
      }

      remote_connection_alive[(rank * ports) + port] = true;

      return true;
    }

    remote_connection_alive[(rank * ports) + port] = true;
  }

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

  remote_socket_receive_list =
      (int *)arts_malloc(sizeof(int) * (size_t)(count + 1) * ports);
  remote_server_receive_list = (struct sockaddr_in *)arts_calloc(
      (size_t)(count + 1) * ports, sizeof(struct sockaddr_in));
  poll_incoming =
      (struct pollfd *)arts_malloc(sizeof(struct pollfd) * (count + 1) * ports);

  struct sockaddr_in test;

  struct sockaddr_in *local_server_addr =
      (struct sockaddr_in *)arts_calloc(ports, sizeof(struct sockaddr_in));
  local_socket_receive = (int *)arts_calloc(ports, sizeof(int));

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
      if (__atomic_load_n(&arts_node_info.shutdown_state, __ATOMIC_ACQUIRE)) {
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

  FD_ZERO(&read_set);
  for (i = 0; i < (int)arts_global_message_table->table_length; i++) {
    if (arts_global_message_table->my_rank ==
        arts_global_message_table->table[i].rank) {
      for (j = 0; j < count; j++) {
        for (int z = 0; z < (int)ports; z++) {
          s_length = sizeof(struct sockaddr_in);

          /* Poll with the startup budget before the blocking accept, so a
           * peer that never starts (or a port a stale process still holds)
           * ends the rank instead of hanging it. */
          struct pollfd accept_pfd = {.fd = local_socket_receive[z],
                                      .events = POLLIN};
          int poll_res = poll(&accept_pfd, 1, (int)arts_startup_budget_ms());
          if (poll_res <= 0) {
            ARTS_WARN("arts_transport_setup_incoming: rank %u timed out "
                      "after %u s waiting for peer connection %d of %d on "
                      "port index %d (poll=%d, errno=%d: %s)",
                      arts_global_rank_id, arts_startup_budget_ms() / 1000u,
                      j + 1, count, z, poll_res, errno, strerror(errno));
            return false;
          }

          remote_socket_receive_list[z + (j * ports)] = accept(
              local_socket_receive[z], (struct sockaddr *)&test, &s_length);
          if (remote_socket_receive_list[z + (j * ports)] < 0) {
            ARTS_WARN("arts_transport_setup_incoming: rank %u accept failed "
                      "on port index %d: %s",
                      arts_global_rank_id, z, strerror(errno));
            return false;
          }
          arts_socket_set_nodelay(remote_socket_receive_list[z + (j * ports)]);

          poll_incoming[z + (j * ports)].fd =
              remote_socket_receive_list[z + (j * ports)];
          poll_incoming[z + (j * ports)].events = POLLIN;
        }
      }
    } else {
      for (int z = 0; z < (int)ports; z++) {
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

  remote_socket_send_list =
      (int *)arts_malloc(sizeof(int) * (size_t)count * ports);
  remote_socket_send_lock_list =
      (volatile unsigned int *)arts_calloc((size_t)count * ports, sizeof(int));
  remote_server_send_list = (struct sockaddr_in *)arts_calloc(
      (size_t)count * ports, sizeof(struct sockaddr_in));
  remote_connection_alive =
      (bool *)arts_calloc((size_t)count * ports, sizeof(bool));

  for (i = 0; i < count; i++) {
    unsigned int *target_ports = arts_global_message_table->table[i].ports;

    ARTS_INFO("arts_transport_setup_outgoing: node %d ip_list='%s' port=%u", i,
              ip_list + ((ptrdiff_t)100 * i), target_ports[0]);

    for (j = 0; j < (int)ports; j++) {
      remote_socket_send_list[(i * ports) + j] = arts_get_socket_outgoing(
          remote_server_send_list + ((size_t)i * ports) + j, target_ports[j],
          inet_addr(ip_list + ((ptrdiff_t)100 * i)));
    }
  }
}

int arts_get_new_socket() {
  int socket_out = socket(PF_INET, SOCK_STREAM, 0);
  if (socket_out < 0) {
    ARTS_ERROR("socket() failed: %s", strerror(errno));
  }
  arts_socket_set_nodelay(socket_out);
  return socket_out;
}

int arts_get_socket_listening(struct sockaddr_in *listening_socket,
                              unsigned int port) {
  memset((char *)listening_socket, 0, sizeof(*listening_socket));
  int socket_out = socket(PF_INET, SOCK_STREAM, 0);
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
  int socket_out = socket(PF_INET, SOCK_STREAM, 0);
  if (socket_out < 0) {
    ARTS_ERROR("socket() failed: %s", strerror(errno));
  }
  outgoing_socket->sin_family = AF_INET;
  outgoing_socket->sin_addr.s_addr = s_addr;
  outgoing_socket->sin_port = htons(port);
  return socket_out;
}
