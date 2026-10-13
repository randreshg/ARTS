/******************************************************************************
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/* coherence_late_write_completion — blocks destroyed while the one-sided
 * writes that moved their payloads may not have completed locally.
 *
 * Each block is homed round-robin.  Its creator stamps it and releases it; one
 * writer on another rank checks the stamp and writes the block's final
 * contents; readers on other ranks, ordered after the writer, check those
 * contents word for word; the block's home destroys it once every reader has
 * finished.  Where payloads travel through the fabric, every hop moves one
 * with a one-sided write, and the fabric orders nothing between a write's
 * local completion and the messages its target sends afterwards, so a
 * completion may drop the last reference to a payload buffer after the
 * block's descriptor is gone.  Registered with the write-completion delay,
 * which makes that the common case: whatever the late drop touches must
 * still be the buffer's own.
 *
 * The writes to each block are ordered, so every arm that moves payloads
 * through the fabric runs it.  The run ends with every block destroyed and
 * nothing pending, so its shutdown is quiescent.  Needs two ranks; SKIPs on
 * one.
 */

#include <stdint.h>

#include "arts.h"
#include "../test_failure_status.h"

#define BLOCKS 48u
#define READERS 3u
#define WORDS 64u

static uint64_t stamp(uint64_t id, unsigned int w, uint64_t phase) {
  return (id << 24) ^ ((uint64_t)w << 8) ^ phase;
}

static void writer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t *d = (uint64_t *)depv[0].ptr;
  if (d == NULL) {
    arts_printf("FAIL: block %lu reached its writer with no payload\n",
                (unsigned long)paramv[0]);
    arts_test_fail();
    return;
  }
  for (unsigned int w = 0; w < WORDS; w++) {
    if (d[w] != stamp(paramv[0], w, 1)) {
      arts_printf("FAIL: block %lu word %u reached its writer as %lx\n",
                  (unsigned long)paramv[0], w, (unsigned long)d[w]);
      arts_test_fail();
      return;
    }
  }
  for (unsigned int w = 0; w < WORDS; w++) {
    d[w] = stamp(paramv[0], w, 2);
  }
}

static void reader(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  const uint64_t *d = (const uint64_t *)depv[0].ptr;
  if (d == NULL) {
    arts_printf("FAIL: block %lu reached a reader with no payload\n",
                (unsigned long)paramv[0]);
    arts_test_fail();
    return;
  }
  for (unsigned int w = 0; w < WORDS; w++) {
    if (d[w] != stamp(paramv[0], w, 2)) {
      arts_printf("FAIL: block %lu word %u reached a reader as %lx\n",
                  (unsigned long)paramv[0], w, (unsigned long)d[w]);
      arts_test_fail();
      return;
    }
  }
}

static void destroyer(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                      arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  arts_db_destroy((arts_guid_t)paramv[0]);
}

static void finish(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_printf("PASS: coherence_late_write_completion %u blocks\n",
              BLOCKS);
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  unsigned int n = arts_get_total_ranks();
  if (n < 2) {
    arts_printf("SKIP: coherence_late_write_completion needs 2+ ranks\n");
    arts_shutdown();
    return;
  }

  arts_guid_t all = arts_event_create(&ARTS_EVENT_HINT_FINISH);
  arts_guid_t done =
      arts_edt_create(finish, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(all, done, 0, DB_MODE_NULL);

  for (uint64_t id = 0; id < BLOCKS; id++) {
    unsigned int home = (unsigned int)(id % n);
    uint64_t *d = NULL;
    arts_guid_t db = arts_db_create((void **)&d, WORDS * sizeof(uint64_t),
                                    ARTS_DB, ARTS_DB_PROP_NONE,
                                    &(arts_db_hint_t){.rank = home});
    for (unsigned int w = 0; w < WORDS; w++) {
      d[w] = stamp(id, w, 1);
    }
    arts_db_release(db, DB_MODE_RW);

    arts_guid_t written = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    arts_guid_t read = arts_event_create(&ARTS_EVENT_HINT_FINISH);

    arts_guid_t wr = arts_edt_create(
        writer, 1, &id, 1,
        &(arts_edt_hint_t){.rank = (home + 1u) % n, .finish_event = written});
    arts_add_dependence(db, wr, 0, DB_MODE_RW);

    for (unsigned int r = 0; r < READERS; r++) {
      unsigned int rank = (home + 1u + (r % (n - 1u))) % n;
      arts_guid_t rd = arts_edt_create(
          reader, 1, &id, 2,
          &(arts_edt_hint_t){.rank = rank, .finish_event = read});
      arts_add_dependence(db, rd, 0, DB_MODE_RO);
      arts_add_dependence(written, rd, 1, DB_MODE_NULL);
    }

    uint64_t g = (uint64_t)db;
    arts_guid_t ds = arts_edt_create(
        destroyer, 1, &g, 1,
        &(arts_edt_hint_t){.rank = home, .finish_event = all});
    arts_add_dependence(read, ds, 0, DB_MODE_NULL);
  }
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
