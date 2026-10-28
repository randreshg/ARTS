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
| `ARTS_USE_CXL` | `OFF` | This tree's own library keeps every data block's payload in CXL memory. Requires `ARTS_COHERENCE_PROTOCOL=EXCL` (either `ARTS_RELEASE_POLICY`) and `ARTS_CXL_RESIDENCY`; anything else is a configure error. |
| `ARTS_CXL_RESIDENCY` | (empty) | `STAGED` (a rank-local copy staged from the block's slot at the two ownership edges) or `DIRECT` (the slot itself, with the edges reduced to a flush each). Required with `ARTS_USE_CXL=ON`, refused otherwise. |
| `ARTS_CXL_REAL` | `OFF` | OFF: the vendored fake library (`third_party/fake_arts_cxl_lib`, built at configure time — see below). ON: the device library named by `ARTS_CXL_RAPID_INCLUDE_DIR` and `ARTS_CXL_LIB` below (both required and must exist). A path given with `ARTS_CXL_REAL=OFF` is a configure error. |
| `ARTS_CXL_RAPID_INCLUDE_DIR` | (empty) | `ARTS_CXL_REAL=ON`: the directory holding the device SDK's `MemOps.h` and `SharedAlloc.h`. |
| `ARTS_CXL_LIB` | (empty) | `ARTS_CXL_REAL=ON`: the device glue library file (`libarts_cxl_lib.so`), linked by path — no CMake package lookup. |
| `ARTS_CXL_DB_ARENA_SIZE_BYTES` | `5000000000` | Bytes per CXL arena: one data-block arena per device count (the EDT deque's arena only under `ARTS_CXL_EDT_DEQUE`). Arena creation ignores the device ids it is handed, so allocation selects an arena index, not a device. Validation trees pass `268435456`. |
| `ARTS_LOG_LEVEL` | `3` (Debug) / `1` (Release) | Log verbosity: `0`=ERROR, `1`=+WARN, `2`=+INFO, `3`=+DEBUG. |
| `ARTS_USE_SANS` | `OFF` | Enable ASan + UBSan + LSan in Debug builds (excludes CUDA). Mutually exclusive with `ARTS_USE_TSAN`. |
| `ARTS_USE_TSAN` | `OFF` | Enable ThreadSanitizer in Debug builds (excludes CUDA). Compiler-incompatible with `ARTS_USE_SANS`; use a separate build dir. |
| `ARTS_COUNTER_CONFIG` | `configs/counters_off.cfg` | Counter configuration file parsed at configure time into introspection macros. The default is the all-OFF file; `configs/counters.cfg` is the profiling example, not the default. |
| `ARTS_USE_PAPI` | `OFF` | Read hardware events (cache/TLB misses, cycles, stalls) through PAPI into the `PAPI_*` counters. See [PAPI hardware counters](#papi-hardware-counters). |
| `ARTS_PAPI_DIR` | `$PAPI_DIR` | `ARTS_USE_PAPI=ON`: the PAPI install prefix holding `include/papi.h` and `lib/libpapi.so` (or `lib64/`). PAPI is never vendored; a prefix without both is a configure error. |

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

### The fake library: CXL memory on one machine

`third_party/fake_arts_cxl_lib` is the vendored fake library: it implements
the device library's API over one POSIX shared-memory region mapped at a
fixed address, so the CXL arms can be built and tested on one machine.
`ARTS_CXL_REAL=OFF` (the default) is that library; CMake fetches the
submodule, builds it into the build tree, and links it:

```bash
cmake -GNinja -Bbuild -DARTS_BUILD_TESTS=ON \
  -DARTS_COHERENCE_PROTOCOL=EXCL -DARTS_RELEASE_POLICY=PURGE \
  -DARTS_USE_CXL=ON -DARTS_CXL_RESIDENCY=STAGED
ninja -C build
ctest --test-dir build -L multinode
```

A tree on another cell builds the two CXL benchmark variants against the
same library (its own library is not CXL-enabled, so it names no
residency). Every rank started by the local launcher attaches to the same
region (`/dev/shm/arts_fake_cxl`).

- `ARTS_FAKE_CXL_REGION_SIZE` (bytes, default 32 GiB) sizes the region when
  the library loads, before the runtime reads its cfg. The runtime takes its
  arenas from it (`ARTS_CXL_DB_ARENA_SIZE_BYTES`, one data-block arena per
  device count, plus the EDT deque's arena only under `ARTS_CXL_EDT_DEQUE`),
  so the region must exceed their total;
  it is allocated sparsely and only touched pages count against `/dev/shm`.
  The ctest registration of a fake-library tree and `artsrun` both set it to
  2 × the tree's `ARTS_CXL_DB_ARENA_SIZE_BYTES` plus 128 MiB (the data-block
  arena, the deque, and headroom) — for every test and every CXL cell,
  respectively; a manual run sets it itself.
- `ARTS_FLUSH_LOG` names where each process writes its binary flush trace at
  exit (default `./arts_flush_trace.bin`, in the working directory). The
  ctest registration of a CXL tree sets it to `/dev/null` for every test;
  a manual run sets it itself.
- The region has one name and one address per host, so a host runs one CXL
  run at a time. The library does not refuse a second concurrent run — it
  attaches to the live region — so keeping runs apart is the runner's job
  (ctest's `RESOURCE_LOCK` within one invocation, one runner per host
  beyond it).
- It covers one host only: a run of more than one rank is refused at load
  unless `launcher=local`. See `third_party/fake_arts_cxl_lib/README.md`.

Benchmarks and experiment campaigns are driven by `artsrun` rather than run
by hand — see `tools/artsrun/README.md`.

## PAPI hardware counters

ARTS can read the CPU's hardware performance counters through
[PAPI](https://icl.utk.edu/papi/) and report them beside its own
introspection counters, so a run can say not only *how many* acquires,
flushes or sends happened but *what they cost* in cycles, instructions and
cache/TLB misses. The intended use is comparing network ARTS against CXL
ARTS on the same application: where the misses land (EDT bodies, the
network send/handler path, the CXL flush edges) is the hardware evidence for
the difference.

### How it works

PAPI values are ordinary introspection counters. They are compiled in by the
same `ARTS_COUNTER_CONFIG` file, kept in the same per-thread storage, reduced
the same way (THREAD/NODE/CLUSTER) and written to the same
`n{rank}.json`/`cluster.json` files, so every existing tool reads them
unchanged. Only reading the hardware is new (`libs/src/core/counter/papi.c`,
`libs/include/internal/arts/counter/papi.h`).

- **Scopes.** Each instrumented span of the runtime is a *scope*. A scope
  counts only while its thread is inside the span, charging the difference
  between `PAPI_read`s at its start and end.

  | Scope | What it spans |
  |-------|---------------|
  | `PAPI_TOTAL_WORKER` | a worker thread's whole run loop |
  | `PAPI_TOTAL_PROGRESS` | a progress thread's whole run loop (all runtime/network work) |
  | `PAPI_EDT` | EDT bodies only (`func()` in `arts_run_edt`) |
  | `PAPI_DB_ACQUIRE` | acquiring an EDT's DataBlocks before its body runs |
  | `PAPI_DB_RELEASE` | releasing an EDT's DataBlocks (and the ones it created) after its body |
  | `PAPI_CXL_FLUSH_PRODUCER` | producer-side CXL ownership-edge flushes (`cxl/store.c`) |
  | `PAPI_CXL_FLUSH_CONSUMER` | consumer-side CXL ownership-edge flushes (`cxl/store.c`) |
  | `PAPI_NET_SEND` | libfabric sends and payload puts (`transport/net.c`) |
  | `PAPI_MSG_HANDLER` | dispatching each inbound message to its handler (`transport/dispatcher.c`, `loopback.c`) |

- **Slots.** Each scope has six counter slots, `PAPI_<SCOPE>_E0` …
  `PAPI_<SCOPE>_E5`; slot `E<k>` counts the k-th event named by the
  `papi_events` key of `arts.cfg`. Event names are chosen **at run time**, so
  trying another event set needs no rebuild. Six covers the two fixed counters
  (cycles, instructions) plus the four programmable ones per hyperthread on
  Skylake/Sapphire Rapids.
- **One EventSet per thread.** Each runtime thread registers with PAPI and
  starts one EventSet when it enters the runtime and stops it when its run
  loop returns; the stop charges the `TOTAL_*` scope matching the thread's
  role. Scope edges are cheap `PAPI_read`s, never start/stop.
- **No multiplexing.** The event set must fit the core's counters at once;
  multiplexed (time-sliced) counts would make per-scope differences
  meaningless.
- **Scopes are inclusive and may overlap.** A consumer-side CXL flush
  triggered by an inbound message is counted in both `CXL_FLUSH_CONSUMER` and
  `MSG_HANDLER`, and `DB_RELEASE` includes any flushes it causes. Compare
  scopes; do not add them up. A scope re-entered on the same thread is
  measured once, by its outermost span.
- **Parked EDTs.** An EDT that blocks in `arts_event_wait` runs the scheduler
  (and other EDTs) on its own stack. The EDT scope is closed at the wait and
  reopened when it returns (next to the existing `TIME_EDT_EXEC` timer), so
  `PAPI_EDT` holds application bodies only and the scheduling in between
  counts as runtime.
- **Runtime overhead** on workers is therefore `PAPI_TOTAL_WORKER − PAPI_EDT`.
- **Compiled away when unused.** A scope is active only if its
  `PAPI_<SCOPE>_E0` counter is enabled in the counter config. With every
  `PAPI_*` counter OFF, or without `ARTS_USE_PAPI`, the hooks are no-ops.
- **Fail fast.** Every PAPI problem stops the run at startup with an error
  instead of recording zeros: an unknown event name, more than six events, a
  set that does not fit the core's counters, or a host that cannot count
  (the perf_event component's own reason is printed, e.g. `perf_event support
  disabled by Linux with paranoid=4`). Likewise, a counter config that
  enables a `PAPI_*` counter in a tree built without `ARTS_USE_PAPI` is a
  configure error.

### Building with PAPI

PAPI is not vendored. Point the build at an existing install (a system
package, a module, or a source build; PAPI 7.x is tested) and name a counter
config that enables the scopes you want:

```bash
cmake -GNinja -Bbuild_papi -DCMAKE_BUILD_TYPE=Release \
  -DARTS_USE_PAPI=ON -DARTS_PAPI_DIR=/path/to/papi/install \
  -DARTS_COUNTER_CONFIG=$PWD/configs/my_papi_counters.cfg \
  [protocol / CXL / benchmark options as usual]
ninja -C build_papi
```

`ARTS_PAPI_DIR` defaults to `$PAPI_DIR`. The library is linked into
`arts_interface`, so `libarts`, every benchmark variant
(`*_arts_ocr_excl_retain`, `*_cxl_staged`, …) and the tests all pick it up.
If `libpapi.so` is not on the default loader path, add its `lib/` directory
to `LD_LIBRARY_PATH` when running.

A counter config enables PAPI scopes like any other counter. A bare scope
name sets all six of its slots:

```ini
# configs/my_papi_counters.cfg
PAPI_TOTAL_WORKER=ONCE,NODE,SUM
PAPI_TOTAL_PROGRESS=ONCE,NODE,SUM
PAPI_EDT=ONCE,NODE,SUM
PAPI_DB_ACQUIRE=ONCE,NODE,SUM
PAPI_DB_RELEASE=ONCE,NODE,SUM
PAPI_CXL_FLUSH_PRODUCER=ONCE,NODE,SUM
PAPI_CXL_FLUSH_CONSUMER=ONCE,NODE,SUM
PAPI_NET_SEND=ONCE,NODE,SUM
PAPI_MSG_HANDLER=ONCE,NODE,SUM
# counts to normalise by
NUM_EDT_FINISH=ONCE,NODE,SUM
TIME_EDT_EXEC=ONCE,NODE,SUM
NUM_CXL_FLUSH_PRODUCER=ONCE,NODE,SUM
NUM_CXL_FLUSH_CONSUMER=ONCE,NODE,SUM
NUM_REMOTE_SEND=ONCE,NODE,SUM
```

Individual slots can also be set (`PAPI_EDT_E2=ONCE,NODE,SUM`), but a scope
only counts if its `_E0` slot is enabled. `configs/counters.cfg` lists every
slot (OFF) with a description of each scope. Prefer `NODE` level: `THREAD`
level writes one JSON file per thread, which is slow on a network file system.

### Host requirements

Counting needs the kernel's perf_event interface:

- `perf_event_paranoid` must be **≤ 2** for an unprivileged user
  (`cat /proc/sys/kernel/perf_event_paranoid`). Lowering it needs root:
  `sudo sh -c "echo 2 > /proc/sys/kernel/perf_event_paranoid"`.
- A VM must expose the PMU to the guest.
- Check what the host can count with PAPI's own tools: `papi_avail` (presets;
  the "Avail" column must say Yes) and `papi_native_avail` (CPU-specific
  native events). `papi_component_avail` reports why the perf_event component
  is disabled, if it is.

### Choosing events: `papi_events`

`arts.cfg`'s `papi_events` key names up to six comma-separated PAPI preset or
native events. The default is:

```ini
[ARTS]
papi_events=PAPI_TOT_CYC,PAPI_TOT_INS,PAPI_L1_DCM,PAPI_L2_TCM,PAPI_L3_TCM,PAPI_TLB_DM
```

Native stall/memory-bound events are CPU-specific, so they are never hard-coded
in the runtime. On Skylake-SP, for example:

```ini
papi_events=PAPI_TOT_CYC,PAPI_TOT_INS,CYCLE_ACTIVITY:STALLS_L3_MISS,MEM_LOAD_L3_MISS_RETIRED:LOCAL_DRAM,MEM_LOAD_L3_MISS_RETIRED:REMOTE_DRAM,MEM_LOAD_L3_MISS_RETIRED:REMOTE_HITM
```

Confirm names on the target machine with `papi_native_avail` before a
campaign. If the set does not fit the core's counters, the run stops with
`too many or conflicting events`; split the list across runs.

### Output

Each counter file (`n{rank}_t{tid}.json`, `n{rank}.json`, `cluster.json`)
carries the event list in its metadata, so `E<k>` can be mapped back to a
name:

```json
"metadata": {
  "papi_events": ["PAPI_TOT_CYC", "PAPI_TOT_INS", "PAPI_L1_DCM",
                  "PAPI_L2_TCM", "PAPI_L3_TCM", "PAPI_TLB_DM"],
  ...
},
"counters": {
  "PAPI_EDT_E0": { "captureMode": "ONCE", "captureLevel": "NODE",
                   "reduce_method": "SUM", "value": 851642649 },
  ...
}
```

`experiment_scripts/run_cxl_experiments.py` (and its `_optimal` copy) rename
slots to `PAPI_<SCOPE>[<event>]` from that metadata and derive, per scope:
IPC, each event's total, misses per thousand instructions (`_pki`), EDT
events per finished EDT (`_per_edt`), and `PAPI_WORKER_RUNTIME`
(`TOTAL_WORKER − EDT`).

### With artsrun

Two countersets ship in `experiments/countersets/`:

- `papi_cache`: every PAPI scope with the default cache/TLB events.
- `papi_memstall_skx`: every PAPI scope with Skylake-SP stall and DRAM events.

Both also enable the `NUM_*`/`TIME_*` counters that normalise the PAPI counts.
A counterset's `papi_events` list is rendered into each run's `arts.cfg`
(at most six names, validated when the counterset loads).

artsrun reconfigures a benchmark tree with only `ARTS_COUNTER_CONFIG`, so
configure the tree with `-DARTS_USE_PAPI=ON -DARTS_PAPI_DIR=...` once
beforehand; the setting stays in its CMake cache. Then run as usual:

```bash
artsrun counterset show papi_cache
artsrun run -p <profile> -x <experiment> -c papi_cache
```

A PAPI-instrumented tree is a counter build, so it is not used for wall-time
numbers. Take timings from an uninstrumented tree.

### Testing

`tests/unit/papi_scope.c` (registered as `papi_scope` only when
`ARTS_USE_PAPI=ON`) checks the scope bookkeeping directly: charge at end,
proportionality, same-scope re-entry, the `arts_event_wait` close/reopen
pattern, `TOTAL ≥ EDT`, unstarted threads, the fail-fast paths and the
`papi_events` config key. A host that cannot count prints `SKIP papi_scope`
instead of failing.

```bash
ctest --test-dir build_papi -R '^papi_scope$' --output-on-failure
```

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
