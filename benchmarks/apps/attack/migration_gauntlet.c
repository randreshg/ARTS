/* migration_gauntlet -- one shared block whose write right never rests: the
 * writer slots take turns in a fixed rotation across the writer ranks, so
 * consecutive turns land on consecutive ranks and every write migrates the
 * write right to another rank -- except the turn that wraps a rotation back to
 * its first slot, which repeats a rank when the writer-rank count divides one
 * less than the rotation's length (with two writer slots per rank and RINGS=2,
 * only at a single writer rank).  A large reader set free-runs against the
 * same block and chases the owner.  RINGS independent rotations run at once
 * (one by default), which is both the number of writes in flight and the
 * longest chain of writers the home can queue.  Isolates a protocol's per-turn
 * ownership handoff cost from any timing variation.
 *
 * args: BYTES OPS_R OPS_W [RINGS] */
#include "attack/chain.h"
#include <stdlib.h>

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  u64 bytes = 65536, ops_r = 750, ops_w = 2, rings = 1;
  u64 argc = getArgc(depv[0].ptr);
  if (argc > 1) bytes = (u64)atol(getArgv(depv[0].ptr, 1));
  if (argc > 2) ops_r = (u64)atol(getArgv(depv[0].ptr, 2));
  if (argc > 3) ops_w = (u64)atol(getArgv(depv[0].ptr, 3));
  if (argc > 4) rings = (u64)atol(getArgv(depv[0].ptr, 4));
  u64 pd_count = 0;
  ocrAffinityCount(AFFINITY_PD, &pd_count);
  if (pd_count == 0) pd_count = 1;
  attack_chain_spec_t s = {0};
  s.name = "MIGRATION_GAUNTLET";
  s.layout = ATTACK_CHAIN_SHARED;
  s.readers_per_rank = 13;
  s.writers_per_rank = 2;
  s.homes = 1;
  s.blocks = 1;
  s.bytes = bytes;
  s.hold_r_us = 1; s.hold_w_us = 1;
  s.think_r_us = 20; s.think_w_us = 20;
  s.ops_r = ops_r; s.ops_w = ops_w;
  s.writer_rings = rings;
  return attack_chain_run(&s, pd_count);
}
