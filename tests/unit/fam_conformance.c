/// @file fam_conformance.c
/// @brief The five questions of the conformance probe, over fam/pool.h alone.
///
/// Same source on both backends.  On the device machine, with no build tree,
/// one cc line (continued here only for width):
///   cc -std=gnu11 -O1 -DARTS_FAM=1 -DARTS_FAM_BACKEND_DEVICE=1
///      -DARTS_FAM_STAGED=1 -I<device headers> -I<repo>/libs/include/internal
///      -I<repo>/libs/include/public -I<generated include dir>
///      tests/unit/fam_conformance.c tests/unit/fam_stubs.c
///      libs/src/core/fam/pool.c libs/src/core/fam/library.c
///      -o fam_conformance -lpthread <device library>
/// then start one process per rank with --rank i --nranks n --rendezvous PATH.
/// Rank 0 must be the first process to load the library.

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "arts/fam/pool.h"
#include "arts/system/config.h"
#include "arts/system/identity.h"

#define BLOCK_BYTES 256u
#define PAT_A 0x11
#define PAT_B 0x22
#define PAT_C 0x33
#define RV_TIMEOUT_S 30

static int g_fails;
static unsigned g_rank, g_nranks = 2u, g_pool_mb = 8u;
static char g_rv[512] = "fam_conformance_rv";

static void say(const char *fmt, ...) {
  va_list ap;
  char line[512];
  va_start(ap, fmt);
  (void)vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  printf("fam_conformance r%u: %s\n", g_rank, line);
  (void)fflush(stdout);
}

static void fail(const char *what) {
  say("FAIL %s", what);
  g_fails++;
}

static void rv_write(const char *suffix, const char *text) {
  char path[640];
  char tmp[700];
  (void)snprintf(path, sizeof(path), "%s.%s", g_rv, suffix);
  (void)snprintf(tmp, sizeof(tmp), "%s.tmp%u", path, g_rank);
  FILE *f = fopen(tmp, "w");
  if (!f) {
    fail("cannot write the rendezvous");
    return;
  }
  (void)fputs(text, f);
  (void)fclose(f);
  (void)rename(tmp, path);
}

static int rv_read(const char *suffix, char *out, size_t cap) {
  char path[640];
  (void)snprintf(path, sizeof(path), "%s.%s", g_rv, suffix);
  for (int s = 0; s < RV_TIMEOUT_S * 100; s++) {
    FILE *f = fopen(path, "r");
    if (f) {
      out[0] = '\0';
      (void)fgets(out, (int)cap, f);
      (void)fclose(f);
      return 1;
    }
    struct timespec ts = {0, 10 * 1000 * 1000};
    (void)nanosleep(&ts, NULL);
  }
  return 0;
}

/* Every rank marks the phase and waits for all the others' marks.  Returns 0
 * on a timeout, which is terminal for the caller: a peer that cannot be
 * heard from once will not be heard from at the next phase either, so
 * waiting on it again only stacks timeouts past the process's own budget. */
static int rv_barrier(unsigned phase) {
  char suffix[64];
  (void)snprintf(suffix, sizeof(suffix), "p%u.r%u", phase, g_rank);
  rv_write(suffix, "x");
  for (unsigned r = 0; r < g_nranks; r++) {
    char other[64], buf[64];
    (void)snprintf(other, sizeof(other), "p%u.r%u", phase, r);
    if (!rv_read(other, buf, sizeof(buf))) {
      fail("the rendezvous timed out");
      return 0;
    }
  }
  return 1;
}

static void nt_store_pattern(void *p, unsigned char v) {
  /* movnti, written out: this tree's compiler has no nontemporal builtin. */
  uint64_t word = 0x0101010101010101ULL * (uint64_t)v;
  volatile uint64_t *q = (volatile uint64_t *)p;
  for (unsigned i = 0; i < BLOCK_BYTES / 8u; i++) {
    __asm__ __volatile__("movnti %1, %0" : "=m"(q[i]) : "r"(word));
  }
  __asm__ __volatile__("sfence" ::: "memory");
}

static int all_bytes_are(const void *p, unsigned char v) {
  const unsigned char *b = (const unsigned char *)p;
  for (unsigned i = 0; i < BLOCK_BYTES; i++) {
    if (b[i] != v) {
      return 0;
    }
  }
  return 1;
}

static void boot(unsigned mb, unsigned nranks) {
  struct arts_config_s config;
  memset(&config, 0, sizeof(config));
  config.fam_pool_mb = mb;
  config.table_length = nranks;
  config.master_rank = 0;
  config.launcher = (char *)"local";
  arts_fam_boot_prepare(&config);
}

/* Question 4 runs each candidate in a child: a refusal aborts, and the child
 * must start from nothing, so it releases the pool it forked with before
 * taking a fresh arena. */
static uint64_t arena_limit_mb(void) {
  uint64_t best = 0;
  for (unsigned mb = 1; mb <= (1u << 14); mb <<= 1) {
    int fds[2];
    if (pipe(fds) != 0) {
      break;
    }
    pid_t pid = fork();
    if (pid == 0) {
      dup2(fds[1], STDOUT_FILENO);
      dup2(fds[1], STDERR_FILENO);
      close(fds[0]);
      close(fds[1]);
      arts_fam_fini();
      boot(mb, 1u);
      arts_fam_init(0u, 1u);
      void *p = arts_fam_alloc(BLOCK_BYTES);
      printf("ok %u\n", (unsigned)((uintptr_t)p % ARTS_FAM_PAGE));
      /* STDOUT is a pipe, so stdio is fully buffered and _exit does not
       * flush.  Without this the parent reads nothing, reads the candidate as
       * "refused" on the FIRST iteration, and question 4 reports 0 MB
       * whatever the arena could do. */
      (void)fflush(stdout);
      _exit(0);
    }
    close(fds[1]);
    char buf[256] = {0};
    (void)read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
        strncmp(buf, "ok ", 3) == 0) {
      best = mb;
      say("arena %u MB: taken, first block offset in its page %s", mb, buf + 3);
    } else {
      say("arena %u MB: refused", mb);
      break;
    }
  }
  return best;
}

int main(int argc, char **argv) {
  bool spawner = true;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--rank") && i + 1 < argc) {
      g_rank = (unsigned)atoi(argv[++i]);
      spawner = false;
    } else if (!strcmp(argv[i], "--nranks") && i + 1 < argc) {
      g_nranks = (unsigned)atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--rendezvous") && i + 1 < argc) {
      (void)snprintf(g_rv, sizeof(g_rv), "%s", argv[++i]);
    } else if (!strcmp(argv[i], "--pool-mb") && i + 1 < argc) {
      g_pool_mb = (unsigned)atoi(argv[++i]);
    }
  }
  if (g_nranks < 2u || g_nranks > 16u) {
    printf("FAIL fam_conformance: --nranks must be 2..16\n");
    return 1;
  }

  boot(g_pool_mb, g_nranks);

  pid_t kids[16];
  unsigned nkids = 0;
  if (spawner) {
    /* One rendezvous per run of the probe. */
    char uniq[sizeof(g_rv) + 32];
    (void)snprintf(uniq, sizeof(uniq), "%s_%ld", g_rv, (long)getpid());
    (void)snprintf(g_rv, sizeof(g_rv), "%.*s", (int)sizeof(g_rv) - 1, uniq);
    for (unsigned r = 1; r < g_nranks; r++) {
      pid_t pid = fork();
      if (pid == 0) {
        char rs[16], ns[16], ms[16];
        (void)snprintf(rs, sizeof(rs), "%u", r);
        (void)snprintf(ns, sizeof(ns), "%u", g_nranks);
        (void)snprintf(ms, sizeof(ms), "%u", g_pool_mb);
        /* The rank identity the library reads when it loads, as the launcher
         * sets it: a process without it would claim the region as rank 0. */
        setenv("ARTS_RANK", rs, 1);
        execl(argv[0], argv[0], "--rank", rs, "--nranks", ns, "--rendezvous",
              g_rv, "--pool-mb", ms, (char *)NULL);
        _exit(127);
      }
      kids[nkids++] = pid;
    }
  }

  /* The runtime's address exchange, by rendezvous: rank 0 takes the arena
   * and names it here, and every other rank records what it named. */
  arts_global_rank_id = g_rank;
  arts_global_rank_count = g_nranks;
  if (g_rank == 0) {
    uint64_t base = 0, size = 0;
    arts_fam_device_publish(&base, &size);
    char text[64];
    (void)snprintf(text, sizeof(text), "%llu %llu", (unsigned long long)base,
                   (unsigned long long)size);
    rv_write("arena", text);
  } else {
    char text[64];
    unsigned long long base = 0, size = 0;
    if (!rv_read("arena", text, sizeof(text)) ||
        sscanf(text, "%llu %llu", &base, &size) != 2) {
      fail("rank 0 never named its arena");
      return 1;
    }
    arts_fam_device_record(0u, (uint64_t)base, (uint64_t)size);
  }

  arts_fam_init(g_rank, g_nranks);

  /* Q1 + Q2: one address, and bytes that cross after both flushes. */
  void *block = NULL;
  if (g_rank == 0) {
    block = arts_fam_alloc(BLOCK_BYTES);
    memset(block, PAT_A, BLOCK_BYTES);
    arts_fam_flush_producer(block, BLOCK_BYTES);
    char text[64];
    (void)snprintf(text, sizeof(text), "%llu",
                   (unsigned long long)(uintptr_t)block);
    rv_write("block", text);
  } else {
    char text[64];
    if (!rv_read("block", text, sizeof(text))) {
      fail("rank 0 never published a block");
      return 1;
    }
    block = (void *)(uintptr_t)strtoull(text, NULL, 10);
    if (!arts_fam_contains(block)) {
      fail("q1 same-address: rank 0's block is not in this rank's pool");
    }
    arts_fam_flush_consumer(block, BLOCK_BYTES);
    if (!all_bytes_are(block, PAT_A)) {
      fail("q2 visible-after-flush: the pattern did not cross");
    } else {
      say("q1 same-address = ok; q2 visible-after-flush = ok");
    }
  }
  /* A timed-out barrier is terminal from here on: the remaining phases are
   * skipped, and every rank falls through to its own cleanup and a non-zero
   * exit rather than waiting on a peer it has already given up on again. */
  bool alive = rv_barrier(1);

  /* Q3: recorded, never asserted -- a cache-coherent host answers no. */
  if (alive && g_rank == 0) {
    memset(block, PAT_B, BLOCK_BYTES);
  }
  if (alive) {
    alive = rv_barrier(2);
  }
  if (alive && g_rank != 0) {
    arts_fam_flush_consumer(block, BLOCK_BYTES);
    bool stale = !all_bytes_are(block, PAT_B);
    say("q3 stale-without-flush = %s", stale ? "yes" : "no");
  }
  if (alive) {
    alive = rv_barrier(3);
  }

  /* Q5: non-temporal stores plus a fence, no line sweep.  Recorded. */
  if (alive && g_rank == 0) {
    nt_store_pattern(block, PAT_C);
  }
  if (alive) {
    alive = rv_barrier(4);
  }
  if (alive && g_rank != 0) {
    arts_fam_flush_consumer(block, BLOCK_BYTES);
    say("q5 nontemporal-without-sweep = %s",
        all_bytes_are(block, PAT_C) ? "visible" : "not visible");
  }
  if (alive) {
    alive = rv_barrier(5);
  }

  /* Q4: the arena's limits.  Rank 0 only, and after every shared phase. */
  if (alive && g_rank == 0) {
    uint64_t best = arena_limit_mb();
    say("q4 largest arena taken = %llu MB", (unsigned long long)best);
    if (best < g_pool_mb) {
      fail("q4 arena-limits: the configured pool size was refused");
    }
  }

  if (spawner) {
    for (unsigned i = 0; i < nkids; i++) {
      int status = 0;
      waitpid(kids[i], &status, 0);
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail("a peer rank of the probe ended badly");
      }
    }
    char glob_cmd[sizeof(g_rv) + 2];
    (void)snprintf(glob_cmd, sizeof(glob_cmd), "%s.", g_rv);
    /* The rendezvous files are the probe's only byproduct; remove them. */
    for (unsigned phase = 1; phase <= 5; phase++) {
      for (unsigned r = 0; r < g_nranks; r++) {
        char path[sizeof(glob_cmd) + 32];
        (void)snprintf(path, sizeof(path), "%sp%u.r%u", glob_cmd, phase, r);
        (void)unlink(path);
      }
    }
    char path[sizeof(glob_cmd) + 8];
    (void)snprintf(path, sizeof(path), "%sblock", glob_cmd);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%sarena", glob_cmd);
    (void)unlink(path);
  }

  arts_fam_fini();
  if (g_fails) {
    return 1;
  }
  if (spawner) {
    printf("PASS fam_conformance\n");
  }
  return 0;
}
