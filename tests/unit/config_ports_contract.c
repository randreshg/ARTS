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

/// @file config_ports_contract.c
/// @brief Who fixes the listen port, and who may not.
///
/// Every rank listens on one port.  A local run puts every rank on this
/// machine, so the runtime claims a free block of node_count ports itself and
/// rank i listens on the block's base + i; naming a port by hand there would
/// only invite the collisions that machinery exists to avoid.  Every other
/// launcher is the mirror image: its ranks sit on different hosts and resolve
/// each other's port from configs they read independently, so the config must
/// name the one port every rank listens on.
///
///   - launcher=local, ports absent  -> the runtime chooses a base in the
///     window, node i on base + i, every node on 127.0.0.1 and sharing this
///     machine's processing units
///   - launcher=local, ports named   -> refused, it is not the operator's call
///   - a spawned local rank          -> takes the base its spawner handed it
///     in ARTS_RESOLVED_PORT (one port; anything else is refused)
///   - launcher=ssh,   ports absent  -> refused, nothing else can supply it
///   - launcher=ssh,   ports named   -> every node listens on that port
///   - a list or a range of ports    -> refused
///
/// Config-parser test: arts_config_load() against crafted temp cfgs, no
/// runtime started and no ports bound.  The refusals are ARTS_ERROR death
/// paths, so they are probed in a forked child whose output is captured.

#include "arts.h"
#include "arts/system/config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_cfg(char *path_out, size_t path_cap, const char *tag,
                     const char *body) {
  snprintf(path_out, path_cap, "config_ports_contract_%s_%ld.cfg", tag,
           (long)getpid());
  FILE *f = fopen(path_out, "w");
  if (!f) {
    return -1;
  }
  fputs("[ARTS]\n"
        "worker_threads=2\n"
        "progress_threads=1\n"
        "route_table_size=14\n",
        f);
  fputs(body, f);
  (void)fclose(f);
  return 0;
}

static void clean_env(void) {
  unsetenv("ports");
  unsetenv(ARTS_RESOLVED_PORT_ENV);
}

/* Load the cfg in a forked child with ARTS_RESOLVED_PORT set to `resolved`
 * (NULL: unset); 1 = refused (nonzero exit or signal) with `says` in its
 * output (NULL: any output), 0 = accepted, -1 = harness failure. */
static int load_refused(const char *cfg_path, const char *resolved,
                        const char *says) {
  int out[2];
  if (pipe(out) != 0) {
    perror("pipe");
    return -1;
  }
  fflush(NULL);
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return -1;
  }
  if (pid == 0) {
    dup2(out[1], STDOUT_FILENO);
    dup2(out[1], STDERR_FILENO);
    close(out[0]);
    setenv("ARTS_CONFIG", cfg_path, 1);
    clean_env();
    if (resolved != NULL) {
      setenv(ARTS_RESOLVED_PORT_ENV, resolved, 1);
    }
    struct arts_config_s config;
    arts_config_load(&config);
    _exit(0);
  }
  close(out[1]);
  char text[8192];
  size_t len = 0;
  for (;;) {
    ssize_t r = read(out[0], text + len, sizeof(text) - 1 - len);
    if (r <= 0) {
      break;
    }
    len += (size_t)r;
  }
  text[len] = '\0';
  close(out[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid");
    return -1;
  }
  bool refused =
      WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0);
  if (refused && says != NULL && strstr(text, says) == NULL) {
    printf("config_ports_contract: refused without '%s':\n%s", says, text);
    return 0;
  }
  return refused ? 1 : 0;
}

static int expect_refused(const char *tag, const char *body,
                          const char *resolved, const char *says, int *fails) {
  char cfg_path[256];
  if (write_cfg(cfg_path, sizeof(cfg_path), tag, body) != 0) {
    printf("FAIL config_ports_contract: cannot write %s cfg\n", tag);
    return -1;
  }
  int refused = load_refused(cfg_path, resolved, says);
  (void)remove(cfg_path);
  if (refused != 1) {
    printf("FAIL config_ports_contract: %s was ACCEPTED (expected a "
           "refusal)\n",
           tag);
    (*fails)++;
  }
  return 0;
}

int main(void) {
  int fails = 0;
  char cfg_path[256];

  /* (A) local + no ports -> the runtime picks a base; node i on base + i.
     Four nodes, so a node's offset is checked past the first two. */
  if (write_cfg(cfg_path, sizeof(cfg_path), "localauto",
                "launcher=local\nnode_count=4\n") != 0) {
    printf("FAIL config_ports_contract: cannot write localauto cfg\n");
    return 1;
  }
  setenv("ARTS_CONFIG", cfg_path, 1);
  clean_env();
  struct arts_config_s config;
  arts_config_load(&config);
  if (config.port < ARTS_PORT_WINDOW_LO || config.port >= ARTS_PORT_WINDOW_HI) {
    printf("FAIL config_ports_contract: base %u outside the window %u-%u\n",
           config.port, ARTS_PORT_WINDOW_LO, ARTS_PORT_WINDOW_HI);
    fails++;
  }
  /* Every rank of a local run shares this machine's loopback interface and
     its processing units. */
  if (config.table == NULL || config.table_length != 4) {
    printf("FAIL config_ports_contract: routing table %p of %u nodes, want "
           "4\n",
           (void *)config.table, config.table_length);
    fails++;
  }
  if (!config.shared_pu_pool) {
    printf("FAIL config_ports_contract: a local run's nodes do not share "
           "the processing units\n");
    fails++;
  }
  for (unsigned int i = 0; config.table && i < config.table_length; i++) {
    if (config.table[i].ip_address == NULL ||
        strcmp(config.table[i].ip_address, "127.0.0.1") != 0) {
      printf("FAIL config_ports_contract: node %u address is %s, want "
             "127.0.0.1\n",
             i,
             config.table[i].ip_address ? config.table[i].ip_address
                                        : "(null)");
      fails++;
    }
    if (config.table[i].port != config.port + i) {
      printf("FAIL config_ports_contract: node %u listens on %u, want %u\n", i,
             config.table[i].port, config.port + i);
      fails++;
    }
  }
  arts_config_destroy(&config);
  (void)remove(cfg_path);

  /* (B) a local run may not be told which port to take. */
  if (expect_refused("localnamed",
                     "launcher=local\nnode_count=2\nports=25000\n", NULL, NULL,
                     &fails) != 0) {
    return 1;
  }

  /* (C) a remote launcher naming no port at all. */
  if (expect_refused("sshnoports",
                     "launcher=ssh\nnodes=n01,n02\nnode_count=2\n", NULL, NULL,
                     &fails) != 0) {
    return 1;
  }

  /* (D) a list or a range names more than the one port. */
  static const char *const many[] = {"25000,25001", "[25000-25001]"};
  for (int i = 0; i < 2; i++) {
    char body[128];
    snprintf(body, sizeof(body),
             "launcher=ssh\nnodes=n01,n02\nnode_count=2\nports=%s\n", many[i]);
    if (expect_refused(i == 0 ? "list" : "range", body, NULL,
                       "ports names the one port every rank listens on",
                       &fails) != 0) {
      return 1;
    }
  }

  /* (E) a remote launcher's port is every node's: its ranks sit on
     different hosts, so they never contend. */
  if (write_cfg(cfg_path, sizeof(cfg_path), "sshnamed",
                "launcher=ssh\nnodes=n01,n02\nnode_count=2\n"
                "ports=25000\n") != 0) {
    printf("FAIL config_ports_contract: cannot write sshnamed cfg\n");
    return 1;
  }
  setenv("ARTS_CONFIG", cfg_path, 1);
  clean_env();
  struct arts_config_s named;
  arts_config_load(&named);
  for (unsigned int i = 0; named.table && i < named.table_length; i++) {
    if (named.table[i].port != 25000) {
      printf("FAIL config_ports_contract: node %u listens on %u, want "
             "25000\n",
             i, named.table[i].port);
      fails++;
      break;
    }
  }
  arts_config_destroy(&named);
  (void)remove(cfg_path);

  /* (F) a local rank takes the base its spawner claimed, and a handoff of
     more than one port is refused. */
  if (write_cfg(cfg_path, sizeof(cfg_path), "localresolved",
                "launcher=local\nnode_count=2\n") != 0) {
    printf("FAIL config_ports_contract: cannot write localresolved cfg\n");
    return 1;
  }
  setenv("ARTS_CONFIG", cfg_path, 1);
  clean_env();
  setenv(ARTS_RESOLVED_PORT_ENV, "21234", 1);
  struct arts_config_s spawned;
  arts_config_load(&spawned);
  if (spawned.port != 21234) {
    printf("FAIL config_ports_contract: spawned rank took base %u, want "
           "21234\n",
           spawned.port);
    fails++;
  }
  if (spawned.table == NULL || spawned.table[1].port != 21235) {
    printf("FAIL config_ports_contract: spawned rank derived peer port %u, "
           "want 21235\n",
           spawned.table ? spawned.table[1].port : 0);
    fails++;
  }
  arts_config_destroy(&spawned);
  unsetenv(ARTS_RESOLVED_PORT_ENV);
  int refused = load_refused(cfg_path, "21234,21235", NULL);
  if (refused != 1) {
    printf("FAIL config_ports_contract: a handoff of two ports was "
           "ACCEPTED\n");
    fails++;
  }
  (void)remove(cfg_path);

  if (fails) {
    printf("FAIL config_ports_contract: %d check(s) failed\n", fails);
    return 1;
  }
  printf("PASS config_ports_contract: a local run claims its own ports, every "
         "other launcher must name its one port\n");
  return 0;
}
