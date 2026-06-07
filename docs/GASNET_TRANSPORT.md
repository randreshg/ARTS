# ARTS GASNet-EX Transport

ARTS runs its multinode data plane on **GASNet-EX** (one-sided RMA + Active
Messages, connectionless wireup) as the v4 production default, in place of the
legacy `librdmacm` rsocket plane.

## Architecture: who does what (PMI is *not* MPI, and *not* the transport)

Three independent layers — don't conflate them:

| Layer | Component | Responsibility |
|---|---|---|
| Distributed runtime | **ARTS** (`arts_remote_*`, EDTs, datablocks, coherence) | *what* moves and when |
| Wire transport + **RDMA** | **GASNet-EX → UCX → RoCE NIC** (`rc_verbs`) | moves the bytes (one-sided RDMA) |
| **Bootstrap only** | **PMI2 via Slurm** (`srun --mpi=pmi2`) | launch processes + rank wireup at startup |

- **The data path is GASNet/UCX/RoCE RDMA — there is no MPI anywhere.** No MPI
  library is installed or linked (`mpicc` is absent). Slurm's `--mpi=pmi2` flag
  is a historically misnamed option that only selects the **PMI** (Process
  Management Interface) bootstrap plugin. PMI runs for milliseconds at launch to
  start the ranks and exchange endpoint addresses, then does nothing — it carries
  zero application data.
- ARTS owns the distributed-runtime semantics; it delegates the actual NIC byte
  movement to a backend. This branch swaps that backend from rsockets to
  GASNet-EX so transfers use **real one-sided RDMA** (`rc_verbs` over RoCE).
- The old rsockets path also needed a bootstrap (its SSH launcher + per-pair
  socket connect). PMI2 simply replaces that with a reliable, race-free one.

## What changed

- **`libs/src/core/transport/gasnet_transport.c`** — new low-level transport
  backend. Provides the same symbol surface the runtime expects from `socket.c`
  (the rsocket backend), so `dispatcher.c` (`arts_server_process_packet`),
  `protocol.c` (outbound queue + the three `*_async` enqueue functions) and
  `handler.c` are reused unchanged.
  - **Control + bulk over Active Messages** (`gex_AM_RequestMedium`); messages
    larger than one Medium are chunked and reassembled.
  - **Connectionless wireup** via `gex_Client_Init` — no per-pair socket mesh,
    no SSH/rconnect connection storm.
  - **AM handler → inbound queue → receiver thread** decoupling:
    `handler.c` handlers send and allocate, which is illegal inside a GASNet AM
    handler, so the handler only copies the packet into an inbound queue and the
    ARTS receiver thread (`arts_server_try_to_receive`) drains it and runs
    `arts_server_process_packet` outside AM context.
  - **Bulk transfer** of RMA-eligible datablocks is zero-copy via one-sided
    `gex_RMA_Put` into a segment-resident destination (`db_arena.c` allocates
    eligible datablocks in the GASNet segment; the move is driven from
    `handler.c`). Non-RMA-eligible traffic falls back to the Medium-AM path
    (header+payload sent as one contiguous message, chunked when larger than the
    conduit max-Medium).
- **`CMakeLists.txt` / `cmake/GasnetFlags.cmake`** — `ARTS_USE_GASNET` option,
  consumes a conduit `.mak` fragment, selects `gasnet_transport.c` vs `socket.c`,
  and exposes an `arts_gasnet` (build-only) interface target with the conduit
  include/define/link flags. The backend sets `config->master_boot=false` so the
  launcher is skipped — the GASNet spawner starts all ranks; `config.c` carries
  only the compile-time transport label reported at startup.

## Conduits / using real RDMA

The mi300x cluster fabric is **RoCE** (`ibv_devinfo`: `link_layer: Ethernet`,
`rocep*` devices). Conduit choice:

- **`ucx` conduit — the RDMA path (recommended here).** UCX drives RoCE
  Reliable-Connected verbs. Confirmed: with `UCX_TLS=rc,ud,sm,self` the UCX log
  reports the inter-node transport as `rc_verbs/rocep28s0:1 ...` — i.e. true
  RDMA over RoCE, not a TCP fallback. `fib 12` → 144 on 2 nodes over RoCE.
- `ofi` conduit — builds against libfabric but the verbs provider returns
  `-61 (No data available)` for RoCE without extra config, so it falls back to
  the `tcp` provider (not RDMA). Use `ucx` for RDMA.
- `ibv` conduit — does **not** support RoCE; unusable here.

Deps installed via apt (passwordless sudo): `libucx-dev libpmix-dev
libfabric-dev libibverbs-dev librdmacm-dev`. GASNet built with `--enable-ucx`.
(PMI/PMIx launch is unavailable: Ubuntu's `libpmix-dev` ships only internal
headers, no public `pmix.h`, and there is no `libpmi2`; so launch uses the
ssh-spawner recipe below.)

## Building

GASNet-EX 2024.5.0, threading mode `par`. ARTS requires the Ninja generator.

Local (udp conduit, single-host multi-proc testing):
```
cmake -S . -B build-gasnet -GNinja \
  -DARTS_USE_GASNET=ON -DARTS_GASNET_PREFIX=<gasnet-udp-install> \
  -DARTS_GASNET_CONDUIT=udp -DARTS_GASNET_THREADMODE=par \
  -DARTS_USE_GPU=OFF -DARTS_USE_RDMA=OFF -DARTS_BUILD_SHARED=OFF \
  -DCMAKE_BUILD_TYPE=Release
ninja -C build-gasnet fib
```

Cluster (ofi/libfabric conduit). Build GASNet with `--enable-ofi`, PIC, par.
Build ARTS **non-PIE** so EDT function pointers match across ranks regardless of
ASLR (`-DCMAKE_C_FLAGS=-fno-pie -DCMAKE_EXE_LINKER_FLAGS=-no-pie`). If libfabric
ships only `libfabric.so.1` (no dev symlink), add a `libfabric.so` symlink on
`LIBRARY_PATH`.

## Running multinode

A config file (`ARTS_CONFIG`) with `node_count` > 1 makes the runtime initialize
the network (and thus GASNet). Node IP/port fields are ignored by GASNet.

Local (udp), 2 procs on one host:
```
GASNET_SPAWNFN=L ARTS_CONFIG=<2-node.cfg> \
  amudprun -np 2 setarch $(uname -m) -R ./fib 12     # ASLR off for PIE builds
```
(Non-PIE builds don't need `setarch -R`.)

Cluster (ofi), N nodes under Slurm: `gasnetrun_ofi -n N <abs-path-to-binary> ...`
with `GASNET_SSH_SERVERS` set to the allocation's hostnames, and `~/.hushlogin`
to suppress login banners that corrupt the ssh-spawner.

## Validation status

- Local 2-rank (udp): `fib 12` → 144 ✓; `remote_db_read`/`distributed_reduction`
  PASS; `tiled_matmul` ✓. `simple_reduction` hangs **on the TCP baseline too**
  (pre-existing ARTS `DB_MODE_EW` remote-acquire issue, not the transport).
- **Cluster (ucx/RoCE, PMI2): reliable** — see the example table below.

### Distributed example tests (real datablock RDMA)

Two examples added (`examples/cpu/`) that exercise the distributed data path:
- **`remote_db_read`** — node 0 creates a 1 MiB datablock; a reader EDT on node 1
  acquires it read-only (whole-DB transfer node0→node1) and checksums it.
  PASS locally (udp, 2 procs) and **on the cluster over real RoCE RDMA**
  (reproduced on multiple node pairs): node 1 reads 1,048,576 bytes, checksum
  matches.
- **`distributed_reduction`** — every node contributes a read-only datablock to a
  collector EDT on node 0 (per-node remote RO acquire), summed and verified.
  PASS locally (udp, 2 procs). (RO counterpart of `simple_reduction`, whose
  `DB_MODE_EW` remote path hangs pre-existingly on both TCP and GASNet.)

Cluster runs are subject to the GASNet ssh-spawner bootstrap intermittency
described below (retry-until-bootstrap-succeeds); the data path itself is
byte-correct over RoCE RDMA whenever the job bootstraps.

### RDMA confirmed (UCX over RoCE)

With the `ucx` conduit and `UCX_TLS=rc,ud,sm,self`, the UCX runtime log reports
the inter-node transport as:

```
UCX INFO ... inter-node cfg#1 tag(rc_verbs/rocep28s0:1 rc_verbs/rocep29s0:1 ...)
[0] Fib 12: 144 time: ... nodes: 2 workers: 8
```

`rc_verbs` = Reliable-Connected verbs over the RoCE NICs (`rocep*`) — i.e. true
one-sided-capable RDMA, **not** the TCP fallback the ofi conduit used. Correct
multinode result (`fib`→144) reproduced on 2 nodes over the RoCE fabric.
Dependencies installed via apt (sudo): `libucx-dev libpmix-dev libfabric-dev
libibverbs-dev librdmacm-dev`.

### Robust launch: Slurm PMI2 (`srun --mpi=pmi2`) — the fix

The GASNet **ssh-spawner is unreliable** on this cluster: it bootstraps via a
TCP connect-back from each worker to the master, and that handshake races (and
on some nodes the master's advertised IP / routing varies), so jobs
intermittently fail with `reap_one ... one or more processes died before setup
completed`. The firewall was investigated and ruled out (nft INPUT policy =
ACCEPT). The intermittency is inherent to the ssh-spawner.

**Fix — launch via Slurm PMI2**, which does the rank exchange through Slurm with
no ssh fan-out and no connect-back, so the race cannot occur:

1. `apt install libpmi2-0t64` (Slurm 23.11 PMI2 client; interoperates with the
   cluster's 25.11 `pmi2` server), fetch `pmi2.h` from the Slurm source, and
   stage a `pmi2/{include/pmi2.h, lib/libpmi2.so->libpmi2.so.0}` dir on shared
   `/home`.
2. Build GASNet: `--enable-ucx --enable-pmi --with-pmi-version=2
   --with-pmi-home=<that dir> --with-ucx-spawner=pmi`. The
   **`--with-ucx-spawner=pmi`** makes PMI the **built-in default spawner**
   (`GASNET_SPAWNER_DEFAULT = pmi`), so the ssh-spawner is never used unless
   explicitly requested — PMI by default, never ssh.
3. Build ARTS non-PIE against it (`-DARTS_GASNET_CONDUIT=ucx`).
4. Launch with `docs/gasnet_slurm_launch.sh` (wraps `srun --mpi=pmi2 ... --export
   ARTS_CONFIG,LD_LIBRARY_PATH(pmi2),GASNET_UCX_SPAWNER=pmi,UCX_TLS=rc,ud,sm,self`).
   Put `libpmi2.so.0` and the ARTS config on shared `/home` (srun tasks run on
   remote nodes that don't share `/tmp`).

**Validated reliably** (incl. b-rack nodes that failed 100% with the
ssh-spawner), all over **rc_verbs/RoCE RDMA**, 3/3 reliability runs clean:

| example | 2 nodes | 4 nodes | notes |
|---|---|---|---|
| `fib` | 2584 ✓ | 6765 ✓ | EDT move + signal |
| `fib_yield` | 2584 ✓ | 6765 ✓ | correct result; UCX fatal during *teardown* only |
| `seidel_dep_at` | ✓ | — | stencil, whole-DB + halos |
| `tiled_matmul` | ✓ | ✓ | datablock matmul |
| `remote_db_read` | PASS | — | 1 MiB remote DB read, checksum ✓ |
| `distributed_reduction` | 24576 ✓ | 81920 ✓ | per-node RO remote acquire |

(Graph examples `bfs`/`graph_search`/`triangle_count*` need external graph
datasets — out of scope for transport validation. `simple_reduction` uses
`DB_MODE_EW` remote acquire which hangs pre-existingly on both TCP and GASNet.
`fib_yield`'s post-result teardown fault is a shutdown-ordering quirk, not a data
error.)

The legacy ssh-spawner recipe (`gasnetrun_ucx`, `GASNET_MASTERIP`, etc.) is kept
in git history for reference but is superseded by PMI2.

### Cluster launch (the working recipe)

Use `docs/gasnet_slurm_launch.sh` inside an sbatch `-N <nodes>` allocation. It
sets the GASNet env tunables this cluster needs (separate ssh cmd/options,
`GASNET_TMPDIR=/tmp`, `GASNET_HOST_DETECT=hostname`,
`GASNET_SUPERNODE_MAXSIZE=1`, a routable `GASNET_MASTERIP`, and `-E` env
forwarding) and calls `gasnetrun_ofi`. Requires `~/.hushlogin`.

**Caveat:** the GASNet ssh-spawner bootstraps via a TCP connect-back to the
master, and the routable inter-node IP differs per rack/subnet on this cluster,
so some node pairs fail the bootstrap (`reap_one ... ssh connection`). This is a
cluster networking/launcher issue, not the ARTS transport. The robust long-term
fix is the PMI/PMIx spawner (`srun --mpi=pmi2`), which requires building GASNet
against PMI headers (absent in the default image here).

## Notes / gotchas

- ARTS EDT move ships a raw function pointer → all ranks must share the same
  code layout: build non-PIE, or disable ASLR (`setarch -R`).
- GASNet AM handlers must not block or initiate non-reply comms — hence the
  inbound-queue decoupling.
- GASNet conduit `.mak` fragments inject `-O3 --param ...`; these are filtered
  out of the CMake flags (CMake de-duplicates repeated `--param` and mangles the
  command line).
