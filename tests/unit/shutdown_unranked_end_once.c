/* SPDX-License-Identifier: Apache-2.0
 *
 * shutdown_unranked_end_once — an end that began before the process became a
 * rank is the process's one end: an arts_exit made after the rank was
 * published is a late entry, which leaves that end and its status alone.
 *
 * Thread A ends the process before it is a rank (arts_exit(143, SIGNAL)) and
 * blocks in that end's flush of a stream whose pipe is full.  The main thread
 * then publishes the rank and calls arts_exit(75, RUNTIME_ERROR).  An
 * observer reads the end's rc 200 ms later (143: the second call ran no end
 * of its own; 75: it took the point and ran a rank end), prints it, and
 * drains the pipe, so A's end finishes with its own status, 143.
 *
 * With argument "late", A's end is arts_exit(1, RUNTIME_ERROR) and the main
 * thread's call, made before any rank is published, is
 * arts_exit(143, SIGNAL): a late entry, whose rc the end's status takes as
 * a rank end's would, so the process ends 143 whatever the timing.
 *
 * Links the runtime library and starts no runtime.  Nothing here touches
 * stdio once A's flush holds the stream list.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "arts/system/shutdown.h"

static int g_pipe[2];
/* Release store by the main thread right before its arts_exit, acquire
 * loads by the observer. */
static _Atomic(bool) g_second_end;

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

static void say(const char *line) {
  (void)write(STDERR_FILENO, line, strlen(line));
}

static bool g_late;

static void *first_end(void *arg) {
  (void)arg;
  if (g_late) {
    arts_exit(1, ARTS_EXIT_RUNTIME_ERROR);
  }
  arts_exit(143, ARTS_EXIT_SIGNAL);
}

static void *observer(void *arg) {
  (void)arg;
  while (!atomic_load_explicit(&g_second_end, memory_order_acquire)) {
    sleep_ms(1);
  }
  sleep_ms(200);
  const int rc = atomic_load_explicit(&arts_exit_meta.rc, memory_order_relaxed);
  char line[64];
  int n = snprintf(line, sizeof(line), "UNRANKED_END rc=%d\n", rc);
  (void)write(STDERR_FILENO, line, (size_t)n);
  char buf[4096];
  for (;;) {
    if (read(g_pipe[0], buf, sizeof(buf)) <= 0) {
      sleep_ms(1);
    }
  }
  return NULL;
}

int main(int argc, char **argv) {
  g_late = argc > 1 && strcmp(argv[1], "late") == 0;
  arts_exit_process_init();
  /* A stream with buffered bytes whose pipe is full: the end's flush of it
   * blocks until the pipe is drained. */
  if (pipe(g_pipe) != 0) {
    say("FAIL shutdown_unranked_end_once: pipe\n");
    return 1;
  }
  (void)fcntl(g_pipe[1], F_SETFL, O_NONBLOCK);
  char fill[4096];
  memset(fill, 'f', sizeof(fill));
  while (write(g_pipe[1], fill, sizeof(fill)) > 0) {
  }
  (void)fcntl(g_pipe[1], F_SETFL, 0);
  (void)fcntl(g_pipe[0], F_SETFL, O_NONBLOCK);
  FILE *held = fdopen(g_pipe[1], "w");
  if (held == NULL || fputs("x", held) == EOF) {
    say("FAIL shutdown_unranked_end_once: fdopen\n");
    return 1;
  }
  pthread_t a;
  pthread_t o;
  if (pthread_create(&o, NULL, observer, NULL) != 0 ||
      pthread_create(&a, NULL, first_end, NULL) != 0) {
    say("FAIL shutdown_unranked_end_once: pthread_create\n");
    return 1;
  }
  while (!arts_exit_began()) {
    sleep_ms(1);
  }
  sleep_ms(50);
  if (g_late) {
    atomic_store_explicit(&g_second_end, true, memory_order_release);
    arts_exit(143, ARTS_EXIT_SIGNAL);
  }
  arts_exit_publish_rank(".");
  atomic_store_explicit(&g_second_end, true, memory_order_release);
  arts_exit(75, ARTS_EXIT_RUNTIME_ERROR);
}
