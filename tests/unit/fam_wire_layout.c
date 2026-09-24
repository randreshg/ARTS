/* Wire-shape guard for the fabric-memory arm: the create packet's slot fields
 * and the two appended messages.  Layout only — no runtime is started. */
#include "arts/transport/protocol.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static int fails;
#define CHECK(cond, ...)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      (void)fprintf(stderr, "FAIL: " __VA_ARGS__);                           \
      (void)fprintf(stderr, "\n");                                           \
      fails++;                                                               \
    }                                                                        \
  } while (0)

int main(void) {
  const size_t hdr = sizeof(struct arts_msg_header_s);
#ifdef ARTS_FAM
  CHECK(sizeof(struct arts_msg_db_create_coherent_packet_s) == hdr + 40,
        "create packet body is %zu, expected 40",
        sizeof(struct arts_msg_db_create_coherent_packet_s) - hdr);
  CHECK(offsetof(struct arts_msg_db_create_coherent_packet_s, fam_addr) ==
            hdr + 24,
        "fam_addr must follow create_token");
#else
  CHECK(sizeof(struct arts_msg_db_create_coherent_packet_s) == hdr + 32,
        "a non-FAM create packet keeps its body");
#endif
  /* Both messages exist in every build: only struct layouts are conditional. */
  CHECK(sizeof(struct arts_msg_db_fam_free_packet_s) == hdr + 8,
        "the free-to-owner message carries one address");
  CHECK(sizeof(struct arts_msg_db_fam_fetch_done_packet_s) == hdr + 16,
        "the fetch-done message carries a guid and a mode");
  CHECK((int)MSG_DB_FAM_FREE + 1 == (int)MSG_DB_FAM_FETCH_DONE,
        "the two appended members are adjacent");
  CHECK((int)MSG_DB_FAM_FETCH_DONE + 1 == (int)MSG_COUNT,
        "MSG_DB_FAM_FETCH_DONE must be the last member before the sentinel");
  if (fails == 0) {
    (void)printf("PASS fam_wire_layout\n");
  }
  return fails ? 1 : 0;
}
