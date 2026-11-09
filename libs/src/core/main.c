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
#define _FILE_OFFSET_BITS 64 // NOLINT(readability-identifier-naming)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "arts.h"
#include "arts/gas/guid.h"
#include "arts/runtime_state.h"
#include "arts/system/config.h"
#include "arts/system/print.h"
#include "arts/system/shutdown.h"
#include "arts/system/signals.h"
#include "arts/system/threads.h"
#include "arts/transport/socket.h"
#include "arts/transport/spawner.h"

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A weak reference: a binary that links the runtime without starting it needs
 * no entry, and arts_rt refuses to start a program no main_edt reached. */
#pragma weak main_edt

__attribute__((weak)) int arts_app_exit_status(void) { return 0; }

/* The CPUs this process may run on; every rank of a local run inherits the
 * same set. */
static int arts_rt_cpu_count(void) {
  for (size_t cpus = 1024; cpus <= ((size_t)1 << 20); cpus *= 2) {
    cpu_set_t *set = CPU_ALLOC(cpus);
    if (set == NULL) {
      break;
    }
    size_t size = CPU_ALLOC_SIZE(cpus);
    CPU_ZERO_S(size, set);
    if (sched_getaffinity(0, size, set) == 0) {
      int count = CPU_COUNT_S(size, set);
      CPU_FREE(set);
      return count;
    }
    CPU_FREE(set);
    if (errno != EINVAL) {
      break;
    }
  }
  ARTS_ERROR("cannot read the set of CPUs this process may run on");
}

int arts_rt(int argc, char **argv) {
  if (main_edt == NULL) {
    (void)fputs("[ARTS] arts_rt: no main_edt reached the runtime: none is "
                "defined, or it is only in an archive member the link did not "
                "pull\n",
                stderr);
    return 1;
  }
  arts_exit_process_init();
  arts_startup_begin();
  /* A rank a remote shell started in the background inherits SIGINT ignored;
   * the default lets an interrupt end it before its handling is installed. */
  (void)signal(SIGINT, SIG_DFL);
  struct arts_config_s config;
  arts_config_load(&config);

  /* Checks of the run as a whole, made by every process of it before any
   * rank starts.  GUID layout encodes rank in 14 bits, with the top value
   * reserved as headroom beside ARTS_DISTRIBUTED_RANK (0x3FFE).  Cap the
   * usable range at 2^14 - 2 = 16382 so encoded ranks never collide with the
   * sentinel.  Anything higher than the field width would silently truncate
   * during encoding. */
  if (config.table_length > (ARTS_GUID_RANK_MASK - 1U)) {
    ARTS_ERROR("Rank count %u exceeds GUID layout limit %u "
               "(14-bit rank field, two values reserved). "
               "Reduce node count or widen ARTS_GUID_RANK_BITS.",
               config.table_length, ARTS_GUID_RANK_MASK - 1U);
  }
  (void)arts_end_grace_s();
  arts_startup_check_run(&config);
  const bool local = strcmp(config.launcher, "local") == 0;
  if (local) {
    int cpus = arts_rt_cpu_count();
    if ((uint64_t)config.table_length * config.thread_count > (uint64_t)cpus) {
      ARTS_ERROR("launcher=local: %u ranks x %u threads exceed the %d CPUs "
                 "this process may run on",
                 config.table_length, config.thread_count, cpus);
    }
  }
  /* Under a launcher of this runtime's own, the process no launcher gave a
   * rank starts every rank of a run of several and is none of them. */
  if (getenv("ARTS_RANK") == NULL &&
      (local || strcmp(config.launcher, "ssh") == 0) &&
      config.table_length > 1) {
    arts_startup_end();
    arts_spawn(&config, argc, argv);
  }
  arts_startup_config_done(&config);
  /* A process of a run of several takes its rank from its launcher's word in
   * the transport setup below. */
  arts_global_rank_id = 0;
  arts_global_rank_count = config.table_length;
  if (config.table_length <= 1) {
    arts_exit_publish_rank(config.counter_folder);
  }

  /* Read before any other thread exists: the end that reads it can run on
   * the signal watcher. */
  arts_node_info.e2e_marker_enabled = (getenv("ARTS_E2E_MARKER") != NULL);
  arts_install_signal_handlers();
  /* Start the dedicated SIGTERM/SIGINT/SIGHUP/SIGALRM watcher thread BEFORE
   * any worker or progress thread is spawned, so the SIG_BLOCK mask is
   * inherited by every downstream thread. */
  arts_install_signal_watcher_thread();
  arts_configure_core_dumps(config.core_dump);

  if (config.table_length > 1) {
    arts_startup_step(ARTS_MESH_PHASE_RESOLVE);
    arts_socket_setup(&config);
    arts_exit_publish_rank(config.counter_folder);
  }
  arts_signal_handlers_set_rank(arts_global_rank_id);
  arts_global_master_rank_id = config.master_rank;

  if (arts_global_rank_count > 1) {
    arts_mesh_form(&config);
    arts_startup_step(ARTS_MESH_PHASE_FABRIC);
  }

  arts_thread_init(&config);
  arts_thread_zero_node_start(argc, argv, main_edt);
  arts_thread_main_loop();
}
