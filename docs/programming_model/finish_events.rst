Finish Events (Termination Detection)
=====================================

Finish events provide hierarchical termination detection — a mechanism
to know when a dynamic set of tasks has completed.

.. contents:: On this page
   :local:
   :depth: 2

Problem Statement
-----------------

In a task-based runtime, tasks can spawn new tasks recursively.
Determining when *all* transitively spawned work has finished requires
tracking a dynamically growing task set.  ARTS solves this with
**finish events**: ordinary LATCH events created with the finish hint,
whose latch counts the live tasks of a *finish scope*.

Creating a Finish Scope
-----------------------

A finish event is created with :c:func:`arts_event_create` and
``ARTS_EVENT_HINT_FINISH``:

.. code-block:: c

   arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);

The hint forces a simple latch event on the current rank with an
initial count of 1 — the **creator-token** — and marks it
auto-destroy.  The token keeps the scope open while the creating EDT is
still spawning members; it is released when the creating EDT completes,
or handed back early by :c:func:`arts_event_release_creator_token` once
every member is joined.  The event fires when the latch drains to zero:
creator-token released and every member task finished.

Joining EDTs to a Scope
-----------------------

An EDT joins a finish scope through the ``finish_event`` field of
:c:type:`arts_edt_hint_t`:

.. code-block:: c

   arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
   h.finish_event = fe;
   arts_edt_create(worker, 0, NULL, 0, &h);

Joining increments the finish event's latch at EDT creation and
decrements it at EDT completion.

The scope named in ``finish_event`` must be homed on the creating rank:
a finish event created by the creating EDT (the finish hint always
places it on the current rank) or the ambient scope, which is always a
local object.  The join increment is applied on the creating rank before
the new EDT can run, so it is ordered before the member's completion
decrement; an increment sent to a scope on another rank would travel as a
message that nothing orders against that decrement, so naming another
rank's scope is undefined (a Debug build reports it).  A member that runs
on another rank joins the local scope through a proxy on its own rank.

Membership is **inherited**: when the hint's ``finish_event`` is
``NULL_GUID`` (the default), a newly created EDT joins the *ambient*
finish scope of its creating EDT — the scope that EDT itself belongs
to.  Setting ``finish_event`` explicitly attaches the EDT (and, through
inheritance, its entire descendant subtree) to that scope instead.
Inside an EDT, :c:func:`arts_current_finish_event` returns the ambient
scope GUID (or ``NULL_GUID`` if none).

Nested Scopes (Auto-Chain)
--------------------------

Creating a finish event while running under an ambient finish scope
automatically chains the new (inner) scope under the outer one: the
runtime increments the outer latch and wires the inner event to
decrement it on fire.  The outer scope therefore cannot complete until
every nested scope has drained — termination detection is hierarchical
with no extra user code.

Consuming the Result
--------------------

Scope completion is acted on by a continuation: wire a successor EDT
onto the finish event with :c:func:`arts_add_dependence`; it becomes
ready once the scope has drained:

.. code-block:: c

   arts_guid_t s = arts_edt_create(successor, 0, NULL, 1, NULL);
   arts_add_dependence(fe, s, 0, DB_MODE_NULL);

An EDT body never blocks on a scope: work that must follow the scope's
completion goes into the successor, which receives what it needs through
its parameters and dependences.  The finish event is reclaimed when it
fires, so the dependence is added while the creator still holds its
token — before the creating EDT returns, and before any
:c:func:`arts_event_release_creator_token`; a dependence added after the
fire finds no event and is never satisfied.

With a NULL hint, or a hint whose ``finish_event`` is ``NULL_GUID``, the
successor joins its creator's ambient scope.  It must never name the
scope it depends on as its ``finish_event``: that would make it a member
of the scope it waits for, and the scope could never drain.  A NULL hint
also leaves the successor's placement to the no-hint policy
(``ARTS_NOHINT_EDT_PLACEMENT``); set ``rank`` in a hint when it must run
on a given rank, for example when it reads state kept in one process's
memory.

Minimal Example
---------------

A fork-join over ``NUM_TASKS`` workers (after
``tests/ocr/counter_smoke_value.c``):

.. code-block:: c

   void worker(uint32_t paramc, const uint64_t *paramv,
               uint32_t depc, arts_edt_dep_t depv[]) {
       /* ... do work; may spawn children, which inherit the scope ... */
   }

   void done(uint32_t paramc, const uint64_t *paramv,
             uint32_t depc, arts_edt_dep_t depv[]) {
       /* all scope-joined work is done here */
       arts_shutdown();
   }

   void main_edt(uint32_t paramc, const uint64_t *paramv,
                 uint32_t depc, arts_edt_dep_t depv[]) {
       arts_guid_t fe = arts_event_create(&ARTS_EVENT_HINT_FINISH);

       arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
       h.finish_event = fe;
       for (unsigned int i = 0; i < NUM_TASKS; i++)
           arts_edt_create(worker, 0, NULL, 0, &h);

       arts_guid_t d = arts_edt_create(done, 0, NULL, 1, NULL);
       arts_add_dependence(fe, d, 0, DB_MODE_NULL);
   }   /* returning releases the creator-token */
