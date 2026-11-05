/* SPDX-License-Identifier: Apache-2.0
 *
 * exit_origin_posts — the shutdown origins (arts/system/shutdown.h): an end
 * that begins on this rank (the program, a terminating signal, a fault, a
 * fatal runtime error) posts SHUTDOWN to the peers, and an end this rank
 * learned of (a peer's SHUTDOWN, a peer's closed connection, either seen in a
 * send's spin) posts nothing; each origin has its name, and every origin fits
 * in three bits.  Header-only: no runtime.
 */
#include <stdio.h>
#include <string.h>

#include "arts/system/shutdown.h"

static int g_failures;

static void check(enum arts_exit_origin o, bool originates, const char *name) {
  if (arts_exit_origin_originates(o) != originates) {
    printf("FAIL exit_origin_posts: %s %s\n", name,
           originates ? "does not originate" : "originates");
    g_failures++;
  }
  if (strcmp(arts_exit_origin_name(o), name) != 0) {
    printf("FAIL exit_origin_posts: origin %d is named %s, want %s\n", (int)o,
           arts_exit_origin_name(o), name);
    g_failures++;
  }
  if ((int)o <= 0 || (int)o > 7) {
    printf("FAIL exit_origin_posts: %s is %d, outside 1..7\n", name, (int)o);
    g_failures++;
  }
}

int main(void) {
  check(ARTS_EXIT_APP, true, "APP");
  check(ARTS_EXIT_SIGNAL, true, "SIGNAL");
  check(ARTS_EXIT_FAULT, true, "FAULT");
  check(ARTS_EXIT_RUNTIME_ERROR, true, "RUNTIME_ERROR");
  check(ARTS_EXIT_WIRE, false, "WIRE");
  check(ARTS_EXIT_SENTINEL, false, "SENTINEL");
  check(ARTS_EXIT_BACKPRESSURE, false, "BACKPRESSURE");
  enum arts_exit_origin none = (enum arts_exit_origin)0;
  if (arts_exit_origin_originates(none) ||
      strcmp(arts_exit_origin_name(none), "NONE") != 0) {
    printf("FAIL exit_origin_posts: no origin reads as %s, originating %d\n",
           arts_exit_origin_name(none), (int)arts_exit_origin_originates(none));
    g_failures++;
  }
  if (g_failures != 0) {
    printf("FAIL exit_origin_posts: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("PASS exit_origin_posts\n");
  return 0;
}
