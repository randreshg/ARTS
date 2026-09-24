/* The allocator's fatal diagnostics: one child per refusal, each read back for
 * the message that names what was wrong, plus the half no abort can show --
 * that a surviving allocator never returns one block twice after a single
 * free.  The addresses covered are every one the two entry points must refuse:
 * a block freed twice, a granule interior to a block, another rank's slice,
 * the header page, and the tail a page-multiple slice length leaves past the
 * last slice. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "arts/fam/pool.h"

#define POOL_BYTES ((uint64_t)4 * 1024 * 1024)
static void *g_region;

void arts_fam_backend_map(unsigned rank, unsigned nranks, void **out_base,
                          uint64_t *out_bytes) {
  (void)rank;
  uint64_t per = (POOL_BYTES - ARTS_FAM_HEADER_BYTES) / nranks;
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
void arts_fam_backend_unmap(void *b, uint64_t n) {
  (void)b;
  (void)n;
}
void arts_fam_backend_flush(const void *p, size_t n, bool producer) {
  (void)p;
  (void)n;
  (void)producer;
}
void arts_fam_backend_poison(void *p, size_t n) {
  (void)p;
  (void)n;
}
void arts_fam_backend_hold(const void *p, size_t n, bool hold) {
  (void)p;
  (void)n;
  (void)hold;
}
bool arts_fam_backend_strict(void) { return false; }
void arts_fam_backend_config_check(const struct arts_config_s *c) { (void)c; }

static void body_double_free(void) {
  void *p = arts_fam_alloc(128);
  arts_fam_free(p);
  arts_fam_free(p);
}
static void body_owner_of_header(void) {
  (void)arts_fam_owner_of(g_region); /* the header page names no slice */
}
static void body_owner_of_tail(void) {
  const struct arts_fam_header_s *h =
      (const struct arts_fam_header_s *)g_region;
  /* Past the last slice, still inside the pool: the tail a page-multiple
   * slice length leaves over is handed to nobody and names no slice. */
  const unsigned char *past = (const unsigned char *)g_region +
                              ARTS_FAM_HEADER_BYTES +
                              (uint64_t)h->nranks * h->slice_len;
  (void)arts_fam_owner_of(past);
}
static void body_free_other_slice(void) {
  const struct arts_fam_header_s *h =
      (const struct arts_fam_header_s *)g_region;
  unsigned char *other =
      (unsigned char *)g_region + ARTS_FAM_HEADER_BYTES + h->slice_len;
  arts_fam_free(other); /* rank 1's first block; this rank is 0 */
}
static void body_free_interior(void) {
  unsigned char *p = (unsigned char *)arts_fam_alloc(2u * ARTS_FAM_GRANULE);
  arts_fam_free(p + ARTS_FAM_GRANULE); /* the block's second granule */
}
/* Runs one death path in a child and returns its output.  ARTS_ERROR prints
 * with a raw write(2) and the stub's arts_abort is _exit, so nothing here is
 * buffered and no flush is owed; a child that PRINTED with stdio would need
 * one, which is why none of these do. */
static int child_dies(void (*body)(void), char *out, size_t cap) {
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
    body();
    _exit(0);
  }
  close(fds[1]);
  size_t got = 0;
  for (;;) {
    ssize_t n = read(fds[0], out + got, cap - 1 - got);
    if (n <= 0 || got + 1 >= cap) {
      break;
    }
    got += (size_t)n;
  }
  out[got] = '\0';
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status));
}

int main(void) {
  g_region = aligned_alloc(ARTS_FAM_PAGE, POOL_BYTES);
  if (!g_region) {
    printf("FAIL fam_double_free: region\n");
    return 1;
  }
  arts_fam_init(0u, 2u);

  const struct arts_fam_header_s *h =
      (const struct arts_fam_header_s *)g_region;
  if (ARTS_FAM_HEADER_BYTES + (uint64_t)h->nranks * h->slice_len >=
      POOL_BYTES) {
    /* Without a tail the owner_of case below would only re-test the bound the
     * header case already covers, and would pass without proving anything. */
    printf("FAIL fam_double_free: this pool leaves no tail past its slices\n");
    return 1;
  }

  static const struct {
    void (*body)(void);
    const char *says;
    const char *what;
  } deaths[] = {
      {body_double_free, "already freed", "a second free of one block"},
      {body_free_interior, "does not start a block",
       "a free of a granule interior to a block"},
      {body_free_other_slice, "is not a block of this rank's slice",
       "a free of another rank's slice"},
      {body_owner_of_header, "not a slot of the pool",
       "owner_of on the header page"},
      {body_owner_of_tail, "not a slot of the pool",
       "owner_of past the last slice"},
  };
  char out[2048];
  for (size_t i = 0; i < sizeof(deaths) / sizeof(deaths[0]); i++) {
    if (!child_dies(deaths[i].body, out, sizeof(out)) ||
        !strstr(out, deaths[i].says)) {
      printf("FAIL fam_double_free: %s was accepted or misdiagnosed: %s\n",
             deaths[i].what, out);
      return 1;
    }
  }

  /* One free, then two allocations of that class: they must be distinct. */
  void *p = arts_fam_alloc(128);
  arts_fam_free(p);
  void *q = arts_fam_alloc(128);
  void *r = arts_fam_alloc(128);
  if (q == r) {
    printf("FAIL fam_double_free: one block handed out twice\n");
    return 1;
  }
  arts_fam_free(q);
  arts_fam_free(r);

  arts_fam_fini();
  free(g_region);
  printf("PASS fam_double_free\n");
  return 0;
}
