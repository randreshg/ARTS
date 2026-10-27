/* SPDX-License-Identifier: Apache-2.0
 *
 * regpool_entry_word — the registered pool's table entry word
 * (arts/memory/regpool_entry.h): a lock-free reader takes an entry's state
 * and range in this one word, so every range the pool publishes must survive
 * the packing exactly, and a range the word cannot name must be refused
 * before anything is published.
 *
 *   - pack and unpack at the limits: bases up to the last 2 MiB granule
 *     below 2^57, lengths up to the last granule below 2^47, every state; a
 *     state change keeps the range;
 *   - the lookup's single unsigned comparison (p - base < len) holds the
 *     range's two ends and nothing outside it;
 *   - each refusal (a base or length off the 2 MiB granule, a base at 2^57, a
 *     length at 2^47) is the pool's fatal error, each in a child of its own.
 *
 * Whitebox unit test: header-only, no ARTS runtime, no pool. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "arts/memory/regpool_entry.h"
#include "arts/runtime_state.h"
#include "arts/system/identity.h"

/* What the fatal path references. */
unsigned int arts_global_rank_id = 0;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;
#define FATAL_STATUS 3
void arts_abort(uint8_t code) {
  (void)code;
  _exit(FATAL_STATUS);
}

#define MIB ((uint64_t)1 << 20)
#define GRANULE ((uint64_t)2 * MIB)
#define BASE_LIMIT ((uint64_t)1 << 57)
#define LEN_LIMIT ((uint64_t)1 << 47)

static int g_fails;

static void check(bool ok, const char *what, uint64_t a, uint64_t b) {
  if (!ok) {
    printf("FAIL regpool_entry_word: %s (0x%llx, 0x%llx)\n", what,
           (unsigned long long)a, (unsigned long long)b);
    g_fails++;
  }
}

static bool covers(uint64_t w, uint64_t p) {
  return (uintptr_t)p - arts_regpool_entry_base(w) < arts_regpool_entry_len(w);
}

static void round_trips(void) {
  const uint64_t bases[] = {0,
                            GRANULE,
                            (uint64_t)0x7f0000000000ull,
                            (uint64_t)1 << 47,
                            (uint64_t)1 << 56,
                            BASE_LIMIT - GRANULE};
  const uint64_t lens[] = {GRANULE, 32 * MIB, (uint64_t)16 << 30,
                           (uint64_t)1 << 46, LEN_LIMIT - GRANULE};
  for (size_t i = 0; i < sizeof bases / sizeof bases[0]; i++) {
    for (size_t j = 0; j < sizeof lens / sizeof lens[0]; j++) {
      for (unsigned st = 0; st < 3; st++) {
        uint64_t w = arts_regpool_entry_word(st, (const void *)bases[i],
                                             (size_t)lens[j]);
        check(arts_regpool_entry_state(w) == st, "state", w, st);
        check(arts_regpool_entry_base(w) == bases[i], "base", w, bases[i]);
        check(arts_regpool_entry_len(w) == lens[j], "len", w, lens[j]);
        for (unsigned to = 0; to < 3; to++) {
          uint64_t v = arts_regpool_entry_with_state(w, to);
          check(arts_regpool_entry_state(v) == to &&
                    arts_regpool_entry_base(v) == bases[i] &&
                    arts_regpool_entry_len(v) == lens[j],
                "state change keeps the range", w, v);
        }
        if (bases[i] + lens[j] <= BASE_LIMIT) {
          check(covers(w, bases[i]), "covers its first byte", w, bases[i]);
          check(covers(w, bases[i] + lens[j] - 1), "covers its last byte", w,
                bases[i] + lens[j] - 1);
          check(!covers(w, bases[i] + lens[j]), "stops at its end", w,
                bases[i] + lens[j]);
          if (bases[i] != 0) {
            check(!covers(w, bases[i] - 1), "starts at its base", w,
                  bases[i] - 1);
          }
        }
      }
    }
  }
  check(arts_regpool_entry_state(0) == 0 && arts_regpool_entry_len(0) == 0,
        "the zero word is a free entry naming nothing", 0, 0);
}

struct refusal {
  const char *what;
  uint64_t base;
  uint64_t len;
};

static const struct refusal g_refusals[] = {
    {"base off the granule", (uint64_t)0x7f0000000000ull + 4096, 32 * MIB},
    {"length off the granule", (uint64_t)0x7f0000000000ull, 32 * MIB + 64},
    {"base at 2^57", BASE_LIMIT, 32 * MIB},
    {"length at 2^47", (uint64_t)0x7f0000000000ull, LEN_LIMIT},
};

/* True when the word for the refusal's range ends its child in the pool's
 * fatal error. */
static bool refused(const struct refusal *r) {
  int fds[2];
  if (pipe(fds) != 0) {
    printf("FAIL regpool_entry_word: pipe: %s\n", strerror(errno));
    return false;
  }
  fflush(NULL);
  pid_t pid = fork();
  if (pid < 0) {
    printf("FAIL regpool_entry_word: fork: %s\n", strerror(errno));
    return false;
  }
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    uint64_t w =
        arts_regpool_entry_word(2u, (const void *)r->base, (size_t)r->len);
    printf("published 0x%llx\n", (unsigned long long)w);
    _exit(0);
  }
  close(fds[1]);
  char text[4096];
  size_t len = 0;
  for (;;) {
    ssize_t n = read(fds[0], text + len, sizeof text - 1 - len);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    len += (size_t)n;
    if (len == sizeof text - 1)
      break;
  }
  text[len] = '\0';
  close(fds[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  bool ok = WIFEXITED(status) && WEXITSTATUS(status) == FATAL_STATUS &&
            strstr(text, "cannot be named by one table word") != NULL;
  if (!ok) {
    printf("FAIL regpool_entry_word: %s was not refused (status 0x%x); the "
           "child wrote:\n%s",
           r->what, (unsigned)status, text);
  }
  return ok;
}

int main(void) {
  round_trips();
  for (size_t i = 0; i < sizeof g_refusals / sizeof g_refusals[0]; i++) {
    if (!refused(&g_refusals[i])) {
      g_fails++;
    }
  }
  if (g_fails != 0) {
    printf("FAIL regpool_entry_word: %d check(s) failed\n", g_fails);
    return 1;
  }
  printf("PASS regpool_entry_word\n");
  return 0;
}
