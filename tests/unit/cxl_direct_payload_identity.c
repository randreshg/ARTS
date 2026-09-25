/* SPDX-License-Identifier: Apache-2.0
 *
 * Under direct residency an EDT's working bytes ARE the block's store:
 *
 *   1. every holder's pointer lies inside the store (arts_cxl_store_contains),
 *      and the creator's pointer and the next holder's are the same address --
 *      a staged arm hands each turn its own rank-local copy, so any copy in
 *      the path changes the address;
 *   2. that address is the same on every RANK that holds it -- the no-copy
 *      oracle, since a staged arm hands each rank its own working copy and a
 *      copy anywhere in the path changes the address;
 *   3. what a holder writes through that pointer is what the next holder
 *      reads, so the identity is not identity of an unused pointer.
 *
 * Property 2 needs more than one rank, so the multinode registration adds one
 * carrier stage per additional rank.  A rank is a separate process, so a
 * predecessor's resolved pointer is not otherwise visible, and it has to
 * arrive in an order the PROGRAM states rather than one a protocol's
 * admission rule happens to enforce today: each carrier's incoming pointer
 * is delivered through the previous stage's OUTPUT EVENT
 * (arts_edt_set_result), which the runtime fires only after that stage's own
 * data-block release -- so the value compared can never be one a release has
 * not yet published.  A carrier compares that delivered value against its
 * own DB_MODE_RO acquisition of the block and re-checks the sentinel a stale
 * copy would not carry.
 *
 * A second value threads the same path -- a plain running count -- and needs
 * release ordering only where it feeds the READER's own gate, never between
 * two carriers: a next carrier cannot become ready before its predecessor's
 * OUTPUT EVENT delivers the pointer (depv[2]), and that event fires only
 * after the predecessor's release, so an intermediate hop's count can travel
 * by a direct slot satisfy the instant its body computes it -- arriving
 * early gates nothing.  The reader compares no carried value, though, so
 * nothing but this count gates it; on every rank count the stage immediately
 * before the reader (the writer alone on one rank, otherwise the last
 * carrier) delivers it through ITS OWN output event too, so the reader's
 * readiness never rides on a protocol's admission rule standing in for a
 * signal the program never actually sent.  The count itself is how many
 * carriers ran, and the reader requires it equal nranks - 1: a chain that
 * silently ran zero carriers reds here instead of passing on property 1
 * alone.
 *
 * With one rank there are no carriers and the writer is the stage immediately
 * before the reader, matching nranks - 1 == 0: the chain collapses to a
 * single release-ordered writer -> reader hand-off with nothing lost.
 *
 * The reader names the block TWICE, so one of its slots is an alias of the
 * other's acquisition: a second slot filled from the first's resolved pointer
 * rather than by an acquisition of its own.  Both slots must carry the same
 * address as the creator's, which is what puts the alias fill and its release
 * on this program's path as well as the ordinary acquire.
 */
#include "arts.h"
#include "arts/cxl/store.h"

#include "../test_failure_status.h"

#include <stdio.h>

#define WORDS 64u
#define SENTINEL 0x5A5AA5A5u
/* Bounds the carrier array (ranks - 1). */
#define MAX_HOPS 15u

static arts_guid_t g_db;
static uint64_t g_first_ptr;

/* depv[0] = the block, RW.  paramv[0] = the GUID of the next stage (the
 * first carrier, or the reader directly on one rank).  paramv[1] = nonzero
 * when that next stage IS the reader, in which case this EDT's own output
 * event carries the hop count (0) instead of the pointer -- nothing
 * downstream of the reader ever reads a carried pointer. */
static void writer_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  unsigned int *d = (unsigned int *)depv[0].ptr;
  if (d == NULL || !arts_cxl_store_contains(d)) {
    (void)fprintf(stderr, "FAIL cxl_direct_payload_identity: a holder's "
                          "pointer is not in the store\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  if ((uint64_t)(uintptr_t)d != g_first_ptr) {
    (void)fprintf(stderr,
                  "FAIL cxl_direct_payload_identity: the holder's pointer "
                  "%llx is not the creator's %llx\n",
                  (unsigned long long)(uintptr_t)d,
                  (unsigned long long)g_first_ptr);
    arts_test_fail();
    arts_shutdown();
    return;
  }
  for (unsigned int i = 0; i < WORDS; i++) {
    d[i] = SENTINEL ^ i;
  }
  if (paramv[1] != 0u) {
    /* One rank: the reader's only incoming edge, so it has to be the one
     * that is release-ordered by the API's own contract, not by however the
     * coherence engine happens to admit its own DB_MODE_RO acquisition. */
    arts_edt_set_result((arts_guid_t)0);
  } else {
    /* Delivered only after this EDT's own release: the next carrier's
     * carried-pointer slot cannot see this before the write is published. */
    arts_edt_set_result((arts_guid_t)(uintptr_t)d);
    arts_edt_satisfy_slot((arts_guid_t)paramv[0], 1, (arts_guid_t)0,
                          DB_MODE_NULL);
  }
}

/* depv[0] = the block, RO; depv[1] = the running hop count (a plain forward
 * from a carrier that is not immediately before the reader, since the next
 * carrier's readiness is also gated by this carrier's output event, which
 * fires only after this carrier's release); depv[2] = the previous stage's
 * own resolved pointer, delivered through that stage's output event (so only
 * after its release).
 * paramv[0] = the GUID of the next stage.  paramv[1] = nonzero when that next
 * stage IS the reader, in which case this carrier is the last one and its
 * own output event carries the hop count instead of a pointer -- nothing
 * downstream of the reader ever reads a carried pointer. */
static void carrier_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                        arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const unsigned int *d = (const unsigned int *)depv[0].ptr;
  uint64_t prev = (uint64_t)depv[2].guid;
  if (d == NULL || !arts_cxl_store_contains(d)) {
    (void)fprintf(stderr, "FAIL cxl_direct_payload_identity: a holder's "
                          "pointer is not in the store\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  if ((uint64_t)(uintptr_t)d != prev) {
    (void)fprintf(stderr,
                  "FAIL cxl_direct_payload_identity: rank %u's pointer %llx "
                  "is not the previous holder's %llx\n",
                  arts_get_current_rank(), (unsigned long long)(uintptr_t)d,
                  (unsigned long long)prev);
    arts_test_fail();
    arts_shutdown();
    return;
  }
  for (unsigned int i = 0; i < WORDS; i++) {
    if (d[i] != (SENTINEL ^ i)) {
      (void)fprintf(stderr,
                    "FAIL cxl_direct_payload_identity: rank %u sees stale "
                    "word %u = 0x%x\n",
                    arts_get_current_rank(), i, d[i]);
      arts_test_fail();
      arts_shutdown();
      return;
    }
  }
  uint64_t count = (uint64_t)depv[1].guid + 1u;
  if (paramv[1] != 0u) {
    /* Last carrier: the reader's only incoming edge from this chain, so it
     * travels through this stage's own output event exactly like a
     * carried pointer would, never by a mid-body satisfy. */
    arts_edt_set_result((arts_guid_t)count);
  } else {
    arts_edt_set_result((arts_guid_t)(uintptr_t)d);
    arts_edt_satisfy_slot((arts_guid_t)paramv[0], 1, (arts_guid_t)count,
                          DB_MODE_NULL);
  }
}

static void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                       arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  const unsigned int *d = (const unsigned int *)depv[0].ptr;
  if (d == NULL || !arts_cxl_store_contains(d) ||
      (uint64_t)(uintptr_t)d != g_first_ptr) {
    (void)fprintf(stderr, "FAIL cxl_direct_payload_identity: the reader's "
                          "pointer is not the block's slot\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  if (depv[2].ptr != depv[0].ptr || !arts_cxl_store_contains(depv[2].ptr)) {
    (void)fprintf(stderr, "FAIL cxl_direct_payload_identity: a second slot "
                          "naming the block was handed %p, not the block's "
                          "slot %p\n",
                  depv[2].ptr, depv[0].ptr);
    arts_test_fail();
    arts_shutdown();
    return;
  }
  uint64_t hops = (uint64_t)depv[1].guid;
  uint64_t want = (uint64_t)arts_get_total_ranks() - 1u;
  if (hops != want) {
    (void)fprintf(stderr,
                  "FAIL cxl_direct_payload_identity: the chain ran %llu "
                  "carrier(s), not the %llu the rank count implies\n",
                  (unsigned long long)hops, (unsigned long long)want);
    arts_test_fail();
    arts_shutdown();
    return;
  }
  for (unsigned int i = 0; i < WORDS; i++) {
    if (d[i] != (SENTINEL ^ i)) {
      (void)fprintf(stderr,
                    "FAIL cxl_direct_payload_identity: word %u reads 0x%x\n", i,
                    d[i]);
      arts_test_fail();
      arts_shutdown();
      return;
    }
  }
  printf("PASS cxl_direct_payload_identity\n");
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  void *p = NULL;
  g_db = arts_db_create(&p, WORDS * sizeof(unsigned int), ARTS_DB,
                        ARTS_DB_PROP_NONE, NULL);
  if (p == NULL || !arts_cxl_store_contains(p)) {
    (void)fprintf(stderr, "FAIL cxl_direct_payload_identity: the creator's "
                          "pointer is not in the store\n");
    arts_test_fail();
    arts_shutdown();
    return;
  }
  g_first_ptr = (uint64_t)(uintptr_t)p;
  arts_db_release(g_db, DB_MODE_RW);

  unsigned int nranks = arts_get_total_ranks();
  if (nranks - 1u > MAX_HOPS) {
    (void)fprintf(stderr, "FAIL cxl_direct_payload_identity: built for at "
                          "most %u carriers (have %u ranks)\n",
                  MAX_HOPS, nranks);
    arts_test_fail();
    arts_shutdown();
    return;
  }

  /* The reader reads g_first_ptr directly (this process's own static), so it
   * stays pinned to this rank rather than left to no-hint placement. */
  arts_edt_hint_t reader_hint = {.rank = 0};
  arts_guid_t r = arts_edt_create(reader_edt, 0, NULL, 3, &reader_hint);

  /* Create the carrier chain backwards, one stage per rank beyond this one,
   * from the rank closest to the reader down to rank 1: each stage's paramv
   * names the GUID it must hand its own hop count to once it runs, which has
   * to already exist by the time an earlier stage is created.  Every stage
   * gets its own output event -- every non-tail carrier's carries its
   * pointer to the next carrier; the tail's carries the hop count to the
   * reader instead (wired here, since the tail is only ever visited once,
   * on this loop's first iteration), because nothing downstream of the
   * reader ever reads the tail's own pointer. */
  arts_guid_t hop_guid[MAX_HOPS];
  arts_guid_t hop_out[MAX_HOPS];
  arts_guid_t next_stage = r;
  for (unsigned int rank = nranks - 1; rank >= 1; rank--) {
    bool is_tail = (rank == nranks - 1u);
    arts_guid_t out_ev = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
    arts_edt_hint_t hh = {.rank = rank, .output_event = out_ev};
    uint64_t hp[2] = {(uint64_t)next_stage, is_tail ? 1u : 0u};
    arts_guid_t hop = arts_edt_create(carrier_edt, 2, hp, 3, &hh);
    arts_add_dependence(g_db, hop, 0, DB_MODE_RO);
    hop_guid[rank - 1u] = hop;
    hop_out[rank - 1u] = out_ev;
    if (is_tail) {
      arts_add_dependence(out_ev, r, 1, DB_MODE_NULL);
    }
    next_stage = hop;
  }

  /* On one rank the writer is the stage immediately before the reader, so
   * its own output event carries the hop count (0) there instead of a
   * pointer, exactly like a tail carrier would. */
  bool writer_next_is_reader = (nranks == 1u);
  arts_guid_t w_out = arts_event_create(&ARTS_EVENT_HINT_LATCH(1));
  arts_edt_hint_t wh = {.rank = 0, .output_event = w_out};
  uint64_t wp[2] = {(uint64_t)next_stage, writer_next_is_reader ? 1u : 0u};
  arts_guid_t w = arts_edt_create(writer_edt, 2, wp, 1, &wh);
  arts_add_dependence(g_db, w, 0, DB_MODE_RW);
  if (writer_next_is_reader) {
    arts_add_dependence(w_out, r, 1, DB_MODE_NULL);
  }

  /* Wire each non-tail stage's own output event to the next stage's
   * carried-pointer slot -- forward order, since a stage's output event
   * does not exist until that stage has been created above.  0 iterations
   * on one rank, where w_out was just wired to the reader's count slot
   * instead. */
  arts_guid_t prev_out = w_out;
  for (unsigned int rank = 1u; rank < nranks; rank++) {
    arts_add_dependence(prev_out, hop_guid[rank - 1u], 2, DB_MODE_NULL);
    prev_out = hop_out[rank - 1u];
  }

  arts_add_dependence(g_db, r, 0, DB_MODE_RO);
  arts_add_dependence(g_db, r, 2, DB_MODE_RO);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
