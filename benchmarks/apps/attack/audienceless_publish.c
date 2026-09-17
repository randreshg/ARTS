/* audienceless_publish -- fifteen writers on one rank hammer a single block
 * that no reader ever touches, isolating a protocol's pure write-publish path
 * from any read-side coherence traffic. */
#include "attack/chain.h"
#include <stdlib.h>

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  u64 bytes = 65536, ops_w = 350;
  u64 argc = getArgc(depv[0].ptr);
  if (argc > 1) bytes = (u64)atol(getArgv(depv[0].ptr, 1));
  if (argc > 2) ops_w = (u64)atol(getArgv(depv[0].ptr, 2));
  u64 pd_count = 0;
  ocrAffinityCount(AFFINITY_PD, &pd_count);
  if (pd_count == 0) pd_count = 1;
  attack_chain_spec_t s = {0};
  s.name = "AUDIENCELESS_PUBLISH";
  s.layout = ATTACK_CHAIN_SHARED;
  s.writers_total = 15;
  s.writer_ranks = 1;
  s.homes = 1;
  s.blocks = 1;
  s.bytes = bytes;
  s.hold_w_us = 1; s.think_w_us = 20;
  s.ops_w = ops_w;
  return attack_chain_run(&s, pd_count);
}
