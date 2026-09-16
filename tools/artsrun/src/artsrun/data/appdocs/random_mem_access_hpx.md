# random_mem_access_hpx

*A distributed array of counters under lost-update stress — random remote
increments, gathered back into a delivered-update oracle.*
Origin: HPX's `random_mem_access` example
(`third_party/hpx/examples/random_mem_access/`, pin `v1.11.0`), copied to
`benchmarks/hpx/random_mem_access_hpx/` and built as
`random_mem_access_hpx_hpx`.

## Overview

Models a distributed array of counters: `array-size` component instances
are laid out block-wise over every locality, each is `init`ialized to its
own index, `iterations` random accesses each fire an `add` action at the
chosen element's home, and a final gather sums every element's net
increment — which must equal `iterations` exactly (a lost update is the
defect this row exists to catch). One program on four runtimes —
`random_mem_access_hpx_hpx` is the HPX program,
`random_mem_access_hpx_arts_<variant>`, `random_mem_access_hpx_xsocr` and
`random_mem_access_hpx_ocrvx` are the OCR mirror
(`benchmarks/apps/hpx_origin/random_mem_access_hpx.c`), one row in
`hpx_apps.yaml`. HPX primitives used: an HPX component
(`server::random_mem_access`), `hpx::default_layout`, `hpx::async`,
`hpx::wait_all`. The mirror's mapping: one 32-byte DB per element homed at
its owning rank, one independent task per init/update/query hinted to that
owner and guarded by a spinlock inside the element's own DB, and a single
flat fold over every element's final count producing the `COUNT_SUM` oracle.

## Parameters

| option | meaning | origin default | CLI reachability |
|---|---|---|---|
| `--array-size` | number of elements (components) | 8 | reachable |
| `--iterations` | number of random updates | 16 | reachable |
| `--seed` | seed of the random index sequence | a `random_device` draw (the mirror draws the same entropy with `getrandom(2)`, which is what `random_device` reads on this platform; the wall clock and the process id are its fallback) | reachable — an addition over the origin, disclosed in `ORIGIN.md` (A4), so both sides can be pointed at the same sequence |

A malformed argument, or an `--array-size` above `2^31`, prints usage and
calls `ocrShutdown()` with status 0, where the origin's option parsing exits
non-zero; the missing `COUNT_SUM`/`[APP_E2E]` marker fails the cell either
way.

## Structure

Let `n` = `array-size`, `k` = `iterations`. Per run:

| object | count | size |
|---|---|---|
| element DBs | `n` | 32 bytes (`atomic_uint locked`, `u64 count`, `u64 initial`, `uint32_t prefix`, padded) |
| element pointer table | 1 | `n` GUIDs |
| init tasks | `n` — one per element, each with an output event | — |
| update tasks | `k` — one per random draw, each independent and each with an output event | — |
| query tasks | `n` — one per element, each producing an 8-byte count DB | 8 bytes |
| report task | 1 — creates the `n` query tasks and the summer task once the update phase's latch fires | — |
| summer task | 1 — a single flat fold over the `n` query results | — |
| latch events | 2 per run — the `n`-count init latch, the `k`-count update-done latch | — |

The task count is exactly `k`: one increment is one task, never batched,
whatever the draw. Every update task is independent — created and dispatched
in draw order, the same order the origin issues its `add_async` calls in —
and the element's own DB-contained spinlock, not a task-level chain, is what
keeps concurrent updates on one element from racing.

## Wiring

**The component-scoped lock.** OCR's `DB_MODE_RW` is not exclusive — the
standard puts the burden of ordering concurrent writers on the program, and
all four runtimes grant two concurrent RW holders of one block — so a set of
sibling update tasks on one element, run concurrently under RW, would race,
lose increments, and break the very oracle this row exists to state. The
origin does not race: its component is a
`hpx::components::locking_hook<component_base<…>>`, so every `add()` on an
element runs under that element's own mutex. The mirror carries that
exclusion inside the element's own DB rather than as a task-level edge: an
`atomic_uint` field is a spinlock (`element_lock`/`element_unlock`,
acquire/release ordered) that every init, update and query task on that
element takes and drops within its own body, around the read or the
increment, before releasing the block. Each update task is independent —
created and dispatched in the draw order the mirror generates, exactly as
the origin issues its `add_async` calls in that order — and the spinlock,
not a chain of output events, is what makes the increments land one at a
time.

Two latch-based joins: an `n`-count init latch gates the update phase
(mirrors `wait_all(inits)`), and a `k`-count update-done latch gates the
report (mirrors `wait_all(barrier)`). **Both latches are decremented by an
output event, never from a task body.** An OCR EDT releases its data blocks
before its output event is satisfied; a satisfy issued from inside the body
carries no such guarantee, so a join built on one can fire while the last
writer's block is still held and unpublished, and the reader it releases
can then see a pre-write version. Each init task's output event decrements
`inited`, each update task's decrements `done`, and the count is therefore a
count of completed releases.

The report's gather is a single flat reply-by-dependence fold: `report_edt`
creates the `n`-slot `summer_edt` first, then creates the `n` query tasks
(each hinted to its element's owner), wiring each query's 8-byte result
output event into the summer's slot `i` (`DB_MODE_RO`) and the element DB
into the query's own slot (`DB_MODE_RW`). `summer_edt` sums `value[i] - i`
over its `n` slots in index order — the origin's own `sum += counts[i] - i`
loop — prints `COUNT_SUM`, destroys the `n` result DBs, and shuts down.

A latch and an EDT's output event are both once-type OCR events: each is
destroyed the instant it fires, so every consumer must already be registered
on it before it can fire — an EDT create followed "eventually" by
`ocrAddDependence` is not soon enough when the party that fires it is
already runnable. `updates_edt` and `report_edt` both follow that rule: each
join's consumer EDT (`updates_edt` on `inited`, `report_edt` on `done`, the
summer and every query task on their own output events) is created and
registered before any task that will decrement it is created.

## Flow

`mainEdt` validates the arguments and seed, creates every element DB
hinted at its owner, snapshots the element table's GUIDs into a local
array and releases the table DB, then creates the `n`-count `inited`
latch and `updates_edt` — wiring `updates_edt` to both `inited` (slot 0)
and the table (slot 1) immediately. Only then does it create the `n` init
tasks (from the local snapshot), each gated on its own element DB, its
output event registered on `inited` before that DB dependence is added.

`updates_edt` follows the same order: it creates the `k`-count `done` latch,
creates `report_edt` and wires it to both `done` (slot 0) and the table
(slot 1), and only then touches the update phase. It seeds an `mt19937`
stream from `--seed` and, for each of the `k` iterations in turn, draws the
next bounded index and immediately creates that update task (hinted to the
drawn element's owner) — draw, then issue, in the same order the origin's
loop draws and posts `add_async` — wiring each update's output event to
`done`'s decrement. `report_edt` runs once `done` fires: it creates the flat
summer task and the `n` query tasks, and the summer prints `COUNT_SUM <sum>`
and shuts down once every query has replied.

`--iterations=0` is not a hang: a latch created at zero is never *carried*
to zero and so never fires, so an empty update phase gets a counter of one
and one explicit decrement, and the program prints `COUNT_SUM 0` and shuts
down the way the origin's `wait_all` on an empty vector does.

The mirror's `mirror_mt_bounded` reproduces libstdc++'s
`uniform_int_distribution<>(0, array_size-1)` algorithm for a full-range
32-bit engine exactly — the same multiply-high downscaling with rejection,
the same `1812433253`-based seed, the same twist/temper — so at the same
seed the two sides draw the identical index sequence; the oracle does not
depend on this (it counts delivered updates, not which element received
them), but the sequences are no longer different by construction.

| HPX wait site | classification | mirror |
|---|---|---|
| `hpx::wait_all(inits)` | end wait — join of every init | the `n`-count `inited` latch |
| `hpx::wait_all(barrier)`, the `add_async` posts | end wait — join of every update | the `k`-count `done` latch |
| `hpx::wait_all(counts)`, the `query_async` gather | end wait — join of every element's final count | the flat `n`-slot summer fed by the `n` query tasks |

**Mid waits: 0.** All three sites above are end waits; no stage body blocks
on a future and then computes.

`[APP_E2E]` starts in `mainEdt`, after option/seed parsing and the
`ocrAffinityCount` call and before the element DBs are created (the six
template creations immediately after the stamp have no HPX-side
counterpart, so they sit inside the mirror's interval only — a few
microseconds against a multi-second cell), and stops in `summer_edt` after
the fold, before `COUNT_SUM` and before the result DBs are destroyed. HPX's
`run_clock` opens after the option reads and the seed selection, before
`hpx::new_<>`, and `print_e2e` closes right after the `sum += counts[i]-i`
loop, before `COUNT_SUM` and before the component array's destructor. The
runtime's own `[E2E]` is still printed on both sides and kept as a separate
observation.

## Placement

`hpx::default_layout` block-distributes `array-size` components over every
locality: `ceil(n / nl)` elements to each of the first `nl - 1`
localities, the remainder to the last. The mirror's `owner(i, n, nl)`
computes the identical block map, and every element DB, its init task, and
every update task addressed to it carry
`ocrAffinityGetAt(AFFINITY_PD, owner(i, n, nl))` as an explicit hint — so,
as in the origin (where `add`'s action runs at the component, not at the
caller), every update is a task at the element's home rather than a
remote read-modify-write. Each query's 8-byte result DB carries an explicit
home hint to the rank that writes it (the element's own rank), so its
placement is stated rather than taken from the build's no-hint DB policy.

The origin's component is `hpx::components::locking_hook<component_base<…>>`:
co-locating every `add()` on an element with that element is only half of
what the origin states, the other half being that those calls run one at a
time under the component's own mutex. The DB-contained spinlock is the
translation of that lock, not a mirror-side invention — and because the
lock admits its callers in an unspecified order, independent update tasks
taking it in draw order is a legal interleaving of the origin's own
critical section.

`owner()` has a branch for `items < nl`: fewer elements than localities go
one each to the first `items` of them. This implements the intended rule of
the layout; the vendored `default_distribution_policy` computes
`(items < num_loc) ? 1 : 0` per locality there, which hands out more
elements than exist (3 items over 8 localities gives four localities one
each). The roster's sizes never reach the branch on either side — the
smallest `--array-size` in use is far above any node count the profiles
sweep — so the two cannot disagree in a measured cell; the mirror
implements the rule rather than the upstream arithmetic, and this is the
disclosure of that.

## Sizing

The width knob is `iterations`; `array-size` sets object count rather than
update volume, and is bounded above by `2^31` (`mainEdt` rejects anything
larger as usage — the flat summer's dependence count is `n`, one slot per
element). The `hpx-gate` roster runs
`--array-size=4096 --iterations=200000 --seed=1`, finishing in well under
a few seconds. The measurement argument is the calibrated one at the end of this section.

**Calibrated arguments.** `--array-size=4096 --iterations=2100000 --seed=1`
at every node count; the pin is the update count itself. Derived to the 10 s
window from the probe through the linear law; the sizing pass measured
8.4–9.7 s on the ARTS arms and 11.5 s on HPX, 5.4 GB resident.
