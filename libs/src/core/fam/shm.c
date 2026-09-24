/* SPDX-License-Identifier: Apache-2.0
 *
 * The pool is an anonymous shared mapping the spawning rank creates before it
 * forks and every rank maps at one address, first of all its mappings.  Being
 * anonymous is load-bearing: nothing survives a kill, nothing can be unlinked
 * under a live run, and two runs cannot meet.  Mapping first is load-bearing
 * too: a rank that mapped a fixed address after the fabric and the registered
 * pool could find it taken while another rank did not.
 *
 * Two invariants govern the handoff itself, and both are about what a rank
 * must NOT still be carrying once it has mapped: the descriptor is
 * inheritable across exactly one exec, the one that turns a forked child into
 * a rank, and the environment entry that names it is gone from every rank
 * before that rank has a second thread that could read the environment. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cpuid.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "arts.h"
#include "arts/fam/pool.h"
#include "arts/system/config.h"
#include "arts/system/print.h"

static int g_fd = -1;
static void *g_mapped;
static uint64_t g_mapped_bytes;
static unsigned g_pool_mb;
static bool g_strict;
static bool g_clflushopt;

static uint64_t fam_slice_len(uint64_t bytes, unsigned nranks) {
  uint64_t usable = bytes - ARTS_FAM_HEADER_BYTES;
  uint64_t per = usable / nranks;
  return per - (per % ARTS_FAM_PAGE);
}

/* One field of the handoff, taken whole: the conversion must start on a
 * digit, stay inside the range the field is read back into, and stop exactly
 * on the separator that field is followed by.  A sign, a space, a saturating
 * value or a trailing character makes the spec unusable rather than
 * truncated, because every field here is read back into a narrower type than
 * the conversion produces. */
static bool fam_spec_field(const char **cursor, char sep, unsigned long long max,
                           unsigned long long *out) {
  const char *s = *cursor;
  if (*s < '0' || *s > '9') {
    return false;
  }
  char *end = NULL;
  errno = 0;
  unsigned long long v = strtoull(s, &end, 10);
  if (errno == ERANGE || v > max || *end != sep) {
    return false;
  }
  *out = v;
  *cursor = sep ? end + 1 : end;
  return true;
}

void arts_fam_boot_prepare(const struct arts_config_s *config) {
  /* Recorded before any branch: a spawned rank takes this path too, and it
   * decides its own strict mode from its own parsed configuration. */
  g_pool_mb = config->fam_pool_mb;
  g_strict = config->fam_strict;

  const char *spec = getenv(ARTS_FAM_SHM_ENV);
  unsigned nranks = config->table_length ? config->table_length : 1u;
  uint64_t bytes;
  bool creator;

  if (spec) {
    const char *cursor = spec;
    unsigned long long spec_fd = 0;
    unsigned long long spec_bytes = 0;
    unsigned long long spec_ranks = 0;
    if (!fam_spec_field(&cursor, ',', (unsigned long long)INT_MAX, &spec_fd) ||
        !fam_spec_field(&cursor, ',', UINT64_MAX, &spec_bytes) ||
        !fam_spec_field(&cursor, '\0', (unsigned long long)UINT_MAX,
                        &spec_ranks)) {
      ARTS_ERROR("fam: %s is unusable ('%s')", ARTS_FAM_SHM_ENV, spec);
    }
    int fd = (int)spec_fd;
    /* Everything the spec claims is checked before it is used: a descriptor
     * this rank does not hold, a rank count that is not this run's, and a
     * size that is not the one this rank's own config asks for.  The last is
     * not pedantry -- every rank parses its own cfg, so two ranks CAN name
     * different fam_pool_mb values, and the creator's size is the only one
     * the object actually has. */
    if (fcntl(fd, F_GETFD) < 0) {
      ARTS_ERROR("fam: %s names descriptor %d, which this rank does not have",
                 ARTS_FAM_SHM_ENV, fd);
    }
    if (spec_ranks != (unsigned long long)nranks) {
      ARTS_ERROR("fam: the pool was made for %llu ranks, this run has %u",
                 spec_ranks, nranks);
    }
    if (spec_bytes != (unsigned long long)config->fam_pool_mb * 1024u * 1024u) {
      ARTS_ERROR("fam: the pool is %llu bytes, but this rank's fam_pool_mb=%u "
                 "asks for %llu - every rank of a run must name the same value",
                 spec_bytes, config->fam_pool_mb,
                 (unsigned long long)config->fam_pool_mb * 1024u * 1024u);
    }
    g_fd = fd;
    bytes = (uint64_t)spec_bytes;
    creator = false;
  } else {
    bytes = (uint64_t)g_pool_mb * 1024u * 1024u;
    /* Created CLOEXEC, and cleared on exactly one edge: a forked child that
     * is about to become a rank. */
    g_fd = memfd_create("arts-fam", MFD_CLOEXEC);
    if (g_fd < 0) {
      ARTS_ERROR("fam: cannot create the pool object: %s", strerror(errno));
    }
    if (ftruncate(g_fd, (off_t)bytes) != 0) {
      ARTS_ERROR("fam: cannot size the pool to %llu bytes: %s",
                 (unsigned long long)bytes, strerror(errno));
    }
    creator = true;
  }

  if (g_strict) {
    ARTS_ERROR("fam: fam_strict is not available in this backend yet");
  }

  /* Sparse and never populated: a run pays for the granules it touches. */
  void *want = (void *)(uintptr_t)ARTS_FAM_BASE;
  void *view = mmap(want, (size_t)bytes, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_FIXED_NOREPLACE, g_fd, 0);
  if (view == MAP_FAILED || view != want) {
    ARTS_ERROR("fam: the pool's address %p is not available in this rank: %s",
               want, view == MAP_FAILED ? strerror(errno) : "taken");
  }
  if (creator) {
    struct arts_fam_header_s *h = (struct arts_fam_header_s *)view;
    memset(h, 0, sizeof(*h));
    h->magic = ARTS_FAM_MAGIC;
    h->bytes = bytes;
    h->slice_len = fam_slice_len(bytes, nranks);
    h->nranks = nranks;
  }

  /* Reached only with a mapping at the one address, so a later reader of
   * g_mapped can treat a null as "no pool" and nothing else. */
  g_mapped = view;
  g_mapped_bytes = bytes;

  if (creator) {
    char env[64];
    (void)snprintf(env, sizeof(env), "%d,%llu,%u", g_fd,
                   (unsigned long long)bytes, nranks);
    setenv(ARTS_FAM_SHM_ENV, env, 1);
  } else {
    /* A rank that adopted the pool is done with both the moment it has
     * mapped: the mapping owns the object, so the descriptor goes rather than
     * living on as an inheritable one, and the entry leaves the environment
     * here, where this process still has no thread that reads it. */
    close(g_fd);
    g_fd = -1;
    unsetenv(ARTS_FAM_SHM_ENV);
  }
}

void arts_fam_boot_child_exec(void) {
  /* The one edge on which the descriptor may be inherited: a child that is
   * about to become a rank.  That rank drops the inheritability again as soon
   * as it has mapped. */
  if (g_fd >= 0) {
    (void)fcntl(g_fd, F_SETFD, 0);
  }
}

void arts_fam_boot_launched(void) {
  /* The ranks that needed the handoff have it.  The environment is rewritten
   * only while no thread that reads it exists; the fabric's threads are the
   * first that may, and they come later. */
  unsetenv(ARTS_FAM_SHM_ENV);
}

void arts_fam_backend_map(unsigned rank, unsigned nranks, void **out_base,
                          uint64_t *out_bytes) {
  (void)rank;
  (void)nranks;
  if (!g_mapped) {
    ARTS_ERROR("fam: the pool was not mapped before the runtime came up");
  }
  /* Only the rank that created the object still holds a descriptor here: it
   * had to outlive the fork, and the mapping keeps the object alive without
   * it.  A rank that adopted the pool closed its own at boot and left this
   * negative to say so. */
  if (g_fd >= 0) {
    close(g_fd);
    g_fd = -1;
  }
  {
    unsigned a, b, c, d;
    g_clflushopt = __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & (1u << 23));
  }
  *out_base = g_mapped;
  *out_bytes = g_mapped_bytes;
}

void arts_fam_backend_unmap(void *base, uint64_t bytes) {
  if (base) {
    munmap(base, (size_t)bytes);
  }
  g_mapped = NULL;
  g_mapped_bytes = 0;
}

/* A per-rank fact of the rank's own parsed config, settled in
 * arts_fam_boot_prepare before the first mapping: a test may ask which mode it
 * is running under rather than assume it. */
bool arts_fam_backend_strict(void) { return g_strict; }

void arts_fam_backend_config_check(const struct arts_config_s *config) {
  if (config->launcher && strcmp(config->launcher, "local") == 0) {
    return;
  }
  /* The launcher is decided by handle_launcher, where a scheduler's variables
   * in the environment override the cfg and an absent key defaults to ssh --
   * both of which surprise the reader, so the message names whichever decided
   * it. */
  static const char *const deciders[] = {"SLURM_PROCID", "SLURM_NNODES",
                                         "LSB_HOSTS", "LSB_MCPU_HOSTS",
                                         "FLUX_TASK_RANK"};
  const char *decider = NULL;
  for (unsigned i = 0; i < sizeof(deciders) / sizeof(deciders[0]); i++) {
    if (getenv(deciders[i])) {
      decider = deciders[i];
      break;
    }
  }
  ARTS_ERROR("fam: this build's fabric-attached memory is an inherited "
             "mapping, so it exists only under launcher=local; this run has "
             "launcher=%s%s%s",
             config->launcher ? config->launcher : "(unset)",
             decider ? ", decided by the environment variable " : "",
             decider ? decider : "");
}

static inline void fam_flush_line(const void *p) {
  if (g_clflushopt) {
    __asm__ __volatile__(".byte 0x66; clflush %0" : "+m"(*(volatile char *)p));
  } else {
    __asm__ __volatile__("clflush %0" : "+m"(*(volatile char *)p));
  }
}

void arts_fam_backend_flush(const void *p, size_t bytes, bool producer) {
  /* The instruction writes back AND invalidates, so one sweep serves both
   * roles; only the fence differs. */
  const char *s =
      (const char *)((uintptr_t)p & ~(uintptr_t)(ARTS_FAM_GRANULE - 1u));
  const char *e = (const char *)p + bytes;
  for (; s < e; s += ARTS_FAM_GRANULE) {
    fam_flush_line(s);
  }
  if (producer) {
    /* The write-backs must precede whatever tells a peer to read them. */
    __asm__ __volatile__("sfence" ::: "memory");
  } else {
    /* The invalidations must precede the loads that follow, which is a
     * load-ordering property a store fence does not give. */
    __asm__ __volatile__("mfence" ::: "memory");
  }
}

void arts_fam_backend_poison(void *p, size_t bytes) {
  (void)p;
  (void)bytes;
}

void arts_fam_backend_hold(const void *p, size_t bytes, bool hold) {
  (void)p;
  (void)bytes;
  (void)hold;
}
