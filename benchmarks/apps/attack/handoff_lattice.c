/* handoff_lattice -- one satisfy per generation wakes FANOUT readers of the
 * region just written and the writer of the NEXT region, on ranks that rotate
 * every generation.  Event order alone makes the two sides byte-disjoint, so
 * whatever a configuration charges per generation beyond the holds is the
 * price of deciding permission at block granularity. */
#include "attack/handoff.h"
#include <stdlib.h>

ocrGuid_t mainEdt(u32 paramc, u64 *paramv, u32 depc, ocrEdtDep_t depv[]) {
  (void)paramc; (void)paramv; (void)depc;
  u64 bytes = 65536, hold_w = 20, hold_r = 2000, fanout = 0, gens = 2000;
  u64 lattices = 16, regions = 8;
  u64 argc = getArgc(depv[0].ptr);
  u64 *args[] = {&bytes, &hold_w, &hold_r, &fanout, &gens, &lattices,
                 &regions};
  for (u64 i = 0; i < sizeof(args) / sizeof(args[0]); i++)
    if (argc > i + 1) *args[i] = (u64)atol(getArgv(depv[0].ptr, i + 1));
  u64 pd_count = 0;
  ocrAffinityCount(AFFINITY_PD, &pd_count);
  if (pd_count == 0) pd_count = 1;
  if (fanout == 0) fanout = pd_count - 1; /* every rank but the next writer's */
  attack_handoff_spec_t s = {0};
  s.name = "HANDOFF_LATTICE";
  s.lattices = lattices;
  s.regions = regions;
  s.bytes = bytes;
  s.hold_w_us = hold_w;
  s.hold_r_us = hold_r;
  s.think_us = 20;
  s.fanout = fanout;
  s.gens = gens;
  return attack_handoff_run(&s, pd_count);
}
