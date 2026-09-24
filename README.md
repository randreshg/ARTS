# ARTS Runtime Overview

ARTS (Asynchronous Runtime System) is a distributed, event-driven runtime loosely based on the ideas pioneered in the **Open Community Runtime (OCR)**: work is expressed as Event-Driven Tasks (EDTs), data lives in datablocks identified by GUIDs, and dependencies are tracked through events instead of global synchronization. The runtime implements the OCR v1.2.0 §1.6 memory model: each DataBlock has a canonical owner, remote updates flow via ownership transfer, and the runtime wires dependency graphs dynamically.

## What ARTS Provides

- **Event-Driven Tasks (EDTs)** – Lightweight units of work scheduled when their input dependencies are satisfied.
- **Datablocks (DBs)** – Explicit data objects with globally unique identifiers (GUIDs). They carry ownership/rendezvous information so ARTS can ship or replicate data across nodes and enforce the OCR consistency contract.
- **Events & Dependencies** – OCR-style events connect producers/consumers. The runtime builds a dynamic DAG and triggers EDTs once all prereqs fire.
- **GUID system** – Every EDT, datablock, and event has a GUID so DAGs can be wired across nodes without global pointers.
- **Datablock lifecycle** – Applications allocate datablocks via `arts_db_create`, pass GUIDs to EDTs, and the runtime handles acquire/release semantics (read/write modes, owner hand-offs). Reference counts and versioning live in `libs/src/core/db.c` and `libs/src/core/coherence/`.
- **Distributed Scheduling** – A decentralized scheduler assigns EDTs to worker threads, maintains per-thread deques, supports work stealing, and cooperates with the transport layer (`libs/src/core/transport/`) to migrate work or data.
- **Networked DB protocol** – Cross-rank DB acquire/release/publish and control messages ride a libfabric (OFI) RDM-endpoint transport (`libs/src/core/transport/net.c`); the provider is chosen via the config file's `provider` key (or the `FI_PROVIDER` env var), defaulting to `tcp`. DataBlock payloads are staged in a per-NUMA registered memory pool ("regpool") and delivered by one-sided RDMA into the receiver's pre-registered buffer; small control messages ride ordinary two-sided sends. Dedicated worker threads run EDTs while separate progress thread(s) drive completion. libfabric is vendored as a submodule and built in-tree, so no external networking library is required. A TCP socket mesh still exists, but only for process launch, the one-shot startup address exchange, and post-bootstrap liveness detection — no DB payload or protocol traffic crosses it.

## Relationship to OCR

ARTS borrows heavily from OCR concepts:
- EDTs ↔ OCR tasks
- Datablocks ↔ OCR datablocks
- Events ↔ OCR events/slots
- GUIDs ↔ OCR GUIDs

However ARTS is purpose-built for this repository and trimmed to match its compiler/tooling integration: lean APIs in `libs/include/public/` and a GUID allocator tailored to cartesian DAGs.

## Dependencies

See `INSTALL.md` for detailed package lists. At a high level you need:
- A C/C++ compiler (GCC or Clang)
- CMake + Ninja — the build enforces the Ninja generator; Make is not supported
- pthreads (system, via glibc); `libhwloc` and `libnuma` are vendored dist tarballs built in-tree, not system packages
- The libfabric (OFI) transport is a vendored submodule, built in-tree — no external networking library to install
- Optional: MPI, only for the xsocr/ocr-vx/baseline reference benchmark backends (not needed for ARTS itself)

## Building

Follow `INSTALL.md` for prerequisites. Typical steps:

```bash
cmake -GNinja -Bbuild -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

### Build Options

All options are set on the cmake line with `-D<NAME>=<VALUE>`, e.g.
`cmake -GNinja -Bbuild -DCMAKE_BUILD_TYPE=Release -DARTS_USE_GPU=ON`.

| Option | Default | Purpose |
|--------|---------|---------|
| `ARTS_BUILD_SHARED` | `ON` | Build the shared library `libarts.so` alongside `libarts.a`. The static library is unconditional — it is the substrate the benchmark variants, the OCR shim and the installed archive derive from. |
| `ARTS_BUILD_EXAMPLES` | `OFF` (forced) | **Deprecated** — `examples/` predates the current public API and does not build; enabling is a configure error until the examples are modernized. See `tests/ocr/` and `benchmarks/` for working usage. |
| `ARTS_BUILD_TESTS` | `OFF` | Build the test programs and register them with ctest. Off by default: tests statically link the runtime (hermetic), so a full test tree costs real disk/link time — turn it on for validation builds. |
| `ARTS_BUILD_BENCHMARKS` | `ON` | Build the benchmark apps (ARTS and the enabled reference runtimes). |
| `ARTS_BUILD_XSOCR` | `ON` | Build the XSOCR reference runtime and its `*_xsocr` app variants (needs a host MPI). |
| `ARTS_BUILD_OCRVX` | `ON` | Build the ocr-vx reference runtime and its `*_ocrvx` app variants (needs a host MPI). |
| `ARTS_BUILD_HPX` | `ON` | Source-build HPX v1.11.0 with its MPI parcelport and build the standalone HPX benchmark apps (needs a host MPI). |
| `ARTS_BUILD_BASELINES` | `OFF` | Build the native (non-OCR) OpenMP/MPI baseline implementations — outside the experiment driver's catalog, explicit opt-in. |
| `ARTS_BUILD_DOCS` | `OFF` | Build the Doxygen + Sphinx documentation. |
| `ARTS_USE_GPU` | `OFF` | Enable CUDA GPU support (builds `libarts_cuda`). |
| `ARTS_USE_LOCAL_CUDA_ARCHITECTURES` | `ON` | When GPU is on, auto-detect the local GPU's CUDA architecture via `nvidia-smi`. Only meaningful with `ARTS_USE_GPU=ON`; pair with the stock `CMAKE_CUDA_ARCHITECTURES` (e.g. `-DCMAKE_CUDA_ARCHITECTURES="80;86"`) to set SM targets by hand. |
| `ARTS_MEMORY_MODEL` | `OCR` (derived) | Memory model — **derived** from `ARTS_COHERENCE_PROTOCOL`, not chosen independently (passing it explicitly only asserts the value its protocol already implies; a mismatch is a configure error): `OCR` (races legal, the runtime orders every conflict it must; required by `VAL`/`INV`/`EXCL`) or `DB_WRF` (prose DB-WRF; exclusive write acquisition — the program guarantees that at most one write-mode acquisition of a DataBlock is live at any time, system-wide; required by `FLUSH`; evaluation only). Compile-time; all ranks must share one build. |
| `ARTS_COHERENCE_PROTOCOL` | `VAL` | Coherence protocol — who keeps reader copies valid: `VAL` (default; acquire-time version validation, readers never blocked/tracked/invalidated), `INV` (release-time invalidation rounds), or `EXCL` (per-DB distributed reader-writer lock), all under the `OCR` model; or `FLUSH` (fetch the whole payload at every remote acquire, write it back at every remote RW release, block for the home's ACK) under the `DB_WRF` model, with no write- or release-policy axis. Valid combos: OCR×{VAL,INV}×WT×{PURGE,RETAIN}, OCR×{VAL,INV}×WB×RETAIN, OCR×EXCL×WB×{PURGE,RETAIN}, DB_WRF×FLUSH. |
| `ARTS_WRITE_POLICY` | `WB` | Write policy at release granularity — `WT` (write-through: payload flushed to the block's home at every release; home serves reads) or `WB` (default; write-back: payload stays with the last writer, directory forwards on demand). Live in INV/VAL; EXCL requires WB. |
| `ARTS_RELEASE_POLICY` | `RETAIN` | What a node does with its write grant when the last local user finishes — `PURGE` (hand copy and permission back to the home) or `RETAIN` (default; keep both until another node asks). Live in EXCL and in WT × {VAL, INV}; WB requires RETAIN. |
| `ARTS_DEFAULT_DB_KIND` | `ARTS_DB` | Default DB storage kind that the `ARTS_DB_DEFAULT` macro expands to — `ARTS_DB` (regular DRAM). |
| `ARTS_USE_CXL` | `OFF` (DEPRECATED) | Enable the CXL DataBlock storage kind — forced OFF: enabling it is a configure error. The kind predates the current coherence design and is unmaintained; the sources stay for reference. For fabric-attached memory, configure `ARTS_FAM_BACKEND` / `ARTS_FAM_RESIDENCY` below instead. |
| `ARTS_FAM_BACKEND` | `OFF` | Fabric-attached-memory backend: `OFF` (default), `SHM` or `DEVICE`, one device-library API spoken by one adapter. `SHM` is the vendored fake library (`third_party/fake_arts_cxl_lib`, built into the build tree at configure time: one host's shared memory at one fixed address — see below); `DEVICE` links the device library named by `ARTS_FAM_DEVICE_INCLUDE_DIR` and `ARTS_FAM_DEVICE_LIBRARY` (both required, both must exist). No `AUTO`; every mismatch is a configure error, and the old `ARTS_FAM_DEVICE_VENDORED` / `ARTS_USE_FAKE_CXL_LIB` are errors naming `SHM`. |
| `ARTS_FAM_RESIDENCY` | (empty) | Where an EDT's working bytes live when this tree's own library is FAM-enabled: a rank-local copy staged from the block's slot at the two ownership edges (`STAGED`), or the slot itself, with the edges reduced to a flush each (`DIRECT`). |
| `ARTS_LOG_LEVEL` | `3` (Debug) / `1` (Release) | Log verbosity: `0`=ERROR, `1`=+WARN, `2`=+INFO, `3`=+DEBUG. |
| `ARTS_USE_SANS` | `OFF` | Enable ASan + UBSan + LSan in Debug builds (excludes CUDA). Mutually exclusive with `ARTS_USE_TSAN`. |
| `ARTS_USE_TSAN` | `OFF` | Enable ThreadSanitizer in Debug builds (excludes CUDA). Compiler-incompatible with `ARTS_USE_SANS`; use a separate build dir. |
| `ARTS_COUNTER_CONFIG` | `configs/counters_off.cfg` | Counter configuration file parsed at configure time into introspection macros. The default is the all-OFF file; `configs/counters.cfg` is the profiling example, not the default. |

Standard CMake variables also apply: `CMAKE_BUILD_TYPE` (`Debug` default, or `Release`),
`CMAKE_INSTALL_PREFIX` (`./install` default), `CMAKE_CUDA_ARCHITECTURES` (see `ARTS_USE_LOCAL_CUDA_ARCHITECTURES`),
and `CMAKE_LINKER_TYPE` (cmake ≥ 3.29; e.g. `-DCMAKE_LINKER_TYPE=MOLD` to pick a faster linker like mold/lld/gold).

## Running

An ARTS program reads its runtime configuration from `arts.cfg` in the
working directory; the `ARTS_CONFIG` environment variable overrides the path:

```bash
ARTS_CONFIG=configs/local/test/1n.cfg ./my_program
```

Ready-to-run localhost shapes (single-node and 2/3/4-rank multinode) live in
`configs/local/test/`, and `configs/example.cfg` is the annotated reference
listing every recognized key. A `launcher=local` configuration spawns all
ranks on this machine and claims its own ports; the remote launchers (`ssh`,
and `slurm`/`lsf`, which are auto-detected from the scheduler's environment)
must name `ports` in the configuration. The full walk-through — writing and
linking a program, multi-node configurations, the complete key reference —
is in the Sphinx guides: `docs/getting_started/quickstart.rst` and
`docs/configuration/arts_cfg.rst`.

### The SHM backend: fabric-attached memory on one machine

`third_party/fake_arts_cxl_lib` is the vendored fake library: it implements
the device library's API over one POSIX shared-memory region mapped at the
device's fixed address, so the fabric-attached arms can be built and tested
on one machine. `ARTS_FAM_BACKEND=SHM` is that library; CMake fetches the
submodule, builds it into the build tree, and links it:

```bash
cmake -GNinja -Bbuild -DARTS_BUILD_TESTS=ON \
  -DARTS_COHERENCE_PROTOCOL=EXCL -DARTS_RELEASE_POLICY=PURGE \
  -DARTS_FAM_BACKEND=SHM -DARTS_FAM_RESIDENCY=STAGED
ninja -C build
ctest --test-dir build -L multinode
```

A tree on another cell builds the fabric-attached benchmark variants against
the same library (its own library is not FAM-enabled, so it names no
residency). Every rank started by the local launcher attaches to the same
region (`/dev/shm/arts_fake_cxl`); the strict oracle (`fam_strict`) is on by
default in such a tree.

- `ARTS_FAKE_CXL_REGION_SIZE` (bytes, default 32 GiB) sizes the region when
  the library loads, before the runtime reads its cfg. The runtime takes one
  arena of `fam_pool_mb` plus one page from it, so the region must exceed
  the configured pool; it is allocated sparsely and only touched pages count
  against `/dev/shm`. The ctest registration of an SHM tree sets 2 GiB for
  every test; `artsrun` sets the profile's `fam_pool_mb` plus 64 MiB for
  every FAM cell; a manual run sets it itself.
- `ARTS_FLUSH_LOG` names where each process writes its binary flush trace at
  exit (default `./arts_flush_trace.bin`, in the working directory). The
  ctest registration of a FAM tree sets it to `/dev/null` for every test;
  `artsrun` sets it to `<cell>.flush.bin` beside a one-rank FAM cell's log,
  and to `/dev/null` for a cell with more than one rank: the library takes
  one path for all ranks, so every rank on a host would overwrite the same
  file.
- The region has one name and one address per host, so a host runs one FAM
  run at a time. The library does not refuse a second concurrent run — it
  attaches to the live region — so keeping runs apart is the runner's job
  (ctest's `RESOURCE_LOCK` within one invocation, one runner per host
  beyond it).
- It covers one host only: a run of more than one rank is refused at load
  unless `launcher=local`. See `third_party/fake_arts_cxl_lib/README.md`.

Benchmarks and experiment campaigns are driven by `artsrun` rather than run
by hand — see `tools/artsrun/README.md`.

## Repository Layout (selected paths)

- `libs/include/` – Public and internal API headers
- `libs/src/` – Runtime sources: task scheduler, GUID tables, datablock manager, network transports, logging
- `cmake/` – Build helpers
- `configs/` – Runtime configuration files (`local/`, `mpi/`, `twosisters/` subdirs)
- `examples/` – Small standalone programs showing how to create EDTs/datablocks. **Deprecated**: predates the current public API and does not build; `ARTS_BUILD_EXAMPLES` is forced off, kept for historical reference only
- `benchmarks/` – Runtime microbenchmarks and OCR benchmark applications (XSOCR + ARTS + ocrvx backends)
- `docs/` – Sphinx/Doxygen documentation sources (see Documentation below)

## Documentation

API reference and guides are built with Doxygen + Sphinx.

### Prerequisites

```bash
# Doxygen (if not already installed)
sudo apt install doxygen

# Python dependencies
pip install -r docs/requirements.txt
```

### Build

```bash
cmake -GNinja -Bbuild -DARTS_BUILD_DOCS=ON
ninja -C build docs
```

HTML output goes to `build/docs/sphinx/`.

### View locally

```bash
cd build/docs/sphinx/
python3 -m http.server 8000
```

Then open <http://localhost:8000> in a browser.

## Learn More

- `INSTALL.md` – Build instructions and optional components
- `FULL_LICENSE.md` / `LICENSE.md` – Licensing information
- Example programs under `examples/` for hands-on API references (**deprecated**: predates the current public API and does not build; kept for historical reference)
