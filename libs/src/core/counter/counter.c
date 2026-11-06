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
#include "arts/counter/counter.h"

#include "arts/utils/stdatomic_compat.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "arts.h"
#include "arts/counter/json.h"
#include "arts/runtime_state.h"
#include "arts/system/print.h"
#include "arts/system/shutdown.h"
#include "arts/system/threads.h"
#include "arts/transport/net.h"      /* outbound send helpers */
#include "arts/transport/protocol.h" /* wire packet structs */
#include "arts/utils/malloc.h"

// Arrays are defined as static const in Preamble.h (included via arts.h)

// Thread-local counter storage - simple array of counters.
// Each thread updates these directly during execution.
// No captures here - capture thread handles periodic snapshots separately.
ARTS_THREAD_LOCAL_IE arts_counter_t
    arts_thread_local_counters[NUM_COUNTER_TYPES];

// Capture thread state - only used for periodic counter capture.
//
// One word decides the capture thread's life against the stop.  The start
// creates the thread, stores its handle and only then moves the word
// IDLE -> RUNNING (release).  The stop moves it RUNNING -> STOPPED (acquire,
// so it reads the handle) and joins, or IDLE -> STOPPED and joins nothing; a
// start whose CAS then finds STOPPED joins its own thread, which never
// captured.  The start and the stop change the word, and signal capture_cond,
// only under capture_mutex, and the thread reads the word under it before
// each wait, so no signal falls between a check and a wait.
enum { CAPTURE_IDLE, CAPTURE_RUNNING, CAPTURE_STOPPED };
static _Atomic(unsigned int) capture_state = CAPTURE_IDLE;
static pthread_t capture_thread;
static pthread_mutex_t capture_mutex = PTHREAD_MUTEX_INITIALIZER;
/* On CLOCK_MONOTONIC; the start initializes it before the thread exists. */
static pthread_cond_t capture_cond;

#ifdef ARTS_DEBUG_INJECTION
void (*arts_capture_started_hook)(void) = NULL;

unsigned int arts_counter_capture_state(void) {
  return atomic_load_explicit(&capture_state, memory_order_acquire);
}
#endif

// Time synchronization: offset = workerTime - masterTime (positive if the
// worker is ahead), so local_time - offset = masterTime.
//
// A non-master rank sends one request before any of its EDTs can run
// (arts_counter_capture_sync_begin), and its capture start waits, bounded, for
// the response.  The word below decides the offset once: the response handler
// claims it WAITING -> RECEIVING, stores the offset and publishes RECEIVED
// (release); the waiter, at its timeout, moves it WAITING -> TIMED_OUT and the
// offset stays 0.  A response that finds the word anything but WAITING writes
// nothing, so once the waiter has decided the offset never changes.  The
// offset's readers come after an acquire of RECEIVED: the capture thread,
// which the waiter starts only after its own, and the record's writer,
// through arts_counter_clock_sync's.  The master rank's capture start stores
// MASTER: its offset is 0, and no request or response concerns it.  T3 is
// taken when the response is dispatched, so the round trip includes any wait
// for this rank's progress threads.
enum {
  TIME_SYNC_WAITING = 0,
  TIME_SYNC_RECEIVING,
  TIME_SYNC_RECEIVED,
  TIME_SYNC_TIMED_OUT,
  TIME_SYNC_MASTER,
};
static _Atomic(unsigned int) time_sync_state = TIME_SYNC_WAITING;
/* Written once, by the response that claims time_sync_state. */
static _Atomic(int64_t) time_sync_offset = 0;

// Timestamp on the master node's clock; called only once the time sync has
// decided.
static inline uint64_t arts_get_synced_time_stamp(void) {
  int64_t offset =
      atomic_load_explicit(&time_sync_offset, memory_order_relaxed);
  return (uint64_t)((int64_t)arts_get_time_stamp() - offset);
}

static uint64_t arts_counter_capture_counter(arts_counter_t *counter) {
  uint64_t expected =
      atomic_load_explicit(&counter->start, memory_order_relaxed);
  while (expected) {
    uint64_t start = arts_get_time_stamp();
    if (atomic_compare_exchange_strong_explicit(&counter->start, &expected,
                                                start, memory_order_relaxed,
                                                memory_order_relaxed)) {
      atomic_fetch_add_explicit(&counter->count, start - expected,
                                memory_order_relaxed);
      expected = 0;
    }
  }
  return atomic_load_explicit(&counter->count, memory_order_relaxed);
}

/* The CLOCK_MONOTONIC instant ns from now, as capture_cond's waits take it. */
static struct timespec arts_capture_deadline(uint64_t ns) {
  uint64_t at = arts_get_time_stamp() + ns;
  return (struct timespec){.tv_sec = (time_t)(at / 1000000000ULL),
                           .tv_nsec = (long)(at % 1000000000ULL)};
}

static void *arts_counter_capture_thread(void *args) {
  (void)args; // Unused - capture thread doesn't need its own counters

  // Validate counter_capture_interval to prevent division by zero
  if (arts_node_info.counter_capture_interval == 0) {
    ARTS_INFO("counter_capture_interval is 0, capture thread exiting");
    return NULL;
  }

  // Validate interval to prevent overflow (max ~18 billion ms before overflow)
  if (arts_node_info.counter_capture_interval > UINT64_MAX / 1000000) {
    ARTS_INFO("counter_capture_interval too large, capture thread exiting");
    return NULL;
  }

  // milli to nano
  uint64_t interval_ns = arts_node_info.counter_capture_interval * 1000000;

  // Time synchronization: align captures to synchronized time.
  // All nodes use master node's time (via arts_get_synced_time_stamp) so they
  // all capture at the same logical time, preventing drift between nodes. A
  // rank whose time sync timed out uses its own clock instead, so its capture
  // instants do not line up with those of ranks on other hosts.
  //
  // Epochs are RELATIVE to capture thread start, not absolute timestamps.
  // This makes epoch numbers small (0, 1, 2...) and meaningful for comparing
  // captures across different runs. All nodes use the same baseline alignment
  // so their epoch numbers match.
  uint64_t synced_time = arts_get_synced_time_stamp();

  // Compute baseline: align to the current interval boundary
  // This ensures all nodes that start within the same interval get the same
  // baseline, producing identical epoch numbers across the cluster.
  uint64_t baseline_time = (synced_time / interval_ns) * interval_ns;

  // Calculate the capture epoch relative to baseline (starts at 0)
  uint64_t capture_epoch = 0;
  // Next capture will be at the first interval boundary after baseline
  // (i.e., baseline + interval_ns for epoch 0)
  uint64_t next_capture_time = baseline_time + interval_ns;

  pthread_mutex_lock(&capture_mutex);
  for (;;) {
    unsigned int state =
        atomic_load_explicit(&capture_state, memory_order_acquire);
    if (state == CAPTURE_STOPPED) {
      break;
    }
    if (state == CAPTURE_IDLE) {
      // The start signals once its CAS has decided the word.
      pthread_cond_wait(&capture_cond, &capture_mutex);
      continue;
    }

    synced_time = arts_get_synced_time_stamp();
    if (synced_time < next_capture_time) {
      // Sleep until the aligned capture time (in synced time); the stop's
      // signal or a spurious wake makes the pass read the word again.
      struct timespec deadline =
          arts_capture_deadline(next_capture_time - synced_time);
      pthread_cond_timedwait(&capture_cond, &capture_mutex, &deadline);
      continue;
    }

    // The capture time has come.  Every whole interval since it passed is an
    // epoch the last capture overran; jump past them to avoid drift.
    uint64_t late_ns = synced_time - next_capture_time;
    uint64_t intervals_skipped = late_ns / interval_ns;
    uint64_t lagging_capture_time = next_capture_time;
    capture_epoch += intervals_skipped + 1;
    next_capture_time += (intervals_skipped + 1) * interval_ns;
    uint64_t current_epoch = capture_epoch;
    pthread_mutex_unlock(&capture_mutex);

    if (intervals_skipped) {
      ARTS_INFO(
          "Counter capture lagging: synced_time=%lf ms, "
          "next_capture_time=%lf ms, lateBy=%lf ms, skipping %lu interval(s)",
          (double)synced_time / 1000000.0,
          (double)lagging_capture_time / 1000000.0, (double)late_ns / 1000000.0,
          intervals_skipped);
    }

    // Capture counters from all threads - store in nodeInfo.capture_arrays.
    // Every thread registered before this thread started, and a runtime
    // thread never exits, so every row stays alive (see live_counters).
    for (unsigned int i = 0; i < NUM_COUNTER_TYPES; i++) {
      if (arts_counter_mode_array[i] == ARTS_COUNTER_MODE_PERIODIC) {
        for (unsigned int t = 0; t < arts_node_info.total_thread_count; t++) {
          arts_counter_t *thread_counters = arts_node_info.live_counters[t];
          arts_counter_capture_t capture;
          capture.epoch = current_epoch;
          capture.value = arts_counter_capture_counter(&thread_counters[i]);
          if (arts_node_info.capture_arrays[t][i]) {
            arts_push_to_array_list(arts_node_info.capture_arrays[t][i],
                                    &capture);
          }
        }
      }
    }
    pthread_mutex_lock(&capture_mutex);
  }
  pthread_mutex_unlock(&capture_mutex);
  return NULL;
}

// Need capture thread for PERIODIC mode counters
static bool arts_counter_capture_needed(void) {
  for (unsigned int i = 0; i < NUM_COUNTER_TYPES; i++) {
    if (arts_counter_mode_array[i] == ARTS_COUNTER_MODE_PERIODIC) {
      return true;
    }
  }
  return false;
}

void arts_counter_capture_sync_begin(void) {
  // The request leaves before any EDT of this rank can end the run, so no
  // closed send gate drops it, whatever the program does.
  if (arts_counter_capture_needed() &&
      arts_global_rank_id != arts_global_master_rank_id) {
    arts_send_time_sync_request();
  }
}

void arts_counter_capture_start() {
  if (arts_counter_capture_needed()) {
    // RTT-based time synchronization: workers send request to master,
    // master responds with its timestamp, workers calculate offset using RTT.
    // This provides better accuracy than one-way broadcast.

    if (arts_global_rank_id == arts_global_master_rank_id) {
      // Master node: the offset is 0 and no response comes.
      atomic_store_explicit(&time_sync_state, TIME_SYNC_MASTER,
                            memory_order_relaxed);
      ARTS_INFO("Time sync: Master node (rank %u), offset=0",
                arts_global_master_rank_id);
    } else {
      // Worker nodes: wait for the response to the request
      // arts_counter_capture_sync_begin sent.
      uint64_t timeout = arts_get_time_stamp() + 500000000ULL; // 500ms
      while (atomic_load_explicit(&time_sync_state, memory_order_acquire) !=
                 TIME_SYNC_RECEIVED &&
             arts_get_time_stamp() < timeout) {
        usleep(1000); // Wait 1ms
      }
      unsigned int state = TIME_SYNC_WAITING;
      if (atomic_compare_exchange_strong_explicit(
              &time_sync_state, &state, TIME_SYNC_TIMED_OUT,
              memory_order_acquire, memory_order_acquire)) {
        ARTS_WARN("Time sync: Timeout waiting for master response, "
                  "using local time (offset=0); this rank's capture instants "
                  "will not line up with those of ranks on other hosts");
      } else {
        /* A response claimed the word: it publishes RECEIVED next. */
        while (state != TIME_SYNC_RECEIVED) {
          arts_runtime_idle_pause();
          state = atomic_load_explicit(&time_sync_state, memory_order_acquire);
        }
      }
    }

    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
    pthread_cond_init(&capture_cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    int ret = pthread_create(&capture_thread, NULL, arts_counter_capture_thread,
                             NULL);
    if (ret) {
      // The word stays IDLE, so the stop joins nothing.
      ARTS_WARN("Failed to create capture thread: %d", ret);
      return;
    }

#ifdef ARTS_DEBUG_INJECTION
    if (arts_capture_started_hook != NULL) {
      arts_capture_started_hook();
    }
#endif
    pthread_mutex_lock(&capture_mutex);
    unsigned int state = CAPTURE_IDLE;
    bool running = atomic_compare_exchange_strong_explicit(
        &capture_state, &state, CAPTURE_RUNNING, memory_order_release,
        memory_order_relaxed);
    pthread_cond_signal(&capture_cond);
    pthread_mutex_unlock(&capture_mutex);
    if (running) {
      ARTS_INFO("Counter capture thread started");
      return;
    }
    // A stop came first and joined nothing; the capture thread reads
    // STOPPED and ends without capturing, and its starter joins it.
    assert(state == CAPTURE_STOPPED);
    ret = pthread_join(capture_thread, NULL);
    if (ret) {
      ARTS_WARN("Failed to join capture thread: %d", ret);
      return;
    }
    ARTS_INFO("Counter capture thread joined by its starter: "
              "a stop came first");
  }
}

void arts_counter_capture_stop() {
  pthread_mutex_lock(&capture_mutex);
  unsigned int state = CAPTURE_RUNNING;
  bool running = atomic_compare_exchange_strong_explicit(
      &capture_state, &state, CAPTURE_STOPPED, memory_order_acquire,
      memory_order_relaxed);
  if (running) {
    pthread_cond_signal(&capture_cond);
  } else if (state == CAPTURE_IDLE) {
    // No capture thread was created, or its start has not decided yet; that
    // start's CAS finds STOPPED, and it joins its own thread.
    (void)atomic_compare_exchange_strong_explicit(
        &capture_state, &state, CAPTURE_STOPPED, memory_order_relaxed,
        memory_order_relaxed);
  }
  pthread_mutex_unlock(&capture_mutex);
  if (!running) {
    return;
  }
  int ret = pthread_join(capture_thread, NULL);
  if (ret) {
    ARTS_WARN("Failed to join capture thread: %d", ret);
  }
}

void arts_counter_increment_by(arts_counter_t *counter, uint64_t num) {
  atomic_fetch_add_explicit(&counter->count, num, memory_order_relaxed);
}

void arts_counter_decrement_by(arts_counter_t *counter, uint64_t num) {
  atomic_fetch_sub_explicit(&counter->count, num, memory_order_relaxed);
}

void arts_counter_timer_start(arts_counter_t *counter) {
  uint64_t stopped = 0;
  if (!atomic_compare_exchange_strong_explicit(
          &counter->start, &stopped, arts_get_time_stamp(),
          memory_order_relaxed, memory_order_relaxed)) {
    ARTS_ERROR("Counter timer already started");
  }
}

void arts_counter_timer_end(arts_counter_t *counter) {
  uint64_t end = arts_get_time_stamp();
  uint64_t start =
      atomic_exchange_explicit(&counter->start, 0, memory_order_relaxed);
  if (!start) {
    ARTS_ERROR("Counter timer not started");
  }
  atomic_fetch_add_explicit(&counter->count, end - start, memory_order_relaxed);
}

// Helper: apply one reduction step
static inline uint64_t
arts_apply_reduction(uint64_t accumulator, uint64_t value,
                     arts_counter_reduce_method_t reduce_method,
                     unsigned int source_index) {
  switch (reduce_method) {
  case ARTS_COUNTER_REDUCE_SUM:
    return accumulator + value;
  case ARTS_COUNTER_REDUCE_MAX:
    return (accumulator < value) ? value : accumulator;
  case ARTS_COUNTER_REDUCE_MIN:
    return (accumulator > value) ? value : accumulator;
  case ARTS_COUNTER_REDUCE_MASTER:
    return (source_index == 0) ? value : accumulator;
  }
  return accumulator;
}

// Helper: convert reduce method to string for JSON output
static inline const char *
arts_reduce_method_to_string(arts_counter_reduce_method_t reduce_method) {
  switch (reduce_method) {
  case ARTS_COUNTER_REDUCE_SUM:
    return "SUM";
  case ARTS_COUNTER_REDUCE_MAX:
    return "MAX";
  case ARTS_COUNTER_REDUCE_MIN:
    return "MIN";
  case ARTS_COUNTER_REDUCE_MASTER:
    return "MASTER";
  }
  return "SUM";
}

// Helper: convert capture mode to string for JSON output
static inline const char *arts_counter_mode_to_string(unsigned int mode) {
  switch (mode) {
  case ARTS_COUNTER_MODE_ONCE:
    return "ONCE";
  case ARTS_COUNTER_MODE_PERIODIC:
    return "PERIODIC";
  default:
    return "OFF";
  }
}

// Helper: check if counter name contains "Time" (case-insensitive)
static bool arts_counter_is_time_counter(const char *name) {
  if (!name) {
    return false;
  }
  const char *p = name;
  while (*p) {
    if ((*p == 'T' || *p == 't') && (*(p + 1) == 'I' || *(p + 1) == 'i') &&
        (*(p + 2) == 'M' || *(p + 2) == 'm') &&
        (*(p + 3) == 'E' || *(p + 3) == 'e')) {
      return true;
    }
    p++;
  }
  return false;
}

// Helper: write capture history array to JSON as compact single line
// Format: [[epoch, value], [epoch, value], ...]
// Epochs are relative (synced across nodes) for proper offline merging
static void arts_write_capture_history(arts_json_writer_t *writer,
                                       uint64_t *epochs, uint64_t *values,
                                       uint64_t count) {
  if (count == 0) {
    return;
  }

  // Build compact JSON string: [[e,v],[e,v],...]
  // Estimate size: each entry is at most ~40 chars, plus brackets
  size_t buf_size = (count * 45) + 10;
  char *buf = (char *)arts_malloc(buf_size);
  char *p = buf;
  *p++ = '[';

  for (uint64_t c = 0; c < count; c++) {
    if (c > 0) {
      *p++ = ',';
    }
    p += sprintf(p, "[%llu,%llu]", (unsigned long long)epochs[c],
                 (unsigned long long)values[c]);
  }
  *p++ = ']';
  *p = '\0';

  arts_json_writer_write_raw_array(writer, "captureHistory", buf);
  arts_free(buf);
}

enum arts_clock_sync arts_counter_clock_sync(void) {
  switch (atomic_load_explicit(&time_sync_state, memory_order_acquire)) {
  case TIME_SYNC_RECEIVED:
  case TIME_SYNC_MASTER:
    return ARTS_CLOCK_SYNC_RTT;
  case TIME_SYNC_TIMED_OUT:
    return ARTS_CLOCK_SYNC_TIMEOUT;
  default:
    return ARTS_CLOCK_SYNC_NONE;
  }
}

static const char *arts_clock_sync_name(enum arts_clock_sync sync) {
  switch (sync) {
  case ARTS_CLOCK_SYNC_RTT:
    return "rtt";
  case ARTS_CLOCK_SYNC_TIMEOUT:
    return "timeout";
  case ARTS_CLOCK_SYNC_NONE:
    break;
  }
  return "none";
}

/* The metadata every counter file carries: the run, this rank and how its end
 * began.  Written by the thread that took the rank's shutdown point, which
 * wrote the stamps and the origin itself. */
static void arts_write_common_metadata(arts_json_writer_t *writer,
                                       const char *folder) {
  arts_json_writer_write_u_int64(writer, "timestamp", (uint64_t)time(NULL));
  arts_json_writer_write_string(writer, "version", "1.7.0");
  arts_json_writer_write_string(writer, "counter_folder", folder);
  enum arts_clock_sync sync = arts_counter_clock_sync();
  uint64_t run_id =
      atomic_load_explicit(&arts_exit_meta.run_id, memory_order_relaxed);
  char run[17] = "";
  if (run_id != 0) {
    (void)snprintf(run, sizeof(run), "%016" PRIx64, run_id);
  }
  arts_json_writer_write_int64(writer, "rc", arts_exit_status());
  arts_json_writer_write_string(writer, "run", run);
  arts_json_writer_write_u_int64(writer, "rank", arts_global_rank_id);
  arts_json_writer_write_string(writer, "origin",
                                arts_exit_origin_name(arts_exit_meta.origin));
  arts_json_writer_write_u_int64(writer, "t0_mono_ns",
                                 arts_exit_meta.t0_mono_ns);
  arts_json_writer_write_u_int64(writer, "t0_realtime_ns",
                                 arts_exit_meta.t0_realtime_ns);
  if (sync == ARTS_CLOCK_SYNC_RTT) {
    uint64_t synced = (uint64_t)((int64_t)arts_exit_meta.t0_mono_ns -
                                 atomic_load_explicit(&time_sync_offset,
                                                      memory_order_relaxed));
    arts_json_writer_write_u_int64(writer, "t0_synced_ns", synced);
  }
  arts_json_writer_write_string(writer, "clock_sync",
                                arts_clock_sync_name(sync));
}

// Helper: check if any counters exist at given level, return count
static unsigned int arts_counters_at_level(unsigned int level) {
  unsigned int count = 0;
  for (unsigned int i = 0; i < NUM_COUNTER_TYPES; i++) {
    if (arts_counter_mode_array[i] != ARTS_COUNTER_MODE_OFF &&
        arts_counter_level_array[i] == level) {
      count++;
    }
  }
  return count;
}

/* One counter file in the making: written to <name>.tmp through a large
 * stdio buffer, then renamed, so a reader never meets a partial file.
 *
 * Only the folder's final component is created: a caller naming a path whose
 * parents do not exist gets a diagnosed failure, not a silently discarded
 * measurement -- counters that vanish are worse than counters that were never
 * asked for, because the run still reports success.  A failure prints one
 * WARN and changes no status. */
struct arts_counter_file_s {
  FILE *fp;
  char path[PATH_MAX];
  char tmp[PATH_MAX];
};

static bool arts_open_counter_file(struct arts_counter_file_s *f,
                                   const char *output_folder,
                                   const char *filename) {
  /* Ranks dumping into one shared folder all attempt the mkdir; EEXIST is
   * another creator's success, and probing with stat first would open a
   * window in which the loser mistakes that success for a failure. */
  if (mkdir(output_folder, 0755) == -1 && errno != EEXIST) {
    ARTS_WARN("counters: cannot create %s: %s — no counters written",
              output_folder, strerror(errno));
    return false;
  }
  int n = snprintf(f->path, sizeof(f->path), "%s/%s", output_folder, filename);
  int m =
      snprintf(f->tmp, sizeof(f->tmp), "%s/%s.tmp", output_folder, filename);
  if (n < 0 || m < 0 || (size_t)m >= sizeof(f->tmp)) {
    ARTS_WARN("counters: the path of %s in %s is too long", filename,
              output_folder);
    return false;
  }
  f->fp = fopen(f->tmp, "w");
  if (!f->fp) {
    ARTS_WARN("counters: cannot write %s: %s", f->tmp, strerror(errno));
    return false;
  }
  (void)setvbuf(f->fp, NULL, _IOFBF, (size_t)1 << 20);
  return true;
}

static void arts_close_counter_file(arts_json_writer_t *writer,
                                    struct arts_counter_file_s *f) {
  arts_json_writer_end_object(writer);
  arts_json_writer_finish(writer);
  (void)fputc('\n', f->fp);
  bool written = !ferror(f->fp);
  if (fclose(f->fp) != 0) {
    written = false;
  }
  if (!written) {
    ARTS_WARN("counters: cannot write %s: %s", f->tmp, strerror(errno));
    (void)unlink(f->tmp);
    return;
  }
  if (rename(f->tmp, f->path) != 0) {
    ARTS_WARN("counters: cannot rename %s to %s: %s", f->tmp, f->path,
              strerror(errno));
    (void)unlink(f->tmp);
  }
}

/* Thread t's count of counter i, read live: every row stays alive, and each
 * count carries only its own value. */
static uint64_t arts_counter_live(unsigned int t, unsigned int i) {
  const arts_counter_t *row = arts_node_info.live_counters[t];
  return row != NULL ? atomic_load_explicit(&row[i].count, memory_order_relaxed)
                     : 0;
}

// Helper function to compute node-level reduced value across all threads
static uint64_t arts_compute_node_reduced_value(unsigned int index) {
  arts_counter_reduce_method_t reduce_method =
      arts_counter_reduce_method_array[index];
  uint64_t node_value =
      (reduce_method == ARTS_COUNTER_REDUCE_MIN) ? UINT64_MAX : 0;
  for (unsigned int t = 0; t < arts_node_info.total_thread_count; t++) {
    node_value = arts_apply_reduction(node_value, arts_counter_live(t, index),
                                      reduce_method, t);
  }
  return node_value;
}

// Helper function to compute node-level reduced captures for PERIODIC mode
// Returns arrays of epochs and values, sets count. Caller must free both.
static void arts_compute_node_reduced_captures(unsigned int counter_index,
                                               uint64_t **out_epochs,
                                               uint64_t **out_values,
                                               uint64_t *out_count) {
  *out_epochs = NULL;
  *out_values = NULL;
  *out_count = 0;

  // Find the max number of captures across all threads
  uint64_t max_captures = 0;
  for (unsigned int t = 0; t < arts_node_info.total_thread_count; t++) {
    arts_array_list_t *thread_list =
        arts_node_info.capture_arrays[t][counter_index];
    if (thread_list && thread_list->index > max_captures) {
      max_captures = thread_list->index;
    }
  }

  if (max_captures == 0) {
    return;
  }

  // Allocate output arrays (upper bound size)
  *out_epochs = (uint64_t *)arts_malloc(max_captures * sizeof(uint64_t));
  *out_values = (uint64_t *)arts_malloc(max_captures * sizeof(uint64_t));

  // Create iterators for all threads (stack-allocated array)
  unsigned int tc = arts_node_info.total_thread_count;
  arts_array_list_iterator_t *iters = (arts_array_list_iterator_t *)arts_calloc(
      tc, sizeof(arts_array_list_iterator_t));
  bool *iter_valid = (bool *)arts_calloc(tc, sizeof(bool));
  arts_counter_capture_t **current_captures =
      (arts_counter_capture_t **)arts_calloc(tc,
                                             sizeof(arts_counter_capture_t *));

  for (unsigned int t = 0; t < tc; t++) {
    arts_array_list_t *thread_list =
        arts_node_info.capture_arrays[t][counter_index];
    if (thread_list && thread_list->index > 0) {
      arts_array_list_iter_init(&iters[t], thread_list);
      iter_valid[t] = true;
      if (arts_array_list_has_next(&iters[t])) {
        current_captures[t] =
            (arts_counter_capture_t *)arts_array_list_next(&iters[t]);
      }
    }
  }

  // Reduce by epoch - process captures in epoch order
  uint64_t captures_written = 0;
  while (captures_written < max_captures) {
    // Find minimum epoch among all current captures
    uint64_t min_epoch = UINT64_MAX;
    for (unsigned int t = 0; t < arts_node_info.total_thread_count; t++) {
      if (current_captures[t] && current_captures[t]->epoch < min_epoch) {
        min_epoch = current_captures[t]->epoch;
      }
    }
    if (min_epoch == UINT64_MAX) {
      break;
    }

    // Reduce all captures at this epoch
    arts_counter_reduce_method_t reduce_method =
        arts_counter_reduce_method_array[counter_index];
    uint64_t reduced_value =
        (reduce_method == ARTS_COUNTER_REDUCE_MIN) ? UINT64_MAX : 0;
    bool has_value = false;
    for (unsigned int t = 0; t < arts_node_info.total_thread_count; t++) {
      if (current_captures[t] && current_captures[t]->epoch == min_epoch) {
        has_value = true;
        reduced_value = arts_apply_reduction(
            reduced_value, current_captures[t]->value, reduce_method, t);
        // Advance this thread's iterator
        if (arts_array_list_has_next(&iters[t])) {
          current_captures[t] =
              (arts_counter_capture_t *)arts_array_list_next(&iters[t]);
        } else {
          current_captures[t] = NULL;
        }
      }
    }

    if (has_value) {
      (*out_epochs)[captures_written] = min_epoch;
      (*out_values)[captures_written] = reduced_value;
      captures_written++;
    }
  }

  *out_count = captures_written;

  arts_free(iters);
  arts_free(iter_valid);
  arts_free(current_captures);
}

static void arts_counter_write_thread(const char *output_folder,
                                      unsigned int node_id,
                                      unsigned int thread_id) {
  char filename[64];
  (void)snprintf(filename, sizeof(filename), "n%u_t%u.json", node_id,
                 thread_id);
  struct arts_counter_file_s file;
  if (!arts_open_counter_file(&file, output_folder, filename)) {
    return;
  }

  arts_json_writer_t writer;
  arts_json_writer_init(&writer, file.fp, 2);
  arts_json_writer_begin_object(&writer, NULL);

  // Metadata
  arts_json_writer_begin_object(&writer, "metadata");
  arts_json_writer_write_u_int64(&writer, "node_id", node_id);
  arts_json_writer_write_u_int64(&writer, "thread_id", thread_id);
  arts_write_common_metadata(&writer, output_folder);
  arts_json_writer_write_u_int64(&writer, "counter_capture_interval",
                                 arts_node_info.counter_capture_interval);
  arts_json_writer_end_object(&writer);

  // Counters
  arts_json_writer_begin_object(&writer, "counters");
  for (unsigned int i = 0; i < NUM_COUNTER_TYPES; i++) {
    if (arts_counter_mode_array[i] == ARTS_COUNTER_MODE_OFF ||
        arts_counter_level_array[i] != ARTS_COUNTER_LEVEL_THREAD) {
      continue;
    }

    arts_json_writer_begin_object(&writer, arts_counter_names[i]);
    arts_json_writer_write_string(
        &writer, "captureMode",
        arts_counter_mode_to_string(arts_counter_mode_array[i]));
    arts_json_writer_write_string(&writer, "captureLevel", "THREAD");

    // Always write final value
    uint64_t final_value = arts_counter_live(thread_id, i);
    arts_json_writer_write_u_int64(&writer, "value", final_value);
    if (arts_counter_is_time_counter(arts_counter_names[i])) {
      arts_json_writer_write_double(&writer, "value_ms",
                                    (double)final_value / 1000000.0);
    }

    if (arts_counter_mode_array[i] == ARTS_COUNTER_MODE_PERIODIC) {
      arts_array_list_t *capture_list =
          arts_node_info.capture_arrays[thread_id][i];
      if (capture_list && capture_list->index > 0) {
        uint64_t count = capture_list->index;
        uint64_t *epochs = (uint64_t *)arts_malloc(count * sizeof(uint64_t));
        uint64_t *values = (uint64_t *)arts_malloc(count * sizeof(uint64_t));
        arts_array_list_iterator_t iter;
        arts_array_list_iter_init(&iter, capture_list);
        for (uint64_t idx = 0; arts_array_list_has_next(&iter) && idx < count;
             idx++) {
          arts_counter_capture_t *cap =
              (arts_counter_capture_t *)arts_array_list_next(&iter);
          if (cap) {
            epochs[idx] = cap->epoch;
            values[idx] = cap->value;
          }
        }
        arts_write_capture_history(&writer, epochs, values, count);
        arts_free(epochs);
        arts_free(values);
      }
    }
    arts_json_writer_end_object(&writer);
  }
  arts_json_writer_end_object(&writer);

  arts_close_counter_file(&writer, &file);
}

/* The node file.  Before the node was ready no counter row exists, so it
 * carries the metadata and no counter value. */
static void arts_counter_write_node(const char *output_folder,
                                    unsigned int node_id, bool ready) {
  char filename[64];
  (void)snprintf(filename, sizeof(filename), "n%u.json", node_id);
  struct arts_counter_file_s file;
  if (!arts_open_counter_file(&file, output_folder, filename)) {
    return;
  }

  arts_json_writer_t writer;
  arts_json_writer_init(&writer, file.fp, 2);
  arts_json_writer_begin_object(&writer, NULL);

  // Metadata
  arts_json_writer_begin_object(&writer, "metadata");
  arts_json_writer_write_u_int64(&writer, "node_id", node_id);
  arts_write_common_metadata(&writer, output_folder);
  arts_json_writer_write_u_int64(&writer, "captureInterval",
                                 ready ? arts_node_info.counter_capture_interval
                                       : 0);
  arts_json_writer_write_u_int64(&writer, "total_threads",
                                 ready ? arts_node_info.total_thread_count : 0);
  arts_json_writer_end_object(&writer);

  // Counters
  arts_json_writer_begin_object(&writer, "counters");
  for (unsigned int i = 0; ready && i < NUM_COUNTER_TYPES; i++) {
    if (arts_counter_mode_array[i] == ARTS_COUNTER_MODE_OFF ||
        arts_counter_level_array[i] != ARTS_COUNTER_LEVEL_NODE) {
      continue;
    }

    arts_json_writer_begin_object(&writer, arts_counter_names[i]);
    arts_json_writer_write_string(
        &writer, "captureMode",
        arts_counter_mode_to_string(arts_counter_mode_array[i]));
    arts_json_writer_write_string(&writer, "captureLevel", "NODE");
    arts_json_writer_write_string(
        &writer, "reduce_method",
        arts_reduce_method_to_string(arts_counter_reduce_method_array[i]));

    uint64_t final_value = arts_compute_node_reduced_value(i);
    arts_json_writer_write_u_int64(&writer, "value", final_value);
    if (arts_counter_is_time_counter(arts_counter_names[i])) {
      arts_json_writer_write_double(&writer, "value_ms",
                                    (double)final_value / 1000000.0);
    }

    if (arts_counter_mode_array[i] == ARTS_COUNTER_MODE_PERIODIC) {
      uint64_t *epochs;
      uint64_t *values;
      uint64_t count;
      arts_compute_node_reduced_captures(i, &epochs, &values, &count);
      arts_write_capture_history(&writer, epochs, values, count);
      if (epochs) {
        arts_free(epochs);
      }
      if (values) {
        arts_free(values);
      }
    }
    arts_json_writer_end_object(&writer);
  }

  arts_json_writer_end_object(&writer);

  arts_close_counter_file(&writer, &file);
}

void arts_counter_record(void) {
  const char *folder = arts_exit_counter_folder();
  if (folder[0] == '\0') {
    return;
  }
  bool ready = arts_exit_node_ready();
  if (arts_counters_at_level(ARTS_COUNTER_LEVEL_THREAD) && ready) {
    for (unsigned int t = 0; t < arts_node_info.total_thread_count; t++) {
      arts_counter_write_thread(folder, arts_global_rank_id, t);
    }
  }
  if (arts_counters_at_level(ARTS_COUNTER_LEVEL_NODE)) {
    arts_counter_write_node(folder, arts_global_rank_id, ready);
  }
}

uint64_t arts_counter_record_estimate(void) {
  bool ready = arts_exit_node_ready();
  uint64_t tc = ready ? arts_node_info.total_thread_count : 0;
  uint64_t bytes = 4096;
  for (unsigned int i = 0; i < NUM_COUNTER_TYPES; i++) {
    if (arts_counter_mode_array[i] == ARTS_COUNTER_MODE_OFF) {
      continue;
    }
    bool thread_level =
        arts_counter_level_array[i] == ARTS_COUNTER_LEVEL_THREAD;
    bytes += 160;
    if (thread_level) {
      bytes += 160 * tc;
    }
    if (arts_counter_mode_array[i] != ARTS_COUNTER_MODE_PERIODIC) {
      continue;
    }
    /* A node file reduces the threads' histories by epoch into one, as
     * long as the longest; a thread file writes each thread's own. */
    uint64_t entries = 0;
    for (uint64_t t = 0; t < tc; t++) {
      const arts_array_list_t *list = arts_node_info.capture_arrays[t][i];
      uint64_t n = list != NULL ? list->index : 0;
      entries = thread_level ? entries + n : (n > entries ? n : entries);
    }
    bytes += 40 * entries;
  }
  return bytes;
}

// RTT-based time synchronization for counter capture alignment

// Worker sends sync request to master with its current timestamp (T1)
void arts_send_time_sync_request(void) {
  struct arts_msg_time_sync_req_packet_s packet;
  packet.worker_send_time = arts_get_time_stamp(); // T1
  arts_fill_packet_header(&packet.header, sizeof(packet),
                          MSG_TIME_SYNC_REQUEST);

  // Send to master
  arts_transport_send_async((int)arts_global_master_rank_id, (char *)&packet,
                            sizeof(packet));
  ARTS_INFO("Time sync: Worker %u sent request to master %u at T1=%lu",
            arts_global_rank_id, arts_global_master_rank_id,
            packet.worker_send_time);
}

// Master handles sync request: records T2 and sends response with T1, T2
void arts_handler_time_sync_request(void *pack) {
  struct arts_msg_time_sync_req_packet_s *req =
      (struct arts_msg_time_sync_req_packet_s *)pack;
  uint64_t master_recv_time = arts_get_time_stamp(); // T2

  struct arts_msg_time_sync_resp_packet_s resp;
  resp.worker_send_time = req->worker_send_time; // Echo T1
  resp.master_recv_time = master_recv_time;      // T2
  arts_fill_packet_header(&resp.header, sizeof(resp), MSG_TIME_SYNC_RESPONSE);

  // Send response back to the requesting worker
  arts_transport_send_async((int)req->header.rank, (char *)&resp, sizeof(resp));
  ARTS_INFO("Time sync: Master received request from rank %u, T1=%lu, T2=%lu",
            req->header.rank, req->worker_send_time, master_recv_time);
}

// Worker handles sync response: calculates offset using RTT
void arts_handler_time_sync_response(void *pack) {
  struct arts_msg_time_sync_resp_packet_s *resp =
      (struct arts_msg_time_sync_resp_packet_s *)pack;
  uint64_t worker_recv_time = arts_get_time_stamp(); // T3

  uint64_t ntp_t1 = resp->worker_send_time;
  uint64_t ntp_t2 = resp->master_recv_time;
  uint64_t ntp_t3 = worker_recv_time;

  // RTT = T3 - T1 (round-trip time in worker's clock)
  // One-way delay estimate = RTT / 2 (assuming symmetric network)
  // At T2 (master clock), worker clock was approximately T1 + RTT/2
  // offset = workerTime - masterTime = (T1 + RTT/2) - T2 = (T1 + T3)/2 - T2
  int64_t offset = (int64_t)((ntp_t1 + ntp_t3) / 2) - (int64_t)ntp_t2;

  unsigned int waiting = TIME_SYNC_WAITING;
  if (!atomic_compare_exchange_strong_explicit(
          &time_sync_state, &waiting, TIME_SYNC_RECEIVING, memory_order_relaxed,
          memory_order_relaxed)) {
    ARTS_INFO("Time sync: Worker %u ignores a response that arrived after "
              "the wait ended",
              arts_global_rank_id);
    return;
  }
  atomic_store_explicit(&time_sync_offset, offset, memory_order_relaxed);
  atomic_store_explicit(&time_sync_state, TIME_SYNC_RECEIVED,
                        memory_order_release);

  uint64_t rtt = ntp_t3 - ntp_t1;
  ARTS_INFO("Time sync: Worker %u received response, T1=%lu, T2=%lu, T3=%lu, "
            "RTT=%lu ns (%.3f ms), offset=%ld ns (%.3f ms)",
            arts_global_rank_id, ntp_t1, ntp_t2, ntp_t3, rtt,
            (double)rtt / 1000000.0, offset, (double)offset / 1000000.0);
}
