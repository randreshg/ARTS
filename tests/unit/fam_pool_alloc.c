/* Standalone: compiles core/fam/pool.c against a malloc-backed region, so the
 * allocator's contract is checked with no backend, no runtime and no fork. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "arts/fam/pool.h"

#define NRANKS 4u
#define MY_RANK 1u
#define POOL_BYTES ((uint64_t)16 * 1024 * 1024)

/* The test IS the backend, and it writes the header the allocator verifies. */
static void *g_region;

void arts_fam_backend_map(unsigned rank, unsigned nranks, void **out_base,
                          uint64_t *out_bytes) {
  (void)rank;
  uint64_t usable = POOL_BYTES - ARTS_FAM_HEADER_BYTES;
  uint64_t per = usable / nranks;
  per -= per % ARTS_FAM_PAGE;
  struct arts_fam_header_s *h = (struct arts_fam_header_s *)g_region;
  memset(h, 0, sizeof(*h));
  h->magic = ARTS_FAM_MAGIC;
  h->bytes = POOL_BYTES;
  h->slice_len = per;
  h->nranks = nranks;
  *out_base = g_region;
  *out_bytes = POOL_BYTES;
}
void arts_fam_backend_unmap(void *base, uint64_t bytes) {
  (void)base;
  (void)bytes;
}
void arts_fam_backend_flush(const void *p, size_t bytes, bool producer) {
  (void)p;
  (void)bytes;
  (void)producer;
}
void arts_fam_backend_config_check(const struct arts_config_s *c) { (void)c; }

static int failures;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("FAIL fam_pool_alloc: %s\n", (msg));                              \
      failures++;                                                              \
    }                                                                          \
  } while (0)

#define CHURN_THREADS 8
#define CHURN_PHASES 4
#define CHURN_ROUNDS 500 /* per phase */
#define CHURN_LIVE 16
#define CHURN_MAILBOX 16

/* Sizes span five classes, so the churn exercises the split path too. */
static size_t churn_size(unsigned *seed) {
  static const size_t sizes[] = {1, 64, 65, 900, 4096, 4097, 16384};
  return sizes[rand_r(seed) % (sizeof(sizes) / sizeof(sizes[0]))];
}

/* A block handed to the next thread carries what that thread needs to judge
 * it: the byte oracle is the ALLOCATING thread's tag, and the freeing thread
 * has no other way to know it. */
struct churn_gift_s {
  void *p;
  size_t len;
  unsigned char tag;
};

/* One mailbox per thread, filled only by the thread before it and drained only
 * by its owner, so the count alone publishes the entries: released when the
 * producer raises it, acquired when the consumer reads it. */
static struct churn_mail_s {
  struct churn_gift_s gift[CHURN_MAILBOX];
  _Atomic unsigned n;
} g_mail[CHURN_THREADS];
static pthread_barrier_t g_churn_barrier;

static void churn_check_tag(const void *p, size_t len, unsigned char tag) {
  const unsigned char *b = (const unsigned char *)p;
  for (size_t k = 0; k < len; k++) {
    if (b[k] != tag) {
      printf("FAIL fam_pool_alloc: block overlapped another thread's\n");
      __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
      return;
    }
  }
}

/* Allocation and free are not paired per thread: a share of every thread's
 * blocks is freed by the NEXT thread, which is the module's stated contract
 * (any thread may free) and the only way a per-thread cache could be caught
 * handing one granule to two owners. */
static void *churn(void *arg) {
  unsigned me = (unsigned)(uintptr_t)arg; /* 1 .. CHURN_THREADS */
  unsigned char tag = (unsigned char)me;
  unsigned seed = me * 7919u + 13u;
  struct churn_mail_s *inbox = &g_mail[me - 1u];
  struct churn_mail_s *outbox = &g_mail[me % CHURN_THREADS];
  void *live[CHURN_LIVE];
  size_t len[CHURN_LIVE];
  memset(live, 0, sizeof(live));
  memset(len, 0, sizeof(len));
  for (int phase = 0; phase < CHURN_PHASES; phase++) {
    unsigned sent = 0;
    for (int r = 0; r < CHURN_ROUNDS; r++) {
      int i = rand_r(&seed) % CHURN_LIVE;
      if (live[i]) {
        churn_check_tag(live[i], len[i], tag);
        if (sent < CHURN_MAILBOX) {
          outbox->gift[sent].p = live[i];
          outbox->gift[sent].len = len[i];
          outbox->gift[sent].tag = tag;
          sent++;
          atomic_store_explicit(&outbox->n, sent, memory_order_release);
        } else {
          arts_fam_free(live[i]);
        }
        live[i] = NULL;
      } else {
        size_t n = churn_size(&seed);
        void *p = arts_fam_alloc(n);
        if (((uintptr_t)p & (ARTS_FAM_GRANULE - 1u)) != 0) {
          printf("FAIL fam_pool_alloc: unaligned block %p\n", p);
          __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
        }
        if (!arts_fam_contains(p)) {
          printf("FAIL fam_pool_alloc: allocated block outside the pool\n");
          __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
        }
        memset(p, (int)tag, n);
        live[i] = p;
        len[i] = n;
      }
    }
    /* No thread is still filling a mailbox past this point. */
    pthread_barrier_wait(&g_churn_barrier);
    unsigned got = atomic_load_explicit(&inbox->n, memory_order_acquire);
    for (unsigned k = 0; k < got; k++) {
      churn_check_tag(inbox->gift[k].p, inbox->gift[k].len, inbox->gift[k].tag);
      arts_fam_free(inbox->gift[k].p);
    }
    atomic_store_explicit(&inbox->n, 0u, memory_order_relaxed);
    /* Every mailbox is drained and reset before any of them is filled again. */
    pthread_barrier_wait(&g_churn_barrier);
  }
  for (int i = 0; i < CHURN_LIVE; i++) {
    if (live[i]) {
      arts_fam_free(live[i]);
    }
  }
  return NULL;
}

/* Exhaustion aborts, so it is observed in a child whose output is read back:
 * the message must name the knob that fixes it. */
static int exhaustion_names_the_knob(void) {
  int fds[2];
  if (pipe(fds) != 0) {
    return 0;
  }
  pid_t pid = fork();
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[0]);
    close(fds[1]);
    for (;;) {
      (void)arts_fam_alloc(1u << 20);
    }
  }
  close(fds[1]);
  char buf[4096];
  size_t got = 0;
  for (;;) {
    ssize_t n = read(fds[0], buf + got, sizeof(buf) - 1 - got);
    if (n <= 0) {
      break;
    }
    got += (size_t)n;
    if (got + 1 >= sizeof(buf)) {
      break;
    }
  }
  buf[got] = '\0';
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  int died = WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status));
  return died && strstr(buf, "fam_pool_mb") != NULL &&
         strstr(buf, "granules") != NULL;
}

/* An empty class must be served by halving a larger free block rather than
 * aborting while the slice is mostly free.  Exhaustion is fatal, so the
 * sequence runs in a child; and it re-initialises the allocator first, because
 * the bump cursor never rewinds and the counts below assume it starts at 0.
 * The counts come from the test's own constants: with the cursor drained and
 * class 0 empty, a 64-byte request can only be served by splitting the freed
 * big block. */
#define BIG_BYTES (1u << 20)
static void split_serves_an_empty_class(void) {
  int fds[2];
  if (pipe(fds) != 0) {
    CHECK(0, "pipe");
    return;
  }
  pid_t pid = fork();
  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[0]);
    close(fds[1]);
    arts_fam_fini();
    arts_fam_init(MY_RANK, NRANKS);
    uint64_t usable = POOL_BYTES - ARTS_FAM_HEADER_BYTES;
    uint64_t per = usable / NRANKS;
    per -= per % ARTS_FAM_PAGE;
    uint64_t granules = per / ARTS_FAM_GRANULE;
    uint64_t big_granules = BIG_BYTES / ARTS_FAM_GRANULE;
    uint64_t nbig = granules / big_granules;
    uint64_t small_left = granules - nbig * big_granules;
    void *first_big = NULL;
    for (uint64_t i = 0; i < nbig; i++) {
      void *p = arts_fam_alloc(BIG_BYTES);
      if (i == 0) {
        first_big = p;
      }
    }
    for (uint64_t i = 0; i < small_left; i++) {
      (void)arts_fam_alloc(ARTS_FAM_GRANULE);
    }
    /* The cursor is gone and class 0 is empty; only a split can serve this. */
    arts_fam_free(first_big);
    void *small = arts_fam_alloc(ARTS_FAM_GRANULE);
    printf("%s\n", arts_fam_contains(small) ? "split-ok" : "split-bad");
    /* STDOUT is a pipe here, so stdio is FULLY buffered and _exit does not
     * flush: without this the parent reads zero bytes and the check below
     * fails whatever the allocator did. */
    (void)fflush(stdout);
    _exit(0);
  }
  close(fds[1]);
  char buf[128] = {0};
  (void)read(fds[0], buf, sizeof(buf) - 1);
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
            strstr(buf, "split-ok"),
        "a 64-byte request is served by splitting a freed 1 MiB block");
}

int main(void) {
  g_region = aligned_alloc(ARTS_FAM_PAGE, POOL_BYTES);
  CHECK(g_region != NULL, "region");
  if (!g_region) {
    return 1;
  }

  int automatic = 0;
  CHECK(!arts_fam_contains(&automatic), "contains() true before init");

  arts_fam_init(MY_RANK, NRANKS);

  CHECK(!arts_fam_contains(&automatic), "a stack address is not in the pool");

  void *a = arts_fam_alloc(1);
  void *b = arts_fam_alloc(64);
  void *c = arts_fam_alloc(65);
  CHECK(((uintptr_t)a % ARTS_FAM_GRANULE) == 0 &&
            ((uintptr_t)b % ARTS_FAM_GRANULE) == 0 &&
            ((uintptr_t)c % ARTS_FAM_GRANULE) == 0,
        "64-byte alignment");
  CHECK(a != b && b != c && a != c, "distinct blocks");
  CHECK(arts_fam_contains(a) && arts_fam_contains(b) && arts_fam_contains(c),
        "contains() true for live blocks");

  /* The owner of a slot is its address, and nothing else.  Own blocks answer
   * this rank; a computed address inside another rank's slice answers that
   * rank; the ends of every slice are checked because the division's boundary
   * is where an off-by-one lives. */
  CHECK(arts_fam_owner_of(a) == MY_RANK && arts_fam_owner_of(b) == MY_RANK &&
            arts_fam_owner_of(c) == MY_RANK,
        "owner_of names this rank for its own blocks");
  {
    const struct arts_fam_header_s *h =
        (const struct arts_fam_header_s *)g_region;
    int ok = 1;
    for (unsigned r = 0; r < NRANKS; r++) {
      const unsigned char *lo = (const unsigned char *)g_region +
                                ARTS_FAM_HEADER_BYTES +
                                (uint64_t)r * h->slice_len;
      const unsigned char *hi = lo + h->slice_len - ARTS_FAM_GRANULE;
      if (arts_fam_owner_of(lo) != r || arts_fam_owner_of(hi) != r) {
        ok = 0;
      }
    }
    CHECK(ok, "owner_of is the slice index at both ends of every slice");
  }

  /* Reuse: a freed block of a class is what the next same-class request gets. */
  arts_fam_free(b);
  void *b2 = arts_fam_alloc(64);
  CHECK(b2 == b, "a freed 64-byte block is reused");
  arts_fam_free(b2);
  arts_fam_free(a);
  arts_fam_free(c);

  pthread_t t[CHURN_THREADS];
  CHECK(pthread_barrier_init(&g_churn_barrier, NULL, CHURN_THREADS) == 0,
        "churn barrier");
  for (int i = 0; i < CHURN_THREADS; i++) {
    pthread_create(&t[i], NULL, churn, (void *)(uintptr_t)(i + 1));
  }
  for (int i = 0; i < CHURN_THREADS; i++) {
    pthread_join(t[i], NULL);
  }
  pthread_barrier_destroy(&g_churn_barrier);

  split_serves_an_empty_class();
  CHECK(exhaustion_names_the_knob(), "exhaustion is fatal and self-diagnosing");

  arts_fam_fini();
  CHECK(!arts_fam_contains(a), "contains() false after fini");
  free(g_region);

  if (failures) {
    return 1;
  }
  printf("PASS fam_pool_alloc\n");
  return 0;
}
