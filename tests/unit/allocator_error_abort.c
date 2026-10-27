/* Every corruption or misuse the allocator detects ends the process with
 * the allocator error report, and every misuse of the registered pool that
 * the pool itself can see is its fatal error.
 *
 * Each mode named on the command line runs in a child of its own: the parent
 * stays single-threaded, forks, drains the child's output to its end, waits
 * for it and checks how it ended.  It prints only its own verdict lines,
 * never the child's text, unless that text carries a sanitizer report, which
 * it forwards so the suite's fail patterns see it, or the mode failed.  A
 * child drops its soft core limit (it aborts on purpose), declares its
 * machine (regpool_numa_sim.h: one node with room, 32 MiB slabs), writes
 * nothing before the call that provokes its end, and orders every step it
 * takes on another thread by thread creation and join, except where a mode
 * races two frees on purpose.
 *
 * Modes, and the end each expects:
 *   1   a pool block freed twice by a thread that does not own it, the
 *       owner collecting its page later: SIGABRT, heap metadata
 *   2   as 1, after an ordinary exhaustion of the first arena, the
 *       detection falling in the exhaustion cascade of the second: SIGABRT,
 *       heap metadata (an earlier refusal excuses nothing later)
 *   3   an oversize block freed twice: the pool's fatal error, no live slab
 *   3b  an oversize block freed by two threads at once: the pool's fatal
 *       error, whichever free comes second
 *   4   an arena block freed after the pool's cleanup: the pool's fatal
 *       error, no live slab
 *   5   an alignment that is not a power of two: the pool's fatal error
 * and, where the allocator checks them (a debug build of it):
 *   6   a pool block freed twice by one thread: SIGABRT, double free
 *   7   a default-heap block freed twice by one thread: SIGABRT, double free
 *   8   an interior, unaligned pool pointer freed: SIGABRT, invalid pointer
 *   9   a C library block freed to the allocator: SIGABRT, invalid pointer
 *   10  a default-heap block freed by another thread, then by its owner:
 *       SIGABRT, double free
 *   11  freed by its owner, then by another thread: SIGABRT, heap metadata
 *   12  freed by another thread, collected by the owner, freed by another
 *       thread again: SIGABRT, heap metadata
 *
 * Whitebox unit test: no ARTS runtime, no ports, no config.  It links the
 * pool's object and the allocator directly, so the pool, the allocator calls
 * below and the error report share one allocator. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mimalloc.h>

#include "arts/memory/regpool.h"
#include "arts/runtime_state.h"
#include "arts/system/identity.h"
#include "regpool_numa_sim.h"

/* What the pool's log and fatal paths and its per-thread tables reference. */
unsigned int arts_global_rank_id = 0;
ARTS_THREAD_LOCAL struct arts_runtime_private_s arts_thread_info;
void *arts_calloc(size_t nmemb, size_t size) { return mi_calloc(nmemb, size); }
void arts_free(void *ptr) { mi_free(ptr); }
#define FATAL_STATUS 3
void arts_abort(uint8_t code) {
  (void)code;
  _exit(FATAL_STATUS);
}

#define SLAB_BYTES ((size_t)32 << 20)
#define NODE_FREE ((size_t)4 << 30)
#define OVERSIZE ((size_t)20 << 20) /* above half the slab: a mapping */
#define FILL_BLOCK ((size_t)4 << 10)
#define SMALL ((size_t)1 << 10)
#define MAX_FILL 200000u

/* ------------------------------------------------------------------------- */
/* child side                                                                  */
/* ------------------------------------------------------------------------- */

static void *g_block;

static unsigned slabs(void) {
  char *buf = NULL;
  size_t len = 0;
  FILE *f = open_memstream(&buf, &len);
  if (f == NULL)
    abort();
  arts_regpool_report(f);
  fclose(f);
  unsigned v = ~0u;
  const char *line = strstr(buf, "[REGPOOL] total: slabs=");
  if (line == NULL ||
      sscanf(line + strlen("[REGPOOL] total: slabs="), "%u", &v) != 1)
    v = ~0u;
  free(buf);
  return v;
}

static void fill_until(unsigned n) {
  for (unsigned i = 0; i < MAX_FILL && slabs() < n; i++)
    memset(arts_regpool_alloc_aligned(FILL_BLOCK, 64), 0x11, 64);
}

static void on_other_thread(void *(*fn)(void *)) {
  pthread_t t;
  pthread_create(&t, NULL, fn, NULL);
  pthread_join(t, NULL);
}

static void *pool_free_twice(void *arg) {
  (void)arg;
  arts_regpool_free(g_block);
  arts_regpool_free(g_block);
  return NULL;
}

static void *mi_free_block(void *arg) {
  (void)arg;
  mi_free(g_block);
  return NULL;
}

/* A small block on a page its owner keeps and that is not full. */
static void owned_pool_block(void) {
  g_block = arts_regpool_alloc_aligned(SMALL, 64);
  for (int i = 0; i < 8; i++)
    (void)arts_regpool_alloc_aligned(SMALL, 64);
}

static void owned_default_block(void) {
  g_block = mi_malloc(SMALL);
  for (int i = 0; i < 8; i++)
    (void)mi_malloc(SMALL);
}

/* What collects the owner's pages: allocations of the block's size. */
static void owner_collects_pool(void) {
  for (int i = 0; i < 4096; i++)
    memset(arts_regpool_alloc_aligned(SMALL, 64), 0, 64);
}

static void owner_collects_default(void) {
  for (int i = 0; i < 4096; i++)
    memset(mi_malloc(SMALL), 0, 64);
  mi_collect(true);
}

static pthread_barrier_t g_together;
static sem_t g_freed;

static void *free_oversize_together(void *arg) {
  (void)arg;
  pthread_barrier_wait(&g_together);
  arts_regpool_free(g_block);
  sem_post(&g_freed);
  return NULL;
}

/* Returns only when the mode passed its provocation silently. */
static void provoke(const char *mode) {
  if (!strcmp(mode, "1")) {
    owned_pool_block();
    on_other_thread(pool_free_twice);
    owner_collects_pool();
  } else if (!strcmp(mode, "2")) {
    owned_pool_block();
    fill_until(2);
    on_other_thread(pool_free_twice);
    fill_until(3);
  } else if (!strcmp(mode, "3")) {
    void *p = arts_regpool_alloc_aligned(OVERSIZE, 64);
    arts_regpool_free(p);
    arts_regpool_free(p);
  } else if (!strcmp(mode, "3b")) {
    /* Detached threads, and the main thread waits for both frees to return:
     * whichever free ends the process, no thread is left joinable. */
    g_block = arts_regpool_alloc_aligned(OVERSIZE, 64);
    pthread_barrier_init(&g_together, NULL, 2);
    sem_init(&g_freed, 0, 0);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &attr, free_oversize_together, NULL);
    pthread_create(&t, &attr, free_oversize_together, NULL);
    pthread_attr_destroy(&attr);
    sem_wait(&g_freed);
    sem_wait(&g_freed);
  } else if (!strcmp(mode, "4")) {
    void *a = arts_regpool_alloc_aligned(SMALL, 64);
    (void)arts_regpool_alloc_aligned(OVERSIZE, 64);
    arts_regpool_cleanup();
    arts_regpool_free(a);
  } else if (!strcmp(mode, "5")) {
    (void)arts_regpool_alloc_aligned(SMALL, 96);
  } else if (!strcmp(mode, "6")) {
    void *p = arts_regpool_alloc_aligned(256, 64);
    arts_regpool_free(p);
    arts_regpool_free(p);
  } else if (!strcmp(mode, "7")) {
    void *p = mi_malloc(48);
    mi_free(p);
    mi_free(p);
  } else if (!strcmp(mode, "8")) {
    char *p = (char *)arts_regpool_alloc_aligned(256, 64);
    arts_regpool_free(p + 1);
  } else if (!strcmp(mode, "9")) {
    void *p = malloc(64);
    mi_free(p);
  } else if (!strcmp(mode, "10")) {
    owned_default_block();
    on_other_thread(mi_free_block);
    mi_free(g_block);
    owner_collects_default();
  } else if (!strcmp(mode, "11")) {
    owned_default_block();
    mi_free(g_block);
    on_other_thread(mi_free_block);
    owner_collects_default();
  } else if (!strcmp(mode, "12")) {
    owned_default_block();
    on_other_thread(mi_free_block);
    mi_collect(false);
    on_other_thread(mi_free_block);
    owner_collects_default();
  } else {
    _exit(2);
  }
}

static void child(const char *mode) {
  struct rlimit core;
  if (getrlimit(RLIMIT_CORE, &core) == 0) {
    core.rlim_cur = 0;
    (void)setrlimit(RLIMIT_CORE, &core);
  }
  regpool_sim_declare(1, REGPOOL_SIM_FREE);
  regpool_sim_set_node_free(0, NODE_FREE);
  arts_regpool_set_topology(1, 1, NULL);
  if (!arts_regpool_init(NULL, NULL, SLAB_BYTES, 1))
    _exit(2);
  provoke(mode);
  _exit(0);
}

/* ------------------------------------------------------------------------- */
/* parent side                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct {
  const char *mode;
  bool aborts;        /* SIGABRT with the report, else the pool's fatal exit */
  const char *expect; /* the report's class, or the fatal error's text */
  const char *expect2; /* another acceptable fatal error text, or NULL */
} abort_mode_t;

static const abort_mode_t g_modes[] = {
    {"1", true, "heap metadata corrupted or unobtainable", NULL},
    {"2", true, "heap metadata corrupted or unobtainable", NULL},
    {"3", false, "which no live slab holds", NULL},
    {"3b", false, "not a live oversize block's start",
     "which no live slab holds"},
    {"4", false, "which no live slab holds", NULL},
    {"5", false, "is not a power of two", NULL},
    {"6", true, "double free", NULL},
    {"7", true, "double free", NULL},
    {"8", true, "invalid pointer or argument", NULL},
    {"9", true, "invalid pointer or argument", NULL},
    {"10", true, "double free", NULL},
    {"11", true, "heap metadata corrupted or unobtainable", NULL},
    {"12", true, "heap metadata corrupted or unobtainable", NULL},
};

static const abort_mode_t *find_mode(const char *name) {
  for (size_t i = 0; i < sizeof g_modes / sizeof g_modes[0]; i++)
    if (!strcmp(g_modes[i].mode, name))
      return &g_modes[i];
  return NULL;
}

static bool has_sanitizer_report(const char *text) {
  return strstr(text, "WARNING: ThreadSanitizer") != NULL ||
         strstr(text, "ERROR: AddressSanitizer") != NULL ||
         strstr(text, "ERROR: LeakSanitizer") != NULL ||
         strstr(text, "runtime error:") != NULL;
}

/* Runs one mode in a child; true when it ended as the mode expects. */
static bool run_mode(const abort_mode_t *m) {
  int fds[2];
  if (pipe(fds) != 0) {
    printf("FAIL allocator_error_abort: %s: pipe: %s\n", m->mode,
           strerror(errno));
    return false;
  }
  fflush(NULL);
  pid_t pid = fork();
  if (pid < 0) {
    printf("FAIL allocator_error_abort: %s: fork: %s\n", m->mode,
           strerror(errno));
    return false;
  }
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    child(m->mode);
  }
  close(fds[1]);
  size_t cap = 1 << 16, len = 0;
  char *text = (char *)malloc(cap);
  for (;;) {
    if (len + 4096 + 1 > cap) {
      cap *= 2;
      text = (char *)realloc(text, cap);
    }
    ssize_t n = read(fds[0], text + len, cap - len - 1);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    len += (size_t)n;
  }
  text[len] = '\0';
  close(fds[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }

  bool ok;
  if (m->aborts) {
    char marker[128];
    snprintf(marker, sizeof marker, "[ARTS] allocator error: %s (code",
             m->expect);
    ok = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT &&
         strstr(text, marker) != NULL;
  } else {
    ok = WIFEXITED(status) && WEXITSTATUS(status) == FATAL_STATUS &&
         (strstr(text, m->expect) != NULL ||
          (m->expect2 != NULL && strstr(text, m->expect2) != NULL));
  }
  if (has_sanitizer_report(text)) {
    fputs(text, stderr);
    ok = false;
  }
  if (ok) {
    printf("PASS allocator_error_abort: %s\n", m->mode);
  } else {
    printf("FAIL allocator_error_abort: %s: status 0x%x (%s %d); the child "
           "wrote:\n",
           m->mode, (unsigned)status,
           WIFSIGNALED(status) ? "signal" : "exit",
           WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
    fputs(text, stdout);
  }
  free(text);
  return ok;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    printf("FAIL allocator_error_abort: no mode named\n");
    return 1;
  }
  bool all = true;
  for (int i = 1; i < argc; i++) {
    const abort_mode_t *m = find_mode(argv[i]);
    if (m == NULL) {
      printf("FAIL allocator_error_abort: unknown mode %s\n", argv[i]);
      all = false;
      continue;
    }
    if (!run_mode(m))
      all = false;
  }
  if (!all)
    return 1;
  printf("PASS allocator_error_abort: all %d modes\n", argc - 1);
  return 0;
}
