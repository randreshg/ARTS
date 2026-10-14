/* SPDX-License-Identifier: Apache-2.0
 *
 * A GRANT_RESPONSE is bounded by the bytes that arrived with it.  One shorter
 * than its header, or one whose header declares a version map longer than the
 * bytes that follow the header, is a protocol violation only the receiving
 * rank can see, and it is fatal there (ARTS_ERROR, exit status 1) instead of
 * being read past.  argv[1] names the case:
 *   short -- a packet one byte shorter than the header;
 *   map   -- a header for a live block declaring four map entries, followed
 *            by the eight-byte count field alone.
 * Each packet is a heap allocation of exactly the size handed to the handler,
 * so a read past it is a sanitizer report as well.
 */
#include "arts.h"
#include "arts/coherence/handlers.h"
#include "arts/transport/protocol.h"

#include <stdlib.h>
#include <string.h>

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)depc;
  (void)depv;
  int argc = (int)paramv[0];
  char **argv = (char **)paramv[1];
  const char *which = argc > 1 ? argv[1] : "";
  struct arts_msg_grant_response_packet_s hdr;
  memset(&hdr, 0, sizeof(hdr));
  if (strcmp(which, "short") == 0) {
    size_t size = sizeof(hdr) - 1;
    void *pkt = malloc(size);
    memcpy(pkt, &hdr, size);
    arts_handler_db_grant_response(pkt, size);
    free(pkt);
  } else if (strcmp(which, "map") == 0) {
    void *p = NULL;
    hdr.db_guid =
        arts_db_create(&p, 64, ARTS_DB, ARTS_DB_PROP_NONE, NULL);
    arts_db_release(hdr.db_guid, DB_MODE_RW);
    hdr.header.rank = arts_get_current_rank();
    hdr.map_entry_count = 4;
    hdr.data_size = 64;
    size_t size = sizeof(hdr) + sizeof(uint32_t) * 2;
    unsigned char *pkt = (unsigned char *)calloc(1, size);
    memcpy(pkt, &hdr, sizeof(hdr));
    arts_handler_db_grant_response(pkt, size);
    free(pkt);
  } else {
    arts_printf("FAIL grant_response_bounds: unknown case '%s'\n", which);
    arts_shutdown();
    return;
  }
  arts_printf("FAIL grant_response_bounds: the '%s' packet was accepted\n",
              which);
  arts_shutdown();
}

int main(int argc, char **argv) { return arts_rt(argc, argv) != 0 ? 1 : 0; }
