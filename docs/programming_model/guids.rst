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

Reusing a labeled GUID: create is first-wins, and that is a deviation
---------------------------------------------------------------------

A labeled create is **first-wins**: one label names one object for its
lifetime, the create whose install lands is its creator, and a create of
a label that already exists creates nothing — it takes no hold, is
handed no pointer, and returns success.  A create hands back a pointer
only through a hold it took, and a later creator that goes on to use the
label does so through a dependence on the GUID, like any other task.
The native hints carry a ``check`` flag for the standard's CHECK
property; it is accepted and changes nothing, because a replacing
install would leave two directories for one GUID and every message that
finds a block by its label would land on whichever the slot holds now.
What a loser is TOLD differs by object: an event or range creator at the
label's home gets ``NULL_GUID`` back, while a data-block creator — like a
remote creator, whose install is fire-and-forget — is told nothing at
all.

A cross-rank rendezvous on a label — every rank creating the same sticky
event and one of them satisfying it — relies on first-wins: a labeled
event is installed only at the label's home rank, so the racing creators
have one arbiter and nothing to disagree about.

Neither of the OCR standard's two checked variants can be provided as
specified, and the OCR shim therefore treats every labeled create the
same way — **first-wins, and no creator is told**:

``GUID_PROP_CHECK``
    the standard reports ``OCR_EGUIDEXISTS`` to a creator whose label is
    taken.  That report presupposes "already exists" is a fact the
    runtime can state at the moment the creator asks.  Here it is not: a
    remote creator cannot be told at all, and a destroy and a create of
    one label are ordered only by the order they happen to land at the
    label's home, so a report would reach some creators, miss others, and
    sometimes name a generation the program had already retired.  The
    shim accepts the property, installs first-wins, and returns success to
    every creator; a creator that goes on to use the label uses the
    winner's object through a dependence on it, which is what every
    rendezvous in the roster expects (each of them already treated
    ``OCR_EGUIDEXISTS`` as the normal outcome).  A later creator's
    ``*addr`` is NULL, because it took no hold to write through — the
    same answer the native create gives.

``GUID_PROP_BLOCK``
    the standard waits until the label can be re-created — a reuse of the
    label across a lifetime boundary.  The runtime has no per-slot
    generation, so that wait cannot be given a meaning; the shim accepts
    the property and installs first-wins like the others.  A program that
    relied on receiving a *fresh* object under an occupied label receives
    the existing one instead.

Because no creator is told, the two properties change nothing through
the shim; a bare labeled create is first-wins there as it is natively.
What a label reused across a lifetime boundary does is unsupported and
engine-dependent: a create whose install lands
before the destroy of the previous generation is dropped as a loser and
every later operation on the label parks on a slot no install will fill;
one that lands after it replaces nothing and the late destroy tears down
the wrong generation.  Derive a distinct label per unit of work instead.

Creating the same label from several places, where one create wins and
the others go on to use the winner's object through dependences, works:
the block under that label is the first creator's, and no later create
touches it.

What that does not license is CREATING it from several places at once.
A create acquires its data block by default, and that hold is per-rank
state — a create on a rank that has never touched the label cannot know
the label exists elsewhere, and its home is not asked at create time —
so two such creates each record themselves as holding a block only one
of them can hold.  Hence:

  Creates of one label must be **one create**, with every other user
  reaching the block through a dependence on the GUID — or take no hold
  at all (``ARTS_DB_PROP_NO_ACQUIRE`` / ``DB_PROP_NO_ACQUIRE``), which
  several ranks may do.

A create makes the **block**: the object at its home, which every
operation on the label is ordered against.  A rank's **cache** of a DB is
a different thing — the landing and coherence state that rank keeps for
it, which any rank makes the first time it touches the DB, with or
without a create.  A dependence dispatched before its label's create
therefore leaves a cache on its own rank and a deferred request at the
home, and the create that then runs on that rank is still the block's
create: it records the creator's hold in the cache already there, gives
the block its first image, and announces it like any create, after which
the deferred request is drained against a block that exists.  A create is
always its block's creator and is always handed a pointer; what it never
does is create over a block that exists, which is what a cache whose word
already carries a hold tells it.

What is **undefined** is racing creates of one label.  Two creates of one
label on one rank, and acquiring creates of one label on two ranks, are
outside the contract: the runtime does not arbitrate them, it does not
carry logic or overhead to survive them, and what they leave behind is
not defined — the same standing as everything else the OCR standard
permits and ARTS narrows.  A rank that has never touched a label cannot
know the label exists elsewhere, and its home is not asked at create
time, which is why the cross-rank case is out of contract on every arm
rather than diagnosed on some.  Creates of one label that take NO hold
(``ARTS_DB_PROP_NO_ACQUIRE``) are fine on as many ranks as you like:
there is no hold to disagree about, and the block's home is its holder
from creation.

Whether a given build happens to survive an out-of-contract create is an
implementation detail and not a promise: a debug build may abort at the
transition such a program corrupts — a grant installs only on a rank
that holds nothing, a hand-back is accepted only onto a home that holds
nothing — and a release build may simply compute the wrong answer.

What does **not** work in any arrangement is *reusing* a label across a
lifetime boundary:

.. code-block:: c

   arts_db_destroy(g);                  /* generation A */
   arts_db_create_with_guid(g, ...);    /* generation B, same label */

The destroy and the create are independent messages and ARTS gives no
ordering contract between them, so the destroy can be applied after the
create.  Nothing distinguishes the two generations — the route table
carries no per-slot generation stamp — so the destroy meant for A tears
down B, and any protocol operation still in flight for A is applied to
B.

Programs that need a label per unit of work should therefore derive a
**distinct index per unit** (``ocrGuidFromIndex`` over a range sized to
the work) rather than cycling a small range with an index that wraps.
Every application in the benchmark suite already does this; the pattern
that does not is ``prodcon``, which is marked ``unsupported`` in the
driver's catalog for exactly this reason.

``tests/ocr/ooo_gen_crossgen_drop.c`` drives the reuse pattern and is
deliberately left unregistered: it is the executable statement of this
limitation, ready to run if the semantics are ever implemented.
