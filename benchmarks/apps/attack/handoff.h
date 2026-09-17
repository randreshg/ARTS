/* handoff.h -- event-ordered producer/consumer lattice engine for the attack
 * programs.
 *
 * A program describes ONE workload in a spec: how many lattices advance in
 * parallel, how many regions their blocks are cut into, how long each side
 * holds, how many consumers a generation wakes, and how many generations a
 * lattice runs.  The engine creates the lattices from mainEdt, advances each
 * as a self-perpetuating producer sequence whose placement rotates every
 * generation, and prints the program's result line once every lattice has
 * finished.
 *
 * Contract with the caller: mainEdt fills the spec and returns the result of
 * attack_handoff_run(); nothing else is created by the program.  All
 * per-lattice accounting travels in that lattice's own paramv and lands in a
 * per-lattice result block, so the engine shares no memory between EDTs.
 * Work is fixed: every lattice produces exactly gens generations, so no
 * figure depends on a clock reaching a deadline, and every figure the engine
 * reports is taken on one rank's clock. */
#ifndef ATTACK_HANDOFF_H
#define ATTACK_HANDOFF_H

#include "ocr.h"
#include "extensions/ocr-affinity.h"

typedef struct {
  const char *name; /* printed as "<NAME> OK ..." */
  u64 lattices;     /* independent lattices, one block each */
  u64 regions;      /* regions per block, at least 2; generation g writes
                     * g % regions, so a generation's producer and its
                     * consumers touch byte-disjoint, line-aligned spans and
                     * the program is race-free at byte granularity by event
                     * order alone */
  u64 bytes;        /* raised until every region holds a full stamp + fill */
  u64 hold_w_us, hold_r_us; /* spin while holding, per side */
  u64 think_us;             /* producer spin between its release and the
                             * satisfy that opens the next generation */
  u64 fanout; /* consumers woken per generation, clamped to [1, pd_count] */
  u64 gens;   /* generations per lattice; required > 0 */
} attack_handoff_spec_t;

/* Creates blocks, lattices and the collector; returns NULL_GUID.  Prints
 * "<NAME>-ORACLE-FAIL kind=args ..." and shuts down on an impossible spec. */
ocrGuid_t attack_handoff_run(const attack_handoff_spec_t *spec, u64 pd_count);

#endif
