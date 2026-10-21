Installation
============

This page covers building ARTS from source.

.. contents:: On this page
   :local:
   :depth: 2

Prerequisites
-------------

- **C17 compiler**: GCC >= 7 or Clang >= 5 (other compilers are not supported)
- **CMake** >= 3.22
- **Ninja** build system (Make is not supported)
- **POSIX threads** (pthreads)

- **hwloc** (bundled as a git submodule; built from source automatically)

Optional:

- **CUDA toolkit** for GPU support

Obtaining the Source
--------------------

.. code-block:: bash

   git clone <repository-url> arts
   cd arts

Building
--------

ARTS *requires* the Ninja generator. Make is not supported.

.. code-block:: bash

   mkdir build && cd build
   cmake -GNinja .. -DCMAKE_BUILD_TYPE=Release
   ninja
   ninja install   # installs to CMAKE_INSTALL_PREFIX (default: project/install)

Debug build, here with AddressSanitizer (every debug facility is an
``ARTS_DEBUG_*`` option, OFF by default; a Release build carries none):

.. code-block:: bash

   cmake -GNinja .. -DCMAKE_BUILD_TYPE=Debug -DARTS_DEBUG_SANITIZER=ADDRESS

CMake Options
~~~~~~~~~~~~~

All options are set with ``-D<NAME>=<VALUE>`` on the cmake line.

.. list-table::
   :header-rows: 1
   :widths: 34 14 52

   * - Option
     - Default
     - Description
   * - ``ARTS_BUILD_SHARED``
     - ON
     - Build the shared library ``libarts.so`` alongside ``libarts.a``.  The
       static library is unconditional — it is the substrate the benchmark
       variants, the OCR shim and the installed archive derive from.
   * - ``ARTS_BUILD_TESTS``
     - OFF
     - Build the test programs (registers them with ctest).  Off by
       default: tests statically link the runtime (hermetic), so a full
       test tree costs real disk and link time — turn it on for
       validation builds.
   * - ``ARTS_BUILD_BENCHMARKS``
     - ON
     - Build the benchmark tree: a runtime variant ``arts_static_<suffix>``
       per coherence cell and, for each section below that is on, its
       programs on every variant and on the reference runtimes it runs on.
       On with every section off is a configure error.  The benchmark tree
       is x86-64 only: on another target the configure skips it, with
       ``ARTS_BUILD_HPX``, and says so.
   * - ``ARTS_BUILD_OCR_APPS``
     - ON
     - Section: the ocr-apps corpus and the in-tree OCR programs
       (``<app>_arts_<suffix>``, ``_xsocr``, ``_ocrvx``, ``_hinted``).  Takes
       effect only in a benchmark tree.
   * - ``ARTS_BUILD_HPX_APPS``
     - ON
     - Section: the HPX originals (``<row>_hpx`` on HPX) and their OCR
       mirrors (``<row>_arts_<suffix>``, ``_xsocr``, ``_ocrvx``); builds
       FFTW.  Takes effect only in a benchmark tree.
   * - ``ARTS_BUILD_EXAMPLES``
     - ON
     - Section: the native ARTS examples under ``examples/``.  Every tree
       builds them for its own cell (``<stem>_example``, in
       ``build/examples``) and smoke-tests them with ``ARTS_BUILD_TESTS``; a
       benchmark tree also builds ``<stem>_example_arts_<suffix>``.
   * - ``ARTS_BUILD_SHIM``
     - ON
     - Build the OCR-to-ARTS compatibility shim against this tree's runtime
       and the vendored OCR headers.  With ``ARTS_BUILD_TESTS`` its tests
       are registered with ctest in every configuration.  Turning it off in
       a benchmark tree with ``ARTS_BUILD_OCR_APPS`` or
       ``ARTS_BUILD_HPX_APPS`` on is a configure error, since every ARTS
       variant of an OCR-API program links the shim.
   * - ``ARTS_BUILD_XSOCR``
     - ON
     - Build the XSOCR reference runtime and the ``*_xsocr`` variants of the
       OCR-API programs (needs a host MPI).
   * - ``ARTS_BUILD_OCRVX``
     - ON
     - Build the ocr-vx reference runtime and the ``*_ocrvx`` variants of
       the OCR-API programs (needs a host MPI).
   * - ``ARTS_BUILD_HPX``
     - ON
     - Source-build HPX v1.11.0 with its MPI parcelport for the HPX
       originals of ``ARTS_BUILD_HPX_APPS`` (needs a host MPI; x86-64 only,
       like the rest of the benchmark tree).
   * - ``ARTS_BUILD_DOCS``
     - OFF
     - Build the Doxygen + Sphinx documentation.
   * - ``ARTS_USE_GPU``
     - OFF
     - Enable CUDA GPU support (requires the CUDA toolkit).
   * - ``ARTS_USE_LOCAL_CUDA_ARCHITECTURES``
     - ON
     - Auto-detect the local GPU's CUDA architecture via ``nvidia-smi`` (only
       when ``ARTS_USE_GPU=ON``; otherwise set ``CMAKE_CUDA_ARCHITECTURES``).
   * - ``ARTS_COHERENCE_PROTOCOL``
     - INV
     - Coherence protocol, which also fixes the memory model the program
       must satisfy — ``INV`` (default; invalidation: directory
       write-invalidate with acknowledged per-release invalidation rounds),
       ``VAL`` (validation: versioned snapshots, readers never
       blocked/invalidated), or ``EXCL`` (exclusion: per-DB distributed
       reader-writer lock), all under the OCR model (races legal, the
       runtime orders every conflict it must); or ``FLUSH`` (fetch the
       whole payload at every remote acquire, write it back at every remote
       RW release) under the DB-WRF model (exclusive write acquisition: the
       program guarantees that at most one write-mode acquisition of a
       DataBlock is live at any time, system-wide; evaluation only), with
       no write- or release-policy axis of its own.
       Valid combos: {VAL,INV}×WT×{PURGE,RETAIN}, {VAL,INV}×WB×RETAIN,
       EXCL×WB×{PURGE,RETAIN}, FLUSH.  Compile-time; all ranks must share
       one build.
   * - ``ARTS_WRITE_POLICY``
     - WB
     - Write policy at release granularity, live in INV/VAL — ``WT``
       (write-through at release; the home holds a current copy and serves
       reads) or ``WB`` (write-back; the payload stays with the last writer
       and moves on demand).
   * - ``ARTS_RELEASE_POLICY``
     - RETAIN
     - Release policy, live in EXCL and in WT × {VAL, INV} — ``PURGE``
       (copy and permission return to the home when the last local user
       finishes) or ``RETAIN`` (keep both until another node asks; the home
       recalls on demand). ``WB`` requires ``RETAIN``.
   * - ``ARTS_MALLOC``
     - mimalloc
     - General allocator — ``mimalloc`` (default; vendored static, the
       only multinode-capable choice) or ``system`` (libc malloc;
       single-node / sanitizer builds only).
   * - ``ARTS_USE_CXL``
     - OFF
     - This tree's own library keeps every data block's payload in CXL
       memory.  Requires ``ARTS_COHERENCE_PROTOCOL=EXCL``,
       ``ARTS_RELEASE_POLICY=PURGE``, ``ARTS_CXL_RESIDENCY`` and an x86-64
       target (the store orders the device with x86 flush and fence
       instructions); anything else is a configure error.
   * - ``ARTS_CXL_RESIDENCY``
     - (empty)
     - ``STAGED`` (a rank-local copy staged from the block's slot at the
       two ownership edges) or ``DIRECT`` (the slot itself, with the
       edges reduced to a flush each).  Required with
       ``ARTS_USE_CXL=ON``, refused otherwise.
   * - ``ARTS_CXL_REAL``
     - OFF
     - OFF: the vendored fake library (``third_party/fake_arts_cxl_lib``,
       built at configure time).  ON: the device library named by the four
       paths below (all required and must exist).  A path given with
       ``ARTS_CXL_REAL=OFF`` is a configure error.
   * - ``ARTS_CXL_RAPID_INCLUDE_DIR``
     - (empty)
     - ``ARTS_CXL_REAL=ON``: the directory holding the device SDK's
       ``MemOps.h`` and ``SharedAlloc.h``.
   * - ``ARTS_CXL_RAPID_LIB_DIR``
     - (empty)
     - ``ARTS_CXL_REAL=ON``: the directory holding the device SDK's
       libraries (``librapid.so``), linked with ``-lrapid`` and an rpath.
   * - ``ARTS_CXL_LIB_INCLUDE_DIR``
     - (empty)
     - ``ARTS_CXL_REAL=ON``: the directory holding the glue library's
       installed headers.
   * - ``ARTS_CXL_LIB``
     - (empty)
     - ``ARTS_CXL_REAL=ON``: the device glue library file
       (``libarts_cxl_lib.so``), linked by path — no CMake package
       lookup.
   * - ``ARTS_CXL_DB_ARENA_SIZE_BYTES``
     - 5000000000
     - Bytes per arena the runtime creates from the CXL library (one
       data-block arena per device).  Validation trees pass 268435456.
   * - ``ARTS_NOHINT_EDT_PLACEMENT``
     - ROUNDROBIN
     - Where an EDT created with no placement preference (NULL hint, or
       hint rank ``ARTS_HINT_ANY_RANK``) lands — ``ROUNDROBIN`` (default)
       or ``CREATOR``.  The OCR shim's ``ocrEdtCreate`` with no affinity
       hint follows it.
   * - ``ARTS_NOHINT_DB_HOME``
     - CREATOR
     - Where a DB created with no placement preference (NULL hint) is
       homed — ``CREATOR`` (default; first-touch, home stays on the
       creating rank) or ``ROUNDROBIN``.  An explicit hint rank or
       pre-reserved GUID always wins.  The OCR shim's ``ocrDbCreate`` with
       no affinity hint follows it.
   * - ``ARTS_LOG_LEVEL``
     - 3 / 1
     - Log verbosity (3 in Debug, 1 in Release; empty = the build type's):
       0=ERROR … 3=DEBUG.  A Release build accepts 0 or 1.
   * - ``ARTS_DEBUG_SANITIZER``
     - OFF
     - Debug builds only: ``ADDRESS`` (ASan + UBSan + LSan) or ``THREAD``
       (TSan + UBSan), C/C++ only, never CUDA.  One choice, because the
       compiler cannot combine the two.  Under ``ADDRESS`` the test suite
       runs with one LeakSanitizer suppression,
       ``tests/lsan_suppressions.txt`` (a leak inside the vendored
       libfabric's tcp provider); the work a non-quiescent shutdown
       abandons stays reachable at exit and is not reported.  Under
       ``THREAD`` the audited tests listed in
       ``tests/tsan/residue_tests.cmake`` run, in the cells and variants
       each entry names, with one of two suppression files
       (``tests/tsan/residue_send.supp``, ``residue_send_land.supp``),
       chosen by the race shapes the test's program leaves unordered in
       that cell; they name only the fabric provider's payload copy
       routines, and every other test keeps those frames visible.
   * - ``ARTS_DEBUG_INJECTION``
     - OFF
     - Debug builds only: compile the run-time injection hooks, each inert
       until its environment variable is set — ``ARTS_SCHED_FUZZ=<pct>``
       (short random delays at the lock-free decision points),
       ``ARTS_WRITE_COMPLETION_DELAY_MS=<n>`` (hold each one-sided write's
       local completion for a random 0..n ms),
       ``ARTS_NET_MAX_MSG=<bytes>`` (lower the provider's one-sided ceiling),
       ``ARTS_REGPOOL_FORCE_FULL_NODES=<n,...>`` (treat NUMA nodes as full),
       ``ARTS_REQUEST_DELAY_MS=<n>[:ro|:hop]`` (hold each coherence request
       a rank sends another rank for a random 0..n ms; ``:ro`` holds only
       read requests and ``:hop`` only a home's redirect or forward of a
       request it took in, each exactly n ms).
       The tests that need a hook register only where this is on.
   * - ``ARTS_DEBUG_DB_QUARANTINE``
     - OFF
     - Debug builds only: fill every freed coherent data-block descriptor and
       hold it back for the next 1024 frees made by the same thread,
       reporting a write through a reference that outlived it when the
       descriptor leaves the quarantine and at teardown, and refusing a second
       free; under ``ARTS_DEBUG_SANITIZER=ADDRESS`` the held memory is
       poisoned as well.
   * - ``ARTS_DEBUG_LANDING_BALANCE``
     - OFF
     - Debug builds only: count each rank's rendezvous landings drawn and not
       yet ended (returned, adopted or installed), one atomic update each.
       A reply the shutdown cuts legally leaves its landing out, so the
       count is no shutdown verdict: the tests that destroy blocks and then
       wait for every home to be torn down also wait for every rank's count
       to reach zero, and the teardown walk prints a non-zero count as
       information.
   * - ``ARTS_COUNTER_CONFIG``
     - configs/counters_off.cfg
     - Counter configuration file parsed into introspection macros. The
       default is the all-OFF file; ``configs/counters.cfg`` is the
       profiling example, not the default.

To pick a faster linker, use CMake's own ``-DCMAKE_LINKER_TYPE=MOLD`` (cmake ≥ 3.29).

See :ref:`coherence_protocols` for the normative definition of the protocols and their contracts.

GPU Build
~~~~~~~~~

.. code-block:: bash

   cmake -GNinja .. -DCMAKE_BUILD_TYPE=Release -DARTS_USE_GPU=ON

Verifying the Build
-------------------

Configure a test tree and run the single-node test suite as a smoke check.
``-DARTS_BUILD_BENCHMARKS=OFF`` keeps the tree from also building the
benchmark tree, and ``-DARTS_BUILD_EXAMPLES=ON``, given explicitly, also
turns the examples on in a tree whose cache still holds an earlier OFF:

.. code-block:: bash

   cmake -GNinja .. -DARTS_BUILD_TESTS=ON -DARTS_BUILD_BENCHMARKS=OFF -DARTS_BUILD_EXAMPLES=ON && ninja
   ctest --test-dir . -L single_node --output-on-failure
   ctest --test-dir . -L examples --output-on-failure

Every test should report ``Passed``.
