/* Wire-shape guard for the CXL arm: the create packet's slot field.  Layout
 * only — no runtime is started. */
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
#ifdef ARTS_USE_CXL
  CHECK(sizeof(struct arts_msg_db_create_coherent_packet_s) == hdr + 40,
        "create packet body is %zu, expected 40",
        sizeof(struct arts_msg_db_create_coherent_packet_s) - hdr);
  CHECK(offsetof(struct arts_msg_db_create_coherent_packet_s, cxl_addr) ==
            hdr + 24,
        "cxl_addr must follow create_token");
#else
  CHECK(sizeof(struct arts_msg_db_create_coherent_packet_s) == hdr + 32,
        "a non-CXL create packet keeps its body");
#endif
  if (fails == 0) {
    (void)printf("PASS cxl_wire_layout\n");
  }
  return fails ? 1 : 0;
}
