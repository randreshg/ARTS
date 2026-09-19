# CXL datablocks under EXCL: permission moves, payload stays

This document records the change that put `ARTS_DB_CXL` under the EXCL
coherence protocol, the resulting behaviour of each storage kind × access mode,
and the optimizations left on the table — chiefly the ones that let a flush be
skipped rather than merely made cheaper.

Date: 2026-09-19. Branch: `cxl-merge`.

---

## 1. What this replaced

`ARTS_DB_CXL` previously bypassed DB coherence entirely. Its GUID encoded the
CXL pointer directly, no route-table entry existed, and the acquire/release
paths short-circuited to a pair of cache operations:

```c
/* old acquire, in prep_dbs */        /* old release, in release_one_dep */
arts_cxl_consumer_flush(guid);        if (mode == DB_MODE_RW)
                                          arts_cxl_producer_flush(guid);
                                      return;  /* no route table */
```

Correctness therefore rested entirely on the CDAG: whatever ordering the
program's dependence edges imposed was the only ordering there was. Two EDTs
with an unordered write-write conflict on the same CXL block had no arbiter at
all, and nothing in the runtime could tell you so.

Note that this was true **only of the CXL path**. The rest of the runtime
already carried the full multi-protocol coherence module (`coherence/{val, inv,
excl, wrf_val}/`, selected by `ARTS_MEMORY_MODEL × ARTS_COHERENCE_PROTOCOL ×
ARTS_WRITE_POLICY × ARTS_RELEASE_POLICY`). EXCL did not need importing; CXL
needed connecting to it.

## 2. The protocol

A CXL block now runs the **EXCL × PURGE** state machine unchanged — the same
home `lock_state` word, the same `rw_waiters` / `ro_waiters` queues, the same
per-rank `cache_state` word, the same arbiters (`excl_compute_next`,
`cache_compute_next`). What differs is what a grant carries and what a release
ships:

| | ordinary block (`ARTS_DB`) | CXL block (`ARTS_DB_CXL`) |
|---|---|---|
| grant | one-sided PUT of the payload into the grantee's stable buffer | permission only; the grantee **invalidates** its cached lines |
| release | PUT of the dirty bytes into home's buffer, then notify | **flush** the modified lines to the window, then notify |
| bytes on the wire | `db_size` per grant, `db_size` per RW release | zero |

That substitution is what makes the visibility chain hold on a fabric with no
hardware cache coherence. The home never orders the *data* — it cannot, it does
not see it. It orders the *permission*, and the two cache edges convert that
ordering into visibility:

```
Writer:                          Home:                       Reader:
  acquire exclusive grant          receive release             receive grant
  modify CXL payload               transition lock_state       invalidate local lines
  FLUSH_FENCE_PRODUCER             grant waiting readers/      read CXL payload
  send release  ───────────────▶     writer     ───────────▶
```

No reader is granted until the writer's release has arrived; no release is sent
until the writer's flush has retired; no granted reader touches a line it has
not first invalidated.

### Where the state lives

**Nothing coherence-related is in CXL.** The window holds payload and only
payload, 64-byte aligned. A lock word in memory without hardware coherence is
not a lock word, so:

- every rank keeps a DRAM `struct arts_db_s` for the block;
- the **home rank** gets the full struct (`lock_state`, `rw_waiters`,
  `ro_waiters`, `cached_ranks`), every other rank the cache-only stub
  (`arts_db_cache_stub_size()`, which stops before the home fields);
- every authoritative transition is an ordinary local atomic on memory the
  hardware does keep coherent.

### Which rank is home

An ordinary DB GUID names its home in its rank field. A CXL GUID spends that
field on the `ARTS_CXL_RANK` (`0x3FFF`) marker and its whole 48-bit key on the
payload's offset in the window, so it names no home. The home is **derived**:

```c
/* gas/guid.h */
static inline unsigned int arts_cxl_home_rank(arts_guid_t guid) {
  uint64_t x = ARTS_GUID_GET_KEY(guid) >> 6;   /* granule index */
  /* splitmix64 finalizer */
  return (unsigned int)(mix(x) % arts_global_rank_count);
}
```

Every rank computes the same answer from the GUID alone — no communication, no
registry, no GUID format change, and the full 256 TB offset range preserved.
The payload is 64-byte aligned so the low 6 bits carry no entropy; hashing the
*granule* index keeps consecutive allocations off the same home. Every
coherence site that asks "who arbitrates this block" goes through
`arts_db_home_rank()`, which returns the rank field for an ordinary block and
the derived home for a CXL one.

### Lifecycle

- **Create** allocates the payload (only) in the window, mints the GUID from
  its offset, builds the DRAM descriptor, seeds the creator's RW hold, registers
  it on the thread's created-DB list (so the EDT epilogue releases it), and
  installs it in the route table. If the derived home is a different rank, it
  sends that rank the ordinary `MSG_DB_CREATE` tagged `ARTS_DB_CXL` — the home
  needs to know the block exists *and* that its creator is holding it RW, or a
  third rank's writer would be granted alongside the creator.
- **First touch on a third rank** installs a cache-only stub through
  `arts_db_cache_stub_install(guid, 0, ARTS_DB_CXL)`. The size is unknown there
  (no szhint in a CXL key) and is learned from the first grant, which states it,
  before that grant's invalidate needs a range.
- **Destroy** routes through the home like any coherent block and rides the
  existing PURGE teardown bit in `lock_state`: whoever reaches the zero edge
  tears down the directory. The payload bytes are not reclaimed — the CXL arena
  is a bump allocator and `GLOBAL_CXL_FREE` is the real library's business.

---

## 3. Behaviour by storage kind × access mode

`ARTS_DB_RW` and `ARTS_DB_RO` are not storage kinds — they are the access modes
`DB_MODE_RW` / `DB_MODE_RO`, and they apply to both coherent kinds. The matrix
is `{ARTS_DB, ARTS_DB_CXL} × {RO, RW}`. `ARTS_DB_CXL` still exists; it is now a
*coherent* kind rather than a pinned one.

```c
/* db.h */
static inline bool arts_db_type_is_coherent(arts_db_types_t t);  /* ARTS_DB | ARTS_DB_CXL */
static inline bool arts_db_type_has_buffer(arts_db_types_t t);   /* ARTS_DB only */
```

### 3.1 `ARTS_DB` × `DB_MODE_RW` — payload moves, twice

1. The acquirer CASes its `cache_state`. With no covering grant it parks a
   waiter and sends `MSG_DB_EXCL_REQUEST(RW)` to the home, advertising a
   rendezvous landing: its own registered stable buffer, with a fresh txid.
2. The home pushes onto `rw_waiters` and CASes `lock_state`. If the turn is free
   it pops the waiter and sends `MSG_DB_EXCL_GRANT`, **one-sided PUTting the
   home buffer's bytes into the requester's landing**, and advertising *home's*
   buffer as the publish landing for the eventual release.
3. The requester pairs {packet, write-completion}, CASes `GRANT_RW`, and drains
   its parked waiters. The bytes now live locally, at a fixed address that never
   moves for the block's lifetime.
4. On release the last holder **PUTs its dirty bytes into the stashed home
   landing**, then sends a control-only `MSG_DB_EXCL_RELEASE`. The home pairs
   packet with write-completion before it transitions and grants the next turn.

**Per RW turn: 2 control messages + 2 full payload transfers.**

### 3.2 `ARTS_DB` × `DB_MODE_RO` — payload moves, once per reader

Same request path with `mode=RO`; the home queues on `ro_waiters`. When the RO
phase opens, `LOCK_GRANT_ALL_RO` drains the entire reader queue and PUTs the
home buffer's bytes to **every** waiting rank. Readers run concurrently, counted
by `r` in `lock_state`. RO release is a data-less notify (`r--`); at `r == 0`
the home may flip to a queued writer.

**Per RO phase: N control message pairs + N full payload transfers**, for N
distinct reader ranks.

### 3.3 `ARTS_DB_CXL` × `DB_MODE_RW` / `DB_MODE_RO` — permission moves, payload never does

Identical arbiter, identical exclusion guarantees, **zero payload on the wire**.

- **Request** carries no rendezvous landing (nothing to PUT into), and the home
  skips the size-CTS round — a landing-less CXL request is the normal case, not
  a first touch.
- **Grant** is a data-less packet carrying `{mode, db_size}`. On arrival the
  grantee runs `FLUSH_FENCE_CONSUMER` over the 64-byte-aligned payload range
  **before** the CAS that releases local EDTs, then drains its parked waiters.
  `dep->ptr` is `arts_cxl_get_ptr(guid)`.
  The invalidate runs for **RW grants too**, not only RO: a writer holding stale
  clean lines would otherwise read its own pre-turn bytes through them.
- **RW release** runs `FLUSH_FENCE_PRODUCER` over the payload **first**, then
  sends a control-only release. Never merged into one step, never reordered — if
  the message overtook the dirty lines, the next reader would invalidate, read
  the window, and see the state before this writer's turn. The home-local case
  takes the same edge: two ranks on one node do share hardware coherence and the
  flush is redundant between them, but the elision is wrong the moment the job
  spans nodes (see §6.5).
- **RO release** is a control-only notify with no flush. A reader dirtied
  nothing; the *next* grantee's invalidate is what protects it.
- **Mutual exclusion** is unchanged: one RW holder at a time, all-RO fan-out
  concurrent, and an RW turn waits for `r == 0`.

**Per turn: 2 control packets, 0 bytes of payload.** For a 1 MB tile that is the
entire point of the design.

### 3.4 Summary table

| kind × mode | arbitration | payload transfer | cache edges |
|---|---|---|---|
| `ARTS_DB` × RW | home `lock_state`, one holder | home→holder, holder→home | none |
| `ARTS_DB` × RO | home `lock_state`, all readers | home→each reader | none |
| `ARTS_DB_CXL` × RW | identical | **none** | invalidate at grant, flush at release |
| `ARTS_DB_CXL` × RO | identical | **none** | invalidate at grant |
| `ARTS_DB_PIN` / `ARTS_DB_GPU*` | none (pinned to creator) | none | none |

---

## 4. Using it

### 4.1 Configuring the build

Both kinds come from one binary; the coherence protocol is a compile-time
choice and every rank in a run must use the same build.

```sh
# DRAM datablocks under EXCL, no CXL at all
cmake .. -GNinja -DARTS_COHERENCE_PROTOCOL=EXCL -DARTS_RELEASE_POLICY=PURGE

# DRAM *and* CXL datablocks under EXCL
cmake .. -GNinja \
  -DARTS_USE_CXL=On \
  -DARTS_COHERENCE_PROTOCOL=EXCL -DARTS_RELEASE_POLICY=PURGE \
  -DARTS_CXL_RAPID_INCLUDE_DIR=/path/to/fake_arts_cxl_lib/inc \
  -DARTS_CXL_LIB_DIR=/path/to/fake_arts_cxl_lib \
  -DARTS_OFI_CXI=Off
ninja
```

`ARTS_USE_CXL=On` with any other protocol is a configure error naming the
reason. `ARTS_WRITE_POLICY` is not a live axis under EXCL (it is forced to
`WB`); `ARTS_RELEASE_POLICY=RETAIN` builds for `ARTS_DB` but is rejected
alongside `ARTS_USE_CXL`.

A CXL build still supports `ARTS_DB`. The two kinds coexist in one program and
one run: use whichever fits each block.

Optional, to retarget a whole benchmark suite without touching sources:

```sh
-DARTS_DEFAULT_DB_KIND=ARTS_DB_CXL     # makes the ARTS_DB_DEFAULT macro expand to CXL
```

Then write `ARTS_DB_DEFAULT` at every create site and flip the kind from CMake.

At run time the fake library takes `ARTS_FAKE_CXL_REGION_SIZE` (default 32 GiB)
for the emulated window; the ARTS-side arena size is a compile-time knob
(`cxl/deque.h`).

### 4.2 Non-CXL datablocks (`ARTS_DB`) under EXCL

Nothing about the API changed. The usual OCR shape:

```c
void producer(uint32_t paramc, const uint64_t *paramv,
              uint32_t depc, arts_edt_dep_t depv[]) {
    double *a = (double *)depv[0].ptr;      /* RW grant held for the body */
    for (unsigned i = 0; i < N; i++) a[i] = 1.0;
}                                            /* epilogue releases the grant */

void consumer(uint32_t paramc, const uint64_t *paramv,
              uint32_t depc, arts_edt_dep_t depv[]) {
    const double *a = (const double *)depv[0].ptr;   /* RO grant */
    use(a);
}

void main_edt(uint32_t paramc, const uint64_t *paramv,
              uint32_t depc, arts_edt_dep_t depv[]) {
    double *init = NULL;
    arts_guid_t db = arts_db_create((void **)&init, N * sizeof(double),
                                    ARTS_DB, ARTS_DB_PROP_NONE,
                                    &(arts_db_hint_t){.rank = 0});
    /* The creator holds RW: `init` is writable right now. */
    for (unsigned i = 0; i < N; i++) init[i] = 0.0;
    arts_db_release(db, DB_MODE_RW);         /* or let the epilogue do it */

    arts_guid_t pe = arts_event_create(&ARTS_EVENT_HINT_FINISH);
    arts_guid_t p = arts_edt_create(producer, 0, NULL, 1,
                                    &(arts_edt_hint_t){.rank = 1,
                                                       .finish_event = pe});
    arts_add_dependence(db, p, 0, DB_MODE_RW);

    arts_guid_t c = arts_edt_create(consumer, 0, NULL, 2,
                                    &(arts_edt_hint_t){.rank = 2});
    arts_add_dependence(db, c, 0, DB_MODE_RO);
    arts_add_dependence(pe, c, 1, DB_MODE_NULL);   /* order c after p */
}
```

Points that matter under EXCL specifically:

- **The creator holds RW from `arts_db_create`.** The returned pointer is
  writable immediately, and the hold blocks every other writer until it is
  released — explicitly with `arts_db_release(db, DB_MODE_RW)`, or implicitly
  by the EDT epilogue. If the creating EDT then blocks (`arts_event_wait`),
  release first or the epilogue cannot run and writers queue behind you.
- **`ARTS_DB_PROP_NO_ACQUIRE`** skips that hold; `arts_db_create` returns
  `NULL` for the pointer and the first real acquirer is granted immediately.
- **`hint->rank` picks the home** — the arbitrating rank, and under PURGE also
  the rank that holds the canonical bytes between turns. Put it where the
  traffic is.
- **RW and RO deps are both blocking and both GUID-serialized**
  (`arts_db_acquire_is_serialized` returns true for each), so an EDT naming
  several blocks acquires them in GUID order and cannot deadlock against
  another EDT doing the same.
- **Ordering between writers is the program's job.** The runtime serializes
  same-block RW, but registration order does not imply grant order. Chain
  stages on finish events when a stage must observe the previous one.

### 4.3 CXL datablocks (`ARTS_DB_CXL`) under EXCL

Same API, same modes, same ordering rules. Change the kind argument:

```c
    double *tile = NULL;
    arts_guid_t db = arts_db_create((void **)&tile, N * sizeof(double),
                                    ARTS_DB_CXL, ARTS_DB_PROP_NONE, NULL);
    for (unsigned i = 0; i < N; i++) tile[i] = 0.0;   /* creator holds RW */
    arts_db_release(db, DB_MODE_RW);                  /* flushes, then notifies home */
```

EDT bodies are byte-for-byte identical to the `ARTS_DB` case — `depv[i].ptr`
points into the shared window instead of into a local buffer, and that is the
only difference visible to a kernel.

**Do not call the flush helpers yourself.** `arts_cxl_producer_flush` /
`arts_cxl_consumer_flush` are now protocol-internal and are driven at the
release and grant edges. Application-level flushing is at best redundant and at
worst hides a missing dependence edge.

Rules specific to the CXL kind:

- **Creation is always local.** `hint->rank` is ignored (with a warning): the
  block is allocated where it is created, and the arbitrating home is derived
  from the payload offset. Pass `NULL` for the hint.
- **No pre-reserved / labeled GUIDs.** `hint->guid` is ignored (with a
  warning) — a CXL GUID is minted *from* the payload address, which does not
  exist until the allocator has run. Distribute CXL GUIDs the ordinary way:
  create them on one rank and hand them out through `arts_add_dependence` /
  `arts_signal_edt`, or through a labeled `ARTS_DB`/event that carries them.
- **Every rank addresses the same bytes.** There is no "copy on rank N"; the
  grant is the right to touch the single copy.
- **Size is remembered per rank from the first grant.** A rank that has never
  seen the block learns `db_size` from the grant that admits it, so a CXL GUID
  is useless to a rank that has not been granted the block — which is exactly
  the rule you already follow by declaring a dependence.
- **Destroy tears down the directory, not the bytes.** `arts_db_destroy` routes
  to the home and runs the arbitrated teardown; the window allocation is not
  reclaimed (see §6.8).
- **Alignment.** The runtime rounds each payload up to a 64-byte granule, so a
  flush of one block can never reach another's lines. Structures the
  application places *inside* a CXL block should still be laid out so that
  independently-written fields do not share a cache line — the protocol
  arbitrates whole blocks, and two EDTs with false sharing inside one block are
  serialized, not made correct.

### 4.4 Choosing between them

| use `ARTS_DB` when | use `ARTS_DB_CXL` when |
|---|---|
| the payload is small relative to the control traffic | the payload is large and the transfers dominate |
| access is bursty from one rank at a time, so a local copy is reused many times without the home | many ranks touch the block and copies would be shipped repeatedly |
| the run may span nodes with no shared window | every rank in the job maps the same CXL window |
| you want the well-trodden path | you want to measure permission-only coherence |

The two are mixable at block granularity; a natural pattern is CXL for the
large shared arrays and `ARTS_DB` for small control/metadata blocks.

### 4.5 Running and checking

```sh
ARTS_CONFIG=configs/local/test/1n.cfg ./tests/coherence_cxl_grant_chain
ctest -R coherence_cxl_grant_chain --output-on-failure
```

Multi-rank configs live in `configs/local/test/{2n,3n,4n,2n_io}.cfg`, but see
the status note in §5 about the emulator blocking multi-rank CXL today.

`arts_flush_trace.bin` is written on exit by the fake library; it is the
measurement vehicle for everything in §6.

---

## 5. Files changed

| file | change |
|---|---|
| `libs/include/internal/arts/gas/guid.h` | `arts_cxl_home_rank()` (splitmix64 over the granule index) and `arts_db_home_rank()`, the single site every coherence path asks "who arbitrates this". |
| `libs/include/internal/arts/db.h` | `ARTS_CXL_COHERENT` derived macro; `arts_db_type_is_coherent()` / `arts_db_type_has_buffer()`; documented the two flush entry points as the producer/consumer visibility edges. |
| `libs/src/core/db.c` | CXL create rewritten to payload-only + DRAM descriptor + `MSG_DB_CREATE` to the derived home; CXL acquire/release/destroy short-circuits removed so they run the protocol; `arts_db_user_ptr` and the flush helpers derive the payload from the GUID; `prep_dbs` excludes both coherent kinds. |
| `libs/src/core/coherence/excl/purge.c` | permission-only grant fan-out; no landing on a CXL request; no size-CTS round; the invalidate in `lock_grant_commit` and the flush in `lock_send_release_rw`; home derivation via `arts_db_home_rank`. |
| `libs/src/core/coherence/coherence.c` | CXL arm in `mark_edt_ready_by_guid` and `arts_db_acquire_local` (no buffer ref, pointer from the GUID); `arts_db_cache_stub_install` takes the storage kind; destroy routes to the derived home. |
| `libs/src/core/coherence/handlers.c` | CXL branch in `arts_handler_db_create` — a payload-free home directory seeded with the creator's RW hold. |
| `libs/src/core/edt.c` | `edt_apply_satisfy` no longer pre-fills `dep->ptr` for a CXL GUID; a satisfy names the block, the acquire grants it. |
| `CMakeLists.txt` | `ARTS_USE_CXL` now requires `EXCL × PURGE` (configure error otherwise, naming the reason); CXL include path made directory-wide to match the directory-wide `ARTS_USE_CXL` define. |
| `tests/CMakeLists.txt` | `link_libraries(arts_cxl_lib)` for the whitebox tests; registration for the new test. |
| `tests/ocr/coherence_cxl_grant_chain.c` | new: remote write → remote read, a concurrent reader cohort, and a writer that must wait the cohort out. |

### Build

```
cmake .. -GNinja -DARTS_USE_CXL=On \
  -DARTS_COHERENCE_PROTOCOL=EXCL -DARTS_RELEASE_POLICY=PURGE \
  -DARTS_CXL_RAPID_INCLUDE_DIR=/path/to/fake_arts_cxl_lib/inc \
  -DARTS_CXL_LIB_DIR=/path/to/fake_arts_cxl_lib \
  -DARTS_OFI_CXI=Off
ninja
```

The two protocol flags are new and mandatory. Without them the build silently
selected VAL, where a CXL block has no protocol that can serve it.

### Status

- Full build (library, all 8 protocol variants, benchmarks, all tests): clean.
- Single-node suite: 306/308. The two failures are `counter_smoke` /
  `counter_smoke_value`, which want `counters/n0.json` in a build configured
  with `counters_off.cfg`. Unrelated to coherence.
- `coherence_cxl_grant_chain` passes single-rank: all three stages, through the
  real arbiter and both cache edges (the home is simply local).
- **Multi-rank is unvalidated.** Ranks 1+ die in `arts_runtime_init` with *"CXL
  DB arena is missing or uninitialized"*, before any datablock exists. The cause
  is in the emulation: a non-zero rank joins the segment via
  `arts_cxl_deque_get()` → `LAST_SHARED_MALLOC`, and `fake_arts_cxl_lib`
  implements that as a plain bump allocation, so it returns a fresh zeroed block
  instead of rank 0's deque. The 2n/3n/4n variants of the new test are
  registered `DISABLED TRUE` with that reason recorded; enable them the moment
  the CXL library in use can hand a second process the first process's
  allocation.
- The benchmark variant libraries (`arts_static_ocr_val_*`, `..._inv_*`) compile
  these same sources with `ARTS_USE_CXL` defined. There `ARTS_CXL_COHERENT` is
  undefined and `ARTS_DB_CXL` falls back to the pinned, creator-local path
  rather than entering a coherence arm that cannot grant it. That is deliberate;
  do not "fix" it by widening the gate.

---

## 6. Future optimizations

Ordered roughly by (expected win ÷ effort). The flush-elision opportunities are
§6.1–§6.7; the rest is context.

**Measure first.** `fake_arts_cxl_lib` already writes `arts_flush_trace.bin` —
one record per flush call (`flush_entry_t`: timestamp, thread id, address, size,
and producer/consumer direction), ring-buffered and dumped at exit. Every
proposal below can be evaluated against that trace *before* it is implemented:
count how many flushed bytes each policy would have removed from a real
benchmark run. Do that before writing any of this code.

### 6.1 Write-epoch skip on the reader side — sound, cheap, biggest easy win

The home already serializes every write: a block is modified only by a rank
holding an RW grant, and every such turn ends with a release the home observes.
So the home can keep a monotonic **write epoch** in DRAM, incremented on each RW
release, and ship it in the grant. Each rank records the epoch of its last
grant; when a new grant carries the same epoch, *no writer ran anywhere since
this rank last read*, its lines are still valid, and the `FLUSH_FENCE_CONSUMER`
can be skipped entirely.

- Cost: one `uint64_t` per DB at the home, one per DB per rank, 8 bytes in the
  grant packet.
- Win: a read-mostly block — an input tile, a lookup table, a converged field —
  is invalidated **once**, on its first grant after the last write, instead of
  once per grant forever. For a phase-structured application that reads the same
  tiles every iteration this removes nearly all reader-side flush traffic.
- Soundness: relies on "no writer ran" ⟹ "no line changed", which holds only if
  nothing writes the payload outside the DB API. An application that keeps a raw
  pointer into the window and writes through it after release breaks this; so
  does DMA into the region. Both already break the protocol, but this makes the
  failure silent instead of merely racy, so it should be behind a build flag
  with the assumption documented.
- Implement as an **ablation**, not a replacement: keep the unconditional path
  compiled so the two can be measured against each other.

### 6.2 Lazy (recall-driven) flush — biggest structural win, highest cost

Today the writer flushes at *its* release, whether or not anyone wants the block
next. Under high reuse — a rank that writes the same block in consecutive EDTs,
or a producer that owns a tile for a whole phase — that is N flushes where one
would do.

The alternative is to make the flush an obligation the home calls in: the holder
releases *permission* but keeps its lines dirty, and when the home needs to
grant someone else it sends a `RECALL`; the holder flushes and acks; the home
then grants. N flushes per ownership episode collapse to 1.

This is the CXL-shaped analogue of what `ARTS_RELEASE_POLICY=RETAIN` does for
permission, which is why the natural place to build it is a **CXL × RETAIN** arm
rather than a modification to PURGE. (The current CMake guard forbids CXL ×
RETAIN precisely because RETAIN's *payload migration* is meaningless here — the
recall-flush reinterpretation would be a new arm, not a relaxation of the
guard.)

- Trade: adds a round trip to every hand-over, and makes the home's grant
  latency depend on the ex-holder's responsiveness. Excellent for high reuse,
  actively worse for ping-pong.
- Composes with §6.1: the epoch still names when a reader may skip.

### 6.3 Partial-range flush — order-of-magnitude for stencils, needs an API

Both edges currently cover the whole payload. Most writers touch a sub-range: a
tile row, a halo, a column strip. If the writer declares what it will dirty —

```c
arts_db_dirty_range(dep, offset, length);        /* or a strided descriptor */
```

— then the producer flush covers only those lines, the release reports the range
to the home, the home records it, and the next grant carries it so the consumer
invalidates only what actually changed.

For LULESH/Cholesky/stencil halo exchange, where the written fraction of a block
is a few percent, this is the largest single reduction available. It needs
either a user-facing API (cheap, but the program must be honest) or a
compiler/DSL that already knows the access pattern. An under-declared range is
silent corruption, so it wants a debug mode that flushes everything and compares.

### 6.4 Read-only-after-init blocks — trivially sound special case

A block written once at create and thereafter only read (inputs, constant
tables, a converged field) can say so at create time via a property flag. After
the creator's first release flush, every subsequent grant skips the invalidate,
and the home can serve RO grants without tracking anything. This is §6.1's
benefit with no per-rank epoch state and no grant-packet field, at the price of
the program having to assert the property. Worth doing first as the
low-risk proof of the idea.

### 6.5 Same-node peers share hardware coherence

Two ranks on one node are MESI-coherent through the CPU, so the CXL flush
between them is pure overhead. The home knows the node of the previous writer
and of the next grantee, so it can tag the grant "the previous holder was on
your node — no invalidate needed", and tell the writer "the only waiter is on
your node — no flush needed".

Soundness requires the home to track whether *any* off-node rank has held the
block since the last global flush — one bit plus a node id per block. Get that
wrong and cross-node readers see stale data.

This was deliberately **not** implemented now: the test environment is
single-node, so the elision would remove essentially every flush and hide
exactly the bugs the flushes exist to prevent. On real hardware with several
ranks per node it is a large win and should be revisited — but only once
multi-rank runs are exercisable on a configuration where the elision can be
turned off and compared.

### 6.6 Non-temporal stores — removes the producer sweep entirely

If the writer stores with `movnt`, the lines never enter the cache dirty and the
producer edge collapses from a `clflushopt` sweep to a single `sfence`. This
requires the payload writes to go through an ARTS-provided store or memcpy
primitive, which is realistic for bulk producers (STREAM, tile writes,
initialization) and unrealistic for pointer-chasing or read-modify-write kernels.
Offer it as an opt-in write helper rather than a global policy, and note it
interacts with §6.3 — a non-temporal writer has no dirty range to declare.

### 6.7 Skipping the flush when an RW holder never wrote

An RW *dep* does not imply a write; a kernel may take RW and take a branch that
writes nothing. There is no cheap userspace way to know. Three options, in
increasing order of cost and decreasing order of appeal:

1. **Program assertion** — `arts_db_mark_clean(dep)` before release, or a
   `DB_MODE_RW_MAYBE` the runtime treats as clean unless told otherwise. Cheap
   and honest, but a wrong assertion is silent corruption.
2. **`mprotect` dirty tracking** — map the payload read-only on grant, catch the
   first write fault, mark dirty. Exact, but costs a signal handler and a TLB
   shootdown per turn; likely only pays for large, rarely-written blocks.
3. Nothing — accept the flush. This is the current behaviour and is the right
   default until the trace says otherwise.

### 6.8 Non-elision items worth noting

- **Invalidate placement.** The consumer flush currently runs on the progress
  thread at grant time, which overlaps it with the EDT's remaining dependence
  waits. Moving it to the worker immediately before the EDT body would overlap
  it with scheduling instead. Which is better is an empirical question; both are
  correct, and the trace plus a scheduling counter will answer it.
- **Home placement.** `hash(offset) % nranks` spreads directory load but ignores
  locality: the home is very unlikely to be a rank that touches the block. An
  alternative is to encode the creator as home in the GUID (shrinking the offset
  field; 64-byte alignment frees 6 low bits, so a 14-bit rank leaves a ~1 TB
  window). That removes one network hop whenever the creator is also the main
  writer, and concentrates load when it is not. Worth measuring once multi-rank
  runs work.
- **Grant coalescing for RO storms.** The EXCL cache has no combining window
  (unlike the VAL family), so K same-rank readers arriving while a request is in
  flight already park behind it — that part is fine. What is not batched is the
  home's RO fan-out across ranks, which sends one packet per rank. At high rank
  counts a broadcast/tree fan-out of a data-less grant would cut the home's
  serial send cost.
- **Reclaiming CXL payloads.** Destroy tears down the directory but never frees
  the window bytes, because the arena is a bump allocator. A real deployment
  needs `GLOBAL_CXL_FREE` wired into the teardown, plus a decision about whether
  a freed offset may be reused (a recycled offset produces a recycled GUID,
  which the GUID lifetime rules do not support).
- **Making multi-rank testable.** Everything above is unmeasurable until
  `fake_arts_cxl_lib` can hand a second process the first process's allocation.
  A last-allocation offset recorded in the region header, returned by
  `fake_cxl_last_shared_malloc` when the requested size matches, would be
  enough.
