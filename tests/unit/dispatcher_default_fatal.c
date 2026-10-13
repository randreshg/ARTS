/* SPDX-License-Identifier: Apache-2.0
 *
 * dispatcher_default_fatal — the dispatcher's `default` (unknown
 * message_type) arm.
 *
 * The dispatcher has one entry point, arts_transport_dispatch_packet, which
 * both the wire receive path and the self-loopback drain call.  This test
 * calls it with a header-only packet whose message_type is MSG_COUNT — the
 * sentinel ordinal, which is NOT a live case in ANY build's switch — so
 * routing falls into `default`.
 *
 * The contract under test: a packet of a type the build does not know means
 * the two ends disagree about the wire (a binary mode mismatch) or the packet
 * is corrupt, so the dispatcher neither misparses it nor lets the run go on to
 * end as a success: it reports the type and the sender and aborts the process
 * with exit status 1, like every other mismatch arm.  The test is registered
 * with that status and the report as its pass condition; a dispatcher that
 * returns reaches the FAIL line below and the run then ends with status 0,
 * which the registered status refuses.
 *
 * White-box: like the tests/unit dispatcher/route-table tests, this includes
 * the internal transport headers to call the dispatcher and to fill a wire
 * header. MSG_COUNT is the gapless enum's last sentinel (== live-member count),
 * so it is a stable "unknown" ordinal in every config (the live ordinals are
 * 0..MSG_COUNT-1).
 *
 * Config-agnostic: the default arm is unconditional in every protocol build, so
 * this runs unchanged under every protocol.
 * Single-node: no wire, no peers — the call is purely local.
 */

#include <stdint.h>
#include <stdio.h>

#include "arts.h"

/* White-box: the dispatcher + arts_msg_header_s + arts_fill_packet_header. */
#include "arts/transport/dispatcher.h"
#include "arts/transport/protocol.h"

void main_edt(uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;

  arts_printf("=== dispatcher_default_fatal ===\n");

  /* Build a header-only packet with an unknown message_type.  MSG_COUNT is the
   * sentinel ordinal == (live member count); no switch case matches it, so the
   * body routes to `default`.  arts_fill_packet_header sets size + type + rank;
   * size == sizeof(header) (header-only, no trailing payload). */
  struct arts_msg_header_s pkt;
  arts_fill_packet_header(&pkt, (uint64_t)sizeof(pkt), (unsigned int)MSG_COUNT);

  /* The default arm aborts the process; it never returns here. */
  arts_transport_dispatch_packet(&pkt);

  arts_printf("FAIL: dispatcher_default_fatal: the dispatcher returned from "
              "unknown message_type %u\n",
              (unsigned int)MSG_COUNT);
  arts_shutdown();
}

int main(int argc, char **argv) {
  /* Status 0 whenever the run returns: the expected exit is the dispatcher's
     own abort with status 1, never a return from here. */
  (void)arts_rt(argc, argv);
  return 0;
}
