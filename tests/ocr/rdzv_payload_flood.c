/* SPDX-License-Identifier: Apache-2.0
 *
 * rdzv_payload_flood — one-sided payloads saturating both directions at once.
 *
 * Every payload a data block moves between ranks is a PUT into a landing its
 * receiver advertised, paired on the receiver with the metadata message that
 * names it (the rendezvous pairing), and with the provider's one-sided
 * ceiling lowered each payload leaves as several pieces paired on the last.
 * This program keeps many such transfers in flight in both directions while
 * the senders run into backpressure: blocks homed on either rank are each
 * written in turn by a chain of EDTs alternating between the ranks, every
 * chain running at once, so each hop moves the whole payload across.
 *
 * Each writer checks every element against the version the previous writer
 * stamped, then stamps the next one; the last of each chain checks the final
 * version.  A payload that lands partly, at the wrong offset, or is paired
 * with another transfer's metadata shows as a mismatch (FAIL and an abort);
 * a lost pairing leaves a chain waiting and the run times out.  Requires two
 * or more ranks.
 */
#include "arts.h"

#include <stdint.h>

#define N_DBS 128
#define DB_INTS (12u * 1024u) /* 48 KiB per block */
#define HOPS 4                /* writers per chain after the initial stamp */

static inline uint32_t pattern(uint32_t d, uint32_t j, uint32_t v) {
  return (d * 2654435761u) ^ (j * 40503u) ^ (v * 0x9e3779b9u);
}

static void check(const uint32_t *data, uint32_t d, uint32_t v) {
  if (data == NULL) {
    arts_printf("FAIL: block %u reached a writer with no payload\n", d);
    arts_abort(1);
  }
  for (uint32_t j = 0; j < DB_INTS; j++) {
    if (data[j] != pattern(d, j, v)) {
      arts_printf("FAIL: block %u element %u holds 0x%x, expected version %u "
                  "(0x%x)\n",
                  d, j, data[j], v, pattern(d, j, v));
      arts_abort(1);
    }
  }
}

/* paramv: block index, version this writer stamps, the block's GUID, the
 * block's home.  Each hop joins the work scope by inheritance. */
static void hop_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                    arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint32_t d = (uint32_t)paramv[0];
  uint32_t v = (uint32_t)paramv[1];
  uint32_t *data = (uint32_t *)depv[0].ptr;
  check(data, d, v - 1u);
  if (v > HOPS) {
    return; /* the chain's last hop only checks */
  }
  for (uint32_t j = 0; j < DB_INTS; j++) {
    data[j] = pattern(d, j, v);
  }
  unsigned int nranks = arts_get_total_ranks();
  uint64_t next[4] = {d, v + 1u, paramv[2], paramv[3]};
  unsigned int rank = (unsigned int)((paramv[3] + v + 1u) % nranks);
  arts_guid_t e =
      arts_edt_create(hop_edt, 4, next, 1, &(arts_edt_hint_t){.rank = rank});
  arts_add_dependence((arts_guid_t)paramv[2], e, 0, DB_MODE_RW);
}

static void init_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint32_t d = (uint32_t)paramv[0];
  uint32_t *data = (uint32_t *)depv[0].ptr;
  for (uint32_t j = 0; j < DB_INTS; j++) {
    data[j] = pattern(d, j, 0);
  }
}

static void done_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                     arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("PASS: rdzv_payload_flood %d blocks x %d cross-rank hops "
              "verified\n",
              N_DBS, HOPS);
  arts_shutdown();
}

/* Gated on the stamps: starts every chain at once. */
static void start_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)depc;
  (void)depv;
  unsigned int nranks = arts_get_total_ranks();
  arts_guid_t done =
      arts_edt_create(done_edt, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  arts_guid_t work = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(work, done, 0, DB_MODE_NULL);
  for (uint32_t d = 0; d < paramc; d++) {
    uint64_t home = d % nranks;
    uint64_t pv[4] = {d, 1u, paramv[d], home};
    arts_guid_t e = arts_edt_create(
        hop_edt, 4, pv, 1,
        &(arts_edt_hint_t){.rank = (unsigned int)((home + 1u) % nranks),
                           .finish_event = work});
    arts_add_dependence((arts_guid_t)paramv[d], e, 0, DB_MODE_RW);
  }
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int nranks = arts_get_total_ranks();
  if (nranks < 2) {
    arts_printf("FAIL: rdzv_payload_flood needs two or more ranks\n");
    arts_abort(1);
  }
  uint64_t dbs[N_DBS];
  for (uint32_t d = 0; d < N_DBS; d++) {
    void *none = NULL;
    dbs[d] = (uint64_t)arts_db_create(&none, DB_INTS * sizeof(uint32_t),
                                      ARTS_DB, ARTS_DB_PROP_NO_ACQUIRE,
                                      &(arts_db_hint_t){.rank = d % nranks});
  }
  arts_guid_t start =
      arts_edt_create(start_edt, N_DBS, dbs, 1, &(arts_edt_hint_t){.rank = 0});
  arts_guid_t stamps = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_add_dependence(stamps, start, 0, DB_MODE_NULL);
  for (uint32_t d = 0; d < N_DBS; d++) {
    uint64_t pv[1] = {d};
    arts_guid_t e = arts_edt_create(
        init_edt, 1, pv, 1,
        &(arts_edt_hint_t){.rank = d % nranks, .finish_event = stamps});
    arts_add_dependence((arts_guid_t)dbs[d], e, 0, DB_MODE_RW);
  }
}

int main(int argc, char **argv) {
  /* Non-zero when a rank this process spawned ended badly. */
  return arts_rt(argc, argv) != 0 ? 1 : 0;
}
