/* pipeline_converge -- readers converge on a block set sized to pd_count-1 so
 * every block sees exactly two writers; each reader waits for its block's
 * final write count rather than a raw read tally, so the read class is
 * value-coupled to the writers feeding its block. */
#include "attack/chain.h"
#include <stdlib.h>

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  u64 bytes = 65536, ops_r = 56, ops_w = 28;
  u64 argc = getArgc(depv[0].ptr);
  if (argc > 1) bytes = (u64)atol(getArgv(depv[0].ptr, 1));
  if (argc > 2) ops_r = (u64)atol(getArgv(depv[0].ptr, 2));
  if (argc > 3) ops_w = (u64)atol(getArgv(depv[0].ptr, 3));
  u64 pd_count = 0;
  ocrAffinityCount(AFFINITY_PD, &pd_count);
  if (pd_count == 0) pd_count = 1;
  attack_chain_spec_t s = {0};
  s.name = "PIPELINE_CONVERGE";
  s.layout = ATTACK_CHAIN_SHARED;
  s.readers_per_rank = 13;
  s.writers_per_rank = 2;
  s.homes = 1;
  s.blocks = pd_count > 1 ? pd_count - 1 : 1;
  s.bytes = bytes;
  s.coupled = 1;
  s.hold_r_us = 1; s.hold_w_us = 1;
  s.think_r_us = 20; s.think_w_us = 20;
  s.ops_r = ops_r; s.ops_w = ops_w;
  return attack_chain_run(&s, pd_count);
}
