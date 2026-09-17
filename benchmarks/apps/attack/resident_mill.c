/* resident_mill -- one writer keeps a block resident while a readers-per-rank
 * swarm mills against it; the writer's think time can be pinned separately
 * from the readers', so a protocol's write-turn frequency is an independent
 * knob from its read-side churn. */
#include "attack/chain.h"
#include <stdlib.h>

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  u64 bytes = 65536, readers_per_rank = 15, think_us = 20, writer_think_us = 0;
  u64 ops_r = 1750, ops_w = 70;
  u64 argc = getArgc(depv[0].ptr);
  if (argc > 1) bytes = (u64)atol(getArgv(depv[0].ptr, 1));
  if (argc > 2) readers_per_rank = (u64)atol(getArgv(depv[0].ptr, 2));
  if (argc > 3) think_us = (u64)atol(getArgv(depv[0].ptr, 3));
  if (argc > 4) writer_think_us = (u64)atol(getArgv(depv[0].ptr, 4));
  if (argc > 5) ops_r = (u64)atol(getArgv(depv[0].ptr, 5));
  if (argc > 6) ops_w = (u64)atol(getArgv(depv[0].ptr, 6));
  u64 pd_count = 0;
  ocrAffinityCount(AFFINITY_PD, &pd_count);
  if (pd_count == 0) pd_count = 1;
  attack_chain_spec_t s = {0};
  s.name = "RESIDENT_MILL";
  s.layout = ATTACK_CHAIN_SHARED;
  s.readers_per_rank = readers_per_rank;
  s.writers_total = 1;
  s.writer_ranks = 1;
  s.homes = 1;
  s.blocks = 1;
  s.bytes = bytes;
  s.hold_r_us = 1; s.hold_w_us = 1;
  s.think_r_us = think_us;
  s.think_w_us = writer_think_us ? writer_think_us : think_us;
  s.ops_r = ops_r; s.ops_w = ops_w;
  return attack_chain_run(&s, pd_count);
}
