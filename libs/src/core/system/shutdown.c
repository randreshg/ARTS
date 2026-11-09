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
#include <errno.h>
#include <limits.h>
#include <poll.h>
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
/* The signal a FAULT end ends by, stored before its watchdog is armed and
 * only by a FAULT end: only its own value. */
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

/* The watchdog: a CLOCK_MONOTONIC timer whose signal reaches whichever thread
 * the kernel picks.  Its fire reads only lock-free atomic objects, each a
 * value of its own (the deadline, the timer, the phase, the fault signal, the
 * owner's rc, the late rc, the spawner flag), and publishes nothing. */
static_assert(ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2,
              "a signal handler reads an atomic uint64_t");
static_assert(ATOMIC_POINTER_LOCK_FREE == 2 &&
                  sizeof(timer_t) == sizeof(void *),
              "a signal handler reads an atomic timer_t");
static_assert(ATOMIC_BOOL_LOCK_FREE == 2,
              "a signal handler reads an atomic bool");
#define EXIT_NS_PER_S 1000000000u
#define EXIT_WATCHDOG_NS ((uint64_t)ARTS_EXIT_WATCHDOG_S * EXIT_NS_PER_S)
/* The rate the record's estimated size is taken to be written at. */
#define EXIT_RECORD_BYTES_PER_S ((uint64_t)4 << 20)
/* The steps of an end, in their order, as the watchdog's line names them. */
enum exit_phase {
  EXIT_PHASE_POST,
  EXIT_PHASE_CAPTURE,
  EXIT_PHASE_REGPOOL,
  EXIT_PHASE_RECORD,
#ifdef ARTS_DEBUG_TEARDOWN
  EXIT_PHASE_ORACLE,
#endif
  EXIT_PHASE_LEAK,
  EXIT_PHASE_FLUSH,
};
/* Written once by arts_exit_process_init, before any other thread exists;
 * until then every arming is a no-op.  A child the process forks keeps the
 * flag but not the timer, and its end (step 0) arms nothing. */
static _Atomic(bool) g_exit_timer_made;
static _Atomic(timer_t) g_exit_timer;
/* The armed deadline, CLOCK_MONOTONIC ns; 0 while none was armed. */
static _Atomic(uint64_t) g_exit_deadline;
/* The step of the end in progress, stored by the end's thread. */
static _Atomic(unsigned int) g_exit_phase;
/* Set once by the spawner, whose watchdog line names no rank. */
static _Atomic(bool) g_exit_spawner;
/* The startup timer shares the watchdog's signal and handler; the value its
 * signal carries tells the two apart.  Its handle is written by
 * arts_exit_process_init before any other thread exists. */
#define EXIT_TIMER_WATCHDOG 0
#define EXIT_TIMER_STARTUP 1
static timer_t g_startup_timer;
/* Whether the startup bound is open.  Set by the main thread (release)
 * before the startup timer is armed, which publishes what arts_startup_begin
 * wrote before the arming (the budget, the deadline, the node name and the
 * config step) to the handler's acquire load; cleared (relaxed) before the
 * timer is disarmed, since a handler that reads false reads nothing else.
 * The steps at and past listen publish the rest of the line through
 * socket.c's step word. */
static _Atomic(bool) g_startup_open;

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

/* Sets `timer` to fire at `deadline`, or disarms it when deadline is 0. */
static void exit_settime(timer_t timer, uint64_t deadline) {
  struct itimerspec at;
  at.it_interval.tv_sec = 0;
  at.it_interval.tv_nsec = 0;
  at.it_value.tv_sec = (time_t)(deadline / EXIT_NS_PER_S);
  at.it_value.tv_nsec = (long)(deadline % EXIT_NS_PER_S);
  (void)timer_settime(timer, TIMER_ABSTIME, &at, NULL);
}

static void exit_watchdog_settime(uint64_t deadline) {
  exit_settime(atomic_load_explicit(&g_exit_timer, memory_order_relaxed),
               deadline);
}

/* Arms the watchdog to `deadline`: the value its handler judges a fire by,
 * then the timer.  Async-signal-safe. */
static void exit_watchdog_set(uint64_t deadline) {
  if (!atomic_load_explicit(&g_exit_timer_made, memory_order_relaxed)) {
    return;
  }
  atomic_store_explicit(&g_exit_deadline, deadline, memory_order_relaxed);
  exit_watchdog_settime(deadline);
}

/* The end of a process that is no rank begins, to end with rc, by signal sig
 * when sig is nonzero: from now on it is bounded by the watchdog, in phase
 * flush.  Async-signal-safe. */
static void exit_watchdog_arm_now(int rc, int sig) {
  atomic_store_explicit(&arts_exit_meta.rc, rc, memory_order_relaxed);
  if (sig != 0) {
    atomic_store_explicit(&g_fault_sig, sig, memory_order_relaxed);
  }
  atomic_store_explicit(&g_exit_phase, EXIT_PHASE_FLUSH, memory_order_relaxed);
  exit_watchdog_set(arts_get_time_stamp() + EXIT_WATCHDOG_NS);
}

void arts_exit_mark_spawner(void) {
  atomic_store_explicit(&g_exit_spawner, true, memory_order_relaxed);
}

void arts_exit_watchdog_arm(int rc) {
  const int given_errno = errno;
  exit_watchdog_arm_now(rc, 0);
  errno = given_errno;
}

static void exit_phase(enum exit_phase phase) {
  atomic_store_explicit(&g_exit_phase, (unsigned int)phase,
                        memory_order_relaxed);
}

/* Step 7's one move of the deadline, when the record writes anything: the
 * bound from the take plus the record's estimated write time. */
static void exit_watchdog_extend(uint64_t record_bytes) {
  if (record_bytes == 0u) {
    return;
  }
  const uint64_t write_ns =
      (record_bytes / EXIT_RECORD_BYTES_PER_S) * EXIT_NS_PER_S +
      (record_bytes % EXIT_RECORD_BYTES_PER_S) * EXIT_NS_PER_S /
          EXIT_RECORD_BYTES_PER_S;
  exit_watchdog_set(arts_exit_meta.t0_mono_ns + EXIT_WATCHDOG_NS + write_ns);
}

bool arts_exit_rank_known(void) {
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
  if (!arts_exit_rank_known()) {
    exit_watchdog_arm_now(rc, origin == ARTS_EXIT_FAULT ? rc - 128 : 0);
    if (origin == ARTS_EXIT_FAULT) {
      arts_signal_crash_report(rc - 128);
    }
    (void)fflush(NULL);
    arts_fake_cxl_fini_replay();
    exit_end(rc, origin);
  }
}

/* Steps 1-2 once the word was taken: the record's stamps, the watchdog armed
 * to T0 + the bound before anything that may wait (the program's status, the
 * crash report's unwinder), the gate, the owner's rc. */
static void exit_close(int rc, enum arts_exit_origin origin) {
  uint64_t t0 = arts_get_time_stamp();
  struct timespec now;
  (void)clock_gettime(CLOCK_REALTIME, &now);
  arts_exit_meta.t0_mono_ns = t0;
  arts_exit_meta.t0_realtime_ns =
      (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
  arts_exit_meta.origin = origin;
  if (origin == ARTS_EXIT_FAULT) {
    atomic_store_explicit(&g_fault_sig, rc - 128, memory_order_relaxed);
  }
  atomic_store_explicit(&arts_exit_meta.rc, rc, memory_order_relaxed);
  exit_phase(EXIT_PHASE_POST);
  exit_watchdog_set(t0 + EXIT_WATCHDOG_NS);
  if (arts_global_rank_count > 1) {
    /* A send that reads the gate closed is dropped; the post bypasses it. */
    arts_net_close_gate();
  }
  atomic_store_explicit(&arts_exit_meta.rc,
                        exit_max(rc, arts_app_exit_status()),
                        memory_order_relaxed);
  if (origin == ARTS_EXIT_FAULT) {
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
  exit_phase(EXIT_PHASE_CAPTURE);
  arts_counter_capture_stop();
  exit_phase(EXIT_PHASE_REGPOOL);
  if (ready) {
    arts_regpool_report_at_exit();
  }
  exit_phase(EXIT_PHASE_RECORD);
  exit_watchdog_extend(arts_counter_record_estimate());
  arts_counter_record();
#ifdef ARTS_DEBUG_TEARDOWN
  exit_phase(EXIT_PHASE_ORACLE);
  arts_runtime_teardown_oracle(ready);
#endif
#ifdef ARTS_EXIT_LEAK_CHECK
  exit_phase(EXIT_PHASE_LEAK);
  __lsan_do_leak_check();
#endif
  exit_phase(EXIT_PHASE_FLUSH);
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

bool arts_exit_began(void) {
  return arts_shutdown_taken() ||
         atomic_load_explicit(&g_exit_deadline, memory_order_relaxed) != 0u;
}

static const char *exit_phase_name(unsigned int phase) {
  switch (phase) {
  case EXIT_PHASE_POST:
    return "post";
  case EXIT_PHASE_CAPTURE:
    return "capture";
  case EXIT_PHASE_REGPOOL:
    return "regpool";
  case EXIT_PHASE_RECORD:
    return "record";
#ifdef ARTS_DEBUG_TEARDOWN
  case EXIT_PHASE_ORACLE:
    return "oracle";
#endif
  case EXIT_PHASE_LEAK:
    return "leak";
  case EXIT_PHASE_FLUSH:
    return "flush";
  }
  return "?";
}

/* Appends s to the n bytes of buf, keeping one byte for the newline. */
static size_t exit_put(char *buf, size_t cap, size_t n, const char *s) {
  while (*s != '\0' && n < cap - 1) {
    buf[n++] = *s++;
  }
  return n;
}

/* The watchdog's signal.  A fire before the deadline (the deadline moved
 * later, or the signal came from elsewhere) re-arms the same deadline, and
 * one with no deadline armed is ignored; either returns.  At the deadline the
 * end has not finished: one line, written only if stderr can take it within
 * a second, then a FAULT end raises its signal, the record's phase ends with
 * the status as it stands, a 0 included, and any other phase with that
 * status, or 71 when it is 0. */
static void exit_watchdog_fire(int sig) {
  (void)sig;
  const int given_errno = errno;
  const uint64_t deadline =
      atomic_load_explicit(&g_exit_deadline, memory_order_relaxed);
  if (deadline == 0u) {
    errno = given_errno;
    return;
  }
  if (arts_get_time_stamp() < deadline) {
    exit_watchdog_settime(deadline);
    errno = given_errno;
    return;
  }
  const unsigned int phase =
      atomic_load_explicit(&g_exit_phase, memory_order_relaxed);
  char line[160];
  size_t n = exit_put(line, sizeof(line), 0, "[ARTS] shutdown watchdog: ");
  if (atomic_load_explicit(&g_exit_spawner, memory_order_relaxed)) {
    n = exit_put(line, sizeof(line), n, "spawner");
  } else {
    char num[24];
    num[sizeof(num) - 1] = '\0';
    n = exit_put(line, sizeof(line), n, "rank ");
    n = exit_put(
        line, sizeof(line), n,
        exit_digits(num + sizeof(num) - 1, arts_signal_handlers_rank()));
  }
  n = exit_put(line, sizeof(line), n, " stuck in phase ");
  n = exit_put(line, sizeof(line), n, exit_phase_name(phase));
  if (phase == EXIT_PHASE_RECORD) {
    n = exit_put(line, sizeof(line), n, "; the counter record is lost");
  }
  line[n++] = '\n';
  /* A stderr whose reader stopped draining would block the write, and with
   * it the end the watchdog exists to bound; one still draining, momentarily
   * full, takes the line within the second. */
  struct pollfd err = {STDERR_FILENO, POLLOUT, 0};
  if (poll(&err, 1, 1000) == 1 && (err.revents & POLLOUT) != 0) {
    (void)write(STDERR_FILENO, line, n);
  }
  const int fault = atomic_load_explicit(&g_fault_sig, memory_order_relaxed);
  if (fault != 0) {
    exit_raise(fault);
  }
  const int status = exit_max(exit_owner_rc(), exit_late_rc());
  exit_group_now(status != 0 || phase == EXIT_PHASE_RECORD ? status : 71);
}

/* The startup timer's fire.  Async-signal-safe. */
static void exit_startup_fire(void) {
  const int given_errno = errno;
  if (!atomic_load_explicit(&g_startup_open, memory_order_acquire) ||
      arts_exit_began()) {
    errno = given_errno;
    return;
  }
  /* A startup line lists no rank, so it is far shorter than this. */
  char line[512];
  const size_t n = arts_startup_expired_line(line, sizeof(line));
  struct pollfd err = {STDERR_FILENO, POLLOUT, 0};
  if (poll(&err, 1, 1000) == 1 && (err.revents & POLLOUT) != 0) {
    (void)write(STDERR_FILENO, line, n);
  }
  exit_group_now(ARTS_MESH_EXIT_STATUS);
}

static void exit_timer_fire(int sig, siginfo_t *info, void *context) {
  (void)context;
  if (info != NULL && info->si_code == SI_TIMER &&
      info->si_value.sival_int == EXIT_TIMER_STARTUP) {
    exit_startup_fire();
    return;
  }
  exit_watchdog_fire(sig);
}

void arts_exit_process_init(void) {
  atomic_store_explicit(&g_exit_pid, getpid(), memory_order_relaxed);
  /* Made here, outside any end: timer_create is not async-signal-safe, and
   * an end may begin in a signal handler. */
  const int sig = SIGRTMIN + 1;
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = exit_timer_fire;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  /* A mask the process was started with may block the signal; every thread
   * it starts, and through the spawner every rank, inherits this thread's
   * mask, so the signal would stay pending on all of them. */
  sigset_t watchdog;
  sigemptyset(&watchdog);
  sigaddset(&watchdog, sig);
  struct sigevent ev;
  memset(&ev, 0, sizeof(ev));
  ev.sigev_notify = SIGEV_SIGNAL;
  ev.sigev_signo = sig;
  ev.sigev_value.sival_int = EXIT_TIMER_WATCHDOG;
  timer_t timer;
  if (sigaction(sig, &sa, NULL) != 0 ||
      pthread_sigmask(SIG_UNBLOCK, &watchdog, NULL) != 0 ||
      timer_create(CLOCK_MONOTONIC, &ev, &timer) != 0) {
    ARTS_ERROR("shutdown watchdog: cannot make its timer: %s", strerror(errno));
  }
  atomic_store_explicit(&g_exit_timer, timer, memory_order_relaxed);
  atomic_store_explicit(&g_exit_timer_made, true, memory_order_relaxed);
  ev.sigev_value.sival_int = EXIT_TIMER_STARTUP;
  if (timer_create(CLOCK_MONOTONIC, &ev, &g_startup_timer) != 0) {
    ARTS_ERROR("startup timer: cannot make it: %s", strerror(errno));
  }
}

void arts_exit_startup_arm(uint64_t at_ns) {
  if (!atomic_load_explicit(&g_exit_timer_made, memory_order_relaxed)) {
    return;
  }
  atomic_store_explicit(&g_startup_open, true, memory_order_release);
  exit_settime(g_startup_timer, at_ns);
}

void arts_exit_startup_disarm(void) {
  /* The main thread reads its own store. */
  if (!atomic_load_explicit(&g_startup_open, memory_order_relaxed)) {
    return;
  }
  atomic_store_explicit(&g_startup_open, false, memory_order_relaxed);
  exit_settime(g_startup_timer, 0u);
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
