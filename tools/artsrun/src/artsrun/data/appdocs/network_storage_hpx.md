# network_storage_hpx

*Bulk put and get between per-locality storage arrays — every rank sends a
fixed-size transfer into some rank's storage for every slot it owns, pass
after pass, then pulls as many back, between barriers every rank enters.*
Origin: HPX's `network_storage` performance test
(`third_party/hpx/tests/performance/network/network_storage/network_storage.cpp`
and the `simple_profiler.hpp` it includes, pin `v1.11.0`), copied to
`benchmarks/hpx/network_storage_hpx/` and built as `network_storage_hpx_hpx`.

## Overview

`hpx_main` runs on every locality. Each allocates `localMB` of storage as
one uninitialised `new char[]` and cuts it, by offset arithmetic, into
`slots = localMB MiB / transferKB KiB` transfer slots. It then runs three
tests in turn: `test_write` once as a warm-up with one iteration,
`test_write` with `iterations`, and `test_read` with `iterations`. Every test
opens and closes with `hpx::distributed::barrier::synchronize()`. One
iteration is a pass over the slots: for slot `i` the destination locality is
`(rank + i) % nranks` under the default `--distribution=1` (a uniform random
locality under `--distribution=0`), redrawn at random while `--no-local`
keeps it on the sender, and the offset is a uniform random slot. A write
posts the plain action `CopyToStorage` to the destination with the sender's
own slot `i` wrapped by reference in a `serialize_buffer`; the action copies
it into the destination's storage at the offset. A read posts
`CopyFromStorage`, which copies the destination's slot into a buffer of its
own and returns it; the reply is deserialised through a pointer allocator
straight into the *asker's* storage at the same offset. A pass collects its
futures and joins them with `when_all(...).then(reduce)` and `.get()` before
the next pass starts. Without `--all-to-all` only locality 0 transfers; the
others still enter every barrier.

Three facts of this version, read from the code, govern the row.
`--semaphore` is dead: `USE_CLEANING_THREAD` and `USE_PARCELPORT_THREAD` are
commented out, so the sliding semaphore, the future-cleaning thread and the
parcelport thread are compiled out, and the option is parsed, stored and
never read. Nothing is verified: `copy_to_local_storage` and
`copy_from_local_storage` return `TEST_SUCCESS` unconditionally and `reduce`
only folds those, so the scalar counts completed transfers. And the storage
has concurrent writers by construction — see Wiring.

One program on four runtimes: `network_storage_hpx_hpx` is the HPX program,
`network_storage_hpx_arts_<variant>`, `network_storage_hpx_xsocr` and
`network_storage_hpx_ocrvx` are the OCR mirror
(`benchmarks/apps/hpx_origin/network_storage_hpx.c`), one row in
`hpx_apps.yaml`. HPX primitives used: two plain actions
(`HPX_DEFINE_PLAIN_ACTION`), `hpx::async_cb` and `hpx::async` under
`launch::fork`, `serialize_buffer` (by reference out, through a pointer
allocator back), `when_all(...).then(...)`, `distributed::barrier`, and — for
the scalar edit only — a communicator and `all_reduce`. The mirror's mapping:
one block per slot, one put task per `CopyToStorage` at the destination rank,
one get task per `CopyFromStorage` at the rank that owns the slot read and a
landing task at the asker for its reply, one completion task per transfer at
the requester (the origin's continuation), one turn task per rank per
pass holding the pass's latch, one barrier task on rank 0 per
`synchronize()`, and the slot names exchanged once, since an OCR program
cannot address another rank's storage by offset.

## Parameters

| option | meaning | origin default | CLI reachability |
|---|---|---|---|
| `--localMB` | storage per locality, MB | 256 | reachable |
| `--globalMB` | storage for the job; when positive it replaces `localMB` with `globalMB / nranks` | 0 | reachable |
| `--transferKB` | size of one transfer and of one slot, KB | 64 | reachable |
| `--iterations` | passes of the write test and of the read test (the warm-up is always one) | 5 | reachable |
| `--all-to-all` | every locality transfers; when false only locality 0 does | true | reachable |
| `--no-local` | a transfer never targets its sender (redrawn at random) | false | reachable |
| `--distribution` | 0 = random destination, anything else = block-cyclic | 1 | reachable |
| `--semaphore` | in-flight limit — read by no compiled code in this version | 16 | reachable, inert on both sides |
| `--parceltype` | a label for the origin's CSV report | `unknown` | reachable, inert on both sides |

The row runs `--globalMB`, not `--localMB`: the program divides a global
store among the localities itself, so one argument list is the same job at
every node count (see Sizing), where a per-locality store would grow the job
with the nodes.

Both booleans take the spellings the origin's option library accepts,
without regard to case (`true`/`yes`/`on`/`1`, `false`/`no`/`off`/`0`, and
an empty value for true); anything else is malformed and is usage, as is any
value that will not parse and a key given with no value.

The mirror also rejects, as usage, what the origin cannot run as stated:
`--transferKB=0` (the origin divides by it and dies of a floating-point
exception); `2^31` slots or more (the origin's slot index is an `int`);
`--no-local` on one rank (the origin prints a fatal error and terminates);
more than 16384 ranks (the origin's compile-time `MAX_RANKS`, where it
prints an error and returns); and `18 · ranks² ≥ 2^32`, the mirror's own
rendezvous reservation, which only rank counts above about 15,400 reach. Any
of these prints usage and calls `ocrShutdown()` with status 0, where the
origin's own rejections exit non-zero or terminate; the missing
`TRANSFERS_OK` marker fails the cell either way. A
storage smaller than one transfer — `--localMB=0`, a `transferKB` larger
than the storage, a `--globalMB` smaller than one transfer per rank — has
zero slots, and both sides run it: every test and every barrier happens,
each pass joins nothing, and the line reads `TRANSFERS_OK 0 0`.

## Structure

Let `nl` = ranks, `S` = slots per rank, `I` = iterations, and `A` = the
transferring ranks (`nl` with `--all-to-all`, 1 without). Per run:

| object | count | size |
|---|---|---|
| driver tasks | `nl` — the SPMD fork | — |
| slot blocks | `nl·S` — a rank's storage, cut at the transfer unit | `transferKB` KiB each |
| slot tables | `nl` — a rank's slot names, published to every rank | `S` GUIDs |
| state blocks | `nl` — a rank's stream and every rank's slot names, carried turn to turn | an `mt19937` state plus `nl·S` GUIDs |
| turn tasks | `2I + 4` per transferring rank (a pass turn per pass plus a closing turn per test), 3 per other rank | the first `nl + 2` dependences; a turn that follows a pass 4 (the pass's join, its results and order blocks, the state); a test's first turn 2 |
| put tasks | `A·(I + 1)·S` — the warm-up included, at the destination rank | 3 dependences — destination slot RW, the slot a straddling transfer would spill into (always NULL: an address is a whole number of transfers), sender's slot RO (NULL when it is the destination slot itself) |
| success blocks | `A·(I + 1)·S` — a put's `TEST_SUCCESS`, created at the destination, destroyed by the requester's completion | 4 bytes |
| get tasks | `A·I·S` — at the rank that owns the slot read | 2 dependences — the owner's slot RO, the spill slot (NULL) |
| reply blocks | `A·I·S` — a get's payload, created at the owner, destroyed by the landing task | `transferKB` KiB each |
| landing tasks | `A·I·S` — at the asker | 3 dependences — the asker's slot RW, the spill slot (NULL), the reply block RO |
| completion tasks | `A·(2I + 1)·S` — one per transfer, at the requester | 3 dependences — the put's success block RO or the landing's output, the rank's counters block RW, the pass's results block RW |
| results and order blocks | `2·A·(2I + 1)` — one pair per pass, destroyed by the next turn | `S` ints and `S` indices |
| counters blocks | `nl` | two counts and the origin's own `MAX_RANKS` (16 384) in-flight counts, the size of its `FuturesWaiting` array |
| pass joins | `A·(2I + 1)` latches, counting `S` output events each | — |
| relay tasks | `2·nl` — one per rank between consecutive barriers | 1 dependence |
| barrier tasks | 6, on rank 0 | `nl` dependences |
| tally tasks and blocks | `nl` | 16 bytes |
| final task | 1, on rank 0 | `nl` dependences |
| table points | `nl²`, of which `nl² − nl` cross a rank | — |
| entry and release points | `12·nl`, of which `12·(nl − 1)` cross | — |

The point reservation is `18·nl²` names: three kinds for every unit, the
largest unit space being the entries' (six barriers times `nl` senders),
each for every consumer rank.

Read against the origin: the put and get tasks are its actions one for one,
and the slot blocks are its storage — the same bytes, cut where every
transfer cuts it, since a transfer reads or writes exactly one slot and an
OCR dependence is taken on a whole block. The turn tasks are its driver
loop, one per pass; the barrier and relay tasks are its six
`synchronize()` calls; the joins are its per-pass `when_all`. What the
origin does not have is the name service: the tables, their points and the
`nl·S`-GUID map inside each state block, which an offset address makes
unnecessary on the HPX side. Per pass the mirror allocates what the origin
allocates per pass: a result per put (the origin's action returns its
`TEST_SUCCESS` through the future, the mirror's put task through a 4-byte
block the requester fetches and destroys), a transfer-sized reply per get
(the origin's `serialize_buffer` reply), and the pass's result and order
vectors. The slot blocks live until the rank's tally task destroys them,
where the origin's `delete_local_storage()` frees its array — inside the
span on both sides, though not the same work: the origin's is one
synchronous `delete[]` of the whole array on each locality, and locality 0's
stamp does not wait for the others', while an `ocrDbDestroy` hands each
block back to the runtime (plus a message per rank that still holds a copy
of it) and the final task waits for every rank's tally.

## Wiring

**An OCR data-block dependence is satisfied when it is *added*, not when the
block is written**, so every handover is checked against the rail:

* **The slots need no point.** A driver creates its slots with
  `DB_PROP_NO_ACQUIRE`, so no task holds them, before it publishes their
  names; every transfer that names a slot is created by a turn that has
  already received every rank's names, so the blocks exist and are free when
  their first consumer's slot is filled. No task is a slot's producer for
  another — which bytes a transfer copies is exactly what the origin leaves
  unordered.
* **The names need one.** A rank cannot guess another rank's slot GUIDs, so
  each driver writes its names into a table homed at itself, releases it,
  and satisfies one labeled STICKY point per consumer rank; each rank's first
  turn registers on all `nl` of them, copies the tables into its state block
  and destroys its points. The index is `mirror_edge`'s: one producer and one
  consumer per point, never two units of work on one. Where a labeled range is
  homed by index — ARTS and ocr-vx both spread it `index % nranks` — a point's
  home is its consumer and a within-rank publish is no message; where the whole
  reserved range is homed at the PD that reserved it, as on xsocr, each satisfy
  is a message to that PD and a forward from it. A table has `nl`
  readers and so no single destroyer; it lives to teardown.
* **The state block needs no point, and is released to earn that.** It is
  written by every turn of its rank (the stream advances), and each turn
  releases it before adding it — last — to its single successor.
* **The tally blocks need no point.** A tally task writes its counts,
  releases the block and only then adds it to the final task, which destroys
  it.
* **The barriers are signals.** An entry point carries one rank's arrival at
  a barrier to rank 0, a release point carries the barrier's end back to one
  rank; both carry no block and are raised from task bodies.

**Nothing orders two transfers that land on one slot.** Two puts can draw
the same destination slot; two gets on one rank can draw the same offset,
and each writes the asker's slot there; and a transfer can read a slot that
another is writing. The origin's storage is a bare `char` array with no lock
(`std::copy` into it from any number of action threads), and the mirror adds
no chain of write turns, for three reasons in order: a chain would need a
cross-rank sequencer the origin does not have, which means messages the
origin never sends; the program is a bandwidth test whose storage is
deliberately unsynchronised, and repairing it would make the two columns
different programs; and the result does not read the racing bytes. This
row's *payload* is therefore outside DB-WRF at block granularity: the
counts survive only because every completion runs on the counters' home
rank — an implementation accident, not program ordering. It is the one
row of the section with that property.

It is also a row in which a block is read from another rank while its home
writes it, so how each coherence protocol treats a reader of a block under
write is part of what the row exercises — none of it added by the mirror.
VAL serves a reader the owner's current buffer without waiting; INV serves
it and invalidates the copy at the writer's release; EXCL holds it until the
writers release; XSOCR's lockable blocks defer a remote read while local
writers hold the block. None of them can wedge on the crossing pairs this
program makes (rank `a` reading `b`'s slot into its own while `b` reads
`a`'s into its own): ARTS issues in GUID order every acquire its protocol
lets wait on another task's release, and XSOCR and OCR-vx take all of a
task's blocks in GUID order.

**A put whose source and destination slot name the same block copies inside
its own RW acquisition, never on a second slot.** A put from slot `i` onto
the same rank's offset `i` would otherwise name one block twice — read-write
on the destination slot and read-only on a source slot — which OCR 1.2
declares undefined for a block repeated in one task's dependences under
differing access modes. `issue()` detects the alias — `to == rank` and
(`block == i`, or, for a transfer that straddles two blocks, `block + 1 ==
i`) — and passes `P_SRC_ALIAS` instead of adding a third dependence: `put_edt`
then reads the source bytes straight out of the RW-acquired destination
block it already holds (`depv[alias-1]`, at the drawn offset), one buffer
and one `memmove`, exactly the origin's `std::copy(src, src+len, dest)` with
`src == dest`. A non-aliasing put still carries the sender's slot RO on its
own slot. A get never aliases this way: the destination-rank read
(`get_edt`) and the requester-rank write into its own storage (`land_edt`)
are two separate EDTs even when the asker is also the destination — a
legitimate concurrent access to two different blocks, exactly as in the
origin.

**The joins are per pass and per rank, because the origin's are.** A pass
turn creates a latch counting its `S` transfers, creates and registers the
next turn on it before the first transfer exists (a latch is once-type), and
counts each transfer's output event into it before adding that transfer's
blocks, the writable one last. A pass over zero slots is the one empty
join: a latch created at zero never fires, so it counts one and the turn
decrements it once itself, which is the origin's `when_all` of an empty list
completing at once. A test that gives a rank no pass at all — the write and
read tests at `--iterations=0`, every test of a rank that `--all-to-all`
leaves idle — has no join: its first turn is already its closing turn.

**The barriers are collective, because the origin's are.** Each
`synchronize()` is one task on rank 0 that waits on one entry point per rank
and raises one release point per rank — the star HPX's barrier node forms
around its root. A rank's closing turn enters the barrier that ends its
test; between two tests a relay task, released from that barrier, enters the
one that opens the next, where the next test's first turn waits.

**The end is collective too.** The final task waits on every rank's tally,
each behind the barrier that closes the read test, which no rank enters
before its last pass has joined.

## Flow

`mainEdt` validates the arguments, reserves the `18·nl²` point names, creates
the ten templates, creates the final task and the six barrier tasks on
rank 0 (each registered on its `nl` entry points), and forks one driver per
rank.

A driver creates its table, its state block (the stream seeded with the
default `mt19937` seed, 5489) and its `S` slots, publishes the table on one
point per rank, creates its first turn on the release point of the barrier
that opens the warm-up, the `nl` table points and the state block, and
enters that barrier.

A pass turn draws, for each slot `i` in order, the destination — `(rank + i)
% nl`, or a uniform draw under `--distribution=0`, redrawn while
`--no-local` leaves it on the sender — and then the offset, exactly the
origin's draw order, and issues one transfer: in the write tests a put task
on the destination rank reading the sender's slot `i` and writing the
destination's slot at the offset; in the read test a get task on the
destination rank copying its slot at the offset into a reply block, and a
landing task on the rank itself writing that reply into its own slot
there. Either way a completion task on the issuing rank counts the transfer
and records its result, as the origin's continuation does. After the last pass of a test, a closing turn enters the barrier that
ends the test and wires what follows it. The final task prints
`TRANSFERS_OK <puts> <gets>` and calls `ocrShutdown()`.

| HPX wait site | classification | mirror |
|---|---|---|
| `distributed::barrier::synchronize()`, at the start and the end of each test | driver wait — collective, six per locality | a barrier task on rank 0 per call, entered and left through one point per rank |
| `when_all(final_list).then(reduce)` and `result.get()`, once per pass | driver wait — the pass's join, per locality | the per-rank per-pass latch of `S` output events, gating the next turn |
| the write continuation's `fut.get()` | start wait — a ready future inside its continuation | the put task's output event |
| `transfer_data`'s `f.get()` and the counting continuation | start wait — the reply, already arrived | the landing task's read-only dependence on the get task's reply block |
| the tally `all_reduce(...).get()` | end collective, before the end stamp | a tally task per rank feeding the final task |

Mid waits: 0.

**The scalar.** The origin already folds each transfer's `TEST_SUCCESS` in a
continuation; the edit counts those completions in two per-locality atomics
and sums them over localities before the end stamp, inside the span. The warm-up runs through
the same write continuation, so it is counted: with `--localMB=8` the line
reads `384 256` at one locality and `768 512` at two, and with the row's
`--globalMB=16` it reads `768 512` at every geometry the gate runs. The
mirror counts the same transfers where the origin counts them — at the
requester, in the transfer's completion task, against the requester's own
counters block. The
mirror's stream is the origin's generator at the origin's default seed
(5489), and its bounded draw reproduces libstdc++'s
`uniform_int_distribution` algorithm exactly, so at the same seed the two
sides draw the identical sequence of destinations and offsets; the counts
do not depend on this, since they are not the property this row's oracle
checks.

**One bug fixed in the copy.** The origin's pointer allocator — the one that
lands a remote read's reply in the asker's storage — checks, when that reply
buffer is released, `HPX_TEST_EQ(p == pointer_ && n, size_)`. The macro takes
two operands, so it compares the `bool` `p == pointer_ && n` with `size_`,
the transfer size in bytes: `1` against a multiple of 1024 (the size is at
least 1024 in any run that gets past the slot division, and a zero transfer
size dies there first), which can never be equal. The check its author meant
reads `p == pointer_ && n == size_`, which is exactly what the failure
message prints. It failed for every read that crossed a locality, each time
taking the test framework's lock and writing a line to standard error on the
asking locality, inside the measured window; it never stopped the program.
The copy comments that one line out, under the `bugfix` class.

This program exposed the defect the HPX build's local allocator patch fixes:
an HPX thread that suspends can resume on another worker, and HPX's
inlinable thread-local cache lookup, reached across that suspension, left a
continuation cache and the destructor registered for it with the previous
worker, so the build keeps that lookup out of line (`benchmarks/hpx/README.md`,
"The configuration every table was measured on").

The measurement is `[E2E]` on both sides, stamped on rank/locality 0 alone:
the whole application, from its first statement to the point it asks the
runtime to stop, runtime start-up and teardown excluded. On the OCR side the
runtime stamps it (the main task becoming eligible, shutdown recognised on
rank 0). On the HPX side every locality runs `hpx_main`; `run_clock` opens as
its first statement and `print_e2e` closes it, on locality 0, immediately
before the `hpx::finalize()` only that locality calls — after the tally
`all_reduce` every locality takes part in, so locality 0's end is the
application's. Option handling, the storage allocation, the warm-up, the
write and read tests with their barriers and reports, the tally and the
release of the storage are inside the span on both sides. The mirror's
span additionally holds representation work the origin has no counterpart
for — the rank's table/state DBs and the per-rank labeled STICKY slot-name
rendezvous (`nl²` events overall), since the origin passes a raw pointer in
its action arguments instead of naming a block.

Inside the span on the HPX side only: every put's keep-alive bookkeeping —
inserting a `shared_ptr` into a map under a spinlock, later erased from a
parcel callback under the same lock — which is HPX's own send-buffer
lifetime mechanism, the job the mirror's DB dependence performs
structurally; it is application-level code in the origin's own translation
unit and it is not free (two spinlock acquisitions and a map insert/erase
per transfer), so it stays disclosed as a measurement asymmetry rather than
credited to the mirror. The origin's own `high_resolution_timer`s time each
test separately — the warm-up's time is printed too; only its CSV record and
its profile table are suppressed — and the measured span holds the warm-up
on both sides, since the mirror performs it too.

## Placement

A rank's slots, its table and its state block are homed at the rank, which
is where the origin's storage and driver state live. A put task runs at the
destination rank, where `CopyToStorage` runs; a get task runs at the rank
that owns the slot, where `CopyFromStorage` runs, and its reply block moves
back to the landing task at the asker. Completion, turn, relay, tally and driver tasks run
at their rank; the barrier tasks and the final task run on rank 0, the
barrier's root. Every task and block carries an explicit hint; nothing is
left to the build's no-hint policy. Points are homed at their consumers
wherever a labeled range is homed by index (ARTS, ocr-vx); on a runtime that
homes a whole reserved range at the PD that reserved it (xsocr) they all live
at that one PD, and each satisfy costs a message there and a forward.

What crosses a rank is what the origin moves: a crossing put fetches the
sender's slot to the destination, and a crossing get fetches the reply block
to the asker, in the direction of the origin's action payload and reply. The messages that carry it are the runtime's: a crossing
transfer also costs its remote task creation, its dependence registrations
and its completion signal, against the origin's action parcel and its reply;
a crossing put's result is a block the requester fetches and then destroys
at its home across the rank, where the origin's int rides the reply parcel,
and a crossing get's reply block is destroyed from the asker, where the
origin frees its owner-side buffer locally.
The count per crossing transfer is not measured here. Because a slot read
across ranks is also written at its home, a reader's copy of it goes stale
between reads — invalidated at the writer's release under INV, re-validated
at the next read under VAL — and that traffic is the coherence plane's cost
on the access pattern the program states. The same plane can also spare
bytes the origin always moves: a put's source is the sender's slot `i`, read
by the same destination every pass under the default distribution, and a
slot nobody wrote since that destination last fetched it is served without
its payload — a header-only reply under VAL, no message at all under INV —
where the origin serialises the transfer's bytes into every `CopyToStorage`
parcel. How many puts that covers follows the drawn offsets (a slot is
rewritten when another rank's put or the rank's own read lands on it) and is
not measured here: none at one rank, where nothing crosses, and at two ranks
and more about the `(1 − 1/S)^S ≈ 37 %` of slots that one pass of `S`
uniform draws leaves unwritten. A reader caching what it read is the plane's
design, not an edit to the program, and EXCL's purge arm and FLUSH never do
it. The name tables cross once, at
setup: about four messages for each (owner, consumer) pair on different
ranks — the owner's remote labeled create, its satisfy, and the consumer's
read-only acquire request and payload reply — and none within a rank. A
barrier costs each non-root rank about four — the remote labeled create and
the satisfy of its entry point, then the same for its release point — and
rank 0 none; HPX's star sends one action and one reply per non-root
locality.

## Sizing

The in-flight width per rank is its slot count, `localMB·1024 / transferKB`:
no semaphore is compiled in, so every transfer of a pass is issued before
its join. The slot count sets both the width and the bytes per pass;
`iterations` sets the height; `transferKB` sets how the same bytes are cut.

The row runs strong scaling through the origin's own `--globalMB=G`: each of
the `n` localities gets `G/n` MB and `(G/n)·1024/transferKB` slots, so the
store, the slot blocks and — with `--all-to-all` — both counts are fixed
totals: `(I + 1)·G·1024/transferKB` puts and `I·G·1024/transferKB` gets.
That holds exactly under three conditions: `--all-to-all` is true (without
it only locality 0 transfers, and the counts shrink as `1/n`), `n` divides
`G`, and `transferKB` divides `(G/n)·1024`. A geometry that breaks the
second or the third truncates the division and runs a smaller job. The pin
on the put count rests on all three.

Limits a calibration must respect, all taken at the one-node anchor, where a
locality's share is the whole store: `G ≤ 4096` MB, because the origin's
offsets are `uint32_t` and a larger store makes them wrap onto lower slots
(the counts would not change, the bytes would land elsewhere); fewer than
`2^31` slots, `G·1024/transferKB < 2^31`, because the origin's slot index is
an `int`; and the mirror's name map, `n·S = G·1024/transferKB` GUIDs per
rank on top of the storage, which the global store also holds fixed.

The `controls-gate` roster runs `--globalMB=16 --transferKB=64 --iterations=2
--semaphore=16 --all-to-all=true --no-local=false --distribution=1`: 16 MB
in total — 256 slots per rank at one node, 128 at two, 64 at four, 32 at
eight — and `TRANSFERS_OK 768 512` at every one of those geometries, pinned
on the put count, finishing well inside a second on every runtime.
The measurement argument is the calibrated one at the end of this section.

**Calibrated arguments.** `--globalMB=4096 --transferKB=64 --iterations=5
--semaphore=16 --all-to-all=true --no-local=false --distribution=1` at every
node count. `iterations` and `transferKB` keep the published values;
`globalMB` is the size knob and stops at the origin's 4096 MB store ceiling
above, below its window by the law — the anchor measured 6.7–7.2 s on the
ARTS arms and 4.8 s on HPX, 15 GB resident on ARTS — which is recorded as the
row's disposition, not sized past.
