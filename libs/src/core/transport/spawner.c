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
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "arts/transport/spawner.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "arts/system/config.h"
#include "arts/system/print.h"
#include "arts/system/shutdown.h"
#include "arts/transport/socket.h"
#include "arts/utils/malloc.h"

extern char **environ;

static int arts_shell_quote(const char *input, char *output,
                            size_t output_size) {
  size_t out_index = 0;

  if (output_size < 3) {
    return -1;
  }

  output[out_index++] = '\'';
  while (*input && out_index + 1 < output_size) {
    if (*input == '\'') {
      if (out_index + 5 >= output_size) {
        return -1;
      }
      output[out_index++] = '\'';
      output[out_index++] = '"';
      output[out_index++] = '\'';
      output[out_index++] = '"';
      output[out_index++] = '\'';
    } else {
      output[out_index++] = *input;
    }
    input++;
  }

  if (out_index + 2 > output_size) {
    return -1;
  }

  output[out_index++] = '\'';
  output[out_index] = '\0';
  return 0;
}

/* Saturating formatted append into a fixed-size buffer.
 *
 * `length` is the number of bytes already written (excluding the terminator)
 * and is updated in place to the new written length, always clamped to
 * `buf_size - 1` so the buffer stays null-terminatable at `buf[length]`.
 *
 * snprintf returns the length it WOULD have written had the buffer been
 * unbounded, which can exceed the space that remains.  Accumulating that raw
 * return value lets `length` run past `buf_size`, after which the remaining
 * size `buf_size - length` underflows and a later `buf[length]` indexes out of
 * bounds.  Guarding the remaining size against underflow and clamping the
 * running length keeps every write — including the terminating one — in
 * bounds even when the composed string is truncated. */
static void arts_cmd_appendf(char *buf, size_t buf_size, size_t *length,
                             const char *fmt, ...) {
  if (buf_size == 0 || *length >= buf_size) {
    if (buf_size != 0) {
      *length = buf_size - 1;
    }
    return;
  }

  size_t remaining = buf_size - *length;
  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(buf + *length, remaining, fmt, args);
  va_end(args);

  if (written < 0) {
    return;
  }
  if ((size_t)written >= remaining) {
    /* Truncated: the buffer is now full up to its last writable byte. */
    *length = buf_size - 1;
  } else {
    *length += (size_t)written;
  }
}

/* The shell command that starts `rank` on its host, `sh -c <quoted>`: it
 * changes to this process's working directory and runs this program by
 * absolute path with an absolute ARTS_CONFIG, the caller's LD_LIBRARY_PATH,
 * ARTS_RANK and this process's arguments.  False when it does not fit. */
static bool arts_ssh_command(unsigned int rank, int argc, char **argv,
                             const char *self_exe, const char *cwd,
                             const char *config_abs, char *wrapped,
                             size_t wrapped_size) {
  char command[4096];
  char quoted[(sizeof(command) * 6) + 8];
  size_t length = 0;

  arts_cmd_appendf(command, sizeof(command), &length, "cd %s && ", cwd);
  arts_cmd_appendf(command, sizeof(command), &length, "ARTS_CONFIG=%s ",
                   config_abs);
  const char *ld_library_path = getenv("LD_LIBRARY_PATH");
  if (ld_library_path) {
    arts_cmd_appendf(command, sizeof(command), &length, "LD_LIBRARY_PATH=%s ",
                     ld_library_path);
  }
  arts_cmd_appendf(command, sizeof(command), &length, "ARTS_RANK=%u ", rank);
  if (self_exe[0] != '\0') {
    arts_cmd_appendf(command, sizeof(command), &length, "%s", self_exe);
    for (int j = 1; j < argc; j++) {
      arts_cmd_appendf(command, sizeof(command), &length, " %s", argv[j]);
    }
  } else {
    for (int j = 0; j < argc; j++) {
      arts_cmd_appendf(command, sizeof(command), &length, " %s", argv[j]);
    }
  }
  command[length] = '\0';
  ARTS_DEBUG("SSH command[%u]: %s", rank, command);

  if (arts_shell_quote(command, quoted, sizeof(quoted)) != 0) {
    return false;
  }
  int n = snprintf(wrapped, wrapped_size, "sh -c %s", quoted);
  return n >= 0 && (size_t)n < wrapped_size;
}

/* A whole number of seconds in 1-86400: decimal digits only. */
static bool arts_grace_value(const char *text, unsigned int *out) {
  if (text[0] == '\0' || strspn(text, "0123456789") != strlen(text)) {
    return false;
  }
  unsigned long value = 0;
  for (const char *p = text; *p != '\0'; p++) {
    value = (value * 10u) + (unsigned long)(*p - '0');
    if (value > 86400ul) {
      return false;
    }
  }
  if (value == 0) {
    return false;
  }
  *out = (unsigned int)value;
  return true;
}

unsigned int arts_end_grace_s(void) {
  /* A rank that cannot finish its startup ends at the budget, and one that
   * is ending ends within the shutdown bound, so a grace below their sum
   * could kill a rank that is about to end on its own. */
  const unsigned int floor_s =
      (arts_startup_budget_ms() / 1000u) + (unsigned int)ARTS_EXIT_WATCHDOG_S;
  const char *text = getenv("ARTS_END_GRACE_S");
  if (text == NULL) {
    return floor_s > 30u ? floor_s : 30u;
  }
  unsigned int grace = 0;
  if (!arts_grace_value(text, &grace)) {
    ARTS_ERROR("ARTS_END_GRACE_S=\"%s\" is not a whole number of seconds in "
               "1-86400",
               text);
  }
  if (grace < floor_s) {
    ARTS_ERROR("ARTS_END_GRACE_S=%u is below this run's startup budget plus "
               "the shutdown bound (%u s)",
               grace, floor_s);
  }
  return grace;
}

/* The ranks of a local run re-read the same configuration, which names no
 * ports, so the block this process settled on reaches them only through the
 * environment they inherit. */
static void arts_export_local_ports(const struct arts_config_s *config) {
  char port_spec[512];
  size_t spec_len = 0;
  port_spec[0] = '\0';
  for (unsigned int j = 0; j < config->port_count && config->ports; j++) {
    int written = snprintf(port_spec + spec_len, sizeof(port_spec) - spec_len,
                           "%s%u", j == 0 ? "" : ",", config->ports[j]);
    if (written < 0 || (size_t)written >= sizeof(port_spec) - spec_len) {
      port_spec[0] = '\0';
      break;
    }
    spec_len += (size_t)written;
  }
  if (port_spec[0] != '\0') {
    setenv(ARTS_RESOLVED_PORTS_ENV, port_spec, 1);
  }
}

static const char *arts_spawner_signal_name(int sig) {
  switch (sig) {
  case SIGINT:
    return "INT";
  case SIGTERM:
    return "TERM";
  case SIGHUP:
    return "HUP";
  case SIGALRM:
    return "ALRM";
  }
  return "?";
}

#if ARTS_LOG_LEVEL >= 3
#define ARTS_SPAWNER_DEBUG(format, ...)                                        \
  arts_print_line(ARTS_CLR_DIM                                                 \
                  "[ARTS] [DEBUG] spawner: " format ARTS_CLR_RESET "\n",       \
                  ##__VA_ARGS__)
#else
#define ARTS_SPAWNER_DEBUG(...) ((void)0)
#endif

static struct timespec arts_spawner_now(void) {
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  return now;
}

static bool arts_spawner_reached(const struct timespec *now,
                                 const struct timespec *deadline) {
  return now->tv_sec > deadline->tv_sec ||
         (now->tv_sec == deadline->tv_sec && now->tv_nsec >= deadline->tv_nsec);
}

_Noreturn void arts_spawn(struct arts_config_s *config, int argc, char **argv) {
  const bool ssh = strcmp(config->launcher, "ssh") == 0;
  const unsigned int ranks = config->table_length;
  const unsigned int grace_s = arts_end_grace_s();

  char self_exe[PATH_MAX];
  ssize_t self_len = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
  if (self_len >= 0) {
    self_exe[self_len] = '\0';
  } else if (!ssh) {
    ARTS_ERROR("spawner: readlink(/proc/self/exe) failed: %s", strerror(errno));
  } else if (argc > 0 && argv[0] != NULL) {
    strncpy(self_exe, argv[0], sizeof(self_exe) - 1);
    self_exe[sizeof(self_exe) - 1] = '\0';
  } else {
    self_exe[0] = '\0';
  }

  char cwd[1024] = "";
  char config_abs[4096] = "";
  if (ssh) {
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
      ARTS_ERROR("spawner: getcwd failed: %s", strerror(errno));
    }
    /* A remote rank must not depend on its working directory to find the
     * configuration. */
    const char *env_cfg = getenv("ARTS_CONFIG");
    const char *cfg_src =
        (env_cfg && env_cfg[0] != '\0') ? env_cfg : "arts.cfg";
    if (cfg_src[0] == '/') {
      strncpy(config_abs, cfg_src, sizeof(config_abs) - 1);
      config_abs[sizeof(config_abs) - 1] = '\0';
    } else {
      (void)snprintf(config_abs, sizeof(config_abs), "%s/%s", cwd, cfg_src);
    }
  } else {
    (void)arts_transport_select_local_ports(config);
    arts_export_local_ports(config);
  }

  /* Every signal the spawner acts on is taken by sigwaitinfo and never
   * delivered, from before the first fork until the process ends, so none is
   * lost between a check and a wait.  SIGCHLD at its default action leaves
   * every ended rank to be reaped here. */
  sigset_t watched;
  sigemptyset(&watched);
  sigaddset(&watched, SIGCHLD);
  sigaddset(&watched, SIGINT);
  sigaddset(&watched, SIGTERM);
  sigaddset(&watched, SIGHUP);
  sigaddset(&watched, SIGALRM);
  sigset_t given_mask;
  (void)sigprocmask(SIG_BLOCK, &watched, &given_mask);
  struct sigaction dfl;
  memset(&dfl, 0, sizeof(dfl));
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  struct sigaction given_chld;
  (void)sigaction(SIGCHLD, &dfl, &given_chld);
  /* A reader of its output that went away must not end the spawner, and with
   * it every rank: a write to it fails instead. */
  struct sigaction ign;
  memset(&ign, 0, sizeof(ign));
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  struct sigaction given_pipe;
  (void)sigaction(SIGPIPE, &ign, &given_pipe);

  /* Each child's argument vector and environment are built here, so a child
   * only restores what this process was given and execs. */
  char **child_argv =
      arts_calloc((size_t)(argc > 0 ? argc : 1) + 1u, sizeof(char *));
  if (argc > 0) {
    for (int j = 0; j < argc; j++) {
      child_argv[j] = argv[j];
    }
  } else {
    child_argv[0] = self_exe;
  }
  size_t env_count = 0;
  while (environ[env_count] != NULL) {
    env_count++;
  }
  char **child_env = arts_calloc(env_count + 2u, sizeof(char *));
  memcpy(child_env, environ, env_count * sizeof(char *));
  char rank_var[32];
  child_env[env_count] = rank_var;
  char ssh_command[(4096 * 6) + 16];
  char *ssh_argv[] = {"ssh", "-n",
                      "-o",  "BatchMode=yes",
                      "-o",  "StrictHostKeyChecking=no",
                      "-o",  "ConnectTimeout=10",
                      "-o",  "ServerAliveInterval=5",
                      "-o",  "ServerAliveCountMax=3",
                      NULL,  ssh_command,
                      NULL};
  /* The vector ends with the host, the command and the terminator. */
  const size_t ssh_host = (sizeof(ssh_argv) / sizeof(ssh_argv[0])) - 3;

  pid_t *pids = arts_calloc(ranks, sizeof(pid_t));
  const pid_t spawner = getpid();
  for (unsigned int i = 0; i < ranks; i++) {
    (void)snprintf(rank_var, sizeof(rank_var), "ARTS_RANK=%u", i);
    if (ssh) {
      if (!arts_ssh_command(i, argc, argv, self_exe, cwd, config_abs,
                            ssh_command, sizeof(ssh_command))) {
        ARTS_ERROR("spawner: the ssh command for rank %u does not fit", i);
      }
      ssh_argv[ssh_host] = config->table[i].ip_address;
    }
    pid_t child = fork();
    if (child == 0) {
      /* A child whose spawner ended before the death signal was armed has
       * already been handed to another parent, and leaves. */
      (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
      if (getppid() != spawner) {
        _exit(0);
      }
      (void)sigaction(SIGCHLD, &given_chld, NULL);
      (void)sigaction(SIGPIPE, &given_pipe, NULL);
      (void)sigprocmask(SIG_SETMASK, &given_mask, NULL);
      if (ssh) {
        (void)execvpe("ssh", ssh_argv, child_env);
      } else {
        (void)execve("/proc/self/exe", child_argv, child_env);
      }
      _exit(127);
    }
    if (child < 0) {
      arts_print_line("[ARTS] spawner: fork failed for rank %u: %s\n", i,
                      strerror(errno));
      (void)syscall(SYS_exit_group, 75);
      _exit(75);
    }
    pids[i] = child;
  }

  unsigned int live = ranks;
  int worst = 0;
  bool grace_started = false;
  bool grace_spent = false;
  struct timespec deadline = {0, 0};
  while (live > 0) {
    siginfo_t info;
    int sig;
    if (grace_started && !grace_spent) {
      struct timespec now = arts_spawner_now();
      if (arts_spawner_reached(&now, &deadline)) {
        for (unsigned int i = 0; i < ranks; i++) {
          if (pids[i] != 0) {
            arts_print_line("[ARTS] spawner: end-of-step grace of %u s "
                            "expired; killing rank %u (pid %d)\n",
                            grace_s, i, (int)pids[i]);
            (void)kill(pids[i], SIGKILL);
          }
        }
        grace_spent = true;
        continue;
      }
      struct timespec left = {deadline.tv_sec - now.tv_sec,
                              deadline.tv_nsec - now.tv_nsec};
      if (left.tv_nsec < 0) {
        left.tv_sec--;
        left.tv_nsec += 1000000000L;
      }
      sig = sigtimedwait(&watched, &info, &left);
    } else {
      sig = sigwaitinfo(&watched, &info);
    }
    if (sig < 0) {
      continue;
    }
    if (sig != SIGCHLD) {
      for (unsigned int i = 0; i < ranks; i++) {
        if (pids[i] != 0) {
          ARTS_SPAWNER_DEBUG("forward %s to rank %u (pid %d)",
                             arts_spawner_signal_name(sig), i, (int)pids[i]);
          (void)kill(pids[i], sig);
        }
      }
      continue;
    }
    int status = 0;
    pid_t ended;
    while ((ended = waitpid(-1, &status, WNOHANG)) > 0) {
      unsigned int i = 0;
      while (i < ranks && pids[i] != ended) {
        i++;
      }
      if (i == ranks) {
        continue;
      }
      pids[i] = 0;
      live--;
      int s = 0;
      if (WIFEXITED(status)) {
        s = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        s = 128 + WTERMSIG(status);
      }
      ARTS_SPAWNER_DEBUG("rank %u (pid %d) reaped, status %d", i, (int)ended,
                         s);
      if (s != 0) {
        arts_print_line("[ARTS] spawner: rank %u (pid %d) ended with status "
                        "%d\n",
                        i, (int)ended, s);
      }
      if (s > worst) {
        worst = s;
      }
      if (!grace_started) {
        deadline = arts_spawner_now();
        deadline.tv_sec += (time_t)grace_s;
        grace_started = true;
      }
    }
  }

  (void)fflush(NULL);
  arts_fake_cxl_fini_replay();
  (void)syscall(SYS_exit_group, worst);
  _exit(worst);
}
