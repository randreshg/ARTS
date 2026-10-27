/******************************************************************************
** This material was prepared as an account of work sponsored by an agency   **
** of the United States Government.  Neither the United States Government    **
** nor the United States Department of Energy, nor Battelle, nor any of      **
** their employees, nor any jurisdiction or organization that has cooperated **
** in the development of these materials, makes any warranty, express or     **
** implied, or assumes any legal liability or responsibility for the accuracy,*
** completeness, or usefulness or any information, apparatus, product,       **
** software, or process disclosed, or represents that its use would not      **
** infringe privately owned rights.                                          **
**                                                                           **
** Copyright 2019 Battelle Memorial Institute                                **
** Licensed under the Apache License, Version 2.0 (the "License").           **
******************************************************************************/

/// @file db_create_with_data_bytes.c
/// @brief A created block's bytes are the creator's, byte for byte, at the
///        consumer that acquires it next.
///
/// The creator fills each block with a byte pattern through the hold its
/// create took and releases it; a consumer EDT then acquires the block and
/// compares every byte, and checks that the dependence names the block it
/// delivered.  Labeled blocks of several sizes (sub-word, exact word, an
/// unaligned tail, one well past the buffer header, and no bytes at all) are
/// read RO; a block with a runtime-minted GUID is acquired RW, so a write turn
/// starts from the creator's bytes too.
///
/// A stranded waiter is caught by the ctest TIMEOUT.

#include "arts.h"

#include <stdint.h>
#include <string.h>

#include "../test_failure_status.h"

#define NUM_LABELED 5u
#define MINTED_BYTES 256u

/// paramv[0] = the block's GUID (also the pattern's key), paramv[1] = its size
/// in bytes, paramv[2] = the case id.
#define PV_DB 0
#define PV_SIZE 1
#define PV_ID 2

/// Keyed by the GUID and the byte index, so each case has distinct bytes and a
/// stale, zeroed or short copy is caught.
static unsigned char pattern_byte(uint64_t key, uint64_t i) {
  return (unsigned char)((key * 1315423911u) ^ (i * 2654435761u) ^ (i << 3));
}

void check_bytes(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                 arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  uint64_t key = paramv[PV_DB];
  uint64_t size = paramv[PV_SIZE];
  unsigned int id = (unsigned int)paramv[PV_ID];
  if (depv[0].guid != (arts_guid_t)key) {
    arts_printf("  FAIL: case %u dependence does not name its block\n", id);
    arts_test_fail();
    return;
  }
  const unsigned char *data = (const unsigned char *)depv[0].ptr;
  if (data == NULL && size != 0) {
    arts_printf("  FAIL: case %u acquire returned NULL\n", id);
    arts_test_fail();
    return;
  }
  for (uint64_t i = 0; i < size; i++) {
    unsigned char want = pattern_byte(key, i);
    if (data[i] != want) {
      arts_printf("  FAIL: case %u byte %lu = 0x%02x want 0x%02x\n", id,
                  (unsigned long)i, data[i], want);
      arts_test_fail();
      return;
    }
  }
  arts_printf("  PASS: case %u byte-exact %lu bytes\n", id,
              (unsigned long)size);
}

static void consume(arts_guid_t g, uint64_t size, unsigned int id,
                    arts_db_access_mode_t mode, arts_guid_t scope) {
  uint64_t pv[3] = {(uint64_t)g, size, id};
  arts_guid_t e =
      arts_edt_create(check_bytes, 3, pv, 1,
                      &(arts_edt_hint_t){.rank = 0, .finish_event = scope});
  arts_add_dependence(g, e, 0, mode);
}

/// Runs once every consumer's scope drained.
static void finale(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                   arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  if (arts_test_status() == 0) {
    arts_printf("PASS: db_create_with_data_bytes %u cases byte-exact\n",
                NUM_LABELED + 1u);
  }
  arts_shutdown();
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== db_create_with_data_bytes ===\n");

  const uint64_t sizes[NUM_LABELED] = {1u, 7u, 64u, 1000u, 0u};
  arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);

  for (unsigned int c = 0; c < NUM_LABELED; c++) {
    uint64_t size = sizes[c];
    arts_guid_t g = arts_guid_reserve(ARTS_GUID_DB, 0);
    unsigned char *p = (unsigned char *)arts_db_create_with_guid(
        g, size, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
    for (uint64_t i = 0; i < size; i++) {
      p[i] = pattern_byte((uint64_t)g, i);
    }
    arts_db_release(g, DB_MODE_RW);
    consume(g, size, c, DB_MODE_RO, fe);
  }

  void *mp = NULL;
  arts_guid_t minted = arts_db_create(&mp, MINTED_BYTES, ARTS_DB,
                                      ARTS_DB_PROP_NONE, NULL);
  for (uint64_t i = 0; i < MINTED_BYTES; i++) {
    ((unsigned char *)mp)[i] = pattern_byte((uint64_t)minted, i);
  }
  arts_db_release(minted, DB_MODE_RW);
  consume(minted, MINTED_BYTES, NUM_LABELED, DB_MODE_RW, fe);

  arts_guid_t f =
      arts_edt_create(finale, 0, NULL, 1, &(arts_edt_hint_t){.rank = 0});
  arts_add_dependence(fe, f, 0, DB_MODE_NULL);
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
