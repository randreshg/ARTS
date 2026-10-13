/* SPDX-License-Identifier: Apache-2.0
 *
 * dispatcher_default_fatal — the dispatcher's `default` (unknown
 * message_type) arm.
 *
 * The dispatcher has one entry point, arts_transport_dispatch_packet, which
 * both the wire receive path and the self-loopback drain call.  This test
 * calls it with a header-only packet whose message_type is MSG_COUNT — the
 * sentinel ordinal, which is NOT a live case in ANY build's switch — so
 * routing falls into `default`:
 *     ARTS_INFO("Unknown Packet ...") + arts_shutdown() + arts_runtime_stop().
 *
 * The contract under test: an unknown/foreign-ordinal packet does not misparse
 * or crash — it routes to the single defensive default arm that idempotently
 * shuts the runtime down (arts_shutdown is a CAS-gated idempotent stop, and the
 * test's main_edt itself ran via the live runtime, so the runtime was up when
 * the dispatcher was invoked).  The dispatcher must RETURN to its caller after
 * the default arm (the switch breaks out of the function), which is what lets
 * this test print its PASS token and end cleanly rather than the process
 * aborting.
 *
 * White-box: like the tests/unit dispatcher/route-table tests, this includes
 * the internal transport headers to call the dispatcher and to fill a wire
 * header. MSG_COUNT is the gapless enum's last sentinel (== live-member count),
 * so it is a stable "unknown" ordinal in every config (the live ordinals are
 * 0..MSG_COUNT-1).
 *
 * Config-agnostic: the default arm is unconditional in every protocol build, so
 * this runs unchanged under every protocol.
 * Single-node (runtime_single): no wire, no peers — the call is purely local.
 */

#include <stdint.h>
#include <stdio.h>

#include "arts.h"

/* White-box: the dispatcher + arts_msg_header_s + arts_fill_packet_header. */
#include "arts/transport/dispatcher.h"
#include "arts/transport/protocol.h"

#include "../test_failure_status.h"

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

  /* The default arm calls arts_shutdown() + arts_runtime_stop() then breaks
   * out, returning here.  No crash, no misparse, no infinite loop — the
   * runtime is told to stop. */
  arts_transport_dispatch_packet(&pkt);

  /* Reached here ⇒ the dispatcher routed the unknown ordinal to the default
   * arm and returned cleanly (the runtime stop was already requested inside
   * it). */
  arts_printf(
      "PASS: dispatcher_default_fatal unknown message_type %u routed to "
      "default (shutdown+runtime_stop)\n",
      (unsigned int)MSG_COUNT);

  /* arts_shutdown was already invoked by the default arm; calling it again is
   * the idempotent CAS-gated no-op (safe, matches the runtime's own re-entry).
   */
  arts_shutdown();
}

int main(int argc, char **argv) {
  int rc = arts_rt(argc, argv);
  return rc ? 1 : arts_test_status();
}
