#!/usr/bin/env python3
"""Strong-scaling campaign for CXL-DIRECT ARTS, with node-level counters.

The roster of run_cxl_experiments.py cut to one placement variant per app
family (the one that strong-scales; see the comment above APPS), with inputs
sized so each is balanced at 1-4 nodes.  mapreduce_wordcount runs a fixed
total of map tiles via a {per_node:M} input placeholder, so it strong-scales
instead of weak-scaling.

Every (application, node count, repetition) cell runs the same input on 1..4
nodes, one ARTS rank per node.  Each cell is launched through the site's
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
set's events are written into that cell's arts.cfg as papi_events.  A set is
a name from PAPI_EVENT_SETS, NAME=EV1,EV2,... or a bare EV1,EV2,... (at most
six events; they must fit the core's counters without multiplexing).
Preflight checks the tree's cache for ARTS_USE_PAPI and ssh-es to every node
to confirm it can count each set (perf_event_paranoid <= 2, papi_command_line
adds every event).  An instrumented tree's e2e times are not results: take
timings from the plain campaign.

Output (under RESULTS_ROOT/<run-id>/):

  manifest.json        the constants, git revision, and roster of this campaign
  runs.jsonl           one complete JSON record per run (source of truth;
                       appended as each run finishes, so it survives a crash)
  runs.csv             one flat row per run: fixed columns, derived metrics,
                       and one ctr_<NAME> column per counter (cluster sum)
  counters_long.csv    tidy per-rank counters: cell_id, rank, counter, value
  cells/<cell_id>/     args.json, arts.cfg, output.log, counters/n*.json

Usage:

  python3 experiment_scripts/run_cxl_experiments_optimal.py --dry-run
  python3 experiment_scripts/run_cxl_experiments_optimal.py
  python3 experiment_scripts/run_cxl_experiments_optimal.py --apps fft_dist,stream_dist --nodes 1,2
  python3 experiment_scripts/run_cxl_experiments_optimal.py --resume <run-dir>
  python3 experiment_scripts/run_cxl_experiments_optimal.py --smoke-test
  python3 experiment_scripts/run_cxl_experiments_optimal.py --papi cache --dry-run
  python3 experiment_scripts/run_cxl_experiments_optimal.py --papi cache --papi memstall_skx
  python3 experiment_scripts/run_cxl_experiments_optimal.py \\
      --papi spr=PAPI_TOT_CYC,PAPI_TOT_INS,CYCLE_ACTIVITY:STALLS_L3_MISS
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
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

# Node counts of the strong-scaling sweep.
NODE_COUNTS = [1, 2, 3, 4]

# Threads per rank (per node).  Kept identical at every node count.
WORKER_THREADS = 92
PROGRESS_THREADS = 4

# Measured repetitions per cell, plus unrecorded-in-analysis warm-up runs per
# (app, node count).  Warm-up runs are still logged, flagged is_warmup=true.
REPEATS = 5
WARMUP_RUNS = 1

# Per-run wall-clock budget, seconds.  A run past it is killed and recorded as
# status "timeout".
CELL_TIMEOUT_S = 1800

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

# The site launch wrapper and the arguments it receives besides
# --path-to-json.  "--local" runs the binary on this host and lets ARTS
# remote-launch its own ranks from the configuration.
RUN_PY = REPO_ROOT / "script" / "run.py"
RUN_PY_ARGS = ["--local", "--region-size", str(10 * 1024**3)]
# Hand the rendered arts.cfg to run.py's update_cfg_for_allocation()
# ("arts_cfg" in the JSON).  Leave False unless that helper preserves the
# per-cell node list: this script already writes the exact hosts and count.
RUN_PY_UPDATES_CFG = False

# Results live here.  It MUST be on a filesystem every node shares: remote
# ranks cd into the cell directory and write their counter files into it.
RESULTS_ROOT = REPO_ROOT / "results" / "cxl_strong_scaling_optimal"

# ARTS runtime configuration written into each cell's arts.cfg.
ARTS_LAUNCHER = "ssh"          # ARTS remote-launches ranks onto NODES over ssh
ARTS_PORTS = [25000]           # base port(s); length must equal port count
ARTS_PROVIDER = "tcp"          # e.g. "tcp", "verbs;ofi_rxm", "cxi"
ARTS_NET_INTERFACE = None      # e.g. "ib0"; None lets the runtime choose
ARTS_PIN = True
ARTS_ROUTE_TABLE_SIZE = 16
COUNTER_CAPTURE_INTERVAL = 250  # irrelevant to ONCE counters, must be > 0
# Any further [ARTS] keys, e.g. {"cxl_db_allocation_strategy": "interleaved"}.
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

# Applications (base names, without the _<variant> suffix) and their inputs.
# "{repo}" is replaced with REPO_ROOT.  Inputs are fixed across node counts
# (strong scaling); "{per_node:M}" is replaced with M / nodes for apps whose
# command line takes a per-rank share of a fixed total (see expand_args).
#
# One placement variant per family: the one whose source keeps work and data
# on the rank that uses them, so it both runs fastest and strong-scales.  The
# variants left out lose for structural reasons:
#   * no hint: EDTs are dealt round-robin while DBs stay on their creator, so
#     nearly every edge crosses nodes (every base variant below);
#   * mainEdt on rank 0 builds the whole graph and homes every event there,
#     hints or not (cholesky_blas*, smithwaterman base/hinted);
#   * one RW datablock shared by every task (fft, fft_hinted), or a fresh DB
#     per kernel or exchange (stream, stream_hinted, hpcg_intel);
#   * too little parallelism for any placement (LCS_all_db_distributed's
#     quadrant recursion, ~7.5 at N/base = 128);
#   * hint layer computes the base map for these geometries
#     (miniAMR_intel_chandra_hinted), or no locality at all (npb_cg).
APPS: list[tuple[str, str]] = [
    ("cholesky_dist", "--ds 22400 --ts 100 --places 144"),
    ("CoMD_intel_chandra_tiled_hinted", "-x 360 -y 180 -z 180 -N 100 -n 10 -i 24 -j 12 -k 12"),
    ("fft_dist", "28 768 24"),
    ("fibonacci_hinted", "40 14"),
    ("graph500_dist", "20 16 128 128"),
    ("hpcg_intel_dist", "24 12 12 32 50"),
    ("hpgmg_dist", "4 13824"),
    ("LCS_wavefront", "1769472 256"),
    ("miniAMR_intel_dist",
     "--nx 10 --ny 10 --nz 10 --npx 48 --npy 24 --npz 18 --num_refine 1 "
     "--num_tsteps 20"),
    ("nekbone_dist", "18 16 12 4 4 4 8 100"),
    ("npb_cg_dist", "-t B -c 188"),
    ("nqueens_hinted", "20 16 1 4"),
    ("RSBench_intel_sharedDB_hinted", "-s large -l 2000000 -t 216 -p 24"),
    # Strong scaling: M = 384 map tiles in total at every node count.
    ("mapreduce_wordcount", "16384 333334 {per_node:384} 8 100 0 4 1"),
    ("smithwaterman_dist",
     "3 3 {repo}/datasets/smithwaterman/string1-gate.txt "
     "{repo}/datasets/smithwaterman/string2-gate.txt "
     "{repo}/datasets/smithwaterman/score-gate.txt 24"),
    ("Stencil2D_intel_channelEVTs", "51840 13824 100"),
    ("stream_dist", "233280000 3456 1000"),
    ("triangle_hinted", "8 1 8"),
    ("XSBench_intel_sharedDB", "-s large -g 96 -l 176000000 -t 216 -p 32"),
]

# --smoke-test inputs: each app of APPS shrunk to a correctness smoke that
# finishes well inside 30 s at every node count (1..4), for checking the campaign
# end to end before committing to the real matrix.  Nothing timed here is a
# result.  Vectors are experiments/experiments/smoke.yaml's (which keeps the
# per-row legality notes) where it has one.
SMOKE_APPS: dict[str, str] = {
    "cholesky_dist": "--ds 40 --ts 10 --places 8",
    "CoMD_intel_chandra_tiled_hinted": "-x 16 -y 8 -z 8 -N 2 -n 1 -i 4 -j 2 -k 2",
    "fft_dist": "8 4 2",
    "fibonacci_hinted": "15 5",
    "graph500_dist": "8 16 4 4",
    "hpcg_intel_dist": "2 2 2 16 5",
    "hpgmg_dist": "4 8",
    "LCS_wavefront": "1024 64",
    "miniAMR_intel_dist": ("--nx 4 --ny 4 --nz 4 --npx 2 --npy 2 --npz 2 "
                           "--num_refine 1 --num_tsteps 2"),
    "nekbone_dist": "2 2 2 2 2 2 8 2",
    "npb_cg_dist": "-t S -c 16",
    "nqueens_hinted": "8 4 1 2",
    "RSBench_intel_sharedDB_hinted": "-s large -l 300 -t 2 -p 8",
    "mapreduce_wordcount": "1024 10000 {per_node:12} 4 100 0 2 1",
    # 12 places, not smoke.yaml's 8: the app reserves event names for
    # rows/ranks + 2 rows per rank, so a place count the rank count does not
    # divide (8 at 3 ranks gives rank 0 251 rows against 224) aliases two
    # tiles' events and the wavefront hangs.
    "smithwaterman_dist": (
        "3 3 {repo}/datasets/smithwaterman/string1-gate.txt "
        "{repo}/datasets/smithwaterman/string2-gate.txt "
        "{repo}/datasets/smithwaterman/score-gate.txt 12"),
    "Stencil2D_intel_channelEVTs": "12 4 2",
    "stream_dist": "400 4 2",
    "triangle_hinted": "3 1",
    "XSBench_intel_sharedDB": "-s large -g 8 -l 300 -t 2 -p 8",
}

# --profile medium inputs: the whole campaign (3 repeats, no warm-up) in a
# working day on the testbed.  Sizing rules, per app:
#   * 1-node e2e ~30-60 s on the CXL build, inside the 300 s timeout at every
#     node count;
#   * datablock bytes ALLOCATED over the run (the CXL arena reclaims nothing
#     within a run) at most ~7 GB of the 15 GB arena;
#   * every rank-following knob (places, instances, tiles, map tiles) divides
#     evenly over 1, 2, 3 and 4 nodes.
# Vectors marked "calibrate" are estimated from the source and the 20261003
# medium run, not measured: run them at 1 and 4 nodes first (see --estimate).
_MEDIUM_SW = ("{repo}/datasets/smithwaterman/string1-gate.txt "
              "{repo}/datasets/smithwaterman/string2-gate.txt "
              "{repo}/datasets/smithwaterman/score-gate.txt")
MEDIUM_APPS: dict[str, str] = {
    # Same matrix as cholesky_blas (t = 96 tiles per side).  144 places form a
    # 12x12 grid that stays block-cyclic over 1, 2x1, 3x1 and 2x2 rank grids;
    # the default 32 (8x4) splits 3/3/2 at 3 nodes.  Calibrate.
    "cholesky_dist": "--ds 9600 --ts 100 --places 144",
    # 768 subdomains (12x8x8), balanced at 1-4 nodes.  Time is halo-DB flushes,
    # ~2.8 s per step on CXL, so 20 steps (~56 s) instead of 100 (~280 s).
    "CoMD_intel_chandra_tiled_hinted": "-x 108 -y 72 -z 72 -N 20 -n 10 -i 12 -j 8 -k 8",
    # Power 27 was flat at ~3 s (fixed costs); 28 is N1 = N2 = 16384,
    # ~8.6 GB allocated -- over the 7 GB guideline, so fall back to
    # "27 768 24" if the arena runs out.  24 places divide 1-4.  Calibrate.
    "fft_dist": "28 768 24",
    # 22 s at 1 node; slows with nodes in the 20261003 run and timed out at 4
    # although almost nothing crosses ranks: watch it.
    "fibonacci_hinted": "29 14",
    # Scale 21: ~2x scale 20's 23 s, ~4.8 GB.  Calibrate.
    "graph500_dist": "21 16 128 128",
    # m = 32 x 50 iterations timed out at every node count.  The bisection
    # gives node 0 half the tiles at 3 nodes (1.5x) for any tile grid.
    # Calibrate.
    "hpcg_intel_dist": "8 6 4 16 10",
    # 12^3 boxes of 16^3, balanced at 1-4 nodes ("5 1000" splits 4/3/3 at 3).
    # Calibrate.
    "hpgmg_dist": "4 1728",
    # L = N/base = 512 band rows, ~4.3 GB.  Fallback "786432 2048".  Calibrate.
    "LCS_wavefront": "1048576 2048",
    # 12x8x8 = 768 blocks (--npx counts blocks here); no --init_* or
    # --max_blocks in this tier.  Never run in a campaign: validate, calibrate.
    "miniAMR_intel_dist": (
        "--npx 12 --npy 8 --npz 8 --nx 16 --ny 16 --nz 16 --num_vars 40 "
        "--num_tsteps 10 --stages_per_ts 10 --num_refine 0 --checksum_freq 10 "
        "--report_diffusion 1"),
    # 768 ranks x 8 elements; 60 CG iterations, ~40 s at 1 node estimated.
    "nekbone_dist": "12 8 8 2 2 2 8 60",
    # Class B timed out on CXL; class A verifies at its default 15 iterations.
    "npb_cg_dist": "-t A -c 188",
    # Scatter depth 4: boards with <= 3 queens are hashed over ranks, deeper
    # ones stay with their parent (at 6 every task is hashed, as in base).
    "nqueens_hinted": "19 15 1 4",
    # 24 instances: balanced at 1-4 nodes and lanes stay with their instance.
    "RSBench_intel_sharedDB_hinted": "-s large -l 1728000 -t 216 -p 24",
    # Strong scaling: M = 1128 map tiles in total at every node count (TPN =
    # 1128/nodes); 1128 x 2.67M words is the old 1-node 188 x 16M, ~30 s.
    "mapreduce_wordcount": "16384 2670000 {per_node:1128} 8 100 0 16 1",
    # Gate pair (2000 x 2000); 24 places (row bands) divide 1-4.  No larger
    # fixture fits the arena.
    "smithwaterman_dist": f"3 3 {_MEDIUM_SW} 24",
    # NR = 1152 tiles (32x36) divide the 1x1, 1x2, 1x3 and 2x2 node grids.
    # Calibrate NT.
    "Stencil2D_intel_channelEVTs": "13824 1152 200",
    # 67,500 doubles per chain (a bandwidth test, not a task-rate one); three
    # persistent DBs per chain, ~5.6 GB.  3456 chains divide 1-4.
    "stream_dist": "233280000 3456 100",
    # 1.37M tasks, ~13 s at 1 node estimated.  Never run in a campaign.
    "triangle_hinted": "7 1 8",
    # 24 instances divide 1-4.  Calibrate -l.
    "XSBench_intel_sharedDB": "-s large -g 48 -l 25000000 -t 64 -p 24",
}

# An input profile: which inputs, and the repeats / warm-ups / timeout it
# runs with unless the command line says otherwise.  An app with no input in
# a profile is not part of that profile.
PROFILES: dict[str, dict] = {
    "main": {"apps": dict(APPS), "repeats": REPEATS,
             "warmups": WARMUP_RUNS, "timeout": CELL_TIMEOUT_S},
    "medium": {"apps": MEDIUM_APPS, "repeats": 3, "warmups": 0,
               "timeout": 300},
    # A smoke is one pass, no warm-up, and a short leash.
    "smoke": {"apps": SMOKE_APPS, "repeats": 1, "warmups": 0,
              "timeout": 120},
}

# =============================================================================
# End of user configuration
# =============================================================================

E2E_RE = re.compile(r"^\[E2E\]\s+(\d+)\s*$", re.M)
DONE_RE = re.compile(r"^DONE!\s+(-?\d+)", re.M)

# Fixed leading columns of runs.csv; derived metrics and ctr_* follow.
BASE_COLUMNS = [
    "cell_id", "app", "variant", "papi_set", "executable", "nodes",
    "worker_threads",
    "progress_threads", "repeat", "is_warmup", "status", "return_code",
    "e2e_ns", "e2e_s", "wall_s", "ranks_reported", "counters_complete",
    "checks_failed", "started_at",
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

def plan_cells(apps, variants, node_counts, repeats, warmups, papi_sets=None):
    """Warm-ups first per (app, variant, event set, nodes), then measured
    repetitions repeat-major, so slow drift over the campaign spreads across
    all cells instead of landing on whichever app happens to run last.
    papi_sets is [(name, events)] for a --papi campaign, None otherwise."""
    sets = papi_sets or [None]
    cells = []
    for variant in variants:
        for papi in sets:
            for app, args in apps:
                for n in node_counts:
                    for w in range(warmups):
                        cells.append(_cell(app, args, variant, n, w,
                                           warmup=True, papi=papi))
    for rep in range(repeats):
        for variant in variants:
            for papi in sets:
                for app, args in apps:
                    for n in node_counts:
                        cells.append(_cell(app, args, variant, n, rep,
                                           warmup=False, papi=papi))
    return cells

PER_NODE_RE = re.compile(r"\{per_node:(\d+)\}")

def per_node_conflicts(args: str, node_counts) -> list[str]:
    """The {per_node:M} totals of args that some node count does not divide."""
    return [f"{{per_node:{m}}} at {n} nodes"
            for m in map(int, PER_NODE_RE.findall(args))
            for n in node_counts if m % n]

def expand_args(args: str, nodes: int) -> str:
    """An input template as one cell runs it: {repo} becomes REPO_ROOT and
    {per_node:M} becomes M / nodes, the per-rank share of a fixed total.
    That turns an app that sizes its work as share x ranks (mapreduce's map
    tiles) into a strong-scaling cell; an uneven share is an error, never a
    silent imbalance."""
    if per_node_conflicts(args, [nodes]):
        raise ValueError(f"{args!r}: a per_node total is not divisible by "
                         f"{nodes} nodes")
    args = PER_NODE_RE.sub(lambda m: str(int(m.group(1)) // nodes), args)
    return args.replace("{repo}", str(REPO_ROOT))

def _cell(app, args, variant, nodes, index, *, warmup, papi=None):
    tag = f"w{index}" if warmup else f"r{index}"
    # The event set is part of the cell's identity: two sets of one
    # (app, variant, nodes, repeat) are two runs, not a retry.
    papi_set, papi_events = papi if papi else (None, None)
    set_tag = f"__{papi_set}" if papi_set else ""
    return {
        "cell_id": f"{app}__{variant}__n{nodes}{set_tag}__{tag}",
        "app": app,
        "variant": variant,
        "papi_set": papi_set,
        "papi_events": papi_events,
        "executable": f"{app}_{variant}",
        "args": expand_args(args, nodes),
        "nodes": nodes,
        "repeat": index,
        "is_warmup": warmup,
    }

# ---------------------------------------------------------------------------
# Per-cell files
# ---------------------------------------------------------------------------

def render_arts_cfg(nodes: int, counter_folder: Path,
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
                                   cell["papi_events"]))

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
                          rank_events=rank_events)
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

def sanity_checks(cell, stamps, per_rank, sums, complete,
                  rank_events) -> list[str]:
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
    # A rank that counted some other list (or none: a tree without
    # ARTS_USE_PAPI) would publish its slots under the wrong event names.
    if cell["papi_set"]:
        off = sorted(r for r, ev in rank_events.items()
                     if ev != cell["papi_events"])
        if off:
            failed.append(f"papi_events_mismatch:ranks_{off}")
    return failed

def build_record(cell, status, rc, stamps, wall_s, per_rank, started_at, *,
                 wrapper_done, rank_events):
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
                           rank_events)
    if status == "ok" and checks:
        status = "check_failed"
    return {
        **{k: cell[k] for k in ("cell_id", "app", "variant", "papi_set",
                                "papi_events", "executable", "args", "nodes",
                                "repeat", "is_warmup")},
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
            row.update(r["derived"])
            for c in counters:
                row[f"ctr_{c}"] = r["counters_cluster_sum"].get(c)
            w.writerow(row)
    tmp.replace(run_dir / "runs.csv")

    tmp = run_dir / "counters_long.csv.tmp"
    with open(tmp, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["cell_id", "app", "variant", "papi_set", "nodes",
                    "repeat", "is_warmup", "rank", "counter", "value"])
        for r in rows:
            for rank, vals in r["counters_per_rank"].items():
                for name, value in sorted(vals.items()):
                    w.writerow([r["cell_id"], r["app"], r["variant"],
                                r.get("papi_set"), r["nodes"], r["repeat"],
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
            "worker_threads": WORKER_THREADS,
            "progress_threads": PROGRESS_THREADS,
            "repeats": args.repeats,
            "warmup_runs": args.warmups,
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
            "extra_arts_cfg": EXTRA_ARTS_CFG,
            "extra_env": EXTRA_ENV,
            "counter_config": str(COUNTER_CONFIG),
            "expected_counters": EXPECTED_COUNTERS,
            # {name: events}; null for a campaign without PAPI.
            "papi_sets": dict(args.papi) if args.papi else None,
            "papi_library": papi_library() if args.papi else None,
        },
        "apps": [{"app": a, "args": s.replace("{repo}", str(REPO_ROOT))}
                 for a, s in args.apps],
        "cells": [c["cell_id"] for c in cells],
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

def estimate(cells, paths: list[Path], timeout_s: int):
    """Project the plan's duration from the wall times of earlier runs at the
    same (app, variant, args).  A node count with no run borrows the slowest
    measured node count of that app (strong scaling here is not reliably
    monotone), then the other variant's.  A failed run carries no time; a
    timed-out one counts as the full timeout.  Each cell is capped at this
    plan's timeout."""
    seen: dict[tuple, list[float]] = {}
    bad: dict[tuple, set[str]] = {}
    for path in paths:
        for r in load_records(path):
            key = (r["app"], r["variant"], r["args"], r["nodes"])
            if r["status"] in ("ok", "check_failed", "no_e2e"):
                seen.setdefault(key, []).append(r["wall_s"])
            elif r["status"] == "timeout":
                seen.setdefault(key, []).append(float(timeout_s))
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
        t = min(t, timeout_s)
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
    p.add_argument("--variants", help="comma-separated build variants "
                   f"(default: {','.join(VARIANTS)})")
    p.add_argument("--profile", choices=PROFILES, default="main",
                   help="input profile; apps without an input in it are "
                   "skipped.  " + "; ".join(
                       f"{n}: {c['repeats']} repeats, {c['warmups']} warm-ups, "
                       f"{c['timeout']}s timeout" for n, c in PROFILES.items()))
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
                   help="per-run timeout, seconds (overrides the profile's)")
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
    roster = [(a, inputs[a]) for a, _ in APPS if a in inputs]
    if args.apps:
        wanted = args.apps.split(",")
        known = {a for a, _ in APPS}
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
    uneven = [f"{a}: {c}" for a, s in args.apps
              for c in per_node_conflicts(s, args.node_counts)]
    if uneven:
        p.error("per-node share not even: " + "; ".join(uneven))
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
                       args.repeats, args.warmups, args.papi)

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
            print(f"\n[{c['cell_id']}] hosts={','.join(NODES[:c['nodes']])}")
            print(f"  args.json: {json.dumps(spec)}")
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
        record = run_cell(cell, run_dir, args.timeout)
        with open(jsonl, "a") as f:
            f.write(json.dumps(record) + "\n")
        records.append(record)
        write_tables(run_dir, records)
        e2e = f"{record['e2e_s']:.3f}s" if record["e2e_s"] is not None else "-"
        extra = f" {';'.join(record['checks_failed'])}" if record["checks_failed"] else ""
        print(f"{record['status']} e2e={e2e} wall={record['wall_s']:.1f}s "
              f"ranks={record['ranks_reported']}/{cell['nodes']}{extra}",
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
