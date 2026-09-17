/* own_reread -- every chain re-reads the block it owns, homed one rank away.
 * The workload needs no communication at all, so whatever a configuration
 * charges per op is the price of forgetting its own state at a release. */
#include "attack/chain.h"
#include <stdlib.h>

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  u64 bytes = 65536, ops = 740000, per_rank = 3;
  u64 argc = getArgc(depv[0].ptr);
  if (argc > 1) bytes = (u64)atol(getArgv(depv[0].ptr, 1));
  if (argc > 2) ops = (u64)atol(getArgv(depv[0].ptr, 2));
  if (argc > 3) per_rank = (u64)atol(getArgv(depv[0].ptr, 3));
  u64 pd_count = 0;
  ocrAffinityCount(AFFINITY_PD, &pd_count);
  if (pd_count == 0) pd_count = 1;
  attack_chain_spec_t s = {0};
  s.name = "OWN_REREAD";
  s.layout = ATTACK_CHAIN_PRIVATE;
  s.readers_total = per_rank * pd_count;
  s.bytes = bytes;
  s.hold_r_us = 1; s.think_r_us = 20;
  s.ops_r = ops;
  s.first_touch_write = 1;
  return attack_chain_run(&s, pd_count);
}
