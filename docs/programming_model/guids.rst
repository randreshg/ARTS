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
  create installs.  The creator's hold is on that image, and the creator's
  own operations name it.  A release that reaches the block's home as a
  message, one the home rank sends itself included, is looked up by GUID
  there and so meets whatever the slot holds meanwhile (see `Reuse and
  generations`_).

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

A label can be destroyed and created again:

.. code-block:: c

   arts_db_destroy(g);                  /* generation k   */
   arts_db_create_with_guid(g, ...);    /* generation k+1 */

The destroy and the create need no ordering between them: a create that
reaches the slot before the destroy waits for it (on a creating rank other
than the home, a dependence's cache of the GUID that kept no right to the
block is claimed instead, see `Data blocks: the block and a rank's cache`_).  An
EDT's completion ends its life the same way — the runtime retires the EDT's
GUID before its output event or its finish scope is signalled, so a task
gated on either can create the GUID's next generation.  The create is the
only message the runtime holds back for a destroy (see `Reuse and
generations`_).

Concurrent creates are undefined
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Two creates of one GUID with no end of life between them are **undefined**:
exactly one installs, and every other one parks for a destroy the program
may never issue.  The runtime does not arbitrate them, report them, or carry
logic to survive them.  A parked EDT create holds open the finish scope its
creator joined it to; a parked event or data-block create keeps its payload
until a destroy of the occupant admits it, or to shutdown if none comes.  A
defined program has at most one create of a GUID pending at any time.

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
dependence dispatched on that rank before the create leaves a cache there,
and the create claims that cache as its own: it is the block's first image,
not an occupant.  A dependence's cache an earlier generation of the label
left on the rank, whose teardown notice has not reached it yet, is claimed
the same way when it kept no right to the block: the two are not told apart
(see `Reuse and generations`_).  A create parks on that rank behind the
descriptor of an earlier create made there, or behind a cache that kept a
right to the block (the write right; under ``EXCL`` × ``RETAIN`` also the
block's resident bytes, which outlast a read phase that took the write
right, or a retained read grant), which refuses the create's hold, and
installs when that block's teardown notice retires it.  While a create on a
rank is deciding or parked, an acquire there that finds the slot empty waits
for the create's install instead of making a cache.  At the home, an acquire
of a block that is not installed waits for the create.  On the arms where a
creator's first publish goes back to the home (write-through, and FLUSH),
the home answers the announce with a credit for that publish.

A labeled data-block GUID is reserved before its block exists, so it
carries no size hint, and a rank's first fetch of the block from its home
asks for the size first.

Reuse and generations
~~~~~~~~~~~~~~~~~~~~~

ARTS's own model gives every object a fresh GUID and never reuses one.
Create, destroy, dependence, satisfy and acquire are fire-and-forget, and a
release is the only point where a coherence protocol may wait, so no task's
time includes a round trip it does not need; a message that arrives before
its object exists waits on the home's out-of-order queue until the object is
installed.  Labeled GUIDs, and destroying and creating one GUID again, are
there to run OCR programs that use them.

A GUID carries no generation.  Telling one generation of a label from the
next would need every message that names the GUID — satisfies, dependences,
EDT parameters, coherence messages — to carry the generation as well, the
piggybacked state the coherence protocols are designed without, or a create
or destroy that waits for a reply.  Neither is provided, so in a program
that reuses a label a message meant for one generation can be applied to
another: a satisfy, dependence, acquire or release that reaches the home
before the previous generation's destroy, a teardown notice that reaches a
rank caching the block after that rank's use of the next generation, or,
on the write-through and FLUSH arms, the home's answer to a create that
reaches the next generation's creator, whose first publish then writes into
the previous block's released buffer.  A program written to ARTS's model,
with a fresh GUID for every object, never meets this.  Where an OCR program
reuses a label, or creates one label from two tasks (undefined, above), the
fix belongs in the program: one creator per label, and a fresh label for
each lifetime.
