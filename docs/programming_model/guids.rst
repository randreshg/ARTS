GUIDs
=====

Every runtime object in ARTS — EDTs, DataBlocks, events — is
identified by a 64-bit **Globally Unique Identifier** (GUID).

.. contents:: On this page
   :local:
   :depth: 2

Bitfield Layout
---------------

.. code-block:: text

   ┌──────────┬──────────────────┬────────────────────────────────────────┐
   │ kind (2) │   rank (14)      │              key (48)                  │
   └──────────┴──────────────────┴────────────────────────────────────────┘
    Bits 63–62    Bits 61–48                   Bits 47–0

- **kind** (2 bits): Object kind from :c:enum:`arts_guid_kind_t`
  (``ARTS_GUID_DB`` / ``ARTS_GUID_EVENT`` / ``ARTS_GUID_EDT``; the
  all-zero pattern is the reserved/NULL sentinel).
- **rank** (14 bits): Node that owns the object (up to 16 384 nodes).
- **key** (48 bits): Node-local unique key, stored in the
  least-significant bits so that GUID-range arithmetic reduces to plain
  integer addition.

Inspecting GUIDs
----------------

.. code-block:: c

   arts_guid_t guid = ...;

   unsigned int rank = arts_guid_get_rank(guid);
   arts_guid_kind_t kind = arts_guid_get_kind(guid);
   bool local = arts_guid_is_local(guid);

GUID Ranges
-----------

For bulk allocation, use GUID ranges to reserve a contiguous block of
keys:

.. code-block:: c

   arts_guid_t start =
       arts_guid_reserve_range(ARTS_GUID_DB, 100, target_node);

   for (unsigned int i = 0; i < 100; i++) {
       arts_guid_t g = arts_guid_from_index(start, i);
       /* use g ... */
   }

Round-Robin Allocation
~~~~~~~~~~~~~~~~~~~~~~

To distribute the homes of a range evenly across nodes, pass the
``ARTS_HINT_ROUND_ROBIN`` sentinel rank (``home = idx % nrank``;
broadcast the range GUID to every rank that derives children):

.. code-block:: c

   arts_guid_t start =
       arts_guid_reserve_range(ARTS_GUID_DB, total_count,
                               ARTS_HINT_ROUND_ROBIN);

``NULL_GUID``
-------------

The sentinel ``NULL_GUID`` (``0x0``) represents an absent or invalid
GUID.  Always check against it before dereferencing:

.. code-block:: c

   if (guid != NULL_GUID) {
       /* safe to use */
   }

.. _labeled-guid-reuse:

Labeled GUIDs: one rule for every create
----------------------------------------

A GUID can be named before its object exists: an index into a reserved
range (:c:func:`arts_guid_reserve_range` + :c:func:`arts_guid_from_index`,
or ``ocrGuidFromIndex`` through the OCR shim) passed as ``hint.guid`` to
:c:func:`arts_edt_create`, :c:func:`arts_event_create` or
:c:func:`arts_db_create`.  Every create of such a GUID — EDT, event or data
block, whatever property it carries — follows one rule at the rank whose
route slot the object occupies:

- **The slot is empty:** the create installs its object, then replays the
  messages that reached the GUID before it (a satisfy, a dependence, an
  acquire, a destroy).
- **The slot holds a live object:** the create waits, parked on the slot.
  Nothing is replaced, dropped or reported.  When the occupant's life ends —
  its destroy, or, for an EDT, its completion — the parked create installs
  as the GUID's **next generation**.

Every other message keeps its own rule: it runs against the object the slot
holds, and while the slot is empty it waits for the next install.  A create
parked behind an occupant that never ends stays parked until shutdown, where
a debug build's teardown walk counts it among the work left undone (the
``ooo`` contributor), so the shutdown is reported non-quiescent.

What a create returns does not depend on whether it waited:

- :c:func:`arts_event_create` and :c:func:`arts_edt_create` return the GUID.
- :c:func:`arts_db_create` returns at once, and ``*addr`` names this
  create's own image of the block (``NULL`` under
  ``ARTS_DB_PROP_NO_ACQUIRE``), which becomes the block's content when the
  create installs.  The creator's hold is on that image, and the
  creator's release goes to it, not to whatever the slot holds meanwhile.

The OCR standard's two checked properties have no native counterpart, and
the OCR shim accepts and ignores both:

``GUID_PROP_CHECK``
    the standard reports ``OCR_EGUIDEXISTS`` to a creator whose label is
    taken.  "Already exists" is not a fact the runtime can state when a
    creator asks: a create on another rank is fire-and-forget, and a create
    that lands before the destroy of the previous generation would be told
    about a generation the program has already retired.  No creator is told
    anything about a label.

``GUID_PROP_BLOCK``
    the standard waits until the label can be created again.  That is what
    every create does.

Reusing a label
~~~~~~~~~~~~~~~

Reusing a label across a lifetime boundary is supported:

.. code-block:: c

   arts_db_destroy(g);                  /* generation k   */
   arts_db_create_with_guid(g, ...);    /* generation k+1 */

The destroy and the create need no ordering between them: a create that
reaches the slot before the destroy waits for it.  An EDT's completion ends
its life the same way — the runtime retires the EDT's GUID before its output
event or its finish scope is signalled, so a task gated on either can create
the GUID's next generation.  What the program still orders is set out under
`Known weaknesses`_: the create is the only message the runtime holds back
for a destroy.

Concurrent creates are undefined
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Two creates of one GUID with no end of life between them are **undefined**.
Exactly one installs; every other one parks for a destroy the program may
never issue.  The runtime does not arbitrate them, report them, or carry
logic to survive them; this is how the contract says *one create per
lifetime*.  What the undefined shape costs:

- A parked EDT create holds open the finish scope its creator joined it to
  (the creating rank counted it in at creation), so that scope never fires
  and a program that waits for it does not finish.
- A parked event or data-block create keeps its payload until a destroy of
  the occupant admits it, or to shutdown if none comes.  A losing data-block creator still receives a pointer — its own
  image — but what it writes there reaches the block only if its create
  installs, as the generation after the occupant's destroy, never the
  occupant itself.  ``ARTS_DB_PROP_NO_ACQUIRE`` changes nothing here: it
  drops the creator's hold, not the create.
- The OCR idiom in which every rank creates the same sticky event and one of
  them satisfies it therefore installs one event and parks every other
  create behind it.  Satisfies and dependences from every rank reach the
  installed event through its GUID, so the idiom still computes its answer,
  but each losing create stays parked for the rest of the run; if the label
  is destroyed once, one of them then installs as an unused generation.
- A label created ``k`` times per lifetime and destroyed once per lifetime
  leaves ``k − 1`` parked creates behind each generation, so the backlog
  grows by ``k − 1`` per cycle; each destroy admits one of them and replays
  the rest, which re-park, so the replay work grows with the backlog and the
  whole run's cost is quadratic in the number of cycles.  No order among
  several parked creates of one GUID is promised.

In a defined program at most one create of a GUID is pending at any time:
the next generation's create may be issued before the destroy only once the
previous generation is known to be installed (see `Known weaknesses`_), so a
create parks only behind an installed generation, never behind another
parked create.

Data blocks: the block and a rank's cache
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A create makes the **block**: the object at its home, which every operation
on the label is ordered against.  A rank's **cache** of a block is a
different thing — the landing and coherence state that rank keeps for it,
made the first time the rank touches the block, with or without a create.

A create issued on a rank other than the block's home installs its
descriptor on its own rank first and announces the block to the home when it
installs there; the home then installs the block by the same rule (empty:
install; occupied: the announce waits for the occupant's destroy).  A
dependence dispatched on that rank before the create leaves a cache there
that the home has not answered — the home cannot answer before the announce
— and the create claims that cache as its own: it is the block's first
image, not an occupant.  A cache the home *has* answered belongs to an
earlier generation whose teardown notice has not reached the rank yet; the
create parks behind it and installs when the notice retires it.  While a
create on a rank is deciding or parked, an acquire there that finds the slot
empty waits for the create's install instead of making a cache.  At the
home, an acquire of a block that is not installed waits for the create.
On the arms where a creator's first publish goes back to the home
(write-through, and FLUSH), the home answers the announce with a credit for
that publish; the announce names the creator's descriptor, the answer echoes
the name, and the creator applies the credit only to the descriptor that
carries it, so an answer meant for an earlier generation never arms a later
one.

Known weaknesses
~~~~~~~~~~~~~~~~

The rule orders exactly one thing: a create behind the destroy of the
generation before it.  No generation number travels on the wire or lives in
a route slot, and a destroy is fire-and-forget — the home tears the block
down and notifies the ranks that cache it, and no message tells anyone when
every rank has done so.  The following are weaknesses of the runtime, not of
the programs that meet them, and they are left open.  The first three would
need an acknowledged destroy to close.  The dispatch-order shape is closed
only by the orderings the program provides, listed at the end: per-peer FIFO
dispatch in the transport (each peer's messages dispatched in arrival order
by one progress thread) would close it for two messages sent by one rank,
never for two
messages the program orders through a third rank.  ARTS provides neither an
acknowledged destroy nor per-peer FIFO dispatch.

**A third rank's stale cache.**  Exposed by a data-block label reused while
a rank other than its creator and its home still caches the previous
generation.  A consumer on that rank whose acquire of generation ``k+1``
runs before the home's teardown notice for generation ``k`` arrives finds
generation ``k``'s cache present and, being a non-create message, runs
against it: it reads generation ``k``.  Ordering the acquire after the
destroy does not by itself exclude it: the destroy and the notice reach that
rank by different paths, and no program can observe the notice's arrival
without an acknowledged destroy.

**A key-addressed teardown notice.**  The notice a home sends to each rank
that caches a block names the GUID, not the generation, and retires whatever
that rank's slot holds when it lands; a late or duplicated notice for
generation ``k`` can retire the rank's cache of generation ``k+1``, and a
waiter on that cache dies with the teardown.  The runtime keeps
every arm's roster exact and tells each rank once (the creator is on every
arm's roster, and a rank that more than one roster entry names is told
once), which leaves the residual of the weakness above: a rank other than the
creator and the home may see the old generation, or lose the new one, when
its use of the next generation is not ordered behind the notice.

**The creator rank's other tasks.**  While a create of generation ``k+1`` is
parked behind generation ``k``'s cache on its own rank, a dependence on the
label that *another* task on that rank acquires resolves by GUID and reaches
generation ``k``: a read sees generation ``k``, and a write request the home
counts for generation ``k+1`` loses its waiter when generation ``k`` is torn
down.  A request whose waiter is lost that way is never answered, so the
task that made it hangs, whether it reads or writes.  The creator's own
operations name its descriptor and are unaffected.

**Dispatch order.**  The home applies a GUID's messages in the order it
dispatches them, which need not be the order the program issued them in.
Two messages to one GUID's home are dispatched in the order they were sent
only when both come from one rank and one progress thread dispatches that
rank's messages (``progress_threads=1``).  Two ranks' messages are unordered
whatever the thread count: a rank that acts on word from a third rank is
ordered after that rank's message to the home only through the home itself
(R destroys, then signals S; S's satisfy can still reach the home first).
Under several progress threads per rank even one rank's messages are
unordered.  The rule orders the create behind the destroy, and nothing else;
for every object kind this leaves one shape:

- A non-create message meant for generation ``k+1`` (a satisfy, a
  dependence, a request) that reaches the home before the destroy of
  generation ``k`` is applied to generation ``k``, because a non-create
  message runs against whatever object is present.

A destroy that reaches the home before *any* create of its label parks on
the empty slot and retires whatever installs next.  With one create in
flight that is the intended outcome: the create installs its generation and
the parked destroy retires it.  Retiring the wrong generation — the parked
destroy taking generation ``k+1`` while generation ``k`` installs later and
stays live — needs two creates of the label in flight at once, which is the
undefined shape the orderings below exclude; no generation travels on the
wire, so the runtime cannot tell the two creates apart.

A program that reuses a label therefore provides these orderings:

- **Every order between two messages to one GUID** that the program relies
  on — create ``k`` before destroy ``k``, create ``k`` before create
  ``k+1``, destroy ``k`` before satisfy ``k+1`` — is carried by a chain of
  messages through the GUID's home, or by issuing both messages from the
  home rank.  Program order alone suffices only when both messages come from
  one rank and that rank runs one progress thread.
- **The next generation's satisfies and dependences** are issued from a
  point ordered after the destroy by such a chain, or from the home rank
  itself — for instance by a consumer at the home that destroys the
  generation it consumed and then signals the next generation's producer.
- **An early create** of generation ``k+1``, issued before generation
  ``k``'s destroy so that it parks, is legal once generation ``k`` is known
  to be installed — a message from its home, or from a task that ran on it,
  has reached the creator — and before generation ``k`` ends.  Two creates
  of one GUID both in flight can be dispatched in either order, and the
  wrong order installs generation ``k+1`` first and parks generation ``k``
  behind it.

These orderings close the dispatch-order shape; they do not close the first
three weaknesses.
