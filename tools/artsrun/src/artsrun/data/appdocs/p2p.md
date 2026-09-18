# p2p

*PRK synch_p2p — a P-wide column-partitioned wavefront pipeline built for one
purpose: measuring per-hop synchronization latency, not bandwidth.*
Source: `third_party/ocr-apps/apps/p2p/refactored/ocr/intel/p2p.c` (730 lines).

## Overview

p2p is the OCR/ARTS port of the Parallel Research Kernels `synch_p2p`
benchmark: a `p`-way column-partitioned pipeline computing the 2D recurrence
`ARRAY(i,j) = ARRAY(i-1,j) + ARRAY(i,j-1) - ARRAY(i-1,j-1)` over an `n`-row ×
`m`-column grid, where each of `p` logical "ranks" owns a contiguous stripe
of `~m/p` columns and needs its left neighbor's rightmost boundary column
before it can compute its own first column of a row-block. This wires the `p`
logical ranks into a closed pipeline/ring (rank `p-1` wraps back to rank 0),
each one a single persistent EDT chain — one `p2pEdt` self-recreation per
(timestep, phase) generation — that never migrates once born. The result
scalar checks the last cell (`ARRAY(n-1,k-1)`) against the closed form
`(t+1)·(n+m-2)`; timing is also printed (rank 0 stamps the start, rank `p-1`
the end). Because every hot-path datablock is either exclusively private to
one rank's chain or a single-producer/single-consumer 1:1 handoff — never
shared by more than two ranks, never read by more than one consumer — the app
has essentially no coherence contention; any slowdown is per-hop
create/acquire/wire latency along a long dependency chain, which is exactly
what this kernel is built to probe.

Everything inside the measured window is the published workload: the argument
parse, the `p`-wide spawn loop (the program's own graph construction, and its
exhibit), the per-rank array initialisation — done in parallel by the task
that owns the array, from a closed form of the column index — the `t+1`
sweeps, and one O(1) closed-form comparison. No data file is read, no result
file is written, nothing is recomputed for verification, and no RNG is used.

## Parameters

| arg | meaning | default | CLI reachability |
|-----|---------|---------|-------------------|
| `argv[1]` = `p` | number of logical p2p "ranks" (independent of the actual ARTS node/worker count) | 10 | ✓ parsed in `mainEdt`, carried via `realMainEdt`'s paramv to every `initEdt` — multinode-safe |
| `argv[2]` = `m` | total columns, split ~evenly across `p` ranks | 100 | ✓ same path |
| `argv[3]` = `n` | rows of the grid (each rank owns all `n` rows of its own column stripe) | 1000 | ✓ same path |
| `argv[4]` = `t` | timesteps (`t+1` sweeps counting one untimed warm-up timestep) | 100 | ✓ same path |
| `argv[5]` = `gf` | group factor: rows processed per phase before a boundary send | 1 | ✓ optional 5th arg; same path |
| `argv[6]` = `chunk` | placement chunk: the run of consecutive ranks a policy domain takes before the map moves on. Parsed in both tiers, carried in `realMainEdt`'s paramv, and read only under `OCR_APP_OPTIMIZED_PLACEMENT`; `0` (and any value above the contiguous block) selects base's contiguous map | 0 | ✓ optional 6th arg; same path |
| `BLOCK` / `AFFINITY` | compile-time `#define`s (unguarded, always on base): `BLOCK` = contiguous-block PD assignment for the `p` ranks (vs `WRAP` = round-robin); `AFFINITY` = actually apply the computed `EDT_AFFINITY` hints (vs `NULL_HINT`, which the source's own comment says gives "bad performance regardless") | both defined | ✗ compile-time, and with no `-D` escape: selecting `WRAP` needs the `#define` edited out of the source, so any BLOCK-vs-WRAP figure is an out-of-tree measurement |

`argc` must be exactly 1, 5, 6, or 7 (program name plus 0, 4, 5, or 6 args). Every
rejected argument now aborts the launch: `mainEdt` returns without creating
`realMainEdt`, so no run is built on a bad value. The accepted domain is
`p ≥ 1`, `m ≥ p` (a rank owns at least one column), `n ≥ 2` (a timestep
advances at least one row), `t ≥ 1`, `gf ≥ 1`, `chunk ≥ 0`, and the arguments are parsed
into a signed temporary so a negative value is rejected instead of wrapping to
a huge unsigned one.

## Structure

Let `w = ceil((n-1)/gf)` (phases per timestep) and `G = (t+1)·w` (generations
per rank, including the untimed timestep 0).

| object | count | size |
|--------|-------|------|
| `p2pEdt` (chain generations) | `p·G` | — |
| `initEdt` / `initp2pEdt` / `initChannelEdt` | `p` each | — |
| `mainEdt` / `realMainEdt` / `p2pShutdownEdt` | 1 each | — |
| EDTs total | `3 + 3p + p·G` | — |
| `dataDBK` (per-rank working array) | `p`, persistent, re-acquired by each generation of its own rank's chain and never touched by another rank; the chain is pinned, so the block never leaves the node it was created on | `n·(⌊m/p⌋+1)·8` bytes each |
| `privateDBK` (per-rank state) | `p`, same persistence pattern | `sizeof(private_t)` (~150-250 B) each |
| `sharedDBK` / `timerDBK` / `channelDBK` (×p) | 1 / 1 / `p` | small, ≤ a few hundred bytes |
| `bufferOutDBK` (boundary handoff) | `(p-1)·G + t` (every non-last-rank generation, plus one wrap-around send per completed timestep from rank `p-1` — `t`, not `t+1`: the final timestep's wrap-around is skipped because the termination check in `p2pEdt` returns before the wrap-around create is ever reached) | `(gf+1)·8` bytes — 16 B at `gf=1` |
| labeled STICKY rendezvous (`sendRightEVT`/`recvLeftEVT`) | `p` distinct objects, `2p` `ocrEventCreate` calls — `GUID_PROP_CHECK` makes the loser of each racing pair an idempotent *install*, but the shim still increments the create counter for the loser too (`arts_event_create`'s `INCREMENT_NUM_EVENT_CREATE_BY(1)` runs before the install-or-fail check), so all `2p` calls count | — |
| CHANNEL events (per-rank boundary transport) | `p`, one per rank, satisfied once per generation except the last | — |
| shim-materialized output event | `1`, constant — the one `p2pEdt` create that spawns rank `p-1`'s terminal instance passes a non-NULL `outputEvent` (`termOETp` in `p2pEdt`'s clone branch) so the shim materializes an event feeding `p2pShutdownEdt`; every other `p2pEdt`/`initEdt`/`initp2pEdt`/`initChannelEdt` create passes `NULL` for it | — |

Events total: `3p + 1`. Worked numbers at the small consensus arguments
`8 256 100 10` (`p=8, m=256, n=100, t=10, gf=1`): `w=99`, `G=1089`, `p2pEdt`
total = 8,712, EDTs total = 8,739, `bufferOutDBK` = 7,633, all other DBs =
`3p+2` = 26 (total DBs = 7,659), events = `3·8+1` = 25, and the result line is
`PASS checksum = 3894.000000`. The campaign runs `p=3456 m=34560 n=3457
t=100 gf=1 chunk=108` (`p` at the width floor, `n` chosen so `w` balances at
`p`, `m` the size knob); the same closed forms give its counts. The one-node
anchor is measured: 81.3 s, 7 GB resident, against a 20 s target (both tiers
anti-scale, clamped).

Counter cross-check: verified (1 node, `args=[2,4,5,2]` vs `args=[2,4,5,3]`,
i.e. `p=2, m=4, n=5, gf=1, t=2` vs `t=3`, so `w=4`, `G=12` vs `G=16`): measured
absolutes EDT 34/42, DB 23/28, EVT 7/7; subtracting the runtime's constant
baseline (+1 EDT, +1 DB, +0 EVT per run) gives app-side EDT 33/41 (exactly
`3+3p+p·G`) and EVT 7/7 (exactly `3p+1` at `p=2`, unaffected by `t`). The
corrected DB formula — `bufferOutDBK = (p-1)·G+t` (not the earlier `+(t+1)`)
— gives `bufferOutDBK` = `1·12+2=14` and `1·16+3=19`; adding the constant
`3p+2=8` gives app-side DB 22/27, exactly reproducing the measured totals
(23/28) once the runtime's +1 DB baseline is added back.

## Wiring

p2p's DB graph is almost entirely private per rank: `dataDBK` and
`privateDBK` are each acquired RW by exactly one generation of exactly one
rank's chain at a time and never touched by any other rank — no fan-out, no
contention. The only cross-rank object is `bufferOutDBK`: created fresh every
generation by the sender, released, delivered via that rank's persistent
CHANNEL event, and acquired+destroyed exactly once by the immediate right
neighbor's next generation — strict single-producer/single-consumer, at most
1 concurrent accessor. The very first generation of each rank is the one mode
asymmetry in the program: `initChannelEdt` wires its boundary slot `DB_MODE_RO`
where every later generation wires it `DB_MODE_RW`, and the body destroys the
block whichever mode it arrived in. It is 1 of `G` generations per rank, so it
cannot move a measurement, but the arms treat an RO and an RW acquire
differently and the asymmetry is real. `sharedDBK` fans out RO to all `p`
`initEdt` instances once at startup only. The one-time labeled-STICKY
rendezvous (`ocrGuidFromIndex` into a pre-reserved GUID range) exists purely so
each rank can learn its left neighbor's CHANNEL guid without a central
directory — it fires once per rank, not per generation. `timerDBK`'s guid is
broadcast to every rank's private state but is only actually acquired twice in
the whole run (rank 0 at timestep 1, rank `p-1` at the terminal generation);
those two instances are the only ones created with four dependences, and the
timer slot is read only inside the two branches that own it — an ordinary
three-dependence generation never touches it.

## Flow

This is a diagonal wavefront/systolic pipeline, not a width-1 serial chain:
rank `i`'s generation `N` depends on rank `i-1`'s generation `N` (the same
(timestep, phase) index, paired FIFO through the CHANNEL), and each rank's
own generations execute strictly one after another (a single persistent
chain). Once filled, essentially all `p` ranks are concurrently active, each
on a different generation index — parallel width approaches `min(p, w)`, not
1. The fill is not amortised over the run: rank `p-1` sends its wrap value
only at the last phase of a timestep and rank 0's next timestep consumes it,
so the pipeline fills and drains once per *timestep*, not once per run, and
the per-timestep critical path is `p + w - 1` hops for every `t`. What makes
it a *latency* probe is the length of that path, not its narrowness: since
every hop's payload is 16 bytes, wall time is dominated by per-hop
create/acquire/wire round-trip cost rather than bandwidth or compute — the
`Rate (MFlops/s)` the program prints is a derived, secondary number.
Termination is a single rank-`p-1` special case (the last generation returns
without cloning; a dedicated `p2pShutdownEdt` waits on that generation's own
output event, which fires only after its dependence releases complete, so
print/shutdown never truncates the measured run).

## Placement (base)

Unlike fibonacci/fft/triangle, p2p's placement is *not* NULL_HINT-throughout
base: `BLOCK`/`AFFINITY` are plain, unguarded `#define`s, so `realMainEdt`
explicitly computes a BLOCK
partition (`myPD = i / block`, `block = ⌈p / count⌉`) and creates each rank's
`initEdt` with an explicit `EDT_AFFINITY` hint pinning it there;
`initEdt`/`initp2pEdt` propagate `ocrAffinityGetCurrent()` downward so each
rank's *entire* chain (all `G` generations) stays pinned to the PD it was born
on — real, load-bearing base locality, by design. The map is exactly balanced
whenever `count` divides `p`, which the campaign's sizes hold to; with a `p`
that leaves a large remainder, `⌈p/count⌉` can empty the top policy domains
altogether, so a size must keep `p` divisible by every node count in the
sweep. `bufferOutDBK` carries no affinity hint in either tier, so it homes at the
sender (creator/first-touch) and the receiving
rank's acquire is always a one-hop remote fetch from its immediate left
neighbor — exactly the point-to-point traffic the benchmark is named for.

## Sizing

`p` is the number of virtual ranks the program decomposes into.  It is an
application-level knob with no relation to the worker threads `arts.cfg` starts,
so it is free to choose; what constrains it is memory, not a rule.

The wavefront's instantaneous width is `min(p, w)` with `w = ⌈(n-1)/gf⌉`: rank
`i` phase `j` waits on rank `i-1` phase `j` and on its own phase `j-1`, so the
runnable set is the diagonal and **both** `p` and the phase count cap it.  The
catalog had this wrong twice -- first at `3456 8192 50 2100` (width 49) and
then at `3456 1347840 500 515`, where `p` was read as "the pipeline's width"
while the 500 rows held the real width to 499 against 3456 workers.

Averaged over a timestep the width is `p·w/(p+w-1)`, because the pipeline fills
and drains inside every timestep.  `p = w` is where the two caps balance, and
for a given array it buys the most width.  The size knobs are therefore `p`,
`n` (through `w`) and `m` (plus `chunk`, the hinted tier's map knob, which
changes no work); the timestep count stays at the published default
`t = 100` and `gf` stays 1 (the PRK README calls any other value cheating and
the program negates its own reported flop rate for it), so the window is
reached with size alone.

**Width formula.** peak `min(p, w)`, average `p·w/(p+w-1)`, with
`w = ⌈(n-1)/gf⌉`.  At `p = w` the peak is `p` and the average `p/2 + O(1)`, so
a peak of `2x` the worker total is an average of `1x`: the two readings differ
by exactly the factor two and any width claim has to say which one it is
quoting.  Raising the *average* means raising `p` and `w` together, which the
memory ceiling bounds.

**Memory formula.** The array is `p · n · (⌊m/p⌋+1) · 8` bytes ≈ `8·n·(m+p)`;
the live boundary blocks are the second term.  Their bound is the program's own
wrap barrier, not the channel's `maxGen` (the ARTS OCR shim ignores `maxGen`
outright): a producer runs ahead of its consumer only until the timestep's wrap
forces rank 0 to wait, so unconsumed 16-byte blocks are bounded by about `w`
per rank, `p·w` in all, each carrying its runtime object.  Total resident is
therefore `8·n·(m+p) + c·p·w` with `c` the per-block runtime cost; an earlier
campaign's numbers put `c` near 1.8 kB, but no RSS sample of that run survives
on disk, so the constant is an estimate.  The measured anchor (7 GB resident
at the campaign arguments) confirms the 1-node budget (≤ 190 GB) holds by a
wide margin regardless.

Note that the trade between the two terms is real: at fixed `m` a larger `p`
only slices the array more finely, but `m` is itself free (`m ≥ p`), so a
coarser or finer per-rank stripe `k = m/p` moves array bytes into pipeline
depth or back.  A claim that no better width point exists holds only at fixed
`m`.

The row strong-scaled at the previous size (`6912 1347840 6913 32`, gains per
doubling of 1.17x, 1.66x, 1.81x to 3.50x over eight nodes at 15 workers a
node), and that is why it carries no restructured tier: the structure a
restructure would attack is the boundary handoff -- one block of 16 bytes per
phase per rank -- and the knob that batches them exists already, as the group
factor the benchmark forbids.  Two caveats on that trend, both of which the
next campaign settles: the run table is not reproducible from anything under
`logs/` (only a single 1-node anchor is), and the shape may be partly the
application's own contiguous map rather than the pipeline widening — under
BLOCK the runnable rank interval covers only `⌈W/block⌉` of the nodes, so the
machine is fully engaged only at the crest, and that coverage penalty eases as
`block` shrinks with the node count.  Re-measure before quoting either the
ratio or the mechanism — and note that the hinted tier below is precisely the
experiment that separates them, since it removes the coverage penalty and
changes nothing else.

## Placement (hinted)

The guarded layer overrides exactly one thing: the rank -> policy-domain map in
`realMainEdt`.  base keeps the published contiguous map (`myPD = i / block`,
`block = ceil(p / count)`); under `OCR_APP_OPTIMIZED_PLACEMENT` the same ranks
are dealt to the domains in runs of `chunk`
(`myPD = (i / chunk) % count`, with `chunk = argv[6]` clamped to `block` when it
is 0 or larger than a block, which reproduces base's map exactly).  Nothing else
is under the guard: same task graph, same DB and event counts, same wiring, same
`AFFINITY` pinning of each chain to the domain its rank was born on, and `chunk`
is parsed identically in the unguarded tier and left unused there.

**Why.** The runnable set of the wavefront at sweep step `s` is the *contiguous*
rank interval `[max(0, s-w), min(p-1, s-1)]`.  A contiguous map sends that
interval to a contiguous *domain* interval, so it covers `~W/block` domains and
each of them holds up to `block` runnable ranks while it can run only `workers`
of them: for most of a sweep the machine is engaged in one corner.  Dealing the
same interval in runs of `chunk` spreads it over `min(count, W/chunk)` domains
instead.

**Choosing `chunk`.**  With `chunk` at or below the per-node worker count every
engaged domain saturates exactly once, and the coverage is the same for every
such value; above it the map decays back toward base.  The crossing count is
`p / chunk` (an edge crosses only where a chunk ends), so the largest value at
the knee — `chunk` = the per-node worker count — is the one that buys the
coverage at the fewest crossings.  At `p = 6912` on 32 nodes with 108 workers a
node that is 64 crossings per generation index out of 6912 handoffs (0.93 %)
against the contiguous map's 32 (0.46 %) — both far from the wrap map's 100 %.

**Load balance (R2).**  Ranks per domain are `chunk` per chunk dealt, so the map
is exactly balanced whenever `chunk` divides `p` and the node count divides
`p / chunk`.  At `chunk = 108` and `p` a multiple of 3456 the imbalance is
1.000 at 2, 8, 16 and 32 nodes (and at 1 and 4); the same divisibility rule the
contiguous map already needed.  No domain is ever empty and no rank is pinned to
the creating domain, so nothing funnels to rank 0.

**What it should change (R3).**  A coverage model of the runnable set — busy
workers averaged over a timestep, `p = w = 6912`, 108 workers a node,
`chunk = 108` — puts the guarded map at **1.33x / 1.57x / 1.69x / 1.67x / 1.48x**
the contiguous map at 2 / 4 / 8 / 16 / 32 nodes.  That is a mechanism change,
not a trivial gain, which is why the layer exists; whether it is kept is the
campaign's decision on measured cells, not this model's.

**What is *not* evidence.**  An earlier layer on this row homed the per-phase
boundary block at its consumer and was measured at 0.87x / 0.99x / 0.97x on two
/ four / eight nodes and removed.  That layer moved the DB home, not the map,
and it addressed a sub-percent of handoffs: cutting a `p`-cycle into `L`
balanced parts cuts at least `L` edges, and the contiguous map already achieves
that bound.  Neither those runs nor the `11x` figure once quoted for forcing the
wrap map survives under `logs/` — and the wrap map cannot even be selected from
this tree, since `BLOCK` is an unguarded `#define` with no `-D` escape, so that
figure was an out-of-tree edit.  None of it carries a verdict on the map
override above.

**Verdict (measured).**  The chunk map above is the one alternative the
pipeline admits, and it was measured on the local trend roster (INV × WB,
15w+1p × 1/2/4/8, `chunk` = the per-node worker count): base 8.41 / 9.38 /
10.69 / 12.94 s against hinted 8.44 / 108.24 / 58.13 / 38.95 s -- 11.5x /
5.4x / 3.0x slower at two, four and eight nodes.  So hinted was tried and no
map beats the base: the base's own contiguous map is the best hinted map, the
base tier stands in for the hinted comparison (the catalog carries
`hinted: false`), and the layer stays in the source behind its guard.
