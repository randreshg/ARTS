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
#include "arts/transport/socket.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "arts.h"
#include "arts/runtime_state.h"
#include "arts/system/config.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/transport/connection.h"
#include "arts/transport/dispatcher.h"
#include "arts/transport/protocol.h"
#include "arts/utils/atomics.h"
#include "arts/utils/malloc.h"

struct arts_config_s *arts_global_message_table;
unsigned int ports;
static const char *arts_transport_name(void);
#ifdef ARTS_USE_RDMA
static bool arts_transport_uses_rdma_cached = true;
#endif
// SOCKETS!
int *remote_socket_send_list;
volatile unsigned int *volatile remote_socket_send_lock_list;
struct sockaddr_in *remote_server_send_list;
bool *remote_connection_alive;
static unsigned int *remote_send_success_count;
#ifdef ARTS_USE_RDMA
static unsigned int *remote_receive_success_count;
static bool *remote_receive_close_pending;
#endif
static unsigned int *remote_connect_success_seen;
static unsigned int *remote_connect_retry_count;
static uint64_t *remote_connect_retry_after;
#ifdef ARTS_USE_RDMA
static volatile unsigned int *remote_connect_abandoned_inflight;
static unsigned int *remote_connect_abandoned_generation;
static uint64_t *remote_connect_abandoned_until;
static volatile unsigned int *remote_connect_active_permit;
#endif

int *local_socket_recieve;
int *remote_socket_recieve_list;
fd_set read_set;
int max_fd;
struct sockaddr_in *remote_server_recieve_list;
struct pollfd *poll_incoming;
struct arts_pending_receive_socket_s {
  int fd;
  struct arts_pending_receive_socket_s *next;
};
static struct arts_pending_receive_socket_s **remote_pending_receive_sockets;
static struct arts_pending_receive_socket_s **remote_pending_receive_socket_tails;
static volatile unsigned int *remote_receive_socket_locks;
static ARTS_THREAD_LOCAL unsigned int thread_start;
static ARTS_THREAD_LOCAL unsigned int thread_stop;
static ARTS_THREAD_LOCAL char **bypass_buf;
static ARTS_THREAD_LOCAL uint64_t *bypass_packet_size;
static ARTS_THREAD_LOCAL int64_t *re_recieve_res;
static ARTS_THREAD_LOCAL void **re_recieve_packet;
static ARTS_THREAD_LOCAL bool *max_incoming;
static ARTS_THREAD_LOCAL bool max_out_working;
static ARTS_THREAD_LOCAL uint64_t next_lazy_accept_time;
#ifdef ARTS_USE_RDMA
static ARTS_THREAD_LOCAL bool rdma_accept_thread_active;
static ARTS_THREAD_LOCAL bool rdma_connect_helper_thread_active;
#endif

#define EDT_MUG_SIZE 32
#define PACKET_SIZE 4194304
#define INITIAL_OUT_SIZE 80000000
#define ARTS_CONNECT_TIMEOUT_MS 1000
#define ARTS_CONNECT_MAX_RETRIES 60
#define ARTS_CONNECT_RETRY_DELAY_US 100000
#define ARTS_RDMA_CONNECT_TIMEOUT_MS 3000
#define ARTS_RDMA_ACCEPT_HELLO_TIMEOUT_MS 3000
#define ARTS_RDMA_CONNECT_MAX_RETRIES 6
#define ARTS_RDMA_STARTUP_CONNECT_GRACE_MS 10000
#define ARTS_RDMA_ABANDONED_CONNECT_BACKOFF_US 1000000
#define ARTS_RDMA_CONNECT_STAGGER_US 50000
#define ARTS_RDMA_CONNECT_BETWEEN_US 5000
#define ARTS_RDMA_CONNECT_STEADY_BETWEEN_US 1000
#define ARTS_RDMA_ABANDONED_CONNECT_QUARANTINE_US 6000000
#define ARTS_LAZY_ACCEPT_POLL_MS 10
#define ARTS_LAZY_ACCEPT_DRAIN_LIMIT 128
#define ARTS_LAZY_ACCEPT_IDLE_INTERVAL_US 1000
#define ARTS_RDMA_ACCEPT_SLEEP_US 1000
#define ARTS_RDMA_EAGER_CONNECT 1
#define ARTS_RDMA_EAGER_CONNECT_ROUNDS 16
#define ARTS_RDMA_LISTENER_READY 1
#define ARTS_RDMA_LISTENER_READY_MODE "ring"
#define ARTS_RDMA_LISTENER_READY_TIMEOUT_MS 60000
#define ARTS_RDMA_LISTENER_READY_MAX_BACKOFF_US 1000000
#define ARTS_RDMA_FIRST_CONNECT_REFUSED_FATAL 1
#define ARTS_RDMA_ACCEPT_THREAD 1
#define ARTS_RDMA_ABANDON_CLOSE_AFTER_SEND 0
#define ARTS_RDMA_ABANDON_ABANDONED_CONNECT_CLOSE 0
/* Do not block shutdown indefinitely in rclose(). */
#define ARTS_RDMA_ABANDON_SHUTDOWN_CLOSE 1
#define ARTS_RDMA_CLOSE_WORKER_JOIN_MS 2000
#define ARTS_RDMA_FULL_DUPLEX 0
#define ARTS_RDMA_ALLOW_RSOCKET_REUSE 1
#define ARTS_RDMA_FORCE_RSOCKET_REUSE 0
#define ARTS_RDMA_CLOSE_AFTER_SEND 0
#define ARTS_RDMA_CLOSE_AFTER_SEND_EVERY 0
/* Optional rsocket provider tuning. A value of 0 leaves provider defaults in
 * place; some providers reject RDMA_INLINE with EINVAL. */
#define ARTS_RDMA_INLINE 0
#define ARTS_RDMA_MAX_ACTIVE_CONNECTS 1
#define ARTS_RDMA_CLOSE_WORKERS 4
#define ARTS_RDMA_SEND_MAX_BYTES 1048576
#define ARTS_RDMA_SEND_MAX_ITERS 256
#define ARTS_RDMA_RECV_PACKETS_PER_SOCKET 16
#define ARTS_MAX_PACKET_BYTES 1073741824ULL
#define ARTS_RDMA_SOCKET_RETIRE_MSG 0xFFFFFFFEu
#define ARTS_CONNECTION_HELLO_MAGIC 0x41525453u
#define ARTS_CONNECTION_HELLO_READY 0x1u
#define ARTS_TRANSPORT_TCP_NAME "tcp"
#define ARTS_TRANSPORT_RDMA_NAME "rdma-rsocket"

char *ip_list;
static volatile unsigned int remote_accept_lock = 0;
static volatile unsigned int remote_accept_next_port = 0;
static volatile unsigned int remote_incoming_connected_count = 0;
static volatile unsigned int remote_receiver_wakeup_started = 0;
static volatile unsigned int remote_transport_shutdown_started = 0;
static unsigned int remote_expected_incoming_count = 0;
#ifdef ARTS_USE_RDMA
static pthread_t rdma_accept_thread;
static volatile unsigned int rdma_accept_thread_started = 0;
static volatile uint64_t rdma_connect_attempt_count = 0;
static volatile uint64_t rdma_connect_success_count = 0;
static volatile uint64_t rdma_connect_fail_count = 0;
static volatile uint64_t rdma_connect_timeout_count = 0;
static volatile uint64_t rdma_startup_connect_refused_count = 0;
static volatile uint64_t rdma_listener_ready_incoming_count = 0;
static volatile uint64_t rdma_connect_abandoned_count = 0;
static volatile uint64_t rdma_connect_backoff_skip_count = 0;
static volatile uint64_t rdma_connect_helper_hello_count = 0;
static volatile uint64_t rdma_connect_helper_hello_fail_count = 0;
static volatile uint64_t rdma_connect_helper_active_count = 0;
static volatile uint64_t rdma_active_connect_permit_count = 0;
static volatile uint64_t rdma_active_connect_permit_peak = 0;
static volatile uint64_t rdma_active_connect_permit_wait_count = 0;
static volatile uint64_t rdma_accept_fd_count = 0;
static volatile uint64_t rdma_accept_attempt_count = 0;
static volatile uint64_t rdma_accept_success_count = 0;
static volatile uint64_t rdma_accept_eagain_count = 0;
static volatile uint64_t rdma_accept_hello_fail_count = 0;
static volatile uint64_t rdma_accept_hello_timeout_count = 0;
static volatile uint64_t rdma_pending_recv_depth = 0;
static volatile uint64_t rdma_pending_recv_peak = 0;
static volatile uint64_t rdma_recv_register_count = 0;
static volatile uint64_t rdma_recv_close_count = 0;
static volatile uint64_t rdma_recv_eof_count = 0;
static volatile uint64_t rdma_recv_error_count = 0;
static volatile uint64_t rdma_deferred_close_start_count = 0;
static volatile uint64_t rdma_deferred_close_done_count = 0;
static volatile uint64_t rdma_deferred_close_abandon_count = 0;
static volatile uint64_t rdma_deferred_close_inflight_count = 0;
static volatile uint64_t rdma_deferred_close_queue_depth = 0;
static volatile uint64_t rdma_deferred_close_queue_peak = 0;
static volatile uint64_t rdma_close_after_send_context_count = 0;
static volatile uint64_t rdma_close_receive_context_count = 0;
static volatile uint64_t rdma_close_discard_context_count = 0;
static volatile uint64_t rdma_close_other_context_count = 0;
static volatile uint64_t rdma_abandoned_connect_inflight_count = 0;
static volatile uint64_t rdma_send_cap_hit_count = 0;
static volatile uint64_t rdma_recv_cap_hit_count = 0;
static volatile uint64_t rdma_socket_retire_send_count = 0;
static volatile uint64_t rdma_socket_retire_send_fail_count = 0;
static volatile uint64_t rdma_socket_retire_recv_count = 0;
static volatile uint64_t rdma_full_duplex_connected_recv_count = 0;
static volatile uint64_t rdma_full_duplex_accepted_send_count = 0;
static volatile uint64_t rdma_full_duplex_skip_close_count = 0;
static volatile uint64_t rdma_full_duplex_detach_count = 0;
static volatile uint64_t rdma_provider_fanout_hint_count = 0;
static volatile unsigned int rdma_provider_fanout_warning_emitted = 0;
static volatile unsigned int rdma_startup_network_setup_active = 0;
static volatile unsigned int rdma_listener_readiness_completed = 0;
static volatile int rdma_last_connect_errno = 0;
static volatile int rdma_last_connect_peer = -1;
static volatile unsigned int rdma_last_connect_port = 0;
static volatile uint64_t rdma_next_summary_time = 0;
static volatile uint64_t rdma_startup_connect_grace_until = 0;
#endif

struct arts_connection_hello_s {
  uint32_t magic;
  uint32_t rank;
  uint32_t port;
  uint32_t flags;
};

#ifdef ARTS_USE_RDMA
enum arts_rdma_listener_ready_mode_e {
  ARTS_RDMA_LISTENER_READY_OFF = 0,
  ARTS_RDMA_LISTENER_READY_RING = 1,
  ARTS_RDMA_LISTENER_READY_FULL = 2,
};
#endif

static const char *arts_transport_name(void);
static unsigned int arts_env_uint(const char *name, unsigned int fallback);
static uint64_t arts_env_uint64(const char *name, uint64_t fallback);
static bool arts_close_send_after_complete_send(void);
static unsigned int arts_close_send_after_complete_send_every(void);
#ifdef ARTS_USE_RDMA
static void arts_rdma_maybe_print_summary(const char *reason);
static unsigned int arts_rdma_max_active_connects(void);
static bool arts_rdma_active_connect_throttle_enabled(void);
static bool arts_rdma_acquire_active_connect_permit_for_socket(
    int socket_index, int peer_rank, unsigned int port);
static bool arts_rdma_take_active_connect_permit_for_socket(int socket_index);
static void arts_rdma_release_active_connect_permit(const char *reason);
static bool arts_rdma_socket_has_active_connect_permit(int socket_index);
static unsigned int arts_lazy_accept_drain_limit(void);
static bool arts_remote_accept_pending(unsigned int limit, int first_timeout_ms);
static bool arts_send_connection_hello_with_flags(int socket_fd,
                                                  unsigned int port,
                                                  uint32_t flags, int *err_out);
static bool arts_send_connection_hello(int socket_fd, unsigned int port,
                                       int *err_out);
static bool arts_send_rdma_socket_retire(int socket_fd, unsigned int port,
                                         int *err_out);
static bool arts_rdma_allow_rsocket_reuse(void);
static bool arts_rdma_full_duplex_enabled(void);
static unsigned int arts_rdma_startup_connect_grace_ms(void);
static enum arts_rdma_listener_ready_mode_e arts_rdma_listener_ready_mode(void);
static bool arts_rdma_first_connect_refused_fatal(void);
static bool arts_rdma_should_defer_startup_connect_warn(int socket_index,
                                                        int last_errno);
static void arts_rdma_maybe_warn_provider_fanout_limit(
    int peer_rank, unsigned int port, const struct sockaddr_in *addr,
    unsigned int retry_count, int connect_errno);
static bool arts_rdma_wait_for_connect_helpers(void);
static void *arts_rdma_accept_thread_main(void *arg);
static void arts_rdma_start_accept_thread(void);
static void arts_rdma_stop_accept_thread(void);
static unsigned int
arts_rdma_mark_abandoned_connect_inflight(int socket_index, uint64_t now);
static void arts_rdma_clear_abandoned_connect_inflight(int socket_index,
                                                       unsigned int generation);
#endif

static void arts_close_socket_fd(int *socket_fd) {
  if (!socket_fd || *socket_fd < 0) {
    return;
  }

#ifndef ARTS_USE_RDMA
  RSHUTDOWN(*socket_fd, SHUT_RDWR);
#endif
  RCLOSE(*socket_fd);
  *socket_fd = -1;
}

#ifdef ARTS_USE_RDMA
static void arts_rdma_update_peak(volatile uint64_t *peak, uint64_t value) {
  uint64_t observed = *peak;
  while (value > observed) {
    if (__sync_bool_compare_and_swap(peak, observed, value)) {
      return;
    }
    observed = *peak;
  }
}

static void arts_rdma_sub_counter_floor_zero(volatile uint64_t *counter,
                                             uint64_t value) {
  uint64_t observed = *counter;
  while (observed > 0) {
    uint64_t next = observed > value ? observed - value : 0;
    if (__sync_bool_compare_and_swap(counter, observed, next)) {
      return;
    }
    observed = *counter;
  }
}

static void arts_rdma_note_deferred_close_context(const char *context) {
  if (!context) {
    __sync_fetch_and_add(&rdma_close_other_context_count, 1ULL);
  } else if (strcmp(context, "close_after_send") == 0) {
    __sync_fetch_and_add(&rdma_close_after_send_context_count, 1ULL);
  } else if (strcmp(context, "close_receive") == 0) {
    __sync_fetch_and_add(&rdma_close_receive_context_count, 1ULL);
  } else if (strcmp(context, "discard_unconnected") == 0) {
    __sync_fetch_and_add(&rdma_close_discard_context_count, 1ULL);
  } else {
    __sync_fetch_and_add(&rdma_close_other_context_count, 1ULL);
  }
}

static bool arts_rdma_abandon_close_after_send_enabled(void) {
  static bool initialized = false;
  static bool enabled = true;
  if (!initialized) {
    enabled = arts_env_uint("ARTS_RDMA_ABANDON_CLOSE_AFTER_SEND",
                            ARTS_RDMA_ABANDON_CLOSE_AFTER_SEND) != 0U;
    initialized = true;
  }
  return enabled;
}

static bool arts_rdma_abandon_abandoned_connect_close_enabled(void) {
  static bool initialized = false;
  static bool enabled = true;
  if (!initialized) {
    enabled =
        arts_env_uint("ARTS_RDMA_ABANDON_ABANDONED_CONNECT_CLOSE",
                      ARTS_RDMA_ABANDON_ABANDONED_CONNECT_CLOSE) != 0U;
    initialized = true;
  }
  return enabled;
}

static bool arts_rdma_abandon_shutdown_close_enabled(void) {
  static bool initialized = false;
  static bool enabled = true;
  if (!initialized) {
    enabled = arts_env_uint("ARTS_RDMA_ABANDON_SHUTDOWN_CLOSE",
                            ARTS_RDMA_ABANDON_SHUTDOWN_CLOSE) != 0U;
    initialized = true;
  }
  return enabled;
}

static bool arts_rdma_should_abandon_deferred_close(const char *context) {
  if (!context) {
    return false;
  }
  if (strcmp(context, "close_after_send") == 0) {
    return arts_rdma_abandon_close_after_send_enabled();
  }
  if (strcmp(context, "shutdown") == 0 ||
      strcmp(context, "shutdown-send") == 0 ||
      strcmp(context, "shutdown-local-recv") == 0 ||
      strcmp(context, "wakeup-local-recv") == 0) {
    return arts_rdma_abandon_shutdown_close_enabled();
  }
  return false;
}

static bool arts_rdma_allow_rsocket_reuse(void) {
  static bool initialized = false;
  static bool enabled = false;
  if (!initialized) {
    enabled = arts_env_uint("ARTS_RDMA_ALLOW_RSOCKET_REUSE",
                            ARTS_RDMA_ALLOW_RSOCKET_REUSE) != 0U;
    initialized = true;
  }
  return enabled;
}

static bool arts_rdma_force_rsocket_reuse(void) {
  static bool initialized = false;
  static bool enabled = false;
  if (!initialized) {
    enabled = arts_env_uint("ARTS_RDMA_FORCE_RSOCKET_REUSE",
                            ARTS_RDMA_FORCE_RSOCKET_REUSE) != 0U;
    initialized = true;
  }
  return enabled;
}

static void arts_rdma_warn_rsocket_reuse_fallback(const char *setting,
                                                  unsigned int requested) {
  static volatile unsigned int warning_emitted = 0;
  if (!__sync_bool_compare_and_swap(&warning_emitted, 0U, 1U)) {
    return;
  }
  arts_atomic_print(
      "[RDMA-WARN][rank=%u] %s ignoring %s=%u because rdma-rsocket reuse is "
      "not enabled; using one-shot close-after-send fallback.\n",
      arts_global_rank_id, arts_transport_name(),
      setting ? setting : "rdma reuse", requested);
}

static unsigned int arts_rdma_max_active_connects(void) {
  static bool initialized = false;
  static unsigned int max_active = ARTS_RDMA_MAX_ACTIVE_CONNECTS;
  if (!initialized) {
    max_active = arts_env_uint("ARTS_RDMA_MAX_ACTIVE_CONNECTS",
                               ARTS_RDMA_MAX_ACTIVE_CONNECTS);
    unsigned int rank_count = arts_global_message_table
                                  ? arts_global_message_table->table_length
                                  : arts_global_rank_count;
    bool force = arts_env_uint("ARTS_RDMA_FORCE_ACTIVE_CONNECTS", 0U) != 0U;
    if (max_active > 1U && rank_count > 1U && !force) {
      arts_atomic_print(
          "[RDMA-WARN][rank=%u] %s forcing ARTS_RDMA_MAX_ACTIVE_CONNECTS "
          "from %u to 1 for %u ranks; set ARTS_RDMA_FORCE_ACTIVE_CONNECTS=1 "
          "only for provider triage.\n",
          arts_global_rank_id, arts_transport_name(), max_active, rank_count);
      max_active = 1U;
    }
    initialized = true;
  }
  return max_active;
}

static bool arts_rdma_active_connect_throttle_enabled(void) {
  unsigned int max_active = arts_rdma_max_active_connects();
  return max_active != 0U && max_active != UINT_MAX &&
         arts_close_send_after_complete_send() &&
         arts_close_send_after_complete_send_every() == 1U &&
         !arts_rdma_full_duplex_enabled();
}

static void arts_rdma_release_active_connect_permit(const char *reason) {
  uint64_t active = rdma_active_connect_permit_count;
  while (active != 0ULL) {
    if (__sync_bool_compare_and_swap(&rdma_active_connect_permit_count, active,
                                     active - 1ULL)) {
      ARTS_TRACE_RDMA("active connect permit release reason=%s active=%llu",
                      reason ? reason : "unknown",
                      (unsigned long long)(active - 1ULL));
      arts_rdma_maybe_print_summary("active-connect-release");
      return;
    }
    active = rdma_active_connect_permit_count;
  }
  ARTS_TRACE_RDMA("active connect permit release skipped reason=%s active=0",
                  reason ? reason : "unknown");
}

static bool arts_rdma_socket_has_active_connect_permit(int socket_index) {
  return socket_index >= 0 && remote_connect_active_permit &&
         remote_connect_active_permit[socket_index] != 0U;
}

static bool arts_rdma_acquire_active_connect_permit_for_socket(
    int socket_index, int peer_rank, unsigned int port) {
  if (!arts_rdma_active_connect_throttle_enabled()) {
    return false;
  }
  if (socket_index < 0 || !remote_connect_active_permit) {
    return false;
  }
  if (arts_rdma_socket_has_active_connect_permit(socket_index)) {
    __sync_fetch_and_add(&rdma_connect_backoff_skip_count, 1ULL);
    arts_rdma_maybe_print_summary("active-connect-inflight");
    return false;
  }

  unsigned int max_active = arts_rdma_max_active_connects();
  uint64_t active = rdma_active_connect_permit_count;
  if (active < (uint64_t)max_active &&
      __sync_bool_compare_and_swap(&rdma_active_connect_permit_count, active,
                                   active + 1ULL)) {
    uint64_t acquired = active + 1ULL;
    arts_rdma_update_peak(&rdma_active_connect_permit_peak, acquired);
    __sync_lock_test_and_set(&remote_connect_active_permit[socket_index], 1U);
    ARTS_TRACE_RDMA("active connect permit acquire peer=%d port=%u "
                    "active=%llu max=%u waited=0",
                    peer_rank, port, (unsigned long long)acquired,
                    max_active);
    return true;
  }

  __sync_fetch_and_add(&rdma_active_connect_permit_wait_count, 1ULL);
  ARTS_TRACE_RDMA("active connect permit defer peer=%d port=%u active=%llu "
                  "max=%u",
                  peer_rank, port, (unsigned long long)active, max_active);
  arts_rdma_maybe_print_summary("active-connect-throttle");
  arts_remote_accept_pending(arts_lazy_accept_drain_limit(), 0);
  return false;
}

static bool arts_rdma_take_active_connect_permit_for_socket(int socket_index) {
  if (socket_index < 0 || !remote_connect_active_permit) {
    return false;
  }
  return __sync_bool_compare_and_swap(&remote_connect_active_permit[socket_index],
                                      1U, 0U);
}

struct arts_rdma_deferred_close_s {
  int socket_fd;
  bool shutdown_first;
  const char *context;
  bool release_active_connect_permit;
  struct arts_rdma_deferred_close_s *next;
};

static pthread_mutex_t rdma_close_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rdma_close_queue_cond = PTHREAD_COND_INITIALIZER;
static struct arts_rdma_deferred_close_s *rdma_close_queue_head = NULL;
static struct arts_rdma_deferred_close_s *rdma_close_queue_tail = NULL;
static volatile unsigned int rdma_close_workers_started = 0;
static volatile unsigned int rdma_close_queue_stopping = 0;
static pthread_t *rdma_close_worker_threads = NULL;
static unsigned int rdma_close_worker_count = 0;

static void arts_rdma_close_socket_now(int socket_fd, bool shutdown_first,
                                       const char *context) {
  ARTS_TRACE_RDMA("deferred close enter fd=%d context=%s shutdown=%u",
                  socket_fd, context ? context : "unknown",
                  shutdown_first ? 1U : 0U);
  __sync_fetch_and_add(&rdma_deferred_close_inflight_count, 1ULL);
  if (shutdown_first) {
    RSHUTDOWN(socket_fd, SHUT_RDWR);
  }
  RCLOSE(socket_fd);
  arts_rdma_sub_counter_floor_zero(&rdma_deferred_close_inflight_count, 1ULL);
  __sync_fetch_and_add(&rdma_deferred_close_done_count, 1ULL);
  arts_rdma_maybe_print_summary("deferred-close-done");
  ARTS_TRACE_RDMA("deferred close leave fd=%d context=%s", socket_fd,
                  context ? context : "unknown");
}

static unsigned int arts_rdma_connect_helper_shutdown_wait_ms(void) {
  static bool initialized = false;
  static unsigned int wait_ms = 0;
  if (!initialized) {
    wait_ms = arts_env_uint("ARTS_RDMA_CONNECT_HELPER_SHUTDOWN_WAIT_MS", 5000U);
    initialized = true;
  }
  return wait_ms;
}

static bool arts_rdma_wait_for_connect_helpers(void) {
  unsigned int wait_ms = arts_rdma_connect_helper_shutdown_wait_ms();
  uint64_t deadline = arts_get_time_stamp() + ((uint64_t)wait_ms * 1000000ULL);
  while (rdma_connect_helper_active_count != 0ULL) {
    if (wait_ms == 0 || arts_get_time_stamp() >= deadline) {
      arts_atomic_print(
          "[RDMA-WARN][rank=%u] %s cleanup continuing with %llu detached RDMA "
          "connect helper thread(s) still active; keeping abandoned-connect "
          "state alive to avoid use-after-free during provider teardown\n",
          arts_global_rank_id, arts_transport_name(),
          (unsigned long long)rdma_connect_helper_active_count);
      return false;
    }
    usleep(1000);
  }
  return true;
}

static void *arts_rdma_deferred_close_main(void *arg) {
  struct arts_rdma_deferred_close_s *close_arg =
      (struct arts_rdma_deferred_close_s *)arg;
  arts_rdma_close_socket_now(close_arg->socket_fd, close_arg->shutdown_first,
                             close_arg->context);
  if (close_arg->release_active_connect_permit) {
    arts_rdma_release_active_connect_permit(close_arg->context);
  }
  free(close_arg);
  return NULL;
}

static void *arts_rdma_deferred_close_worker_main(void *arg) {
  (void)arg;
  while (true) {
    pthread_mutex_lock(&rdma_close_queue_lock);
    while (!rdma_close_queue_head && !rdma_close_queue_stopping) {
      pthread_cond_wait(&rdma_close_queue_cond, &rdma_close_queue_lock);
    }
    if (!rdma_close_queue_head && rdma_close_queue_stopping) {
      pthread_mutex_unlock(&rdma_close_queue_lock);
      break;
    }
    struct arts_rdma_deferred_close_s *close_arg = rdma_close_queue_head;
    rdma_close_queue_head = close_arg->next;
    if (!rdma_close_queue_head) {
      rdma_close_queue_tail = NULL;
    }
    arts_rdma_sub_counter_floor_zero(&rdma_deferred_close_queue_depth, 1ULL);
    pthread_mutex_unlock(&rdma_close_queue_lock);

    close_arg->next = NULL;
    arts_rdma_close_socket_now(close_arg->socket_fd, close_arg->shutdown_first,
                               close_arg->context);
    if (close_arg->release_active_connect_permit) {
      arts_rdma_release_active_connect_permit(close_arg->context);
    }
    free(close_arg);
  }
  return NULL;
}

static bool arts_rdma_ensure_close_workers_locked(void) {
  if (rdma_close_workers_started) {
    return true;
  }
  if (rdma_close_queue_stopping) {
    return false;
  }

  unsigned int worker_count =
      arts_env_uint("ARTS_RDMA_CLOSE_WORKERS", ARTS_RDMA_CLOSE_WORKERS);
  if (worker_count == 0) {
    worker_count = 1;
  }

  pthread_t *threads = (pthread_t *)calloc(worker_count, sizeof(*threads));
  if (!threads) {
    return false;
  }

  pthread_attr_t attr;
  pthread_attr_init(&attr);
#ifdef PTHREAD_STACK_MIN
  pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN * 2);
#endif

  unsigned int started = 0;
  for (unsigned int i = 0; i < worker_count; i++) {
    pthread_t thread;
    int create_res =
        pthread_create(&thread, &attr, arts_rdma_deferred_close_worker_main,
                       NULL);
    if (create_res != 0) {
      ARTS_WARN("%s could not start deferred close worker %u/%u: %s",
                arts_transport_name(), i + 1U, worker_count,
                strerror(create_res));
      continue;
    }
    threads[started++] = thread;
  }
  pthread_attr_destroy(&attr);

  if (started == 0) {
    free(threads);
    return false;
  }

  rdma_close_worker_threads = threads;
  rdma_close_worker_count = started;
  __sync_lock_test_and_set(&rdma_close_workers_started, 1U);
  return true;
}

static bool arts_rdma_close_queue_enqueue(
    struct arts_rdma_deferred_close_s *close_arg) {
  if (!close_arg) {
    return false;
  }

  pthread_mutex_lock(&rdma_close_queue_lock);
  if (!arts_rdma_ensure_close_workers_locked()) {
    pthread_mutex_unlock(&rdma_close_queue_lock);
    return false;
  }
  close_arg->next = NULL;
  if (rdma_close_queue_tail) {
    rdma_close_queue_tail->next = close_arg;
  } else {
    rdma_close_queue_head = close_arg;
  }
  rdma_close_queue_tail = close_arg;
  uint64_t depth =
      __sync_add_and_fetch(&rdma_deferred_close_queue_depth, 1ULL);
  arts_rdma_update_peak(&rdma_deferred_close_queue_peak, depth);
  pthread_cond_signal(&rdma_close_queue_cond);
  pthread_mutex_unlock(&rdma_close_queue_lock);
  return true;
}

static unsigned int arts_rdma_close_worker_join_ms(void) {
  static bool initialized = false;
  static unsigned int join_ms = 0;
  if (!initialized) {
    join_ms = arts_env_uint("ARTS_RDMA_CLOSE_WORKER_JOIN_MS",
                            ARTS_RDMA_CLOSE_WORKER_JOIN_MS);
    initialized = true;
  }
  return join_ms;
}

static void arts_rdma_close_worker_shutdown(void) {
  pthread_t *threads = NULL;
  unsigned int worker_count = 0;

  pthread_mutex_lock(&rdma_close_queue_lock);
  if (rdma_close_workers_started) {
    __sync_lock_test_and_set(&rdma_close_queue_stopping, 1U);
    pthread_cond_broadcast(&rdma_close_queue_cond);
    threads = rdma_close_worker_threads;
    worker_count = rdma_close_worker_count;
  }
  pthread_mutex_unlock(&rdma_close_queue_lock);

  if (worker_count == 0) {
    free(threads);
    return;
  }

  bool abandon = arts_rdma_abandon_shutdown_close_enabled();
  unsigned int join_ms = arts_rdma_close_worker_join_ms();
  unsigned int abandoned = 0;

  for (unsigned int i = 0; i < worker_count; i++) {
    if (!abandon || join_ms == 0) {
      pthread_join(threads[i], NULL);
      continue;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(join_ms / 1000U);
    deadline.tv_nsec += (long)((join_ms % 1000U) * 1000000UL);
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec += 1;
      deadline.tv_nsec -= 1000000000L;
    }
    int rc = pthread_timedjoin_np(threads[i], NULL, &deadline);
    if (rc != 0) {
      pthread_detach(threads[i]);
      abandoned++;
    }
  }

  if (abandoned != 0) {
    ARTS_INFO("%s shutdown detached %u deferred-close worker(s) still in "
              "rclose(); process teardown will reclaim provider state",
              arts_transport_name(), abandoned);
    /* Leak threads array + queue state: a detached worker may still touch
     * them. Safe -- process is exiting. */
    return;
  }

  free(threads);

  pthread_mutex_lock(&rdma_close_queue_lock);
  rdma_close_worker_threads = NULL;
  rdma_close_worker_count = 0;
  rdma_close_workers_started = 0;
  rdma_close_queue_stopping = 0;
  rdma_close_queue_head = NULL;
  rdma_close_queue_tail = NULL;
  pthread_mutex_unlock(&rdma_close_queue_lock);
}

static void arts_defer_rdma_close_socket_fd_with_permit(
    int *socket_fd, bool shutdown_first, const char *context,
    bool release_active_connect_permit) {
  if (!socket_fd || *socket_fd < 0) {
    if (release_active_connect_permit) {
      arts_rdma_release_active_connect_permit(context);
    }
    return;
  }

  int fd = *socket_fd;
  *socket_fd = -1;
  arts_rdma_note_deferred_close_context(context);

  if (arts_rdma_should_abandon_deferred_close(context)) {
    __sync_fetch_and_add(&rdma_deferred_close_abandon_count, 1ULL);
    arts_rdma_maybe_print_summary("deferred-close-abandon");
    ARTS_TRACE_RDMA("deferred close abandoned fd=%d context=%s", fd,
                    context ? context : "unknown");
    if (release_active_connect_permit) {
      arts_rdma_release_active_connect_permit(context);
    }
    return;
  }

  struct arts_rdma_deferred_close_s *close_arg =
      (struct arts_rdma_deferred_close_s *)calloc(1, sizeof(*close_arg));
  if (!close_arg) {
    ARTS_WARN("%s could not allocate deferred close for fd=%d (%s); "
              "abandoning fd to keep RDMA sender progress",
              arts_transport_name(), fd, context ? context : "unknown");
    if (release_active_connect_permit) {
      arts_rdma_release_active_connect_permit(context);
    }
    return;
  }
  close_arg->socket_fd = fd;
  close_arg->shutdown_first = shutdown_first;
  close_arg->context = context;
  close_arg->release_active_connect_permit = release_active_connect_permit;

  if (!arts_rdma_close_queue_enqueue(close_arg)) {
    if (remote_transport_shutdown_started || rdma_close_queue_stopping) {
      arts_rdma_close_socket_now(close_arg->socket_fd,
                                 close_arg->shutdown_first,
                                 close_arg->context);
      if (close_arg->release_active_connect_permit) {
        arts_rdma_release_active_connect_permit(close_arg->context);
      }
      free(close_arg);
      return;
    }
    pthread_t thread;
    int create_res =
        pthread_create(&thread, NULL, arts_rdma_deferred_close_main, close_arg);
    if (create_res == 0) {
      pthread_detach(thread);
      __sync_fetch_and_add(&rdma_deferred_close_start_count, 1ULL);
      arts_rdma_maybe_print_summary("deferred-close-start");
      ARTS_TRACE_RDMA("deferred close scheduled fd=%d context=%s shutdown=%u",
                      fd, context ? context : "unknown",
                      shutdown_first ? 1U : 0U);
      return;
    }
    ARTS_WARN("%s could not start deferred close for fd=%d (%s): %s; "
              "abandoning fd to keep RDMA sender progress",
              arts_transport_name(), fd, context ? context : "unknown",
              strerror(create_res));
    if (close_arg->release_active_connect_permit) {
      arts_rdma_release_active_connect_permit(close_arg->context);
    }
    free(close_arg);
    return;
  }
  __sync_fetch_and_add(&rdma_deferred_close_start_count, 1ULL);
  arts_rdma_maybe_print_summary("deferred-close-start");
  ARTS_TRACE_RDMA("deferred close scheduled fd=%d context=%s shutdown=%u", fd,
                  context ? context : "unknown", shutdown_first ? 1U : 0U);
}

static void arts_defer_rdma_close_socket_fd(int *socket_fd,
                                            bool shutdown_first,
                                            const char *context) {
  arts_defer_rdma_close_socket_fd_with_permit(socket_fd, shutdown_first,
                                              context, false);
}
#endif

static void arts_discard_unconnected_socket_fd_with_permit(
    int *socket_fd, bool release_active_connect_permit) {
  if (!socket_fd || *socket_fd < 0) {
#ifdef ARTS_USE_RDMA
    if (release_active_connect_permit) {
      arts_rdma_release_active_connect_permit("discard_unconnected");
    }
#else
    (void)release_active_connect_permit;
#endif
    return;
  }

#ifdef ARTS_USE_RDMA
  /*
   * librdmacm/rsocket close can block after a failed nonblocking rconnect.
   * Do not hold the sender thread on cleanup for a socket that is about to be
   * retried; progress on later peers is more important than synchronous close.
   */
  arts_defer_rdma_close_socket_fd_with_permit(
      socket_fd, false, "discard_unconnected", release_active_connect_permit);
#else
  (void)release_active_connect_permit;
  RCLOSE(*socket_fd);
  *socket_fd = -1;
#endif
}

static void arts_discard_unconnected_socket_fd(int *socket_fd) {
  arts_discard_unconnected_socket_fd_with_permit(socket_fd, false);
}

static void arts_abandon_unconnected_socket_fd(int *socket_fd) {
  if (!socket_fd || *socket_fd < 0) {
    return;
  }

  *socket_fd = -1;
}

static void arts_close_socket_fd_everywhere(int *socket_fd) {
  if (!socket_fd || *socket_fd < 0) {
    return;
  }

  int fd = *socket_fd;
#ifndef ARTS_USE_RDMA
  RSHUTDOWN(fd, SHUT_RDWR);
#endif
  RCLOSE(fd);

  if (arts_global_message_table) {
    int count = (int)arts_global_message_table->table_length;
    for (int i = 0; remote_socket_send_list && i < count * (int)ports; i++) {
      if (remote_socket_send_list[i] == fd) {
        remote_socket_send_list[i] = -1;
      }
    }

    int incoming_count = (count > 0) ? (count - 1) * (int)ports : 0;
    for (int i = 0; remote_socket_recieve_list && i < incoming_count; i++) {
      if (remote_socket_recieve_list[i] == fd) {
        remote_socket_recieve_list[i] = -1;
      }
    }

    for (int i = 0; local_socket_recieve && i < (int)ports; i++) {
      if (local_socket_recieve[i] == fd) {
        local_socket_recieve[i] = -1;
      }
    }
  }
}

static void arts_receive_slot_lock(int socket_index) {
  if (remote_receive_socket_locks && socket_index >= 0) {
    arts_lock(&remote_receive_socket_locks[socket_index]);
  }
}

static void arts_receive_slot_unlock(int socket_index) {
  if (remote_receive_socket_locks && socket_index >= 0) {
    arts_unlock(&remote_receive_socket_locks[socket_index]);
  }
}

static int arts_receive_index_peer_rank(int socket_index) {
  if (!arts_global_message_table || ports == 0 || socket_index < 0) {
    return -1;
  }
  int peer_slot = socket_index / (int)ports;
  int my_rank = (int)arts_global_message_table->my_rank;
  return (peer_slot < my_rank) ? peer_slot : peer_slot + 1;
}

static unsigned int arts_receive_index_port(int socket_index) {
  return (ports == 0 || socket_index < 0) ? 0U
                                          : (unsigned int)(socket_index %
                                                           (int)ports);
}

static void arts_install_receive_socket_locked(int socket_index,
                                               int socket_fd) {
  remote_socket_recieve_list[socket_index] = socket_fd;
  poll_incoming[socket_index].fd = socket_fd;
  poll_incoming[socket_index].events = POLLIN;
  poll_incoming[socket_index].revents = 0;
#ifdef ARTS_USE_RDMA
  if (remote_receive_success_count) {
    remote_receive_success_count[socket_index] = 0;
  }
  if (remote_receive_close_pending) {
    remote_receive_close_pending[socket_index] = false;
  }
#endif
}

static bool arts_queue_pending_receive_socket_locked(int socket_index,
                                                     int socket_fd,
                                                     const char *source) {
  if (!remote_pending_receive_sockets ||
      !remote_pending_receive_socket_tails || socket_fd < 0) {
    return false;
  }

  struct arts_pending_receive_socket_s *pending =
      (struct arts_pending_receive_socket_s *)arts_malloc(sizeof(*pending));
  pending->fd = socket_fd;
  pending->next = NULL;

  if (remote_pending_receive_socket_tails[socket_index]) {
    remote_pending_receive_socket_tails[socket_index]->next = pending;
  } else {
    remote_pending_receive_sockets[socket_index] = pending;
  }
  remote_pending_receive_socket_tails[socket_index] = pending;

#ifdef ARTS_USE_RDMA
  uint64_t pending_depth =
      __sync_add_and_fetch(&rdma_pending_recv_depth, 1ULL);
  arts_rdma_update_peak(&rdma_pending_recv_peak, pending_depth);
#endif
  ARTS_TRACE_RDMA("queue pending recv source=%s index=%d peer=%d port=%u fd=%d "
                  "active_fd=%d",
                  source ? source : "unknown", socket_index,
                  arts_receive_index_peer_rank(socket_index),
                  arts_receive_index_port(socket_index), socket_fd,
                  remote_socket_recieve_list
                      ? remote_socket_recieve_list[socket_index]
                                             : -1);
  return true;
}

static bool arts_promote_pending_receive_socket_locked(int socket_index,
                                                       const char *reason) {
  if (!remote_pending_receive_sockets ||
      !remote_pending_receive_socket_tails || !remote_socket_recieve_list ||
      !poll_incoming) {
    return false;
  }

  struct arts_pending_receive_socket_s *pending =
      remote_pending_receive_sockets[socket_index];
  if (!pending) {
    return false;
  }

  remote_pending_receive_sockets[socket_index] = pending->next;
  if (!remote_pending_receive_sockets[socket_index]) {
    remote_pending_receive_socket_tails[socket_index] = NULL;
  }

  int socket_fd = pending->fd;
  arts_free(pending);
#ifdef ARTS_USE_RDMA
  arts_rdma_sub_counter_floor_zero(&rdma_pending_recv_depth, 1ULL);
#endif
  arts_install_receive_socket_locked(socket_index, socket_fd);
  INCREMENT_NUM_REMOTE_PENDING_RECV_PROMOTE_BY(1);
  ARTS_TRACE_RDMA("promote pending recv reason=%s index=%d peer=%d port=%u "
                  "fd=%d connected=%u expected=%u",
                  reason ? reason : "unknown", socket_index,
                  arts_receive_index_peer_rank(socket_index),
                  arts_receive_index_port(socket_index), socket_fd,
                  remote_incoming_connected_count,
                  remote_expected_incoming_count);
  return true;
}

static void arts_close_pending_receive_sockets_locked(int socket_index,
                                                     const char *reason) {
  if (!remote_pending_receive_sockets ||
      !remote_pending_receive_socket_tails || socket_index < 0) {
    return;
  }

  unsigned int closed = 0;
  struct arts_pending_receive_socket_s *pending =
      remote_pending_receive_sockets[socket_index];
  remote_pending_receive_sockets[socket_index] = NULL;
  remote_pending_receive_socket_tails[socket_index] = NULL;
  while (pending) {
    struct arts_pending_receive_socket_s *next = pending->next;
    int socket_fd = pending->fd;
#ifdef ARTS_USE_RDMA
    arts_defer_rdma_close_socket_fd(&socket_fd, false, reason);
#else
    arts_close_socket_fd(&socket_fd);
#endif
    arts_free(pending);
    pending = next;
    closed++;
  }
#ifdef ARTS_USE_RDMA
  if (closed) {
    arts_rdma_sub_counter_floor_zero(&rdma_pending_recv_depth, closed);
  }
#endif
  if (closed) {
    ARTS_TRACE_RDMA("closed pending recv sockets reason=%s index=%d peer=%d "
                    "port=%u count=%u",
                    reason ? reason : "unknown", socket_index,
                    arts_receive_index_peer_rank(socket_index),
                    arts_receive_index_port(socket_index), closed);
  }
}

static void arts_clear_receive_slot_partial_state(int socket_index) {
  if (socket_index < 0 || (unsigned int)socket_index < thread_start ||
      (unsigned int)socket_index >= thread_stop) {
    return;
  }
  unsigned int pos = (unsigned int)socket_index - thread_start;
  if (re_recieve_res) {
    re_recieve_res[pos] = 0;
  }
  if (re_recieve_packet) {
    re_recieve_packet[pos] = NULL;
  }
  if (max_incoming) {
    max_incoming[pos] = false;
  }
}

#ifdef ARTS_USE_RDMA
static void arts_rdma_detach_send_socket_fd(int socket_fd) {
  if (!arts_rdma_full_duplex_enabled() || socket_fd < 0 ||
      !remote_socket_send_list || !remote_connection_alive ||
      !arts_global_message_table) {
    return;
  }

  unsigned int total = arts_global_message_table->table_length * ports;
  for (unsigned int i = 0; i < total; i++) {
    arts_lock(&remote_socket_send_lock_list[i]);
    if (remote_socket_send_list[i] == socket_fd) {
      remote_socket_send_list[i] = -1;
      remote_connection_alive[i] = false;
      if (remote_send_success_count) {
        remote_send_success_count[i] = 0;
      }
      __sync_fetch_and_add(&rdma_full_duplex_detach_count, 1ULL);
      ARTS_TRACE_RDMA("full-duplex detach send index=%u fd=%d", i, socket_fd);
    }
    arts_unlock(&remote_socket_send_lock_list[i]);
  }
}
#endif

static void arts_close_receive_socket_index(int socket_index) {
  if (socket_index < 0) {
    return;
  }

  arts_receive_slot_lock(socket_index);
  int socket_fd = remote_socket_recieve_list
                      ? remote_socket_recieve_list[socket_index]
                      : -1;
  bool had_live_socket = socket_fd >= 0;
  if (poll_incoming) {
    poll_incoming[socket_index].fd = -1;
    poll_incoming[socket_index].events = 0;
    poll_incoming[socket_index].revents = 0;
  }
  if (remote_socket_recieve_list) {
    remote_socket_recieve_list[socket_index] = -1;
  }
#ifdef ARTS_USE_RDMA
  if (remote_receive_success_count) {
    remote_receive_success_count[socket_index] = 0;
  }
  if (remote_receive_close_pending) {
    remote_receive_close_pending[socket_index] = false;
  }
#endif
  arts_clear_receive_slot_partial_state(socket_index);

  bool promoted =
      arts_promote_pending_receive_socket_locked(socket_index, "active_closed");
  if (had_live_socket && !promoted && remote_incoming_connected_count > 0) {
    __sync_sub_and_fetch(&remote_incoming_connected_count, 1U);
  } else if (!had_live_socket && promoted) {
    __sync_add_and_fetch(&remote_incoming_connected_count, 1U);
  }
#ifdef ARTS_USE_RDMA
  if (had_live_socket) {
    __sync_fetch_and_add(&rdma_recv_close_count, 1ULL);
  }
#endif
  ARTS_TRACE_RDMA("close recv index=%d peer=%d port=%u had_live=%u "
                  "promoted=%u connected=%u expected=%u",
                  socket_index, arts_receive_index_peer_rank(socket_index),
                  arts_receive_index_port(socket_index),
                  had_live_socket ? 1U : 0U, promoted ? 1U : 0U,
                  remote_incoming_connected_count,
                  remote_expected_incoming_count);
  arts_receive_slot_unlock(socket_index);

  if (socket_fd >= 0) {
#ifdef ARTS_USE_RDMA
    arts_rdma_detach_send_socket_fd(socket_fd);
    arts_defer_rdma_close_socket_fd(&socket_fd, false, "close_receive");
#else
    arts_close_socket_fd(&socket_fd);
#endif
  }
}

static bool arts_receiver_wakeup_requested(void) {
  bool local_thread_stopped = !arts_thread_info.alive;
#ifdef ARTS_USE_RDMA
  if (rdma_startup_network_setup_active || rdma_accept_thread_active ||
      rdma_connect_helper_thread_active) {
    local_thread_stopped = false;
  }
#endif
  return remote_receiver_wakeup_started != 0U ||
         remote_transport_shutdown_started != 0U ||
         local_thread_stopped;
}

void arts_remote_set_message_table(struct arts_config_s *table) {
  arts_global_message_table = table;
  ports = table->port_count;
#ifdef ARTS_USE_RDMA
  arts_transport_uses_rdma_cached =
      !(table && table->protocol &&
        strcmp(table->protocol, ARTS_TRANSPORT_TCP_NAME) == 0);
#endif
}
bool hostname_to_ip(char *host_name, char *ip) {
  int j;
  struct hostent *he;
  struct in_addr **addr_list;
  struct addrinfo *result;
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET; // Force IPv4 - inet_addr() doesn't handle IPv6
  int error = getaddrinfo(host_name, NULL, &hints, &result);
  if (error == 0) {
    if (result->ai_addr->sa_family == AF_INET) {
      struct sockaddr_in *res = (struct sockaddr_in *)result->ai_addr;
      inet_ntop(AF_INET, &res->sin_addr, ip, 100);
    } else if (result->ai_addr->sa_family == AF_INET6) {
      struct sockaddr_in6 *res = (struct sockaddr_in6 *)result->ai_addr;
      inet_ntop(AF_INET6, &res->sin6_addr, ip, 100);
    }
    freeaddrinfo(result);
    return true;
  }
  ARTS_INFO("%s", gai_strerror(error));

  return false;
}

bool arts_server_set_ip(struct arts_config_s *config) {
  // Always initialize ip_list - it's used by arts_remote_setup_outgoing()
  ip_list = (char *)arts_malloc(100 * sizeof(char) * config->table_length);
  bool result;
  for (int i = 0; i < config->table_length; i++) {
    result = hostname_to_ip(config->table[i].ip_address,
                            ip_list + ((ptrdiff_t)100 * i));
    // result = hostname_to_ip("www.google.com", ip_list+100*i);

    if (!result) {
      ARTS_ERROR("Cannot get ip address for '%s'", config->table[i].ip_address);
    }
  }

  // When net_interface is set (e.g., "ib0"), remap resolved IPs to the
  // target interface's subnet.  Each node resolves hostnames to the default
  // interface (e.g., eno1 172.16.x.x).  We detect the local subnet
  // difference between the default and target interfaces, then apply the
  // same transformation to all resolved IPs so that traffic flows over the
  // target interface (e.g., ib0 172.17.x.x).
  if (config->net_interface) {
    char local_hostname[256];
    char local_default_ip[100];
    gethostname(local_hostname, sizeof(local_hostname));
    if (!hostname_to_ip(local_hostname, local_default_ip)) {
      ARTS_INFO("net_interface=%s: cannot resolve local hostname '%s', "
                "skipping IP remap",
                config->net_interface, local_hostname);
    } else {
      // Find the target interface's IP via getifaddrs
      struct in_addr default_addr;
      struct in_addr iface_addr;
      bool found_iface = false;
      struct ifaddrs *ifap;
      struct ifaddrs *ifa;
      getifaddrs(&ifap);
      for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET &&
            strcmp(ifa->ifa_name, config->net_interface) == 0) {
          struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
          iface_addr = sa->sin_addr;
          found_iface = true;
          break;
        }
      }
      freeifaddrs(ifap);

      if (!found_iface) {
        ARTS_INFO("net_interface=%s: interface not found, using default IPs",
                  config->net_interface);
      } else {
        inet_pton(AF_INET, local_default_ip, &default_addr);
        uint32_t offset = ntohl(iface_addr.s_addr) - ntohl(default_addr.s_addr);

        if (offset != 0) {
          char iface_ip[100];
          inet_ntop(AF_INET, &iface_addr, iface_ip, sizeof(iface_ip));
          ARTS_INFO("net_interface=%s: remapping IPs (%s -> %s)",
                    config->net_interface, local_default_ip, iface_ip);
          for (int i = 0; i < config->table_length; i++) {
            struct in_addr addr;
            char old_ip[100];
            inet_pton(AF_INET, ip_list + ((ptrdiff_t)100 * i), &addr);
            inet_ntop(AF_INET, &addr, old_ip, sizeof(old_ip));
            addr.s_addr = htonl(ntohl(addr.s_addr) + offset);
            inet_ntop(AF_INET, &addr, ip_list + ((ptrdiff_t)100 * i), 100);
            ARTS_INFO("  node %d: %s -> %s", i, old_ip,
                      ip_list + ((ptrdiff_t)100 * i));
          }
        } else {
          ARTS_INFO("net_interface=%s: already on target subnet (%s)",
                    config->net_interface, local_default_ip);
        }
      }
    }
  }

  // Check if rank was passed via environment (SSH-launched child process)
  // This prevents recursive spawning when multiple nodes resolve to the same IP
  char *arts_rank_env = getenv("ARTS_RANK");
  if (arts_rank_env) {
    arts_global_rank_id = (unsigned int)strtol(arts_rank_env, NULL, 10);
    if (arts_global_rank_id >= config->table_length) {
      ARTS_ERROR("ARTS_RANK=%u exceeds routing table length %u",
                 arts_global_rank_id, config->table_length);
      return false;
    }
    config->my_rank = arts_global_rank_id;
    arts_global_rank_count = config->table_length;
    return true;
  }

  // SLURM: use SLURM_PROCID for rank (srun sets this per task)
  // IP matching fails when all nodes resolve to the same address (e.g., WSL2)
  char *slurm_proc_id = getenv("SLURM_PROCID");
  if (slurm_proc_id) {
    arts_global_rank_id = (unsigned int)strtol(slurm_proc_id, NULL, 10);
    if (arts_global_rank_id >= config->table_length) {
      ARTS_ERROR("SLURM_PROCID=%u exceeds routing table length %u",
                 arts_global_rank_id, config->table_length);
      return false;
    }
    config->my_rank = arts_global_rank_id;
    arts_global_rank_count = config->table_length;
    return true;
  }

  int fd;
  struct ifreq ifr;
  char *connection = NULL;
  ifr.ifr_addr.sa_family = AF_INET;

  bool found = false;
  // if(config->net_interface == NULL)
  {
    struct ifaddrs *ifap;
    struct ifaddrs *ifa;
    struct sockaddr_in *sa;
    struct sockaddr_in6 *sa6;
    char addr[100];

    getifaddrs(&ifap);
    for (ifa = ifap; ifa && !found; ifa = ifa->ifa_next) {
      if (ifa->ifa_addr->sa_family == AF_INET) {
        sa = (struct sockaddr_in *)ifa->ifa_addr;
        inet_ntop(AF_INET, &sa->sin_addr, addr, 100);

        for (int i = 0; i < config->table_length && !found; i++) {
          if (strcmp(addr, ip_list + ((ptrdiff_t)100 * i)) == 0) {
            found = true;
            config->my_rank = i;
            arts_global_rank_id = i;
            arts_global_rank_count = arts_global_message_table->table_length;
          }
        }
      } else if (ifa->ifa_addr->sa_family == AF_INET6) {
        sa6 = (struct sockaddr_in6 *)ifa->ifa_addr;
        inet_ntop(AF_INET6, &sa6->sin6_addr, addr, 100);
        ;

        for (int i = 0; i < config->table_length && !found; i++) {
          if (strcmp(addr, ip_list + ((ptrdiff_t)100 * i)) == 0) {
            found = true;
            config->my_rank = i;
            arts_global_rank_id = i;
            arts_global_rank_count = arts_global_message_table->table_length;
          }
        }
      }
    }
    freeifaddrs(ifap);
  }
  return found;
}

void arts_ll_server_setup(struct arts_config_s *config) {
  arts_remote_set_message_table(config);

  if (!arts_server_set_ip(config) && config->nodes > 1) {
    // ARTS_INFO("[%d]Could not connect to %s", arts_global_rank_id,
    // config->net_interface);
    ARTS_ERROR("Could not resolve ip to any device");
  }
}

void arts_ll_server_shutdown() {
  if (!arts_global_message_table) {
    return;
  }
  if (!__sync_bool_compare_and_swap(&remote_transport_shutdown_started, 0U, 1U)) {
    return;
  }
  __sync_lock_test_and_set(&remote_receiver_wakeup_started, 1U);
#ifdef ARTS_USE_RDMA
  arts_rdma_stop_accept_thread();
#endif
  int count = (int)arts_global_message_table->table_length;
  int incoming_count = (count > 0) ? (count - 1) * (int)ports : 0;
  for (int i = 0; remote_socket_recieve_list && i < incoming_count; i++) {
    int socket_fd = -1;
    arts_receive_slot_lock(i);
    if (poll_incoming) {
      poll_incoming[i].fd = -1;
      poll_incoming[i].events = 0;
      poll_incoming[i].revents = 0;
    }
    socket_fd = remote_socket_recieve_list[i];
    remote_socket_recieve_list[i] = -1;
#ifdef ARTS_USE_RDMA
    if (remote_receive_success_count) {
      remote_receive_success_count[i] = 0;
    }
    if (remote_receive_close_pending) {
      remote_receive_close_pending[i] = false;
    }
#endif
    arts_clear_receive_slot_partial_state(i);
    arts_close_pending_receive_sockets_locked(i, "shutdown");
    arts_receive_slot_unlock(i);
#ifdef ARTS_USE_RDMA
    arts_defer_rdma_close_socket_fd(&socket_fd, false, "shutdown");
#else
    arts_close_socket_fd(&socket_fd);
#endif
  }

  for (int i = 0; remote_socket_send_list && i < count * (int)ports; i++) {
    if (i / ports != arts_global_rank_id) {
#ifdef ARTS_USE_RDMA
      arts_defer_rdma_close_socket_fd(&remote_socket_send_list[i], false,
                                      "shutdown-send");
#else
      arts_close_socket_fd_everywhere(&remote_socket_send_list[i]);
#endif
    }
  }

  for (int i = 0; local_socket_recieve && i < (int)ports; i++) {
#ifdef ARTS_USE_RDMA
    arts_defer_rdma_close_socket_fd(&local_socket_recieve[i], false,
                                    "shutdown-local-recv");
#else
    arts_close_socket_fd_everywhere(&local_socket_recieve[i]);
#endif
  }

#ifdef ARTS_USE_RDMA
  arts_rdma_close_worker_shutdown();
#endif
}

void arts_ll_server_wakeup_receivers() {
  if (!arts_global_message_table) {
    return;
  }

  if (!__sync_bool_compare_and_swap(&remote_receiver_wakeup_started, 0U, 1U)) {
    return;
  }
#ifdef ARTS_USE_RDMA
  arts_rdma_stop_accept_thread();
#endif

  int count = (int)arts_global_message_table->table_length;
  int incoming_count = (count > 0) ? (count - 1) * (int)ports : 0;
  ARTS_TRACE_RDMA("wakeup receivers begin incoming=%d ports=%u",
                  incoming_count, ports);

  for (int i = 0; local_socket_recieve && i < (int)ports; i++) {
#ifdef ARTS_USE_RDMA
    arts_defer_rdma_close_socket_fd(&local_socket_recieve[i], false,
                                    "wakeup-local-recv");
#else
    arts_close_socket_fd(&local_socket_recieve[i]);
#endif
  }

  for (int i = 0; remote_socket_recieve_list && i < incoming_count; i++) {
    int socket_fd = -1;
    arts_receive_slot_lock(i);
    if (poll_incoming) {
      poll_incoming[i].fd = -1;
      poll_incoming[i].events = 0;
      poll_incoming[i].revents = 0;
    }
    socket_fd = remote_socket_recieve_list[i];
    remote_socket_recieve_list[i] = -1;
#ifdef ARTS_USE_RDMA
    if (remote_receive_success_count) {
      remote_receive_success_count[i] = 0;
    }
    if (remote_receive_close_pending) {
      remote_receive_close_pending[i] = false;
    }
#endif
    arts_clear_receive_slot_partial_state(i);
    arts_close_pending_receive_sockets_locked(i, "wakeup");
    arts_receive_slot_unlock(i);
#ifdef ARTS_USE_RDMA
    arts_defer_rdma_close_socket_fd(&socket_fd, false, "wakeup");
#else
    arts_close_socket_fd(&socket_fd);
#endif
  }

  remote_incoming_connected_count = 0;
  ARTS_TRACE_RDMA("wakeup receivers leave");
}

void arts_ll_server_cleanup() {
  arts_ll_server_shutdown();
#ifdef ARTS_USE_RDMA
  bool rdma_connect_helpers_drained = arts_rdma_wait_for_connect_helpers();
#endif

  arts_free(ip_list);
  ip_list = NULL;
  arts_free(remote_socket_send_list);
  remote_socket_send_list = NULL;
  arts_free((void *)remote_socket_send_lock_list);
  remote_socket_send_lock_list = NULL;
  arts_free(remote_server_send_list);
  remote_server_send_list = NULL;
  arts_free(remote_connection_alive);
  remote_connection_alive = NULL;
  arts_free(remote_send_success_count);
  remote_send_success_count = NULL;
#ifdef ARTS_USE_RDMA
  arts_free(remote_receive_success_count);
  remote_receive_success_count = NULL;
  arts_free(remote_receive_close_pending);
  remote_receive_close_pending = NULL;
#endif
  arts_free(remote_connect_success_seen);
  remote_connect_success_seen = NULL;
  arts_free(remote_connect_retry_count);
  remote_connect_retry_count = NULL;
  arts_free(remote_connect_retry_after);
  remote_connect_retry_after = NULL;
#ifdef ARTS_USE_RDMA
  if (rdma_connect_helpers_drained) {
    arts_free((void *)remote_connect_abandoned_inflight);
    remote_connect_abandoned_inflight = NULL;
    arts_free(remote_connect_abandoned_generation);
    remote_connect_abandoned_generation = NULL;
    arts_free(remote_connect_abandoned_until);
    remote_connect_abandoned_until = NULL;
    arts_free((void *)remote_connect_active_permit);
    remote_connect_active_permit = NULL;
  }
#endif
  arts_free(remote_socket_recieve_list);
  remote_socket_recieve_list = NULL;
  arts_free(remote_pending_receive_sockets);
  remote_pending_receive_sockets = NULL;
  arts_free(remote_pending_receive_socket_tails);
  remote_pending_receive_socket_tails = NULL;
  arts_free((void *)remote_receive_socket_locks);
  remote_receive_socket_locks = NULL;
  arts_free(remote_server_recieve_list);
  remote_server_recieve_list = NULL;
  arts_free(poll_incoming);
  poll_incoming = NULL;
  arts_free(local_socket_recieve);
  local_socket_recieve = NULL;
  remote_incoming_connected_count = 0;
  remote_receiver_wakeup_started = 0;
#ifdef ARTS_USE_RDMA
  if (rdma_connect_helpers_drained) {
    remote_transport_shutdown_started = 0;
  }
#else
  remote_transport_shutdown_started = 0;
#endif
  remote_expected_incoming_count = 0;
#ifdef ARTS_USE_RDMA
  rdma_abandoned_connect_inflight_count = 0;
  rdma_active_connect_permit_count = 0;
  rdma_active_connect_permit_peak = 0;
  rdma_active_connect_permit_wait_count = 0;
  rdma_next_summary_time = 0;
#endif
  if (arts_global_rank_id == 0) {
    arts_atomic_print("[%s][rank=0] ll_server_cleanup done (COMPLETE)\n",
                      arts_transport_name());
  }
}

unsigned int arts_remote_get_my_rank() {
  return arts_global_message_table->my_rank;
}

static const char *arts_transport_name(void) {
#ifdef ARTS_USE_RDMA
  return arts_transport_runtime_uses_rdma() ? ARTS_TRANSPORT_RDMA_NAME
                                            : ARTS_TRANSPORT_TCP_NAME;
#else
  return ARTS_TRANSPORT_TCP_NAME;
#endif
}

// Public transport-kind name for this (socket) backend: "rdma-rsocket" or "tcp".
// The GASNet backend (gasnet_transport.c) supplies the "gasnet" definition.
const char *arts_transport_kind_name(void) { return arts_transport_name(); }

// --- One-sided RMA DB-move seam ------------------------------------------
// TCP/rsocket has no GASNet segment or gex_RMA equivalent, so callers use the
// Medium-AM/socket snapshot path.
bool arts_transport_rma_capable(void) { return false; }

void *arts_transport_segment_base(uint64_t *size_out) {
  if (size_out) {
    *size_out = 0;
  }
  return NULL;
}

int arts_remote_rma_put(int dest_rank, uint64_t remote_addr,
                        const void *local_src, uint64_t nbytes) {
  (void)dest_rank;
  (void)remote_addr;
  (void)local_src;
  (void)nbytes;
  return -1;
}

bool arts_transport_runtime_uses_rdma(void) {
#ifdef ARTS_USE_RDMA
  return arts_transport_uses_rdma_cached;
#else
  return false;
#endif
}

static bool arts_transport_uses_rdma(void) {
  return arts_transport_runtime_uses_rdma();
}

static unsigned int arts_env_uint(const char *name, unsigned int fallback) {
  const char *raw = getenv(name);
  if (!raw || raw[0] == '\0') {
    return fallback;
  }

  errno = 0;
  char *end = NULL;
  unsigned long value = strtoul(raw, &end, 10);
  if (errno != 0 || end == raw || *end != '\0' || value > UINT_MAX) {
    ARTS_WARN("Ignoring invalid %s='%s'; using %u", name, raw, fallback);
    return fallback;
  }
  return (unsigned int)value;
}

static uint64_t arts_env_uint64(const char *name, uint64_t fallback) {
  const char *raw = getenv(name);
  if (!raw || raw[0] == '\0') {
    return fallback;
  }

  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(raw, &end, 10);
  if (errno != 0 || end == raw || *end != '\0') {
    ARTS_WARN("Ignoring invalid %s='%s'; using %lu", name, raw, fallback);
    return fallback;
  }
  return (uint64_t)value;
}

#ifdef ARTS_USE_RDMA
static bool arts_trace_rdma_summary_enabled(void) {
  static bool initialized = false;
  static bool enabled = false;
  if (!initialized) {
    enabled = arts_trace_env_enabled("ARTS_TRACE_RDMA_SUMMARY") ||
              arts_trace_env_enabled("ARTS_TRACE_RDMA_CONNECT_SUMMARY") ||
              arts_trace_env_enabled("ARTS_TRACE");
    initialized = true;
  }
  return enabled;
}

static unsigned int arts_rdma_summary_interval_us(void) {
  static bool initialized = false;
  static unsigned int interval_us = 0;
  if (!initialized) {
    interval_us = arts_env_uint("ARTS_TRACE_RDMA_SUMMARY_INTERVAL_US",
                                5000000U);
    if (interval_us == 0) {
      interval_us = 1;
    }
    initialized = true;
  }
  return interval_us;
}

static void arts_rdma_maybe_print_summary(const char *reason) {
  if (!arts_trace_rdma_summary_enabled()) {
    return;
  }

  uint64_t now = arts_get_time_stamp();
  uint64_t interval_ns =
      (uint64_t)arts_rdma_summary_interval_us() * 1000ULL;
  uint64_t next = rdma_next_summary_time;
  if (next != 0 && now < next) {
    return;
  }
  if (!__sync_bool_compare_and_swap(&rdma_next_summary_time, next,
                                    now + interval_ns)) {
    return;
  }

  arts_atomic_print("[RDMA-SUMMARY][t_us=%llu rank=%u reason=%s] "
                    "connect_attempt=%llu connect_success=%llu "
                    "connect_fail=%llu connect_timeout=%llu "
                    "startup_connect_refused=%llu connect_abandoned=%llu "
                    "connect_backoff_skip=%llu "
                    "helper_hello=%llu helper_hello_fail=%llu "
                    "helper_active=%llu "
                    "active_connect_permits=%llu "
                    "active_connect_permit_peak=%llu "
                    "active_connect_permit_wait=%llu "
                    "abandoned_inflight=%llu accept_attempt=%llu "
                    "accept_fd=%llu accept_success=%llu accept_eagain=%llu "
                    "accept_hello_fail=%llu accept_hello_timeout=%llu "
                    "close_every=%u incoming=%u/%u pending_depth=%llu "
                    "pending_peak=%llu recv_register=%llu recv_close=%llu "
                    "recv_eof=%llu recv_error=%llu "
                    "deferred_close_start=%llu deferred_close_done=%llu "
                    "deferred_close_abandon=%llu "
                    "deferred_close_inflight=%llu "
                    "deferred_close_queue_depth=%llu "
                    "deferred_close_queue_peak=%llu close_ctx_send=%llu "
                    "close_ctx_recv=%llu close_ctx_discard=%llu "
                    "close_ctx_other=%llu send_cap_hit=%llu "
                    "recv_cap_hit=%llu retire_send=%llu "
                    "retire_send_fail=%llu retire_recv=%llu "
                    "full_duplex_connected_recv=%llu "
                    "full_duplex_accepted_send=%llu "
                    "full_duplex_skip_close=%llu "
                    "full_duplex_detach=%llu "
                    "provider_fanout_hint=%llu "
                    "last_connect_errno=%d "
                    "last_connect_peer=%d last_connect_port=%u\n",
                    arts_trace_time_us(), arts_global_rank_id,
                    reason ? reason : "periodic",
                    (unsigned long long)rdma_connect_attempt_count,
                    (unsigned long long)rdma_connect_success_count,
                    (unsigned long long)rdma_connect_fail_count,
                    (unsigned long long)rdma_connect_timeout_count,
                    (unsigned long long)rdma_startup_connect_refused_count,
                    (unsigned long long)rdma_connect_abandoned_count,
                    (unsigned long long)rdma_connect_backoff_skip_count,
                    (unsigned long long)rdma_connect_helper_hello_count,
                    (unsigned long long)rdma_connect_helper_hello_fail_count,
                    (unsigned long long)rdma_connect_helper_active_count,
                    (unsigned long long)rdma_active_connect_permit_count,
                    (unsigned long long)rdma_active_connect_permit_peak,
                    (unsigned long long)rdma_active_connect_permit_wait_count,
                    (unsigned long long)rdma_abandoned_connect_inflight_count,
                    (unsigned long long)rdma_accept_attempt_count,
                    (unsigned long long)rdma_accept_fd_count,
                    (unsigned long long)rdma_accept_success_count,
                    (unsigned long long)rdma_accept_eagain_count,
                    (unsigned long long)rdma_accept_hello_fail_count,
                    (unsigned long long)rdma_accept_hello_timeout_count,
                    arts_close_send_after_complete_send_every(),
                    remote_incoming_connected_count,
                    remote_expected_incoming_count,
                    (unsigned long long)rdma_pending_recv_depth,
                    (unsigned long long)rdma_pending_recv_peak,
                    (unsigned long long)rdma_recv_register_count,
                    (unsigned long long)rdma_recv_close_count,
                    (unsigned long long)rdma_recv_eof_count,
                    (unsigned long long)rdma_recv_error_count,
                    (unsigned long long)rdma_deferred_close_start_count,
                    (unsigned long long)rdma_deferred_close_done_count,
                    (unsigned long long)rdma_deferred_close_abandon_count,
                    (unsigned long long)rdma_deferred_close_inflight_count,
                    (unsigned long long)rdma_deferred_close_queue_depth,
                    (unsigned long long)rdma_deferred_close_queue_peak,
                    (unsigned long long)rdma_close_after_send_context_count,
                    (unsigned long long)rdma_close_receive_context_count,
                    (unsigned long long)rdma_close_discard_context_count,
                    (unsigned long long)rdma_close_other_context_count,
                    (unsigned long long)rdma_send_cap_hit_count,
                    (unsigned long long)rdma_recv_cap_hit_count,
                    (unsigned long long)rdma_socket_retire_send_count,
                    (unsigned long long)rdma_socket_retire_send_fail_count,
                    (unsigned long long)rdma_socket_retire_recv_count,
                    (unsigned long long)rdma_full_duplex_connected_recv_count,
                    (unsigned long long)rdma_full_duplex_accepted_send_count,
                    (unsigned long long)rdma_full_duplex_skip_close_count,
                    (unsigned long long)rdma_full_duplex_detach_count,
                    (unsigned long long)rdma_provider_fanout_hint_count,
                    rdma_last_connect_errno, rdma_last_connect_peer,
                    rdma_last_connect_port);
}
#endif

static unsigned int arts_connect_timeout_ms(void) {
  static bool initialized = false;
  static unsigned int timeout_ms = 0;
  if (!initialized) {
    unsigned int fallback = arts_transport_uses_rdma()
                                ? ARTS_RDMA_CONNECT_TIMEOUT_MS
                                : ARTS_CONNECT_TIMEOUT_MS;
    timeout_ms = arts_env_uint("ARTS_CONNECT_TIMEOUT_MS", fallback);
    if (timeout_ms == 0) {
      timeout_ms = 1;
    }
    initialized = true;
  }
  return timeout_ms;
}

static unsigned int arts_accept_hello_timeout_ms(void) {
  static bool initialized = false;
  static unsigned int timeout_ms = 0;
  if (!initialized) {
#ifdef ARTS_USE_RDMA
    const char *rdma_raw = getenv("ARTS_RDMA_ACCEPT_HELLO_TIMEOUT_MS");
    timeout_ms =
        (rdma_raw && rdma_raw[0] != '\0')
            ? arts_env_uint("ARTS_RDMA_ACCEPT_HELLO_TIMEOUT_MS",
                            arts_connect_timeout_ms())
            : arts_env_uint("ARTS_ACCEPT_HELLO_TIMEOUT_MS",
                            arts_connect_timeout_ms());
#else
    timeout_ms =
        arts_env_uint("ARTS_ACCEPT_HELLO_TIMEOUT_MS", arts_connect_timeout_ms());
#endif
    if (timeout_ms == 0) {
      timeout_ms = 1;
    }
    initialized = true;
  }
  return timeout_ms;
}

static unsigned int arts_connect_max_retries(void) {
  static bool initialized = false;
  static unsigned int max_retries = 0;
  if (!initialized) {
    unsigned int fallback = arts_transport_uses_rdma()
                                ? ARTS_RDMA_CONNECT_MAX_RETRIES
                                : ARTS_CONNECT_MAX_RETRIES;
    max_retries = arts_env_uint("ARTS_CONNECT_MAX_RETRIES", fallback);
    if (max_retries == 0) {
      max_retries = 1;
    }
    initialized = true;
  }
  return max_retries;
}

static unsigned int arts_connect_retry_delay_us(void) {
  static bool initialized = false;
  static unsigned int delay_us = 0;
  if (!initialized) {
    delay_us = arts_env_uint("ARTS_CONNECT_RETRY_DELAY_US",
                             ARTS_CONNECT_RETRY_DELAY_US);
    initialized = true;
  }
  return delay_us;
}

#ifdef ARTS_USE_RDMA
static unsigned int arts_rdma_startup_connect_grace_ms(void) {
  static bool initialized = false;
  static unsigned int grace_ms = 0;
  if (!initialized) {
    grace_ms = arts_env_uint("ARTS_RDMA_STARTUP_CONNECT_GRACE_MS",
                             ARTS_RDMA_STARTUP_CONNECT_GRACE_MS);
    initialized = true;
  }
  return grace_ms;
}

static bool arts_rdma_listener_ready_enabled(void) {
  static bool initialized = false;
  static bool enabled = true;
  if (!initialized) {
    enabled = arts_env_uint("ARTS_RDMA_LISTENER_READY",
                            ARTS_RDMA_LISTENER_READY) != 0U;
    initialized = true;
  }
  return enabled;
}

static enum arts_rdma_listener_ready_mode_e arts_rdma_listener_ready_mode(void) {
  static bool initialized = false;
  static enum arts_rdma_listener_ready_mode_e mode =
      ARTS_RDMA_LISTENER_READY_RING;
  if (!initialized) {
    if (!arts_rdma_listener_ready_enabled()) {
      mode = ARTS_RDMA_LISTENER_READY_OFF;
    } else {
      const char *raw = getenv("ARTS_RDMA_LISTENER_READY_MODE");
      if (!raw || raw[0] == '\0' || !strcasecmp(raw, "ring") ||
          !strcasecmp(raw, "neighbor")) {
        mode = ARTS_RDMA_LISTENER_READY_RING;
      } else if (!strcasecmp(raw, "full") || !strcasecmp(raw, "all") ||
                 !strcasecmp(raw, "mesh") || !strcasecmp(raw, "alltoall") ||
                 !strcasecmp(raw, "all_to_all")) {
        mode = ARTS_RDMA_LISTENER_READY_FULL;
      } else if (!strcasecmp(raw, "off") || !strcasecmp(raw, "none") ||
                 !strcasecmp(raw, "false") || !strcmp(raw, "0")) {
        mode = ARTS_RDMA_LISTENER_READY_OFF;
      } else {
        ARTS_WARN("%s invalid ARTS_RDMA_LISTENER_READY_MODE=%s; using %s",
                  arts_transport_name(), raw, ARTS_RDMA_LISTENER_READY_MODE);
        mode = ARTS_RDMA_LISTENER_READY_RING;
      }
    }
    initialized = true;
  }
  return mode;
}

static const char *arts_rdma_listener_ready_mode_name(
    enum arts_rdma_listener_ready_mode_e mode) {
  switch (mode) {
  case ARTS_RDMA_LISTENER_READY_RING:
    return "ring";
  case ARTS_RDMA_LISTENER_READY_FULL:
    return "full";
  case ARTS_RDMA_LISTENER_READY_OFF:
  default:
    return "off";
  }
}

static unsigned int arts_rdma_listener_ready_timeout_ms(void) {
  static bool initialized = false;
  static unsigned int timeout_ms = 0;
  if (!initialized) {
    timeout_ms = arts_env_uint("ARTS_RDMA_LISTENER_READY_TIMEOUT_MS",
                               ARTS_RDMA_LISTENER_READY_TIMEOUT_MS);
    if (timeout_ms == 0) {
      timeout_ms = 1;
    }
    initialized = true;
  }
  return timeout_ms;
}

static bool arts_rdma_first_connect_refused_fatal(void) {
  static bool initialized = false;
  static bool fatal = true;
  if (!initialized) {
    fatal = arts_env_uint("ARTS_RDMA_FIRST_CONNECT_REFUSED_FATAL",
                          ARTS_RDMA_FIRST_CONNECT_REFUSED_FATAL) != 0U;
    initialized = true;
  }
  return fatal;
}

static bool arts_rdma_should_defer_startup_connect_warn(int socket_index,
                                                        int last_errno) {
  if (!arts_transport_uses_rdma() || last_errno != ECONNREFUSED ||
      socket_index < 0 || !remote_connect_success_seen) {
    return false;
  }
  if (remote_connect_success_seen[socket_index] != 0U) {
    return false;
  }
  if (rdma_listener_readiness_completed != 0U) {
    return false;
  }

  uint64_t grace_until = rdma_startup_connect_grace_until;
  return grace_until != 0 && arts_get_time_stamp() < grace_until;
}
#endif

void arts_remote_refresh_startup_connect_grace() {
#ifdef ARTS_USE_RDMA
  if (!arts_transport_uses_rdma()) {
    return;
  }
  if (rdma_listener_readiness_completed != 0U &&
      arts_rdma_listener_ready_mode() != ARTS_RDMA_LISTENER_READY_OFF) {
    rdma_startup_connect_grace_until = 0;
    return;
  }
  unsigned int startup_connect_grace_ms = arts_rdma_startup_connect_grace_ms();
  rdma_startup_connect_grace_until =
      startup_connect_grace_ms == 0
          ? 0
          : arts_get_time_stamp() +
                ((uint64_t)startup_connect_grace_ms * 1000000ULL);
#endif
}

static bool arts_close_send_after_complete_send(void) {
#ifdef ARTS_USE_RDMA
  if (!arts_transport_uses_rdma()) {
    return false;
  }
  static bool initialized = false;
  static bool close_after_send = false;
  if (!initialized) {
    const char *raw = getenv("ARTS_RDMA_CLOSE_AFTER_SEND");
    close_after_send =
        (raw && raw[0] != '\0')
            ? arts_env_uint("ARTS_RDMA_CLOSE_AFTER_SEND",
                            ARTS_RDMA_CLOSE_AFTER_SEND) != 0U
            : !arts_rdma_allow_rsocket_reuse();
    if (!close_after_send && !arts_rdma_allow_rsocket_reuse()) {
      arts_rdma_warn_rsocket_reuse_fallback("ARTS_RDMA_CLOSE_AFTER_SEND", 0U);
      close_after_send = true;
    }
    initialized = true;
  }
  return close_after_send;
#else
  return false;
#endif
}

#ifdef ARTS_USE_RDMA
static unsigned int arts_rdma_adaptive_close_after_send_every(void) {
  return arts_close_send_after_complete_send() ? 1U : 0U;
}
#endif

static unsigned int arts_close_send_after_complete_send_every(void) {
#ifdef ARTS_USE_RDMA
  static bool initialized = false;
  static unsigned int close_after_send_every = 0;
  if (!initialized) {
    if (arts_close_send_after_complete_send()) {
      const char *raw = getenv("ARTS_RDMA_CLOSE_AFTER_SEND_EVERY");
      close_after_send_every =
          (raw && raw[0] != '\0')
              ? arts_env_uint("ARTS_RDMA_CLOSE_AFTER_SEND_EVERY", 0U)
              : arts_rdma_adaptive_close_after_send_every();
      if (close_after_send_every == 0) {
        close_after_send_every = arts_rdma_adaptive_close_after_send_every();
      }
      if (close_after_send_every > 1U && !arts_rdma_allow_rsocket_reuse()) {
        arts_rdma_warn_rsocket_reuse_fallback(
            "ARTS_RDMA_CLOSE_AFTER_SEND_EVERY", close_after_send_every);
        close_after_send_every = 1U;
      }
    }
    initialized = true;
  }
  return close_after_send_every;
#else
  return 0;
#endif
}

static bool arts_receive_socket_is_one_shot(void) {
#ifdef ARTS_USE_RDMA
  return arts_close_send_after_complete_send() &&
         arts_close_send_after_complete_send_every() == 1U;
#else
  return false;
#endif
}

#ifdef ARTS_USE_RDMA
static bool arts_receive_socket_has_pending(int socket_index) {
  if (socket_index < 0 || !remote_pending_receive_sockets) {
    return false;
  }

  arts_receive_slot_lock(socket_index);
  bool has_pending = remote_pending_receive_sockets[socket_index] != NULL;
  arts_receive_slot_unlock(socket_index);
  return has_pending;
}

static bool arts_should_close_receive_after_packet(int socket_index,
                                                   bool local_buffer_empty) {
  if (!arts_close_send_after_complete_send() || socket_index < 0) {
    return false;
  }

  unsigned int close_every = arts_close_send_after_complete_send_every();
  if (close_every == 0) {
    return false;
  }
  if (close_every == 1) {
    return local_buffer_empty;
  }

  bool close_due = false;
  if (remote_receive_close_pending &&
      remote_receive_close_pending[socket_index]) {
    close_due = true;
  }

  if (arts_receive_socket_has_pending(socket_index)) {
    close_due = true;
  }

  if (!close_due) {
    return false;
  }

  if (!local_buffer_empty) {
    if (remote_receive_close_pending) {
      remote_receive_close_pending[socket_index] = true;
    }
    return false;
  }

  if (remote_receive_close_pending) {
    remote_receive_close_pending[socket_index] = false;
  }
  return true;
}
#else
static bool arts_should_close_receive_after_packet(int socket_index,
                                                   bool local_buffer_empty) {
  (void)socket_index;
  (void)local_buffer_empty;
  return false;
}
#endif

static bool arts_rdma_connect_helper_enabled(void) {
#ifdef ARTS_USE_RDMA
  static bool initialized = false;
  static bool enabled = true;
  if (!initialized) {
    enabled = arts_env_uint("ARTS_RDMA_CONNECT_HELPER", 1U) != 0U;
    initialized = true;
  }
  return enabled;
#else
  return false;
#endif
}

static bool arts_trace_rdma_accept_poll_enabled(void) {
  static bool initialized = false;
  static bool enabled = false;
  if (!initialized) {
    enabled = arts_trace_env_enabled("ARTS_TRACE_RDMA_ACCEPT_POLL") != 0;
    initialized = true;
  }
  return enabled;
}

static bool arts_rdma_receive_rpoll_enabled(void) {
#ifdef ARTS_USE_RDMA
  static bool initialized = false;
  static bool enabled = false;
  if (!initialized) {
    enabled = arts_env_uint("ARTS_RDMA_RECEIVE_RPOLL", 0U) != 0U;
    initialized = true;
  }
  return enabled;
#else
  return false;
#endif
}

static bool arts_rdma_accept_thread_enabled(void) {
#ifdef ARTS_USE_RDMA
  static bool initialized = false;
  static bool enabled = false;
  if (!initialized) {
    enabled = arts_env_uint("ARTS_RDMA_ACCEPT_THREAD",
                            ARTS_RDMA_ACCEPT_THREAD) != 0U;
    initialized = true;
  }
  return enabled;
#else
  return false;
#endif
}

static bool arts_rdma_full_duplex_enabled(void) {
#ifdef ARTS_USE_RDMA
  static bool initialized = false;
  static bool enabled = false;
  if (!initialized) {
    enabled = arts_env_uint("ARTS_RDMA_FULL_DUPLEX",
                            ARTS_RDMA_FULL_DUPLEX) != 0U;
    if (enabled && !arts_rdma_allow_rsocket_reuse()) {
      arts_rdma_warn_rsocket_reuse_fallback("ARTS_RDMA_FULL_DUPLEX", 1U);
      enabled = false;
    }
    if (enabled && arts_close_send_after_complete_send()) {
      arts_atomic_print(
          "[RDMA-WARN][rank=%u] %s disabling ARTS_RDMA_FULL_DUPLEX because "
          "ARTS_RDMA_CLOSE_AFTER_SEND is enabled; one-shot fallback sockets "
          "cannot be shared bidirectionally.\n",
          arts_global_rank_id, arts_transport_name());
      enabled = false;
    }
    initialized = true;
  }
  return enabled;
#else
  return false;
#endif
}

static int arts_receive_poll_timeout_ms(bool has_live_inbound,
                                        bool awaiting_lazy_accept) {
#ifdef ARTS_USE_RDMA
  (void)has_live_inbound;
  (void)awaiting_lazy_accept;
  unsigned int timeout_ms =
      arts_env_uint("ARTS_RECEIVE_POLL_MS", ARTS_LAZY_ACCEPT_POLL_MS);
  if (timeout_ms == 0) {
    timeout_ms = 1;
  }
  return (timeout_ms > (unsigned int)INT_MAX) ? INT_MAX : (int)timeout_ms;
#else
  return (has_live_inbound && !awaiting_lazy_accept) ? 300000
                                                     : ARTS_LAZY_ACCEPT_POLL_MS;
#endif
}

static int arts_deadline_remaining_ms(uint64_t deadline) {
  uint64_t now = arts_get_time_stamp();
  if (now >= deadline) {
    return 0;
  }
  uint64_t remaining_ns = deadline - now;
  uint64_t remaining_ms = (remaining_ns + 999999ULL) / 1000000ULL;
  return (remaining_ms > (uint64_t)INT_MAX) ? INT_MAX : (int)remaining_ms;
}

#ifdef ARTS_USE_RDMA
struct arts_rdma_connect_attempt_s {
  int socket_fd;
  int socket_index;
  int peer_rank;
  unsigned int port;
  uint32_t hello_flags;
  unsigned int abandoned_generation;
  struct sockaddr_storage addr;
  socklen_t addrlen;
  pthread_mutex_t lock;
  bool done;
  bool abandoned;
  bool hello_started;
  int result;
  int err;
  bool hello_sent;
  int hello_err;
};

static void arts_rdma_connect_attempt_destroy(
    struct arts_rdma_connect_attempt_s *attempt) {
  pthread_mutex_destroy(&attempt->lock);
  free(attempt);
}

static void *arts_rdma_connect_attempt_main(void *arg) {
  rdma_connect_helper_thread_active = true;
  __sync_fetch_and_add(&rdma_connect_helper_active_count, 1ULL);
  struct arts_rdma_connect_attempt_s *attempt =
      (struct arts_rdma_connect_attempt_s *)arg;
  ARTS_TRACE_RDMA("connect rconnect enter peer=%d port=%u fd=%d",
                  attempt->peer_rank, attempt->port, attempt->socket_fd);

  errno = 0;
  int result = RCONNECT(attempt->socket_fd,
                        (const struct sockaddr *)&attempt->addr,
                        attempt->addrlen);
  int saved_errno = (result < 0) ? errno : 0;

  if (result < 0 && (saved_errno == EINPROGRESS || saved_errno == EALREADY)) {
    struct pollfd pfd = {.fd = attempt->socket_fd, .events = POLLOUT};
    int poll_res;
    ARTS_TRACE_RDMA("connect helper poll enter peer=%d port=%u fd=%d "
                    "timeout_ms=%u",
                    attempt->peer_rank, attempt->port, attempt->socket_fd,
                    arts_connect_timeout_ms());
    do {
      poll_res = RPOLL(&pfd, 1, arts_connect_timeout_ms());
    } while (poll_res < 0 && errno == EINTR);
    ARTS_TRACE_RDMA("connect helper poll leave peer=%d port=%u fd=%d res=%d "
                    "errno=%d revents=%d",
                    attempt->peer_rank, attempt->port, attempt->socket_fd,
                    poll_res, (poll_res < 0) ? errno : 0, pfd.revents);

    if (poll_res <= 0) {
      result = -1;
      saved_errno = (poll_res == 0) ? ETIMEDOUT : errno;
    } else {
      int socket_error = 0;
      socklen_t socket_error_len = sizeof(socket_error);
      if (RGETSOCKOPT(attempt->socket_fd, SOL_SOCKET, SO_ERROR, &socket_error,
                      &socket_error_len) < 0) {
        result = -1;
        saved_errno = errno;
      } else if (socket_error != 0) {
        result = -1;
        saved_errno = socket_error;
      } else {
        result = 0;
        saved_errno = 0;
      }
      ARTS_TRACE_RDMA("connect helper getsockopt peer=%d port=%u fd=%d "
                      "so_error=%d result=%d errno=%d",
                      attempt->peer_rank, attempt->port, attempt->socket_fd,
                      socket_error, result, saved_errno);
    }
  }

  bool hello_sent = false;
  int hello_errno = 0;
  bool abandoned_before_hello = false;
  pthread_mutex_lock(&attempt->lock);
  abandoned_before_hello = attempt->abandoned;
  pthread_mutex_unlock(&attempt->lock);

  if (result == 0 && !abandoned_before_hello) {
    pthread_mutex_lock(&attempt->lock);
    attempt->hello_started = true;
    pthread_mutex_unlock(&attempt->lock);
    ARTS_TRACE_RDMA("connect helper hello send enter peer=%d port=%u fd=%d",
                    attempt->peer_rank, attempt->port, attempt->socket_fd);
    hello_sent = arts_send_connection_hello_with_flags(
        attempt->socket_fd, attempt->port, attempt->hello_flags, &hello_errno);
    ARTS_TRACE_RDMA("connect helper hello send leave peer=%d port=%u fd=%d "
                    "ok=%u errno=%d",
                    attempt->peer_rank, attempt->port, attempt->socket_fd,
                    hello_sent ? 1U : 0U, hello_sent ? 0 : hello_errno);
    if (hello_sent) {
      __sync_fetch_and_add(&rdma_connect_helper_hello_count, 1ULL);
    } else {
      __sync_fetch_and_add(&rdma_connect_helper_hello_fail_count, 1ULL);
      result = -1;
      saved_errno = hello_errno ? hello_errno : EIO;
    }
  }

  pthread_mutex_lock(&attempt->lock);
  attempt->result = result;
  attempt->err = saved_errno;
  attempt->hello_sent = hello_sent;
  attempt->hello_err = hello_errno;
  attempt->done = true;
  bool abandoned = attempt->abandoned;
  pthread_mutex_unlock(&attempt->lock);

  ARTS_TRACE_RDMA("connect rconnect leave peer=%d port=%u fd=%d res=%d errno=%d",
                  attempt->peer_rank, attempt->port, attempt->socket_fd, result,
                  saved_errno);

  if (abandoned) {
    if (arts_rdma_abandon_abandoned_connect_close_enabled() && result != 0 &&
        !hello_sent) {
      __sync_fetch_and_add(&rdma_deferred_close_abandon_count, 1ULL);
      arts_rdma_maybe_print_summary("abandoned-connect-close-abandon");
      ARTS_TRACE_RDMA("connect abandoned worker leaked fd=%d peer=%d port=%u",
                      attempt->socket_fd, attempt->peer_rank, attempt->port);
    } else {
      if (result == 0) {
        RSHUTDOWN(attempt->socket_fd, SHUT_RDWR);
      }
      RCLOSE(attempt->socket_fd);
    }
    if (!remote_transport_shutdown_started) {
      arts_rdma_clear_abandoned_connect_inflight(
          attempt->socket_index, attempt->abandoned_generation);
    }
    ARTS_TRACE_RDMA("connect abandoned worker cleanup peer=%d port=%u fd=%d",
                    attempt->peer_rank, attempt->port, attempt->socket_fd);
    if (arts_rdma_take_active_connect_permit_for_socket(
            attempt->socket_index)) {
      arts_rdma_release_active_connect_permit("abandoned-connect-helper");
    }
    arts_rdma_connect_attempt_destroy(attempt);
  }
  rdma_connect_helper_thread_active = false;
  arts_rdma_sub_counter_floor_zero(&rdma_connect_helper_active_count, 1ULL);
  return NULL;
}

static int arts_rdma_connect_with_thread_timeout(int socket_fd,
                                                const struct sockaddr *addr,
                                                socklen_t addrlen,
                                                int socket_index,
                                                int peer_rank,
                                                unsigned int port,
                                                uint32_t hello_flags,
                                                int *err_out,
                                                bool *abandoned_fd_out,
                                                bool *hello_sent_out) {
  if (abandoned_fd_out) {
    *abandoned_fd_out = false;
  }
  if (hello_sent_out) {
    *hello_sent_out = false;
  }
  if (addrlen > sizeof(struct sockaddr_storage)) {
    if (err_out) {
      *err_out = EINVAL;
    }
    return -1;
  }

  struct arts_rdma_connect_attempt_s *attempt =
      (struct arts_rdma_connect_attempt_s *)calloc(1, sizeof(*attempt));
  if (!attempt) {
    if (err_out) {
      *err_out = ENOMEM;
    }
    return -1;
  }

  attempt->socket_fd = socket_fd;
  attempt->socket_index = socket_index;
  attempt->peer_rank = peer_rank;
  attempt->port = port;
  attempt->hello_flags = hello_flags;
  memcpy(&attempt->addr, addr, addrlen);
  attempt->addrlen = addrlen;
  int mutex_res = pthread_mutex_init(&attempt->lock, NULL);
  if (mutex_res != 0) {
    free(attempt);
    if (err_out) {
      *err_out = mutex_res;
    }
    return -1;
  }

  pthread_t thread;
  ARTS_TRACE_RDMA("connect helper thread create enter peer=%d port=%u fd=%d",
                  peer_rank, port, socket_fd);
  int create_res = pthread_create(&thread, NULL, arts_rdma_connect_attempt_main,
                                  attempt);
  ARTS_TRACE_RDMA("connect helper thread create leave peer=%d port=%u fd=%d "
                  "res=%d",
                  peer_rank, port, socket_fd, create_res);
  if (create_res != 0) {
    arts_rdma_connect_attempt_destroy(attempt);
    if (err_out) {
      *err_out = create_res;
    }
    return -1;
  }

  uint64_t connect_deadline =
      arts_get_time_stamp() + ((uint64_t)arts_connect_timeout_ms() * 1000000ULL);
  bool done = false;
  bool hello_started = false;
  uint64_t hello_deadline = 0;
  while (true) {
    pthread_mutex_lock(&attempt->lock);
    done = attempt->done;
    hello_started = attempt->hello_started;
    pthread_mutex_unlock(&attempt->lock);
    if (done) {
      break;
    }
    uint64_t now = arts_get_time_stamp();
    if (hello_started) {
      if (hello_deadline == 0) {
        hello_deadline =
            now + ((uint64_t)arts_accept_hello_timeout_ms() * 1000000ULL);
      }
      if (now >= hello_deadline) {
        break;
      }
    } else if (now >= connect_deadline) {
      break;
    }
#ifdef ARTS_USE_RDMA
    arts_remote_accept_pending(arts_lazy_accept_drain_limit(), 0);
#endif
    usleep(1000);
  }

  pthread_mutex_lock(&attempt->lock);
  done = attempt->done;
  pthread_mutex_unlock(&attempt->lock);

  if (done) {
    pthread_join(thread, NULL);
    int result = attempt->result;
    int saved_errno = attempt->err;
    bool hello_sent = attempt->hello_sent;
    arts_rdma_connect_attempt_destroy(attempt);
    if (err_out) {
      *err_out = saved_errno;
    }
    if (hello_sent_out) {
      *hello_sent_out = hello_sent;
    }
    return result;
  }

  pthread_mutex_lock(&attempt->lock);
  attempt->abandoned = true;
  attempt->abandoned_generation =
      arts_rdma_mark_abandoned_connect_inflight(socket_index,
                                                arts_get_time_stamp());
  pthread_mutex_unlock(&attempt->lock);
  pthread_detach(thread);

  if (abandoned_fd_out) {
    *abandoned_fd_out = true;
  }
  if (err_out) {
    *err_out = ETIMEDOUT;
  }
  ARTS_WARN("%s rconnect/hello did not complete for rank %u -> rank %d port %u "
            "(fd=%d, connect_timeout_ms=%u, accept_hello_timeout_ms=%u, "
            "hello_started=%u); abandoning this socket and retrying later",
            arts_transport_name(), arts_global_rank_id, peer_rank, port,
            socket_fd, arts_connect_timeout_ms(), arts_accept_hello_timeout_ms(),
            hello_started ? 1U : 0U);
  ARTS_TRACE_RDMA("connect rconnect timeout peer=%d port=%u fd=%d hello_started=%u",
                  peer_rank, port, socket_fd, hello_started ? 1U : 0U);
  return -1;
}
#endif

static void arts_configure_transport_socket(int socket_fd) {
  int flags = RF_GETFL(socket_fd);
  if (flags < 0) {
    ARTS_WARN("%s failed to read socket flags for fd=%d: %s",
              arts_transport_name(), socket_fd, strerror(errno));
  } else if ((flags & O_NONBLOCK) == 0 &&
             RF_SETFL(socket_fd, flags | O_NONBLOCK) < 0) {
    ARTS_WARN("%s failed to set nonblocking mode for fd=%d: %s",
              arts_transport_name(), socket_fd, strerror(errno));
  }

#ifdef ARTS_USE_RDMA
  if (arts_transport_uses_rdma()) {
    unsigned int sq_size = arts_env_uint("ARTS_RDMA_SQSIZE", 0);
    unsigned int rq_size = arts_env_uint("ARTS_RDMA_RQSIZE", 0);
    unsigned int inline_size =
        arts_env_uint("ARTS_RDMA_INLINE", ARTS_RDMA_INLINE);
    if (sq_size > 0 &&
        RSETSOCKOPT(socket_fd, SOL_RDMA, RDMA_SQSIZE, &sq_size,
                    sizeof(sq_size)) < 0) {
      ARTS_WARN("rdma-rsocket setsockopt(RDMA_SQSIZE=%u) failed: %s", sq_size,
                strerror(errno));
    }
    if (rq_size > 0 &&
        RSETSOCKOPT(socket_fd, SOL_RDMA, RDMA_RQSIZE, &rq_size,
                    sizeof(rq_size)) < 0) {
      ARTS_WARN("rdma-rsocket setsockopt(RDMA_RQSIZE=%u) failed: %s", rq_size,
                strerror(errno));
    }
    if (inline_size > 0 &&
        RSETSOCKOPT(socket_fd, SOL_RDMA, RDMA_INLINE, &inline_size,
                    sizeof(inline_size)) < 0) {
      ARTS_WARN("rdma-rsocket setsockopt(RDMA_INLINE=%u) failed: %s",
                inline_size, strerror(errno));
    }
  }
#else
  (void)socket_fd;
#endif
}

static unsigned int arts_connect_stagger_us(void) {
  static bool initialized = false;
  static unsigned int stagger_us = 0;
  if (!initialized) {
    unsigned int fallback =
        arts_transport_uses_rdma() ? ARTS_RDMA_CONNECT_STAGGER_US : 0;
    stagger_us = arts_env_uint("ARTS_CONNECT_STAGGER_US", fallback);
    initialized = true;
  }
  return stagger_us;
}

static unsigned int arts_connect_between_us(void) {
  static bool initialized = false;
  static unsigned int between_us = 0;
  if (!initialized) {
    unsigned int fallback =
        arts_transport_uses_rdma() ? ARTS_RDMA_CONNECT_BETWEEN_US : 0;
    between_us = arts_env_uint("ARTS_CONNECT_BETWEEN_US", fallback);
    initialized = true;
  }
  return between_us;
}

static unsigned int arts_connect_steady_between_us(void) {
  static bool initialized = false;
  static unsigned int steady_between_us = 0;
  if (!initialized) {
    unsigned int fallback = arts_transport_uses_rdma()
                                ? ARTS_RDMA_CONNECT_STEADY_BETWEEN_US
                                : arts_connect_between_us();
    steady_between_us =
        arts_env_uint("ARTS_CONNECT_STEADY_BETWEEN_US", fallback);
    initialized = true;
  }
  return steady_between_us;
}

static unsigned int arts_lazy_accept_drain_limit(void) {
  static bool initialized = false;
  static unsigned int limit = 0;
  if (!initialized) {
    const char *raw = getenv("ARTS_LAZY_ACCEPT_DRAIN_LIMIT");
    limit = (raw && raw[0] != '\0')
                ? arts_env_uint("ARTS_LAZY_ACCEPT_DRAIN_LIMIT", 0U)
                : ARTS_LAZY_ACCEPT_DRAIN_LIMIT;
    if (limit == 0) {
#ifdef ARTS_USE_RDMA
      unsigned int rank_count =
          arts_global_message_table ? arts_global_message_table->table_length
                                    : 1U;
      unsigned int peer_count = rank_count > 0 ? rank_count - 1U : 0U;
      limit = peer_count * (ports ? ports : 1U) * 4U;
      if (limit < ARTS_LAZY_ACCEPT_DRAIN_LIMIT) {
        limit = ARTS_LAZY_ACCEPT_DRAIN_LIMIT;
      }
      if (rank_count >= 8U && limit < 256U) {
        limit = 256U;
      }
#else
      limit = ARTS_LAZY_ACCEPT_DRAIN_LIMIT;
#endif
    }
    initialized = true;
  }
  return limit;
}

static unsigned int arts_lazy_accept_idle_interval_us(void) {
  static bool initialized = false;
  static unsigned int interval_us = 0;
  if (!initialized) {
    interval_us = arts_env_uint("ARTS_LAZY_ACCEPT_IDLE_INTERVAL_US",
                                ARTS_LAZY_ACCEPT_IDLE_INTERVAL_US);
    initialized = true;
  }
  return interval_us;
}

static bool arts_should_try_lazy_accept(bool has_live_inbound,
                                        bool awaiting_lazy_accept) {
  if (!awaiting_lazy_accept) {
    return false;
  }
  if (!has_live_inbound) {
    return true;
  }

  unsigned int interval_us = arts_lazy_accept_idle_interval_us();
  if (interval_us == 0) {
    return true;
  }

  uint64_t now = arts_get_time_stamp();
  if (now < next_lazy_accept_time) {
    return false;
  }
  next_lazy_accept_time = now + ((uint64_t)interval_us * 1000ULL);
  return true;
}

static bool arts_rdma_eager_connect_enabled(void) {
#ifdef ARTS_USE_RDMA
  static bool initialized = false;
  static bool enabled = true;
  if (!initialized) {
    enabled = arts_env_uint("ARTS_RDMA_EAGER_CONNECT",
                            ARTS_RDMA_EAGER_CONNECT) != 0U;
    initialized = true;
  }
  return enabled && !arts_close_send_after_complete_send();
#else
  return false;
#endif
}

static unsigned int arts_rdma_eager_connect_rounds(void) {
  static bool initialized = false;
  static unsigned int rounds = 0;
  if (!initialized) {
    rounds = arts_env_uint("ARTS_RDMA_EAGER_CONNECT_ROUNDS",
                           ARTS_RDMA_EAGER_CONNECT_ROUNDS);
    if (rounds == 0) {
      rounds = 1;
    }
    initialized = true;
  }
  return rounds;
}

static uint64_t arts_send_max_bytes_per_call(void) {
#ifdef ARTS_USE_RDMA
  static bool initialized = false;
  static uint64_t max_bytes = 0;
  if (!initialized) {
    max_bytes = arts_env_uint("ARTS_RDMA_SEND_MAX_BYTES",
                              ARTS_RDMA_SEND_MAX_BYTES);
    initialized = true;
  }
  return max_bytes;
#else
  return 0;
#endif
}

static unsigned int arts_send_max_iters_per_call(void) {
#ifdef ARTS_USE_RDMA
  static bool initialized = false;
  static unsigned int max_iters = 0;
  if (!initialized) {
    max_iters = arts_env_uint("ARTS_RDMA_SEND_MAX_ITERS",
                              ARTS_RDMA_SEND_MAX_ITERS);
    initialized = true;
  }
  return max_iters;
#else
  return 0;
#endif
}

static unsigned int arts_recv_packets_per_socket_limit(void) {
#ifdef ARTS_USE_RDMA
  static bool initialized = false;
  static unsigned int limit = 0;
  if (!initialized) {
    limit = arts_env_uint("ARTS_RDMA_RECV_PACKETS_PER_SOCKET",
                          ARTS_RDMA_RECV_PACKETS_PER_SOCKET);
    initialized = true;
  }
  return limit;
#else
  return 0;
#endif
}

#ifdef ARTS_USE_RDMA
static unsigned int arts_rdma_abandoned_connect_backoff_us(void) {
  static bool initialized = false;
  static unsigned int backoff_us = 0;
  if (!initialized) {
    backoff_us = arts_env_uint("ARTS_RDMA_ABANDONED_CONNECT_BACKOFF_US",
                               ARTS_RDMA_ABANDONED_CONNECT_BACKOFF_US);
    initialized = true;
  }
  return backoff_us;
}

static unsigned int arts_rdma_abandoned_connect_quarantine_us(void) {
  static bool initialized = false;
  static unsigned int quarantine_us = 0;
  if (!initialized) {
    quarantine_us = arts_env_uint("ARTS_RDMA_ABANDONED_CONNECT_QUARANTINE_US",
                                  0U);
    if (quarantine_us == 0) {
      quarantine_us = ARTS_RDMA_ABANDONED_CONNECT_QUARANTINE_US;
      unsigned int timeout_quarantine_us = arts_connect_timeout_ms() * 2000U;
      if (quarantine_us < timeout_quarantine_us) {
        quarantine_us = timeout_quarantine_us;
      }
    }
    initialized = true;
  }
  return quarantine_us;
}

static unsigned int arts_rdma_mark_abandoned_connect_inflight(
    int socket_index, uint64_t now) {
  if (socket_index < 0 || !remote_connect_abandoned_inflight ||
      !remote_connect_abandoned_generation || !remote_connect_abandoned_until) {
    return 0U;
  }

  unsigned int generation =
      __sync_add_and_fetch(&remote_connect_abandoned_generation[socket_index],
                           1U);
  if (generation == 0U) {
    generation =
        __sync_add_and_fetch(&remote_connect_abandoned_generation[socket_index],
                             1U);
  }
  unsigned int previous =
      __sync_lock_test_and_set(&remote_connect_abandoned_inflight[socket_index],
                               generation);
  if (previous == 0U) {
    __sync_fetch_and_add(&rdma_abandoned_connect_inflight_count, 1ULL);
  }
  remote_connect_abandoned_until[socket_index] =
      now + ((uint64_t)arts_rdma_abandoned_connect_quarantine_us() * 1000ULL);
  return generation;
}

static void arts_rdma_clear_abandoned_connect_inflight(int socket_index,
                                                       unsigned int generation) {
  if (socket_index < 0 || generation == 0U ||
      !remote_connect_abandoned_inflight) {
    return;
  }
  if (__sync_bool_compare_and_swap(
          &remote_connect_abandoned_inflight[socket_index], generation, 0U)) {
    if (rdma_abandoned_connect_inflight_count > 0) {
      __sync_fetch_and_sub(&rdma_abandoned_connect_inflight_count, 1ULL);
    }
  }
}

static bool arts_rdma_abandoned_connect_still_quarantined(int socket_index,
                                                          uint64_t now) {
  if (socket_index < 0 || !remote_connect_abandoned_inflight ||
      !remote_connect_abandoned_until) {
    return false;
  }

  unsigned int generation = remote_connect_abandoned_inflight[socket_index];
  if (generation == 0U) {
    return false;
  }
  if (remote_connect_abandoned_until[socket_index] > now) {
    __sync_fetch_and_add(&rdma_connect_backoff_skip_count, 1ULL);
    arts_rdma_maybe_print_summary("abandoned-connect-quarantine");
    return true;
  }

  arts_rdma_clear_abandoned_connect_inflight(socket_index, generation);
  remote_connect_abandoned_until[socket_index] = 0;
  return false;
}

static void arts_rdma_maybe_warn_provider_fanout_limit(
    int peer_rank, unsigned int port, const struct sockaddr_in *addr,
    unsigned int retry_count, int connect_errno) {
  if (connect_errno != ETIMEDOUT || rdma_connect_success_count < 4ULL ||
      rdma_connect_success_count > 4ULL || rdma_connect_timeout_count == 0ULL) {
    return;
  }
  if (!__sync_bool_compare_and_swap(&rdma_provider_fanout_warning_emitted, 0U,
                                    1U)) {
    return;
  }

  __sync_fetch_and_add(&rdma_provider_fanout_hint_count, 1ULL);
  char target_ip[INET_ADDRSTRLEN];
  snprintf(target_ip, sizeof(target_ip), "unknown");
  unsigned int target_port = 0;
  if (addr) {
    if (!inet_ntop(AF_INET, &addr->sin_addr, target_ip, sizeof(target_ip))) {
      snprintf(target_ip, sizeof(target_ip), "unknown");
    }
    target_port = ntohs(addr->sin_port);
  }

  arts_atomic_print(
      "[RDMA-WARN][rank=%u] %s observed RDMA rsocket ETIMEDOUT after %llu "
      "successful connects (peer=%d port=%u target=%s:%u retry=%u, "
      "timeouts=%llu, accepted=%llu, close_every=%u). Keeping RDMA peer "
      "fanout bounded; use close-after-send only for provider triage.\n",
      arts_global_message_table ? arts_global_message_table->my_rank
                                : arts_global_rank_id,
      arts_transport_name(), (unsigned long long)rdma_connect_success_count,
      peer_rank, port, target_ip, target_port, retry_count,
      (unsigned long long)rdma_connect_timeout_count,
      (unsigned long long)rdma_accept_success_count,
      arts_close_send_after_complete_send_every());
  arts_rdma_maybe_print_summary("provider-fanout-limit");
}
#endif

static void arts_stagger_remote_connect(int peer_rank, bool steady_state) {
  static bool initial_stagger_done = false;
  unsigned int stagger_us = arts_connect_stagger_us();
  unsigned int between_us =
      steady_state ? arts_connect_steady_between_us() : arts_connect_between_us();
  if ((stagger_us == 0 && between_us == 0) || peer_rank < 0) {
    return;
  }

  unsigned int local_rank = arts_global_message_table->my_rank;
  if (!initial_stagger_done && stagger_us > 0) {
    useconds_t delay = (useconds_t)(local_rank * stagger_us);
    if (delay > 0) {
      usleep(delay);
    }
    initial_stagger_done = true;
  }

  if (between_us > 0) {
    usleep((useconds_t)between_us);
  }
}

static bool arts_socket_send_all(int socket_fd, const void *buffer, size_t length,
                                 int *err_out) {
  const char *cursor = (const char *)buffer;
  size_t total = 0;
  uint64_t deadline =
      arts_get_time_stamp() + ((uint64_t)arts_connect_timeout_ms() * 1000000ULL);
  while (total < length) {
    if (arts_receiver_wakeup_requested()) {
      if (err_out) {
        *err_out = ECANCELED;
      }
      return false;
    }
    int timeout_ms = arts_deadline_remaining_ms(deadline);
    if (timeout_ms <= 0) {
      if (err_out) {
        *err_out = ETIMEDOUT;
      }
      return false;
    }
#ifdef ARTS_USE_RDMA
    (void)timeout_ms;
#else
    struct pollfd pfd = {.fd = socket_fd, .events = POLLOUT};
    int poll_res = RPOLL(&pfd, 1, timeout_ms);
    if (poll_res < 0 && errno == EINTR) {
      continue;
    }
    if (poll_res <= 0) {
      if (err_out) {
        *err_out = (poll_res == 0) ? ETIMEDOUT : errno;
      }
      return false;
    }
#endif

    int sent = RSEND(socket_fd, cursor + total, length - total, MSG_DONTWAIT);
    if (sent < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
#ifdef ARTS_USE_RDMA
        usleep(ARTS_RDMA_ACCEPT_SLEEP_US);
#endif
        continue;
      }
      if (err_out) {
        *err_out = errno;
      }
      return false;
    }
    if (sent == 0) {
      if (err_out) {
        *err_out = ECONNRESET;
      }
      return false;
    }
    total += (size_t)sent;
  }
  return true;
}

static bool arts_socket_recv_all(int socket_fd, void *buffer, size_t length,
                                 unsigned int timeout_ms, int *err_out) {
  char *cursor = (char *)buffer;
  size_t total = 0;
  if (timeout_ms == 0) {
    timeout_ms = 1;
  }
  uint64_t deadline = arts_get_time_stamp() +
                      ((uint64_t)timeout_ms * 1000000ULL);
  while (total < length) {
    if (arts_receiver_wakeup_requested()) {
      if (err_out) {
        *err_out = ECANCELED;
      }
      return false;
    }
    int remaining_ms = arts_deadline_remaining_ms(deadline);
    if (remaining_ms <= 0) {
      if (err_out) {
        *err_out = ETIMEDOUT;
      }
      return false;
    }
#ifdef ARTS_USE_RDMA
    (void)remaining_ms;
#else
    struct pollfd pfd = {.fd = socket_fd, .events = POLLIN};
    int poll_res = RPOLL(&pfd, 1, remaining_ms);
    if (poll_res < 0 && errno == EINTR) {
      continue;
    }
    if (poll_res <= 0) {
      if (err_out) {
        *err_out = (poll_res == 0) ? ETIMEDOUT : errno;
      }
      return false;
    }
#endif

    int received =
        RRECV(socket_fd, cursor + total, length - total, MSG_DONTWAIT);
    if (received < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
#ifdef ARTS_USE_RDMA
        usleep(ARTS_RDMA_ACCEPT_SLEEP_US);
#endif
        continue;
      }
      if (err_out) {
        *err_out = errno;
      }
      return false;
    }
    if (received == 0) {
      if (err_out) {
        *err_out = ECONNRESET;
      }
      return false;
    }
    total += (size_t)received;
  }
  return true;
}

static bool arts_socket_connect_with_timeout(int socket_fd,
                                             const struct sockaddr *addr,
                                             socklen_t addrlen, int peer_rank,
                                             unsigned int port, int socket_index,
                                             uint32_t hello_flags,
                                             int *err_out,
                                             bool *abandoned_fd_out,
                                             bool *hello_sent_out) {
#ifndef ARTS_USE_RDMA
  (void)socket_index;
#endif
  if (abandoned_fd_out) {
    *abandoned_fd_out = false;
  }
  if (hello_sent_out) {
    *hello_sent_out = false;
  }
  ARTS_TRACE_RDMA("connect flags get peer=%d port=%u fd=%d", peer_rank, port,
                  socket_fd);
  int flags = RF_GETFL(socket_fd);
  if (flags < 0) {
    if (err_out) {
      *err_out = errno;
    }
    return false;
  }

  bool restore_flags = ((flags & O_NONBLOCK) == 0);
  ARTS_TRACE_RDMA("connect flags current peer=%d port=%u fd=%d flags=%d "
                  "restore=%u",
                  peer_rank, port, socket_fd, flags, restore_flags ? 1U : 0U);
  if (restore_flags && RF_SETFL(socket_fd, flags | O_NONBLOCK) < 0) {
    if (err_out) {
      *err_out = errno;
    }
    return false;
  }

  ARTS_TRACE_RDMA("connect call enter peer=%d port=%u fd=%d", peer_rank, port,
                  socket_fd);
  bool connection_hello_sent = false;
#ifdef ARTS_USE_RDMA
  bool connect_abandoned = false;
  int connect_errno = 0;
  int connect_res;
  if (arts_rdma_connect_helper_enabled()) {
    connect_res = arts_rdma_connect_with_thread_timeout(
        socket_fd, addr, addrlen, socket_index, peer_rank, port, hello_flags,
        &connect_errno, &connect_abandoned, &connection_hello_sent);
  } else {
    connect_res = RCONNECT(socket_fd, addr, addrlen);
    connect_errno = (connect_res < 0) ? errno : 0;
  }
  if (connect_abandoned) {
    if (err_out) {
      *err_out = connect_errno;
    }
    if (abandoned_fd_out) {
      *abandoned_fd_out = true;
    }
    return false;
  }
  if (connect_res < 0 && connect_errno == 0) {
    connect_errno = errno;
  }
#else
  int connect_res = RCONNECT(socket_fd, addr, addrlen);
  int connect_errno = (connect_res < 0) ? errno : 0;
#endif
  ARTS_TRACE_RDMA("connect call leave peer=%d port=%u fd=%d res=%d errno=%d",
                  peer_rank, port, socket_fd, connect_res, connect_errno);
  if (connect_res == 0) {
    if (restore_flags && RF_SETFL(socket_fd, flags) < 0) {
      ARTS_WARN("%s failed to restore socket flags after connect: %s",
                arts_transport_name(), strerror(errno));
    }
    if (hello_sent_out) {
      *hello_sent_out = connection_hello_sent;
    }
    return true;
  }

#ifdef ARTS_USE_RDMA
  if (connect_errno == ETIMEDOUT) {
    if (err_out) {
      *err_out = connect_errno;
    }
    return false;
  }
#endif

  if (connect_errno == EISCONN) {
    if (restore_flags && RF_SETFL(socket_fd, flags) < 0) {
      ARTS_WARN("%s failed to restore socket flags after connect: %s",
                arts_transport_name(), strerror(errno));
    }
    return true;
  }

  if (connect_errno != EINPROGRESS && connect_errno != EALREADY) {
    if (restore_flags && RF_SETFL(socket_fd, flags) < 0) {
      ARTS_WARN("%s failed to restore socket flags after failed connect: %s",
                arts_transport_name(), strerror(errno));
    }
    if (err_out) {
      *err_out = connect_errno;
    }
    return false;
  }

  struct pollfd pfd = {.fd = socket_fd, .events = POLLOUT};
  int poll_res;
  ARTS_TRACE_RDMA("connect poll enter peer=%d port=%u fd=%d timeout_ms=%u",
                  peer_rank, port, socket_fd, arts_connect_timeout_ms());
  do {
    poll_res = RPOLL(&pfd, 1, arts_connect_timeout_ms());
  } while (poll_res < 0 && errno == EINTR);
  ARTS_TRACE_RDMA("connect poll leave peer=%d port=%u fd=%d res=%d errno=%d "
                  "revents=%d",
                  peer_rank, port, socket_fd, poll_res,
                  (poll_res < 0) ? errno : 0, pfd.revents);

  if (poll_res <= 0) {
    if (restore_flags && RF_SETFL(socket_fd, flags) < 0) {
      ARTS_WARN("%s failed to restore socket flags after connect timeout: %s",
                arts_transport_name(), strerror(errno));
    }
    if (err_out) {
      *err_out = (poll_res == 0) ? ETIMEDOUT : errno;
    }
    return false;
  }

  int socket_error = 0;
  socklen_t socket_error_len = sizeof(socket_error);
  ARTS_TRACE_RDMA("connect getsockopt enter peer=%d port=%u fd=%d", peer_rank,
                  port, socket_fd);
  if (RGETSOCKOPT(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error,
                  &socket_error_len) < 0) {
    if (restore_flags && RF_SETFL(socket_fd, flags) < 0) {
      ARTS_WARN("%s failed to restore socket flags after getsockopt: %s",
                arts_transport_name(), strerror(errno));
    }
    if (err_out) {
      *err_out = errno;
    }
    return false;
  }
  ARTS_TRACE_RDMA("connect getsockopt leave peer=%d port=%u fd=%d so_error=%d",
                  peer_rank, port, socket_fd, socket_error);

  if (restore_flags && RF_SETFL(socket_fd, flags) < 0) {
    ARTS_WARN("%s failed to restore socket flags after connect completion: %s",
              arts_transport_name(), strerror(errno));
  }

  if (socket_error != 0) {
    if (err_out) {
      *err_out = socket_error;
    }
    return false;
  }
  return true;
}

static bool arts_send_connection_hello_with_flags(int socket_fd,
                                                  unsigned int port,
                                                  uint32_t flags, int *err_out) {
  struct arts_connection_hello_s hello = {
      .magic = htonl(ARTS_CONNECTION_HELLO_MAGIC),
      .rank = htonl(arts_global_message_table->my_rank),
      .port = htonl(port),
      .flags = htonl(flags),
  };
  return arts_socket_send_all(socket_fd, &hello, sizeof(hello), err_out);
}

static bool arts_send_connection_hello(int socket_fd, unsigned int port,
                                       int *err_out) {
  return arts_send_connection_hello_with_flags(socket_fd, port, 0U, err_out);
}

#ifdef ARTS_USE_RDMA
static bool arts_is_rdma_socket_retire_packet(
    const struct arts_remote_packet_s *packet) {
  return packet && packet->message_type == ARTS_RDMA_SOCKET_RETIRE_MSG &&
         packet->size == sizeof(*packet);
}

static bool arts_send_rdma_socket_retire(int socket_fd, unsigned int port,
                                         int *err_out) {
  (void)port;
  struct arts_remote_packet_s packet;
  memset(&packet, 0, sizeof(packet));
  arts_fill_packet_header(&packet, sizeof(packet), ARTS_RDMA_SOCKET_RETIRE_MSG);
  ARTS_TRACE_RDMA("retire send enter fd=%d port=%u", socket_fd, port);
  bool sent = arts_socket_send_all(socket_fd, &packet, sizeof(packet), err_out);
  ARTS_TRACE_RDMA("retire send leave fd=%d port=%u ok=%u errno=%d", socket_fd,
                  port, sent ? 1U : 0U, sent ? 0 : (err_out ? *err_out : 0));
  if (sent) {
    __sync_fetch_and_add(&rdma_socket_retire_send_count, 1ULL);
  } else {
    __sync_fetch_and_add(&rdma_socket_retire_send_fail_count, 1ULL);
  }
  return sent;
}
#else
static bool arts_send_rdma_socket_retire(int socket_fd, unsigned int port,
                                         int *err_out) {
  (void)socket_fd;
  (void)port;
  (void)err_out;
  return true;
}
#endif

static uint64_t arts_max_packet_bytes(void) {
  static uint64_t max_packet_bytes = 0;
  static bool initialized = false;
  if (!initialized) {
    max_packet_bytes =
        arts_env_uint64("ARTS_MAX_PACKET_BYTES", ARTS_MAX_PACKET_BYTES);
    if (!max_packet_bytes) {
      max_packet_bytes = ARTS_MAX_PACKET_BYTES;
    }
    initialized = true;
  }
  return max_packet_bytes;
}

static bool arts_remote_message_type_valid(unsigned int message_type) {
  return message_type <= ARTS_REMOTE_SET_DEP_MODE_MSG ||
         message_type == ARTS_RDMA_SOCKET_RETIRE_MSG;
}

static bool arts_remote_packet_header_valid(
    const struct arts_remote_packet_s *packet) {
  if (!packet) {
    return false;
  }
  if (!arts_remote_message_type_valid(packet->message_type)) {
    return false;
  }
  if (packet->rank >= arts_global_rank_count) {
    return false;
  }
  if (packet->message_type == ARTS_RDMA_SOCKET_RETIRE_MSG) {
    return packet->size == sizeof(*packet);
  }
  uint64_t max_packet_bytes = arts_max_packet_bytes();
  return packet->size >= sizeof(*packet) && packet->size <= max_packet_bytes;
}

static void arts_reject_remote_packet_header(
    int socket_index, const struct arts_remote_packet_s *packet) {
  ARTS_INFO("Invalid remote packet header index=%d from=%u msg=%u size=%lu "
            "max=%lu",
            socket_index, packet ? packet->rank : 0,
            packet ? packet->message_type : 0, packet ? packet->size : 0,
            arts_max_packet_bytes());
  arts_close_receive_socket_index(socket_index);
  arts_shutdown();
  arts_runtime_stop();
}

static bool arts_recv_connection_hello(int socket_fd,
                                       struct arts_connection_hello_s *hello,
                                       int *err_out) {
  struct arts_connection_hello_s wire_hello;
  if (!arts_socket_recv_all(socket_fd, &wire_hello, sizeof(wire_hello),
                            arts_accept_hello_timeout_ms(), err_out)) {
    return false;
  }

  hello->magic = ntohl(wire_hello.magic);
  hello->rank = ntohl(wire_hello.rank);
  hello->port = ntohl(wire_hello.port);
  hello->flags = ntohl(wire_hello.flags);
  if (hello->magic != ARTS_CONNECTION_HELLO_MAGIC) {
    if (err_out) {
      *err_out = EPROTO;
    }
    return false;
  }
  return true;
}

static int arts_remote_peer_slot(int peer_rank) {
  int my_rank = (int)arts_global_message_table->my_rank;
  if (peer_rank == my_rank) {
    return -1;
  }
  return (peer_rank < my_rank) ? peer_rank : peer_rank - 1;
}

static bool arts_register_receive_socket(int peer_rank, unsigned int port,
                                         int socket_fd, const char *source) {
  int peer_slot = arts_remote_peer_slot(peer_rank);
  if (peer_slot < 0 || port >= ports || socket_fd < 0 ||
      !remote_socket_recieve_list || !poll_incoming) {
    return false;
  }

  int recv_index = (int)port + (peer_slot * (int)ports);
  arts_receive_slot_lock(recv_index);
  int existing = remote_socket_recieve_list[recv_index];
  if (existing == socket_fd) {
    arts_receive_slot_unlock(recv_index);
    return true;
  }
  if (existing >= 0) {
    bool queued = arts_queue_pending_receive_socket_locked(
        recv_index, socket_fd, source ? source : "duplicate");
    ARTS_INFO("%s queued duplicate receive socket on rank %u for peer %d "
              "port %u (existing fd=%d, %s fd=%d)",
              arts_transport_name(), arts_global_rank_id, peer_rank, port,
              existing, source ? source : "duplicate", socket_fd);
    arts_receive_slot_unlock(recv_index);
    return queued;
  }

  arts_install_receive_socket_locked(recv_index, socket_fd);
  __sync_add_and_fetch(&remote_incoming_connected_count, 1U);
#ifdef ARTS_USE_RDMA
  __sync_fetch_and_add(&rdma_recv_register_count, 1ULL);
#endif
  ARTS_INFO("%s registered %s socket for receives on rank %u from rank %d "
            "port %u (recv_index=%d, fd=%d)",
            arts_transport_name(), source ? source : "unknown",
            arts_global_rank_id, peer_rank, port, recv_index, socket_fd);
  ARTS_TRACE_RDMA("register_recv source=%s peer=%d port=%u index=%d fd=%d "
                  "connected=%u expected=%u",
                  source ? source : "unknown", peer_rank, port, recv_index,
                  socket_fd, remote_incoming_connected_count,
                  remote_expected_incoming_count);
  arts_receive_slot_unlock(recv_index);
  return true;
}

#ifdef ARTS_USE_RDMA
static int arts_receive_index_for_peer_port(int peer_rank, unsigned int port) {
  int peer_slot = arts_remote_peer_slot(peer_rank);
  if (peer_slot < 0 || port >= ports) {
    return -1;
  }
  return (int)port + (peer_slot * (int)ports);
}

static bool arts_rdma_send_socket_is_active_receive(int peer_rank,
                                                    unsigned int port,
                                                    int socket_fd) {
  if (!arts_rdma_full_duplex_enabled() || socket_fd < 0 ||
      !remote_socket_recieve_list) {
    return false;
  }

  int recv_index = arts_receive_index_for_peer_port(peer_rank, port);
  if (recv_index < 0) {
    return false;
  }

  arts_receive_slot_lock(recv_index);
  bool shared = remote_socket_recieve_list[recv_index] == socket_fd;
  arts_receive_slot_unlock(recv_index);
  return shared;
}

static bool arts_rdma_register_send_socket_from_full_duplex(int peer_rank,
                                                           unsigned int port,
                                                           int socket_fd,
                                                           const char *source) {
  if (!arts_rdma_full_duplex_enabled() || peer_rank < 0 ||
      !arts_global_message_table ||
      (unsigned int)peer_rank >= arts_global_message_table->table_length ||
      (unsigned int)peer_rank == arts_global_message_table->my_rank ||
      port >= ports || socket_fd < 0 || !remote_socket_send_list ||
      !remote_connection_alive || !remote_socket_send_lock_list) {
    return false;
  }

  int socket_index = (peer_rank * (int)ports) + (int)port;
  bool adopted = false;
  arts_lock(&remote_socket_send_lock_list[socket_index]);
  int existing = remote_socket_send_list[socket_index];
  if (existing == socket_fd) {
    remote_connection_alive[socket_index] = true;
    adopted = true;
  } else if (existing < 0 || !remote_connection_alive[socket_index]) {
    remote_socket_send_list[socket_index] = socket_fd;
    remote_connection_alive[socket_index] = true;
    if (remote_send_success_count) {
      remote_send_success_count[socket_index] = 0;
    }
    if (remote_connect_retry_count) {
      remote_connect_retry_count[socket_index] = 0;
    }
    if (remote_connect_retry_after) {
      remote_connect_retry_after[socket_index] = 0;
    }
    if (remote_connect_success_seen) {
      remote_connect_success_seen[socket_index] = 1U;
    }
    adopted = true;
  }
  arts_unlock(&remote_socket_send_lock_list[socket_index]);

  if (adopted) {
    __sync_fetch_and_add(&rdma_full_duplex_accepted_send_count, 1ULL);
    ARTS_TRACE_RDMA("full-duplex adopted %s socket for sends peer=%d port=%u "
                    "index=%d fd=%d",
                    source ? source : "unknown", peer_rank, port, socket_index,
                    socket_fd);
  }
  return adopted;
}
#endif

static bool arts_try_accept_lock(void) {
  return __sync_bool_compare_and_swap(&remote_accept_lock, 0U, 1U);
}

static void arts_accept_unlock(void) {
  __sync_lock_release(&remote_accept_lock);
}

static bool arts_thread_has_live_inbound(void) {
  for (unsigned int i = thread_start; i < thread_stop; i++) {
    if (poll_incoming && poll_incoming[i].fd >= 0) {
      return true;
    }
  }
  return false;
}

static bool arts_remote_accept_one_pending(int timeout_ms) {
  if (arts_receiver_wakeup_requested() || !local_socket_recieve ||
      !poll_incoming || !arts_try_accept_lock()) {
    return false;
  }

  int accepted_socket = -1;
  int accepted_port = -1;
#ifdef ARTS_USE_RDMA
  uint64_t deadline = arts_get_time_stamp() +
                      ((uint64_t)(timeout_ms > 0 ? timeout_ms : 0) *
                       1000000ULL);
  do {
    unsigned int start_port = ports
                                  ? (__sync_fetch_and_add(
                                         &remote_accept_next_port, 1U) %
                                     ports)
                                  : 0U;
    for (int offset = 0; offset < (int)ports; offset++) {
      int ready_port = (int)((start_port + (unsigned int)offset) % ports);
      if (!local_socket_recieve || local_socket_recieve[ready_port] < 0 ||
          arts_receiver_wakeup_requested()) {
        continue;
      }

      struct sockaddr_in peer_addr;
      socklen_t peer_len = sizeof(peer_addr);
      bool trace_accept_poll = arts_trace_rdma_accept_poll_enabled();
      if (trace_accept_poll) {
        ARTS_TRACE_RDMA("accept call enter fd=%d port=%d",
                        local_socket_recieve[ready_port], ready_port);
      }
      INCREMENT_NUM_REMOTE_ACCEPT_ATTEMPT_BY(1);
#ifdef ARTS_USE_RDMA
      __sync_fetch_and_add(&rdma_accept_attempt_count, 1ULL);
#endif
      accepted_socket = RACCEPT(local_socket_recieve[ready_port],
                                (struct sockaddr *)&peer_addr, &peer_len);
      int accept_errno = accepted_socket < 0 ? errno : 0;
      if (trace_accept_poll || accepted_socket >= 0 ||
          (accept_errno != EAGAIN && accept_errno != EWOULDBLOCK &&
           accept_errno != EINTR)) {
        ARTS_TRACE_RDMA("accept call leave fd=%d accepted=%d port=%d errno=%d",
                        local_socket_recieve[ready_port], accepted_socket,
                        ready_port, accept_errno);
      }
      if (accepted_socket < 0) {
        if (accept_errno == EAGAIN || accept_errno == EWOULDBLOCK ||
            accept_errno == EINTR) {
          INCREMENT_NUM_REMOTE_ACCEPT_EAGAIN_BY(1);
#ifdef ARTS_USE_RDMA
          __sync_fetch_and_add(&rdma_accept_eagain_count, 1ULL);
#endif
          continue;
        }
        ARTS_WARN("%s lazy accept failed on rank %u port index %d: %s",
                  arts_transport_name(), arts_global_rank_id, ready_port,
                  strerror(accept_errno));
        continue;
      }
      arts_configure_transport_socket(accepted_socket);
      accepted_port = ready_port;
      ARTS_TRACE_RDMA("accept fd=%d port=%d", accepted_socket, ready_port);
      break;
    }
    if (accepted_socket >= 0 || timeout_ms <= 0 ||
        arts_receiver_wakeup_requested()) {
      break;
    }
    usleep(ARTS_RDMA_ACCEPT_SLEEP_US);
  } while (arts_get_time_stamp() < deadline);

  arts_accept_unlock();
#else
  struct pollfd *accept_pfds =
      (struct pollfd *)arts_malloc(sizeof(struct pollfd) * ports);
  for (int z = 0; z < (int)ports; z++) {
    accept_pfds[z].fd = local_socket_recieve[z];
    accept_pfds[z].events = POLLIN;
    accept_pfds[z].revents = 0;
  }

  int poll_timeout_ms = timeout_ms < 0 ? 0 : timeout_ms;
  int poll_res;
  do {
    poll_res = RPOLL(accept_pfds, ports, poll_timeout_ms);
  } while (poll_res < 0 && errno == EINTR &&
           !arts_receiver_wakeup_requested());

  if (poll_res <= 0 || arts_receiver_wakeup_requested()) {
    arts_free(accept_pfds);
    arts_accept_unlock();
    return false;
  }

  for (int ready_port = 0; ready_port < (int)ports; ready_port++) {
    if (accept_pfds[ready_port].fd < 0) {
      continue;
    }
    if (!(accept_pfds[ready_port].revents & POLLIN)) {
      continue;
    }
    if (arts_receiver_wakeup_requested()) {
      continue;
    }

    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    accepted_socket = RACCEPT(local_socket_recieve[ready_port],
                              (struct sockaddr *)&peer_addr, &peer_len);
    int accept_errno = accepted_socket < 0 ? errno : 0;
    INCREMENT_NUM_REMOTE_ACCEPT_ATTEMPT_BY(1);
    if (accepted_socket < 0) {
      if (accept_errno == EAGAIN || accept_errno == EWOULDBLOCK ||
          accept_errno == EINTR) {
        INCREMENT_NUM_REMOTE_ACCEPT_EAGAIN_BY(1);
        continue;
      }
      ARTS_WARN("%s lazy accept failed on rank %u port index %d: %s",
                arts_transport_name(), arts_global_rank_id, ready_port,
                strerror(accept_errno));
      continue;
    }
    arts_configure_transport_socket(accepted_socket);
    accepted_port = ready_port;
    break;
  }

  arts_free(accept_pfds);
  arts_accept_unlock();
#endif

  if (accepted_socket < 0) {
    return false;
  }
#ifdef ARTS_USE_RDMA
  __sync_fetch_and_add(&rdma_accept_fd_count, 1ULL);
#endif
  if (arts_receiver_wakeup_requested()) {
    arts_discard_unconnected_socket_fd(&accepted_socket);
    return false;
  }

  struct arts_connection_hello_s hello;
  int hello_errno = 0;
  if (!arts_recv_connection_hello(accepted_socket, &hello, &hello_errno)) {
#ifdef ARTS_USE_RDMA
    __sync_fetch_and_add(&rdma_accept_hello_fail_count, 1ULL);
    if (hello_errno == ETIMEDOUT) {
      __sync_fetch_and_add(&rdma_accept_hello_timeout_count, 1ULL);
      arts_rdma_maybe_print_summary("accept-hello-timeout");
    }
#endif
    ARTS_WARN("%s lazy accept failed to read hello on rank %u port index %d "
              "within %u ms (errno=%d: %s)",
              arts_transport_name(), arts_global_rank_id, accepted_port,
              arts_accept_hello_timeout_ms(), hello_errno,
              strerror(hello_errno));
    arts_discard_unconnected_socket_fd(&accepted_socket);
    return false;
  }
  ARTS_TRACE_RDMA("accept hello fd=%d peer=%u port=%u", accepted_socket,
                  hello.rank, hello.port);

  if ((hello.flags & ARTS_CONNECTION_HELLO_READY) != 0U) {
    if (hello.rank == arts_global_message_table->my_rank ||
        hello.rank >= arts_global_message_table->table_length ||
        hello.port >= ports || (int)hello.port != accepted_port) {
      ARTS_WARN("%s listener-readiness hello rejected on rank %u: peer rank=%u "
                "port=%u ready_port=%d",
                arts_transport_name(), arts_global_rank_id, hello.rank,
                hello.port, accepted_port);
    } else {
#ifdef ARTS_USE_RDMA
      __sync_fetch_and_add(&rdma_listener_ready_incoming_count, 1ULL);
#endif
      ARTS_TRACE_RDMA("listener-ready accept peer=%u port=%u fd=%d",
                      hello.rank, hello.port, accepted_socket);
    }
    arts_discard_unconnected_socket_fd(&accepted_socket);
    return true;
  }

  if (hello.rank == arts_global_message_table->my_rank ||
      hello.rank >= arts_global_message_table->table_length ||
      hello.port >= ports || (int)hello.port != accepted_port) {
    ARTS_WARN("%s lazy accept rejected hello on rank %u: peer rank=%u port=%u "
              "ready_port=%d",
              arts_transport_name(), arts_global_rank_id, hello.rank, hello.port,
              accepted_port);
    arts_discard_unconnected_socket_fd(&accepted_socket);
    return false;
  }

  if (!arts_register_receive_socket((int)hello.rank, hello.port, accepted_socket,
                                    "accepted")) {
    arts_discard_unconnected_socket_fd(&accepted_socket);
    return false;
  }
#ifdef ARTS_USE_RDMA
  arts_rdma_register_send_socket_from_full_duplex(
      (int)hello.rank, hello.port, accepted_socket, "accepted");
#endif
  INCREMENT_NUM_REMOTE_ACCEPT_SUCCESS_BY(1);
#ifdef ARTS_USE_RDMA
  __sync_fetch_and_add(&rdma_accept_success_count, 1ULL);
  arts_rdma_maybe_print_summary("accept-success");
#endif
  return true;
}

static bool arts_remote_accept_pending(unsigned int limit, int first_timeout_ms) {
  bool accepted_any = false;
  if (limit == 0) {
    limit = 1;
  }
  for (unsigned int i = 0; i < limit; i++) {
    if (!arts_close_send_after_complete_send() &&
        remote_expected_incoming_count > 0 &&
        remote_incoming_connected_count >= remote_expected_incoming_count) {
      break;
    }
    int timeout_ms = (i == 0) ? first_timeout_ms : 0;
    if (!arts_remote_accept_one_pending(timeout_ms)) {
      break;
    }
    accepted_any = true;
  }
#ifdef ARTS_USE_RDMA
  if (accepted_any) {
    arts_rdma_maybe_print_summary("accept-drain");
  }
#endif
  return accepted_any;
}

#ifdef ARTS_USE_RDMA
static void *arts_rdma_accept_thread_main(void *arg) {
  (void)arg;
  rdma_accept_thread_active = true;
  ARTS_TRACE_RDMA("accept thread start rank=%u", arts_global_rank_id);
  while (!arts_receiver_wakeup_requested()) {
    bool accepted =
        arts_remote_accept_pending(arts_lazy_accept_drain_limit(),
                                   ARTS_LAZY_ACCEPT_POLL_MS);
    if (!accepted) {
      usleep(ARTS_RDMA_ACCEPT_SLEEP_US);
    }
  }
  arts_rdma_maybe_print_summary("accept-thread-stop");
  ARTS_TRACE_RDMA("accept thread stop rank=%u", arts_global_rank_id);
  rdma_accept_thread_active = false;
  return NULL;
}

static void arts_rdma_start_accept_thread(void) {
  if (!arts_transport_uses_rdma() || !arts_rdma_accept_thread_enabled() ||
      remote_expected_incoming_count == 0 || !local_socket_recieve) {
    return;
  }
  if (!__sync_bool_compare_and_swap(&rdma_accept_thread_started, 0U, 1U)) {
    return;
  }

  pthread_attr_t attr;
  pthread_attr_init(&attr);
#ifdef PTHREAD_STACK_MIN
  pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN * 2);
#endif

  int create_res = pthread_create(&rdma_accept_thread, &attr,
                                  arts_rdma_accept_thread_main, NULL);
  pthread_attr_destroy(&attr);
  if (create_res != 0) {
    __sync_lock_release(&rdma_accept_thread_started);
    ARTS_WARN("%s could not start RDMA accept thread on rank %u: %s",
              arts_transport_name(), arts_global_rank_id,
              strerror(create_res));
    return;
  }

  ARTS_INFO("%s RDMA accept thread started on rank %u",
            arts_transport_name(), arts_global_rank_id);
}

static void arts_rdma_stop_accept_thread(void) {
  if (!rdma_accept_thread_started) {
    return;
  }
  if (pthread_equal(pthread_self(), rdma_accept_thread)) {
    __sync_lock_release(&rdma_accept_thread_started);
    return;
  }
  pthread_join(rdma_accept_thread, NULL);
  __sync_lock_release(&rdma_accept_thread_started);
  ARTS_TRACE_RDMA("accept thread joined rank=%u", arts_global_rank_id);
}
#endif

static inline bool arts_remote_connect(int rank, unsigned int port) {

  int socket_index = (rank * ports) + (int)port;
  if (!remote_connection_alive[socket_index]) {
    uint64_t now = arts_get_time_stamp();
#ifdef ARTS_USE_RDMA
    if (arts_rdma_abandoned_connect_still_quarantined(socket_index, now)) {
      return false;
    }
#endif
    if (remote_connect_retry_after &&
        remote_connect_retry_after[socket_index] > now) {
#ifdef ARTS_USE_RDMA
      __sync_fetch_and_add(&rdma_connect_backoff_skip_count, 1ULL);
      arts_rdma_maybe_print_summary("connect-backoff");
#endif
      return false;
    }

#ifdef ARTS_USE_RDMA
    arts_remote_accept_pending(arts_lazy_accept_drain_limit(), 0);
#endif

    struct sockaddr_in *addr =
        remote_server_send_list + ((size_t)rank * ports) + port;
    unsigned int retry_count =
        remote_connect_retry_count ? remote_connect_retry_count[socket_index] : 0;
    if (retry_count == 0) {
      bool steady_connect =
          remote_connect_success_seen &&
          remote_connect_success_seen[socket_index] != 0U;
      arts_stagger_remote_connect(rank, steady_connect);
    }
#ifdef ARTS_USE_RDMA
    bool active_connect_permit = false;
    if (arts_rdma_active_connect_throttle_enabled()) {
      active_connect_permit =
          arts_rdma_acquire_active_connect_permit_for_socket(socket_index, rank,
                                                             port);
      if (!active_connect_permit) {
        return false;
      }
    }
#endif
    bool candidate_is_published = remote_socket_send_list[socket_index] >= 0;
    int candidate_fd = candidate_is_published
                           ? remote_socket_send_list[socket_index]
                           : arts_get_new_socket();
    if (candidate_fd < 0) {
#ifdef ARTS_USE_RDMA
      if (active_connect_permit &&
          arts_rdma_take_active_connect_permit_for_socket(socket_index)) {
        arts_rdma_release_active_connect_permit("socket-create-failed");
      }
#endif
      return false;
    }

    int last_errno = 0;
    bool connect_abandoned = false;
    bool connection_hello_sent = false;
    ARTS_TRACE_RDMA("connect attempt peer=%d port=%u fd=%d retry=%u", rank,
                    port, candidate_fd, retry_count);
    INCREMENT_NUM_REMOTE_CONNECT_ATTEMPT_BY(1);
#ifdef ARTS_USE_RDMA
    __sync_fetch_and_add(&rdma_connect_attempt_count, 1ULL);
#endif
    bool connected = arts_socket_connect_with_timeout(
        candidate_fd, (struct sockaddr *)addr, sizeof(struct sockaddr_in), rank,
        port, socket_index, 0U, &last_errno, &connect_abandoned,
        &connection_hello_sent);
    bool hello_sent = connection_hello_sent;
    if (connected && !hello_sent) {
      ARTS_TRACE_RDMA("connect hello send enter peer=%d port=%u fd=%d", rank,
                      port, candidate_fd);
      hello_sent = arts_send_connection_hello(candidate_fd, port, &last_errno);
      ARTS_TRACE_RDMA("connect hello send leave peer=%d port=%u fd=%d ok=%u "
                      "errno=%d",
                      rank, port, candidate_fd, hello_sent ? 1U : 0U,
                      hello_sent ? 0 : last_errno);
    }
    if (connected && hello_sent) {
      remote_socket_send_list[socket_index] = candidate_fd;
      remote_connection_alive[socket_index] = true;
      if (remote_connect_retry_count) {
        remote_connect_retry_count[socket_index] = 0;
      }
      if (remote_connect_retry_after) {
        remote_connect_retry_after[socket_index] = 0;
      }
      if (remote_send_success_count) {
        remote_send_success_count[socket_index] = 0;
      }
      if (remote_connect_success_seen) {
        remote_connect_success_seen[socket_index] = 1U;
      }
      ARTS_INFO("%s connected rank %u to rank %d port %u (fd=%d)",
                arts_transport_name(), arts_global_message_table->my_rank, rank,
                port, candidate_fd);
      INCREMENT_NUM_REMOTE_CONNECT_SUCCESS_BY(1);
#ifdef ARTS_USE_RDMA
      __sync_fetch_and_add(&rdma_connect_success_count, 1ULL);
      if (arts_rdma_full_duplex_enabled()) {
        if (arts_register_receive_socket(rank, port, candidate_fd, "connected")) {
          __sync_fetch_and_add(&rdma_full_duplex_connected_recv_count, 1ULL);
          ARTS_TRACE_RDMA("full-duplex registered connected socket for receives "
                          "peer=%d port=%u fd=%d",
                          rank, port, candidate_fd);
        } else {
          ARTS_WARN("%s could not register connected full-duplex receive socket "
                    "rank %u <- rank %d port %u fd=%d",
                    arts_transport_name(), arts_global_message_table->my_rank,
                    rank, port, candidate_fd);
        }
      }
      arts_rdma_maybe_print_summary("connect-success");
#endif
      ARTS_TRACE_RDMA("connect success peer=%d port=%u fd=%d", rank, port,
                      candidate_fd);
      return true;
    }

    if (!last_errno) {
      last_errno = errno;
    }
#ifdef ARTS_USE_RDMA
    rdma_last_connect_errno = last_errno;
    rdma_last_connect_peer = rank;
    rdma_last_connect_port = port;
#endif
    remote_connection_alive[socket_index] = false;
    retry_count++;
    unsigned int max_retries = arts_connect_max_retries();
    unsigned int retry_delay_us = arts_connect_retry_delay_us();
    if (connect_abandoned) {
#ifdef ARTS_USE_RDMA
      INCREMENT_NUM_REMOTE_CONNECT_ABANDONED_BY(1);
      __sync_fetch_and_add(&rdma_connect_abandoned_count, 1ULL);
      unsigned int abandoned_backoff_us =
          arts_rdma_abandoned_connect_backoff_us();
      unsigned int abandoned_quarantine_us =
          arts_rdma_abandoned_connect_quarantine_us();
      if (retry_delay_us < abandoned_backoff_us) {
        retry_delay_us = abandoned_backoff_us;
      }
      if (retry_delay_us < abandoned_quarantine_us) {
        retry_delay_us = abandoned_quarantine_us;
      }
      char target_ip[INET_ADDRSTRLEN];
      if (!inet_ntop(AF_INET, &addr->sin_addr, target_ip, sizeof(target_ip))) {
        snprintf(target_ip, sizeof(target_ip), "unknown");
      }
      ARTS_WARN("%s abandoned blocking rconnect rank %u -> rank %d port %u "
                "target %s:%u fd=%d retry=%u next_retry_us=%u",
                arts_transport_name(), arts_global_message_table->my_rank, rank,
                port, target_ip, ntohs(addr->sin_port), candidate_fd,
                retry_count, retry_delay_us);
      arts_rdma_maybe_print_summary("connect-abandoned");
#endif
      if (candidate_is_published) {
        arts_abandon_unconnected_socket_fd(
            &remote_socket_send_list[socket_index]);
      } else {
        arts_abandon_unconnected_socket_fd(&candidate_fd);
      }
    } else {
      bool release_active_connect_permit = false;
#ifdef ARTS_USE_RDMA
      release_active_connect_permit =
          active_connect_permit &&
          arts_rdma_take_active_connect_permit_for_socket(socket_index);
#endif
      if (candidate_is_published) {
        arts_discard_unconnected_socket_fd_with_permit(
            &remote_socket_send_list[socket_index],
            release_active_connect_permit);
      } else {
        arts_discard_unconnected_socket_fd_with_permit(&candidate_fd,
                                                       release_active_connect_permit);
      }
    }
    if (remote_send_success_count) {
      remote_send_success_count[socket_index] = 0;
    }
    INCREMENT_NUM_REMOTE_CONNECT_FAIL_BY(1);
#ifdef ARTS_USE_RDMA
    __sync_fetch_and_add(&rdma_connect_fail_count, 1ULL);
#endif
    if (last_errno == ETIMEDOUT) {
      INCREMENT_NUM_REMOTE_CONNECT_TIMEOUT_BY(1);
#ifdef ARTS_USE_RDMA
      __sync_fetch_and_add(&rdma_connect_timeout_count, 1ULL);
      arts_rdma_maybe_warn_provider_fanout_limit(rank, port, addr, retry_count,
                                                 last_errno);
#endif
    }

    if (retry_count >= max_retries) {
#ifdef ARTS_USE_RDMA
      if (arts_rdma_should_defer_startup_connect_warn(socket_index,
                                                      last_errno)) {
        __sync_fetch_and_add(&rdma_startup_connect_refused_count, 1ULL);
        ARTS_TRACE_RDMA("startup connect refused peer=%d port=%u retry=%u "
                        "max_retries=%u; deferring warning during launch grace",
                        rank, port, retry_count, max_retries);
        if (max_retries > 1U) {
          retry_count = max_retries - 1U;
        }
      } else {
        if (last_errno == ECONNREFUSED && rdma_listener_readiness_completed &&
            arts_rdma_first_connect_refused_fatal()) {
          char target_ip[INET_ADDRSTRLEN];
          if (!inet_ntop(AF_INET, &addr->sin_addr, target_ip, sizeof(target_ip))) {
            snprintf(target_ip, sizeof(target_ip), "unknown");
          }
          ARTS_ERROR("arts_remote_connect: %s RDMA connect refused after "
                     "listener readiness rank %u to rank %d port %u after %u "
                     "attempts (target %s:%u, errno=%d: %s)",
                     arts_transport_name(), arts_global_message_table->my_rank,
                     rank, port, retry_count, target_ip, ntohs(addr->sin_port),
                     last_errno, strerror(last_errno));
        }
        if (last_errno == ECONNREFUSED &&
            remote_connect_success_seen &&
            remote_connect_success_seen[socket_index] == 0U) {
          char target_ip[INET_ADDRSTRLEN];
          if (!inet_ntop(AF_INET, &addr->sin_addr, target_ip, sizeof(target_ip))) {
            snprintf(target_ip, sizeof(target_ip), "unknown");
          }
          if (arts_rdma_first_connect_refused_fatal()) {
            ARTS_ERROR("arts_remote_connect: %s first RDMA connect refused "
                       "after startup readiness/grace rank %u to rank %d port "
                       "%u after %u attempts (target %s:%u, errno=%d: %s)",
                       arts_transport_name(),
                       arts_global_message_table->my_rank, rank, port,
                       retry_count, target_ip, ntohs(addr->sin_port),
                       last_errno, strerror(last_errno));
          }
          ARTS_WARN("arts_remote_connect: %s first RDMA connect still refused "
                    "after startup readiness/grace rank %u to rank %d port %u "
                    "after %u attempts (target %s:%u, errno=%d: %s)",
                    arts_transport_name(), arts_global_message_table->my_rank,
                    rank, port, retry_count, target_ip, ntohs(addr->sin_port),
                    last_errno, strerror(last_errno));
        } else {
          ARTS_WARN("arts_remote_connect: %s failed to connect rank %u to "
                    "rank %d port %u after %u attempts (target %s:%d, "
                    "errno=%d: %s)",
                    arts_transport_name(), arts_global_message_table->my_rank,
                    rank, port, retry_count, inet_ntoa(addr->sin_addr),
                    ntohs(addr->sin_port), last_errno, strerror(last_errno));
        }
        retry_count = 0;
      }
#else
      ARTS_WARN("arts_remote_connect: %s failed to connect rank %u to rank %d "
                "port %u after %u attempts (target %s:%d, errno=%d: %s)",
                arts_transport_name(), arts_global_message_table->my_rank, rank,
                port, retry_count, inet_ntoa(addr->sin_addr),
                ntohs(addr->sin_port), last_errno, strerror(last_errno));
      retry_count = 0;
#endif
    }

    if (remote_connect_retry_count) {
      remote_connect_retry_count[socket_index] = retry_count;
    }
    if (remote_connect_retry_after) {
      remote_connect_retry_after[socket_index] =
          arts_get_time_stamp() + ((uint64_t)retry_delay_us * 1000ULL);
    }
    return false;
  }

  return true;
}

void arts_remote_eager_connect_all() {
  if (!arts_global_message_table || arts_global_message_table->table_length <= 1 ||
      !arts_rdma_eager_connect_enabled()) {
    return;
  }

  unsigned int rank_count = arts_global_message_table->table_length;
  unsigned int my_rank = arts_global_message_table->my_rank;
  unsigned int total_targets = (rank_count - 1U) * ports;
  unsigned int rounds = arts_rdma_eager_connect_rounds();

  ARTS_INFO("%s eager persistent connect starting on rank %u: targets=%u "
            "rounds=%u",
            arts_transport_name(), my_rank, total_targets, rounds);

  for (unsigned int round = 0; round < rounds; round++) {
    unsigned int connected = 0;
    unsigned int pending = 0;
    for (unsigned int rank = 0; rank < rank_count; rank++) {
      if (rank == my_rank) {
        continue;
      }
      for (unsigned int port = 0; port < ports; port++) {
        int socket_index = ((int)rank * (int)ports) + (int)port;
        if (remote_connection_alive &&
            remote_connection_alive[socket_index]) {
          connected++;
          continue;
        }
        if (arts_remote_connect((int)rank, port)) {
          connected++;
        } else {
          pending++;
        }
      }
    }

    arts_remote_accept_pending(arts_lazy_accept_drain_limit(), 0);
    ARTS_TRACE_RDMA("eager connect round=%u connected=%u pending=%u "
                    "incoming=%u/%u",
                    round + 1U, connected, pending,
                    remote_incoming_connected_count,
                    remote_expected_incoming_count);

    if (connected >= total_targets &&
        remote_incoming_connected_count >= remote_expected_incoming_count) {
      break;
    }
    usleep(ARTS_RDMA_ACCEPT_SLEEP_US);
  }

  arts_remote_accept_pending(arts_lazy_accept_drain_limit(), 0);
  unsigned int final_connected = 0;
  for (unsigned int rank = 0; rank < rank_count; rank++) {
    if (rank == my_rank) {
      continue;
    }
    for (unsigned int port = 0; port < ports; port++) {
      int socket_index = ((int)rank * (int)ports) + (int)port;
      if (remote_connection_alive && remote_connection_alive[socket_index]) {
        final_connected++;
      }
    }
  }
  ARTS_INFO("%s eager persistent connect finished on rank %u: outbound=%u/%u "
            "incoming=%u/%u",
            arts_transport_name(), my_rank, final_connected, total_targets,
            remote_incoming_connected_count, remote_expected_incoming_count);
}

#ifdef ARTS_USE_RDMA
static void arts_rdma_listener_ready_backoff(unsigned int backoff_us,
                                             uint64_t deadline) {
  if (backoff_us == 0) {
    backoff_us = ARTS_RDMA_ACCEPT_SLEEP_US;
  }

  uint64_t now = arts_get_time_stamp();
  uint64_t sleep_until = now + ((uint64_t)backoff_us * 1000ULL);
  if (sleep_until > deadline) {
    sleep_until = deadline;
  }

  while (!arts_receiver_wakeup_requested()) {
    now = arts_get_time_stamp();
    if (now >= sleep_until) {
      break;
    }
    arts_remote_accept_pending(arts_lazy_accept_drain_limit(), 0);
    uint64_t remaining_us = (sleep_until - now) / 1000ULL;
    useconds_t sleep_us =
        (useconds_t)(remaining_us < ARTS_RDMA_ACCEPT_SLEEP_US
                         ? remaining_us
                         : ARTS_RDMA_ACCEPT_SLEEP_US);
    if (sleep_us == 0) {
      sleep_us = 1;
    }
    usleep(sleep_us);
  }
}

static bool arts_rdma_listener_ready_peer(int rank, unsigned int port,
                                          uint64_t deadline) {
  struct sockaddr_in *addr =
      remote_server_send_list + ((size_t)rank * ports) + port;
  unsigned int attempt = 0;
  unsigned int backoff_us = arts_connect_retry_delay_us();
  if (backoff_us == 0) {
    backoff_us = ARTS_RDMA_ACCEPT_SLEEP_US;
  }
  int last_errno = 0;

  while (!arts_receiver_wakeup_requested() && arts_get_time_stamp() < deadline) {
    arts_remote_accept_pending(arts_lazy_accept_drain_limit(), 0);
    if (attempt == 0) {
      arts_stagger_remote_connect(rank, false);
    }

    int fd = arts_get_new_socket();
    if (fd < 0) {
      last_errno = errno ? errno : EIO;
      arts_rdma_listener_ready_backoff(backoff_us, deadline);
      continue;
    }

    bool abandoned = false;
    bool hello_sent = false;
    int connect_errno = 0;
    bool connected = arts_socket_connect_with_timeout(
        fd, (struct sockaddr *)addr, sizeof(struct sockaddr_in), rank, port,
        -1, ARTS_CONNECTION_HELLO_READY, &connect_errno, &abandoned,
        &hello_sent);
    bool ready = false;
    if (connected) {
      if (hello_sent) {
        ready = true;
      } else {
        ready = arts_send_connection_hello_with_flags(
            fd, port, ARTS_CONNECTION_HELLO_READY, &connect_errno);
      }
    }

    if (ready) {
      char target_ip[INET_ADDRSTRLEN];
      if (!inet_ntop(AF_INET, &addr->sin_addr, target_ip, sizeof(target_ip))) {
        snprintf(target_ip, sizeof(target_ip), "unknown");
      }
      ARTS_INFO("%s listener-ready rank %u -> rank %d port %u "
                "(target %s:%u, attempts=%u)",
                arts_transport_name(), arts_global_rank_id, rank, port,
                target_ip, ntohs(addr->sin_port), attempt + 1U);
      arts_defer_rdma_close_socket_fd(&fd, false, "listener-ready");
      arts_remote_accept_pending(arts_lazy_accept_drain_limit(), 0);
      return true;
    }

    last_errno = connect_errno ? connect_errno : (errno ? errno : ECONNRESET);
    if (abandoned) {
      arts_abandon_unconnected_socket_fd(&fd);
    } else {
      arts_discard_unconnected_socket_fd(&fd);
    }
    attempt++;
    if (last_errno == ECONNREFUSED) {
      __sync_fetch_and_add(&rdma_startup_connect_refused_count, 1ULL);
    }

    arts_rdma_listener_ready_backoff(backoff_us, deadline);
    if (backoff_us < ARTS_RDMA_LISTENER_READY_MAX_BACKOFF_US) {
      unsigned int doubled = backoff_us * 2U;
      backoff_us = (doubled > backoff_us) ? doubled
                                          : ARTS_RDMA_LISTENER_READY_MAX_BACKOFF_US;
      if (backoff_us > ARTS_RDMA_LISTENER_READY_MAX_BACKOFF_US) {
        backoff_us = ARTS_RDMA_LISTENER_READY_MAX_BACKOFF_US;
      }
    }
  }

  char target_ip[INET_ADDRSTRLEN];
  if (!inet_ntop(AF_INET, &addr->sin_addr, target_ip, sizeof(target_ip))) {
    snprintf(target_ip, sizeof(target_ip), "unknown");
  }
  ARTS_WARN("%s listener readiness failed rank %u -> rank %d port %u "
            "(target %s:%u, attempts=%u, errno=%d: %s)",
            arts_transport_name(), arts_global_rank_id, rank, port, target_ip,
            ntohs(addr->sin_port), attempt, last_errno, strerror(last_errno));
  return false;
}

static bool arts_rdma_wait_for_listener_ready_incoming(
    uint64_t expected_incoming, uint64_t deadline,
    enum arts_rdma_listener_ready_mode_e mode) {
  if (expected_incoming == 0) {
    return true;
  }

  while (!arts_receiver_wakeup_requested()) {
    arts_remote_accept_pending(arts_lazy_accept_drain_limit(), 0);
    uint64_t incoming = rdma_listener_ready_incoming_count;
    if (incoming >= expected_incoming) {
      return true;
    }
    if (arts_get_time_stamp() >= deadline) {
      break;
    }
    arts_rdma_listener_ready_backoff(ARTS_RDMA_ACCEPT_SLEEP_US, deadline);
  }

  ARTS_WARN("%s listener readiness %s mode timed out on rank %u waiting for "
            "incoming probes: received=%llu expected=%llu",
            arts_transport_name(), arts_rdma_listener_ready_mode_name(mode),
            arts_global_rank_id,
            (unsigned long long)rdma_listener_ready_incoming_count,
            (unsigned long long)expected_incoming);
  return false;
}

static bool arts_rdma_wait_for_listener_readiness(void) {
  if (!arts_transport_uses_rdma() || !arts_global_message_table ||
      arts_global_message_table->table_length <= 1 || ports == 0) {
    return true;
  }

  enum arts_rdma_listener_ready_mode_e mode = arts_rdma_listener_ready_mode();
  if (mode == ARTS_RDMA_LISTENER_READY_OFF) {
    return true;
  }

  unsigned int rank_count = arts_global_message_table->table_length;
  unsigned int my_rank = arts_global_message_table->my_rank;
  unsigned int timeout_ms = arts_rdma_listener_ready_timeout_ms();
  uint64_t deadline =
      arts_get_time_stamp() + ((uint64_t)timeout_ms * 1000000ULL);

  uint64_t expected_incoming =
      (mode == ARTS_RDMA_LISTENER_READY_FULL)
          ? ((uint64_t)(rank_count - 1U) * (uint64_t)ports)
          : (uint64_t)ports;

  ARTS_INFO("%s listener readiness starting on rank %u: mode=%s ranks=%u "
            "ports=%u expected_incoming=%llu timeout_ms=%u",
            arts_transport_name(), my_rank,
            arts_rdma_listener_ready_mode_name(mode), rank_count, ports,
            (unsigned long long)expected_incoming, timeout_ms);

  if (mode == ARTS_RDMA_LISTENER_READY_RING) {
    unsigned int target_rank = (my_rank + 1U) % rank_count;
    for (unsigned int port = 0; port < ports; port++) {
      if (!arts_rdma_listener_ready_peer((int)target_rank, port, deadline)) {
        arts_rdma_maybe_print_summary("listener-ready-failed");
        return false;
      }
    }
  } else {
    for (unsigned int rank = 0; rank < rank_count; rank++) {
      if (rank == my_rank) {
        continue;
      }
      for (unsigned int port = 0; port < ports; port++) {
        if (!arts_rdma_listener_ready_peer((int)rank, port, deadline)) {
          arts_rdma_maybe_print_summary("listener-ready-failed");
          return false;
        }
      }
    }
  }

  if (!arts_rdma_wait_for_listener_ready_incoming(expected_incoming, deadline,
                                                  mode)) {
    arts_rdma_maybe_print_summary("listener-ready-incoming-failed");
    return false;
  }

  __sync_lock_test_and_set(&rdma_listener_readiness_completed, 1U);
  rdma_startup_connect_grace_until = 0;
  ARTS_INFO("%s listener readiness complete on rank %u: mode=%s incoming=%llu",
            arts_transport_name(), my_rank,
            arts_rdma_listener_ready_mode_name(mode),
            (unsigned long long)rdma_listener_ready_incoming_count);
  arts_rdma_maybe_print_summary("listener-ready-complete");
  return true;
}
#endif

static void arts_close_send_socket_after_complete_send(int socket_index,
                                                       int rank,
                                                       unsigned int port) {
  unsigned int close_after_send_every =
      arts_close_send_after_complete_send_every();
  if (close_after_send_every == 0 || socket_index < 0 ||
      !remote_socket_send_list || !remote_connection_alive ||
      remote_socket_send_list[socket_index] < 0) {
    return;
  }

  unsigned int completed_sends = 1;
  if (remote_send_success_count) {
    completed_sends = ++remote_send_success_count[socket_index];
    if (completed_sends < close_after_send_every) {
      return;
    }
    remote_send_success_count[socket_index] = 0;
  }

#ifdef ARTS_USE_RDMA
  if (arts_rdma_send_socket_is_active_receive(
          rank, port, remote_socket_send_list[socket_index])) {
    __sync_fetch_and_add(&rdma_full_duplex_skip_close_count, 1ULL);
    ARTS_TRACE_RDMA("full-duplex keeping shared socket after %u complete "
                    "send(s) peer=%d port=%u fd=%d",
                    completed_sends, rank, port,
                    remote_socket_send_list[socket_index]);
    arts_rdma_maybe_print_summary("full-duplex-skip-close");
    return;
  }
#endif

  ARTS_TRACE_RDMA("close send socket after %u complete send(s) peer=%d "
                  "port=%u fd=%d",
                  completed_sends, rank, port,
                  remote_socket_send_list[socket_index]);
  INCREMENT_NUM_REMOTE_CLOSE_AFTER_SEND_BY(1);
  remote_connection_alive[socket_index] = false;
#ifdef ARTS_USE_RDMA
  if (close_after_send_every > 1U) {
    int retire_errno = 0;
    if (!arts_send_rdma_socket_retire(remote_socket_send_list[socket_index], port,
                                      &retire_errno)) {
      ARTS_WARN("%s failed to send RDMA socket retire marker rank %u -> rank %d "
                "port %u fd=%d errno=%d: %s",
                arts_transport_name(), arts_global_message_table->my_rank, rank,
                port, remote_socket_send_list[socket_index], retire_errno,
                strerror(retire_errno));
      arts_rdma_maybe_print_summary("retire-send-fail");
    } else {
      arts_rdma_maybe_print_summary("retire-send");
    }
  }
  bool release_active_connect_permit =
      arts_rdma_take_active_connect_permit_for_socket(socket_index);
  arts_defer_rdma_close_socket_fd_with_permit(
      &remote_socket_send_list[socket_index], false, "close_after_send",
      release_active_connect_permit);
#else
  arts_close_socket_fd(&remote_socket_send_list[socket_index]);
#endif
}

// inline int arts_actual_send(char * message, unsigned int length, int rank,
// int port)
uint64_t arts_actual_send(char *message, uint64_t length, int rank, int port) {
  /* RSEND returns ssize_t; keep the send count 64-bit for multi-GB DBs. */
  int64_t res = 0;
  uint64_t total = 0;
  uint64_t original_length = length;
  uint64_t max_bytes = arts_send_max_bytes_per_call();
  unsigned int max_iters = arts_send_max_iters_per_call();
  int iterations = 0;
  while (length != 0 && res >= 0) {
    res = RSEND(remote_socket_send_list[(rank * ports) + port], message + total,
                length, MSG_DONTWAIT);
    if (res >= 0) {
      if (res == 0) {
        struct arts_remote_packet_s *pk = (struct arts_remote_packet_s *)message;
        ARTS_WARN("arts_remote_send_request %u made no progress to rank %d",
                  pk->message_type, rank);
        arts_runtime_stop();
        return -1;
      }
      total += res;
      length -= res;
      if (length != 0 && max_bytes != 0 && total >= max_bytes) {
#ifdef ARTS_USE_RDMA
        __sync_fetch_and_add(&rdma_send_cap_hit_count, 1ULL);
#endif
        INCREMENT_NUM_REMOTE_SEND_PARTIAL_BY(1);
        break;
      }
    }
    iterations++;
    if (length != 0 && max_iters != 0 &&
        iterations >= (int)max_iters) {
#ifdef ARTS_USE_RDMA
      __sync_fetch_and_add(&rdma_send_cap_hit_count, 1ULL);
#endif
      INCREMENT_NUM_REMOTE_SEND_PARTIAL_BY(1);
      break;
    }
    if (iterations > 1000000) {
      ARTS_INFO("arts_actual_send: stuck in loop, res=%d, length=%lu, "
                "total=%lu, errno=%d",
                res, length, total, errno);
      break;
    }
  }

  if (res < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      struct arts_remote_packet_s *pk = (struct arts_remote_packet_s *)message;
      ARTS_INFO(
          "arts_remote_send_request %u Socket appears to be closed to rank %d: "
          " %s",
          pk->message_type, rank, strerror(errno));
      arts_runtime_stop();
      return -1;
    }
    INCREMENT_NUM_REMOTE_SEND_EAGAIN_BY(1);
  }
  INCREMENT_BYTES_REMOTE_SENT_BY(total);
  if (total > 0) {
    INCREMENT_NUM_REMOTE_SEND_BY(1);
  }
  if (length != 0 && length != original_length) {
    INCREMENT_NUM_REMOTE_SEND_PARTIAL_BY(1);
  }
  return length;
}

uint64_t arts_remote_send_request(int rank, unsigned int queue, char *message,
                                  uint64_t length) {
  int port = (int)(queue % ports);
  int socket_index = (rank * (int)ports) + port;
  uint64_t remaining = length;
  arts_lock(&remote_socket_send_lock_list[socket_index]);
  if (arts_remote_connect(rank, port)) {
    remaining = arts_actual_send(message, length, rank, port);
    if (remaining == 0) {
      arts_close_send_socket_after_complete_send(socket_index, rank, port);
    }
  }
  arts_unlock(&remote_socket_send_lock_list[socket_index]);
  return remaining;
}

uint64_t arts_remote_send_payload_request(int rank, unsigned int queue,
                                          char *message, unsigned int length,
                                          char *payload, uint64_t length2) {
  int port = (int)(queue % ports);
  int socket_index = (rank * (int)ports) + port;
  uint64_t remaining = length + length2;
  arts_lock(&remote_socket_send_lock_list[socket_index]);
  if (arts_remote_connect(rank, port)) {
    uint64_t temp_length = arts_actual_send(message, length, rank, port);
    if (temp_length == (uint64_t)-1) {
      remaining = (uint64_t)-1;
    } else if (temp_length) {
      remaining = temp_length + length2;
    } else {
      remaining = arts_actual_send(payload, length2, rank, port);
    }
    if (remaining == 0) {
      arts_close_send_socket_after_complete_send(socket_index, rank, port);
    }
  }
  arts_unlock(&remote_socket_send_lock_list[socket_index]);
  return remaining;
}

bool arts_remote_setup_incoming() {
  // ARTS_INFO("%d", FD_SETSIZE);
  int i;
  int j;
  unsigned int *my_ports =
      arts_global_message_table->table[arts_global_message_table->my_rank]
          .ports;
  socklen_t s_length = sizeof(struct sockaddr);
  int count = (int)(arts_global_message_table->table_length - 1);
  unsigned int stagger_us = arts_connect_stagger_us();
  remote_expected_incoming_count = (unsigned int)count * ports;
  remote_incoming_connected_count = 0;

  ARTS_INFO("ARTS transport: %s (rank %u/%u)", arts_transport_kind_name(),
            arts_global_rank_id, arts_global_rank_count);

  if (stagger_us > 0) {
    ARTS_INFO("%s startup connection staggering enabled: %u us per rank",
              arts_transport_name(), stagger_us);
  }
#ifdef ARTS_USE_RDMA
  bool rdma_connect_helper_enabled = arts_rdma_connect_helper_enabled();
  bool rdma_close_after_send = arts_close_send_after_complete_send();
  unsigned int rdma_close_after_send_every =
      arts_close_send_after_complete_send_every();
  bool rdma_receive_rpoll = arts_rdma_receive_rpoll_enabled();
  bool rdma_accept_thread = arts_rdma_accept_thread_enabled();
  bool rdma_eager_connect = arts_rdma_eager_connect_enabled();
  bool rdma_full_duplex = arts_rdma_full_duplex_enabled();
  bool rdma_allow_rsocket_reuse = arts_rdma_allow_rsocket_reuse();
  bool rdma_force_rsocket_reuse = arts_rdma_force_rsocket_reuse();
  bool rdma_abandon_close_after_send =
      arts_rdma_abandon_close_after_send_enabled();
  bool rdma_abandon_abandoned_connect_close =
      arts_rdma_abandon_abandoned_connect_close_enabled();
  unsigned int connect_timeout_ms = arts_connect_timeout_ms();
  unsigned int accept_hello_timeout_ms = arts_accept_hello_timeout_ms();
  unsigned int connect_max_retries = arts_connect_max_retries();
  unsigned int connect_retry_delay_us = arts_connect_retry_delay_us();
  unsigned int startup_connect_grace_ms =
      arts_rdma_startup_connect_grace_ms();
  enum arts_rdma_listener_ready_mode_e listener_ready_mode =
      arts_rdma_listener_ready_mode();
  unsigned int listener_ready_timeout_ms =
      arts_rdma_listener_ready_timeout_ms();
  bool first_connect_refused_fatal = arts_rdma_first_connect_refused_fatal();
  unsigned int abandoned_connect_backoff_us =
      arts_rdma_abandoned_connect_backoff_us();
  unsigned int abandoned_connect_quarantine_us =
      arts_rdma_abandoned_connect_quarantine_us();
  unsigned int connect_between_us = arts_connect_between_us();
  unsigned int connect_steady_between_us = arts_connect_steady_between_us();
  unsigned int accept_drain_limit = arts_lazy_accept_drain_limit();
  unsigned int accept_idle_us = arts_lazy_accept_idle_interval_us();
  uint64_t send_max_bytes = arts_send_max_bytes_per_call();
  unsigned int send_max_iters = arts_send_max_iters_per_call();
  unsigned int recv_packets = arts_recv_packets_per_socket_limit();
  unsigned int helper_shutdown_wait_ms =
      arts_rdma_connect_helper_shutdown_wait_ms();
  unsigned int max_active_connects = arts_rdma_max_active_connects();
  (void)arts_receive_poll_timeout_ms(false, true);
  ARTS_INFO("%s config rank %u: connect_helper=%u close_after_send=%u "
            "close_after_send_every=%u receive_rpoll=%u eager_connect=%u "
            "accept_thread=%u full_duplex=%u allow_rsocket_reuse=%u "
            "force_rsocket_reuse=%u "
            "abandon_close_after_send=%u "
            "abandon_abandoned_connect_close=%u connect_timeout_ms=%u "
            "accept_hello_timeout_ms=%u "
            "max_retries=%u retry_delay_us=%u "
            "startup_connect_grace_ms=%u listener_ready_mode=%s "
            "listener_ready_timeout_ms=%u first_connect_refused_fatal=%u "
            "abandoned_backoff_us=%u abandoned_quarantine_us=%u "
            "stagger_us=%u between_us=%u steady_between_us=%u "
            "accept_drain_limit=%u accept_idle_us=%u send_max_bytes=%lu "
            "send_max_iters=%u recv_packets_per_socket=%u "
            "helper_shutdown_wait_ms=%u "
            "max_active_connects=%u "
            "expected_incoming=%u",
            arts_transport_name(), arts_global_rank_id,
            rdma_connect_helper_enabled ? 1U : 0U,
            rdma_close_after_send ? 1U : 0U,
            rdma_close_after_send_every, rdma_receive_rpoll ? 1U : 0U,
            rdma_eager_connect ? 1U : 0U, rdma_accept_thread ? 1U : 0U,
            rdma_full_duplex ? 1U : 0U,
            rdma_allow_rsocket_reuse ? 1U : 0U,
            rdma_force_rsocket_reuse ? 1U : 0U,
            rdma_abandon_close_after_send ? 1U : 0U,
            rdma_abandon_abandoned_connect_close ? 1U : 0U,
            connect_timeout_ms,
            accept_hello_timeout_ms,
            connect_max_retries, connect_retry_delay_us,
            startup_connect_grace_ms,
            arts_rdma_listener_ready_mode_name(listener_ready_mode),
            listener_ready_timeout_ms, first_connect_refused_fatal ? 1U : 0U,
            abandoned_connect_backoff_us, abandoned_connect_quarantine_us,
            stagger_us, connect_between_us, connect_steady_between_us,
            accept_drain_limit, accept_idle_us, send_max_bytes, send_max_iters,
            recv_packets, helper_shutdown_wait_ms, max_active_connects,
            remote_expected_incoming_count);
  arts_rdma_maybe_print_summary("setup-incoming");
#endif
  ARTS_TRACE_RDMA("setup_incoming count=%d ports=%u expected=%u", count, ports,
                  remote_expected_incoming_count);

  remote_socket_recieve_list =
      (int *)arts_malloc(sizeof(int) * (size_t)(count + 1) * ports);
  remote_server_recieve_list = (struct sockaddr_in *)arts_calloc(
      (size_t)(count + 1) * ports, sizeof(struct sockaddr_in));
  poll_incoming =
      (struct pollfd *)arts_malloc(sizeof(struct pollfd) * (count + 1) * ports);
  remote_pending_receive_sockets =
      (struct arts_pending_receive_socket_s **)arts_calloc(
          (size_t)(count + 1) * ports,
          sizeof(struct arts_pending_receive_socket_s *));
  remote_pending_receive_socket_tails =
      (struct arts_pending_receive_socket_s **)arts_calloc(
          (size_t)(count + 1) * ports,
          sizeof(struct arts_pending_receive_socket_s *));
  remote_receive_socket_locks =
      (volatile unsigned int *)arts_calloc((size_t)(count + 1) * ports,
                                           sizeof(unsigned int));
#ifdef ARTS_USE_RDMA
  remote_receive_success_count =
      (unsigned int *)arts_calloc((size_t)(count + 1) * ports,
                                  sizeof(unsigned int));
  remote_receive_close_pending =
      (bool *)arts_calloc((size_t)(count + 1) * ports, sizeof(bool));
#endif
  for (i = 0; i < (count + 1) * (int)ports; i++) {
    remote_socket_recieve_list[i] = -1;
    poll_incoming[i].fd = -1;
    poll_incoming[i].events = 0;
    poll_incoming[i].revents = 0;
  }

  struct sockaddr_in *local_server_addr =
      (struct sockaddr_in *)arts_calloc(ports, sizeof(struct sockaddr_in));
  local_socket_recieve = (int *)arts_calloc(ports, sizeof(int));
  for (i = 0; i < (int)ports; i++) {
    local_socket_recieve[i] = -1;
  }

  int i_set_option;
  for (i = 0; i < (int)arts_global_message_table->port_count; i++) {
    local_socket_recieve[i] =
        arts_get_socket_listening(&local_server_addr[i], my_ports[i]);

    i_set_option = 1;
    if (RSETSOCKOPT(local_socket_recieve[i], SOL_SOCKET, SO_REUSEADDR,
                    (char *)&i_set_option, sizeof(i_set_option)) < 0) {
      ARTS_WARN("%s setsockopt(SO_REUSEADDR) failed on rank %u port %u: %s",
                arts_transport_name(), arts_global_rank_id, my_ports[i],
                strerror(errno));
      arts_ll_server_shutdown();
      return false;
    }

    int res =
        RBIND(local_socket_recieve[i], (struct sockaddr *)&local_server_addr[i],
              sizeof(local_server_addr[i]));

    if (res < 0) {
      ARTS_WARN("%s bind failed on rank %u port %u: %s",
                arts_transport_name(), arts_global_rank_id, my_ports[i],
                strerror(errno));
      arts_ll_server_shutdown();
      return false;
    }

    unsigned int fallback_backlog = (unsigned int)(2 * count);
    if (fallback_backlog < 128U) {
      fallback_backlog = 128U;
    }
#ifdef ARTS_USE_RDMA
    unsigned int rdma_backlog = (unsigned int)(4 * count) * (ports ? ports : 1U);
    if (rdma_backlog < 256U) {
      rdma_backlog = 256U;
    }
    if (fallback_backlog < rdma_backlog) {
      fallback_backlog = rdma_backlog;
    }
#endif
    unsigned int listen_backlog = arts_env_uint("ARTS_LISTEN_BACKLOG", 0U);
    if (listen_backlog == 0U) {
      listen_backlog = fallback_backlog;
    }
    res = RLISTEN(local_socket_recieve[i], (int)listen_backlog);

    if (res < 0) {
      ARTS_WARN("%s listen failed on rank %u port %u: %s",
                arts_transport_name(), arts_global_rank_id, my_ports[i],
                strerror(errno));
      arts_ll_server_shutdown();
      return false;
    }
    arts_configure_transport_socket(local_socket_recieve[i]);
    ARTS_TRACE_RDMA("listen port_index=%d port=%u fd=%d backlog=%u", i,
                    my_ports[i], local_socket_recieve[i], listen_backlog);
  }

  arts_free(local_server_addr);

  FD_ZERO(&read_set);
#ifdef ARTS_USE_RDMA
  if (arts_transport_uses_rdma()) {
    unsigned int startup_connect_grace_ms =
        arts_rdma_startup_connect_grace_ms();
    rdma_startup_connect_grace_until =
        startup_connect_grace_ms == 0
            ? 0
            : arts_get_time_stamp() +
                  ((uint64_t)startup_connect_grace_ms * 1000000ULL);
  }
#endif
  ARTS_INFO("%s lazy remote connection setup enabled on rank %u: listening on "
            "%u port(s), accepting up to %u incoming peer sockets on demand",
            arts_transport_name(), arts_global_rank_id, ports,
            remote_expected_incoming_count);

#ifdef ARTS_USE_RDMA
  if (arts_transport_uses_rdma()) {
    __sync_lock_test_and_set(&rdma_startup_network_setup_active, 1U);
  }
  rdma_listener_ready_incoming_count = 0;
  arts_rdma_start_accept_thread();
  if (!arts_rdma_wait_for_listener_readiness()) {
    arts_ll_server_shutdown();
    __sync_lock_release(&rdma_startup_network_setup_active);
    return false;
  }
  __sync_lock_release(&rdma_startup_network_setup_active);
#endif

  return true;
}

void arts_remote_setup_outgoing() {
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
  remote_send_success_count =
      (unsigned int *)arts_calloc((size_t)count * ports, sizeof(unsigned int));
  remote_connect_success_seen =
      (unsigned int *)arts_calloc((size_t)count * ports, sizeof(unsigned int));
  remote_connect_retry_count =
      (unsigned int *)arts_calloc((size_t)count * ports, sizeof(unsigned int));
  remote_connect_retry_after =
      (uint64_t *)arts_calloc((size_t)count * ports, sizeof(uint64_t));
#ifdef ARTS_USE_RDMA
  remote_connect_abandoned_inflight =
      (volatile unsigned int *)arts_calloc((size_t)count * ports,
                                           sizeof(unsigned int));
  remote_connect_abandoned_generation =
      (unsigned int *)arts_calloc((size_t)count * ports, sizeof(unsigned int));
  remote_connect_abandoned_until =
      (uint64_t *)arts_calloc((size_t)count * ports, sizeof(uint64_t));
  remote_connect_active_permit =
      (volatile unsigned int *)arts_calloc((size_t)count * ports,
                                           sizeof(unsigned int));
#endif
  for (i = 0; i < count * (int)ports; i++) {
    remote_socket_send_list[i] = -1;
  }
  ARTS_TRACE_RDMA("setup_outgoing ranks=%d ports=%u", count, ports);

  for (i = 0; i < count; i++) {
    unsigned int *target_ports = arts_global_message_table->table[i].ports;

    ARTS_INFO("arts_remote_setup_outgoing: node %d ip_list='%s' port=%u", i,
              ip_list + ((ptrdiff_t)100 * i), target_ports[0]);

    for (j = 0; j < ports; j++) {
      remote_socket_send_list[(i * ports) + j] = -1;
      struct sockaddr_in *outgoing_socket =
          remote_server_send_list + ((size_t)i * ports) + j;
      memset((char *)outgoing_socket, 0, sizeof(*outgoing_socket));
      outgoing_socket->sin_family = AF_INET;
      outgoing_socket->sin_addr.s_addr =
          inet_addr(ip_list + ((ptrdiff_t)100 * i));
      outgoing_socket->sin_port = htons(target_ports[j]);
      ARTS_TRACE_RDMA("outgoing target rank=%d port_index=%d addr=%s:%u", i, j,
                      ip_list + ((ptrdiff_t)100 * i), target_ports[j]);
    }
  }
}

void arts_remote_set_thread_inbound_queues(unsigned int start,
                                           unsigned int stop) {
  thread_start = start;
  thread_stop = stop;
  // ARTS_INFO_MASTER("%d %d", start, stop);
  unsigned int size = stop - start;
  bypass_buf = (char **)arts_malloc(sizeof(char *) * size);
  bypass_packet_size = (uint64_t *)arts_malloc(sizeof(uint64_t) * size);
  re_recieve_res = (int64_t *)arts_calloc(size, sizeof(int64_t));
  re_recieve_packet = (void **)arts_calloc(size, sizeof(void *));
  max_incoming = (bool *)arts_calloc(size, sizeof(bool));
  for (int i = 0; i < size; i++) {
    bypass_buf[i] = (char *)arts_malloc(PACKET_SIZE);
    bypass_packet_size[i] = PACKET_SIZE;
  }
  ARTS_TRACE_RDMA("inbound queue assigned start=%u stop=%u slots=%u",
                  thread_start, thread_stop, size);
}

void arts_remote_thread_inbound_queues_cleanup() {
  unsigned int size = thread_stop - thread_start;
  for (int i = 0; i < size; i++) {
    arts_free(bypass_buf[i]);
  }
  arts_free(bypass_buf);
  arts_free(bypass_packet_size);
  arts_free(re_recieve_res);
  arts_free(re_recieve_packet);
  arts_free(max_incoming);
}

bool max_out_buffs(unsigned int ignore) {
  int64_t res;
  int64_t res2;
  struct arts_remote_packet_s *packet;
  // ARTS_INFO("MAX");
  res = RPOLL(poll_incoming + thread_start, thread_stop - thread_start,
              arts_receive_poll_timeout_ms(true, false));
  unsigned int pos;

  if (res == -1) {
    if (errno == EINTR) {
      return true;
    }
    arts_shutdown();
    arts_runtime_stop();
  }
  if (res > 0) {
    // ARTS_INFO("MAX LOOP");
    for (int i = (int)thread_start; i < (int)thread_stop; i++) {
      pos = i - (int)thread_start;
      if (i != ignore && poll_incoming[i].revents & POLLIN) {
        max_out_working = true;
        if (re_recieve_res[pos] == 0) {
          packet = (struct arts_remote_packet_s *)bypass_buf[pos];
          res = RRECV(remote_socket_recieve_list[i], bypass_buf[pos],
                      bypass_packet_size[pos], 0);
        } else {
          // packet = re_recieve_packet[pos];
          packet = (struct arts_remote_packet_s *)bypass_buf[pos];
          res = re_recieve_res[pos];
          re_recieve_res[pos] = 0;
        }
        if (res > 0) {
          while (res < bypass_packet_size[pos]) {
            if (bypass_buf[pos] != (char *)packet) {
              memmove(bypass_buf[pos], packet, res);
              packet = (struct arts_remote_packet_s *)bypass_buf[pos];
            }
            res2 = RRECV(remote_socket_recieve_list[i], bypass_buf[pos] + res,
                         bypass_packet_size[pos] - res, MSG_DONTWAIT);

	    if (res2 < 0) {
		      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
#ifdef ARTS_USE_RDMA
		        __sync_fetch_and_add(&rdma_recv_error_count, 1ULL);
#endif
		        ARTS_INFO("Error on recv return 0 %d %d", errno, EAGAIN);
		        arts_close_receive_socket_index(i);
		        arts_shutdown();
		        arts_runtime_stop();
		        return false;
		      }
		      INCREMENT_NUM_REMOTE_RECV_EAGAIN_BY(1);

	      re_recieve_res[pos] = res;
	      max_incoming[pos] = true;
              break;
            }
            if (res2 == 0) {
#ifdef ARTS_USE_RDMA
              __sync_fetch_and_add(&rdma_recv_eof_count, 1ULL);
#endif
              arts_close_receive_socket_index(i);
              arts_shutdown();
              arts_runtime_stop();
              return false;
            }
            res += res2;
          }
          max_incoming[pos] = true;
          re_recieve_res[pos] = res;
	        } else if (res == -1) {
	          if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
	            INCREMENT_NUM_REMOTE_RECV_EAGAIN_BY(1);
	            continue;
	          }
#ifdef ARTS_USE_RDMA
          __sync_fetch_and_add(&rdma_recv_error_count, 1ULL);
#endif
          ARTS_INFO("Error on recv socket return 0");
          ARTS_INFO("error %s", strerror(errno));
          arts_close_receive_socket_index(i);
          arts_shutdown();
          arts_runtime_stop();
          return false;
        } else if (res == 0) {
#ifdef ARTS_USE_RDMA
          __sync_fetch_and_add(&rdma_recv_eof_count, 1ULL);
#endif
          arts_close_receive_socket_index(i);
          continue;
        }
      }
    }
  }
  return true;
}

bool arts_server_try_to_receive(
    char **in_buffer, const int *in_packet_size,
    const volatile unsigned int *remote_steal_lock) {
  if (arts_receiver_wakeup_requested()) {
    return false;
  }

  (void)in_buffer;
  (void)in_packet_size;
  (void)remote_steal_lock;
  int i;
  int steal_handler_thread = 0;
  int64_t res;
  int64_t res2;
  struct arts_remote_packet_s *packet;
  fd_set temp_set;
  bool has_live_inbound = arts_thread_has_live_inbound();
  bool awaiting_lazy_accept =
      remote_incoming_connected_count < remote_expected_incoming_count ||
      arts_close_send_after_complete_send();
  int time_out =
      arts_receive_poll_timeout_ms(has_live_inbound, awaiting_lazy_accept);
  struct timeval sel_timeout;
  unsigned int pos;

	  bool accepted_connection = false;
	  if (arts_should_try_lazy_accept(has_live_inbound, awaiting_lazy_accept)) {
	    int accept_timeout_ms =
	        has_live_inbound ? 0 : ARTS_LAZY_ACCEPT_POLL_MS;
	    accepted_connection =
        arts_remote_accept_pending(arts_lazy_accept_drain_limit(),
                                   accept_timeout_ms);
    has_live_inbound = arts_thread_has_live_inbound();
    if (!has_live_inbound) {
      return accepted_connection;
    }
  }

  bool scan_all_inbound = false;
#ifdef ARTS_USE_RDMA
  if (arts_rdma_receive_rpoll_enabled()) {
    res = RPOLL(poll_incoming + thread_start, thread_stop - thread_start,
                time_out);
  } else {
    (void)time_out;
    scan_all_inbound = true;
    res = 1;
  }
#else
  res =
      RPOLL(poll_incoming + thread_start, thread_stop - thread_start, time_out);
#endif

  if (res == -1) {
    if (arts_receiver_wakeup_requested()) {
      return accepted_connection;
    }
    if (errno == EINTR) {
      return accepted_connection;
    }
    arts_shutdown();
    arts_runtime_stop();
  }

  unsigned int space_left;
  bool packet_incoming_on_a_socket = false;
  bool goto_next = false;
  if (res > 0) {
    // ARTS_INFO("POLL");
    time_out = 1;
    max_out_working = true;
    while (max_out_working) {
      max_out_working = false;
      for (i = (int)thread_start; i < (int)thread_stop; i++) {
        if (arts_receiver_wakeup_requested()) {
          return packet_incoming_on_a_socket || accepted_connection;
        }
        pos = i - thread_start;
        goto_next = false;
        // if( poll_incoming[i].revents & POLLIN )
        // if(!max_out_buffs(-1))
        //     return false;
        // if( max_incoming[pos] )
        if (remote_socket_recieve_list && remote_socket_recieve_list[i] >= 0 &&
            (scan_all_inbound || (poll_incoming[i].revents & POLLIN))) {
          // ARTS_INFO("Here2");
          max_incoming[pos] = false;
          if (re_recieve_res[pos] == 0) {
            // ARTS_INFO("Here3a");
            packet = (struct arts_remote_packet_s *)bypass_buf[pos];
            res = RRECV(remote_socket_recieve_list[i], bypass_buf[pos],
                        bypass_packet_size[pos], MSG_DONTWAIT);
            if (res > 0) {
              INCREMENT_BYTES_REMOTE_RECEIVED_BY(res);
            }
          } else {
            // packet = re_recieve_packet[pos];
            packet = (struct arts_remote_packet_s *)bypass_buf[pos];
            res = re_recieve_res[pos];
            re_recieve_res[pos] = 0;
          }
	          if (res > 0) {
	            packet_incoming_on_a_socket = true;
	            unsigned int packets_processed_this_socket = 0;
	            unsigned int packet_limit = arts_recv_packets_per_socket_limit();
	            while (res > 0) {
              while (res < sizeof(struct arts_remote_packet_s)) {
                if (bypass_buf[pos] != (char *)packet) {
                  memmove(bypass_buf[pos], packet, res);
                  packet = (struct arts_remote_packet_s *)bypass_buf[pos];
                }
                if (arts_receiver_wakeup_requested()) {
                  return packet_incoming_on_a_socket || accepted_connection;
                }
                res2 =
                    RRECV(remote_socket_recieve_list[i], bypass_buf[pos] + res,
                          bypass_packet_size[pos] - res, MSG_DONTWAIT);
                if (res2 > 0) {
                  INCREMENT_BYTES_REMOTE_RECEIVED_BY(res2);
                }

		                if (res2 < 0) {
			                  if (errno != EAGAIN && errno != EWOULDBLOCK &&
			                      errno != EINTR) {
#ifdef ARTS_USE_RDMA
		                    __sync_fetch_and_add(&rdma_recv_error_count, 1ULL);
#endif
		                    ARTS_INFO("Error on recv return 0 %d %d", errno, EAGAIN);
	                    arts_close_receive_socket_index(i);
	                    arts_shutdown();
		                    arts_runtime_stop();
	                    return false;
		                  }
	                  INCREMENT_NUM_REMOTE_RECV_EAGAIN_BY(1);

	                  re_recieve_res[pos] = res;
	                  goto_next = true;
                  break;
                }
                if (res2 == 0) {
#ifdef ARTS_USE_RDMA
                  __sync_fetch_and_add(&rdma_recv_eof_count, 1ULL);
#endif
                  arts_close_receive_socket_index(i);
                  arts_shutdown();
                  arts_runtime_stop();
                  return false;
                }
                // space_left-=res2;
                res += res2;
              }
              if (goto_next) {
                break;
              }

              if (!arts_remote_packet_header_valid(packet)) {
                arts_reject_remote_packet_header(i, packet);
                return false;
              }

              if (bypass_packet_size[pos] < packet->size) {
                uint64_t new_buf_size = (packet->size > (1ULL << 28))
                                            ? packet->size
                                            : packet->size * 4;
                char *next_buf = (char *)arts_malloc(new_buf_size);

                memcpy(next_buf, bypass_buf[pos], bypass_packet_size[pos]);

                arts_free(bypass_buf[pos]);

                packet = (struct arts_remote_packet_s *)(next_buf +
                                                         (((char *)packet) -
                                                          (bypass_buf[pos])));
                bypass_buf[pos] = next_buf;
                bypass_packet_size[pos] = new_buf_size;
              }

              while (res < packet->size) {
                if (bypass_buf[pos] != (char *)packet) {
                  memmove(bypass_buf[pos], packet, res);
                  packet = (struct arts_remote_packet_s *)bypass_buf[pos];
                }
                if (arts_receiver_wakeup_requested()) {
                  return packet_incoming_on_a_socket || accepted_connection;
                }
                res2 =
                    RRECV(remote_socket_recieve_list[i], bypass_buf[pos] + res,
                          bypass_packet_size[pos] - res, MSG_DONTWAIT);
                if (res2 > 0) {
                  INCREMENT_BYTES_REMOTE_RECEIVED_BY(res2);
                }
		                if (res2 < 0) {
			                  if (errno != EAGAIN && errno != EWOULDBLOCK &&
			                      errno != EINTR) {
#ifdef ARTS_USE_RDMA
		                    __sync_fetch_and_add(&rdma_recv_error_count, 1ULL);
#endif
		                    ARTS_INFO("Error on recv return 0 %d %d", errno, EAGAIN);
	                    ARTS_INFO("error %s", strerror(errno));
	                    arts_close_receive_socket_index(i);
	                    arts_shutdown();
		                    arts_runtime_stop();
	                    return false;
		                  }
	                  INCREMENT_NUM_REMOTE_RECV_EAGAIN_BY(1);
	                  re_recieve_res[pos] = res;
	                  goto_next = true;
                  break;
                }
                if (res2 == 0) {
#ifdef ARTS_USE_RDMA
                  __sync_fetch_and_add(&rdma_recv_eof_count, 1ULL);
#endif
                  arts_close_receive_socket_index(i);
                  arts_shutdown();
                  arts_runtime_stop();
                  return false;
                }
                res += res2;
              }
              if (goto_next) {
                break;
              }
              INCREMENT_NUM_REMOTE_RECEIVE_BY(1);
              unsigned int processed_rank = packet->rank;
              unsigned int processed_msg = packet->message_type;
              uint64_t processed_size = packet->size;
              ARTS_TRACE_RDMA("recv packet index=%d from=%u msg=%u size=%lu",
                              i, processed_rank, processed_msg,
                              processed_size);
#ifdef ARTS_USE_RDMA
              if (arts_is_rdma_socket_retire_packet(packet)) {
                __sync_fetch_and_add(&rdma_socket_retire_recv_count, 1ULL);
                ARTS_TRACE_RDMA("recv retire index=%d from=%u size=%lu "
                                "buffer_remaining=%ld",
                                i, processed_rank, processed_size,
                                (long)res);
                res -= (int64_t)processed_size;
                packet = (struct arts_remote_packet_s *)(((char *)packet) +
                                                         processed_size);
                if (res == 0) {
                  if (remote_receive_close_pending) {
                    remote_receive_close_pending[i] = false;
                  }
                  arts_close_receive_socket_index(i);
                  max_out_working = true;
                  break;
                }
                if (remote_receive_close_pending) {
                  remote_receive_close_pending[i] = true;
                }
                continue;
              }
#endif
              arts_server_process_packet(packet);
              if (processed_msg >= ARTS_EPOCH_INIT_MSG ||
                  processed_msg == ARTS_REMOTE_SHUTDOWN_MSG) {
                ARTS_TRACE_RDMA(
                    "recv packet processed index=%d from=%u msg=%u size=%lu",
                    i, processed_rank, processed_msg, processed_size);
              }
              if (arts_receiver_wakeup_requested()) {
                return true;
              }

		              res -= (int64_t)processed_size;
		              packet = (struct arts_remote_packet_s *)(((char *)packet) +
		                                                       processed_size);
		              packets_processed_this_socket++;
              if (arts_should_close_receive_after_packet(i, res == 0)) {
                ARTS_TRACE_RDMA("close bounded recv after packet index=%d "
                                "from=%u msg=%u size=%lu",
                                i, processed_rank, processed_msg,
                                processed_size);
		                arts_close_receive_socket_index(i);
		                max_out_working = true;
		                break;
		              }
              if (packet_limit != 0 &&
                  packets_processed_this_socket >= packet_limit && res > 0) {
#ifdef ARTS_USE_RDMA
	                __sync_fetch_and_add(&rdma_recv_cap_hit_count, 1ULL);
#endif
	                if (bypass_buf[pos] != (char *)packet) {
	                  memmove(bypass_buf[pos], packet, res);
	                }
	                re_recieve_res[pos] = res;
	                max_out_working = true;
	                INCREMENT_NUM_REMOTE_RECV_PARTIAL_BY(1);
	                break;
	              }
	            }
	          } else if (res == -1) {
	            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
	              INCREMENT_NUM_REMOTE_RECV_EAGAIN_BY(1);
	              continue;
	            }
#ifdef ARTS_USE_RDMA
            __sync_fetch_and_add(&rdma_recv_error_count, 1ULL);
#endif
            arts_close_receive_socket_index(i);
            arts_shutdown();
            arts_runtime_stop();
            return false;
          } else if (res == 0) {
#ifdef ARTS_USE_RDMA
            __sync_fetch_and_add(&rdma_recv_eof_count, 1ULL);
#endif
            arts_close_receive_socket_index(i);
            continue;
          }
        }
      }
    }
	    awaiting_lazy_accept =
	        remote_incoming_connected_count < remote_expected_incoming_count ||
	        arts_close_send_after_complete_send();
	    if (arts_should_try_lazy_accept(true, awaiting_lazy_accept)) {
	      accepted_connection =
	          arts_remote_accept_pending(arts_lazy_accept_drain_limit(), 0) ||
	          accepted_connection;
	    }
	    return packet_incoming_on_a_socket || accepted_connection;
	  }
	  awaiting_lazy_accept =
	      remote_incoming_connected_count < remote_expected_incoming_count ||
	      arts_close_send_after_complete_send();
	  if (arts_should_try_lazy_accept(has_live_inbound, awaiting_lazy_accept)) {
	    accepted_connection =
	        arts_remote_accept_pending(arts_lazy_accept_drain_limit(), 0) ||
        accepted_connection;
  }
  return accepted_connection;
}

int arts_get_new_socket() {
  int socket_out = RSOCKET(PF_INET, SOCK_STREAM, 0);
  if (socket_out < 0) {
    ARTS_ERROR("socket() failed: %s", strerror(errno));
  }
  arts_configure_transport_socket(socket_out);
  return socket_out;
}

int arts_get_socket_listening(struct sockaddr_in *listening_socket,
                              unsigned int port) {
  memset((char *)listening_socket, 0, sizeof(*listening_socket));
  int socket_out = RSOCKET(PF_INET, SOCK_STREAM, 0);
  if (socket_out < 0) {
    ARTS_ERROR("socket() failed: %s", strerror(errno));
  }
  arts_configure_transport_socket(socket_out);
  listening_socket->sin_family = AF_INET;
  listening_socket->sin_addr.s_addr = htonl(INADDR_ANY);
  listening_socket->sin_port = htons(port);
  return socket_out;
}

int arts_get_socket_outgoing(struct sockaddr_in *outgoing_socket,
                             unsigned int port, in_addr_t s_addr) {
  memset((char *)outgoing_socket, 0, sizeof(*outgoing_socket));
  int socket_out = RSOCKET(PF_INET, SOCK_STREAM, 0);
  if (socket_out < 0) {
    ARTS_ERROR("socket() failed: %s", strerror(errno));
  }
  arts_configure_transport_socket(socket_out);
  outgoing_socket->sin_family = AF_INET;
  outgoing_socket->sin_addr.s_addr = s_addr;
  outgoing_socket->sin_port = htons(port);
  return socket_out;
}
