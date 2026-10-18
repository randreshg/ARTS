Examples
========

The programs under ``examples/`` are native ARTS programs written against the
public ``arts.h`` (and, for the graph programs, ``arts/graph.h``) alone.  Each
has a page here describing what it computes, how it is wired and how it is
placed across ranks.

Directories
-----------

``basics/``
   Small programs that show the EDT and DataBlock model.
``dense/``
   Dense linear algebra on tiled DataBlocks.
``graph/``
   Graph algorithms on the distributed CSR graph of ``arts/graph.h``.
``stream/``
   Memory-bandwidth kernels.
``random_access/``
   Random updates to a table split across ranks.
``proxy/``
   Proxy applications.

``gpu/`` holds GPU programs that are not yet ported to the current API; it is
not built.

Building
--------

``ARTS_BUILD_EXAMPLES`` (default ON) builds every example for the tree's own
coherence cell as ``build/examples/<stem>_example``.  A benchmark tree also
builds ``<stem>_example_arts_<variant>`` for every ARTS variant.

Running
-------

.. code-block:: bash

   ARTS_CONFIG=<cfg> ./build/examples/<stem>_example <args>

The configurations the tests use are under ``configs/local/test/``.  With
``ARTS_BUILD_TESTS=ON`` each example also has a smoke test, registered by the
test suite's rule: ``fib``, whose outcome no coherence cell can change, in the
tree of the default cell only, every other example in every tree:

.. code-block:: bash

   ctest --test-dir build -L examples --output-on-failure

``artsrun`` runs the examples across node counts and variants and votes a
consensus on their results:

.. code-block:: bash

   artsrun run -p <profile> -x examples

Programs
--------

.. toctree::
   :maxdepth: 1
   :caption: basics

   fib
   fib_db
   reduction
   nqueens

.. toctree::
   :maxdepth: 1
   :caption: dense

   tiled_matmul
   cholesky

.. toctree::
   :maxdepth: 1
   :caption: graph

   bfs
   triangle_count
   scan_statistics

.. toctree::
   :maxdepth: 1
   :caption: stream

   stream

.. toctree::
   :maxdepth: 1
   :caption: random_access

   random_access

.. toctree::
   :maxdepth: 1
   :caption: proxy

   lulesh
   smithwaterman
