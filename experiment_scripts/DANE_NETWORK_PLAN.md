# Plan: the ISCA'27 Network series on Dane (RDMA)

Handoff for an agent working **on LLNL Dane**. The plan was written on another
machine that has no Slurm and no Dane access, so nothing here has been run on
Dane. Every "verify" item below is a check you must make there before
submitting.

## Goal

Re-run the paper's **Network** series (end-to-end time plus ARTS counters, **no
PAPI, no CXL variants**) on Dane. On the CXL testbed the network was TCP over
10 Gb/s. On Dane it is Omni-Path through libfabric `verbs;ofi_rxm`. The output
must use the same layout and schema as the testbed batches, so it can be added
as a sixth series (`network_rdma`) next to the five in
`results_isca27/arts-cxl-consolidated-20261116/`.

## Decisions already made (do not revisit)

| Topic | Decision |
|---|---|
| Code | Current branch `cc27`. The paper's `counters-v1` tree is not available. |
| Variant | `arts_ocr_excl_retain_owner` (cc27's EXCL + RETAIN; the paper's `arts_ocr_excl_retain` no longer exists). Binaries: `<app>_arts_ocr_excl_retain_owner`. |
| Threads | Paper's split: **94 workers + 2 progress** for every app except **HPCG and Smith-Waterman: 92 + 4**. Dane has 112 cores; 16 stay idle on purpose. |
| Nodes | **1, 2, 3, 4** only. One rank per node. |
| Repetitions | **10** per (app, nodes). No warm-up runs. |
| Apps | The 18 apps of the paper's Table 3. **NPB CG is excluded.** |
| Slurm | Partition `pbatch`. No account/bank flag (leave it unset). |
| PAPI | None. |

## The run matrix (exact)

There are 18 apps × 4 node counts × 10 reps = **720 runs**. Arguments are the
paper's final inputs (`arts-cxl-paper/app_inputs.csv` and
`results_isca27/arts-cxl-consolidated-20261116/source_map.csv`).

| # | app (base target) | args | threads (w+p) | per-run timeout |
|---|---|---|---|---|
| 1 | `cholesky_dist` | `--ds 28800 --ts 300 --places 144` | 94+2 | 300 s |
| 2 | `CoMD_intel_chandra_tiled_hinted` | `-x 108 -y 72 -z 72 -N 20 -n 10 -i 12 -j 8 -k 8` | 94+2 | 300 s |
| 3 | `fft_dist` | `28 768 24` | 94+2 | 300 s |
| 4 | `fibonacci_hinted` | `29 14` | 94+2 | 300 s |
| 5 | `graph500_dist` | `21 16 128 128` | 94+2 | 300 s |
| 6 | `hpcg_intel_dist` | `12 8 6 32 50` | **92+4** | 1800 s |
| 7 | `hpgmg_dist` | `4 13824` | 94+2 | 600 s |
| 8 | `LCS_wavefront` | `1048576 2048` | 94+2 | 300 s |
| 9 | `mapreduce_wordcount` | `16384 2670000 {per_node:1128} 8 100 0 16 1` | 94+2 | 300 s |
| 10 | `miniAMR_intel_dist` | `--npx 12 --npy 8 --npz 8 --nx 16 --ny 16 --nz 16 --num_vars 40 --num_tsteps 10 --stages_per_ts 10 --num_refine 0 --checksum_freq 10 --report_diffusion 1` | 94+2 | 300 s |
| 11 | `nekbone_dist` | `12 8 8 2 2 2 8 60` | 94+2 | 300 s |
| 12 | `nqueens_hinted` | `19 15 1 4` | 94+2 | 300 s |
| 13 | `RSBench_intel_sharedDB_hinted` | `-s large -l 1728000 -t 216 -p 24` | 94+2 | 300 s |
| 14 | `smithwaterman_dist` | `100 100 {repo}/datasets/smithwaterman/string1-cmp.txt {repo}/datasets/smithwaterman/string2-cmp.txt {repo}/datasets/smithwaterman/score-cmp.txt 24` | **92+4** | 1800 s |
| 15 | `Stencil2D_intel_channelEVTs` | `13824 1152 200` | 94+2 | 300 s |
| 16 | `stream_dist` | `233280000 3456 100` | 94+2 | 300 s |
| 17 | `triangle_hinted` | `7 1 8` | 94+2 | 300 s |
| 18 | `XSBench_intel_sharedDB` | `-s large -g 48 -l 25000000 -t 64 -p 24` | 94+2 | 300 s |

Argument expansion follows `expand_args()` in
`experiment_scripts/run_cxl_experiments_optimal.py`:
- `{per_node:M}` becomes `M / nodes`. 1128 divides 1, 2, 3 and 4.
- `{repo}` becomes the repository root on Dane.

Executable path: `<build>/benchmarks/apps/<app>_arts_ocr_excl_retain_owner`.
The `*_hinted` names are the hinted flavor of the base target (for example
`add_benchmark_app(NAME fibonacci ...)` in `benchmarks/apps/ocr_apps.cmake`
produces `fibonacci_hinted_arts_<suffix>`).

For scale: on the testbed one pass of all 18 apps over TCP took about 250 s on
1 node and about 130 s on 4 nodes. Dane should not be slower.

## Step 0: build on Dane

Build inside a Slurm job, not on the login node. For example:
`srun -N1 -n1 -c 32 -p pdebug --time=60 <cmake/ninja ...>`. artsrun's
`srun_build_prefix` does the same.

1. Configure a **Release** tree, for example `build_dane_net/`, with:
   - `-DCMAKE_BUILD_TYPE=Release`
   - `-DARTS_BUILD_BENCHMARKS=ON -DARTS_BUILD_OCR_APPS=ON`
   - `-DARTS_COUNTER_CONFIG=$REPO/configs/counters_cxl_ipdps27.cfg`
     (the counter set the paper's runs were built with). A fresh tree defaults
     to `counters_off.cfg`, which records nothing.
   - Use the same compiler/module setup artsrun uses with the `dane` profile.
     If unsure, run `artsrun run -p dane -x smoke --dry-run` and copy the
     configure command it prints.
2. **Verify:** the counter config parses on cc27. It lists
   `TIME/BYTES/NUM_CXL_FLUSH_{PRODUCER,CONSUMER}`. If cc27 rejects any name,
   create `configs/counters_dane_network.cfg` containing the remaining lines
   (keep `TIME_EDT_*`, `NUM_EDT_*`, `*_DB_CREATE`, `*_DB_PAYLOAD_*`) and record
   the change in the manifest.
3. Build only the 18 targets `<app>_arts_ocr_excl_retain_owner` from the table.
   Building every variant is slow and unnecessary.
4. Stage the Smith-Waterman inputs (`string1-cmp.txt`, `string2-cmp.txt`,
   `score-cmp.txt` under `datasets/smithwaterman/`) with
   `tools/artsrun/src/artsrun/fixtures.py` (`fixtures.stage([...])`; read its
   signature first). The generator is deterministic (seed 20260828001,
   200000 × 200000), so the files match the testbed's.
   `stage_inputs()` in `run_cxl_experiments_optimal.py` shows how to call it.

## Step 1: write `experiment_scripts/submit_dane_network.py`

This is a submitter plus a collector. **Reuse, don't copy,** the
record-building helpers of `run_cxl_experiments_optimal.py` by importing them:
`expand_args`, `read_rank_counters`, `derive`, `sanity_checks`,
`build_record`, `write_tables`, `git_info`, `E2E_RE`. That keeps `runs.csv`
column-for-column compatible with the paper batches. If importing that module
has unwanted side effects (it reads `COUNTER_CONFIG` at import), set its
globals after import or move the helpers into a small shared module. Don't
fork them.

### Command line

```
submit_dane_network.py --run-id ID [--repo-root R] [--build-dir B]
                       [--partition pbatch] [--reps 10] [--nodes 1,2,3,4]
                       [--apps a,b,...] [--dry-run] [--smoke]
                       [--resume] [--collect]
```

- Default run directory: `$REPO/results/dane_network/<run-id>/`. It **must be
  on a filesystem every compute node can see** (Lustre/NFS workspace), because
  remote ranks write counter files there.
- `--dry-run` writes every cell directory and job script and prints the
  `sbatch` commands without submitting.
- `--smoke` runs 1 rep at nodes {1, 2} with the `SMOKE_APPS` inputs from
  `run_cxl_experiments_optimal.py`, under a separate run-id. Run it first.
- `--resume` resubmits only the jobs that contain a run with no `done` marker
  or a non-`ok` status. It never overwrites a finished run.
- `--collect` builds `runs.jsonl`, `runs.csv`, `counters_long.csv` and
  `manifest.json` from the cell directories. Safe to run repeatedly.

### Cell layout (same as the testbed's)

```
<run_dir>/cells/<app>__arts_ocr_excl_retain_owner__n<N>__r<k>/
    arts.cfg   args.json   output.log   counters/   done
```

`args.json` = `{"app": "<abs executable>", "args": "<expanded args>"}`.

### `arts.cfg` per cell

Start from the testbed file (example: the `paper8-chol-nocxl-1115` cell in the
appendix) and change only what the machine requires:

```
[ARTS]
worker_threads=94            # 92 for hpcg_intel_dist and smithwaterman_dist
progress_threads=2           # 4 for hpcg_intel_dist and smithwaterman_dist
launcher=slurm
node_count=<N>
pin=1
route_table_size=16
core_dump=0
port_count=1
ports=25000
counter_folder=<cell>/counters
counter_capture_interval=250
provider=verbs;ofi_rxm
regpool_slab_mb=2048
```

- Leave out the testbed's `nodes=ca-fcp0,...` line and the CXL-only
  `cxl_db_allocation_strategy` key. Under `launcher=slurm` the runtime takes
  rank and hosts from the Slurm environment (`libs/src/core/system/config.c`,
  `libs/src/core/transport/socket.c`).
- **Verify:** compare against what artsrun writes for Dane, using
  `artsrun config render -p dane -n 2` (`experiments/profiles/dane.yaml`).
  If artsrun adds a host-list, `net_interface`, `fabric_domain` or
  `stack_size_bytes` key for Slurm runs, copy it. If the cell's key set
  differs from the block above, list the difference in the manifest.

### Slurm job layout: one job per (node count, repetition), 40 jobs

Each job holds an exclusive allocation of N nodes and runs all 18 apps of one
repetition one after another, in table order. This follows the testbed's
repetition-major order, and each repetition gets a new allocation, so
node-placement noise shows up in the 10-rep spread instead of being shared by
all reps.

sbatch header (adapt `sbatch_argv()` in `tools/artsrun/src/artsrun/run/slurm.py`):

```
#SBATCH --partition=pbatch
#SBATCH --nodes=<N>
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=96
#SBATCH --exclusive
#SBATCH --time=02:00:00
#SBATCH --job-name=arts-net-n<N>-r<k>
#SBATCH --output=<run_dir>/jobs/n<N>_r<k>.out
```

(no `--account`)

For each app, the job script:

```
export ARTS_CONFIG=<cell>/arts.cfg
export ARTS_E2E_MARKER=1
export ARTS_END_GRACE_S=30
cd <cell>
s=$(date +%s.%N)
timeout -k 10 <app timeout> \
  srun --ntasks-per-node=1 -N <N> --cpus-per-task=96 \
       --kill-on-bad-exit=0 --wait=30 \
       <exe> <args> > output.log 2>&1
rc=$?
echo "$rc $s $(date +%s.%N)" > done.tmp && mv done.tmp done
```

Two rules for this script:
- One failing or timed-out app must **not** stop the job. Do not use `set -e`;
  continue to the next app.
- Never add `srun --label`, because it breaks the `[E2E]` line parse.

`--cpus-per-task` is set explicitly on the step because srun no longer
inherits it (see the comment on `_launch()` in artsrun's `slurm.py`).

The 2 h walltime is far above the expected ~5–10 min per job. It covers a hung
app reaching its timeout. Expect HPCG and Smith-Waterman to be the longest
(15–30 s on the testbed at 1 node).

### Collect

For each cell:
- Parse `[E2E] <ns>` from `output.log`.
- Read `counters/*.json` with `read_rank_counters`.
- Set the status:
  - `ok`: rc 0, an E2E stamp, and counters from every rank
  - `failed`: rc ≠ 0
  - `timeout`: rc 124 or 137
  - `no_e2e`: no E2E stamp
  - `counters_incomplete`: counters missing from some rank
- Build the record with `build_record`, setting `worker_threads` and
  `progress_threads` to the cell's actual values.
- Add the columns `machine=dane`, `series=network_rdma`,
  `provider=verbs;ofi_rxm`, `slurm_job_id`, `hostnames` (`scontrol show
  hostnames` captured in the job's `.out` file).

`manifest.json` holds:
- the exact command
- `git_info()` (commit, branch, dirty flag, submodules)
- build dir, CMake cache values (`CMAKE_BUILD_TYPE`, `ARTS_COUNTER_CONFIG`,
  compiler)
- `module list` output
- `fi_info -p verbs` output (one line) and the libfabric version
- the full matrix

## Step 2: run order on Dane

1. Build (Step 0) and stage the Smith-Waterman inputs.
2. `--dry-run`: inspect one job script and two `arts.cfg` files (one 94+2, one
   92+4).
3. `--smoke --run-id dane-net-smoke`, then `--collect`. Every cell must be
   `ok`. Then check:
   - `ranks_reported == nodes`
   - the 2-node `output.log` shows the verbs provider in use, not a tcp
     fallback
4. Full run: `--run-id dane-net-<MMDD>`. Wait for the queue to drain, then
   `--collect`, then `--resume` if anything failed, then `--collect` again.
5. Validate (below), then tar the run directory for transfer:
   `results/dane_network/<run-id>.tar.gz`.

## Validation before declaring the data good

- All 720 runs `ok`, and each (app, nodes) has 10.
- **Same input as the testbed:** for each app, `NUM_EDT_FINISH`,
  `NUM_DB_CREATE` and `BYTES_DB_CREATE` should match the testbed's `network`
  series in `results_isca27/arts-cxl-consolidated-20261116/runs.csv` (the
  paper accepted less than 1 % spread across series). Differences can come
  from cc27's protocol changes and are worth reporting, but **any app whose
  DB count or bytes differ by more than 1 % must be flagged in the summary**,
  because its input or program changed.
- The application's result lines (`result_signature`: application output with
  the timings removed) are identical across reps, with no NaN/inf (HPCG prints
  `final deviation`).
- Summary table: median and min/max of `e2e_s` per (app, nodes), next to the
  testbed's `network_median_s` from `e2e_five_series.csv`, plus the ratio
  Dane/testbed.

## Caveats to record in the run's README

- The code differs from the paper: cc27's `ocr_excl_retain_owner` vs.
  `counters-v1`'s `ocr_excl_retain`. Dane/testbed ratios mix network and code
  changes.
- The CPUs differ: Dane is Sapphire Rapids, 2×56 cores; the testbed was Xeon
  Platinum 8568, 2×48 cores. With `pin=1`, 96 threads take cores 0–95: all of
  socket 0 and 40 cores of socket 1. The testbed filled both sockets exactly.
  Record the binding the runtime reports.
- The network differs: Omni-Path via `verbs;ofi_rxm` vs. TCP over 10 GbE.

## Out of scope

PAPI runs, CXL variants (staged/direct), NPB CG, more than 4 nodes, the
108+4 Dane geometry, and any change to the runtime or the apps.

## Appendix: one testbed cell for reference

`results_isca27/paper8-chol-nocxl-1115/cells/cholesky_dist__arts_ocr_excl_retain__n2__r4/arts.cfg`:

```
[ARTS]
worker_threads=94
progress_threads=2
launcher=ssh
node_count=2
nodes=ca-fcp0,ca-fcp1
pin=1
route_table_size=16
core_dump=0
port_count=1
ports=25000
counter_folder=.../counters
counter_capture_interval=250
provider=tcp
regpool_slab_mb=2048
cxl_db_allocation_strategy=round_robin
```

The `results_isca27/` archives are untracked, so they may not be on Dane. If
validation needs them there, copy
`results_isca27/arts-cxl-consolidated-20261116/{runs.csv,e2e_five_series.csv}`
over first.
