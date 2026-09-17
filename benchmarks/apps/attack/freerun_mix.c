/* freerun_mix -- readers and writers free-run against one shared block with
 * one hold time and one think time shared by both classes, the steady-state
 * read/write mix a protocol sees with no artificial phase separation between
 * the roles. */
#include "attack/chain.h"
#include <stdlib.h>

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  u64 bytes = 65536, hold_us = 1, think_us = 20, ops_r = 700, ops_w = 88;
  u64 argc = getArgc(depv[0].ptr);
  if (argc > 1) bytes = (u64)atol(getArgv(depv[0].ptr, 1));
  if (argc > 2) hold_us = (u64)atol(getArgv(depv[0].ptr, 2));
  if (argc > 3) think_us = (u64)atol(getArgv(depv[0].ptr, 3));
  if (argc > 4) ops_r = (u64)atol(getArgv(depv[0].ptr, 4));
  if (argc > 5) ops_w = (u64)atol(getArgv(depv[0].ptr, 5));
  u64 pd_count = 0;
  ocrAffinityCount(AFFINITY_PD, &pd_count);
  if (pd_count == 0) pd_count = 1;
  attack_chain_spec_t s = {0};
  s.name = "FREERUN_MIX";
  s.layout = ATTACK_CHAIN_SHARED;
  s.readers_per_rank = 13;
  s.writers_per_rank = 2;
  s.homes = 1;
  s.blocks = 1;
  s.bytes = bytes;
  s.hold_r_us = hold_us; s.hold_w_us = hold_us;
  s.think_r_us = think_us; s.think_w_us = think_us;
  s.ops_r = ops_r; s.ops_w = ops_w;
  return attack_chain_run(&s, pd_count);
}
