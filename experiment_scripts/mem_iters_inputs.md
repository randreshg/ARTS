# Fixed-iteration apps: sizing inputs by memory footprint

Notes on how `run_cxl_experiments_magnificent7.py` sizes its inputs.  Read
this before changing the inputs or the size models.

Everything here was measured on a development host with no CXL memory, on
the network (DRAM) build.  The numbers are **datablock sizes and DRAM
footprints only**.  Timings from that host are deliberately left out: the
experiments run on the CXL machine, and only its timings are results.

## Footprint

"Footprint" means **total datablock bytes created over the run**
(`BYTES_DB_CREATE`), not peak RSS.  A CXL arena reclaims nothing within a
run, so this total is what the arena has to hold.  The user chose this
definition.  In the campaign it is per node: an N-node cell creates
N × footprint.

## Which apps

All 19 apps in `run_cxl_experiments_optimal.py`'s `APPS` were read and
classified:

- **Fixed iteration count: 7 apps.**  CoMD, miniAMR, nekbone, npb_cg,
  Stencil2D, stream and mapreduce_wordcount.
- **Converging solver: 1 app.**  hpcg_intel_dist runs CG until
  `rtr/rtr0 < 1e-13` or maxIter (arg 5).  The campaign runs it as
  `hpcg_intel_dist_fixed`, built with `HPCG_FIXED_ITERATIONS`, which drops
  the residual exit so it always runs maxIter.
- **Borderline: 2 apps.**  hpgmg_dist runs one fixed F-cycle.  graph500_dist
  iterates BFS levels until the frontier is empty.
- **Non-iterative: 9 apps.**  cholesky, fft, fibonacci, LCS_wavefront, nqueens,
  RSBench, XSBench, smithwaterman and triangle.

The campaign uses six of the seven fixed-iteration apps, plus HPCG with
its iteration count fixed.  npb_cg is out because its size is an NPB class,
not a dial (see below).

## The sweep

Per-node footprints of 1, 10, 150, 200 and 250 GB across 1–4 nodes,
weak-scaled.  Iteration counts are fixed per app.  Each input comes from a
size model (`SIZERS` in the script).  The model holds its per-node
decomposition at every node count and grows the domain along one axis.

| App | Per node | Grows by | Model (DB bytes per node) |
|---|---|---|---|
| Stencil2D | 1152 tiles, NT=50 | NP | 16·NP²/N + 194 B·NP·√(1152N)/N |
| stream | 3456 chains, N=60 | total_elems | 24 B × elems |
| mapreduce | TPN=384, K=8, 6 rounds | V | V × 8 B × (tree blocks of 384·N tiles)/N × 6 |
| miniAMR | near-cubic block grid, nx=10, 5 steps | block count B | B × (0.986 MB + 2.1 MB·B^(−1/3)) |
| CoMD | near-cubic tile grid, 60 steps | tile count | tiles × 6.16 MB (9³ tiles, < 100 GB) or 26.94 MB (18³ tiles) |
| nekbone | 18×16×12 tasks (9×8×6 below ~4 GB), pDOF 8, CG 20 | elements per task E | tasks × (0.455 + 0.355·E + 0.3675·E^(2/3)) MB |
| HPCG | near-cubic tile grid of m³ tiles (m = 16 below 5 GB, 32 below 100 GB, 64 above), 10 iterations | tile count | tiles × (11.4 kB + 424.61 B·m³ + 253.44 B·m²) |

### Per-app notes

- **Stencil2D**
  - The two NP × NP grids are allocated up front.
  - The second term is each tile's halo buffers, which scale with the
    tile's side.
  - NR and NP are free; uneven tiles are allowed.
- **stream**
  - The three arrays are allocated up front.
  - total_elems is a multiple of the chain count.
- **mapreduce**
  - One partial histogram per map tile and one per combine of the app's
    spawner tree.  A range splits into min(n, K) near-equal chunks.
  - Per node that is 457, 548.5, 579 and 530.25 blocks per round at 1–4
    nodes.  The tree is not flat in the node count, so V is sized per node
    count.
  - The per-round check is exact, so any ITER passes.
- **miniAMR**
  - About 1 MB per 10³-cell block (num_vars 40).
  - Bytes per block fall as the grid grows, because refinement is a surface
    effect.  That is the B^(−1/3) term.
- **CoMD**
  - A tile's datablocks depend on its link-cell count.
  - The model uses the two tile sizes that were measured directly and
    scales the tile count.
  - A tile needs at least 2 link cells per axis.
- **nekbone**
  - The fixed part is per task.  The volume part is the vectors created
    fresh every CG iteration: DRAM frees them, the arena keeps them.  The
    E^(2/3) part is the halo.
  - One element per task already costs ~4 GB at 3456 tasks, so smaller
    footprints drop to 432 tasks.
- **HPCG** (`hpcg_intel_dist_fixed`)
  - Everything is created when the tiles start: one private block per tile
    (matrix, indices and vectors of the four multigrid levels, ~425 B per
    fine point), the double-buffered exchange blocks (the m² term) and
    small tables.  An iteration adds only reduction scalars, ~72 B per tile,
    so the footprint does not depend on the iteration count.
  - m must be a multiple of 16 (four multigrid levels), so m is too coarse
    a dial and the tile count is the one that moves.
  - The residual exit never fired: at 10 iterations the unpatched binary
    created exactly as many blocks as the fixed one at every size measured.
    The switch makes the count a guarantee, and the campaign checks rank 0's
    `CG iterations: <n>` line against the input.
  - The tile-to-node bisection splits 3 nodes {0} | {1, 2}: node 0 holds
    ~1.5× the per-node footprint, nodes 1 and 2 ~0.7–0.8×.  The cluster
    total is right.  It is left unbalanced on purpose.
  - Constants fitted from one rank at 4×4×4 to 10×9×8 tiles, m = 16, 32,
    64; per-tile bytes vary under 0.3% with the tile count (edge tiles send
    less halo).
- **npb_cg** (not in the sweep)
  - Class D creates 9.8 GB and class E 86.4 GB on one rank.
  - Multi-rank runs add ≈ (R−1)·nloc·8 B per spmv.
  - Any `-i` below the class's niter prints `Verification FAILED`, though
    the run shuts down cleanly.

### Model checks: one rank

| App | 1 GB | 10 GB | 250 GB | 250 GB peak DRAM |
|---|---|---|---|---|
| Stencil2D | 1.00 | 10.00 | 249.99 | 256 GB |
| stream | 1.00 | 10.00 | 250.00 | 374 GB |
| mapreduce | 1.00 | 10.00 | 250.00 | 256 GB |
| miniAMR | 1.17* | 10.48* | 250.75 | 307 GB |
| CoMD | 0.99 | 10.09 | 249.49 | 289 GB |
| nekbone | 2.04–4.07† | — | 247.82 | 31 GB |
| HPCG | 1.017 (1.017) | 9.986 (9.986) | — | — |

HPCG's column is measured (predicted in parentheses).  It was checked at
150 GB instead of 250: 149.60 measured against 149.55 predicted, with peak
DRAM 156 GB.  The 150 GB run used 2 iterations; that changes its bytes by
under 1 MB.  The campaign runs no network variants, so HPCG has no 250 GB
DRAM number.

\* Measured with the first miniAMR model.  Those runs are what the
surface term was fitted to, and the refit reproduces them within 1%.
† Probes used to fit the nekbone model, which reproduces all five within 2%.

Peak DRAM is the resident set of the process tree on the network build.
nekbone's is low because DRAM frees each CG iteration's vectors.

### Model checks: weak scaling (local ranks on one host)

These ran 1, 2 and 4 rank processes on one host with `launcher=local`;
mapreduce also ran at 3.  Datablock GB per rank:

| App | 1 GB: 1 / 2 / 4 ranks | 10 GB: 1 / 2 / 4 ranks |
|---|---|---|
| Stencil2D | 1.002 / 1.002 / 1.002 | 10.001 / 10.001 / 10.001 |
| stream | 1.000 / 1.000 / 1.000 | 10.000 / 10.000 / 10.000 |
| CoMD | 0.986 / 0.986 / 0.986 | 10.094 / 10.094 / 10.094 |
| nekbone | 1.018 / 1.020 / 1.021 | 9.878 / 9.889 / 9.895 |
| miniAMR | 0.999 / 0.962 / 0.946 | 10.038 / 9.864 / 9.792 |
| mapreduce | 1.000 at 2, 3 and 4 ranks | 10.000 at 2, 3 and 4 ranks |
| HPCG | 1.017 / 1.017 / 1.017 | 9.986 / 9.987 / 9.988 |

HPCG at 3 ranks, per rank: 1.526 / 0.712 / 0.814 GB at 1 GB and
14.53 / 7.27 / 8.17 GB at 10 GB (the bisection; mean 1.017 and 9.988).

- **mapreduce:** with a fixed V the bytes per node rose 20% at 2 ranks.
  The tree-block count in its model fixes that.
- **Balance:** ranks are within 0.2% of each other in every case.
- **miniAMR:** about 5% low at 4 ranks.  Stacking blocks along z changes
  how much the objects refine.  It's left uncorrected.

The raw run directories from these checks were deleted.  The tables above
are the record.

## Before running on the CXL machine

- **CXL capacity:** 14 round-robin arenas of 96 GiB each (1.44 TB).  The
  widest cell needs about 1.34 TB, counting the 10% margin and up to
  224 GiB stranded in 4 GiB pools.  Build every tree with
  `-DARTS_CXL_DB_ARENA_SIZE_BYTES=103079215104`.
- **DRAM on the network variants:** up to 374 GB per node at 250 GB per
  node (stream).
- **Timeouts:** 30, 40, 50 and 60 min at 1–4 nodes.  Nothing about run times
  carries over from the development host; the first CXL cells will show
  where the budget is tight.
- **Release build:** `build/` is a Debug build with counters off.  Use a
  Release tree configured with `configs/counters_cxl_ipdps27.cfg`.

## Reproducing the size checks

```bash
# Release network tree with BYTES_DB_CREATE, only the binaries needed
cmake -GNinja -Bbuild_sweep -DCMAKE_BUILD_TYPE=Release \
  -DARTS_COHERENCE_PROTOCOL=EXCL -DARTS_WRITE_POLICY=WB -DARTS_RELEASE_POLICY=PURGE \
  -DARTS_USE_CXL=OFF -DARTS_BUILD_BENCHMARKS=ON -DARTS_BUILD_TESTS=OFF \
  -DARTS_BUILD_HPX=OFF -DARTS_BUILD_OCRVX=OFF -DARTS_BUILD_XSOCR=OFF \
  -DARTS_COUNTER_CONFIG=$PWD/configs/counters_cxl_ipdps27.cfg
ninja -C build_sweep {CoMD_intel_chandra_tiled_hinted,miniAMR_intel_dist,nekbone_dist,Stencil2D_intel_channelEVTs,stream_dist,mapreduce_wordcount,hpcg_intel_dist_fixed}_arts_ocr_excl_purge

# One input, one rank; the campaign's dry run prints every cell's input
python3 experiment_scripts/sweep_mem_iters_tmp.py --tag check \
  --run 'stream_dist|416665728 3456 60'
```

`experiment_scripts/sweep_mem_iters_tmp.py` is temporary and untracked.  It
reads `BYTES_DB_CREATE` from `counters/n0.json` and polls `/proc` for the
process tree's peak RSS.  It kills a run past its timeout, past 420 GB RSS,
or when MemAvailable drops below 30 GB.  It writes under
`results/sweep_mem_iters_tmp/`.
