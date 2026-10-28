# Fixed-iteration apps sized to ~100 GB, quitting early

Hand-off notes for the input-sizing sweep of 2026-10-28. Read this before
touching these inputs or turning them into a campaign profile.

## Intent

We want CXL experiment runs that put a **large memory footprint (~100 GB)** in
the CXL arena but **finish quickly (<= 5 min e2e)**.  This stresses capacity
and placement without paying for long compute.  The way to get there is to
take the apps whose work is an outer loop with a fixed count, size the problem
up, and cut the iteration count down.

"Footprint" here means **total datablock bytes created over the run**
(`BYTES_DB_CREATE`), not peak RSS.  A CXL-DIRECT arena reclaims nothing within
a run, so this total is what the arena has to hold.  The user chose this
definition.  Peak DRAM RSS on a non-CXL build is recorded alongside, but it is
not the target.

## Which apps, and why these seven

All 19 apps in `run_cxl_experiments_optimal.py`'s `APPS` were read and
classified:

- **Fixed iteration count: 7 apps.**  CoMD, miniAMR, nekbone, npb_cg,
  Stencil2D, stream and mapreduce_wordcount.  These are the ones used here.
- **Converging solver: 1 app.**  hpcg_intel_dist runs CG until
  `rtr/rtr0 < 1e-13` or maxIter (arg 5).
- **Borderline: 2 apps.**  hpgmg_dist runs one fixed F-cycle.  graph500_dist
  iterates BFS levels until the frontier is empty.
- **Non-iterative: 9 apps.**  cholesky, fft, fibonacci, LCS_wavefront, nqueens,
  RSBench, XSBench, smithwaterman and triangle.

hpcg and CoMD were also noted as cuttable from the CLI.  XSBench and RSBench
can decouple memory from runtime through their table-size and lookup flags.
Neither of those was swept.

## Inputs

All measurements: 1 rank on this host (2x Xeon Gold 6148, 80 cores, 503 GB
DRAM, no CXL), 76 workers + 4 progress threads, `launcher=local`.  Binary is
`<app>_arts_ocr_excl_purge` from the Release tree `build_sweep/`.

Column meanings:

- **DB GB** is `BYTES_DB_CREATE`.
- **RSS GB** is the peak resident set of the process tree.
- **e2e** is the `[E2E]` stamp (`ARTS_E2E_MARKER=1`).
- **Bold** marks the recommended setting.

| App | Input | DB GB | RSS GB | Measured e2e |
|---|---|---|---|---|
| Stencil2D_intel_channelEVTs | `79000 1152 NT` | 100.4 | 105 | NT=2: 67 s · 10: 71 s · **50: 154 s** · 100: 297 s · 200: timeout |
| stream_dist | `4147200000 3456 N` | 99.5 | 133 | N=2: 52 s · 10: 80 s · 40: 196 s · **60: 271 s** |
| CoMD_intel_chandra_tiled_hinted | `-x 432 -y 216 -z 216 -N N -n 10 -i 24 -j 12 -k 12` | 93.1 | 133 | N=2: 58 s · 20: 116 s · **60: 250 s** |
| miniAMR_intel_dist | `--nx 10 --ny 10 --nz 10 --npx 48 --npy 48 --npz 42 --num_refine 1 --num_tsteps T` | 96.2 (T=1) → 99.8 (T=5) | 133 | T=1: 94 s · 3: 182 s · **5: 273 s** |
| nekbone_dist | `18 16 12 4 4 4 8 20` | 100.4 | 21 | 33 s |
| mapreduce_wordcount | `4560000 333334 {per_node:384} 8 100 0 6 1` | 100.0 | 107 | 60 s |
| npb_cg_dist | `-t D -c 188 -i 1` | 9.8 | 13 | 24 s |
| npb_cg_dist | **`-t E -c 188 -i 1`** | 86.4 | 90 | 363 s (over budget, kept on purpose) |

### Per-app models

These are fitted from the runs above.  Use them to re-target a different
footprint or time.

- **Stencil2D**
  - Memory is all up front: DB ≈ 2·NP²·8 B.  NR=1152 tiles.
  - Setup takes ~65 s, then ~2–3 s per step (per-step cost rose with NT).
  - Keep NT ≤ ~90.
  - Validation scales with NT, so any NT passes.
- **stream**
  - Memory is all up front: DB = 3 × total_elems × 8 B.  total_elems must
    divide by the chain count (3456).
  - Setup takes ~45 s, then ~3.8 s per iteration.
  - Use N ≥ 2: with N=1 there's no timing output.
- **CoMD**
  - Memory is up front; the arrays are sized by box count.
  - Setup takes ~50 s, then ~3.2 s per step.
  - The campaign's main input (`-x 360 -y 180 -z 180`) gives only 62 GB.
- **miniAMR**
  - About 1.0 GB per 1000 blocks at nx=10 and num_vars=40.
  - Small per-timestep growth (~0.9 GB/timestep) comes from checksum and
    refine blocks.
  - Each timestep takes ~45 s (20 stages by default).
- **nekbone**
  - The footprint *is* the iteration count: DB ≈ 9.1 GB + 4.57 GB × CGcount
    at this size, ~1.25 s per iteration.
  - Fresh vector datablocks are created every CG iteration.  On DRAM they're
    freed, which is why RSS stays at 21 GB.  On CXL they accumulate.
  - To change the iteration count at a fixed ~100 GB, change the element
    block or pDOF.
- **mapreduce**
  - DB = V × 8 B × 457 blocks per round × ITER.  That holds for M=384 total
    map tiles and K=8: 384 maps + 73 combines.
  - About 9 s per round at V≈4M.
  - For R rounds at 100 GB: V ≈ 100e9 / (3656·R).
  - The per-round check is exact, so any ITER passes.
- **npb_cg**
  - Both runs above were single-rank.  On one rank there are no per-spmv
    fragment blocks, so DB is independent of `-i`.
  - Multi-rank runs add ≈ (R−1)·nloc·8 B per spmv.
  - `-i 1` is the minimum: it still runs a warm-up pass plus one pass of 25
    inner iterations.
  - Any `-i` below the class's niter prints `Verification FAILED`, though
    the run shuts down cleanly.
  - Class E builds 6.3e9 nonzeros on each rank.  Matrix generation draws the
    whole generator stream on every rank, so expect it to get no faster with
    more nodes.
  - E is the only class near 100 GB.  It is ~1 min over the 5-min budget,
    and the user chose to keep it anyway.
  - Bringing it under 5 min needs a source change: a custom class between
    D and E, or skipping the warm-up pass.

## Caveats when moving to the CXL testbed

- These are single-node, DRAM-only, 76-worker numbers.  The testbed has 92
  workers per node, slower CXL memory, and 1–4 nodes.  Prefer mid-range
  iteration counts over the top of each range.
- Total DB bytes should not depend on node count for Stencil2D, stream, CoMD
  and miniAMR, nor for mapreduce (`{per_node:384}` keeps M=384).  For nekbone
  the halo and reduction blocks may shift a little with rank count.  For
  npb_cg it grows with the rank count.
- The arena must hold the DB total plus slack.  `script/run.py` currently
  creates regions with `--region-size` 10 GiB (`RUN_PY_ARGS`), which is far
  too small for these inputs.  Raise it and `ARTS_CXL_DB_ARENA_SIZE_BYTES`
  (5e9 in `build/`) before running.
- Network-ARTS (DRAM) variants peak at 105–133 GB RSS per rank-host for the
  up-front apps.
- `build/` (the tree `run_cxl_experiments_optimal.py` uses through
  `APP_BUILD_DIR`) is a **Debug** build with counters off.  Use a Release tree
  for timed runs.

## Reproducing

```bash
# Release tree with BYTES_DB_CREATE, only the 7 binaries needed
cmake -GNinja -Bbuild_sweep -DCMAKE_BUILD_TYPE=Release \
  -DARTS_COHERENCE_PROTOCOL=EXCL -DARTS_WRITE_POLICY=WB -DARTS_RELEASE_POLICY=PURGE \
  -DARTS_USE_CXL=OFF -DARTS_BUILD_BENCHMARKS=ON -DARTS_BUILD_TESTS=OFF \
  -DARTS_BUILD_HPX=OFF -DARTS_BUILD_OCRVX=OFF -DARTS_BUILD_XSOCR=OFF \
  -DARTS_COUNTER_CONFIG=$PWD/configs/counters_cxl_ipdps27.cfg
ninja -C build_sweep {CoMD_intel_chandra_tiled_hinted,miniAMR_intel_dist,nekbone_dist,npb_cg_dist,Stencil2D_intel_channelEVTs,stream_dist,mapreduce_wordcount}_arts_ocr_excl_purge

# One-off runs (one rank, local launcher); 330 s timeout by default
python3 experiment_scripts/sweep_mem_iters_tmp.py --tag check \
  --run 'stream_dist|4147200000 3456 10' \
  --run 'npb_cg_dist|-t E -c 188 -i 1' --timeout 900
```

The script `experiment_scripts/sweep_mem_iters_tmp.py` is temporary and
untracked.  It does three things:

- polls `/proc` for the RSS of the process tree;
- reads `counters/n0.json`;
- kills a run past its timeout, past 420 GB RSS, or when MemAvailable drops
  below 30 GB.

Raw results for every run (`runs.jsonl`, per-cell `output.log`, `arts.cfg`,
counters) are under `results/sweep_mem_iters_tmp/<timestamp>-{cal,r2,r3,r4,r4npb}/`.

## Not done yet

- These inputs are not yet a profile in `run_cxl_experiments_optimal.py`.
  When adding one:
  - mapreduce needs its `{per_node:384}` placeholder;
  - every per-rank knob must divide 1–4 nodes.
- None of these inputs has been run on CXL (STAGED or DIRECT) or on more than
  one node.
- npb_cg under 5 min at ~100 GB would need a source change (see above).
