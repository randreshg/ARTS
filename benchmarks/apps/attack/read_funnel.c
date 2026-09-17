/* read_funnel -- a fixed reader population pulls from BLOCKS blocks homed on
 * HOMES ranks; the population is set by rank count alone, never by HOMES, so
 * two runs that differ only in how many ranks hold the block set still load
 * the runtime with the same reader count. */
#include "attack/chain.h"
#include <stdlib.h>

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  u64 bytes = 65536, blocks = 1, homes = 1, ops_r = 700;
  u64 argc = getArgc(depv[0].ptr);
  if (argc > 1) bytes = (u64)atol(getArgv(depv[0].ptr, 1));
  if (argc > 2) blocks = (u64)atol(getArgv(depv[0].ptr, 2));
  if (argc > 3) homes = (u64)atol(getArgv(depv[0].ptr, 3));
  if (argc > 4) ops_r = (u64)atol(getArgv(depv[0].ptr, 4));
  u64 pd_count = 0;
  ocrAffinityCount(AFFINITY_PD, &pd_count);
  if (pd_count == 0) pd_count = 1;
  u64 zone = pd_count > 1 ? pd_count - 1 : 1;
  attack_chain_spec_t s = {0};
  s.name = "READ_FUNNEL";
  s.layout = ATTACK_CHAIN_SHARED;
  s.readers_total = 15 * zone;
  s.homes = homes;
  s.blocks = blocks;
  s.bytes = bytes;
  s.hold_r_us = 1; s.think_r_us = 20;
  s.ops_r = ops_r;
  return attack_chain_run(&s, pd_count);
}
