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
#include "arts/system/shutdown.h"

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "arts.h"
#include "arts/counter/Preamble.h"
#include "arts/counter/counter.h"
#include "arts/edt_context.h" /* arts_current_edt, the created-DB list */
#include "arts/memory/regpool.h"
#include "arts/runtime_state.h"
#include "arts/system/print.h"
#include "arts/system/signals.h"
#include "arts/transport/net.h"
#include "arts/transport/protocol.h" /* the SHUTDOWN frame's size */
#include "arts/transport/socket.h"
#include "arts/utils/stdatomic_compat.h"

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#include <sanitizer/lsan_interface.h>
#define ARTS_EXIT_LEAK_CHECK 1
#endif

struct arts_exit_meta_s arts_exit_meta;

/* The rank's state word: bit 31 taken, bits 28-30 the owner's origin, bits
 * 0-27 the owner thread's token.  Taken once, by a compare-exchange from 0;
 * every later reader is a compare-exchange or an acquire load of it. */
#define EXIT_TAKEN 0x80000000u
#define EXIT_ORIGIN_SHIFT 28
#define EXIT_TOKEN_MASK 0x0fffffffu
static _Atomic(uint32_t) g_exit_word;
/* The highest rc a late entry brought: a CAS-loop maximum of its own value. */
static _Atomic(int) g_late_rc;
/* The signal a FAULT owner ends by, stored at its take: only its own value. */
static _Atomic(int) g_fault_sig;
/* Tokens are drawn from 1 on, so a token never reads as a free word. */
static _Atomic(uint32_t) g_exit_next_token = 1;
static ARTS_THREAD_LOCAL_IE uint32_t t_exit_token;
/* Set by a WIRE take on its progress thread, which resumes at its loop's
 * top. */
static ARTS_THREAD_LOCAL_IE bool t_exit_resume;

/* The process whose end arts_exit runs, written before any other thread
 * exists: only its own value. */
static_assert(ATOMIC_INT_LOCK_FREE == 2,
              "a signal handler reads an atomic int");
static _Atomic(pid_t) g_exit_pid;
/* Release stores, each read after an acquire load: rank_known publishes the
 * rank id, the rank count and g_exit_folder; node_ready publishes the thread
 * table, the counter rows, the pool and the start stamp. */
static _Atomic(bool) g_rank_known;
static _Atomic(bool) g_node_ready;
static char g_exit_folder[PATH_MAX];

#ifdef ARTS_DEBUG_INJECTION
/* Raised by the rendezvous cut's hook; only its own value. */
static _Atomic(bool) g_exit_cut;
#endif

static uint32_t exit_token(void) {
  if (t_exit_token == 0) {
    t_exit_token = atomic_fetch_add_explicit(&g_exit_next_token, 1u,
                                             memory_order_relaxed) &
                   EXIT_TOKEN_MASK;
  }
  return t_exit_token;
}

static int exit_max(int a, int b) { return a > b ? a : b; }

static int exit_late_rc(void) {
  return atomic_load_explicit(&g_late_rc, memory_order_relaxed);
}

static int exit_owner_rc(void) {
  return atomic_load_explicit(&arts_exit_meta.rc, memory_order_relaxed);
}

static void exit_fold_late(int rc) {
  int cur = atomic_load_explicit(&g_late_rc, memory_order_relaxed);
  while (rc > cur && !atomic_compare_exchange_weak_explicit(
                         &g_late_rc, &cur, rc, memory_order_relaxed,
                         memory_order_relaxed)) {
  }
}

/* The process ends here.  A sanitizer interposes _exit with exit-time work of
 * its own, so the system call is made directly. */
static _Noreturn void exit_group_now(int status) {
  (void)syscall(SYS_exit_group, status);
  _exit(status);
}

/* A fault ends the process by its own signal, so the kernel reports it and
 * dumps a core where the limits allow; the owner may run inside the signal's
 * handler, which keeps it blocked. */
static _Noreturn void exit_raise(int sig) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  (void)sigaction(sig, &sa, NULL);
  sigset_t unblock;
  sigemptyset(&unblock);
  sigaddset(&unblock, sig);
  (void)pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
  (void)raise(sig);
  exit_group_now(128 + sig);
}

static _Noreturn void exit_end(int rc, enum arts_exit_origin origin) {
  if (origin == ARTS_EXIT_FAULT) {
    exit_raise(rc - 128);
  }
  exit_group_now(rc);
}

static bool exit_rank_known(void) {
  return atomic_load_explicit(&g_rank_known, memory_order_acquire);
}

/* Step 0: a process that is not a rank ends with its rc and nothing else. */
static void exit_guard(int rc, enum arts_exit_origin origin) {
  pid_t recorded = atomic_load_explicit(&g_exit_pid, memory_order_relaxed);
  if (recorded == 0) {
    (void)fflush(NULL);
    exit_group_now(rc);
  }
  if (recorded != getpid()) {
    if (origin == ARTS_EXIT_FAULT) {
      arts_signal_crash_report(rc - 128);
    }
    exit_group_now(rc);
  }
  if (!exit_rank_known()) {
    if (origin == ARTS_EXIT_FAULT) {
      arts_signal_crash_report(rc - 128);
    }
    (void)fflush(NULL);
    arts_fake_cxl_fini_replay();
    exit_end(rc, origin);
  }
}

/* Steps 1-2 once the word was taken: the record's stamps, the gate, the
 * owner's rc. */
static void exit_close(int rc, enum arts_exit_origin origin) {
  uint64_t t0 = arts_get_time_stamp();
  struct timespec now;
  (void)clock_gettime(CLOCK_REALTIME, &now);
  arts_exit_meta.t0_mono_ns = t0;
  arts_exit_meta.t0_realtime_ns =
      (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
  arts_exit_meta.origin = origin;
  if (arts_global_rank_count > 1) {
    /* A send that reads the gate closed is dropped; the post bypasses it. */
    arts_net_close_gate();
  }
  atomic_store_explicit(&arts_exit_meta.rc,
                        exit_max(rc, arts_app_exit_status()),
                        memory_order_relaxed);
  if (origin == ARTS_EXIT_FAULT) {
    atomic_store_explicit(&g_fault_sig, rc - 128, memory_order_relaxed);
    arts_signal_crash_report(rc - 128);
  }
}

bool arts_exit_take(int rc, enum arts_exit_origin origin) {
  uint32_t expected = 0;
  uint32_t taken =
      EXIT_TAKEN | ((uint32_t)origin << EXIT_ORIGIN_SHIFT) | exit_token();
  if (!atomic_compare_exchange_strong_explicit(&g_exit_word, &expected, taken,
                                               memory_order_acq_rel,
                                               memory_order_acquire)) {
    return false;
  }
  exit_close(rc, origin);
  if (origin == ARTS_EXIT_WIRE) {
    t_exit_resume = true;
  }
  return true;
}

/* A late entry: the rank's end is another thread's, or this thread re-entered
 * its own. */
static _Noreturn void exit_late(int rc, enum arts_exit_origin origin,
                                uint32_t word) {
  if ((word & EXIT_TOKEN_MASK) == exit_token()) {
    if (origin == ARTS_EXIT_FAULT) {
      exit_raise(rc - 128);
    }
    exit_group_now(exit_max(exit_max(exit_owner_rc(), rc), exit_late_rc()));
  }
  exit_fold_late(rc);
  if (origin == ARTS_EXIT_FAULT) {
    arts_signal_crash_report(rc - 128);
  }
  struct arts_thread_stop_s *slot = arts_thread_stop_slot;
  if (slot != NULL) {
    /* Read by the owner after its acquire of the stop below: the EDT this
     * thread runs and the blocks it holds stay as they are. */
    slot->caller_edt = arts_current_edt;
    slot->caller_dbs = arts_get_created_db_list();
    atomic_store_explicit(&slot->state,
                          origin == ARTS_EXIT_APP ? ARTS_THREAD_STOPPED
                                                  : ARTS_THREAD_STOPPED_UNSAFE,
                          memory_order_release);
  }
  for (;;) {
    (void)pause();
  }
}

_Noreturn void arts_exit(int rc, enum arts_exit_origin origin) {
  exit_guard(rc, origin);
  uint32_t expected = 0;
  uint32_t taken =
      EXIT_TAKEN | ((uint32_t)origin << EXIT_ORIGIN_SHIFT) | exit_token();
  if (!atomic_compare_exchange_strong_explicit(&g_exit_word, &expected, taken,
                                               memory_order_acq_rel,
                                               memory_order_acquire)) {
    exit_late(rc, origin, expected);
  }
  exit_close(rc, origin);
  arts_exit_resume();
}

/* The decimal digits of v, written backwards from end; returns the first. */
static char *exit_digits(char *end, uint64_t v) {
  do {
    *--end = (char)('0' + (v % 10u));
    v /= 10u;
  } while (v != 0u);
  return end;
}

static void exit_write_line(const char *head, uint64_t v) {
  char buf[64];
  char num[24];
  char *d = exit_digits(num + sizeof(num), v);
  size_t n = 0;
  for (const char *c = head; *c != '\0' && n < sizeof(buf) - 1; c++) {
    buf[n++] = *c;
  }
  while (d < num + sizeof(num) && n < sizeof(buf) - 1) {
    buf[n++] = *d++;
  }
  buf[n++] = '\n';
  arts_write_all(STDERR_FILENO, buf, n);
}

/* Step 3: the end of rank 0's application span, or the status that ended it.
 * The start stamp is the node's, published by node_ready. */
static void exit_marker(bool ready) {
  if (arts_global_rank_id != 0 || !arts_node_info.e2e_marker_enabled ||
      !ready) {
    return;
  }
  int rc = exit_max(exit_owner_rc(), exit_late_rc());
  if (rc == 0) {
    exit_write_line("[E2E] ",
                    arts_exit_meta.t0_mono_ns - arts_node_info.e2e_start_stamp);
  } else {
    exit_write_line("[SHUTDOWN] rc=", (uint64_t)rc);
  }
}

/* Step 4: the one post, counted as every accepted two-sided message is.  A
 * thread with no counter row of its own adds to thread 0's, which exists once
 * the node is ready. */
static void exit_post(enum arts_exit_origin origin, bool ready) {
  if (arts_exit_origin_originates(origin) && arts_socket_armed()) {
    unsigned int posted = arts_net_post_shutdown();
    uint64_t bytes = (uint64_t)posted * sizeof(struct arts_msg_header_s);
    ARTS_INFO("arts_exit: posted SHUTDOWN to %u peer(s)", posted);
    if (arts_thread_stop_slot != NULL) {
      INCREMENT_NUM_REMOTE_SEND_BY(posted);
      INCREMENT_BYTES_REMOTE_SENT_BY(bytes);
    } else if (ready) {
#if ENABLE_NUM_REMOTE_SEND
      arts_counter_increment_by(
          &arts_node_info.live_counters[0][NUM_REMOTE_SEND], posted);
#endif
#if ENABLE_BYTES_REMOTE_SENT
      arts_counter_increment_by(
          &arts_node_info.live_counters[0][BYTES_REMOTE_SENT], bytes);
#endif
    }
    (void)bytes;
  }
  arts_net_owner_posted();
}

_Noreturn void arts_exit_resume(void) {
  t_exit_resume = false;
  enum arts_exit_origin origin = arts_exit_meta.origin;
  bool ready = arts_exit_node_ready();
  ARTS_INFO("arts_exit: rank=%u origin=%s rc=%d", arts_global_rank_id,
            arts_exit_origin_name(origin), exit_owner_rc());
  exit_marker(ready);
  exit_post(origin, ready);
  arts_counter_capture_stop();
  if (ready) {
    arts_regpool_report_at_exit();
  }
  arts_counter_record();
  arts_runtime_teardown_oracle(ready);
#ifdef ARTS_EXIT_LEAK_CHECK
  __lsan_do_leak_check();
#endif
  arts_fake_cxl_fini_replay();
  (void)fflush(NULL);
  if (origin == ARTS_EXIT_FAULT) {
    exit_raise(atomic_load_explicit(&g_fault_sig, memory_order_relaxed));
  }
  exit_group_now(arts_exit_status());
}

void arts_exit_poll(void) {
  if (t_exit_resume) {
    arts_exit_resume();
  }
#ifdef ARTS_DEBUG_INJECTION
  if (atomic_load_explicit(&g_exit_cut, memory_order_relaxed) &&
      atomic_exchange_explicit(&g_exit_cut, false, memory_order_relaxed) &&
      arts_exit_take(0, ARTS_EXIT_APP)) {
    arts_exit_resume();
  }
#endif
}

#ifdef ARTS_DEBUG_INJECTION
void arts_exit_cut_raise(void) {
  atomic_store_explicit(&g_exit_cut, true, memory_order_relaxed);
}
#endif

bool arts_shutdown_taken(void) {
  return (atomic_load_explicit(&g_exit_word, memory_order_acquire) &
          EXIT_TAKEN) != 0u;
}

void arts_exit_process_init(void) {
  atomic_store_explicit(&g_exit_pid, getpid(), memory_order_relaxed);
}

void arts_exit_publish_rank(const char *counter_folder) {
  const char *f = counter_folder != NULL ? counter_folder : "";
  size_t n = strlen(f);
  if (n >= sizeof(g_exit_folder)) {
    ARTS_ERROR("counter_folder is longer than %zu bytes",
               sizeof(g_exit_folder) - 1);
  }
  memcpy(g_exit_folder, f, n + 1);
  atomic_store_explicit(&g_rank_known, true, memory_order_release);
}

void arts_exit_publish_node_ready(void) {
  atomic_store_explicit(&g_node_ready, true, memory_order_release);
}

bool arts_exit_node_ready(void) {
  return atomic_load_explicit(&g_node_ready, memory_order_acquire);
}

const char *arts_exit_counter_folder(void) { return g_exit_folder; }

int arts_exit_status(void) {
  return exit_max(exit_max(exit_owner_rc(), exit_late_rc()),
                  arts_app_exit_status());
}

/* The program ends the run (origin APP): the calling thread never returns. */
_Noreturn void arts_shutdown() { arts_exit(0, ARTS_EXIT_APP); }

_Noreturn void arts_abort(uint8_t error_code) {
  arts_exit(error_code, ARTS_EXIT_APP);
}
