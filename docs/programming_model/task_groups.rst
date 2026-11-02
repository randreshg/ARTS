Task Groups and Completion Events
=================================

Two independent slots of an EDT's creation hint give a program two ways to
know that work has finished: the **group** an EDT belongs to, which counts a
dynamic set of tasks and everything they create, and the **completion
event**, which an EDT satisfies once, when it completes.

.. contents:: On this page
   :local:
   :depth: 2

Problem Statement
-----------------

In a task-based runtime, tasks create tasks recursively.  Knowing when *all*
transitively created work has finished means counting a dynamically growing
set.  ARTS counts it with a plain latch event, the **group's latch**, and
lets an EDT name the latch it is a member of.  Nothing about the latch is
special: it is an ordinary event (:doc:`events`) that EDTs join.

Group Membership
----------------

An EDT joins a group through the ``group`` field of
:c:type:`arts_edt_hint_t`:

.. code-block:: c

   arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
   h.group = latch;
   arts_edt_create(worker, 0, NULL, 0, &h);

Joining increments the latch when the EDT is created, before the EDT can
run, and decrements it once when the EDT completes (after its data blocks
are released, so a successor the latch wakes sees the EDT's writes) or when
the EDT is destroyed instead of run.

Membership is **inherited**: when ``group`` is ``NULL_GUID`` (the default,
and what a NULL hint gives), a new EDT joins the *current group* of its
creating EDT, the group that EDT itself belongs to.  Setting ``group``
explicitly attaches the EDT, and through inheritance its whole descendant
subtree, to that group instead.  Inside an EDT,
:c:func:`arts_current_group` returns the current group's latch (or
``NULL_GUID`` if the EDT has none; outside any EDT there is none).

The latch named in ``group`` must be an event created on the creating rank
with the default rank, which is installed there when its create returns.
The join increment is applied there before the new EDT can run, so it is
ordered before the member's completion decrement; an increment sent to a
latch on another rank would travel as a message that nothing orders against
that decrement, so naming another rank's latch is undefined (a Debug build
refuses it).  A latch homed on the creating rank but created on another rank
is undefined as well, and is not refused: its install may not have reached
the creating rank, and an increment that waits for it is not ordered before
the decrement either.  A member that runs on another rank joins through a
proxy latch on its own rank, chained to the group's latch, and
:c:func:`arts_current_group` in such a member names that local proxy.

Opening a Group
---------------

A group is *opened* by the EDT that creates it and its first members.  The
latch starts at 1, the opener's own count, which keeps the group open while
the opener is still creating members:

.. code-block:: c

   void main_edt(uint32_t paramc, const uint64_t *paramv,
                 uint32_t depc, arts_edt_dep_t depv[]) {
       /* 1. the latch; its 1 is the opener's own count */
       arts_event_hint_t lh = ARTS_EVENT_HINT_LATCH(1);
       arts_guid_t group = arts_event_create(&lh);

       /* 2. the members, which may create more, inheriting the group */
       arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
       h.group = group;
       for (unsigned int i = 0; i < NUM_TASKS; i++)
           arts_edt_create(worker, 0, NULL, 0, &h);

       /* 3. the successor, wired outside the group */
       arts_guid_t d = arts_edt_create(done, 0, NULL, 1, NULL);
       arts_add_dependence(group, d, 0, DB_MODE_NULL);

       /* 4. the opener releases its count */
       arts_event_satisfy_slot(group, NULL_GUID,
                               ARTS_EVENT_LATCH_DECR_SLOT);
   }

   void done(uint32_t paramc, const uint64_t *paramv,
             uint32_t depc, arts_edt_dep_t depv[]) {
       /* every member and everything it created has completed */
       arts_shutdown();
   }

The latch fires when the count drains to zero: the opener's DECR and every
member's completion.  A group with no member fires at the opener's DECR.  The
DECR is an ordinary satisfy, and nothing more is promised for it: writes
become visible at their release, so a successor that must see the opener's
updates to a block, or destroy the block, finds it released before the DECR
(the release and destroy contract of :doc:`datablocks`, "Release and
Lifetime").

An EDT body never blocks on a group.  Work that must follow the group goes
into the successor, which receives what it needs through its parameters and
dependences.

The order of the four steps is the correct usage, and the reasons are these:

- **Every registration on the latch comes before the opener's DECR, from the
  latch's home rank.**  The successor's dependence (step 3) and a nested
  chain (below) are applied before the latch can fire.  A registration sent
  from another rank is not ordered against the fire, and the transport
  keeps no per-sender order; one that meets a fired latch finds it as its
  lifetime left it.
- **The opener DECRs only after its last direct create of a member.**  A
  member's own creations are safe at any time, because a member's INCR of a
  new member precedes its own DECR.  A DECR that lets the latch fire before
  the opener's last direct create leaves that create to INCR a latch that
  may be reclaimed already.  A DECR that is missing leaves the group open
  forever.
- **The successor is created outside the group.**  It is created by the
  opener, which is outside, or it names another group.  A successor created
  by a member would inherit the group and wait for itself.

How long the latch lives (lingering after its fire, ``arts_event_destroy``,
``auto_destroy``, a declared consumer count) is the program's choice, as for
any event (see :doc:`events`); the group model does not prescribe one.

Independent Groups and Nesting
------------------------------

Groups are independent counters.  An EDT has one group, and naming a group
for it leaves its creator's group; nothing links the two latches unless the
program does.  Opening a group inside another therefore does not make the
outer group wait for the inner one.

When the outer group must wait for the inner group's members too, the opener
says so with two calls, both before its own DECR of the inner latch:

.. code-block:: c

   arts_guid_t outer = arts_current_group();   /* the opener's group */

   arts_event_hint_t lh = ARTS_EVENT_HINT_LATCH(1);
   arts_guid_t inner = arts_event_create(&lh);

   /* the outer group waits for the inner one */
   if (outer != NULL_GUID) {
     arts_event_satisfy_slot(outer, NULL_GUID, ARTS_EVENT_LATCH_INCR_SLOT);
     arts_add_dependence(inner, outer, ARTS_EVENT_LATCH_DECR_SLOT,
                         DB_MODE_NULL);
   }

   /* ... create the inner group's members with .group = inner ... */

   arts_event_satisfy_slot(inner, NULL_GUID, ARTS_EVENT_LATCH_DECR_SLOT);

The INCR runs on the opener's rank, where its current group (or that group's
local proxy, for a member running on another rank) lives, so it is ordered
before the inner latch can fire, and the two calls are the same on every
rank.  With no current group (``main_edt``) there is nothing to chain.

The Completion Event
--------------------

The ``completion_event`` field of :c:type:`arts_edt_hint_t` names an event
that the runtime DECRs once, when that one EDT completes, after its data
blocks have been released.  The DECR carries the EDT's *result*: the GUID the
body registered with :c:func:`arts_edt_set_result` (``NULL_GUID`` if it
registered none).

.. code-block:: c

   void producer(uint32_t paramc, const uint64_t *paramv,
                 uint32_t depc, arts_edt_dep_t depv[]) {
       arts_guid_t answer = /* a data block this body created */;
       arts_edt_set_result(answer);
   }

   void consumer(uint32_t paramc, const uint64_t *paramv,
                 uint32_t depc, arts_edt_dep_t depv[]) {
       /* depv[0] holds the producer's result */
   }

   void main_edt(uint32_t paramc, const uint64_t *paramv,
                 uint32_t depc, arts_edt_dep_t depv[]) {
       arts_guid_t ce = arts_event_create(NULL);       /* latch(1) */
       arts_edt_hint_t h = ARTS_EDT_HINT_DEFAULTS;
       h.completion_event = ce;
       arts_edt_create(producer, 0, NULL, 0, &h);

       arts_guid_t c = arts_edt_create(consumer, 0, NULL, 1, NULL);
       arts_add_dependence(ce, c, 0, DB_MODE_RO);
   }

The event's latch count is the number of completions it waits for: one
latch(N) event named by N EDTs fires after the N-th completion.  An EDT
destroyed instead of run does not satisfy its completion event.  A completion
event is never inherited, and it is unrelated to the EDT's group: it fires at
that one EDT's own completion, while the EDT's group may still be open.
"Everything under this EDT is done" is observed on a group's latch, never on
a completion event.

The two slots are independent: an EDT may set either, both or neither.
Inheritance and the proxy belong to the group slot alone.  A group needs its
INCR applied on the creating rank before the member's DECR, so a member
running elsewhere reaches it through a local proxy, while a completion event
receives a single DECR at completion and is satisfied at its home by GUID,
like any satisfy.
