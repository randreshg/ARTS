.. _coherence_protocols:

Coherence Protocols
===================

.. contents:: On this page
   :local:
   :depth: 2

This page is **normative**. It defines the memory-model contracts an ARTS
build can provide, the coherence protocols that implement them, and the
surrounding design space — both implemented and roadmap.

Model, family, and the second axes
----------------------------------

A *memory (consistency) model* is a contract: it defines which values a read
may legally return, and therefore which programs are valid. A *coherence
protocol* is an implementation mechanism — replica management, admission
control, ownership transfer, write-back — that satisfies some contract as a
consequence of its design. **Each coherence protocol requires a memory
model of the program**, so the protocol is the build axis and the
**contract** the build implements follows from it: ``OCR`` (races legal,
the runtime orders every conflict it must — required by
``VAL``/``INV``/``EXCL``) or ``DB-WRF`` (exclusive write acquisition: the
program guarantees that at most one write-mode acquisition of a DataBlock
is live at any time, system-wide — required by ``FLUSH``; see
:ref:`db_wrf_contract`). Beside the protocol, two second axes say what a
release moves and when a grant goes back:

* ``ARTS_COHERENCE_PROTOCOL`` selects the coherence **protocol**. Under the
  ``OCR`` model, who keeps reader copies valid: ``INV`` (default;
  *invalidation* — reader copies stay valid until a writer's release
  retires every standing copy in an acknowledged invalidation round),
  ``VAL`` (*validation* — each read acquire checks its cached copy's version
  at the serving side and refetches only when stale; readers are never
  blocked, tracked, or invalidated), or ``EXCL`` (*exclusion* — queue-fair
  distributed reader–writer lock per DB; nodes take turns, so no stale copy
  ever exists). Under the ``DB-WRF`` model, the one protocol
  is ``FLUSH`` (fetch the whole payload at every remote acquire, write it
  back at every remote RW release; see :ref:`flush_protocol`).
* ``ARTS_WRITE_POLICY`` selects the **write policy** of a node's DB cache at
  release granularity — live under INV/VAL: ``WT`` (write-through at
  release; the payload is flushed to the DB's home, which then serves reads)
  or ``WB`` (default; write-back — the payload stays with the last writer
  and moves only on demand, through directory forwarding). FLUSH has no
  write-policy axis: it fetches and writes back the whole payload every
  time, unconditionally.
* ``ARTS_RELEASE_POLICY`` selects the **release policy** — what a node does
  with its grant when the last local user finishes — live under EXCL and
  under ``WT`` in VAL/INV: ``PURGE`` (hand copy and permission back to the
  home) or ``RETAIN`` (default; keep both until another node asks, and let
  the home recall them). FLUSH has no release-policy axis either — there is
  no grant to retain, only a per-release round trip.

The OCR-model axes are nominally independent: the family × write-policy ×
release-policy space has 12 combinations, of which **eight** are built (the
dead ones excluded by the reasons below). DB-WRF has exactly one protocol —
``FLUSH`` needs neither second axis — for **nine build configurations** in
total, one build directory each:
OCR×VAL×WT×{PURGE,RETAIN}, OCR×VAL×WB×RETAIN, OCR×INV×WT×{PURGE,RETAIN},
OCR×INV×WB×RETAIN, OCR×EXCL×WB×{PURGE,RETAIN}, DB-WRF×FLUSH.
Everything else is a configure-time ``FATAL_ERROR`` naming the reason:
EXCL×WT (under exclusion no copy outlives a write turn, so the payload
rides the permission and the release policy already decides both),
and INV/VAL×WB×PURGE (a voluntary return hands back only the permission
while the bytes stay at the ex-owner, so the home still cannot serve a
reader on its own — and shipping the bytes home too is, by definition,
write-through; at ``WT`` the same return costs no data motion, which is why
PURGE is built there instead — see :ref:`orthogonal_dimensions`). DB-WRF
pairs with no protocol but FLUSH: VAL, INV and EXCL each already order
writers across nodes at runtime — a migrating grant for VAL/INV, admission
for EXCL — so under them the program-side write-ordering obligation would be
redundant.

Every rank in a multinode run must use the same build.

.. _ocr_contract:

The OCR contract (reference)
-----------------------------

The contract delivered by the default build (OCR × INV) is the memory model
of the *Open Community Runtime Interface*, version 1.2.0, §1.6, with the
``synchronized-with`` relation completed as in Dokulil, "Consistency model for
runtime objects in the Open Community Runtime", J. Supercomputing 75:2725–2760,
2018 (doi:10.1007/s11227-018-2681-2), which this document incorporates by
reference. The version pin matters: the OCR specification itself reserves the
right to relax its race rules in future versions.

Its skeleton:

* *sequenced-before* orders operations within one EDT (host C semantics).
* *synchronized-with* is established **only through events** (including each
  EDT's output event). Three release rules apply: (1) all loads/stores by an
  EDT to a DB complete before that DB's release completes; (2) an EDT releases
  **all** of its DBs before its post-slot is satisfied; (3) before an EDT
  explicitly satisfies an event, any DB potentially exposed by that
  satisfaction must be written back and released first.
* *happens-before* is the transitive closure of the two, and defines when a
  write must be visible to a read of the same DB.

Data races are **legal** under this contract. Racing writes have defined
semantics; a racing read does not:

* *Non-overlapping rule:* if two unordered EDTs write disjoint 8-byte-aligned
  ranges of one DB, both writes must appear in memory (no write may be masked
  by buffering or whole-block write-back).
* *Overlapping rule:* unordered overlapping writes resolve to one legal
  interleaving at the platform's N-byte store-atomicity granularity (N = 8 on
  AMD64).
* *Racing read:* a read of a word that a write unordered with it may change
  returns an unspecified value, not necessarily one any EDT wrote, because the
  runtime copies payloads plainly.

Access modes are replication directives
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

ARTS deliberately provides only the minimal mode algebra:

* ``DB_MODE_RO`` — "this EDT will not write". The runtime may replicate
  freely. **No isolation is promised**: as in OCR's read-only mode, writes by
  concurrent unordered writers may become visible to the reader. (A remote RO
  acquire happens to receive a copy — a snapshot — but that is a distribution
  artifact, not a contract.)
* ``DB_MODE_RW`` — "this EDT may write". The runtime manages ownership.
* ``DB_MODE_NULL`` — opaque value or control dependence, with no DB access.
  The payload may carry a scalar or an uninterpreted DB GUID. Scalar delivery
  through this mode extends OCR's control-dependence contract.

A mode describes a *sharing pattern* the runtime needs for replica management;
it never carries synchronization semantics. Synchronization is the exclusive
business of events (plus intra-node hardware atomics). For this reason ARTS
**permanently omits** OCR's two service modes:

* OCR ``Exclusive write`` (a critical-section service embedded in a mode) —
  mutual exclusion is the program's job, exactly as on hardware shared memory.
  EW is a runtime scheduling service, not part of the memory model proper: the
  normative rules of OCR §1.6 never mention it.
* OCR ``Constant`` (an isolation promise embedded in a mode).

The OCR-compatibility shim translates ``CONST→RO`` and ``EW→RW``. Both
translations are **exact for data-race-free programs** and lossy only for racy
programs that rely on CONST isolation or EW interval exclusion; such programs
are outside the supported subset (this is a property of the translation, not a
non-conformance of ARTS).

Implementations may be stronger; programs must not rely on it
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Every conforming implementation (all three ARTS families, and likewise XSOCR and
OCR-Vx) is incidentally *stronger* than the contract in places — e.g. the VAL
protocol serializes unordered inter-node RW sessions via single-owner grants,
and remote RO acquires observe an installed-version copy. These
strengthenings are **never** part of the contract. A program that depends on
them is *non-portable* (it may break on another conforming implementation or
protocol), not invalid. Keeping the contract maximally relaxed preserves
implementation freedom — including future multi-writer merge protocols.

.. _db_wrf_contract:

The DB-WRF contract
-------------------

The DB-WRF model, which ``FLUSH`` requires, is a deliberately **weaker**
contract than OCR — *exclusive write acquisition*:

  A write acquisition of a DataBlock is any acquisition in a write mode: an
  EDT dependence in RW or EW mode, or the hold a creating EDT takes when it
  creates the block without ``ARTS_DB_PROP_NO_ACQUIRE``. A program is
  DB-WRF-valid iff, for every DataBlock, every pair of its write
  acquisitions is ordered by happens-before — one is released (explicitly,
  or by its EDT's completion) before the other begins. Read acquisitions
  (RO, CONST) are unconstrained: they may overlap writes and each other.
  Granularity is the whole DataBlock; writers to disjoint byte ranges of one
  block are still two write acquisitions. The property is
  placement-independent: where the EDTs run, and where the block is homed,
  do not enter the definition.

The word "race" is not used for this model: write-race-freedom at byte
granularity already holds for every OCR-legal program, so it would name
nothing new. A write acquisition that violates the contract is a
*write-write conflict*; two overlapping write acquisitions on a FLUSH build
race their whole-DB write-backs and the last one wins, so the outcome is
undefined, not merely stale.

The lattice of program classes is ``DB-WRF ⊂ OCR-legal``: every DB-WRF-valid
program is OCR-legal, but OCR-legal programs that rely on the runtime to
order overlapping writers — whole-DB fan-out, disjoint-region sibling
writers, a hand-off without a preceding release — are **outside DB-WRF**
and can silently produce wrong answers under a build that requires it. A
creator hold is a write acquisition: passing a freshly created block to a
child in RW without releasing it first is a DB-WRF violation regardless of
whether the creator happens to be the block's home (the OCR spec §1.6.2(c)
already demands the release before the satisfy).

Each coherence protocol requires a memory model: ``VAL``/``INV``/``EXCL``
require only ``OCR`` (the runtime orders every conflict). ``FLUSH`` (see
:ref:`flush_protocol`) requires ``DB-WRF``. ``EXCL`` and ``INV`` under
DB-WRF would be redundant — both already order writers across nodes at
runtime, which is exactly what DB-WRF asks of the program instead — which is
why DB-WRF has exactly one protocol and CMake refuses the rest.

.. warning::

   DB-WRF exists to measure the cost of coherence obligations in benchmarks.
   Never use DB-WRF as a correctness baseline. The experiment driver's
   catalog marks applications whose wiring relies on guarantees outside
   this contract with ``unordered_writes`` (a reason plus file:line — a
   contract-ineligibility declaration, not a bug mask); such a row is
   ``N/A`` on the FLUSH entry and runs unaffected on every OCR-model entry.

A CXL store is reached a different way: on ``EXCL`` × ``PURGE`` the
store a payload rests in between write turns is selectable — the
home's DRAM (default) or CXL memory (``ARTS_USE_CXL``) — with the same
state machine, the same PURGE rule and the same re-fetch every turn;
the two residencies (``ARTS_CXL_RESIDENCY``) differ only in whether an
ownership edge stages a rank-local copy (``STAGED``) or flushes in
place against the store (``DIRECT``). The store is one set of arenas
the first rank creates from the device library; every other rank
attaches to them through the same library's startup handshake.
``ARTS_CXL_REAL`` selects which library answers that API — the
vendored fake library (one host's shared memory, so one run per host)
or the real device library, named by path — and nothing else.

.. _cxl_edge_flushes:

CXL store: the two flushes
---------------------------

A CXL store is not coherent across hosts: a write reaches the store only
when its writer flushes it, and a reader sees it only after reloading its
own copy. The store changes only how a turn's bytes move, at the two
ownership edges: when the right arrives at a node, a *consumer* flush of
the block's slot, then the copy into the working copy (or, where a turn
works in the slot itself, the first access); when the node hands the
right back after a write turn, the copy back, then a *producer* flush of the
slot. Each transfer is a plain flush and copy, done synchronously where
the transfer happens — by whichever thread applies the grant (a progress
thread for a grant from another rank; for a home-local grant, the worker or
progress thread whose request, release or install produced it), before the
grant admits anyone, and by the releasing worker at the zero edge, before
the right leaves. Nothing is deferred to another thread and nothing is
acknowledged beyond the protocol's own release: the acquiring task runs only
after its grant was applied, and the release the home waits on follows that
task. Nothing between the edges flushes, and a read turn's edge flushes
nothing. A recording test (``cxl_edge_flush_order``) pins exactly these two
sites on both residencies; the values are checked by the experiment
driver's cross-entry consensus and on the device itself.

.. _protocols:

The protocols
-------------

VAL — versioned snapshots, externally ordered updates
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

*Status:* implemented (``libs/src/core/coherence/val/`` — ``directory.c`` +
``wt.c`` + ``wb.c``, plus the shared grant plane ``coherence/grant.c``);
selected by ``-DARTS_COHERENCE_PROTOCOL=VAL``, write policy by
``ARTS_WRITE_POLICY={WT,WB}`` (default ``WB``).

The name is used in McKenney's sense: **readers are never blocked and never
invalidated**. A reader acquires a versioned snapshot and proceeds on it;
updates install new versions without disturbing readers in flight. The
update side's ordering comes from *outside* the read path: write access is
granted to at most one node at a time via a single-owner grant. Within the
owning node, multiple RW acquirers share a local buffer, so intra-node
writes are multi-writer (hardware cache coherence keeps them consistent). A
remote RW acquire requests ownership from the current owner; the owner
completes its release, ships the payload and transfers the grant. This
serializes inter-node writes — the OCR non-overlapping rule holds because
intra-node concurrent writes are hardware-coherent and inter-node ones never
overlap in time.

The correspondence to canonical RCU is the read-side regime: updaters
serialized outside the read path, readers proceeding on possibly-stale
versions, old versions reclaimed only after their readers drain (refcounted
snapshot buffers standing in for grace periods). Two mechanics deliberately
diverge: updates are applied **in place** rather than copy-on-update —
versions materialize at *serve* time, as reader-bound copies (copy-on-read) —
and reads are consequently **not version-atomic**: a snapshot copied
concurrently with an in-place write may carry mixed content, down to a word
read partly before and partly after the write; only a reader unordered with
that write receives such a snapshot, and the value of its racing read is
unspecified.

Reader-side invalidation — in any form — is a **non-goal** of this protocol
family: read-mostly cases where a lock or invalidation protocol wins are a
philosophy difference, not a defect. RO snapshot serving is deduplicated by a
per-rank ``cached_version`` ledger (a monotonic floor of what each rank's
cache holds, credited by serves, by write-through publishes at the home, and
by the releaser/shipper itself under WB): a request from a rank whose ledger
already matches the master version receives a header-only, no-data reply.

Request combining layers on top of this dedup: at most one snapshot request
per DB and rank is in flight. A read acquire that finds one outstanding
parks, and the readers parked when it completes share the next request and
its response; a reader never rides a request sent before it parked, whose
response may predate the releases it is ordered after. Every remote
read-only acquire combines. It is orthogonal to write and release policy, and
it is how the validation pull path suppresses duplicate concurrent requests,
as every other message class does structurally (parked acquires under
EXCL/INV, batched invalidation rounds, publish flights).

INV — write-invalidate, durable reader copies (default)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

*Status:* implemented (``libs/src/core/coherence/inv/`` — ``directory.c`` +
``engine.c`` + ``wt.c`` + ``wb.c``, plus the shared grant plane
``coherence/grant.c``); selected by ``-DARTS_COHERENCE_PROTOCOL=INV`` (the
default), write policy by ``ARTS_WRITE_POLICY={WT,WB}`` (default ``WB``).

The mirror of VAL on the validity-agent axis: the *writer* keeps reader
copies valid. A reader's copy is durable — holding the copy is the
permission to read it, so a repeated read on that node is a pure local load
with no message, no atomic update, and no version comparison; a copy dies
exactly one way, when an invalidation arrives. In exchange, every RW release
returns only after an *invalidation round* covering it has closed: the home
snapshots its sharer roster (one bit per node, set before a copy is served,
so it safely over-approximates who might hold one), multicasts an
invalidation, and collects an acknowledgment from every target; concurrent
releases may be covered by one round. Write ownership is the same migrating
grant VAL uses, and the sharer plane is deliberately **version-free** — a
versioned sharer plane would already be halfway to VAL. Under ``WT`` the
release additionally publishes the payload to the home, which then serves
reads; under ``WB`` the publish is control-only and the home is a pure
directory that registers readers and redirects them to the owner.

EXCL — queue-fair distributed reader–writer lock
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

*Status:* implemented (``libs/src/core/coherence/excl/`` — ``arbiters.c`` +
``directory.c`` + ``purge.c`` + ``retain.c``); selected by
``-DARTS_COHERENCE_PROTOCOL=EXCL``, release policy by
``ARTS_RELEASE_POLICY={PURGE,RETAIN}`` (default ``RETAIN``).

Each DB carries a distributed reader–writer lock whose authoritative state is
a single 64-bit ``lock_state`` word on the home rank: shared read grants
overlap and a write grant excludes every other node, in the tradition of
queue-based reader–writer synchronization (Mellor-Crummey & Scott).  The lock
is granted to nodes, not to EDTs: on the node that holds the write grant, its
writers share the turn and its readers join it (a write grant covers reads),
as on the node-exclusive arms. Priority between read and write grants
is decided at the phase boundary, not on arrival: a queued writer takes
the lock at the transition that ends a read phase, while a reader arriving
during a live read phase is granted immediately rather than queued behind that
writer. The read phase therefore ends only when its readers do, so a workload
whose readers re-read without pause can hold writers off for as long as it
keeps a read outstanding — the deliberate throughput bias of this arm, and the
fairness exposure the adversarial suite measures. Data ships with the grant,
in the style of entry consistency — the consistency actions are scoped to the
DB whose lock is being acquired.
EXCL satisfies the OCR contract, and across nodes it is strictly stronger: a
reader on a node that holds no write grant is isolated from writers by
admission, not by promise.  It serves as the lock-philosophy point of the
design-space comparison.

Because no copy outlives a write turn, permission and payload are one token
here, and the release policy decides when that token goes back. Under
``PURGE`` the release that ends a node's turn carries the payload to the
home and returns the grant in the same message, making the home canonical;
the next turn re-requests and re-fetches. Under ``RETAIN`` a release with
nothing pending sends nothing: the home stays a pure directory naming the
holder (callback-locking style — it recalls the grant when another node
asks), and a second local write turn costs no traffic.

.. _flush_protocol:

FLUSH — fetch at acquire, flush at release (requires DB-WRF)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

*Status:* implemented (``libs/src/core/coherence/flush/flush.c``); selected
by ``-DARTS_COHERENCE_PROTOCOL=FLUSH``, whose programs must satisfy the
DB-WRF model (see :ref:`db_wrf_contract`). No write-policy or
release-policy axis: every remote acquire fetches and every remote RW
release writes back, unconditionally.

.. list-table::
   :header-rows: 1
   :widths: 20 40 40

   * - Event
     - Home rank
     - Other rank
   * - Acquire (RO or RW)
     - Pointer to the home line, in place (materialised at first use if the
       create took no hold)
     - Allocate a private copy of ``db_size`` bytes, ask the home; the home
       PUTs the line into it and the EDT resumes on the copy
   * - Release RO
     - Nothing
     - Drop the copy
   * - Release RW
     - Nothing (writes were already in place)
     - PUT the copy into the home line, send the commit, block until the
       home's ACK, then drop the copy

No versions, no per-rank ledger, no header-only replies, no request
combining, no write combining, no freelist, no grant, and no counters that
gate anything: every remote acquire moves the payload and every remote RW
release moves it back, one round trip each.

A private copy per EDT, rather than one shared line per rank, closes a
lost-update window: with a single shared line, an RO fetch issued before a
same-rank RW acquire could land after that RW EDT has started writing and
overwrite its in-progress writes, and nothing short of tracking in-flight
fetches would prevent it. A per-EDT copy has no such window and needs no
extra state. It is also the software analogue of a consumer flush
(invalidate, then reload from the pool): a hardware cache gives a same-node
reader no free hit either — same-rank readers and writers of one block do
not share bytes on a non-home rank, while on the home rank they all use the
line in place, exactly as they would use a pool.

The home keeps a sharer bitset — ranks that ever fetched the line or were
told its credential — used only by destroy fan-out; it is lifecycle
bookkeeping, not coherence state. FLUSH also serves as the no-optimization
baseline against the OCR-model protocols; it is a different mechanism from
the CXL store described above, which lives entirely inside ``EXCL`` ×
``PURGE`` and keeps that protocol's own state machine.

Rejected arms
~~~~~~~~~~~~~

* **MRSW** (multi-reader, single *global* writer; version-isolated reads) —
  its extra strength over OCR×VAL (forbidding even node-interleaved writer
  overlap) buys no benchmark insight for its maintenance cost. A mirror of
  the VAL engine with that admission is kept in ``archive/coherence-mrsw/``.
* **SRSW** (fully serialized single-reader/single-writer) — never built;
  useful only as a debug lower bound, and EXCL already serializes nodes.

.. _orthogonal_dimensions:

Orthogonal protocol dimensions
-------------------------------

The implemented axes fix nine points in a larger design space. The following
dimensions are orthogonal to each other; entries marked *roadmap* are
documented options, not commitments.

Write policy (payload placement)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Where does the canonical payload rest between write turns? Live under
INV/VAL (``ARTS_WRITE_POLICY``):

* **WT** (write-through at release) — the payload returns to the DB's home
  at every RW release, synchronously acknowledged; the home always holds a
  current copy and serves reads. Trades release-side flush bandwidth for
  one-hop reads.
* **WB** (write-back, default) — no payload moves at release: the canonical
  payload stays with the last writer, and the home directory forwards
  requesters to the owner. Trades a forwarding hop on first access for zero
  flush traffic while ownership is reused.

Under EXCL this axis is pinned at WB: no reader copy outlives a write turn,
so a per-release flush would have nobody to serve, and the payload instead
rides the permission (the release policy below decides both).

Release policy (permission return)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When does the write grant go back? Live under EXCL
(``ARTS_RELEASE_POLICY``), and — since it governs the same migrating grant —
under ``WT`` in VAL/INV as well:

* **PURGE** — the holder returns copy and permission unasked, at its own
  idle edge (the moment its last local writer finishes); the home never
  sends a holder anything to make this happen.
* **RETAIN** (default) — both stay until another node asks; the home recalls
  them on demand (callback-locking style), so a repeated local turn is free.

Under EXCL no reader copy outlives a write turn, so payload and permission
are one token and this axis alone decides when it returns — see the EXCL
family description above for the mechanics.

Under VAL/INV the write policy already moves the *payload* at every release
under ``WT``, so PURGE there is a decision about the *grant* alone, layered
on top of that:

* Every grant transfer, in either direction, passes through the home — a
  holder never sends another holder a revocation, and, unlike RETAIN, never
  receives one from the home either. An idle holder hands the grant back on
  its own; a holder that wants the grant queues at the home like any other
  requester.
* Because the payload already reached the home on the release that made the
  holder idle, the permission's return costs no data motion — only a
  metadata handoff. This is exactly why PURGE is built at ``WT``, where that
  holds, and refused at ``WB``, where it does not (see the combination list
  above).
* The return travels with that same release's write-through publish when
  one is still in flight, at no extra message cost; a release with nothing
  left to publish sends a standalone return instead. Either path lands the
  grant back at the home, which serves the next request from there — a new
  holder never talks to the one before it.
* A grant request carries the requester's already-installed buffer version.
  When it already matches the DB's canonical version — the common case for
  a rank re-acquiring a DB it already holds a current copy of — the home
  grants permission only, with no payload in the reply.

Under ``WB`` the release policy is pinned at RETAIN in every family: a
voluntary return there would hand back only the permission while the
canonical bytes stay at the ex-owner, so the home still could not serve a
reader on its own — and shipping the bytes home too is, by definition,
write-through.

Propagation strategy
~~~~~~~~~~~~~~~~~~~~

How does fresh data reach readers?

* **Pull-on-acquire** (current VAL) — the acquirer pulls a versioned snapshot
  from the serving side; stale replicas are never invalidated, they simply
  age out when re-acquired. One round trip per (non-deduplicated) acquire.
* **Invalidate-then-reread** (current INV) — stale copies are retired by
  the writer's release-time round; a surviving copy is read with no message
  at all, and a retired one is re-fetched on the next acquire.
* **Ship-with-grant** (current EXCL) — data travels with the lock grant;
  admission control makes staleness impossible by construction.
* **Update-and-push** (roadmap) — the releasing node proactively pushes the
  new value to current readers. Eliminates the pull round trip when the same
  DB is re-acquired RO by many readers in succession; costly if the push is
  discarded. Aggregation-style RO improvements are the accepted evolution
  path for the VAL family (never reader invalidation).

Directory / ownership
~~~~~~~~~~~~~~~~~~~~~

Where does coherence bookkeeping live?

* **Fixed home** (all current arms) — each DB has a designated home rank
  (set at creation; encoded in the GUID rank field). The home holds the
  authoritative directory entry (VAL×OCR: grant directory; INV: grant
  directory + sharer roster; EXCL: ``lock_state``; FLUSH: the canonical
  payload itself — the home line). Simple and
  predictable; the home can become a hot-spot for highly shared DBs.
* **Migratory owner** (current WB placements, and EXCL under RETAIN) — the
  payload (and, under VAL, the per-rank serving ledger) migrates with the
  write grant; the home keeps only directory state and forwards requesters.
  Full pointer-forwarding migration (home consulted only on miss) remains
  roadmap.
* **Hierarchical NUMA/cluster** (roadmap) — directory partitioned into
  levels (intra-socket, inter-socket, inter-node), exploiting locality to
  reduce long-distance traffic.

Read consistency
~~~~~~~~~~~~~~~~

What version of the DB's contents does a reader observe?

* **Position-dependent** (current VAL) — a reader co-located with the owner
  binds to the live shared buffer; a remote reader receives an
  installed-version snapshot copy. Both are legal under both contracts.
* **Durable-copy** (current INV) — a reader's copy stays legal until an
  invalidation retires it, so every re-read between two writes is
  message-free and observes the last completed release.
* **Lock-isolated** (current EXCL) — a reader on another node is admitted
  only when no node holds the write grant, so it observes a quiescent DB; a
  reader on the node that holds it joins that node's write turn and reads
  the live buffer its writers write, as a co-located VAL reader does.

.. note::

   The VAL position dependence gives racy programs *location-variable*
   semantics: a co-located racy reader sits on live hardware-coherent memory
   (effectively the machine's TSO floor), while a remote racy reader sees a
   frozen snapshot that guarantees nothing for a word written while it was
   copied, not even that the word holds a value some EDT wrote — reading in
   inverse write order no longer proves a publication prefix. Both behaviors
   are within the contract (a racing read's value is unspecified), but a racy
   flag-then-payload idiom can appear to work single-node and break
   distributed. This is a property of any snapshot-serving conforming
   implementation, and one more reason racy idioms are non-portable.

Multi-writer resolution
~~~~~~~~~~~~~~~~~~~~~~~

When two writers' accesses to one DB are unordered, how are their writes
reconciled?

* **Node-exclusive serialization** (OCR×VAL, OCR×INV) — intra-node:
  concurrent hardware-coherent writes to a shared buffer; inter-node: the
  single-owner grant serializes writers one node at a time. Preserves the
  OCR non-overlapping rule.
* **Admission exclusion** (OCR×EXCL) — the lock never grants the write turn
  to two nodes at once; within the node that holds it, writers share the
  turn and their concurrent writes are hardware-coherent, as under
  node-exclusive serialization.
* **Lossy whole-DB write-back** (FLUSH) — each RW release PUTs the whole DB
  back into the home line. A DB-WRF-valid program never has two such
  releases overlapping, so nothing is ever actually lost; a program that
  violates the contract's write-exclusivity guarantee gets undefined
  results here precisely because the last write-back wins with no merge.
* **Non-lossy merge** (roadmap) — diff- or twin-based merging in the
  TreadMarks / Midway tradition would reconcile concurrent writers' disjoint
  changes, achieving the full OCR non-overlapping rule while admitting true
  inter-node multi-writer access. This is the only path to a genuinely
  OCR-conformant multi-writer protocol.

Per-DB-kind contracts
---------------------

The global axes govern regular DRAM DBs (``ARTS_DB``). The other storage
kinds carry their own fixed, documented semantics regardless of the global
axes: ``ARTS_DB_PIN`` and ``ARTS_DB_GPU_PIN`` perform no DB-level coherence;
``ARTS_DB_GPU`` allows concurrent per-device replicas merged by reduction at
release. All of these are **app-ordered (full DRF) contracts**: the
application must event-order *every* conflicting access pair — read-write
included — because there is no snapshot or admission machinery underneath.

.. _cdag_dropped:

Considered and dropped: CDAG
-----------------------------

*Cache DAG Consistency* (Landwehr et al., 2017) is a variant of DAG
consistency (Blumofe et al., 1996) in which a task observes the write of the
immediately preceding ancestor version of a shared location ("last-ancestor"
read). It effectively maps to a globally single-writer admission policy with
version-isolated reads — the MRSW design point (see the rejected arms
above).

CDAG was considered as a coherence option for ARTS and was dropped:

1. **Small delta over the VAL engine.** The single-owner grant already
   serializes inter-node writers; adding global writer exclusion and
   last-ancestor reads is a refinement of MRSW, not a separate protocol —
   and MRSW itself buys no benchmark insight.
2. **Weaker parallelism.** Forbidding competing concurrent writes entirely
   reduces parallelism for programs that exploit the OCR non-overlapping
   data-race rule.
3. **Recoverable.** ``archive/coherence-mrsw/`` preserves the engine CDAG
   semantics would build on, should the design point be revisited.

References
----------

* OCR working group, *The Open Community Runtime Interface*, v1.2.0, 2016 — §1.6.
* J. Dokulil, *Consistency model for runtime objects in the Open Community Runtime*, J. Supercomputing, 2018.
* P. McKenney, J. Slingwine, *Read-Copy Update: Using Execution History to Solve Concurrency Problems*, PDCS 1998.
* J. Mellor-Crummey, M. Scott, *Scalable Reader-Writer Synchronization for Shared-Memory Multiprocessors*, PPoPP 1991.
* S. Adve, M. Hill, *Weak Ordering — A New Definition*, ISCA 1990 (DRF).
* D. Hower et al., *Heterogeneous-race-free Memory Models*, ASPLOS 2014 (HRF).
* T. Landwehr et al., *Designing Scalable Distributed Memory Models*, SC 2017 (Cache DAG Consistency).
* G. R. Gao, V. Sarkar, *Location Consistency — A New Memory Model and Cache Consistency Protocol*, IEEE TC 49(8), 2000.
* L. Censier, P. Feautrier, *A New Solution to Coherence Problems in Multicache Systems*, IEEE ToC, 1978 (directory protocols).
* J. Archibald, J.-L. Baer, *Cache Coherence Protocols: Evaluation Using a Multiprocessor Simulation Model*, ACM TOCS, 1986 (write-invalidate vs write-update).
* K. Gharachorloo et al., *Memory Consistency and Event Ordering in Scalable Shared-Memory Multiprocessors*, ISCA 1990.
* P. Keleher et al., *Lazy Release Consistency for Software Distributed Shared Memory*, ISCA 1992.
* B. Bershad, M. Zekauskas, W. Sawdon, *The Midway Distributed Shared Memory System*, COMPCON 1993.
* L. Iftode, J. P. Singh, K. Li, *Scope Consistency: A Bridge between Release Consistency and Entry Consistency*, SPAA 1996.
* R. Blumofe et al., *DAG-Consistent Distributed Shared Memory*, IPPS 1996.
