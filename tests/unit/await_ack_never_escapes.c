/* SPDX-License-Identifier: Apache-2.0
 *
 * await_ack_never_escapes — a release's wait for its answer returns only once
 * the answer has posted its semaphore, whatever the rank's end does.
 *
 * The main thread takes the rank's shutdown point (steps 1-2 of the end, with
 * nothing else of it run), then a thread blocks in arts_db_await_ack on a
 * semaphore nobody has posted.  The wait must still be blocked well after
 * the point was taken, and must return promptly once the semaphore is posted.
 *
 * Links the runtime library and starts no runtime.
 */
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "arts/coherence/coherence.h"
#include "arts/system/shutdown.h"

static sem_t g_answer;
/* Release store after the wait returned, acquire loads by the main thread. */
static _Atomic(bool) g_returned;

static _Noreturn void finish(int status, const char *line) {
  (void)puts(line);
  (void)fflush(stdout);
  _exit(status);
}

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

static void *waiter(void *arg) {
  (void)arg;
  arts_db_await_ack(&g_answer);
  atomic_store_explicit(&g_returned, true, memory_order_release);
  return NULL;
}

int main(void) {
  arts_exit_process_init();
  if (!arts_exit_take(0, ARTS_EXIT_WIRE)) {
    finish(1, "FAIL await_ack_never_escapes: the shutdown point could not be "
              "taken");
  }
  if (sem_init(&g_answer, 0, 0) != 0) {
    finish(1, "FAIL await_ack_never_escapes: sem_init");
  }
  atomic_init(&g_returned, false);
  pthread_t t;
  if (pthread_create(&t, NULL, waiter, NULL) != 0) {
    finish(1, "FAIL await_ack_never_escapes: pthread_create");
  }
  sleep_ms(2500);
  if (atomic_load_explicit(&g_returned, memory_order_acquire)) {
    finish(1, "FAIL await_ack_never_escapes: the wait returned with no "
              "answer posted");
  }
  if (sem_post(&g_answer) != 0) {
    finish(1, "FAIL await_ack_never_escapes: sem_post");
  }
  for (int i = 0; i < 100; i++) {
    if (atomic_load_explicit(&g_returned, memory_order_acquire)) {
      (void)pthread_join(t, NULL);
      finish(0, "PASS await_ack_never_escapes");
    }
    sleep_ms(10);
  }
  finish(1, "FAIL await_ack_never_escapes: the posted wait did not return "
            "within 1 s");
}
