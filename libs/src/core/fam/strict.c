/* SPDX-License-Identifier: Apache-2.0
 *
 * Why this exists: on one host every rank sits behind one hardware-coherent
 * cache, so a missing or misplaced flush can never produce a wrong value there
 * -- and that is exactly the defect a store that is not coherent across hosts
 * turns into one.  This oracle makes it visible on one host.  It checks the
 * flush discipline such a store needs; it is never a performance mode.
 *
 * Two coherency domains on one host: a private copy-on-write view of the pool
 * at the pool's address, and the shared backing elsewhere.  A write that was
 * not flushed therefore does not exist for anyone else, so a missing or
 * misplaced flush is a wrong value rather than nothing at all.  Fresh blocks
 * are poisoned, so a read before the first write is reproducibly wrong too.
 *
 * The two flush roles are DISJOINT.  A producer flush copies its range out and
 * reloads nothing; a consumer flush reloads its range and copies nothing out.
 * A consumer flush is performed by a rank that did not write the range, and
 * copy-on-write is per page, so a rank's own earlier write or reload froze its
 * neighbours' then-current bytes into its private page: anything a consumer
 * path could publish is such a captured byte, and publishing it overwrites a
 * legitimate writer's newer, correctly flushed bytes.
 *
 * The one other write-back is the sampled eviction, and it may touch only a
 * line that is inside a range this rank has registered as held -- within which
 * this rank is the only writer, so the backing has not moved since this rank
 * last reloaded that line and "differs from the backing" means "I wrote it".
 * Never compare contents outside a registered range, and never against a
 * stored snapshot.  It is what catches code that is correct only because an
 * eviction never happened.
 *
 * The two views are built over the pool the library has already mapped
 * shared, by re-mapping exactly the pool's pages private at their own address
 * and the same backing object shared elsewhere (arts_fam_strict_remap). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "arts.h"
#include "arts/fam/pool.h"
#include "arts/runtime_state.h"
#include "arts/system/config.h"
#include "arts/system/identity.h"
#include "arts/system/print.h"
#include "arts/system/threads.h"
#include "arts/utils/malloc.h"

#define FAM_STRICT_PROBES 64u

/* The hold word: a count in the low bits, a claim bit on top.  The claim is
 * what binds an eviction's decision to its copy; the count is what the arm
 * registers and releases, and it can never reach the bit because a hold that
 * would carry it there is fatal where it is taken. */
#define FAM_HELD_CLAIM ((uint16_t)0x8000u)
#define FAM_HELD_COUNT ((uint16_t)0x7fffu)

static void *g_private;
static void *g_shared;
static uint64_t g_bytes;
static uint64_t g_lines;
static _Atomic uint16_t *g_held;
static unsigned g_seed_base;
static ARTS_THREAD_LOCAL unsigned g_seed;
static ARTS_THREAD_LOCAL bool g_seed_ready;

static inline uint64_t fam_line_of(const void *p) {
  return (uint64_t)((const unsigned char *)p - (const unsigned char *)g_private) /
         ARTS_FAM_GRANULE;
}

static inline unsigned char *fam_priv(uint64_t line) {
  return (unsigned char *)g_private + line * ARTS_FAM_GRANULE;
}
static inline unsigned char *fam_back(uint64_t line) {
  return (unsigned char *)g_shared + line * ARTS_FAM_GRANULE;
}

/* Strict mode is decided before the pool is mapped and stays decided after it
 * is gone, so every entry point asks whether there is a mapping at all rather
 * than assuming the one that decided the mode also built it. */
static inline bool fam_strict_live(void) {
  return g_lines != 0u && g_held != NULL;
}

static void fam_write_back(uint64_t line) {
  memcpy(fam_back(line), fam_priv(line), ARTS_FAM_GRANULE);
}
static void fam_reload(uint64_t line) {
  memcpy(fam_priv(line), fam_back(line), ARTS_FAM_GRANULE);
}

/* The shared view first, and never at the base: the base is where pointers
 * point.  The private view then takes the base, over the library's own shared
 * pages.  The descriptor must still be open for both. */
static void *fam_strict_views(int fd, off_t off, void *base, uint64_t bytes) {
  g_shared =
      mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
  if (g_shared == MAP_FAILED) {
    ARTS_ERROR("fam: the pool's backing could not be mapped: %s",
               strerror(errno));
  }
  g_private = mmap(base, (size_t)bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_FIXED, fd, off);
  if (g_private == MAP_FAILED || g_private != base) {
    ARTS_ERROR("fam: the pool's private view could not be placed at %p: %s",
               base, g_private == MAP_FAILED ? strerror(errno) : "moved");
  }
  g_bytes = bytes;
  g_lines = bytes / ARTS_FAM_GRANULE;
  g_held = (_Atomic uint16_t *)arts_calloc((size_t)g_lines, sizeof(*g_held));
  if (!g_held) {
    ARTS_ERROR("fam: no DRAM for the strict oracle's %llu lines",
               (unsigned long long)g_lines);
  }
  /* No process entropy: the eviction sequence must be replayable by naming the
   * same value again, and an unset variable is the base 0. */
  const char *s = getenv(ARTS_FAM_STRICT_SEED_ENV);
  g_seed_base = s ? (unsigned)strtoul(s, NULL, 0) : 0u;
  return g_private;
}

/* One row of the process's mapping table: the range, whether it is shared,
 * the backing object's offset, device and inode, and its path. */
struct fam_vma_s {
  uintptr_t lo, hi;
  bool shared;
  unsigned long long off, inode;
  unsigned dev_major, dev_minor;
  char path[PATH_MAX];
};

static bool fam_vma_parse(const char *line, struct fam_vma_s *v) {
  char perms[8] = {0};
  unsigned long long lo = 0, hi = 0;
  int at = 0;
  if (sscanf(line, "%llx-%llx %7s %llx %x:%x %llu %n", &lo, &hi, perms, &v->off,
             &v->dev_major, &v->dev_minor, &v->inode, &at) < 7) {
    return false;
  }
  v->lo = (uintptr_t)lo;
  v->hi = (uintptr_t)hi;
  v->shared = perms[3] == 's';
  v->path[0] = '\0';
  if (at > 0) {
    (void)snprintf(v->path, sizeof(v->path), "%s", line + at);
    v->path[strcspn(v->path, "\n")] = '\0';
  }
  return true;
}

/* INVARIANT the discovery rests on: the library maps ONE shared object
 * at one fixed address, the pool lies wholly inside that mapping, and the
 * library's own first page -- where it keeps its allocation cursor and its
 * publication fields -- lies below the pool.  So the object behind the page
 * that holds the pool's base is the pool's backing; rows the kernel split off
 * one mapping are rejoined by requiring the same object at a contiguous
 * offset; and the re-mapping covers exactly [base, base + bytes), never a byte
 * of the library's own state.  Anything else is refused by name rather than
 * guessed around. */
void *arts_fam_strict_remap(void *base, uint64_t bytes) {
  uintptr_t lo = (uintptr_t)base;
  uintptr_t hi = lo + (uintptr_t)bytes;
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) {
    ARTS_ERROR("fam: strict mode cannot read this process's mapping table: %s",
               strerror(errno));
  }
  char line[PATH_MAX + 256];
  struct fam_vma_s head = {0}, v;
  bool found = false;
  uintptr_t covered = 0;
  unsigned long long next_off = 0;
  while (fgets(line, sizeof(line), f)) {
    if (!fam_vma_parse(line, &v)) {
      continue;
    }
    if (!found) {
      if (v.lo <= lo && lo < v.hi) {
        head = v;
        found = true;
        covered = v.hi;
        next_off = v.off + (v.hi - v.lo);
      }
    } else if (covered < hi) {
      if (v.lo != covered || v.inode != head.inode ||
          v.dev_major != head.dev_major || v.dev_minor != head.dev_minor ||
          v.off != next_off || !v.shared) {
        break;
      }
      covered = v.hi;
      next_off = v.off + (v.hi - v.lo);
    }
  }
  (void)fclose(f);
  if (!found || !head.shared || head.inode == 0 || head.path[0] != '/') {
    ARTS_ERROR("fam: strict mode needs the pool at %p to lie in a shared "
               "mapping of a named object, and it does not",
               base);
  }
  if (covered < hi) {
    ARTS_ERROR("fam: strict mode needs the pool [%p, +%llu) to lie in ONE "
               "shared mapping, and the mapping of %s ends at 0x%llx",
               base, (unsigned long long)bytes, head.path,
               (unsigned long long)covered);
  }
  if (head.lo >= lo) {
    ARTS_ERROR("fam: the pool at %p starts on the first page of its mapping, "
               "where the library keeps its own state",
               base);
  }
  int fd = open(head.path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    ARTS_ERROR("fam: strict mode cannot open the pool's backing %s: %s",
               head.path, strerror(errno));
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || (unsigned long long)st.st_ino != head.inode ||
      major(st.st_dev) != head.dev_major ||
      minor(st.st_dev) != head.dev_minor) {
    ARTS_ERROR("fam: %s is no longer the object mapped under the pool",
               head.path);
  }
  off_t off = (off_t)(head.off + (lo - head.lo));
  void *view = fam_strict_views(fd, off, base, bytes);
  (void)close(fd);
  return view;
}

void arts_fam_strict_unmap(void *base, uint64_t bytes) {
  if (base) {
    munmap(base, (size_t)bytes);
  }
  if (g_shared && g_shared != MAP_FAILED) {
    munmap(g_shared, (size_t)g_bytes);
  }
  arts_free(g_held);
  g_private = NULL;
  g_shared = NULL;
  g_held = NULL;
  g_bytes = 0;
  g_lines = 0;
}

/* One feasible eviction of a line this rank still holds and has written: it is
 * what stops a test from passing only because the rank's private copy happened
 * to stay private.  Registered ranges only -- within one of those this rank is
 * the only writer, so the backing has not moved since this rank last reloaded
 * the line and the comparison below means "this rank wrote it", not "the page
 * captured it".  Sampled rather than scanned, so a rank may hold any number of
 * ranges at a fixed cost per flush.
 *
 * INVARIANT: a line is copied out here only while its hold is held, or while
 * an eviction that began under that hold has not finished.  The claim bit is
 * what makes the two one step: deciding on the count and then copying would
 * let the copy land after the range was released, re-fetched and rewritten by
 * a peer, putting this rank's stale private line over the peer's published
 * bytes -- and the content gate would read exactly that peer's write as
 * "dirty".  An unhold that finds the claim set waits for it; an evictor never
 * waits, so the two cannot cycle.
 *
 * The same claim excludes the opposite-direction copy: a reload is fatal over
 * any line that is held OR carries a claim, and an eviction touches nothing
 * else, so no line is ever copied both ways at once. */
static void fam_random_write_back(void) {
  if (!fam_strict_live()) {
    return;
  }
  if (!g_seed_ready) {
    /* A pure function of (rank, thread, the named base), and of the thread's
     * own index rather than its position within its role, so a worker and the
     * progress thread of one rank draw different streams.  It makes each
     * THREAD's probe sequence replayable; which thread services a given flush
     * is scheduling, so a rank's eviction interleaving is not. */
    g_seed = g_seed_base * 2654435761u +
             (unsigned)arts_global_rank_id * 2246822519u +
             arts_thread_info.thread_id * 3266489917u + 1u;
    g_seed_ready = true;
  }
  for (unsigned i = 0; i < FAM_STRICT_PROBES; i++) {
    uint64_t line = (uint64_t)rand_r(&g_seed) % g_lines;
    uint16_t h = atomic_load_explicit(&g_held[line], memory_order_relaxed);
    if ((h & FAM_HELD_COUNT) == 0u || (h & FAM_HELD_CLAIM) != 0u) {
      continue;
    }
    if (!atomic_compare_exchange_strong_explicit(
            &g_held[line], &h, (uint16_t)(h | FAM_HELD_CLAIM),
            memory_order_acquire, memory_order_relaxed)) {
      continue; /* the hold moved, or another thread claimed it first */
    }
    /* A flush of a clean line writes nothing back, so a line equal to the
     * backing is passed over and the claim released again. */
    bool dirty = memcmp(fam_priv(line), fam_back(line), ARTS_FAM_GRANULE) != 0;
    if (dirty) {
      fam_write_back(line);
    }
    atomic_fetch_and_explicit(&g_held[line], (uint16_t)~FAM_HELD_CLAIM,
                              memory_order_release);
    if (dirty) {
      return;
    }
  }
}

/* The producer sweep's copy-out, taken under the SAME claim an eviction takes,
 * so that the two copy-outs of one line exclude each other instead of
 * interleaving: an eviction that read the line before the range's last write
 * must not land its bytes after the sweep has published them.
 *
 * The claim is taken whatever the count reads.  A count of zero does not mean
 * no eviction is inside the line -- an unhold drops the count and only then
 * waits the claim out -- so a count test would have to read the claim bit as
 * well, and one rule for both copy-outs is the rule that stays true.
 *
 * Waiting here cannot cycle: an evictor never waits, and a claim is held
 * across one 64-byte copy and nothing else. */
static void fam_write_back_claimed(uint64_t line) {
  uint16_t h = atomic_load_explicit(&g_held[line], memory_order_relaxed);
  for (;;) {
    if ((h & FAM_HELD_CLAIM) != 0u) {
      arts_runtime_idle_pause();
      h = atomic_load_explicit(&g_held[line], memory_order_relaxed);
      continue;
    }
    if (atomic_compare_exchange_weak_explicit(
            &g_held[line], &h, (uint16_t)(h | FAM_HELD_CLAIM),
            memory_order_acquire, memory_order_relaxed)) {
      break; /* h carries the current word on failure, so the retry re-tests */
    }
  }
  fam_write_back(line);
  atomic_fetch_and_explicit(&g_held[line], (uint16_t)~FAM_HELD_CLAIM,
                            memory_order_release);
}

void arts_fam_strict_flush(const void *p, size_t bytes, bool producer) {
  if (!fam_strict_live()) {
    return;
  }
  /* An empty range moves nothing but still fences, as the contract promises.
   * The range's last byte is only a line index once there is one. */
  if (bytes) {
    uint64_t first = fam_line_of(p);
    uint64_t last = fam_line_of((const unsigned char *)p + bytes - 1u);
    if (producer) {
      /* The caller is declaring it wrote this range; that is what a producer
       * flush means, and it stands after the last write. */
      for (uint64_t l = first; l <= last && l < g_lines; l++) {
        fam_write_back_claimed(l);
      }
    } else {
      /* RELOAD ONLY.  A consumer flush is performed by a rank that did not
       * write the range, so it has nothing of its own to publish, and anything
       * it could publish is a page-captured byte. */
      for (uint64_t l = first; l <= last && l < g_lines; l++) {
        /* A reload comes BEFORE the hold, never during it: reloading a line
         * this rank holds would throw away a write only this rank has, and it
         * is the one copy that could meet an eviction of the same line head
         * on.  The claim counts as holding it -- a count that has already
         * fallen to zero still has an eviction finishing under it, which is
         * exactly the copy a reload must not meet. */
        if ((atomic_load_explicit(&g_held[l], memory_order_acquire) &
             (FAM_HELD_COUNT | FAM_HELD_CLAIM)) != 0u) {
          ARTS_ERROR("fam: consumer flush of line %llu, which this rank holds",
                     (unsigned long long)l);
        }
        fam_reload(l);
      }
    }
  }
  fam_random_write_back();
  if (producer) {
    __asm__ __volatile__("sfence" ::: "memory");
  } else {
    __asm__ __volatile__("mfence" ::: "memory");
  }
}

void arts_fam_strict_poison(void *p, size_t bytes) {
  if (!fam_strict_live() || !bytes) {
    return;
  }
  uint64_t first = fam_line_of(p);
  uint64_t last = fam_line_of((unsigned char *)p + bytes - 1u);
  /* Both views: a fresh block is unwritten everywhere, so a read before the
   * first write reads this byte on every rank.  Writing the backing here is
   * safe because a block is allocated before anyone can need it. */
  for (uint64_t l = first; l <= last && l < g_lines; l++) {
    memset(fam_priv(l), (int)ARTS_FAM_POISON_BYTE, ARTS_FAM_GRANULE);
    memset(fam_back(l), (int)ARTS_FAM_POISON_BYTE, ARTS_FAM_GRANULE);
  }
  __asm__ __volatile__("sfence" ::: "memory");
}

/* A count above 1 can only come from the arm registering one range twice,
 * since slots are 64-byte aligned and disjoint.  An unhold that finds 0 is an
 * arm defect -- a range released twice, or never registered -- and it is named
 * rather than absorbed: absorbing it is how a line stays "held" forever and
 * becomes eligible for an eviction the rank has no right to.
 *
 * An unhold leaves only once no eviction is running under the hold it just
 * dropped, which is what makes the claim a hold's extension rather than a race
 * with it. */
void arts_fam_strict_hold_range(const void *p, size_t bytes, bool hold) {
  if (!fam_strict_live() || !bytes) {
    return;
  }
  uint64_t first = fam_line_of(p);
  uint64_t last = fam_line_of((const unsigned char *)p + bytes - 1u);
  for (uint64_t l = first; l <= last && l < g_lines; l++) {
    if (hold) {
      /* Release, so that the reload this hold was taken after is visible to
       * whichever thread later claims the line: the claim's acquire pairs
       * with this store, and without the pair an evictor could compare a line
       * against bytes it has not yet seen reloaded. */
      uint16_t prev = atomic_fetch_add_explicit(&g_held[l], (uint16_t)1,
                                                memory_order_release);
      if ((prev & FAM_HELD_COUNT) == FAM_HELD_COUNT) {
        ARTS_ERROR("fam: line %llu is held %u times, which is as many as the "
                   "count can carry",
                   (unsigned long long)l, (unsigned)FAM_HELD_COUNT);
      }
    } else {
      uint16_t prev = atomic_fetch_sub_explicit(&g_held[l], (uint16_t)1,
                                                memory_order_acq_rel);
      if ((prev & FAM_HELD_COUNT) == 0u) {
        ARTS_ERROR("fam: unhold of line %llu, which this rank does not hold",
                   (unsigned long long)l);
      }
      while ((atomic_load_explicit(&g_held[l], memory_order_acquire) &
              FAM_HELD_CLAIM) != 0u) {
        arts_runtime_idle_pause();
      }
    }
  }
}

bool arts_fam_strict_range_held(const void *p, size_t bytes) {
  if (!fam_strict_live() || !bytes) {
    return false;
  }
  uint64_t first = fam_line_of(p);
  uint64_t last = fam_line_of((const unsigned char *)p + bytes - 1u);
  for (uint64_t l = first; l <= last && l < g_lines; l++) {
    if ((atomic_load_explicit(&g_held[l], memory_order_acquire) &
         FAM_HELD_COUNT) != 0u) {
      return true;
    }
  }
  return false;
}
