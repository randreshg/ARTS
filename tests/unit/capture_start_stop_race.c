/* SPDX-License-Identifier: Apache-2.0
 *
 * A stop that comes first against a starting capture.
 *
 * The capture start creates its thread, stores the handle and only then moves
 * the capture word.  The injection hook sits in that window: it sends the
 * process a SIGTERM and returns only once the stop the signal causes has moved
 * the word to STOPPED.  The start's own transition then finds STOPPED, and the
 * start joins the thread it created and says so.  The thread never reads
 * RUNNING, the only state it captures in, so it never captures.
 *
 * What the test checks: the starter's join line (the pass regex; a start that
 * skips the join, or whose join fails, does not print it), no skipped
 * teardown (the end's owner waits for thread 0, the starter, to park after the
 * start returns, so a start that hangs, or a thread that is not woken, shows
 * as a skipped teardown), and the signal's status, 143.  The capture interval
 * is a day, so a thread that would capture before reading the word waits out
 * the interval first, and its starter's join hangs and shows the same way.
 */
#include "arts.h"

#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "arts/counter/counter.h"

enum { CAPTURE_STOPPED_STATE = 2 };

static void stop_first_hook(void) {
  kill(getpid(), SIGTERM);
  struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
  while (arts_counter_capture_state() != CAPTURE_STOPPED_STATE) {
    nanosleep(&pause, NULL);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
}

int main(int argc, char **argv) {
  arts_capture_started_hook = stop_first_hook;
  return arts_rt(argc, argv);
}
