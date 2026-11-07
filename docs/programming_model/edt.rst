Event-Driven Tasks (EDT)
========================

An EDT is the fundamental unit of computation in ARTS.  EDTs are
lightweight, non-preemptive tasks that execute once all their
dependencies have been satisfied.

.. contents:: On this page
   :local:
   :depth: 2

Overview
--------

An EDT consists of:

- A **function pointer** — the C function to execute.
- **Static parameters** (``paramv``) — up to ``paramc`` 64-bit values
  baked into the EDT at creation time.
- **Dependencies** (``depv``) — up to ``depc`` slots that receive
  DataBlocks or values at runtime.

When every dependency slot has been signaled, the runtime schedules the
EDT for execution on the target node.

EDT Function Signature
----------------------

.. code-block:: c

   void my_edt(uint32_t paramc, const uint64_t *paramv,
               uint32_t depc, arts_edt_dep_t depv[]);

- ``paramc`` / ``paramv`` — static parameters passed at creation time.
- ``depc`` / ``depv`` — satisfied dependencies.  Each slot contains:

  - ``depv[i].guid`` — the GUID of the dependency (or an encoded value).
  - ``depv[i].ptr``  — pointer to the DataBlock payload (for DB deps).

Creating an EDT
---------------

.. code-block:: c

   arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
   h.rank = target_node;                 /* placement */
   arts_guid_t guid = arts_edt_create(
       my_edt,                /* function pointer      */
       paramc, paramv,        /* static params         */
       depc,                  /* dependency count      */
       &h                     /* hint (NULL = no preference) */
   );

The returned GUID identifies the EDT.  The ``hint`` parameter carries
all optional creation features in :c:type:`arts_edt_hint_t`.  Pass
``NULL`` for no preference: the build's no-hint placement policy
(``ARTS_NOHINT_EDT_PLACEMENT``: round-robin by default, or the creating
node) picks the node, the GUID is auto-allocated, and the EDT inherits
the creator's current group.  Set ``.rank`` to place the EDT on a given
node.  If ``depc > 0``, the EDT will not run until all slots are
signaled.

Signaling Dependencies
----------------------

Wire a DataBlock or value into a dependency slot:

.. code-block:: c

   /* Wire a DataBlock into slot 0 (read-write) */
   arts_add_dependence(db_guid, edt_guid, 0, DB_MODE_RW);

   /* Wire a raw 64-bit value into slot 1 */
   arts_edt_satisfy_slot(edt_guid, 1, 42, DB_MODE_NULL);

   /* Satisfy a slot without data (pure control dependency) */
   arts_add_dependence(NULL_GUID, edt_guid, 2, DB_MODE_NULL);

If ``depc == 0``, the EDT fires immediately after creation.

A slot is one of ``0 .. depc-1``.  A satisfy or dependence naming a slot at or
above ``depc`` is a program error that only the EDT's rank can detect, and the
caller (possibly on another rank) cannot be told, so that rank ends the
process with a fatal error naming the EDT, the slot and ``depc``.

An EDT takes exactly ``depc`` satisfies, control dependences included.  A
satisfy that reaches the EDT, while it is still installed, after all of them
is the same kind of program error: that rank ends the process with a fatal
error naming the EDT and its dependence count.

For a value computed after the consumer is created, connect an event to its
slot with ``DB_MODE_NULL`` and later satisfy that event with the value.
``arts_add_dependence`` interprets its source as an object; direct value
delivery uses ``arts_edt_satisfy_slot`` or ``arts_event_satisfy``.

Creation Hint Fields
--------------------

:c:func:`arts_edt_create` is the single entry point; optional features ride
in :c:type:`arts_edt_hint_t`:

- ``rank`` — home node (``ARTS_HINT_CURRENT_RANK`` = current node,
  ``ARTS_HINT_ANY_RANK`` = the no-hint placement policy, as a ``NULL``
  hint).
- ``guid`` — create at a pre-reserved GUID (``NULL_GUID`` =
  auto-allocate; when set, the GUID's rank overrides ``rank``).  A
  create of a GUID whose EDT is live waits, parked at the home, until
  that EDT completes or is destroyed (see :doc:`guids`).
- ``group`` — the group (a latch event) this EDT is a member of, for
  termination detection; ``NULL_GUID`` inherits the creator's current group
  (see :doc:`task_groups`).
- ``completion_event`` — an event satisfied once when this EDT completes,
  after its DBs are released, carrying the result set via
  :c:func:`arts_edt_set_result` (see :doc:`task_groups`).

See :doc:`/api/public_api` for the full list.

Execution Guarantees
--------------------

- An EDT runs **exactly once** (or not at all if destroyed before firing).
- The runtime does **not** preempt a running EDT.
- A CPU EDT's body **runs to completion on the worker thread that starts
  it**: the runtime never switches its stack or moves it to another thread.
  A call the body makes may wait on that thread (a data-block release waits
  until the home has answered; a shutdown ends the process around the wait);
  none runs another body or leaves the thread. A GPU EDT's releases of its
  dependences, after its kernel, run on the CUDA callback thread.
- No runtime call runs another EDT's body inline: a call that makes an EDT
  ready (a create, a satisfy, an added dependence, a release that serves a
  waiting acquire) only queues it, and bodies run only from a worker's
  scheduler loop. Any other EDT, even one the body creates or makes ready,
  may run on a different worker, so a body must not expect thread-local
  state it sets to carry over to another EDT.
- EDTs are **node-affine**: the function executes on the node specified
  at creation time.
- Static parameters are copied at creation; the runtime owns the copy.
