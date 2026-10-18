Table of Contents
=================

*   [Project Overview](#project-overview)
*   [Installation Guide](#installation-guide)
    *   [Environment Requirements](#environment-requirements)
    *   [Dependencies](#dependencies)
    *   [Building](#building)
    *   [Build Options](#build-options)
    *   [Memory Consistency Models](#memory-consistency-models)
*   [Running Tests](#running-tests)
*   [Configuration](#configuration)
*   [Contributors](#contributors)

Project Overview
================

**Project Name:** Abstract RunTime System (ARTS)

**Principal Investigator:** Joshua Suetterlein (Joshua.Suetterlein@pnnl.gov)

**General Area or Topic of Investigation:** Asynchronous Many-Task runtime (AMT)

**Release Number:** 2.0.0

ARTS is an asynchronous, distributed, event-driven task runtime based on OCR
concepts. Programs express work as **EDTs** (Event-Driven Tasks), data as
**DataBlocks** (DBs), and wiring as **Events** — all identified by globally
unique **GUIDs**. The runtime builds dynamic DAGs and schedules EDTs as their
dependencies are satisfied, providing a distributed global address space and a
distributed memory model with efficient synchronization for massively parallel
systems.

Installation Guide
==================

Environment Requirements
------------------------

**Languages:** C17 (runtime core), C++17, and optionally CUDA (GPU support).

**Operating System:** Linux on x86-64 or AArch64 (any ARMv8-A host). The
runtime relies on a double-width compare-and-swap: `cmpxchg16b` on x86-64,
where it is built with `-mcx16`. On AArch64 no `-march` is forced: where the
compiler outlines atomics by default (recent GCC and Clang), the
read-modify-writes are outlined and pick LSE or LL/SC from the host; an older
compiler emits inline LL/SC loops instead, which run on every ARMv8-A host,
with or without LSE. The 16-byte compare-and-swap is libatomic's under GCC
and, under Clang with outlining, libgcc's `__aarch64_cas16_acq_rel`, each
choosing `casp` or an LL/SC loop.
The runtime is linked against `libatomic`, which GCC calls for every 16-byte
atomic. The benchmark tree is x86-64 only: on another target the configure
skips it.

**Compiler:** GCC >= 7 or Clang >= 5 (strict `-std=c17`, no GNU extensions).
Other compilers are not supported.

**Required Memory / Disk:** ~2 GB RAM and ~500 MB disk for a full build.

Dependencies
------------

| Name | Version | Required | Notes |
| ---- | ------- | -------- | ----- |
| CMake | >= 3.22 | Required | The build is CMake-driven. |
| Ninja | any | Required | ARTS enforces the Ninja generator; Make is **not** supported. |
| pthreads / librt / libm | system | Required | Provided by glibc on Linux. |
| libatomic | system | Required | For 16-byte atomics (DWCAS). |
| hwloc | pinned in `third_party/CMakeLists.txt` | Bundled | Built from the official release tarball — no system install, no autotools needed. |
| libfabric | pinned in `third_party/CMakeLists.txt` | Bundled | Built from the official release tarball — no system install, no autotools needed. |
| rdma-core | pinned submodule (`third_party/rdma-core`) | Bundled | Verbs (RDMA) userspace stack, built statically with CMake; makes the libfabric verbs provider available on every build. |
| python3, make | system | Required | python3 runs rdma-core's build scripts; make drives the bundled autotools packages. No pkg-config, flex or bison is needed: the bundled stack neither consults the host's packages nor generates a scanner. |
| CUDA Toolkit | >= 11 (tested) | Optional | Only when `ARTS_USE_GPU=ON`. |
| MPI | any | Optional | Only needed by the reference runtimes (XSOCR, ocr-vx, HPX) of a benchmark tree (`ARTS_BUILD_BENCHMARKS=ON`) with the OCR or HPX applications section on. |

A plain `git clone` is enough: the first cmake run initializes every
required submodule automatically (per enabled option — e.g. the OCR
applications and OpenBLAS only when a benchmark tree has
`ARTS_BUILD_OCR_APPS` on, HPX only with `ARTS_BUILD_HPX_APPS` and
`ARTS_BUILD_HPX`). Initializing them up front is equivalent:

```bash
git clone <repository-url> arts
cd arts
git submodule update --init --recursive   # optional
```

The bundled hwloc and libfabric are **not** submodules: the first `cmake`
run downloads their official release tarballs (URL + SHA256 pinned in
`third_party/CMakeLists.txt`) into `third_party/` and builds from them.
On a host without network access, download the two tarballs elsewhere and
place them at `third_party/hwloc-<version>.tar.bz2` and
`third_party/libfabric-<version>.tar.bz2` before running cmake — the build
verifies the same pinned SHA256 either way. rdma-core *is* a submodule
(auto-initialized like the others) and builds with plain CMake — no
autotools, no system RDMA packages.

RDMA support is always compiled in: the bundled rdma-core backs libfabric's
verbs provider on every build, and a node without RDMA devices simply runs
on the tcp provider. Set `provider=` in `arts.cfg` to pin the choice
explicitly (e.g. `provider=verbs` on an InfiniBand cluster).

Building
--------

ARTS *requires* the Ninja generator. The default install prefix is
`<project>/install`.

```bash
# Configure (Debug is the default build type)
cmake -GNinja -Bbuild -DCMAKE_BUILD_TYPE=Release

# Build
ninja -C build

# Install (to CMAKE_INSTALL_PREFIX, default: <project>/install)
ninja -C build install
```

The public headers (`arts.h`, `arts/graph.h`, `arts/gpu.h`)
and the `libarts` static/shared libraries are installed under the prefix, along
with a CMake package config so downstream projects can `find_package(ARTS)`.
A program linking ARTS is built non-PIE, because task function pointers cross
ranks as addresses: the installed `ARTS::static` and `ARTS::shared` targets
add `-fno-pie` and `-no-pie` to the executables that link them (a shared
library or module that links ARTS keeps its `-fPIC`).  Such an executable must
not combine `POSITION_INDEPENDENT_CODE ON` with `check_pie_supported()`: CMake
then appends `-fPIE -pie` after those options and builds the program PIE again.
For the same reason a task's function must live in the executable, since a
shared library loads at a different address on each rank.

Build Options
-------------

All options are set on the cmake line with `-D<NAME>=<VALUE>`. The full list
with defaults lives in [README.md](README.md#build-options); the most common are:

| Option | Default | Purpose |
| ------ | ------- | ------- |
| `CMAKE_BUILD_TYPE` | `Debug` | `Debug` or `Release`. |
| `ARTS_COHERENCE_PROTOCOL` | `INV` | Coherence protocol — who keeps reader copies valid — which also fixes the memory model the program must satisfy: `INV` (default; release-time invalidation rounds), `VAL` (acquire-time version validation, readers never blocked/tracked/invalidated), or `EXCL` (per-DB distributed reader-writer lock), all under the OCR model (races legal, the runtime orders every conflict it must); or `FLUSH` (fetch the whole payload at every remote acquire, write it back at every remote RW release, block for the home's ACK) under the DB-WRF model (exclusive write acquisition — the program guarantees that at most one write-mode acquisition of a DataBlock is live at any time, system-wide; evaluation only), with no write- or release-policy axis. Valid combos: {VAL,INV}×WT×{PURGE,RETAIN}, {VAL,INV}×WB×RETAIN, EXCL×WB×{PURGE,RETAIN}, FLUSH. Compile-time; all ranks must share one build. |
| `ARTS_WRITE_POLICY` | `WB` | Write policy at release granularity — `WT` (write-through: payload flushed to the block's home at every release; home serves reads) or `WB` (default; write-back: payload stays with the last writer, directory forwards on demand). Live in INV/VAL; EXCL requires WB. |
| `ARTS_RELEASE_POLICY` | `RETAIN` | What a node does with a grant when its last local user finishes — `PURGE` (hand it back to the home, with the payload when it is an EXCL write grant) or `RETAIN` (default; keep it until another node asks). Live in EXCL (read and write grants) and in WT × {VAL, INV} (the write grant); WB requires RETAIN. |
| `ARTS_USE_GPU` | `OFF` | Enable CUDA GPU support. |
| `ARTS_BUILD_TESTS` | `OFF` | Build the ctest suite. |
| `ARTS_BUILD_BENCHMARKS` | `ON` | Build the benchmark tree: a runtime variant per coherence cell and the programs of each section that is on (the reference runtimes need a host MPI; the ARTS variants do not). ON with every section OFF is a configure error. Skipped, with a configure message, on a target other than x86-64. |
| `ARTS_BUILD_OCR_APPS` | `ON` | Section: the OCR applications, on every ARTS variant and reference OCR runtime (benchmark tree only). |
| `ARTS_BUILD_HPX_APPS` | `ON` | Section: the HPX originals on HPX and their OCR mirrors (benchmark tree only). |
| `ARTS_BUILD_EXAMPLES` | `ON` | Section: the native ARTS examples (`examples/{basics,dense,graph,stream,random_access,proxy}`; `gpu/` is not ported and not built), built in every tree for its own cell and smoke-tested with `ARTS_BUILD_TESTS`; a benchmark tree also builds them on every ARTS variant. |
| `ARTS_DEBUG_SANITIZER` | `OFF` | Debug builds only: `ADDRESS` (ASan + UBSan + LSan) or `THREAD` (TSan + UBSan). |
| `ARTS_DEBUG_INJECTION` | `OFF` | Debug builds only: compile the run-time injection hooks (`ARTS_SCHED_FUZZ`, `ARTS_WRITE_COMPLETION_DELAY_MS`, `ARTS_NET_MAX_MSG`, `ARTS_REGPOOL_FORCE_FULL_NODES`), each inert until its environment variable is set. |
| `ARTS_DEBUG_DB_QUARANTINE` | `OFF` | Debug builds only: hold every freed coherent data-block descriptor back, filled, and report a write through a reference that outlived it. |

A Release build carries no debug facility; each `ARTS_DEBUG_*` option is OFF
by default, accepted only in a Debug build, and combines with the others.

A faster linker is selected with CMake's own `-DCMAKE_LINKER_TYPE=MOLD`
(cmake >= 3.29).

Coherence Protocols
-------------------

DataBlock consistency behavior is controlled by one primary compile-time
knob and two protocol-conditional sub-knobs. `ARTS_COHERENCE_PROTOCOL`
selects the protocol, which determines the memory model the program must
satisfy: `INV` (default), `VAL`, or `EXCL` require the OCR model
(implementing the OCR v1.2.0 §1.6 memory model); `FLUSH` requires the DB-WRF
model (exclusive write acquisition — the program guarantees that at most one
write-mode acquisition of a DataBlock is live at any time, system-wide;
evaluation only, and can make a program outside that contract yield wrong
results).
The sub-knobs depend on the protocol: `ARTS_WRITE_POLICY` (`WT`/`WB`,
default `WB`) selects **where** the canonical payload lives under `VAL`/`INV`
— `WB` (with the last writer) or `WT` (flushed back to the DB home at every
release); `ARTS_RELEASE_POLICY` (`PURGE`/`RETAIN`, default `RETAIN`) selects
whether a node hands a grant back to the home when its last local user
finishes or keeps it until another node asks — under `EXCL` its read and
write grants, under `WT` × {`VAL`, `INV`} the write grant. `FLUSH` has
neither axis: every remote acquire fetches and every remote RW release
writes through, unconditionally. One binary is exactly one configuration,
and every rank in a multinode run must use the same build. Each
configuration is its own build tree, for example:

```bash
cmake -GNinja -Bbuild_ocr_val_wt  -DCMAKE_BUILD_TYPE=Debug -DARTS_COHERENCE_PROTOCOL=VAL -DARTS_WRITE_POLICY=WT -DARTS_BUILD_BENCHMARKS=OFF
cmake -GNinja -Bbuild_ocr_val_wb  -DCMAKE_BUILD_TYPE=Debug -DARTS_COHERENCE_PROTOCOL=VAL -DARTS_WRITE_POLICY=WB -DARTS_BUILD_BENCHMARKS=OFF
cmake -GNinja -Bbuild_wrf_flush   -DCMAKE_BUILD_TYPE=Debug -DARTS_COHERENCE_PROTOCOL=FLUSH -DARTS_BUILD_BENCHMARKS=OFF
ninja -C build_ocr_val_wt && ninja -C build_ocr_val_wb && ninja -C build_wrf_flush
```

Running Tests
=============

CTest is the test runner. Tests are grouped by ctest labels:

```bash
cmake -GNinja -Bbuild -DCMAKE_BUILD_TYPE=Debug -DARTS_BUILD_TESTS=ON \
  -DARTS_BUILD_BENCHMARKS=OFF -DARTS_BUILD_EXAMPLES=ON
ninja -C build
ctest --test-dir build -L single_node --output-on-failure   # single-node
ctest --test-dir build -L multinode --output-on-failure     # 2n_9w_1p/4n_4w_1p/2n_8w_2p localhost
ctest --test-dir build -L examples --output-on-failure      # the examples' smoke tests
ctest --test-dir build -L gpu --output-on-failure           # GPU build only (runs on 1n_20w.cfg with gpu=1)
ctest --test-dir build -R edt_create_basic                  # one test by name
```

A test tree passes `-DARTS_BUILD_BENCHMARKS=OFF` so it does not also build
the benchmark tree, and `-DARTS_BUILD_EXAMPLES=ON` explicitly, which also
turns the examples on in a tree whose cache still holds an earlier OFF.

Each test sets its own `ARTS_CONFIG` to point at the matching cfg under
`configs/local/test/`, so no config files are copied into the build directory.
A test that needs a debug facility registers only in a tree that turns it on
(`-DARTS_DEBUG_INJECTION=ON`, `-DARTS_DEBUG_DB_QUARANTINE=ON`).

Configuration
=============

An ARTS program reads its runtime configuration from `arts.cfg` in the working
directory (or the file named by the `ARTS_CONFIG` environment variable).
Templates live under `configs/`:

- `configs/local/test/{1n_20w,2n_9w_1p,2n_8w_2p,4n_4w_1p}.cfg` — localhost, the ctest shapes (`<nodes>n_<workers>w[_<progress>p]`)
- `configs/templates/{arts,ocr,counters}.cfg.j2` — what `artsrun` renders a campaign's configurations from, per node profile (`experiments/profiles/*.yaml`)

The values most often changed are the launcher, the `worker_threads`/
`progress_threads` counts, and the GPU count.

Contributors
============

### Main Team Members

1. Joshua Suetterlein, joshua.suetterlein@pnnl.gov
2. Joseph Manzano, joseph.manzano@pnnl.gov
3. Andres Marquez, andres.marquez@pnnl.gov

### Contributors

1. Vinay Amatya
2. Kiran Ranganath
3. Marcin Zalewski
4. Jesun Firoz
5. Vito Castellana
6. Marco Minutoli
7. Antonino Tumeo
8. John Feo
9. Andrew Lumsdaine
