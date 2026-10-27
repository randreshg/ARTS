Performance Counters (``counters.cfg``)
=======================================

ARTS includes a configurable performance counter system for profiling
EDT execution, DataBlock usage, memory footprint, and network traffic.

.. contents:: On this page
   :local:
   :depth: 2

File Format
-----------

A counter configuration lives in the source tree and is processed at CMake
configure time; the ``ARTS_COUNTER_CONFIG`` CMake cache variable names it.
The default, ``configs/counters_off.cfg``, turns every counter off;
``configs/counters.cfg`` is the profiling example a campaign opts into, and
``configs/counters_validation.cfg`` is the validation trees' set, which
captures only the counters a test reads back.  After editing, re-run
``cmake`` to regenerate counter code.

Each line follows the format:

.. code-block:: text

   COUNTER_NAME=MODE[,LEVEL[,REDUCE]]

Parameters
~~~~~~~~~~

**Mode** (required):

- ``OFF`` — counter disabled.
- ``ONCE`` — captured once at shutdown.
- ``PERIODIC`` — captured at regular intervals (see
  ``counter_capture_interval`` in ``arts.cfg``).

**Level** (optional, default ``NODE``):

- ``THREAD`` — per-thread output, no reduction.
- ``NODE`` — per-node output, reduced across threads.
- ``CLUSTER`` — cluster-wide output, reduced across all nodes.

**Reduce** (optional, default ``SUM``):

- ``SUM`` — sum values.
- ``MAX`` — maximum value.
- ``MIN`` — minimum value.
- ``MASTER`` — master thread/node value only.

Naming Convention
~~~~~~~~~~~~~~~~~

All counter names use a type prefix:

- ``TIME_*`` — timer counters (nanoseconds, measured with START/STOP).
- ``NUM_*`` — count counters (event occurrences, measured with INCREMENT_BY).
- ``BYTES_*`` — byte counters (accumulated bytes, measured with INCREMENT/DECREMENT_BY).

Examples
~~~~~~~~

.. code-block:: ini

   # Disabled
   TIME_EDT_EXEC=OFF

   # Capture once at end, cluster-wide sum
   TIME_EDT_EXEC=ONCE,CLUSTER

   # Periodic per-thread capture (no reduction)
   TIME_EDT_EXEC=PERIODIC,THREAD

   # Periodic cluster-wide maximum
   TIME_EDT_EXEC=PERIODIC,CLUSTER,MAX

Available Counters
------------------

The list below follows the ``ARTS_COUNTER_LIST`` X-macro in
``libs/include/internal/arts/counter/counter.h`` — the single source of
truth the enum, the string table, and ``artsrun``'s catalog are all
derived from.  Grouping follows the header's own comments.

EDT Lifecycle — Time
~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``TIME_EDT_EXEC``
     - Wall time inside EDT bodies, summed over a rank's threads.  It
       includes every runtime call the body makes, not the DB acquires
       before the body or the releases after it.
   * - ``TIME_EDT_CREATE``
     - Wall time of ``arts_edt_create()`` on the creating thread:
       placement, allocation, GUID, finish-scope join, and either the ship
       to a remote home or the local install, which readies the EDT and
       starts its DB acquires when its dependences are already met.
   * - ``TIME_EDT_SIGNAL``
     - Wall time of ``arts_edt_satisfy_slot()`` on the satisfying thread,
       including readying the EDT and starting its DB acquires when the
       satisfy is its last; a satisfy of a remote EDT is only the send.

EDT Lifecycle — Count
~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``NUM_EDT_CREATE``
     - EDT creates issued on this rank, whatever the EDT's home.  Includes
       the runtime's main EDT and, under the OCR shim, the trampoline EDT
       that runs ``mainEdt``.
   * - ``NUM_EDT_ACQUIRE``
     - EDTs whose dependences and DB acquires all completed and that were
       queued to run, counted on the EDT's rank.
   * - ``NUM_EDT_FINISH``
     - EDTs whose body returned, counted on the executing rank.  Includes
       the main EDT and the shim's trampoline, not an EDT destroyed before
       it ran or abandoned at shutdown.
   * - ``NUM_EDT_SIGNAL``
     - EDT dependence-slot satisfactions (``arts_edt_satisfy_slot()``
       calls), counted on the rank that issues them.

DataBlock Lifecycle — Time
~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``TIME_DB_CREATE``
     - Wall time of ``arts_db_create()`` on the creating thread:
       placement, descriptor and payload allocation, GUID mint, and the
       local install or the announce to a remote home.  No payload is
       copied: a created block's payload is uninitialized.

DataBlock Lifecycle — Count
~~~~~~~~~~~~~~~~~~~~~~~~~~~

A create is counted on the creating rank, whatever the block's home.  An
acquisition is one (EDT, block) pair: an EDT whose slots name one block
several times acquires it once, in the strongest mode they name, and
that acquisition is counted on the EDT's rank when its slot reaches the
block's coherence arm (a slot deferred until the block exists is counted
when its replay gets there).  A create that takes the creator's hold is
a write acquisition of its own.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``NUM_DB_CREATE``
     - ``arts_db_create()`` calls that returned a block, with or without a
       hold.
   * - ``NUM_DB_CREATE_NO_ACQUIRE``
     - The creates ``NUM_DB_CREATE`` counts that passed
       ``ARTS_DB_PROP_NO_ACQUIRE`` (no creator hold).
   * - ``NUM_DB_DESTROY``
     - ``arts_db_destroy()`` calls on a DataBlock GUID, counted on the
       calling rank.
   * - ``NUM_DB_ACQUIRE_READ``
     - EDT acquisitions whose strongest mode is RO.
   * - ``NUM_DB_ACQUIRE_WRITE``
     - EDT acquisitions whose strongest mode is RW, plus every create that
       takes the creator's hold.

Coherence — Acquire Resolution
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Every acquisition ``NUM_DB_ACQUIRE_READ`` and ``NUM_DB_ACQUIRE_WRITE``
count is classified once, on its rank, by the same rule on every coherence
cell: by where the permission and the copy it needs are, never by the
messages it caused or by what it waited behind.  A write is a local hit when
its rank holds the block's write right, already or still after a change of
phase on that rank (read turns recalled, a read-only phase ended), and remote
when the write right had to move to its rank from another rank.  A read is
a local hit when its rank holds a copy it may read without asking (a read
turn or read-only phase with a valid copy, a durable copy, the write right,
or, under ``VAL``, the block's home answering itself), and remote when the
copy or the read permission had to come from another rank; under ``VAL`` a
read off the block's home rank that its rank's write right does not cover is
remote, since its version check goes to the home.  Under the voluntary-return
release policy (``PURGE``) the write right belongs to the block's home, so
every acquisition on the home rank is a local hit.  The state at an
acquisition's decision classifies it: one whose rank holds what it needs there
is a local hit even if it then waits behind a gate (a write right installed on
its rank that the home has not yet confirmed).  One that parks without it is
classified by where what it waits for comes from, whichever serve takes it:
remote when that serve installs a permission or a copy another rank sent (a
grant, a migration, a read reply) or wakes the readers of a first publish
another rank made, local when it serves from what the rank already holds (a
phase change on the rank, a create's claim of its first-touch cache, a first
publish of its own).  On a one-rank run every acquisition is a local hit.
``NUM_DB_ACQUIRE_LOCAL_HIT + NUM_DB_ACQUIRE_REMOTE = NUM_DB_ACQUIRE_READ +
NUM_DB_ACQUIRE_WRITE`` holds on every cell for a run whose shutdown is
quiescent: an acquisition a non-quiescent shutdown leaves parked and unserved
is counted in ``NUM_DB_ACQUIRE_READ`` or ``NUM_DB_ACQUIRE_WRITE`` and in
neither of the two.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``NUM_DB_ACQUIRE_LOCAL_HIT``
     - Acquisitions whose rank held what they need at their decision (a write
       right installed but not yet confirmed included): the write right for a
       write; for a read, a valid copy it may read, a read turn, the write
       right or (``VAL``) the block's home answering itself.  A create's hold,
       a zero-size block, and a serve from what the rank holds (a phase change
       on the rank, a create's claim of its first-touch cache) included; under
       ``PURGE`` every acquisition on the block's home rank.
   * - ``NUM_DB_ACQUIRE_REMOTE``
     - Acquisitions whose permission or copy had to come from another rank:
       a grant or migration of the write right, a read turn, read reply or
       first publish another rank sent, and a ``VAL`` read off the block's
       home rank that its rank's write right does not cover.

DataBlock Lifecycle — Bytes
~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``BYTES_DB_CREATE``
     - Declared bytes of the blocks ``NUM_DB_CREATE`` counts.

Coherence — Payload
~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``BYTES_DB_PAYLOAD_SENT``
     - Datablock bytes this rank put on the fabric one-sided.  A CXL
       store's transfers are the CXL counters.
   * - ``NUM_DB_PAYLOAD_SENT``
     - Datablock payloads this rank put on the fabric one-sided, one per
       payload however many pieces the provider's message ceiling cut it
       into.
   * - ``TIME_DB_PAYLOAD_PUT``
     - Time from a payload PUT's post to its last piece's local completion
       (the provider done reading the source, not the bytes visible at the
       peer), time on the sender's retry ring included.  Added by the
       thread that reaps that completion, so it includes time queued
       behind a busy provider.  Concurrent PUTs each add their own time:
       ``BYTES_DB_PAYLOAD_SENT`` over this is the per-transfer throughput,
       not the link's aggregate bandwidth.

Memory — Bytes
~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``BYTES_MEMORY_FOOTPRINT``
     - Usable bytes held through the runtime's ``arts_malloc`` family:
       control structures, not DB payloads or the registered pool.  A
       thread's own value wraps when another thread frees what it
       allocated, so read it as a node or cluster ``SUM``.

Network — Bytes
~~~~~~~~~~~~~~~

Two-sided messages and one-sided datablock payloads together.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``BYTES_REMOTE_SENT``
     - Bytes this rank put on the fabric: each two-sided message's header
       and tail plus each one-sided datablock payload, with no provider
       framing and no retries.
   * - ``BYTES_REMOTE_RECEIVED``
     - Bytes this rank took in from the fabric: each dispatched two-sided
       message's header and tail plus each landed datablock payload's
       whole length, counted when the payload pairs with its metadata.

Network — Count
~~~~~~~~~~~~~~~

Two-sided messages only; the one-sided payloads are
``NUM_DB_PAYLOAD_SENT``.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``NUM_REMOTE_SEND``
     - Two-sided messages this rank sent.
   * - ``NUM_REMOTE_RECEIVE``
     - Two-sided messages this rank dispatched.

CXL Flush
~~~~~~~~~

Data-block flushes in a CXL build, counted inside ``arts_cxl_store_flush``
where every one runs: the cache-line sweep and its fence, not the staged
build's copy beside it.  The producer flush is the purge that publishes a
turn's writes to the store; the consumer flush is the fetch that drops this
rank's stale private lines before a grant is read.  Flushes of the arena
table's own metadata, and of the tournament lock that serializes an arena
bump across ranks, are not counted.  All six stay zero in a build without
a CXL store.  Each timed flush adds two clock reads, which is comparable to
flushing a single cache line, so per-flush means for small blocks run high.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``NUM_CXL_FLUSH_PRODUCER``
     - Producer flushes (purges).
   * - ``NUM_CXL_FLUSH_CONSUMER``
     - Consumer flushes (fetches).
   * - ``BYTES_CXL_FLUSH_PRODUCER``
     - Declared bytes of the blocks producer flushes swept.
   * - ``BYTES_CXL_FLUSH_CONSUMER``
     - Declared bytes of the blocks consumer flushes swept.
   * - ``TIME_CXL_FLUSH_PRODUCER``
     - Time in producer flushes (sweep + ``sfence``).
   * - ``TIME_CXL_FLUSH_CONSUMER``
     - Time in consumer flushes (sweep + ``mfence``).

Event — Count
~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``NUM_EVENT_CREATE``
     - Event creates issued on this rank, including the finish-scope
       proxies the runtime makes on an EDT's home when the EDT's scope is
       homed elsewhere.
   * - ``NUM_EVENT_DESTROY``
     - Events retired on their home: an explicit destroy, an auto-destroy
       after firing, or a counted event's last bind.  Summed over the
       cluster, creates minus this is what the run still holds.

Scheduling — Count
~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``NUM_STEAL_SUCCESS``
     - EDTs a worker took from another thread's deque, a worker's or a
       progress thread's, and ran; counted on the thief.

Out-of-Order
~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``NUM_OO_ENQUEUE``
     - Number of pushes onto a route slot's OoO list, one per park or re-
       park of a payload: a non-create operation whose target is not
       installed, or a create whose GUID is still occupied.

Validation Arm (VAL) — Read Requests
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A reply with no payload is one the version ledger saved; a size round
(``data_present`` 2, never a dismiss) negotiates a landing for a request
that could not size one.  A window is a read request that is not a re-
send.  Inert unless the build compiles the VAL family in.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``NUM_SNAPSHOT_REQUEST``
     - VAL read requests this rank sent, one per message: re-sends after a
       size round, a dismiss or a write give-back, and the home's request
       to itself, included.
   * - ``NUM_SNAPSHOT_HEADER_ONLY``
     - Read replies with no payload because the requester's ledger already
       held that version, counted at the replying rank.
   * - ``NUM_SNAPSHOT_SIZE_CTS``
     - Read size rounds, counted at the replying rank (the home, or the
       owner under WB).
   * - ``NUM_RO_COMBINE_WINDOW``
     - Read windows opened: a first reader's request, the next window an
       answer opens, and a write give-back's.
   * - ``NUM_RO_COMBINE_JOINED``
     - Readers that sent no request of their own: parked for the next
       window or riding the rank's write request.

Size Rounds on the Other Planes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Counted at the replying rank, a dismiss never among them.  With the GUID
size hint only a reserved or labeled GUID (or a size past the hint's
range) takes one, so a nonzero steady rate is the hint NOT covering a
workload.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``NUM_GRANT_SIZE_CTS``
     - Size rounds on the migrating-grant plane (VAL, INV).
   * - ``NUM_EXCL_SIZE_CTS``
     - Size rounds on the EXCL plane.
   * - ``NUM_INV_SIZE_CTS``
     - Size rounds on the INV read plane.
   * - ``NUM_FLUSH_SIZE_CTS``
     - Size rounds on the FLUSH fetch plane.

Invalidation Arm (INV)
~~~~~~~~~~~~~~~~~~~~~~

A round batches every release queued at the home when it starts, and
retires them with one multicast; the release blocks until its round's
acks are in.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``NUM_INVALIDATE_ROUND``
     - Invalidation rounds at the home that retired at least one release.
   * - ``NUM_INVALIDATE_ACK``
     - Sharer copies the rounds targeted, one ack each (a target that held
       no valid copy and the WB home's own copy included).
   * - ``TIME_INVALIDATE_ROUND``
     - Time an RW release blocks on its round; under WT it includes the
       payload's write-through, and a release that joined another's flight
       waits for that flight.

Migrating Write Permission
~~~~~~~~~~~~~~~~~~~~~~~~~~

A write request a rank committed versus a write turn a later local
writer took on a grant it still held (the sticky grant).  Live in the
arms whose ownership migrates (VAL, INV).

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``NUM_GRANT_MIGRATE``
     - Write requests this rank committed, one per grant round asked for,
       the home's request to itself included.
   * - ``NUM_GRANT_LOCAL_REUSE``
     - Write turns taken on a grant this rank already held.

Exclusion Arm (EXCL)
~~~~~~~~~~~~~~~~~~~~

Requests the home could not grant at its decision: the reader/writer
serialization its philosophy pays for.  Under the retention release
policy the home also asks retained read grants back before a write
phase.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``NUM_EXCL_QUEUE_WAIT``
     - Requests, read or write, the home could not grant at its decision
       and queued.
   * - ``NUM_EXCL_RECALL_SENT``
     - Read grants the home asked back so a writer could proceed, one per
       retaining rank, itself included.
   * - ``NUM_EXCL_RECALL_EARLY``
     - Recalls that overtook their grant and had to be remembered rather
       than acted on, once per recall.

Write-Through Publish Flight Machine
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

These count the RARE transitions a green suite cannot prove exercised —
a campaign where one stays zero ran no coverage of that path, not a
healthy path.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``NUM_PUB_FLIGHT``
     - Payload/control flights launched.
   * - ``NUM_PUB_FLIGHT_JOIN``
     - Releases coalesced into an open flight (the write-combining win).
   * - ``NUM_PUB_FLIGHT_TRAILING``
     - Relaunches for waiters the ACK left uncovered.
   * - ``NUM_PUB_CTS_FALLBACK``
     - Credit-less announce legs (steady nonzero = the credit teachers are
       not covering a workload).

Grant Plane Rare Paths
~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``NUM_GRANT_BATON_RECLAIM``
     - A releasing claimant's post-release re-check found queued work (a
       request, or under the voluntary-return policy a parked hand-back)
       and re-claimed the baton: the lost-wake window the re-check loop
       exists to close.
   * - ``NUM_GRANT_HOME_DATALESS``
     - Write requests the write-through home sent its own directory,
       asking for the permission alone (it already holds the newest
       bytes).

Voluntary-Return Release Policy
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``NUM_PURGE_RETURN`` counts this policy's hand-backs on every arm.  On
the grant plane ``RETURN`` and ``FLAG`` are the two vehicles of a
hand-back and do not overlap: their sum is every hand-back, and
``RETURN`` alone is what the fallback still costs.  ``RETURN`` plus
``FLAG`` minus ``ACCEPT`` across the cluster is what is still in flight.

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Counter
     - Description
   * - ``NUM_PURGE_RETURN``
     - Hand-backs of a permission, with its bytes if any, that a rank sent
       the block's home, counted on the returning rank; the home's own idle
       edge sends nothing.  On the grant plane ``NUM_GRANT_PURGE_RETURN +
       NUM_GRANT_PURGE_FLAG``; under exclusion every read and write turn a
       rank other than the home gave back.
   * - ``NUM_GRANT_PURGE_RETURN``
     - Write rights handed back at a holder's own idle edge, in a message
       of their own.
   * - ``NUM_GRANT_PURGE_FLAG``
     - Hand-backs that rode a release's own publish.
   * - ``NUM_GRANT_PURGE_ACCEPT``
     - Hand-backs the home took.
   * - ``NUM_GRANT_PURGE_LATCHED``
     - Hand-backs parked because another thread was inside the home's
       serving tail: the contended hand-off the single-slot latch exists
       for.
   * - ``NUM_GRANT_PURGE_STALE_SETTLE``
     - Settles that found the hand-back token armed under a later grant
       generation and left it alone.
   * - ``NUM_GRANT_REGRANT_DEDUP``
     - Grants served with the permission alone because the requester's own
       copy was already the current one.

.. note::

   End-to-end wall time is not a counter but the env-gated ``[E2E] <ns>``
   stderr marker: set ``ARTS_E2E_MARKER`` in the environment and rank 0 prints
   the span from application start to shutdown recognition on exit (see
   ``libs/src/core/system/threads.c`` / ``runtime.c``), in the same form the
   reference runtimes (xsocr, ocr-vx) use so a harness parses all three
   identically.

   ``ARTS_REGPOOL_REPORT`` is the other env-gated marker: set it and every
   rank prints the registered pool's shape to stderr at teardown — at the
   pool's cleanup, or, when a thread was left unjoined, by the teardown that
   leaves the pool open, so in a ``mimalloc`` build a missing line means a
   rank that never reached its teardown —
   ``[REGPOOL] node <n>:`` per NUMA node (carve state, arena count, mapped
   MiB, grow count, the milliseconds its grows spent mapping and the number
   and milliseconds of waits — a grow attempt that found the node's grow
   already in flight, or the process-wide turn of a kernel-placed grow — the
   cost an exhausted node charges the threads that reach it behind the
   mapping of its next slab or another node's kernel-placed one — and
   ``spills``, the allocations of its threads that the exhaustion cascade
   served from another node's arena because the node could not grow, and
   ``cascades``, the allocations of its threads that left the fast path for
   that cascade), ``[REGPOOL] direct:`` for the
   oversize mappings (live, retired, free table slots),
   ``[REGPOOL] direct placement:`` (how many of them, and how many MiB, are
   ``interleaved`` across the rank's node set,
   landed on a single ``node<k>``, or were left ``unplaced`` for the kernel
   to position) and ``[REGPOOL] total:`` (with ``arena_slots``, the
   allocator arena-table slots the pool's slabs hold out of the table's
   size, ``table_refusals``, the slabs the full table refused — nonzero
   means the pool stopped growing its arenas for the rest of the run — and
   ``theap_refusals``, the binds since the pool's init that found an arena
   with no free slice for the calling thread's part of its heap; the thread
   was served from another arena or a fresh one).  The
   pool's resident set is its mapped capacity, so these lines are what a
   memory measurement is read against; they cost nothing when the variable
   is unset.

Output
------

Counter output is written as JSON to the directory specified by
``counter_folder`` in ``arts.cfg`` (default: ``./counters``).

File naming depends on the counter level:

- **THREAD** — ``n{node}_t{thread}.json`` (one file per thread).
- **NODE** — ``n{node}.json`` (one file per node, reduced across threads).
- **CLUSTER** — ``cluster.json`` (single file on the master node, reduced
  across all nodes).

A thread the shutdown could not join writes no ``n{node}_t{thread}.json``,
and its counts are missing from the node and cluster values.

Each JSON file includes a ``timestamp`` (Unix epoch), a ``version``
string, and a ``counters`` object whose keys are counter names.
``PERIODIC`` counters include a ``captureHistory`` array of
``[epoch, value]`` pairs.
