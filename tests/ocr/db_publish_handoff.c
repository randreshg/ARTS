/* SPDX-License-Identifier: Apache-2.0
 *
 * arts_db_publish hands a block's writes to readers while the publisher
 * keeps writing it, for both kinds of write hold: an RW dependence (dep) and
 * the hold of the publisher's own create (created).
 *
 * P, on rank 0, holds D (512 words: range A, then range B) for writing and
 * runs three rounds.  In round r it stores val(r) into every word of A,
 * publishes, checks that its own view did not change (under dep its slot
 * keeps its guid, ptr and mode; the bytes are what it stored), and makes T_r
 * ready on the readers' rank with an RO dependence on D and the latch E_r as
 * its completion event.  T_r checks that A is val(r) and B is OLD.  E_r
 * makes G_r ready on P's rank, and G_r raises a counter in that rank's
 * memory, which P waits for before its next store: no EDT stores into D while
 * a T_r holds it (an EDT releases its holds before its completion event
 * fires), so every access to D is a plain load or store and no store
 * overlaps the copy that serves a reader.  After the last round P stores
 * LATE into every word of B and ends.  An end EDT, ordered after every EDT of
 * the run (P's release among them), reads D: A is val(2), what P published
 * last, and B is LATE, which only P's release published.
 *
 * Arguments: dep or created; the block's home (self: P's rank; peer: rank 1
 * when there is one; third: rank 2, refused on fewer than three ranks); where
 * the readers run (far: rank 1 when there is one; near: P's rank).  Under dep,
 * rank 0's main EDT creates D, fills it with OLD and releases it, and M, on
 * the readers' rank, reads OLD before P acquires D; under validation and
 * invalidation, where a rank keeps the copy it read, the readers' rank then
 * holds one older than every publication.  Under created, P creates D with
 * the home's hint and stores OLD into B before its first publish.
 *
 * P's wait is legal: a body may run as long as it likes, and nothing P waits
 * for needs its hold to end.  Under validation, invalidation and CDAG a
 * reader on another rank is answered while another rank's writer holds the
 * block.  Under exclusion a reader on another rank is admitted only once no
 * rank holds the write turn, that is at the end of P's hold, so the far
 * shape cannot complete there by design (P's wait for its first reader runs
 * out, a failure) and is not a program for it; the near shape is, its
 * readers sharing P's turn.
 *
 * Three rounds: a runtime that ran a whole release in place of a publish can
 * get through one round where holding the write right counts as one more
 * hold on the rank; the releases of the later rounds use that up.  Where it
 * does, the end EDT's check of B catches such a runtime with two rounds
 * already: P's own release then finds no hold left to release, so LATE is
 * never published.  On one rank every reader shares P's live bytes, so that
 * run exercises the call where nothing is sent: P keeps its hold through
 * three publishes and each reader is served on P's rank while P holds it; a
 * publish that does nothing is not seen there.
 *
 * What the readers cannot tell apart.  Under CDAG with the home on P's rank
 * (self), P writes the home's line in place, so a publish that does nothing
 * and one that ends P's hold leave every reader the same bytes; and there a
 * dependence's release is its write-back and nothing more, so in every dep
 * shape a publish that ran a release reads as a publish.  Under exclusion a
 * publish has nothing to do, so one that does nothing is correct there; one
 * that ran a release still leaves every near reader P's bytes where the rank
 * keeps its ownership and its buffer (retained ownership), and in the near
 * self shape under hand-back, whose bytes go back to the buffer P writes.
 *
 * The verdict is the values; the clock bounds only a failure.  On its first
 * failure P stops its rounds and ends.  The readers it already started are
 * in its group, which the end EDT waits for, so after a deadline the run
 * ends once those readers complete, which P's release lets them do on every
 * arm this program runs on.
 */
#include "arts.h"

#include "../test_failure_status.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define NAME "db_publish_handoff"
#define WORDS 512u
#define WA 256u
#define SZ (WORDS * sizeof(uint64_t))
#define ROUNDS 3u
#define WAIT_MS 60000L

enum { PAT_VAL, PAT_OLD, PAT_LATE };

static const char *const kind_name[2] = {"dep", "created"};
static const char *const home_name[3] = {"self", "peer", "third"};
static const char *const reader_name[2] = {"far", "near"};

static long now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static uint64_t pattern(int pat, unsigned int r, unsigned int j) {
  switch (pat) {
  case PAT_VAL:
    return ((uint64_t)(0xA0u + r) << 40) | ((uint64_t)r << 32) | j;
  case PAT_OLD:
    return 0x01D000000000ull | j;
  default:
    return 0x1A7E00000000ull | j;
  }
}

static void fill(uint64_t *p, unsigned int from, unsigned int to, int pat,
                 unsigned int r) {
  for (unsigned int j = from; j < to; j++) {
    p[j] = pattern(pat, r, j);
  }
}

/* 1 when words [from, to) of p hold the pattern, else a FAIL line and 0. */
static int expect(const uint64_t *p, unsigned int from, unsigned int to,
                  int pat, unsigned int r, const char *range, const char *who) {
  if (p == NULL) {
    printf("FAIL: " NAME " %s has no pointer\n", who);
    arts_test_fail();
    return 0;
  }
  for (unsigned int j = from; j < to; j++) {
    if (p[j] != pattern(pat, r, j)) {
      printf("FAIL: " NAME " %s: %s word %u is 0x%llx, expected 0x%llx\n", who,
             range, j - from, (unsigned long long)p[j],
             (unsigned long long)pattern(pat, r, j));
      arts_test_fail();
      return 0;
    }
  }
  return 1;
}

/* The rounds whose reader has completed: raised only by G_r (release), read
 * only by P (acquire), both on P's rank. */
static _Atomic(uint32_t) g_rounds_read;

/* Waits until `want` rounds' readers have completed; 0 at the deadline. */
static int await_rounds(uint32_t want) {
  long start = now_ms();
  while (atomic_load_explicit(&g_rounds_read, memory_order_acquire) < want) {
    if (now_ms() - start > WAIT_MS) {
      printf("FAIL: " NAME " P: the reader of round %u never completed\n",
             want - 1u);
      arts_test_fail();
      return 0;
    }
  }
  return 1;
}

/* depv[0]: D, RO. */
static void m_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)expect((const uint64_t *)depv[0].ptr, 0, WORDS, PAT_OLD, 0, "A and B",
               "M");
}

/* paramv[0]: the round.  depv[0]: D, RO. */
static void t_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int r = (unsigned int)paramv[0];
  char who[16];
  snprintf(who, sizeof(who), "T_%u", r);
  const uint64_t *p = (const uint64_t *)depv[0].ptr;
  if (expect(p, 0, WA, PAT_VAL, r, "A", who)) {
    (void)expect(p, WA, WORDS, PAT_OLD, 0, "B", who);
  }
}

/* paramv[0]: the round.  depv[0]: E_r, T_r's completion. */
static void g_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  atomic_store_explicit(&g_rounds_read, (uint32_t)paramv[0] + 1u,
                        memory_order_release);
}

/* paramv: created (1) or dep (0), D's home, the readers' rank, the end EDT.
 * Under dep, depv[0]: D, RW; depv[1]: M's completion. */
static void p_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                  arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  int created = paramv[0] != 0u;
  unsigned int home = (unsigned int)paramv[1];
  unsigned int readers = (unsigned int)paramv[2];
  arts_guid_t end = (arts_guid_t)paramv[3];
  arts_guid_t d;
  uint64_t *p;
  arts_edt_dep_t held = {0};
  if (created) {
    arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
    h.rank = home;
    void *vp = NULL;
    d = arts_db_create(&vp, SZ, ARTS_DB, ARTS_DB_PROP_NONE, &h);
    p = (uint64_t *)vp;
  } else {
    held = depv[0];
    d = held.guid;
    p = (uint64_t *)held.ptr;
  }
  int ok = 1;
  if (p == NULL || (!created && held.mode != DB_MODE_RW)) {
    printf("FAIL: " NAME " P: its hold came without a pointer or in mode %d\n",
           (int)held.mode);
    arts_test_fail();
    ok = 0;
  } else if (created) {
    fill(p, WA, WORDS, PAT_OLD, 0);
  }

  for (unsigned int r = 0; ok && r < ROUNDS; r++) {
    fill(p, 0, WA, PAT_VAL, r);
    arts_db_publish(d);
    if (!created && (depv[0].guid != held.guid || depv[0].ptr != held.ptr ||
                     depv[0].mode != held.mode)) {
      printf("FAIL: " NAME " P: after publish %u slot 0 reads guid %llu ptr %p "
             "mode %d, held guid %llu ptr %p mode %d\n",
             r, (unsigned long long)depv[0].guid, depv[0].ptr,
             (int)depv[0].mode, (unsigned long long)held.guid, held.ptr,
             (int)held.mode);
      arts_test_fail();
      ok = 0;
      break;
    }
    if (!expect(p, 0, WA, PAT_VAL, r, "A", "P after its publish") ||
        !expect(p, WA, WORDS, PAT_OLD, 0, "B", "P after its publish")) {
      ok = 0;
      break;
    }
    uint64_t rp = r;
    arts_event_hint_t eh = ARTS_EVENT_HINT_LATCH(1);
    arts_guid_t e = arts_event_create(&eh);
    arts_guid_t g =
        arts_edt_create(g_edt, 1, &rp, 1,
                        &(arts_edt_hint_t){.rank = arts_get_current_rank()});
    arts_add_dependence(e, g, 0, DB_MODE_NULL);
    arts_guid_t t = arts_edt_create(
        t_edt, 1, &rp, 1,
        &(arts_edt_hint_t){.rank = readers, .completion_event = e});
    arts_add_dependence(d, t, 0, DB_MODE_RO);
    ok = await_rounds(r + 1u);
  }

  if (ok) {
    fill(p, WA, WORDS, PAT_LATE, 0);
  }
  arts_add_dependence(d, end, 1, DB_MODE_RO);
}

/* paramv: the three arguments' indices.  depv[0]: the group of every other
 * EDT, P included; depv[1]: D, RO. */
static void end_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const uint64_t *p = (const uint64_t *)depv[1].ptr;
  if (expect(p, 0, WA, PAT_VAL, ROUNDS - 1u, "A", "the end EDT")) {
    (void)expect(p, WA, WORDS, PAT_LATE, 0, "B", "the end EDT");
  }
  if (depv[1].guid != NULL_GUID) {
    arts_db_destroy(depv[1].guid);
  }
  if (arts_test_status() == 0) {
    printf("PASS: " NAME " %s %s %s\n", kind_name[paramv[0]],
           home_name[paramv[1]], reader_name[paramv[2]]);
  }
  arts_shutdown();
}

static int pick(const char *arg, const char *const *names, unsigned int n,
                unsigned int *out) {
  for (unsigned int i = 0; arg != NULL && i < n; i++) {
    if (strcmp(arg, names[i]) == 0) {
      *out = i;
      return 1;
    }
  }
  return 0;
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  int argc = paramc > 0u ? (int)paramv[0] : 0;
  char **argv = paramc > 1u ? (char **)paramv[1] : NULL;
  unsigned int kind = 0, hsel = 0, rsel = 0;
  if (argc != 4 || !pick(argv[1], kind_name, 2, &kind) ||
      !pick(argv[2], home_name, 3, &hsel) ||
      !pick(argv[3], reader_name, 2, &rsel)) {
    printf("FAIL: " NAME " give dep|created self|peer|third far|near\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  unsigned int n = arts_get_total_ranks();
  if (hsel == 2u && n < 3u) {
    printf("FAIL: " NAME " a home on a third rank needs three ranks, ran on "
           "%u\n",
           n);
    arts_test_fail();
    arts_shutdown();
    return;
  }
  unsigned int one = n > 1u ? 1u : 0u;
  unsigned int home = hsel == 0u ? 0u : hsel == 1u ? one : 2u;
  unsigned int readers = rsel == 0u ? one : 0u;

  arts_event_hint_t gh = ARTS_EVENT_HINT_LATCH(1);
  gh.auto_destroy = true;
  arts_guid_t group = arts_event_create(&gh);

  uint64_t eparam[3] = {kind, hsel, rsel};
  arts_guid_t end =
      arts_edt_create(end_edt, 3, eparam, 2, &(arts_edt_hint_t){.rank = one});
  arts_add_dependence(group, end, 0, DB_MODE_NULL);

  uint64_t pparam[4] = {kind, home, readers, (uint64_t)end};
  if (kind == 1u) {
    arts_edt_create(p_edt, 4, pparam, 0,
                    &(arts_edt_hint_t){.rank = 0u, .group = group});
  } else {
    arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
    h.rank = home;
    void *vp = NULL;
    arts_guid_t d = arts_db_create(&vp, SZ, ARTS_DB, ARTS_DB_PROP_NONE, &h);
    if (vp != NULL) {
      fill((uint64_t *)vp, 0, WORDS, PAT_OLD, 0);
    }
    arts_db_release(d);
    arts_event_hint_t ch = ARTS_EVENT_HINT_LATCH(1);
    arts_guid_t m_done = arts_event_create(&ch);
    arts_guid_t m = arts_edt_create(
        m_edt, 0, NULL, 1,
        &(arts_edt_hint_t){
            .rank = readers, .group = group, .completion_event = m_done});
    arts_add_dependence(d, m, 0, DB_MODE_RO);
    arts_guid_t pe = arts_edt_create(
        p_edt, 4, pparam, 2, &(arts_edt_hint_t){.rank = 0u, .group = group});
    arts_add_dependence(d, pe, 0, DB_MODE_RW);
    arts_add_dependence(m_done, pe, 1, DB_MODE_NULL);
  }
  arts_event_satisfy_slot(group, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);
}

int main(int argc, char **argv) {
  return arts_rt(argc, argv);
}
