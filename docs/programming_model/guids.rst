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
- **rank** (14 bits): Node that owns the object (up to 16 382 nodes).
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
       arts_guid_reserve_range(ARTS_GUID_EVENT, 100, target_node);

   for (unsigned int i = 0; i < 100; i++) {
       arts_guid_t g = arts_guid_from_index(start, i);
       /* use g ... */
   }

Round-Robin Allocation
~~~~~~~~~~~~~~~~~~~~~~

To distribute the homes of a range evenly across nodes, pass the
``ARTS_HINT_ROUND_ROBIN`` sentinel rank (``home = idx % nrank``); every
rank that derives members needs the range GUID, which one EDT passes on
(see `Names shared across ranks`_):

.. code-block:: c

   arts_guid_t start =
       arts_guid_reserve_range(ARTS_GUID_EVENT, total_count,
                               ARTS_HINT_ROUND_ROBIN);

.. _names-shared-across-ranks:

Names shared across ranks
~~~~~~~~~~~~~~~~~~~~~~~~~

A program names a data block only after its create: the name is what
:c:func:`arts_db_create` returns.  The task that produces a block creates
it, on the rank where it runs, fills it and releases it, and then hands its
name on along an edge the program already has:

- as a **parameter** of a task it creates;
- as a **slot** of a task that already exists, which it satisfies with
  :c:func:`arts_edt_satisfy_slot`;
- as the value of an **event** it satisfies, when the consumers are created
  independently of the producer and know the block only by an index.

The last form uses a range of events.  A reserved GUID or range is private
to the EDT that reserved it; ranks share a name only by passing it on:

- **Reserve once.** One EDT calls
  ``arts_guid_reserve_range(ARTS_GUID_EVENT, n, ARTS_HINT_ROUND_ROBIN)``
  (member ``i`` is homed on rank ``i % N``) or the same call with one
  explicit home rank.
- **Pass the range GUID** in parameters or a data block.
- **Derive anywhere.** Any EDT on any rank derives
  ``arts_guid_from_index(range, i)`` and gets the same GUID.
- **Create the event before its satisfy.** The producer of block ``i``
  creates event ``i`` with ``hint.guid`` set to that member; every other EDT
  names the event through the same derivation.
- **Satisfy after the release.** The producer satisfies event ``i`` with the
  block's name after its last write and its release.  Consumers depend on
  event ``i`` and find the block's name in their slot.
- **Do not reserve twice.** Separate reserves give separate ranges, so two
  EDTs that each reserve "the same" range name different objects.

A task that only destroys a block gets its name the same two ways without
acquiring it: a ``DB_MODE_NULL`` dependence on event ``i``, or a direct
:c:func:`arts_edt_satisfy_slot` from a producer that knows the name.  It
destroys the block after the block's last reader, and the event after the
last dependence on it was served.

The OCR shim's ``ocrGuidRangeCreate`` / ``ocrGuidFromIndex`` follow the same
model for a range of events or of EDTs.

``NULL_GUID``
-------------

The sentinel ``NULL_GUID`` (``0x0``) represents an absent or invalid
GUID.  Always check against it before dereferencing:

.. code-block:: c

   if (guid != NULL_GUID) {
       /* safe to use */
   }

.. _labeled-guid-reuse:

Labeled GUIDs: one rule for EDT and event creates
-------------------------------------------------

An EDT's or an event's GUID can be named before its object exists: an index
into a reserved range (:c:func:`arts_guid_reserve_range` +
:c:func:`arts_guid_from_index`, or ``ocrGuidFromIndex`` through the OCR shim)
passed as ``hint.guid`` to :c:func:`arts_edt_create` or
:c:func:`arts_event_create`.  A data block takes no such name: its GUID is
issued by :c:func:`arts_db_create` itself.  Every create of a labeled GUID —
EDT or event, whatever property it carries — follows one rule at the rank
whose route slot the object occupies:

- **The slot is empty:** the create installs its object, then replays the
  messages that reached the GUID before it (a satisfy, a dependence, a
  destroy).
- **The slot holds a live object:** the create waits, parked on the slot.
  Nothing is replaced, dropped or reported.  When the occupant's life ends —
  its destroy, or, for an EDT, its completion — the parked create installs
  as the GUID's **next generation**.

Every other message keeps its own rule: it runs against the object the slot
holds, and while the slot is empty it waits for the next install.  A create
parked behind an occupant that never ends stays parked until the run ends,
where the teardown walk of a tree built with ``ARTS_DEBUG_TEARDOWN`` counts it
among the work left undone (the ``ooo`` contributor), so the shutdown is
reported non-quiescent.

What a create returns does not depend on whether it waited:
:c:func:`arts_event_create` and :c:func:`arts_edt_create` return the GUID.

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

An event's label can be destroyed and created again:

.. code-block:: c

   arts_event_destroy(g);               /* generation k   */
   hint.guid = g;
   arts_event_create(&hint);            /* generation k+1 */

The destroy and the create need no ordering between them: a create that
reaches the slot before the destroy waits for it.  An EDT's completion ends
its life the same way — the runtime retires the EDT's GUID before its
completion event or its group is signalled, so a task gated on either can
create the GUID's next generation.  The create is the only message the
runtime holds back for a destroy (see `Reuse and generations`_).

Concurrent creates are undefined
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Two creates of one GUID with no end of life between them are **undefined**:
exactly one installs, and every other one parks for a destroy the program
may never issue.  The runtime does not arbitrate them, report them, or carry
logic to survive them.  A parked EDT create holds open the group its
creator joined it to; a parked event create keeps its payload
until a destroy of the occupant admits it, or to the end of the run if none
comes.  A defined program has at most one create of a GUID pending at any
time.

Data blocks: the block and a rank's cache
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A create makes the **block**: the object at its home, which every operation
on the block is ordered against.  Its GUID is issued by that create, carries
an upper bound on the block's size, and names the block for the whole run.  A
rank's **cache** of a block is a different thing — the landing and coherence
state that rank keeps for it, made the first time the rank touches the block
and never by a create.

The create installs the creator's descriptor on its own rank before it
returns (a create under ``ARTS_DB_PROP_NO_ACQUIRE`` on another rank makes
none), and a creator on a rank other than the block's home announces the
block to the home once that descriptor is installed; the home installs the
block from the announce.  Nothing can name the GUID before the create
returns it, so the creator's slot is empty when the create installs, and a
rank's first touch of the block comes after it.  A request, a release leg, a
destroy or an acquire on the home's own rank that reaches the home before the
announce — the name travelled to another rank by an edge faster than the
announce — waits on the slot's out-of-order list until the block is installed.
On the arms where a creator's first publish goes back to the home
({VAL,INV}·HOME, and FLUSH), the home answers the announce with a credit for
that publish.

Reuse and generations
~~~~~~~~~~~~~~~~~~~~~

ARTS's own model gives every object a fresh GUID and never reuses one.
Create, destroy, dependence, satisfy and acquire are fire-and-forget, and a
release (an explicit one, the one a destroy runs on its caller's own hold, or
the EDT's end), a downgrade and a publish are the only points where a
coherence protocol may wait, so no task's time includes a round trip it does
not need; a message that arrives before its object exists waits on the home's
out-of-order queue until the object is installed.  Labeled GUIDs, and
destroying and creating one GUID again, are there to run OCR programs that use
them; they exist for events and EDTs only, since a data block's GUID comes
from its create and names one block.

A GUID carries no generation.  Telling one generation of a label from the
next would need every message that names the GUID — satisfies, dependences,
EDT parameters, coherence messages — to carry the generation as well, the
piggybacked state the coherence protocols are designed without, or a create
or destroy that waits for a reply.  Neither is provided, so in a program
that reuses a label a message meant for one generation can be applied to
another: a satisfy or dependence registration that reaches the slot while the
previous generation is still installed (before the event's destroy or the
EDT's completion) is applied to it.  A program written to ARTS's model,
with a fresh GUID for every object, never meets this.  Where an OCR program
reuses a label, or creates one label from two tasks (undefined, above), the
fix belongs in the program: one creator per label, and a fresh label for
each lifetime.
