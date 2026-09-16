/// @file api_db_check_rendezvous.c
/// @brief A labeled create is first-wins, with or without the check hint.
///
/// One label names one object for its lifetime: the create whose install
/// lands is its creator, and a create of a label that already exists creates
/// nothing — no hold, no pointer, success — so it cannot overwrite what the
/// first creator wrote.  The hint the standard's CHECK property maps to is
/// accepted and changes nothing, which is what this pins: a replacing install
/// would leave two directories for one GUID, and every message that finds a
/// block by its label would land on whichever the slot holds now.
///
/// Both halves are observed by a reader EDT that depends RO on the labeled
/// GUID and reads back the surviving sentinel:
///   GUID A: first writes 0xAAAA, then a create with check=true tries 0xBBBB
///           -> reader must see 0xAAAA.
///   GUID B: first writes 0xCCCC, then a create with check=false tries 0xDDDD
///           -> reader must see 0xCCCC.
///
/// Config-agnostic: pure single-node public-API DB semantics.
/// exposes_runtime_bug = false (pins the first-wins labeled create).
#include "arts.h"
#include <stdint.h>

#define SENT_A_FIRST 0xAAAAu
#define SENT_A_SECOND 0xBBBBu
#define SENT_B_FIRST 0xCCCCu
#define SENT_B_SECOND 0xDDDDu

static int g_failed = 0;

/* Reader gated on both labeled DBs being RO-readable.  A labeled create is
 * first-wins whether or not the hint asks for the check, so BOTH labels keep
 * their first generation: the second create of each creates nothing, is
 * handed no pointer, and writes nowhere.
 *   depv[0] = labeled GUID A (RO), hint check=true  -> SENT_A_FIRST
 *   depv[1] = labeled GUID B (RO), hint check=false -> SENT_B_FIRST */
void reader_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
                arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  uint32_t a = depv[0].ptr ? *(uint32_t *)depv[0].ptr : 0u;
  uint32_t b = depv[1].ptr ? *(uint32_t *)depv[1].ptr : 0u;

  if (a != SENT_A_FIRST) {
    arts_printf("FAIL api_db_check_rendezvous: CHECK collision did not keep "
                "first generation (got 0x%X want 0x%X)\n",
                a, SENT_A_FIRST);
    g_failed = 1;
  }
  if (b != SENT_B_FIRST) {
    arts_printf("FAIL api_db_check_rendezvous: a create without the check "
                "hint did not keep the first generation (got 0x%X want "
                "0x%X)\n",
                b, SENT_B_FIRST);
    g_failed = 1;
  }
  if (!g_failed) {
    arts_printf("PASS api_db_check_rendezvous: first-wins with and without "
                "the check hint\n");
  }
  arts_shutdown();
}

/* Create a labeled DB at `guid`, write `sentinel`, release.  `check` is the
 * hint the standard's CHECK property maps to; a labeled create is first-wins
 * either way, so the flag only proves the hint changes nothing. */
static void labeled_write(arts_guid_t guid, uint32_t sentinel, bool check) {
  arts_db_hint_t h = ARTS_DB_HINT_DEFAULTS;
  h.guid = guid;
  h.check = check;
  void *p = NULL;
  arts_db_create(&p, sizeof(uint32_t), ARTS_DB, ARTS_DB_PROP_NONE, &h);
  /* A create of a label that already exists creates nothing and is handed no
   * pointer, so the write below simply does not happen; the generation the
   * reader observes is the first creator's. */
  if (p) {
    *(uint32_t *)p = sentinel;
  }
  arts_db_release(guid, DB_MODE_RW);
}

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== api_db_check_rendezvous ===\n");

  /* Pre-reserve two labeled DB GUIDs local to this rank. */
  arts_guid_t ga = arts_guid_reserve(ARTS_GUID_DB, arts_get_current_rank());
  arts_guid_t gb = arts_guid_reserve(ARTS_GUID_DB, arts_get_current_rank());

  /* GUID A: first generation 0xAAAA, then a create with the check hint that
   * must lose the install and keep 0xAAAA. */
  labeled_write(ga, SENT_A_FIRST, /*check=*/false);
  labeled_write(ga, SENT_A_SECOND, /*check=*/true);

  /* GUID B: the same, without the hint — first-wins is the rule either way,
   * so 0xCCCC stands. */
  labeled_write(gb, SENT_B_FIRST, /*check=*/false);
  labeled_write(gb, SENT_B_SECOND, /*check=*/false);

  arts_guid_t r = arts_edt_create(reader_edt, 0, NULL, 2, NULL);
  arts_add_dependence(ga, r, 0, DB_MODE_RO);
  arts_add_dependence(gb, r, 1, DB_MODE_RO);
}

int main(int argc, char **argv) {
  arts_rt(argc, argv);
  return g_failed;
}
