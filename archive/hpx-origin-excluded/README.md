# HPX-origin rows outside the comparison (retired)

Seven programs of the HPX-origin section (`benchmarks/hpx/README.md`) that
went through the section's admission, OCR mirroring, gate and one-node
calibration, and were retired on 2026-09-16 because they fail the
comparison's criterion: the origin suspends started parallel tasks
(blocking on futures, collectives or locks inside a task), or the origin
fixes its parallel width below the machine's (one task per locality, a
fixed cell count). The four rows that pass — `fib_hpx`, `stencil1d_hpx`,
`network_storage_hpx`, `fft_hpx` — are the section and the paper's
comparison; these seven were carried for a while as annotated rows
(`comparison_excluded`) and then taken out of the tree altogether, with
the annotation, the vendored HDF5 and the two origin submodules
(`third_party/miniapps`, `third_party/NBody`) they alone needed.

| row | origin | why it is outside the comparison |
|---|---|---|
| `pi_hpx` | HPX collectives example `distributed_pi` | width fixed by the origin: one integration block per locality, no task spawned — on one node the whole computation is one thread |
| `random_mem_access_hpx` | HPX example `random_mem_access` | started-then-waits: a component lock serialises the updates of one element and its contention yields the running thread; the mirror serialises through data-block ownership, a different mechanism |
| `transpose_hpx` | HPX example `transpose_block` (PRK transpose) | started-then-waits: the validation tasks block on sub-block futures inside a parallel reduce, one per block per iteration |
| `jacobi_hpx` | HPX example `jacobi` (row components) | started-then-waits: `update` tasks `async`, then block on four neighbour `get`s — one suspended thread per line block per row per iteration |
| `mini_ghost_hpx` | STE\|\|AR `miniapps/MiniGhost` (2015 API, modernised) | started-then-waits: a summed variable's step blocks mid-task on an all-reduce, unpacks block on a neighbour's parcel |
| `nbody_hpx` | STE\|\|AR `NBody/DistributedNBody` (2016 API, modernised) | width fixed by the origin: octree level 8 cells below nine localities (64 above), so at most eight cell chains run at once on one node |
| `sheneos_hpx` | HPX example `sheneos` (HDF5 EOS table) | started-then-waits: worker tasks block on their own bulk query (`unwrap`) and on the partition sends (`wait_all`) |

## What is here

```
README.md                 this file
README-rows.md            the seven rows' sections of benchmarks/hpx/README.md, verbatim:
                          origin, disclosed edits, wait sites, placement, oracle, sizing
hpx/<row>/                the HPX program as built: ORIGIN.md (origin path, pin, licence),
                          origin.patch (the complete disclosed diff origin -> ours), sources
mirrors/<row>.c           the OCR mirror (was benchmarks/apps/hpx_origin/<row>.c);
                          shen_rng.h is sheneos_hpx's RNG header
appdocs/<row>.md          the artsrun appdoc of each row (Overview .. Sizing)
hpx_apps-excluded.yaml    the seven catalog rows of hpx_apps.yaml, verbatim, with their
                          calibrated arguments, pins and sizing notes
hpx-gate-excluded.yaml    the seven entries of the hpx-gate roster, verbatim
datasets-sheneos.md       the datasets/README.md section for the EOS table sheneos_hpx reads
```

The build registrations that carried the rows — the HDF5 external project
and its `arts::hdf5` / `arts_hpx::hdf5_cpp` imported targets in
`third_party/CMakeLists.txt` and `benchmarks/hpx/CMakeLists.txt`, the
`ARTS_MINIAPPS_ROOT` / `ARTS_NBODY_ROOT` origin roots handed to the app
project, the seven `add_hpx_origin_app` and `add_bespoke_app` calls — are
in the commit that retired the rows (`git log -- archive/hpx-origin-excluded/README.md`).

## Measured facts, as they stood

The rows were calibrated at one node and swept 1/2/4/8 on the development
host like the four compared rows; their last trend and anchor tables are in
the `benchmarks/hpx/README.md` of commit `311906d` (`## ARTS versus HPX`),
measured before the runtime's data-block descriptor lost its unused inline
payload (`logs/exp/20260916-104712` for `pi_hpx` and `nbody_hpx`,
`logs/exp/20260915-054055` and `-225942` for the rest). `mini_ghost_hpx`
was re-measured after the origin's pack/unpack race was fixed in the origin
(the last bugfix its `ORIGIN.md` discloses). The calibration record
— laws, probes, refit, dispositions — is `logs/adhoc/2026-09-11-hpx-origin/calibration/`.

Things these rows established that the section keeps:

- `sheneos_hpx`'s cliff (ARTS time four times higher above `n = 81`) was
  the registered pool's per-allocation direct path for a block above half
  the base slab — the finding that led to the pool's redesign (preferred
  placement, 256 MiB base, ×1.5 growth, retired-mapping reuse).
- `pi_hpx`'s 2.2× per-iteration difference on an identical loop is the
  worked example of a toolchain effect disclosed, never patched.
- `mini_ghost_hpx`'s origin race (a step's pack reads the generation the
  next step's unpack writes) is the case for the rule that an origin's bug
  is fixed in the origin and disclosed, and the mirror follows.
- `nbody_hpx`'s origin path contains a space; `check_origin.py` passes an
  ORIGIN.md path to diff as one list element for that reason.

## Reviving a row

Put `hpx/<row>/` back under `benchmarks/hpx/`, its mirror under
`benchmarks/apps/hpx_origin/`, its appdoc under
`tools/artsrun/src/artsrun/data/appdocs/`; add its name to
`benchmarks/hpx/apps.cmake` and its `add_hpx_origin_app` /
`add_bespoke_app` calls; append its rows to `hpx_apps.yaml` and
`hpx-gate.yaml`; and re-add the origin submodule (`mini_ghost_hpx`,
`nbody_hpx`) or the HDF5 external project and dataset (`sheneos_hpx`)
from the retiring commit. `check_origin_<row>` then proves the disclosure
still matches the origin pin.
