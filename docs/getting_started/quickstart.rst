Quickstart
==========

This tutorial walks through a minimal ARTS program step by step.

.. contents:: On this page
   :local:
   :depth: 2

Program Structure
-----------------

Every ARTS program consists of three parts:

1. **EDT functions** — the async work units.
2. ``main_edt()`` — the program's one entry, which every program defines.
   The runtime creates it on rank 0 once rank 0's threads are up, with
   ``paramv[0] = argc`` and ``paramv[1] = argv`` (rank 0's process
   arguments, a pointer valid on rank 0 only).  Everything else, on every
   rank, starts from EDTs it creates.
3. ``main()`` — calls :c:func:`arts_rt` to start the runtime.  A program
   that calls ``arts_rt`` without defining ``main_edt`` does not link.

.. code-block:: c

   #include "arts.h"

   /* 1.  EDT function ---------------------------------------------------- */
   void my_task(uint32_t paramc, const uint64_t *paramv,
                uint32_t depc, arts_edt_dep_t depv[]) {
       arts_printf("Hello from node %u, worker %u!\n",
                   arts_get_current_rank(), arts_get_current_worker());
       arts_shutdown();
   }

   /* 2.  Entry-point EDT ------------------------------------------------- */
   void main_edt(uint32_t paramc, const uint64_t *paramv,
                      uint32_t depc, arts_edt_dep_t depv[]) {
       (void)depc;
       (void)depv;
       /* paramv[0] = argc, paramv[1] = argv */
       arts_edt_create(my_task, 0, NULL, 0, NULL);
   }

   /* 3.  Entry point ----------------------------------------------------- */
   int main(int argc, char **argv) {
       arts_rt(argc, argv);   /* blocks until arts_shutdown() */
       return 0;
   }

Three idioms follow from the one entry.

**Arguments reach EDTs as parameters.**  ``main_edt`` parses scalars and
passes them as single parameter words.  Strings travel as a packed argument
block: :c:func:`arts_argv_paramc`, :c:func:`arts_argv_to_params` and, in the
receiving EDT, :c:func:`arts_argv_from_params`.  Parameters are copied with
the EDT, so the block reads the same on every rank.

.. code-block:: c

   /* in main_edt */
   int argc = (int)paramv[0];
   const char *const *argv = (const char *const *)paramv[1];
   uint32_t n = arts_argv_paramc(argc, argv);
   if (n == 0) { /* the block would not fit in a uint32_t */
       arts_shutdown();
       return;
   }
   uint64_t *words = malloc(n * sizeof(uint64_t));
   arts_argv_to_params(argc, argv, words);
   arts_edt_create(worker, n, words, 0,
                   &(arts_edt_hint_t){.rank = arts_get_total_ranks() - 1});
   free(words);

   /* in worker */
   const char *args[16];
   int count = arts_argv_from_params(paramv, paramc, args, 16);
   if (count < 0) { /* malformed, or more than 15 arguments */
       return;
   }

**Work on every rank is one EDT per rank.**  ``main_edt`` creates one EDT
per rank as members of a finish scope, and wires the scope to a continuation
on rank 0.  A per-rank EDT may leave state in its rank's memory for later
EDTs on that rank; each such word has one writer, that rank's EDT, and the
scope and message delivery order every later reader after it.  What other
ranks need travels as parameters or data blocks.

.. code-block:: c

   arts_guid_t done = arts_event_create(&ARTS_EVENT_HINT_FINISH);
   for (unsigned int r = 0; r < arts_get_total_ranks(); r++) {
       arts_edt_create(init_rank, n, words, 0,
                       &(arts_edt_hint_t){.rank = r, .finish_event = done});
   }
   arts_guid_t cont = arts_edt_create(after_init, 0, NULL, 1,
                                      &(arts_edt_hint_t){.rank = 0});
   arts_add_dependence(done, cont, 0, DB_MODE_NULL);

**Names shared across ranks come from a range that one EDT reserves.**
``main_edt`` (or any one EDT) reserves a range and passes the range GUID on;
every EDT on every rank derives the same member GUIDs from it (see
:ref:`names-shared-across-ranks`).

.. code-block:: c

   /* in main_edt: one block per rank, member i homed on rank i % N */
   arts_guid_t range = arts_guid_reserve_range(ARTS_GUID_DB,
                                               arts_get_total_ranks(),
                                               ARTS_HINT_ROUND_ROBIN);
   uint64_t p[1] = {range};
   /* ... pass p to the per-rank EDTs ... */

   /* in the EDT on rank r: create its own member; any EDT derives the rest */
   arts_guid_t mine = arts_guid_from_index((arts_guid_t)paramv[0],
                                           arts_get_current_rank());
   void *ptr = arts_db_create_with_guid(mine, size, ARTS_DB,
                                        ARTS_DB_PROP_NONE, NULL);

Configuration File
------------------

ARTS looks for ``arts.cfg`` in the current directory (or the path in
the ``ARTS_CONFIG`` environment variable).  A minimal configuration:

.. code-block:: ini

   [ARTS]
   worker_threads=4
   launcher=local

The annotated reference listing every recognized key is
``configs/example.cfg`` (documentation only — copy the entries you need
into your ``arts.cfg``). Ready-to-run localhost shapes live in
``configs/local/test/``:

.. code-block:: bash

   cp <arts-root>/configs/local/test/1n_20w.cfg ./arts.cfg

Building and Running
--------------------

Link your program against ``arts_shared`` (or ``arts_static``):

.. code-block:: bash

   gcc -std=c17 -o hello hello.c -larts_shared -lpthread

Run with the config file in the same directory:

.. code-block:: bash

   ./hello

Multi-Node Execution
~~~~~~~~~~~~~~~~~~~~

For SSH-based multi-node runs, list nodes in ``arts.cfg``:

.. code-block:: ini

   launcher=ssh
   node_count=4
   nodes=node1,node2,node3,node4
   ports=25000

Then launch normally — ARTS SSHs into each node automatically. ``ports`` is
required here: every rank resolves its peers' ports from its own copy of the
config, so nothing but the config can supply them. Pick a base below the
kernel ephemeral port range (32768–60999); a base inside it can randomly
collide with outgoing connections' source ports.

A ``launcher=local`` run — several ranks on one machine, as the tests and the
benchmark harness use — names no ports at all: the ranks share a machine and so
cannot share a port, and the spawning rank finds a free block for all of them.
Setting ``ports`` there is a configure error.

On a SLURM cluster the launcher is detected from the scheduler's
environment — ``SLURM_PROCID``/``SLURM_NNODES`` override whatever
``launcher`` the config names — and the geometry follows the allocation:
node count from ``SLURM_NNODES``, the node list from the step, and the
per-rank thread count from ``SLURM_CPUS_PER_TASK``.  Only ``ports`` must
still come from the config, for the same reason as with ssh:

.. code-block:: ini

   ports=25000

.. code-block:: bash

   srun --ntasks-per-node=1 -N 4 -c 16 ./hello

One rank per node: the runtime refuses a step that starts more tasks than
the allocation has nodes.

Next Steps
----------

- :doc:`/programming_model/edt` — learn about EDT creation and signaling.
- :doc:`/programming_model/datablocks` — understand the DataBlock memory model.
- :doc:`/examples/index` — the example programs; the Fibonacci walkthrough
  (``examples/basics/fib.c``, built as ``build/examples/fib_example`` under
  ``ARTS_BUILD_EXAMPLES``) is :doc:`/examples/fib`.
