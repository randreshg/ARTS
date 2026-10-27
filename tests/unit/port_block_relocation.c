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

/// @file port_block_relocation.c
/// @brief A local run must move off a port block someone else already holds.
///
/// The spawning rank picks the block every rank will bind, so an occupied port
/// anywhere in it has to be found BEFORE the peers exist — once they are told a
/// base, nothing can move it.  This occupies one port of the block the selector
/// starts from and checks the selector slides the whole run somewhere free,
/// keeping the ranks' blocks disjoint and the base list in step with the table.
///
/// Two legs.  In the first the occupied port is declared rather than taken from
/// the host — a declared port refuses every bind, as a foreign process's live
/// listener does — and each rank's port of the seeded block is occupied in
/// turn, alone, so only a selector that probes every rank's share relocates
/// every time.  In the second the test holds a real listener on the last
/// rank's port of a block the selector found free, and the selector must move
/// past it through the kernel's own refusal (a port someone else already
/// holds is occupied all the same).  The kernel's ephemeral range (the
/// window's ceiling) is declared as the stock one in both, so the window is
/// the same on every host; every other port is the host's own.
///
/// Pure unit: resolves a config and probes ports, but starts no runtime and
/// spawns no ranks.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "arts.h"
#include "arts/system/config.h"
#include "arts/transport/socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NODES 3

static int fails = 0;

/* The declared occupied ports: [g_held_base, g_held_base + g_held_count). */
static unsigned int g_held_base;
static unsigned int g_held_count;

/* bind and fopen are wrapped at link time (-Wl,--wrap=bind,--wrap=fopen):
 * every call this program's objects make reaches the wrapper, and the C
 * library's own definition stays its __real_ name. */
int __real_bind(int fd, const struct sockaddr *addr, socklen_t len);
FILE *__real_fopen(const char *path, const char *mode);

/* An IPv4 bind to an occupied port fails as one held by a live listener does;
 * every other bind is the C library's. */
int __wrap_bind(int fd, const struct sockaddr *addr, socklen_t len) {
  if (addr != NULL && addr->sa_family == AF_INET &&
      len >= (socklen_t)sizeof(struct sockaddr_in)) {
    unsigned int port = ntohs(((const struct sockaddr_in *)addr)->sin_port);
    if (port >= g_held_base && port - g_held_base < g_held_count) {
      errno = EADDRINUSE;
      return -1;
    }
  }
  return __real_bind(fd, addr, len);
}

/* The kernel's ephemeral port range reads as the stock 32768-60999; every
 * other path is the C library's. */
FILE *__wrap_fopen(const char *path, const char *mode) {
  if (path != NULL &&
      strcmp(path, "/proc/sys/net/ipv4/ip_local_port_range") == 0) {
    static const char range[] = "32768\t60999\n";
    return fmemopen((void *)range, sizeof range - 1, "r");
  }
  return __real_fopen(path, mode);
}

/* Hold a port as a foreign process would: a live listener, which no
 * SO_REUSEADDR on the prober's side can bind past.  -1 with errno set when
 * the port cannot be held. */
static int hold_port(unsigned int port) {
  int fd = socket(PF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons((uint16_t)port);
  if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 1) < 0) {
    int err = errno;
    close(fd);
    errno = err;
    return -1;
  }
  return fd;
}

/* Run the selector from `start` with port `held` occupied: it must leave the
 * block, land on one that does not cover the held port, keep the table in
 * step with the base list, and stay inside the window. */
static void relocate_past(struct arts_config_s *cfg, unsigned int start,
                          unsigned int held, const char *leg) {
  cfg->ports[0] = start;
  if (!arts_transport_select_local_ports(cfg)) {
    printf("FAIL port_block_relocation (%s): selector found no free block at "
           "all\n",
           leg);
    fails++;
    return;
  }
  if (cfg->ports[0] == start) {
    printf("FAIL port_block_relocation (%s): stayed on block %u with port %u "
           "occupied\n",
           leg, start, held);
    fails++;
  } else {
    const unsigned int chosen = cfg->ports[0];
    if (!arts_transport_select_local_ports(cfg) || cfg->ports[0] != chosen) {
      printf("FAIL port_block_relocation (%s): selector left the free block "
             "%u it had chosen\n",
             leg, chosen);
      fails++;
    }
    cfg->ports[0] = chosen;
  }
  if (cfg->ports[0] <= held && held < cfg->ports[0] + NODES) {
    printf("FAIL port_block_relocation (%s): new block %u still covers "
           "occupied port %u\n",
           leg, cfg->ports[0], held);
    fails++;
  }
  for (unsigned int i = 0; i < NODES; i++) {
    unsigned int want = cfg->ports[0] + (i * cfg->port_count);
    if (cfg->table[i].ports == NULL || cfg->table[i].ports[0] != want) {
      printf("FAIL port_block_relocation (%s): rank %u has port %u, want "
             "%u\n",
             leg, i, cfg->table[i].ports ? cfg->table[i].ports[0] : 0, want);
      fails++;
      break;
    }
  }
  if (cfg->ports[0] < ARTS_PORT_WINDOW_LO ||
      cfg->ports[0] + NODES > ARTS_PORT_WINDOW_HI) {
    printf("FAIL port_block_relocation (%s): block %u..%u outside the window "
           "%u-%u\n",
           leg, cfg->ports[0], cfg->ports[0] + NODES - 1, ARTS_PORT_WINDOW_LO,
           ARTS_PORT_WINDOW_HI);
    fails++;
  }
}

int main(void) {
  char cfg_path[256];
  snprintf(cfg_path, sizeof(cfg_path), "port_block_relocation_%ld.cfg",
           (long)getpid());
  FILE *f = fopen(cfg_path, "w");
  if (!f) {
    printf("FAIL port_block_relocation: cannot write cfg\n");
    return 1;
  }
  fputs("[ARTS]\n"
        "launcher=local\n"
        "node_count=3\n"
        "worker_threads=2\n"
        "progress_threads=1\n"
        "route_table_size=14\n",
        f);
  (void)fclose(f);

  setenv("ARTS_CONFIG", cfg_path, 1);
  unsetenv("ports");
  unsetenv("port_count");
  unsetenv(ARTS_RESOLVED_PORTS_ENV);

  struct arts_config_s cfg;
  arts_config_load(&cfg);
  (void)remove(cfg_path);

  if (cfg.table_length != NODES || cfg.ports == NULL || cfg.port_count != 1) {
    printf("FAIL port_block_relocation: config did not resolve (len=%u "
           "ports=%p port_count=%u)\n",
           cfg.table_length, (void *)cfg.ports, cfg.port_count);
    return 1;
  }

  const unsigned int seed = cfg.ports[0];

  /* Declared leg: rank r's port of the seeded block is the only one
     occupied. */
  for (unsigned int r = 0; r < NODES; r++) {
    char leg[32];
    snprintf(leg, sizeof leg, "declared, rank %u's port", r);
    cfg.ports[0] = seed;
    g_held_base = seed + r;
    g_held_count = 1;
    relocate_past(&cfg, seed, seed + r, leg);
    g_held_count = 0;
  }

  /* Kernel leg: a real listener on the last rank's port of a block the
     selector has just found free. */
  unsigned int base = cfg.ports[0];
  unsigned int port = 0;
  int fd = -1;
  for (int attempt = 0; attempt < 8; attempt++) {
    port = base + NODES - 1;
    fd = hold_port(port);
    if (fd >= 0 || errno != EADDRINUSE) {
      break;
    }
    cfg.ports[0] = base + NODES;
    if (!arts_transport_select_local_ports(&cfg)) {
      break;
    }
    base = cfg.ports[0];
  }
  if (fd < 0) {
    printf("FAIL port_block_relocation: cannot hold a listener on the last "
           "rank's port of a free block (last tried %u): %s\n",
           port, strerror(errno));
    fails++;
  } else {
    relocate_past(&cfg, base, port, "kernel listener");
  }
  if (fd >= 0) {
    close(fd);
  }
  arts_config_destroy(&cfg);

  if (fails) {
    printf("FAIL port_block_relocation: %d check(s) failed\n", fails);
    return 1;
  }
  printf("PASS port_block_relocation: slid off a block occupied at each "
         "rank's port, declared and by a live listener\n");
  return 0;
}
