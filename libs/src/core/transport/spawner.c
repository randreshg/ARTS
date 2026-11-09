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
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
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

#if ARTS_LOG_LEVEL >= 3
#define ARTS_SPAWNER_DEBUG(format, ...)                                        \
  arts_print_line(ARTS_CLR_DIM                                                 \
                  "[ARTS] [DEBUG] spawner: " format ARTS_CLR_RESET "\n",       \
                  ##__VA_ARGS__)
#else
#define ARTS_SPAWNER_DEBUG(...) ((void)0)
#endif

/* A command line under construction: `length` bytes and a terminator in the
 * `capacity` bytes of `text`. */
struct arts_cmd_s {
  char *text;
  size_t length;
  size_t capacity;
};

static void arts_cmd_reserve(struct arts_cmd_s *cmd, size_t more) {
  if (cmd->length + more < cmd->capacity) {
    return;
  }
  size_t capacity = cmd->capacity != 0 ? cmd->capacity : 256;
  while (capacity <= cmd->length + more) {
    capacity *= 2;
  }
  cmd->text = arts_realloc(cmd->text, capacity);
  cmd->capacity = capacity;
}

static void arts_cmd_put(struct arts_cmd_s *cmd, const char *text,
                         size_t length) {
  arts_cmd_reserve(cmd, length);
  memcpy(cmd->text + cmd->length, text, length);
  cmd->length += length;
  cmd->text[cmd->length] = '\0';
}

static void arts_cmd_puts(struct arts_cmd_s *cmd, const char *text) {
  arts_cmd_put(cmd, text, strlen(text));
}

/* Appends `value` as one shell word, whatever bytes it holds. */
static void arts_cmd_put_word(struct arts_cmd_s *cmd, const char *value) {
  const size_t room = (5 * strlen(value)) + 3;
  arts_cmd_reserve(cmd, room);
  (void)arts_shell_quote(value, cmd->text + cmd->length, room);
  cmd->length += strlen(cmd->text + cmd->length);
}

/* Whether a rank started through ssh is handed this variable: every ARTS_
 * variable and every variable named after a configuration key.  The name is
 * written unquoted, so only one of letters, digits and underscores qualifies;
 * the runtime reads no other.  ARTS_CONFIG is handed on made absolute. */
static bool arts_ssh_hands_on(const char *name, size_t name_length,
                              const char *const *keys, size_t key_count) {
  for (size_t j = 0; j < name_length; j++) {
    const char c = name[j];
    if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (j > 0 && c >= '0' && c <= '9'))) {
      return false;
    }
  }
  if (name_length == 11 && strncmp(name, "ARTS_CONFIG", 11) == 0) {
    return false;
  }
  if (name_length >= 5 && strncmp(name, "ARTS_", 5) == 0) {
    return true;
  }
  for (size_t k = 0; k < key_count; k++) {
    if (strlen(keys[k]) == name_length &&
        strncmp(keys[k], name, name_length) == 0) {
      return true;
    }
  }
  return false;
}

/* The session wrapper around the working directory and the rank's command.
 * The rank runs with the session's input closed; a reader takes each signal
 * name from that input (fd 3) and sends the signal to the rank, and at the
 * input's end-of-file (this process or its ssh client gone) kills it.  The
 * wrapper exits with the rank's status, which ssh relays. */
static const char arts_ssh_wrapper_start[] =
    " || exit 127; exec 3<&0 </dev/null; ";
static const char arts_ssh_wrapper_watch[] =
    " 3<&- & p=$!; ( while read -r s <&3; do kill -s \"$s\" \"$p\" "
    "2>/dev/null; done; kill -s KILL \"$p\" 2>/dev/null ) & w=$!; "
    "wait \"$p\"; rc=$?; kill \"$w\" 2>/dev/null; exit \"$rc\"";

/* The command a login shell on `rank`'s host runs, `sh -c <wrapper>`: the
 * wrapper changes to `cwd` and runs `exe` with this process's arguments after
 * the first, its environment extended by assignments that hand the rank the
 * variables arts_ssh_hands_on names, ARTS_CONFIG made absolute against `cwd`,
 * ARTS_RANK and LD_LIBRARY_PATH when set.  Every value is one quoted word, so
 * no word after the assignments is taken for one.  The caller frees the
 * result. */
static char *arts_ssh_command(unsigned int rank, int argc, char **argv,
                              const char *exe, const char *cwd) {
  struct arts_cmd_s run = {NULL, 0, 0};
  size_t key_count = 0;
  const char *const *keys = arts_config_key_names(&key_count);
  for (char **entry = environ; *entry != NULL; entry++) {
    const char *equals = strchr(*entry, '=');
    if (equals == NULL) {
      continue;
    }
    const size_t name_length = (size_t)(equals - *entry);
    if (!arts_ssh_hands_on(*entry, name_length, keys, key_count)) {
      continue;
    }
    arts_cmd_put(&run, *entry, name_length + 1);
    arts_cmd_put_word(&run, equals + 1);
    arts_cmd_puts(&run, " ");
  }

  /* A remote rank must not depend on its working directory to find the
   * configuration. */
  const char *config = getenv("ARTS_CONFIG");
  if (config == NULL || config[0] == '\0') {
    config = "arts.cfg";
  }
  struct arts_cmd_s config_path = {NULL, 0, 0};
  if (config[0] != '/') {
    arts_cmd_puts(&config_path, cwd);
    arts_cmd_puts(&config_path, "/");
  }
  arts_cmd_puts(&config_path, config);
  arts_cmd_puts(&run, "ARTS_CONFIG=");
  arts_cmd_put_word(&run, config_path.text);
  arts_free(config_path.text);

  char rank_text[24];
  (void)snprintf(rank_text, sizeof(rank_text), " ARTS_RANK=%u ", rank);
  arts_cmd_puts(&run, rank_text);
  const char *library_path = getenv("LD_LIBRARY_PATH");
  if (library_path != NULL) {
    arts_cmd_puts(&run, "LD_LIBRARY_PATH=");
    arts_cmd_put_word(&run, library_path);
    arts_cmd_puts(&run, " ");
  }
  arts_cmd_put_word(&run, exe);
  for (int j = 1; j < argc; j++) {
    arts_cmd_puts(&run, " ");
    arts_cmd_put_word(&run, argv[j]);
  }

  struct arts_cmd_s wrapper = {NULL, 0, 0};
  arts_cmd_puts(&wrapper, "cd ");
  arts_cmd_put_word(&wrapper, cwd);
  arts_cmd_puts(&wrapper, arts_ssh_wrapper_start);
  arts_cmd_put(&wrapper, run.text, run.length);
  arts_cmd_puts(&wrapper, arts_ssh_wrapper_watch);
  arts_free(run.text);
  ARTS_SPAWNER_DEBUG("ssh command for rank %u: %s", rank, wrapper.text);

  struct arts_cmd_s command = {NULL, 0, 0};
  arts_cmd_puts(&command, "sh -c ");
  arts_cmd_put_word(&command, wrapper.text);
  arts_free(wrapper.text);
  return command.text;
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
      arts_startup_budget_s() + (unsigned int)ARTS_EXIT_WATCHDOG_S;
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

/* Puts back the status flags this process was given on its stdout and stderr
 * (-1: none read).  An ssh client that shares them makes them non-blocking
 * while it runs, and leaves them so when it is killed. */
static void arts_spawner_restore_flags(const int given[2]) {
  for (int fd = STDOUT_FILENO; fd <= STDERR_FILENO; fd++) {
    if (given[fd - STDOUT_FILENO] >= 0) {
      (void)fcntl(fd, F_SETFL, given[fd - STDOUT_FILENO]);
    }
  }
}

_Noreturn void arts_spawn(struct arts_config_s *config, int argc, char **argv) {
  arts_exit_mark_spawner();
  const bool ssh = strcmp(config->launcher, "ssh") == 0;
  const unsigned int ranks = config->table_length;
  const unsigned int grace_s = arts_end_grace_s();

  char self_exe[PATH_MAX];
  ssize_t self_len = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
  if (self_len >= 0) {
    self_exe[self_len] = '\0';
  } else if (!ssh) {
    ARTS_ERROR("spawner: readlink(/proc/self/exe) failed: %s", strerror(errno));
  } else if (argc > 0 && argv[0] != NULL && argv[0][0] != '\0') {
    strncpy(self_exe, argv[0], sizeof(self_exe) - 1);
    self_exe[sizeof(self_exe) - 1] = '\0';
  } else {
    ARTS_ERROR("spawner: readlink(/proc/self/exe) failed (%s) and argv[0] "
               "names no program to start on the ranks' hosts",
               strerror(errno));
  }

  char cwd[PATH_MAX] = "";
  if (ssh) {
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
      ARTS_ERROR("spawner: getcwd failed: %s", strerror(errno));
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
  /* A reader of its output, or an ssh client of a rank's pipe, that went away
   * must not end the spawner, and with it every rank: a write to it fails
   * instead. */
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
  char *ssh_argv[] = {"ssh", "-T",
                      "-o",  "BatchMode=yes",
                      "-o",  "StrictHostKeyChecking=no",
                      "-o",  "ConnectTimeout=10",
                      "-o",  "ServerAliveInterval=5",
                      "-o",  "ServerAliveCountMax=3",
                      NULL,  NULL,
                      NULL};
  /* The vector ends with the host, the command and the terminator. */
  const size_t ssh_host = (sizeof(ssh_argv) / sizeof(ssh_argv[0])) - 3;

  pid_t *pids = arts_calloc(ranks, sizeof(pid_t));
  /* Under ssh, the write end of the pipe each rank's ssh client reads as its
   * input: the rank lives until that input ends, and a signal name written to
   * it reaches the rank as that signal. */
  int *lifelines = ssh ? arts_calloc(ranks, sizeof(int)) : NULL;
  const int given_flags[2] = {fcntl(STDOUT_FILENO, F_GETFL),
                              fcntl(STDERR_FILENO, F_GETFL)};
  const pid_t spawner = getpid();
  for (unsigned int i = 0; i < ranks; i++) {
    (void)snprintf(rank_var, sizeof(rank_var), "ARTS_RANK=%u", i);
    int lifeline[2] = {-1, -1};
    if (ssh) {
      if (pipe2(lifeline, O_CLOEXEC) != 0 ||
          fcntl(lifeline[1], F_SETFL, O_NONBLOCK) != 0) {
        arts_exit_watchdog_arm(75);
        arts_print_line("[ARTS] spawner: no pipe for rank %u: %s\n", i,
                        strerror(errno));
        arts_spawner_restore_flags(given_flags);
        (void)syscall(SYS_exit_group, 75);
        _exit(75);
      }
      ssh_argv[ssh_host] = config->table[i].ip_address;
      ssh_argv[ssh_host + 1] = arts_ssh_command(i, argc, argv, self_exe, cwd);
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
      if (ssh) {
        /* Only this process forwards a signal to an ssh rank, as a name on its
         * pipe: a signal sent to this process group, or to every process,
         * would otherwise end the client and its rank by SIGKILL.  The client
         * keeps a disposition it inherits as ignored, and ignoring before the
         * mask is restored also discards one pending since the fork. */
        (void)sigaction(SIGHUP, &ign, NULL);
        (void)sigaction(SIGINT, &ign, NULL);
        (void)sigaction(SIGQUIT, &ign, NULL);
        (void)sigaction(SIGTERM, &ign, NULL);
        (void)sigaction(SIGALRM, &ign, NULL);
      }
      (void)sigprocmask(SIG_SETMASK, &given_mask, NULL);
      if (ssh) {
        /* dup2 onto itself would keep the close-on-exec flag. */
        if (lifeline[0] == STDIN_FILENO) {
          (void)fcntl(STDIN_FILENO, F_SETFD, 0);
        } else {
          (void)dup2(lifeline[0], STDIN_FILENO);
        }
        (void)execvpe("ssh", ssh_argv, child_env);
      } else {
        /* Only rank 0 keeps the spawner's input. */
        if (i != 0) {
          const int null_fd = open("/dev/null", O_RDONLY);
          if (null_fd < 0 ||
              (null_fd != STDIN_FILENO &&
               (dup2(null_fd, STDIN_FILENO) < 0 || close(null_fd) != 0))) {
            arts_print_line("[ARTS] spawner: rank %u: no /dev/null on its "
                            "input: %s\n",
                            i, strerror(errno));
            _exit(127);
          }
        }
        (void)execve("/proc/self/exe", child_argv, child_env);
      }
      arts_print_line("[ARTS] spawner: rank %u: cannot run %s: %s\n", i,
                      ssh ? "ssh" : "/proc/self/exe", strerror(errno));
      _exit(127);
    }
    if (child < 0) {
      arts_exit_watchdog_arm(75);
      arts_print_line("[ARTS] spawner: fork failed for rank %u: %s\n", i,
                      strerror(errno));
      arts_spawner_restore_flags(given_flags);
      (void)syscall(SYS_exit_group, 75);
      _exit(75);
    }
    pids[i] = child;
    if (ssh) {
      (void)close(lifeline[0]);
      lifelines[i] = lifeline[1];
      arts_free(ssh_argv[ssh_host + 1]);
    }
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
        /* Every kill goes before any line: a line may wait for stderr to take
         * it, and the grace is a bound. */
        for (unsigned int i = 0; i < ranks; i++) {
          if (pids[i] != 0) {
            (void)kill(pids[i], SIGKILL);
          }
        }
        for (unsigned int i = 0; i < ranks; i++) {
          if (pids[i] != 0) {
            arts_print_line("[ARTS] spawner: end-of-step grace of %u s "
                            "expired; killing rank %u (pid %d)\n",
                            grace_s, i, (int)pids[i]);
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
      char line[8];
      const int line_length =
          snprintf(line, sizeof(line), "%s\n", arts_spawner_signal_name(sig));
      for (unsigned int i = 0; i < ranks; i++) {
        if (pids[i] != 0) {
          ARTS_SPAWNER_DEBUG("forward %s to rank %u (pid %d)",
                             arts_spawner_signal_name(sig), i, (int)pids[i]);
          if (ssh) {
            (void)!write(lifelines[i], line, (size_t)line_length);
          } else {
            (void)kill(pids[i], sig);
          }
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
      if (ssh) {
        (void)close(lifelines[i]);
      }
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

  arts_exit_watchdog_arm(worst);
  arts_spawner_restore_flags(given_flags);
  (void)fflush(NULL);
  arts_fake_cxl_fini_replay();
  (void)syscall(SYS_exit_group, worst);
  _exit(worst);
}
