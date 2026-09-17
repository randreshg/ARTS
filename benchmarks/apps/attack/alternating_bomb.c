/* alternating_bomb -- two writers on two ranks race against a reader swarm
 * packed onto a configurable number of sharer ranks, all against one block,
 * isolating a protocol's grant-thrash cost when the reader population greatly
 * outnumbers the writers contending the same block. */
#include "attack/chain.h"
#include <stdlib.h>

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  u64 bytes = 65536, sharer_ranks = 0, ops_r = 2000, ops_w = 200;
  u64 argc = getArgc(depv[0].ptr);
  if (argc > 1) bytes = (u64)atol(getArgv(depv[0].ptr, 1));
  if (argc > 2) sharer_ranks = (u64)atol(getArgv(depv[0].ptr, 2));
  if (argc > 3) ops_r = (u64)atol(getArgv(depv[0].ptr, 3));
  if (argc > 4) ops_w = (u64)atol(getArgv(depv[0].ptr, 4));
  u64 pd_count = 0;
  ocrAffinityCount(AFFINITY_PD, &pd_count);
  if (pd_count == 0) pd_count = 1;
  u64 actor_ranks = pd_count > 1 ? pd_count - 1 : 1;
  u64 readers = 15 * actor_ranks - 2;
  if (sharer_ranks > 0 && sharer_ranks <= actor_ranks) {
    u64 rem = readers % sharer_ranks;
    if (rem) readers += sharer_ranks - rem;
  }
  attack_chain_spec_t s = {0};
  s.name = "ALTERNATING_BOMB";
  s.layout = ATTACK_CHAIN_SHARED;
  s.writers_total = 2;
  s.writer_ranks = 2;
  s.readers_total = readers;
  s.reader_ranks = sharer_ranks;
  s.homes = 1;
  s.blocks = 1;
  s.bytes = bytes;
  s.hold_r_us = 1; s.hold_w_us = 1;
  s.think_r_us = 20; s.think_w_us = 20;
  s.ops_r = ops_r; s.ops_w = ops_w;
  return attack_chain_run(&s, pd_count);
}
