#!/usr/bin/env python3
"""Weak-scaling memory-footprint sweep of six fixed-iteration apps on CXL ARTS.

The apps whose work is an outer loop with a fixed count (CoMD, miniAMR,
nekbone, Stencil2D, stream and mapreduce_wordcount; npb_cg is out, since its
size is an NPB class, not a dial) run on 1..4 nodes at each per-node
footprint of FOOTPRINTS_GB.  A footprint is datablock bytes created per node
over the run (BYTES_DB_CREATE / nodes, what a CXL arena must hold: it
reclaims nothing within a run), and the sweep is weak-scaled: an N-node cell
creates N x footprint in total, with the per-node decomposition held fixed
and the domain grown along one axis.  Iteration counts are fixed per app at
every footprint and node count, so memory is the only thing that moves.

Each app's input is computed from its size model (SIZERS), fitted to
DRAM measurements of datablock bytes; see the "The sweep" section of
experiment_scripts/mem_iters_inputs.md.  A record carries the
model's prediction (predicted_db_bytes_per_node) beside the measured
db_create_bytes_per_node.

Sizes.  CXL DB allocation is round_robin (cxl_db_allocation_strategy):
the runtime opens one arena per CXL device and rotates allocations over
them, so capacity is CXL_DEVICES x the arena.  The arena
(ARTS_CXL_DB_ARENA_SIZE_BYTES, bytes PER ARENA) is a BUILD-TIME constant:
configure every tree (plain and PAPI) with
-DARTS_CXL_DB_ARENA_SIZE_BYTES=<CXL_DB_ARENA_SIZE_BYTES> and
-DCMAKE_BUILD_TYPE=Release.  Preflight refuses a tree whose CMakeCache has a
smaller arena or another build type, and a plan whose widest cell does not
fit CXL_DEVICES arenas.  The run.py region size (CXL_REGION_SIZE_BYTES) and
each rank's pool (CXL_DB_POOL_BYTES, written to the CXL cells' arts.cfg as
cxl_db_pool_bytes) are runtime settings.  All are constants at the top of
the configuration below.  Peak DRAM on the network (non-CXL) variants at
250 GB per node, 1 node: 256-374 GB (stream 374, miniAMR 307, CoMD 289,
mapreduce and Stencil2D 256), nekbone 31 GB (it frees each CG iteration's
vectors, which a CXL arena keeps).

Every (application, footprint, node count) cell runs once, one ARTS rank per
node, under a timeout that grows with the node count (CELL_TIMEOUT_S).  The
runtime's "round_robin across <k> device(s)" line is checked against
CXL_DEVICES on every CXL cell, and so is where the arenas actually landed:
each rank's "CXL DB arena devices: <d0>,<d1>,..." line must name CXL_DEVICES
distinct devices (the count line only says how many arenas were asked for).  Each cell is launched through the site's
run.py wrapper (which creates the CXL regions, runs the command once, and
releases the regions), and records:

  * e2e time: rank 0's "[E2E] <ns>" stamp (ARTS_E2E_MARKER=1), which spans
    application start to shutdown recognition and excludes runtime init and
    teardown.  The wall time of the whole run.py invocation is kept as a
    secondary number (it includes region setup and teardown).
  * counters: each rank writes counters/n<rank>.json (ONCE,NODE,SUM).  The
    per-rank values are kept raw and summed into cluster totals; totals over
    ranks are only reported when every rank's file is present.

Counter selection is a BUILD-TIME property of ARTS: this script only points
the runtime at a per-cell counter folder.  Both variants must be built with
COUNTER_CONFIG (configs/counters_cxl_ipdps27.cfg, via
-DARTS_COUNTER_CONFIG); preflight checks the build tree's CMakeCache for it,
and the counters it lists are the ones every rank file is expected to carry.
Any other counter present in the rank files is still recorded.

PAPI hardware events (--papi SET, repeatable) run the same matrix from a
separate build tree, PAPI_APP_BUILD_DIR, configured with -DARTS_USE_PAPI=ON
and PAPI_COUNTER_CONFIG (the ipdps27 counters plus every PAPI scope).  Each
cell runs once per event set, with the set's name in its cell_id, and the
set's events are written into that cell's arts.cfg as papi_events.  This
script never builds; it only checks the tree.

  1. Build the PAPI tree.  PAPI is not vendored: name an existing install
     (PAPI 7.x is tested), at the same path on every node.  The tree must be
     build_papi, beside the plain tree, which stays as it is: an
     instrumented tree's e2e times are not results.

       cmake -GNinja -Bbuild_papi -DCMAKE_BUILD_TYPE=Release
         -DARTS_CXL_DB_ARENA_SIZE_BYTES=<CXL_DB_ARENA_SIZE_BYTES>
         -DARTS_USE_PAPI=ON -DARTS_PAPI_DIR=/path/to/papi/install
         -DARTS_COUNTER_CONFIG=$PWD/configs/counters_cxl_ipdps27_papi.cfg
         -DARTS_BUILD_HPX=OFF -DARTS_BUILD_OCRVX=OFF -DARTS_BUILD_XSOCR=OFF
         <the CXL options of the plain tree>
       ninja -C build_papi

     Repeat the plain tree's CXL options so both trees build the same
     variants (with ARTS_CXL_REAL=ON, all four library paths):

       grep -E '^ARTS_(CXL|USE_CXL|COHERENCE|WRITE|RELEASE)' build/CMakeCache.txt

     build_papi/CMakeCache.txt must then hold ARTS_USE_PAPI:BOOL=ON, and
     build_papi/benchmarks/apps every <app>_<variant> binary.

  2. Prepare every node.  perf_event_paranoid must be <= 2
     (cat /proc/sys/kernel/perf_event_paranoid; to lower it,
     sudo sh -c "echo 2 > /proc/sys/kernel/perf_event_paranoid").
     papi_avail lists the presets the host counts ("Avail" must say Yes),
     papi_native_avail the CPU's native event names.

  3. Pick event sets.  A set is a name from PAPI_EVENT_SETS (cache: cache
     and TLB misses; memstall_skx: Skylake-SP native names only),
     NAME=EV1,EV2,... or a bare EV1,EV2,... (named custom<k>).  At most six
     events, which must fit the core's counters together (ARTS never
     multiplexes); put PAPI_TOT_CYC,PAPI_TOT_INS first so IPC and the
     per-kilo-instruction rates are derived.  Every --papi runs the whole
     matrix once more.

  4. Run.  Preflight refuses a tree without ARTS_USE_PAPI or with another
     counter config, and ssh-es to every node to check perf_event_paranoid
     and that papi_command_line can add each set's events; a refusal names
     the node and event, so swap that event out or split the set.  A
     --resume must repeat the campaign's --papi arguments.

PAPI results go to RESULTS_ROOT/papi-<timestamp>/, with a papi_set column in
both CSVs.  Counter slots are renamed after their events,
ctr_PAPI_<SCOPE>[<event>] (e.g. ctr_PAPI_EDT[PAPI_L1_DCM]), and derive per
scope papi_<scope>_ipc, papi_<scope>_<event>_pki (per 1000 instructions),
papi_edt_<event>_per_edt, and papi_worker_runtime_* (TOTAL_WORKER - EDT:
worker time outside EDT bodies).  Scopes are inclusive and overlap (a
release includes the flushes it causes): compare them, never add them.

Output (under RESULTS_ROOT/<run-id>/):

  manifest.json        the constants, git revision, and roster of this campaign
  runs.jsonl           one complete JSON record per run (source of truth;
                       appended as each run finishes, so it survives a crash)
  runs.csv             one flat row per run: fixed columns, derived metrics,
                       and one ctr_<NAME> column per counter (cluster sum)
  counters_long.csv    tidy per-rank counters: cell_id, rank, counter, value
  cells/<cell_id>/     args.json, arts.cfg, output.log, counters/n*.json

Usage:

  python3 experiment_scripts/run_cxl_experiments_magnificent7.py --dry-run
  python3 experiment_scripts/run_cxl_experiments_magnificent7.py
  python3 experiment_scripts/run_cxl_experiments_magnificent7.py --apps stream_dist,nekbone_dist --nodes 1,2
  python3 experiment_scripts/run_cxl_experiments_magnificent7.py --footprints 1,10 --dry-run
  python3 experiment_scripts/run_cxl_experiments_magnificent7.py --resume <run-dir>
  python3 experiment_scripts/run_cxl_experiments_magnificent7.py --smoke-test
  python3 experiment_scripts/run_cxl_experiments_magnificent7.py --papi cache --dry-run
  python3 experiment_scripts/run_cxl_experiments_magnificent7.py --smoke-test --papi cache
  python3 experiment_scripts/run_cxl_experiments_magnificent7.py --papi cache --papi memstall_skx
  python3 experiment_scripts/run_cxl_experiments_magnificent7.py \\
      --papi cache --papi stall=PAPI_TOT_CYC,PAPI_TOT_INS,CYCLE_ACTIVITY:STALLS_L3_MISS
  python3 experiment_scripts/run_cxl_experiments_magnificent7.py \\
      --papi cache --papi stall=... --resume <papi-run-dir>
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import functools
import json
import math
import os
import re
import shlex
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

# =============================================================================
# USER CONFIGURATION -- fill these in for the machine
# =============================================================================

REPO_ROOT = Path(__file__).resolve().parent.parent

# Hostnames of the four nodes, in rank order.  An N-node cell uses the first N.
# Rank 0 runs on the host this script is launched from, so NODES[0] must be
# that host.
NODES = ["node0", "node1", "node2", "node3"]

# Node counts of the weak-scaling sweep.
NODE_COUNTS = [1, 2, 3, 4]

# Per-node footprints, GB (10^9 B) of datablocks created per node over the run.
FOOTPRINTS_GB = [1, 10, 150, 200, 250]

# Threads per rank (per node).  Kept identical at every node count.
WORKER_THREADS = 92
PROGRESS_THREADS = 4

# One run per cell, no warm-up.  Warm-up runs, when asked for, are still
# logged, flagged is_warmup=true.
REPEATS = 1
WARMUP_RUNS = 0

# Per-run wall-clock budget by node count, seconds: 30 min at 1 node rising
# linearly to 1 h at 4.  A run past it is killed and recorded as status
# "timeout".  --timeout replaces it with one flat budget.
CELL_TIMEOUT_S = {1: 1800, 2: 2400, 3: 3000, 4: 3600}

# Where the executables live and how their names are formed:
#   <APP_BUILD_DIR>/<app>_<variant>
# Each release policy pairs network ARTS with a CXL build over the same
# matrix: PURGE with CXL-DIRECT, RETAIN with CXL-STAGED.  Every build must use
# the same counterset.  arts_ocr_excl_retain_cxl_staged keeps each rank's
# working copy in DRAM and uses the CXL slot only as the channel a block
# crosses when it changes rank: the owner writes it back only when it ships
# the block (a migration or a read-phase serve), and the receiver copies it in
# before its EDTs run.
APP_BUILD_DIR = REPO_ROOT / "build" / "benchmarks" / "apps"
VARIANTS = ["arts_ocr_excl_purge", "arts_ocr_excl_purge_cxl_direct",
            "arts_ocr_excl_retain", "arts_ocr_excl_retain_cxl_staged"]

# CXL sizes.  The widest cell (250 GB per node x 4 nodes) creates ~1 TB of
# datablocks, and the arena reclaims none of it before the run ends.  Under
# round_robin the runtime opens one arena per device and rotates every
# allocation over them, with no fallback: the first arena to fill ends the
# run, so capacity is CXL_DEVICES x the arena, less what rank pools strand.
#   * CXL_DEVICES / CXL_DEVICE_BYTES: the DAX devices of the CXL machine and
#     what one can allocate.  The runtime must report CXL_DEVICES (checked
#     per cell from its "round_robin across <k> device(s)" line).
#   * CXL_DB_ARENA_SIZE_BYTES: ONE arena, i.e. one device's share.  It is
#     compiled in (-DARTS_CXL_DB_ARENA_SIZE_BYTES), so changing it here only
#     changes what preflight demands of the tree: rebuild to match.  14 x
#     96 GiB = 1.44 TB: the widest cell (~1.1 TB with margin) plus every
#     pool tail it can strand (224 GiB), ~7% to spare.
#   * CXL_REGION_SIZE_BYTES: what run.py allocates per region (--region-size),
#     taken as one device's region, so it must hold one arena.
#   * CXL_DB_POOL_BYTES: each rank's pool on each arena (cxl_db_pool_bytes),
#     the bytes one refill carves off it, under the cross-node tournament
#     lock.  4 GiB keeps refills to ~60 per rank in a 250 GB cell while
#     ranks still share each arena: a rank keeps one pool per arena, so at
#     most devices x ranks x pool (224 GiB at 4 nodes) is stranded, which
#     the arena size covers.  ARTS clamps it to arena / ranks (24 GiB at 4).
CXL_DEVICES = 14
CXL_DEVICE_BYTES = 512 * 10**9
CXL_DB_ARENA_SIZE_BYTES = 96 * 1024**3
CXL_REGION_SIZE_BYTES = 100 * 1024**3
CXL_DB_POOL_BYTES = 4 * 1024**3
# Headroom preflight demands over a cell's predicted cluster total, for
# model error and uneven fill across the rotated arenas.
CXL_CAPACITY_MARGIN = 1.10

# The site launch wrapper and the arguments it receives besides
# --path-to-json.  "--local" runs the binary on this host and lets ARTS
# remote-launch its own ranks from the configuration.
RUN_PY = REPO_ROOT / "script" / "run.py"
RUN_PY_ARGS = ["--local", "--region-size", str(CXL_REGION_SIZE_BYTES)]
# Hand the rendered arts.cfg to run.py's update_cfg_for_allocation()
# ("arts_cfg" in the JSON).  Leave False unless that helper preserves the
# per-cell node list: this script already writes the exact hosts and count.
RUN_PY_UPDATES_CFG = False

# Results live here.  It MUST be on a filesystem every node shares: remote
# ranks cd into the cell directory and write their counter files into it.
RESULTS_ROOT = REPO_ROOT / "results" / "cxl_magnificent7_weak"

# ARTS runtime configuration written into each cell's arts.cfg.
ARTS_LAUNCHER = "ssh"          # ARTS remote-launches ranks onto NODES over ssh
ARTS_PORTS = [25000]           # base port(s); length must equal port count
ARTS_PROVIDER = "tcp"          # e.g. "tcp", "verbs;ofi_rxm", "cxi"
ARTS_NET_INTERFACE = None      # e.g. "ib0"; None lets the runtime choose
ARTS_PIN = True
ARTS_ROUTE_TABLE_SIZE = 16
COUNTER_CAPTURE_INTERVAL = 250  # irrelevant to ONCE counters, must be > 0
# [ARTS] keys written only into CXL cells.  round_robin is the only strategy
# besides the default static (any other value silently means static).
CXL_ARTS_CFG: dict[str, str] = {
    "cxl_db_allocation_strategy": "round_robin",
}
# Any further [ARTS] keys, written into every cell.
EXTRA_ARTS_CFG: dict[str, str] = {}

# Extra environment for every run (ARTS_CONFIG and ARTS_E2E_MARKER are set by
# the script).  LD_LIBRARY_PATH is forwarded to remote ranks by ARTS itself.
EXTRA_ENV: dict[str, str] = {
    # One flush-trace path is shared by every rank; discard it rather than
    # let ranks overwrite one file in the shared cell directory.
    "ARTS_FLUSH_LOG": "/dev/null",
}

# After a timeout, ssh to every node of the cell and pkill the binary so a
# wedged remote rank cannot hold ports or CXL regions into the next run.
CLEANUP_ON_TIMEOUT = True

# The counter configuration both variants are built with.  The run records
# every counter it finds; the ones this file turns on drive the "missing
# counter" warning.
COUNTER_CONFIG = REPO_ROOT / "configs" / "counters_cxl_ipdps27.cfg"

def _configured_counters(path: Path) -> list[str]:
    """The counter names a NAME=MODE,LEVEL,REDUCTION config turns on."""
    names = []
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if "=" in line:
            names.append(line.split("=", 1)[0].strip())
    return names

EXPECTED_COUNTERS = _configured_counters(COUNTER_CONFIG)

# --papi campaigns.  PAPI is compiled in, so they run from their own tree:
#   cmake -GNinja -Bbuild_papi -DCMAKE_BUILD_TYPE=Release \
#     -DARTS_USE_PAPI=ON -DARTS_PAPI_DIR=/path/to/papi/install \
#     -DARTS_COUNTER_CONFIG=$PWD/configs/counters_cxl_ipdps27_papi.cfg \
#     [the same protocol / CXL / benchmark options as the plain tree]
# Every node needs perf_event_paranoid <= 2 and the PAPI install at the same
# path (the binaries link libpapi by path).
PAPI_APP_BUILD_DIR = REPO_ROOT / "build_papi" / "benchmarks" / "apps"
PAPI_COUNTER_CONFIG = REPO_ROOT / "configs" / "counters_cxl_ipdps27_papi.cfg"

# Named event sets for --papi, in slot order (E0..E5), at most six each.
# PAPI_TOT_CYC and PAPI_TOT_INS go first so every set yields IPC and per-
# kilo-instruction rates.  Native names are CPU-specific: memstall_skx is
# Skylake-SP's.  Confirm names with papi_native_avail on the target machine
# and add a set here (or pass NAME=EV1,... on the command line) for another
# CPU; preflight refuses a set a node cannot count.
PAPI_EVENT_SETS: dict[str, list[str]] = {
    # experiments/countersets/papi_cache.yaml's set.
    "cache": ["PAPI_TOT_CYC", "PAPI_TOT_INS", "PAPI_L1_DCM", "PAPI_L2_TCM",
              "PAPI_L3_TCM", "PAPI_TLB_DM"],
    # experiments/countersets/papi_memstall_skx.yaml's set: memory-bound
    # stalls and where L3 misses were served.
    "memstall_skx": ["PAPI_TOT_CYC", "PAPI_TOT_INS",
                     "CYCLE_ACTIVITY:STALLS_L3_MISS",
                     "MEM_LOAD_L3_MISS_RETIRED:LOCAL_DRAM",
                     "MEM_LOAD_L3_MISS_RETIRED:REMOTE_DRAM",
                     "MEM_LOAD_L3_MISS_RETIRED:REMOTE_HITM"],
}
PAPI_MAX_EVENTS = 6

# Applications (base names, without the _<variant> suffix) and how each is
# sized.  A sizer maps (footprint bytes per node, nodes) to (args, predicted
# DB bytes per node).  Weak scaling: the per-node decomposition (tiles,
# blocks, chains, tasks) is held at every node count and the domain grows
# along one axis, so each rank creates the same bytes; iteration counts are
# fixed per app.  The models are fitted to single-rank DRAM runs (1 rank,
# 76 workers, no CXL) on the Release counter tree build_sweep/; the
# measurements are in experiment_scripts/mem_iters_inputs.md.

GB = 10**9

def near_cube(target: float, max_aspect: float = 2.0) -> tuple[int, int, int]:
    """(a, b, c), a >= b >= c, whose product is closest to target with
    a / c <= max_aspect; within 1% of the best fit, the most cube-like grid.
    The aspect bound is doubled until something fits (small targets)."""
    t = max(1, round(target))
    cands = []
    for c in range(1, int(round(t ** (1 / 3))) + 2):
        for b in range(c, int(math.sqrt(t / c)) + 3):
            for a in {max(b, math.floor(t / (b * c))),
                      max(b, math.ceil(t / (b * c)))}:
                if a <= max_aspect * c:
                    cands.append((abs(a * b * c - target) / target, a / c,
                                  (a, b, c)))
    if not cands:
        return near_cube(target, max_aspect * 2)
    best = min(e for e, _, _ in cands)
    return min((asp, abc) for e, asp, abc in cands if e <= best + 0.01)[1]

def size_stencil2d(fp: float, n: int):
    """NP NR NT.  1152 tiles per node (NR = 1152 n).  DB = 16 NP^2 (two
    NP x NP double grids) + STENCIL_HALO_B x NP x sqrt(NR) (each tile's halo
    buffers, ~ its side); NP solved from that quadratic.  50 steps."""
    nr = 1152 * n
    k = STENCIL_HALO_B * math.sqrt(nr)
    np_ = int((-k + math.sqrt(k * k + 64 * fp * n)) / 32)
    return f"{np_} {nr} 50", (16 * np_ ** 2 + k * np_) / n

def size_stream(fp: float, n: int):
    """total_elems chains ntimes.  3456 chains per node; DB = 3 arrays x
    total_elems x 8 B, total_elems a multiple of the chain count.  60
    iterations."""
    chains = 3456 * n
    per_chain = round(fp / (24 * 3456))
    return f"{per_chain * chains} {chains} 60", 24 * per_chain * 3456

@functools.lru_cache(maxsize=None)
def mapreduce_tree_blocks(tiles: int, k: int = 8) -> int:
    """Partial histograms one round creates: one per map tile plus one per
    combine of the app's spawner tree (spawn_subtree: a range splits into
    min(n, K) near-equal chunks).  457 at 384 tiles; per node it is not
    flat in the node count (548.5 at 2 nodes, 579 at 3, 530.25 at 4)."""
    if tiles == 1:
        return 1
    parts = min(tiles, k)
    q, r = divmod(tiles, parts)
    return 1 + sum(mapreduce_tree_blocks(q + (i < r), k) for i in range(parts))

def size_mapreduce(fp: float, n: int):
    """V WORDS TPN K ZIPF GRAIN ITER SEED.  TPN = 384 map tiles per node
    (M = 384 n, the app's own weak form); DB = V x 8 B x the tree's blocks
    per round x 6 rounds, so V is sized per node count to hold bytes per
    node at the footprint."""
    blocks = mapreduce_tree_blocks(384 * n) / n
    v = round(fp / (8 * blocks * 6))
    return f"{v} 333334 384 8 100 0 6 1", v * 8 * blocks * 6

def size_miniamr(fp: float, n: int):
    """10^3-cell blocks (num_vars 40) over 5 timesteps, refinement and
    checksum blocks included; a near-cubic block grid per node, stacked
    along z.  Bytes per block fall as the grid grows (refinement is a
    surface effect): MINIAMR_BLOCK_B + MINIAMR_SURFACE_B x B^(-1/3) for B
    blocks per node."""
    def per_node(blocks):
        return blocks * (MINIAMR_BLOCK_B + MINIAMR_SURFACE_B * blocks ** (-1 / 3))
    blocks = fp / MINIAMR_BLOCK_B
    for _ in range(20):  # fixed point: per-block bytes depend on the count
        blocks = fp / (per_node(blocks) / blocks)
    a, b, c = near_cube(blocks)
    return (f"--nx 10 --ny 10 --nz 10 --npx {a} --npy {b} --npz {c * n} "
            f"--num_refine 1 --num_tsteps 5"), per_node(a * b * c)

def size_comd(fp: float, n: int):
    """Tiles of s^3 unit cells: s = 9 below 100 GB, s = 18 above (one tile's
    DB measured at each, COMD_TILE_B); a near-cubic tile grid per node,
    stacked along x.  60 steps."""
    s = 9 if fp < 100 * GB else 18
    tile = COMD_TILE_B[s]
    a, b, c = near_cube(fp / tile)
    return (f"-x {s * a * n} -y {s * b} -z {s * c} -N 60 -n 10 "
            f"-i {a * n} -j {b} -k {c}"), a * b * c * tile

def nekbone_task_bytes(e: int) -> float:
    """One task's DB bytes at E elements (pDOF 8, 20 CG iterations): a fixed
    part, a volume part (vectors created fresh every CG iteration, which the
    arena keeps) and a halo part ~ E^(2/3)."""
    return NEKBONE_TASK_B + NEKBONE_ELEM_B * e + NEKBONE_HALO_B * e ** (2 / 3)

def size_nekbone(fp: float, n: int):
    """Rx Ry Rz Ex Ey Ez pDOF CGcount.  A per-node task grid (stacked along
    x) of Ex x Ey x Ez elements per task at pDOF 8, 20 CG iterations.  One
    element per task already costs ~4 GB at 3456 tasks, so smaller
    footprints drop to 432 tasks.  The element block is the closest fit
    among grids with aspect <= 3."""
    big = 3456 * nekbone_task_bytes(1)
    rx, ry, rz = (18, 16, 12) if fp >= big else (9, 8, 6)
    tasks = rx * ry * rz
    best = None
    for c in range(1, 9):
        for b in range(c, 3 * c + 1):
            for a in range(b, 3 * c + 1):
                pred = tasks * nekbone_task_bytes(a * b * c)
                key = (abs(pred - fp), a - c)
                if best is None or key < best[0]:
                    best = (key, (a, b, c), pred)
    _, (ex, ey, ez), pred = best
    return f"{rx * n} {ry} {rz} {ex} {ey} {ez} 8 20", pred

# Model constants (bytes).  Fitted from build_sweep/ runs; see each sizer.
STENCIL_HALO_B = 194
MINIAMR_BLOCK_B = 0.986e6
MINIAMR_SURFACE_B = 2.1e6
COMD_TILE_B = {9: 6.16e6, 18: 26.94e6}
NEKBONE_TASK_B = 0.455e6
NEKBONE_ELEM_B = 0.3551e6
NEKBONE_HALO_B = 0.3675e6

SIZERS = {
    "CoMD_intel_chandra_tiled_hinted": size_comd,
    "miniAMR_intel_dist": size_miniamr,
    "nekbone_dist": size_nekbone,
    "mapreduce_wordcount": size_mapreduce,
    "Stencil2D_intel_channelEVTs": size_stencil2d,
    "stream_dist": size_stream,
}
APPS = list(SIZERS)

# --smoke-test inputs: fixed, weak-form correctness smokes (per-node work,
# so they grow with the node count) that finish well inside 30 s at 1..4
# nodes, for checking the campaign end to end before committing to the real
# matrix.  No footprint; nothing timed here is a result.
SMOKE_APPS: dict[str, str] = {
    "CoMD_intel_chandra_tiled_hinted": "-x 16 -y 8 -z 8 -N 2 -n 1 -i 4 -j 2 -k 2",
    "miniAMR_intel_dist": ("--nx 4 --ny 4 --nz 4 --npx 2 --npy 2 --npz 2 "
                           "--num_refine 1 --num_tsteps 2"),
    "nekbone_dist": "2 2 2 2 2 2 8 2",
    "mapreduce_wordcount": "1024 10000 3 4 100 0 2 1",
    "Stencil2D_intel_channelEVTs": "12 4 2",
    "stream_dist": "400 4 2",
}

# An input profile: each app's input (a sizer, or a fixed string), the
# footprints it sweeps (None: the input is fixed), and the repeats /
# warm-ups / timeout it runs with unless the command line says otherwise.
# The timeout is seconds, flat or {nodes: seconds}.  An app with no input in
# a profile is not part of that profile.
PROFILES: dict[str, dict] = {
    "main": {"apps": SIZERS, "footprints": FOOTPRINTS_GB,
             "repeats": REPEATS, "warmups": WARMUP_RUNS,
             "timeout": CELL_TIMEOUT_S},
    # A smoke is one pass, no warm-up, and a short leash.
    "smoke": {"apps": SMOKE_APPS, "footprints": [None], "repeats": 1,
              "warmups": 0, "timeout": 120},
}


# =============================================================================
# End of user configuration
# =============================================================================

E2E_RE = re.compile(r"^\[E2E\]\s+(\d+)\s*$", re.M)
DONE_RE = re.compile(r"^DONE!\s+(-?\d+)", re.M)

# Fixed leading columns of runs.csv; derived metrics and ctr_* follow.
BASE_COLUMNS = [
    "cell_id", "app", "variant", "papi_set", "executable", "footprint_gb",
    "nodes", "worker_threads",
    "progress_threads", "repeat", "is_warmup", "status", "return_code",
    "e2e_ns", "e2e_s", "wall_s", "ranks_reported", "counters_complete",
    "checks_failed", "started_at", "predicted_db_bytes_per_node",
    "db_create_bytes_per_node", "cxl_devices_reported",
    "cxl_arena_devices_reported",
]
DERIVED_COLUMNS = [
    "edt_count", "mean_edt_exec_ns", "edt_create_count", "mean_edt_create_ns",
    "edt_signal_count", "mean_edt_signal_ns",
    "db_create_count", "mean_db_create_ns", "db_create_bytes",
    "mean_db_create_bytes",
    "cxl_protocol_read_bytes", "cxl_protocol_write_bytes",
    "cxl_protocol_total_bytes", "cxl_protocol_read_Bps",
    "cxl_protocol_write_Bps", "cxl_protocol_total_Bps",
    "cxl_flush_producer_count", "cxl_flush_consumer_count",
    "mean_cxl_flush_producer_ns", "mean_cxl_flush_consumer_ns",
    "cxl_flush_producer_op_Bps", "cxl_flush_consumer_op_Bps",
    "network_payload_bytes", "network_payload_Bps", "db_payload_put_count",
    "mean_db_payload_put_ns", "db_payload_put_op_Bps",
]

# ---------------------------------------------------------------------------
# Campaign plan
# ---------------------------------------------------------------------------

def plan_cells(apps, variants, node_counts, footprints, repeats, warmups,
               papi_sets=None):
    """Warm-ups first per (footprint, app, variant, event set, nodes), then
    measured repetitions repeat-major.  Within a pass footprints run smallest
    first, so a broken app or variant shows on cells that take seconds, not
    on the 250 GB ones.  apps is [(app, sizer or fixed args)], footprints GB
    per node ([None] for fixed inputs).  papi_sets is [(name, events)] for a
    --papi campaign, None otherwise."""
    sets = papi_sets or [None]

    def sweep(index, warmup):
        return [_cell(app, spec, variant, n, fp, index, warmup=warmup,
                      papi=papi)
                for fp in footprints
                for variant in variants
                for papi in sets
                for app, spec in apps
                for n in node_counts]

    cells = [c for w in range(warmups) for c in sweep(w, True)]
    return cells + [c for rep in range(repeats) for c in sweep(rep, False)]

def cell_input(spec, footprint_gb, nodes: int):
    """(args, predicted DB bytes per node) of one cell: a sizer evaluated at
    the footprint and node count, or a fixed string (no prediction).
    {repo} becomes REPO_ROOT."""
    if callable(spec):
        args, predicted = spec(footprint_gb * GB, nodes)
    else:
        args, predicted = spec, None
    return args.replace("{repo}", str(REPO_ROOT)), predicted

def _cell(app, spec, variant, nodes, footprint_gb, index, *, warmup,
          papi=None):
    tag = f"w{index}" if warmup else f"r{index}"
    # The event set is part of the cell's identity: two sets of one
    # (app, variant, nodes, repeat) are two runs, not a retry.
    papi_set, papi_events = papi if papi else (None, None)
    set_tag = f"__{papi_set}" if papi_set else ""
    fp_tag = f"__f{footprint_gb}g" if footprint_gb is not None else ""
    args, predicted = cell_input(spec, footprint_gb, nodes)
    return {
        "cell_id": f"{app}__{variant}{fp_tag}__n{nodes}{set_tag}__{tag}",
        "app": app,
        "variant": variant,
        "papi_set": papi_set,
        "papi_events": papi_events,
        "executable": f"{app}_{variant}",
        "args": args,
        "footprint_gb": footprint_gb,
        "predicted_db_bytes_per_node": (round(predicted)
                                        if predicted is not None else None),
        "nodes": nodes,
        "repeat": index,
        "is_warmup": warmup,
    }

def cell_timeout(timeout, nodes: int) -> int:
    """A profile or --timeout budget for one cell: flat, or by node count."""
    return timeout[nodes] if isinstance(timeout, dict) else timeout

# ---------------------------------------------------------------------------
# Per-cell files
# ---------------------------------------------------------------------------

def render_arts_cfg(nodes: int, counter_folder: Path, variant: str,
                    papi_events: list[str] | None = None) -> str:
    lines = [
        "[ARTS]",
        f"worker_threads={WORKER_THREADS}",
        f"progress_threads={PROGRESS_THREADS}",
        f"launcher={ARTS_LAUNCHER}",
        f"node_count={nodes}",
        f"nodes={','.join(NODES[:nodes])}",
        f"pin={1 if ARTS_PIN else 0}",
        f"route_table_size={ARTS_ROUTE_TABLE_SIZE}",
        "core_dump=0",
        f"port_count={len(ARTS_PORTS)}",
        f"ports={','.join(str(p) for p in ARTS_PORTS)}",
        f"counter_folder={counter_folder}",
        f"counter_capture_interval={COUNTER_CAPTURE_INTERVAL}",
    ]
    if ARTS_PROVIDER:
        lines.append(f"provider={ARTS_PROVIDER}")
    if ARTS_NET_INTERFACE:
        lines.append(f"net_interface={ARTS_NET_INTERFACE}")
    if papi_events:
        lines.append(f"papi_events={','.join(papi_events)}")
    # Only a CXL variant knows these keys; a network build would ignore them.
    if "cxl" in variant:
        lines.append(f"cxl_db_pool_bytes={CXL_DB_POOL_BYTES}")
        lines += [f"{k}={v}" for k, v in CXL_ARTS_CFG.items()]
    lines += [f"{k}={v}" for k, v in EXTRA_ARTS_CFG.items()]
    return "\n".join(lines) + "\n"

def prepare_cell(cell, run_dir: Path):
    cell_dir = run_dir / "cells" / cell["cell_id"]
    counter_dir = cell_dir / "counters"
    if cell_dir.exists():
        # A retried cell starts clean: stale rank files would pass as this
        # run's counters.
        for f in counter_dir.glob("*.json"):
            f.unlink()
    counter_dir.mkdir(parents=True, exist_ok=True)

    cfg = cell_dir / "arts.cfg"
    cfg.write_text(render_arts_cfg(cell["nodes"], counter_dir,
                                   cell["variant"], cell["papi_events"]))

    spec = {
        "app": str(APP_BUILD_DIR / cell["executable"]),
        "args": cell["args"],
    }
    if RUN_PY_UPDATES_CFG:
        spec["arts_cfg"] = str(cfg)
    args_json = cell_dir / "args.json"
    args_json.write_text(json.dumps(spec, indent=2) + "\n")
    return cell_dir, counter_dir, cfg, args_json

def launch_argv(args_json: Path) -> list[str]:
    return [sys.executable, str(RUN_PY), *RUN_PY_ARGS,
            "--path-to-json", str(args_json)]

# ---------------------------------------------------------------------------
# Running one cell
# ---------------------------------------------------------------------------

def kill_remote(cell):
    """Best-effort cleanup of a timed-out cell's ranks on every node."""
    name = cell["executable"][:15]  # pkill matches the 15-char comm name
    for host in NODES[:cell["nodes"]]:
        cmd = ["pkill", "-9", name]
        if host != socket.gethostname():
            cmd = ["ssh", "-o", "BatchMode=yes", host, *cmd]
        subprocess.run(cmd, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=30)

def run_cell(cell, run_dir: Path, timeout_s: int):
    cell_dir, counter_dir, cfg, args_json = prepare_cell(cell, run_dir)
    log_path = cell_dir / "output.log"

    env = dict(os.environ)
    env.update(EXTRA_ENV)
    env["ARTS_CONFIG"] = str(cfg)
    env["ARTS_E2E_MARKER"] = "1"
    if cell["papi_set"]:
        # The binaries carry libpapi's build-tree rpath; this covers a tree
        # installed or copied without it.  ARTS forwards it to remote ranks.
        lib_dir = papi_lib_dir()
        if lib_dir:
            env["LD_LIBRARY_PATH"] = os.pathsep.join(
                filter(None, [lib_dir, env.get("LD_LIBRARY_PATH")]))

    argv = launch_argv(args_json)
    started_at = dt.datetime.now().isoformat(timespec="seconds")
    status, rc = "ok", None
    t0 = time.monotonic()
    with open(log_path, "w") as log:
        log.write(f"# cmd: {shlex.join(argv)}\n# cwd: {cell_dir}\n")
        log.write(f"# ARTS_CONFIG={cfg}\n# started: {started_at}\n")
        log.flush()
        # Own process group, so a timeout takes run.py and the local rank
        # down together.
        proc = subprocess.Popen(argv, cwd=cell_dir, env=env, stdout=log,
                                stderr=subprocess.STDOUT,
                                start_new_session=True)
        try:
            rc = proc.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            status = "timeout"
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
            rc = proc.returncode
            if CLEANUP_ON_TIMEOUT:
                kill_remote(cell)
    wall_s = time.monotonic() - t0

    text = log_path.read_text(errors="replace")
    stamps = [int(s) for s in E2E_RE.findall(text)]
    # run.py's own exit status is the application's; "DONE! <rc>" confirms
    # the wrapper reached region teardown.
    done = DONE_RE.search(text)
    if status == "ok" and rc != 0:
        status = "failed"
    if status == "ok" and not stamps:
        status = "no_e2e"

    per_rank, rank_events = read_rank_counters(counter_dir)
    record = build_record(cell, status, rc, stamps, wall_s, per_rank,
                          started_at, wrapper_done=done is not None,
                          rank_events=rank_events,
                          cxl_devices=cxl_devices_reported(text),
                          arena_devices=cxl_arena_devices_reported(text))
    record["log"] = str(log_path.relative_to(run_dir))
    record["counter_dir"] = str(counter_dir.relative_to(run_dir))
    return record

# ---------------------------------------------------------------------------
# Counters
# ---------------------------------------------------------------------------

def read_rank_counters(counter_dir: Path):
    """({rank: {counter: value}}, {rank: papi_events}) from n<rank>.json.
    The integer "value" is used for time counters too (ns), never the rounded
    value_ms.  papi_events is the list the rank's metadata echoes ([] when
    the build counts no PAPI events)."""
    out, rank_events = {}, {}
    for path in sorted(counter_dir.glob("n*.json")):
        m = re.fullmatch(r"n(\d+)\.json", path.name)
        if not m:
            continue  # n<r>_t<t>.json thread files are not used here
        try:
            data = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        events = data.get("metadata", {}).get("papi_events", [])
        rank_events[int(m.group(1))] = events
        out[int(m.group(1))] = {
            papi_slot_name(name, events): int(rec["value"])
            for name, rec in data.get("counters", {}).items()
        }
    return out, rank_events

PAPI_SLOT_RE = re.compile(r"(PAPI_[A-Z_]+)_E(\d)")

def papi_slot_name(name: str, events: list[str]) -> str:
    """PAPI_<SCOPE>_E<k> holds the k-th papi_events entry of that run, so the
    slot is renamed to what it counted: PAPI_EDT[PAPI_L1_DCM].  Two runs with
    different event lists then never sum one event into another."""
    m = PAPI_SLOT_RE.fullmatch(name)
    if not m or int(m.group(2)) >= len(events):
        return name
    return f"{m.group(1)}[{events[int(m.group(2))]}]"

def expected_counters(papi_events: list[str] | None) -> list[str]:
    """EXPECTED_COUNTERS as one cell's rank files name them: a bare PAPI scope
    of the counter config (PAPI_EDT) stands for one PAPI_EDT[<event>] per
    event of the cell's set, and an explicit slot is renamed the same way."""
    out = []
    for name in EXPECTED_COUNTERS:
        if not name.startswith("PAPI_"):
            out.append(name)
        elif PAPI_SLOT_RE.fullmatch(name):
            out.append(papi_slot_name(name, papi_events or []))
        else:
            out += [f"{name}[{e}]" for e in papi_events or []]
    return out

def derive_papi(sums: dict[str, int], edts) -> dict:
    """Per PAPI scope: IPC, each other event per kilo-instruction, and, for the
    EDT scope, per EDT.  Worker runtime overhead is TOTAL_WORKER minus EDT,
    event by event (EDT bodies run only on workers).  Region scopes overlap,
    so they are never summed."""
    scopes: dict[str, dict[str, int]] = {}
    for name, value in sums.items():
        m = re.fullmatch(r"(PAPI_[A-Z_]+)\[(.+)\]", name)
        if m:
            scopes.setdefault(m.group(1), {})[m.group(2)] = value
    tot, edt = scopes.get("PAPI_TOTAL_WORKER"), scopes.get("PAPI_EDT")
    if tot and edt:
        scopes["PAPI_WORKER_RUNTIME"] = {
            e: tot[e] - edt[e] for e in tot if e in edt}
    out = {}
    for scope, ev in scopes.items():
        key = scope.removeprefix("PAPI_").lower()
        ins = ev.get("PAPI_TOT_INS")
        out[f"papi_{key}_ipc"] = _div(ins, ev.get("PAPI_TOT_CYC"))
        for e, v in ev.items():
            out[f"papi_{key}_{e}"] = v
            if e not in ("PAPI_TOT_INS", "PAPI_TOT_CYC"):
                rate = _div(v, ins)
                out[f"papi_{key}_{e}_pki"] = None if rate is None else rate * 1e3
            if scope == "PAPI_EDT":
                out[f"papi_{key}_{e}_per_edt"] = _div(v, edts)
    return out

def _div(a, b):
    return None if a is None or not b else a / b

def derive(sums: dict[str, int], e2e_s: float | None) -> dict:
    """The metrics COUNTER_CONFIG is built for, from cluster sums.  Anything
    whose counter is absent from the build is None.  TIME_* / NUM_* is the
    mean per operation; BYTES_* / TIME_* (ns) is one operation's throughput,
    since operations on different threads overlap; BYTES_* / e2e is the
    aggregate rate."""
    g = sums.get
    # A consumer flush is the fetch's read edge, a producer flush the purge's
    # write edge; their bytes are the block sizes moved on either residency.
    fetch, purge = g("BYTES_CXL_FLUSH_CONSUMER"), g("BYTES_CXL_FLUSH_PRODUCER")
    n_prod, n_cons = g("NUM_CXL_FLUSH_PRODUCER"), g("NUM_CXL_FLUSH_CONSUMER")
    t_prod, t_cons = g("TIME_CXL_FLUSH_PRODUCER"), g("TIME_CXL_FLUSH_CONSUMER")
    cxl_total = fetch + purge if fetch is not None and purge is not None else None
    payload, n_put = g("BYTES_DB_PAYLOAD_SENT"), g("NUM_DB_PAYLOAD_SENT")
    t_put = g("TIME_DB_PAYLOAD_PUT")
    edts, creates, signals = (g("NUM_EDT_FINISH"), g("NUM_EDT_CREATE"),
                              g("NUM_EDT_SIGNAL"))
    # NUM/BYTES count home-local creates only; TIME times every create call.
    db_creates, db_bytes = g("NUM_DB_CREATE"), g("BYTES_DB_CREATE")

    def per_ns(nbytes, ns):
        rate = _div(nbytes, ns)
        return None if rate is None else rate * 1e9

    return {
        "edt_count": edts,
        "mean_edt_exec_ns": _div(g("TIME_EDT_EXEC"), edts),
        "edt_create_count": creates,
        "mean_edt_create_ns": _div(g("TIME_EDT_CREATE"), creates),
        "edt_signal_count": signals,
        "mean_edt_signal_ns": _div(g("TIME_EDT_SIGNAL"), signals),
        "db_create_count": db_creates,
        "mean_db_create_ns": _div(g("TIME_DB_CREATE"), db_creates),
        "db_create_bytes": db_bytes,
        "mean_db_create_bytes": _div(db_bytes, db_creates),
        "cxl_protocol_read_bytes": fetch,
        "cxl_protocol_write_bytes": purge,
        "cxl_protocol_total_bytes": cxl_total,
        "cxl_protocol_read_Bps": _div(fetch, e2e_s),
        "cxl_protocol_write_Bps": _div(purge, e2e_s),
        "cxl_protocol_total_Bps": _div(cxl_total, e2e_s),
        "cxl_flush_producer_count": n_prod,
        "cxl_flush_consumer_count": n_cons,
        "mean_cxl_flush_producer_ns": _div(t_prod, n_prod),
        "mean_cxl_flush_consumer_ns": _div(t_cons, n_cons),
        "cxl_flush_producer_op_Bps": per_ns(purge, t_prod),
        "cxl_flush_consumer_op_Bps": per_ns(fetch, t_cons),
        "network_payload_bytes": payload,
        "network_payload_Bps": _div(payload, e2e_s),
        "db_payload_put_count": n_put,
        "mean_db_payload_put_ns": _div(t_put, n_put),
        "db_payload_put_op_Bps": per_ns(payload, t_put),
        **derive_papi(sums, edts),
    }

RR_DEVICES_RE = re.compile(r"CXL DB allocation: round_robin across (\d+) "
                           r"device\(s\)")

def cxl_devices_reported(text: str) -> list[int]:
    """The device counts the ranks' "round_robin across <k> device(s)" lines
    report (one per rank that reached CXL init; [] under static)."""
    return [int(k) for k in RR_DEVICES_RE.findall(text)]

ARENA_DEVICES_RE = re.compile(r"CXL DB arena devices: ([0-9,]*)")

def cxl_arena_devices_reported(text: str) -> list[list[int]]:
    """Per rank, the device each DB arena actually sits on, in arena order,
    from the "CXL DB arena devices:" line every rank prints after checking
    its arenas."""
    return [[int(d) for d in m.split(",") if d]
            for m in ARENA_DEVICES_RE.findall(text)]

def sanity_checks(cell, stamps, per_rank, sums, complete,
                  rank_events, cxl_devices, arena_devices) -> list[str]:
    """The run-rejection checks of counter_exploration.md that one run can
    evaluate on its own (cross-run checks belong to post-processing)."""
    failed = []
    if len(stamps) > 1:
        failed.append(f"multiple_e2e_markers:{len(stamps)}")
    if not complete:
        failed.append(f"rank_files:{sorted(per_rank)}_of_{cell['nodes']}")
    is_cxl = "cxl" in cell["variant"]
    cxl_bytes = ((sums.get("BYTES_CXL_FLUSH_CONSUMER") or 0)
                 + (sums.get("BYTES_CXL_FLUSH_PRODUCER") or 0))
    if not is_cxl and cxl_bytes:
        failed.append("cxl_bytes_in_network_build")
    # Round-robin capacity is devices x arena: a rank that opened fewer
    # arenas than CXL_DEVICES ran on less CXL than the plan sized for.
    if (is_cxl and CXL_ARTS_CFG.get("cxl_db_allocation_strategy")
            == "round_robin"):
        off = sorted(set(cxl_devices) - {CXL_DEVICES})
        # Every rank prints the line, and the launcher forwards remote ranks'
        # stdout into the cell log.
        if len(cxl_devices) < cell["nodes"]:
            failed.append(f"round_robin_lines:{len(cxl_devices)}_of_"
                          f"{cell['nodes']}")
        elif off:
            failed.append(f"cxl_devices:{off}_not_{CXL_DEVICES}")
        # Asking for CXL_DEVICES arenas is not getting them on CXL_DEVICES
        # devices: a rank whose arenas share a device ran round_robin over
        # fewer devices than the plan assumed.
        if len(arena_devices) < cell["nodes"]:
            failed.append(f"arena_device_lines:{len(arena_devices)}_of_"
                          f"{cell['nodes']}")
        else:
            spread = sorted({len(set(devs)) for devs in arena_devices
                             if len(set(devs)) != CXL_DEVICES})
            if spread:
                failed.append(f"arena_devices_distinct:{spread}_not_"
                              f"{CXL_DEVICES}")
    # A rank that counted some other list (or none: a tree without
    # ARTS_USE_PAPI) would publish its slots under the wrong event names.
    if cell["papi_set"]:
        off = sorted(r for r, ev in rank_events.items()
                     if ev != cell["papi_events"])
        if off:
            failed.append(f"papi_events_mismatch:ranks_{off}")
    return failed

def build_record(cell, status, rc, stamps, wall_s, per_rank, started_at, *,
                 wrapper_done, rank_events, cxl_devices, arena_devices):
    nodes = cell["nodes"]
    complete = sorted(per_rank) == list(range(nodes))
    names = sorted({n for vals in per_rank.values() for n in vals})
    # A cluster sum over a missing rank is not a cluster sum: totals are only
    # published when every rank reported.
    sums = ({n: sum(v.get(n, 0) for v in per_rank.values()) for n in names}
            if complete else {})
    e2e_ns = stamps[0] if stamps else None
    e2e_s = e2e_ns / 1e9 if e2e_ns is not None else None
    checks = sanity_checks(cell, stamps, per_rank, sums, complete,
                           rank_events, cxl_devices, arena_devices)
    if status == "ok" and checks:
        status = "check_failed"
    return {
        **{k: cell[k] for k in ("cell_id", "app", "variant", "papi_set",
                                "papi_events", "executable", "args",
                                "footprint_gb", "predicted_db_bytes_per_node",
                                "nodes", "repeat", "is_warmup")},
        # The measured footprint the sweep is over: datablock bytes created
        # per node (cluster sum / nodes; None without every rank's file).
        "db_create_bytes_per_node": (
            sums["BYTES_DB_CREATE"] / nodes
            if "BYTES_DB_CREATE" in sums else None),
        "cxl_devices_reported": cxl_devices,
        "cxl_arena_devices_reported": arena_devices,
        "hosts": NODES[:nodes],
        "worker_threads": WORKER_THREADS,
        "progress_threads": PROGRESS_THREADS,
        "status": status,
        "return_code": rc,
        "wrapper_done": wrapper_done,
        "started_at": started_at,
        "e2e_ns": e2e_ns,
        "e2e_s": e2e_s,
        "wall_s": round(wall_s, 3),
        "ranks_reported": len(per_rank),
        "counters_complete": complete,
        "missing_expected_counters": [
            c for c in expected_counters(cell["papi_events"])
            if c not in names],
        "checks_failed": checks,
        "counters_cluster_sum": sums,
        "counters_per_rank": {str(r): v for r, v in sorted(per_rank.items())},
        "derived": derive(sums, e2e_s),
    }

# ---------------------------------------------------------------------------
# Output files
# ---------------------------------------------------------------------------

def load_records(path: Path) -> list[dict]:
    if not path.exists():
        return []
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]

def write_tables(run_dir: Path, records: list[dict]):
    """Regenerate the CSV views from runs.jsonl.  A cell that was retried
    keeps only its latest record."""
    latest = {}
    for r in records:
        latest[r["cell_id"]] = r
    rows = list(latest.values())

    counters = sorted({n for r in rows for n in r["counters_cluster_sum"]}
                      | {n for r in rows for v in r["counters_per_rank"].values()
                         for n in v})
    # The papi_* metrics are named after the run's events, so they cannot be
    # fixed columns: they follow DERIVED_COLUMNS in name order.
    papi_derived = sorted({k for r in rows for k in r["derived"]}
                          - set(DERIVED_COLUMNS))
    columns = (BASE_COLUMNS + DERIVED_COLUMNS + papi_derived
               + [f"ctr_{c}" for c in counters])
    tmp = run_dir / "runs.csv.tmp"
    with open(tmp, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=columns)
        w.writeheader()
        for r in rows:
            row = {k: r.get(k) for k in BASE_COLUMNS}
            row["checks_failed"] = ";".join(r["checks_failed"])
            row["cxl_devices_reported"] = ";".join(
                map(str, r.get("cxl_devices_reported") or []))
            row["cxl_arena_devices_reported"] = ";".join(
                ",".join(map(str, devs))
                for devs in r.get("cxl_arena_devices_reported") or [])
            row.update(r["derived"])
            for c in counters:
                row[f"ctr_{c}"] = r["counters_cluster_sum"].get(c)
            w.writerow(row)
    tmp.replace(run_dir / "runs.csv")

    tmp = run_dir / "counters_long.csv.tmp"
    with open(tmp, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["cell_id", "app", "variant", "papi_set", "footprint_gb",
                    "nodes", "repeat", "is_warmup", "rank", "counter",
                    "value"])
        for r in rows:
            for rank, vals in r["counters_per_rank"].items():
                for name, value in sorted(vals.items()):
                    w.writerow([r["cell_id"], r["app"], r["variant"],
                                r.get("papi_set"), r.get("footprint_gb"),
                                r["nodes"], r["repeat"],
                                r["is_warmup"],
                                rank, name, value])
    tmp.replace(run_dir / "counters_long.csv")

def git_info() -> dict:
    def git(*args):
        try:
            return subprocess.run(["git", "-C", str(REPO_ROOT), *args],
                                  capture_output=True, text=True,
                                  timeout=30).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            return None
    return {
        "commit": git("rev-parse", "HEAD"),
        "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "dirty": bool(git("status", "--porcelain", "--untracked-files=no")),
        "submodules": git("submodule", "status"),
    }

def write_manifest(run_dir: Path, cells, args):
    manifest = {
        "created_at": dt.datetime.now().isoformat(timespec="seconds"),
        "launch_host": socket.gethostname(),
        "command": shlex.join(sys.argv),
        "git": git_info(),
        "config": {
            "nodes": NODES,
            "node_counts": args.node_counts,
            "footprints_gb": args.footprints,
            "scaling": "weak",
            "worker_threads": WORKER_THREADS,
            "progress_threads": PROGRESS_THREADS,
            "repeats": args.repeats,
            "warmup_runs": args.warmups,
            # Seconds, flat or {nodes: seconds}.
            "cell_timeout_s": args.timeout,
            "profile": args.profile,
            "variants": args.variants,
            "app_build_dir": str(APP_BUILD_DIR),
            "run_py": str(RUN_PY),
            "run_py_args": RUN_PY_ARGS,
            "run_py_updates_cfg": RUN_PY_UPDATES_CFG,
            "arts_launcher": ARTS_LAUNCHER,
            "arts_ports": ARTS_PORTS,
            "arts_provider": ARTS_PROVIDER,
            "arts_net_interface": ARTS_NET_INTERFACE,
            "arts_pin": ARTS_PIN,
            "arts_route_table_size": ARTS_ROUTE_TABLE_SIZE,
            "counter_capture_interval": COUNTER_CAPTURE_INTERVAL,
            "cxl_arts_cfg": CXL_ARTS_CFG,
            "extra_arts_cfg": EXTRA_ARTS_CFG,
            "extra_env": EXTRA_ENV,
            "cxl_devices": CXL_DEVICES,
            "cxl_device_bytes": CXL_DEVICE_BYTES,
            "cxl_db_arena_size_bytes": CXL_DB_ARENA_SIZE_BYTES,
            "built_cxl_db_arena_size_bytes": cmake_cache_value(
                "ARTS_CXL_DB_ARENA_SIZE_BYTES"),
            "cxl_region_size_bytes": CXL_REGION_SIZE_BYTES,
            "cxl_db_pool_bytes": CXL_DB_POOL_BYTES,
            "counter_config": str(COUNTER_CONFIG),
            "expected_counters": EXPECTED_COUNTERS,
            # {name: events}; null for a campaign without PAPI.
            "papi_sets": dict(args.papi) if args.papi else None,
            "papi_library": papi_library() if args.papi else None,
        },
        # Every cell's input (and the model's per-node prediction) is in
        # "cells"; the sizers' constants are recorded here.
        "apps": [a for a, _ in args.apps],
        "size_model": {
            "stencil_halo_b": STENCIL_HALO_B,
            "miniamr_block_b": MINIAMR_BLOCK_B,
            "miniamr_surface_b": MINIAMR_SURFACE_B,
            "comd_tile_b": COMD_TILE_B,
            "nekbone_task_b": NEKBONE_TASK_B,
            "nekbone_elem_b": NEKBONE_ELEM_B,
            "nekbone_halo_b": NEKBONE_HALO_B,
        },
        "cells": [{k: c[k] for k in ("cell_id", "args", "footprint_gb",
                                     "predicted_db_bytes_per_node")}
                  for c in cells],
        "files": {
            "runs.jsonl": "one JSON record per run (source of truth)",
            "runs.csv": "one row per run; ctr_<NAME> = sum over ranks",
            "counters_long.csv": "per-rank counter values, long format",
        },
    }
    (run_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def input_files(cells) -> list[str]:
    """The repo paths named in the cells' arguments (dataset inputs)."""
    root = str(REPO_ROOT) + "/"
    return sorted({tok for c in cells for tok in c["args"].split()
                   if tok.startswith(root)})

def stage_inputs(cells) -> list[str]:
    """Generate the missing dataset inputs artsrun knows how to make (the
    smithwaterman fixtures are synthesized, not checked in); return the ones
    still missing."""
    missing = [p for p in input_files(cells) if not Path(p).exists()]
    if not missing:
        return []
    sys.path.insert(0, str(REPO_ROOT / "tools" / "artsrun" / "src"))
    from artsrun import fixtures
    for p in missing:
        print(f"staging input: {p}")
    return fixtures.stage(missing)

# The build tree APP_BUILD_DIR (<build>/benchmarks/apps) belongs to.
BUILD_CMAKE_CACHE = APP_BUILD_DIR.parents[1] / "CMakeCache.txt"

def select_build(papi: bool):
    """Point the campaign at the PAPI tree and its counter config.  The rest
    of the script reads these module globals, so a --papi campaign swaps them
    once, before anything is planned against them."""
    global APP_BUILD_DIR, BUILD_CMAKE_CACHE, COUNTER_CONFIG, EXPECTED_COUNTERS
    if not papi:
        return
    APP_BUILD_DIR = PAPI_APP_BUILD_DIR
    BUILD_CMAKE_CACHE = APP_BUILD_DIR.parents[1] / "CMakeCache.txt"
    COUNTER_CONFIG = PAPI_COUNTER_CONFIG
    EXPECTED_COUNTERS = _configured_counters(COUNTER_CONFIG)

def cmake_cache_value(key: str) -> str | None:
    """The value of one entry of the build tree's CMakeCache.txt."""
    try:
        text = BUILD_CMAKE_CACHE.read_text(errors="replace")
    except OSError:
        return None
    m = re.search(rf"^{re.escape(key)}:[A-Z]+=(.*)$", text, re.M)
    return m.group(1).strip() if m else None

def built_counter_config() -> Path | None:
    """The ARTS_COUNTER_CONFIG the binaries were configured with."""
    value = cmake_cache_value("ARTS_COUNTER_CONFIG")
    return Path(value) if value else None

def papi_library() -> str | None:
    """The libpapi the PAPI tree linked (by path, so every node needs it at
    the same path)."""
    value = cmake_cache_value("ARTS_PAPI_LIBRARY")
    return value if value and not value.endswith("-NOTFOUND") else None

def papi_lib_dir() -> str | None:
    lib = papi_library()
    return str(Path(lib).parent) if lib else None

def papi_tool(name: str) -> str:
    """A PAPI utility from the install the tree was built against, or from
    PATH when that prefix has no bin/."""
    prefix = cmake_cache_value("ARTS_PAPI_DIR")
    tool = Path(prefix) / "bin" / name if prefix else None
    return str(tool) if tool and tool.is_file() else name

def _on_host(host: str, argv: list[str], timeout_s: int = 60):
    if host != socket.gethostname():
        argv = ["ssh", "-o", "BatchMode=yes", host, shlex.join(argv)]
    return subprocess.run(argv, capture_output=True, text=True,
                          timeout=timeout_s)

FAILED_ADD_RE = re.compile(r"^Failed adding: (\S+)\s*\nbecause: (.+)$", re.M)

def papi_node_problems(host: str, papi_sets) -> list[str]:
    """Whether one node can count every event set: the same checks ARTS
    fails on at startup, made once per node instead of once per cell.
    papi_command_line adds a set's events to one EventSet, so it refuses a
    name the CPU does not know and a set that does not fit its counters
    together (ARTS never multiplexes)."""
    try:
        r = _on_host(host, ["cat", "/proc/sys/kernel/perf_event_paranoid"])
    except (OSError, subprocess.SubprocessError) as e:
        return [f"{host}: cannot probe PAPI: {e}"]
    try:
        paranoid = int(r.stdout.strip())
    except ValueError:
        return [f"{host}: cannot read perf_event_paranoid: "
                f"{(r.stderr or r.stdout).strip()}"]
    if paranoid > 2:
        return [f"{host}: perf_event_paranoid={paranoid}; PAPI needs <= 2 "
                "(sudo sh -c 'echo 2 > /proc/sys/kernel/perf_event_paranoid')"]
    problems = []
    env = [f"LD_LIBRARY_PATH={papi_lib_dir()}"] if papi_lib_dir() else []
    for name, events in papi_sets:
        argv = ["env", *env, papi_tool("papi_command_line"), *events]
        try:
            r = _on_host(host, argv)
        except (OSError, subprocess.SubprocessError) as e:
            problems.append(f"{host}: papi set {name}: {e}")
            continue
        failed = FAILED_ADD_RE.findall(r.stdout + r.stderr)
        for event, why in failed:
            problems.append(f"{host}: papi set {name}: cannot count {event}: "
                            f"{why.strip()}")
        if r.returncode and not failed:
            out = (r.stderr or r.stdout).strip().splitlines()
            problems.append(f"{host}: papi set {name}: papi_command_line "
                            f"exited {r.returncode}"
                            + (f": {out[-1]}" if out else ""))
    return problems

def cxl_need_bytes(cell) -> float | None:
    """The CXL a cell must find under round_robin: its predicted cluster
    total with CXL_CAPACITY_MARGIN, plus the pool tails every rank may
    strand on every arena.  None for a cell without a prediction."""
    pred = cell["predicted_db_bytes_per_node"]
    if pred is None:
        return None
    return (pred * cell["nodes"] * CXL_CAPACITY_MARGIN
            + CXL_DEVICES * cell["nodes"] * CXL_DB_POOL_BYTES)

def cxl_capacity_problems(cells) -> list[str]:
    """Round-robin capacity is CXL_DEVICES arenas of CXL_DB_ARENA_SIZE_BYTES,
    each on its own device.  An arena that overflows ends the run (no
    fallback to the next), so the check is against the sum, with margin."""
    problems = []
    if CXL_DB_ARENA_SIZE_BYTES > CXL_DEVICE_BYTES:
        problems.append(f"CXL_DB_ARENA_SIZE_BYTES={CXL_DB_ARENA_SIZE_BYTES} "
                        f"exceeds one device ({CXL_DEVICE_BYTES})")
    capacity = CXL_DEVICES * CXL_DB_ARENA_SIZE_BYTES
    over = sorted({(c["app"], c["footprint_gb"], c["nodes"], need)
                   for c in cells if "cxl" in c["variant"]
                   for need in [cxl_need_bytes(c)]
                   if need is not None and need > capacity},
                  key=lambda x: -x[3])
    for app, fp, n, need in over:
        problems.append(f"{app} at {fp} GB x {n} nodes needs ~{need / GB:.0f} "
                        f"GB of CXL; {CXL_DEVICES} arenas hold "
                        f"{capacity / GB:.0f} GB")
    return problems

def preflight(cells, papi_sets=None) -> list[str]:
    problems = [f"input file missing: {p}" for p in input_files(cells)
                if not Path(p).exists()]
    if not RUN_PY.is_file():
        problems.append(f"run.py wrapper not found: {RUN_PY}")
    built = built_counter_config()
    if built is None:
        problems.append(f"no ARTS_COUNTER_CONFIG in {BUILD_CMAKE_CACHE}; "
                        "cannot confirm the counter build")
    elif built.resolve() != COUNTER_CONFIG.resolve():
        problems.append(f"binaries built with counter config {built}, "
                        f"not {COUNTER_CONFIG}")
    # Timings from a Debug tree are not results, and these inputs overflow
    # the default arena: both are fixed at configure time.
    build_type = cmake_cache_value("CMAKE_BUILD_TYPE")
    if build_type != "Release":
        problems.append(f"{BUILD_CMAKE_CACHE} has CMAKE_BUILD_TYPE="
                        f"{build_type}; reconfigure with "
                        "-DCMAKE_BUILD_TYPE=Release")
    if any("cxl" in c["variant"] for c in cells):
        arena = cmake_cache_value("ARTS_CXL_DB_ARENA_SIZE_BYTES")
        if arena is None or not arena.isdigit() \
                or int(arena) < CXL_DB_ARENA_SIZE_BYTES:
            problems.append(f"{BUILD_CMAKE_CACHE} has "
                            f"ARTS_CXL_DB_ARENA_SIZE_BYTES={arena}; these "
                            f"inputs need {CXL_DB_ARENA_SIZE_BYTES} (reconfigure "
                            f"with -DARTS_CXL_DB_ARENA_SIZE_BYTES="
                            f"{CXL_DB_ARENA_SIZE_BYTES})")
        if CXL_REGION_SIZE_BYTES < CXL_DB_ARENA_SIZE_BYTES:
            problems.append(f"CXL_REGION_SIZE_BYTES={CXL_REGION_SIZE_BYTES} "
                            "cannot hold the arena "
                            f"({CXL_DB_ARENA_SIZE_BYTES})")
        # ARTS clamps the pool to arena / ranks without a word, which would
        # hand the widest cells a smaller pool than the manifest records.
        widest = max(c["nodes"] for c in cells)
        if CXL_DB_POOL_BYTES * widest > CXL_DB_ARENA_SIZE_BYTES:
            problems.append(f"CXL_DB_POOL_BYTES={CXL_DB_POOL_BYTES} x "
                            f"{widest} ranks exceeds the arena; ARTS would "
                            "clamp it to arena / ranks")
        problems += cxl_capacity_problems(cells)
    if max(c["nodes"] for c in cells) > len(NODES):
        problems.append(f"{len(NODES)} hosts in NODES, but a cell needs "
                        f"{max(c['nodes'] for c in cells)}")
    missing = sorted({c["executable"] for c in cells
                      if not (APP_BUILD_DIR / c["executable"]).is_file()})
    for exe in missing:
        problems.append(f"executable not found: {APP_BUILD_DIR / exe}")
    if NODES and NODES[0] != socket.gethostname():
        problems.append(f"NODES[0]={NODES[0]} is not this host "
                        f"({socket.gethostname()}); rank 0 runs here")
    if papi_sets:
        use_papi = cmake_cache_value("ARTS_USE_PAPI")
        if use_papi is None or use_papi.upper() not in ("ON", "TRUE", "1",
                                                        "YES"):
            problems.append(f"{BUILD_CMAKE_CACHE} has ARTS_USE_PAPI="
                            f"{use_papi}; --papi needs a tree configured "
                            "with -DARTS_USE_PAPI=ON")
        lib = papi_library()
        if not lib or not Path(lib).is_file():
            problems.append(f"PAPI library not found: {lib}")
        hosts = NODES[:max(c["nodes"] for c in cells)]
        for host in hosts:
            problems += papi_node_problems(host, papi_sets)
    return problems

def estimate(cells, paths: list[Path], timeout):
    """Project the plan's duration from the wall times of earlier runs at the
    same (app, variant, args).  A node count with no run borrows the slowest
    measured node count of that app (strong scaling here is not reliably
    monotone), then the other variant's.  A failed run carries no time; a
    timed-out one counts as its full timeout.  Each cell is capped at this
    plan's timeout for its node count."""
    seen: dict[tuple, list[float]] = {}
    bad: dict[tuple, set[str]] = {}
    for path in paths:
        for r in load_records(path):
            key = (r["app"], r["variant"], r["args"], r["nodes"])
            if r["status"] in ("ok", "check_failed", "no_e2e"):
                seen.setdefault(key, []).append(r["wall_s"])
            elif r["status"] == "timeout":
                seen.setdefault(key, []).append(
                    float(cell_timeout(timeout, r["nodes"])))
                bad.setdefault(key[:3], set()).add("timeout")
            else:
                bad.setdefault(key[:3], set()).add(r["status"])

    def median(xs):
        xs = sorted(xs)
        return xs[len(xs) // 2]

    def cell_time(c):
        key = (c["app"], c["variant"], c["args"], c["nodes"])
        if key in seen:
            return median(seen[key]), "measured"
        for variant in (c["variant"], *(v for v in VARIANTS if v != c["variant"])):
            others = [median(ts) for (a, v, s, _), ts in seen.items()
                      if (a, v, s) == (c["app"], variant, c["args"])]
            if others:
                return max(others), "borrowed"
        return None, "unknown"

    per_app: dict[str, list] = {}
    total, unknown, borrowed = 0.0, 0, 0
    for c in cells:
        t, how = cell_time(c)
        row = per_app.setdefault(c["app"], [0.0, 0, 0])
        if t is None:
            unknown += 1
            row[1] += 1
            continue
        t = min(t, cell_timeout(timeout, c["nodes"]))
        total += t
        row[0] += t
        borrowed += how == "borrowed"
        row[2] += how == "borrowed"

    print(f"\nestimate: {total / 3600:.2f} h over {len(cells) - unknown} cells "
          f"({borrowed} with a borrowed node count); {unknown} cells have no "
          f"time and are not counted")
    print(f"  {'app':<34} {'hours':>6}  notes")
    for app, (t, unk, bor) in sorted(per_app.items(), key=lambda kv: -kv[1][0]):
        notes = []
        if unk:
            notes.append(f"{unk} cells unknown")
        if bor:
            notes.append(f"{bor} borrowed")
        statuses = set().union(*(s for (a, _, _), s in bad.items() if a == app))
        if statuses:
            notes.append("runs " + "/".join(sorted(statuses)))
        print(f"  {app:<34} {t / 3600:6.2f}  {'; '.join(notes)}")

def parse_args():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--apps", help="comma-separated subset of app base names")
    p.add_argument("--nodes", help="comma-separated subset of node counts")
    p.add_argument("--footprints", help="comma-separated subset of per-node "
                   f"footprints, GB (default: {','.join(map(str, FOOTPRINTS_GB))}"
                   "; any positive number is sized by the models)")
    p.add_argument("--variants", help="comma-separated build variants "
                   f"(default: {','.join(VARIANTS)})")
    p.add_argument("--profile", choices=PROFILES, default="main",
                   help="input profile; apps without an input in it are "
                   "skipped.  " + "; ".join(
                       f"{n}: {c['repeats']} repeats, {c['warmups']} warm-ups, "
                       f"timeout {c['timeout']} s" for n, c in PROFILES.items()))
    p.add_argument("--smoke-test", action="store_const", const="smoke",
                   dest="profile", help="same as --profile smoke")
    p.add_argument("--papi", action="append", metavar="SET",
                   help="count PAPI hardware events from the PAPI tree "
                   f"({PAPI_APP_BUILD_DIR.relative_to(REPO_ROOT)}); "
                   "repeatable, each cell runs once per set.  SET is "
                   f"{' / '.join(PAPI_EVENT_SETS)}, NAME=EV1,EV2,... or "
                   f"EV1,EV2,... (at most {PAPI_MAX_EVENTS} events)")
    p.add_argument("--repeats", type=int, help="override the profile's")
    p.add_argument("--warmups", type=int, help="override the profile's")
    p.add_argument("--timeout", type=int,
                   help="per-run timeout, seconds, at every node count "
                   "(overrides the profile's)")
    p.add_argument("--estimate", type=Path, action="append", metavar="RUNS_JSONL",
                   help="with --dry-run, project the plan's duration from the "
                   "wall times in a previous runs.jsonl (repeatable)")
    p.add_argument("--run-id", help="name of the run directory "
                   "(default: a timestamp)")
    p.add_argument("--resume", type=Path, metavar="RUN_DIR",
                   help="continue a campaign; cells already recorded are skipped")
    p.add_argument("--retry-failed", action="store_true",
                   help="with --resume, also rerun cells whose status is not ok")
    p.add_argument("--dry-run", action="store_true",
                   help="print the plan and commands without running anything")
    p.add_argument("--force", action="store_true",
                   help="run despite preflight problems")
    args = p.parse_args()

    profile = PROFILES[args.profile]
    for key in ("repeats", "warmups", "timeout"):
        if getattr(args, key) is None:
            setattr(args, key, profile[key])
    if args.estimate and not args.dry_run:
        p.error("--estimate needs --dry-run")

    inputs = profile["apps"]
    roster = [(a, inputs[a]) for a in APPS if a in inputs]
    if args.apps:
        wanted = args.apps.split(",")
        known = set(APPS)
        unknown = [a for a in wanted if a not in known]
        if unknown:
            p.error(f"unknown app(s): {', '.join(unknown)}")
        no_input = [a for a in wanted if a not in inputs]
        if no_input:
            p.error(f"no {args.profile} input for: {', '.join(no_input)}")
        args.apps = [(a, s) for a, s in roster if a in wanted]
    else:
        args.apps = roster
    args.node_counts = ([int(n) for n in args.nodes.split(",")]
                        if args.nodes else NODE_COUNTS)
    args.variants = args.variants.split(",") if args.variants else VARIANTS
    if args.footprints:
        if profile["footprints"] == [None]:
            p.error(f"the {args.profile} profile has fixed inputs; "
                    "--footprints applies to main")
        try:
            args.footprints = [int(f) if float(f) == int(float(f))
                               else float(f)
                               for f in args.footprints.split(",")]
        except ValueError:
            p.error(f"--footprints {args.footprints}: not numbers")
        if any(f <= 0 for f in args.footprints):
            p.error("--footprints must be positive")
    else:
        args.footprints = profile["footprints"]
    args.papi = parse_papi_sets(args.papi or [], p)
    return args

SET_NAME_RE = re.compile(r"[A-Za-z0-9_]+")

def parse_papi_sets(values: list[str], p) -> list[tuple[str, list[str]]]:
    """--papi values as [(name, events)].  The name goes into cell ids and
    directory names; a bare event list is named custom<k>.  Event names
    cannot hold ',' '#' '=' or blanks (the runtime splits on ',' and the cfg
    line on '#'), so NAME= is never part of an event."""
    sets = []
    for k, value in enumerate(values, 1):
        if value in PAPI_EVENT_SETS:
            name, events = value, PAPI_EVENT_SETS[value]
        elif "=" in value:
            name, _, spec = value.partition("=")
            events = spec.split(",")
        elif "," in value or value.startswith("PAPI_") or ":" in value:
            name, events = f"custom{k}", value.split(",")
        else:
            p.error(f"--papi {value}: not a set name "
                    f"({', '.join(PAPI_EVENT_SETS)}) or an event list")
        events = [e.strip() for e in events]
        if not SET_NAME_RE.fullmatch(name):
            p.error(f"--papi {value}: set name {name!r} must be [A-Za-z0-9_]+")
        bad = [e for e in events if not e or any(c in e for c in ",#= \t")]
        if bad or not events:
            p.error(f"--papi {value}: not event names: {bad}")
        if len(events) > PAPI_MAX_EVENTS:
            p.error(f"--papi {value}: {len(events)} events; a run counts at "
                    f"most {PAPI_MAX_EVENTS}, so split the list across sets")
        if len(set(events)) != len(events):
            p.error(f"--papi {value}: an event is listed twice")
        if any(name == n for n, _ in sets):
            p.error(f"--papi: set {name} given twice")
        sets.append((name, events))
    return sets

def main():
    args = parse_args()
    select_build(bool(args.papi))
    cells = plan_cells(args.apps, args.variants, args.node_counts,
                       args.footprints, args.repeats, args.warmups, args.papi)

    if args.resume:
        run_dir = args.resume.resolve()
        if not (run_dir / "manifest.json").exists():
            sys.exit(f"not a campaign directory: {run_dir}")
        # A resume with other event sets (or without --papi) would plan
        # cell ids the campaign never had and mix trees in one directory.
        recorded = json.loads((run_dir / "manifest.json").read_text())
        recorded = recorded["config"].get("papi_sets")
        if recorded != (dict(args.papi) if args.papi else None):
            sys.exit(f"{run_dir} was planned with papi sets {recorded}; "
                     "resume it with the same --papi arguments")
    else:
        prefix = "" if args.profile == "main" else f"{args.profile}-"
        if args.papi:
            prefix = "papi-" + prefix
        run_id = args.run_id or dt.datetime.now().strftime(
            f"{prefix}%Y%m%d-%H%M%S")
        run_dir = (RESULTS_ROOT / run_id).resolve()

    jsonl = run_dir / "runs.jsonl"
    records = load_records(jsonl)
    done = {r["cell_id"] for r in records
            if not args.retry_failed or r["status"] == "ok"}
    todo = [c for c in cells if c["cell_id"] not in done]

    print(f"campaign: {run_dir}")
    print(f"cells: {len(cells)} planned, {len(cells) - len(todo)} already "
          f"recorded, {len(todo)} to run")

    if args.dry_run:
        for c in ([] if args.estimate else todo):
            spec = {"app": str(APP_BUILD_DIR / c["executable"]), "args": c["args"]}
            print(f"\n[{c['cell_id']}] hosts={','.join(NODES[:c['nodes']])} "
                  f"timeout={cell_timeout(args.timeout, c['nodes'])}s")
            print(f"  args.json: {json.dumps(spec)}")
            if c["predicted_db_bytes_per_node"] is not None:
                need = cxl_need_bytes(c) if "cxl" in c["variant"] else None
                print(f"  predicted DB/node "
                      f"{c['predicted_db_bytes_per_node'] / GB:.2f} GB"
                      + (f"; CXL need ~{need / GB:.0f} GB" if need else ""))
            if c["papi_events"]:
                print(f"  papi_events={','.join(c['papi_events'])}")
            print(f"  {shlex.join(launch_argv(Path('<cell>/args.json')))}")
        for msg in preflight(cells, args.papi):
            print(f"preflight: {msg}")
        if args.estimate:
            estimate(todo, args.estimate, args.timeout)
        return

    stage_inputs(cells)
    problems = preflight(cells, args.papi)
    for msg in problems:
        print(f"preflight: {msg}", file=sys.stderr)
    if problems and not args.force:
        sys.exit("preflight failed (use --force to run anyway)")

    run_dir.mkdir(parents=True, exist_ok=True)
    if not args.resume:
        write_manifest(run_dir, cells, args)

    for i, cell in enumerate(todo, 1):
        print(f"[{i}/{len(todo)}] {cell['cell_id']} ... ", end="", flush=True)
        record = run_cell(cell, run_dir,
                          cell_timeout(args.timeout, cell["nodes"]))
        with open(jsonl, "a") as f:
            f.write(json.dumps(record) + "\n")
        records.append(record)
        write_tables(run_dir, records)
        e2e = f"{record['e2e_s']:.3f}s" if record["e2e_s"] is not None else "-"
        extra = f" {';'.join(record['checks_failed'])}" if record["checks_failed"] else ""
        per_node = record["db_create_bytes_per_node"]
        pred = record["predicted_db_bytes_per_node"]
        db = (f" db/node={per_node / GB:.2f}GB"
              + (f" (model {pred / GB:.2f})" if pred else "")
              if per_node is not None else "")
        print(f"{record['status']} e2e={e2e} wall={record['wall_s']:.1f}s "
              f"ranks={record['ranks_reported']}/{cell['nodes']}{db}{extra}",
              flush=True)
        if record["missing_expected_counters"] and record["counters_complete"]:
            print(f"    missing counters: "
                  f"{','.join(record['missing_expected_counters'])}")

    statuses = {}
    for r in {r["cell_id"]: r for r in records}.values():
        statuses[r["status"]] = statuses.get(r["status"], 0) + 1
    print(f"done: {statuses}; results in {run_dir}")

if __name__ == "__main__":
    main()
