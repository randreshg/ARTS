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
#include "arts/system/signals.h"

#include "arts/utils/stdatomic_compat.h"
#include <assert.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arts.h"
#include "arts/gas/guid.h"
#include "arts/gas/route_table.h"
#include "arts/runtime_state.h"
#include "arts/system/print.h"
#include "arts/system/shutdown.h"

/* The rank the handlers print: a handler may touch only lock-free atomic
 * objects, never arts_global_rank_id.  Stored once the rank is final; until
 * then it reads 0.  Only its own value. */
static_assert(ATOMIC_INT_LOCK_FREE == 2,
              "a signal handler reads an atomic unsigned int");
static _Atomic(unsigned int) handler_rank;

void arts_signal_handlers_set_rank(unsigned int rank) {
  atomic_store_explicit(&handler_rank, rank, memory_order_relaxed);
}

// Async-signal-safe signal name lookup (strsignal() is NOT safe).
static const char *signal_name(int sig) {
  switch (sig) {
  case SIGSEGV:
    return "SIGSEGV";
  case SIGBUS:
    return "SIGBUS";
  case SIGFPE:
    return "SIGFPE";
  case SIGTERM:
    return "SIGTERM";
  case SIGINT:
    return "SIGINT";
  case SIGALRM:
    return "SIGALRM";
  case SIGHUP:
    return "SIGHUP";
  default:
    return "UNKNOWN";
  }
}

// Async-signal-safe: compose "<pre><name> (rank <rank>)<post>" into one stack
// buffer and emit it with a SINGLE write().  A multi-write sequence interleaves
// with other still-running threads' log output (the crashing thread's siblings
// keep running until the handler re-raises), splitting the identifying line —
// which breaks log scrapers and any PASS regex that expects the substring
// contiguous.  snprintf is not async-signal-safe, so the rank is formatted by
// hand.  A single write() of < PIPE_BUF bytes is atomic, so nothing can wedge
// between the prefix and the signal name.
static void write_signal_line(const char *pre, const char *name,
                              unsigned int rank, const char *post) {
  char buf[192];
  size_t n = 0;
  const char *head[3] = {pre, name, " (rank "};
  for (int p = 0; p < 3; p++) {
    for (const char *s = head[p]; *s && n < sizeof(buf) - 1; s++) {
      buf[n++] = *s;
    }
  }
  char num[16];
  int pos = (int)sizeof(num);
  if (rank == 0) {
    num[--pos] = '0';
  } else {
    unsigned int v = rank;
    while (v > 0) {
      num[--pos] = (char)('0' + (v % 10));
      v /= 10;
    }
  }
  while (pos < (int)sizeof(num) && n < sizeof(buf) - 1) {
    buf[n++] = num[pos++];
  }
  for (const char *s = post; *s && n < sizeof(buf) - 1; s++) {
    buf[n++] = *s;
  }
  (void)write(STDERR_FILENO, buf, n);
}

static void write_backtrace(void) {
  void *frames[32];
  int depth = backtrace(frames, 32);
  backtrace_symbols_fd(frames, depth, STDERR_FILENO);
}

void arts_signal_crash_report(int sig) {
  write_signal_line("\n[ARTS] Crashed: ", signal_name(sig),
                    atomic_load_explicit(&handler_rank, memory_order_relaxed),
                    ") — stack trace:\n");
  write_backtrace();
}

/* Crash signals (SIGSEGV, SIGBUS, SIGFPE) end the rank with origin FAULT.
 * The default action comes back first, so the end raises the signal again
 * under it and a fault in the end itself takes the process at once; the
 * report prints once the end is armed. */
static void arts_crash_signal_handler(int sig) {
  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(sig, &sa, NULL);
  arts_exit(128 + sig, ARTS_EXIT_FAULT);
}

// Termination signals (SIGTERM, SIGINT, SIGALRM, SIGHUP) that reach a thread
// other than the watcher: the signal's own default action, after one line.
//
// IMPORTANT: only async-signal-safe operations here. Anything that takes
// stdio/malloc locks (e.g. fprintf) can deadlock against a worker already
// holding those locks, leaving the process hung instead of dying.
static void arts_term_signal_handler(int sig) {
  write_signal_line("\n[ARTS] Killed by ", signal_name(sig),
                    atomic_load_explicit(&handler_rank, memory_order_relaxed),
                    ")\n");

  write_backtrace();

  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(sig, &sa, NULL);
  (void)raise(sig);
}

/* Termination signals handled by the dedicated sigwait watcher thread
 * (see arts_install_signal_watcher_thread). Listed once so the install
 * routine and watcher loop agree on the set. */
static const int arts_term_sigs[] = {SIGTERM, SIGINT, SIGALRM, SIGHUP};
static const unsigned arts_term_sig_count =
    sizeof(arts_term_sigs) / sizeof(arts_term_sigs[0]);

void arts_install_signal_handlers(void) {
  /* backtrace() loads the unwinder on its first call, which allocates; making
   * that call here, outside any handler, keeps the handlers' calls free of
   * allocation. */
  void *warm[1];
  (void)backtrace(warm, 1);

  // Crash handlers: the rank ends with origin FAULT.
  struct sigaction crash_sa;
  crash_sa.sa_handler = arts_crash_signal_handler;
  sigemptyset(&crash_sa.sa_mask);
  crash_sa.sa_flags = 0;
  int crash_sigs[] = {SIGSEGV, SIGBUS, SIGFPE};
  for (int i = 0; i < 3; i++) {
    sigaction(crash_sigs[i], &crash_sa, NULL);
  }

  /* Termination signals are handled by the sigwait watcher thread (see
   * arts_install_signal_watcher_thread).  We still install
   * arts_term_signal_handler as a *fallback*: if a signal somehow
   * reaches a non-watcher thread (e.g. before the mask was inherited,
   * or when no watcher could start), we at least print a backtrace and
   * re-raise SIG_DFL to die cleanly. */
  struct sigaction term_sa;
  term_sa.sa_handler = arts_term_signal_handler;
  sigemptyset(&term_sa.sa_mask);
  term_sa.sa_flags = 0;
  for (unsigned i = 0; i < arts_term_sig_count; i++) {
    sigaction(arts_term_sigs[i], &term_sa, NULL);
  }

  // Ignore SIGPIPE — let socket writes fail with EPIPE instead of killing us.
  signal(SIGPIPE, SIG_IGN); // NOLINT(cert-err33-c)
}

/*--- Signal-watcher thread -------------------------------------------------*/

/* Dedicated thread that owns SIGTERM/SIGINT/SIGALRM/SIGHUP delivery via
 * sigwait(), so the rank's end runs in a normal thread context instead of
 * inside an async-unsafe signal handler.  The first signal ends the rank
 * (origin SIGNAL, status 128+sig); the watcher never returns from that end,
 * and a later signal stays blocked on every thread. */
static void *arts_signal_watcher_main(void *arg) {
  (void)arg;

  sigset_t wait_set;
  sigemptyset(&wait_set);
  for (unsigned i = 0; i < arts_term_sig_count; i++) {
    sigaddset(&wait_set, arts_term_sigs[i]);
  }

  int sig = 0;
  int rc = sigwait(&wait_set, &sig);
  if (rc != 0) {
    ARTS_WARN("signal watcher: sigwait failed (rc=%d), exiting", rc);
    return NULL;
  }
  write_signal_line("[ARTS] Received ", signal_name(sig),
                    atomic_load_explicit(&handler_rank, memory_order_relaxed),
                    "); ending the run\n");
  arts_exit(128 + sig, ARTS_EXIT_SIGNAL);
}

void arts_install_signal_watcher_thread(void) {
  /* Block the term signals on the calling (main) thread so that every
   * subsequently spawned thread inherits the block.  Only the watcher
   * thread will receive them via sigwait. */
  sigset_t block_set;
  sigemptyset(&block_set);
  for (unsigned i = 0; i < arts_term_sig_count; i++) {
    sigaddset(&block_set, arts_term_sigs[i]);
  }
  pthread_sigmask(SIG_BLOCK, &block_set, NULL);

  pthread_t watcher;
  if (pthread_create(&watcher, NULL, arts_signal_watcher_main, NULL) != 0) {
    ARTS_WARN("signal watcher: pthread_create failed; "
              "falling back to sigaction handlers");
    /* Unblock so the sigaction handler can still fire. */
    pthread_sigmask(SIG_UNBLOCK, &block_set, NULL);
    return;
  }
  (void)pthread_detach(watcher);
}

#ifndef __APPLE__

#include <sys/prctl.h>
#include <sys/resource.h>

void arts_configure_core_dumps(bool enabled) {
  struct rlimit limit;
  if (enabled) {
    (void)prctl(PR_SET_DUMPABLE, 1);
    limit.rlim_cur = RLIM_INFINITY;
    limit.rlim_max = RLIM_INFINITY;
  } else {
    /* Only the soft limit drops: a file-backed core pattern then writes
     * nothing, and the process stays dumpable so a debugger can still attach.
     * A piped core pattern is handed the crash regardless of this limit. */
    if (getrlimit(RLIMIT_CORE, &limit) != 0) {
      ARTS_WARN("Failed to read the core dump limit");
      return;
    }
    limit.rlim_cur = 0;
  }
  if (setrlimit(RLIMIT_CORE, &limit) != 0) {
    ARTS_WARN("Failed to %s core dumps", enabled ? "force" : "suppress");
  }
}

#else

void arts_configure_core_dumps(bool enabled) {
  (void)enabled;
  ARTS_INFO("Core dump control not supported on OS X.");
}

#endif
