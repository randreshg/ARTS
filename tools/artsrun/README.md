# artsrun

Selection, build and run driver for ARTS experiments.

One tool holds the three things an experiment is made of — which coherence
configurations to run, on which machine geometry, over which applications —
compiles everything selected in a single pass, runs the resulting cells, and
votes a consensus on their result scalars.

## Install

The repository's `.venv` is uv-managed:

```bash
uv pip install -e "tools/artsrun[dev]"
```

Without uv:

```bash
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/pip install -e "tools/artsrun[dev]"
```

The `dev` extra is what carries `pytest`; `requirements.txt` is compiled from
`pyproject.toml`'s base dependency set only (runtime dependencies), so
installing it alone leaves pytest absent from the venv.

`requirements.txt` at the repository root is generated from this package's
`pyproject.toml` (`uv pip compile tools/artsrun/pyproject.toml -o
requirements.txt`); regenerate it when a dependency changes.

Profiles are tracked with the tree, one per machine the experiment runs on
(`experiments/profiles/`); on a machine none of them describes, the screens
still open — on an unsaved single-node local profile; adjust it on the
Profile tab and Save writes the machine's first one. The same bootstrap
exists on the command line:

```bash
artsrun profile new <machine>     # single-node local defaults, opens $EDITOR
```

The build tree bootstraps the same way: a first real run configures the
experiment build (Release, benchmarks on — its first build also compiles
the vendored dependencies, so it is long) and every later run reuses it.
Dry runs configure nothing, and an existing tree is only verified — a
Debug or no-benchmark tree is somebody's deliberate configuration and is
reported, not replaced. Under a Slurm profile, every piece of build work —
the first configure, a counter reconfigure, the ninja pass — runs inside a
small job of its own rather than on the login node: one task, a few cpus,
nothing exclusive, so it slots into whatever gap the queue has.

## Use

```bash
artsrun                                  # the selection screens
artsrun plane                            # the configuration plane
artsrun apps -b paper-main               # the catalog, with each app's versions
artsrun profile list | show X | validate | fields
artsrun profile new junction2 --from junction   # copy, then $EDITOR
artsrun profile set junction2 workers=31 slurm.partition=pbatch
artsrun benchset list | show X | new Y --from X | edit Y
artsrun benchset set paper-main nqueens --args "12 4" --versions asborn,optimized
artsrun config render -p ferrari-local -n 4    # inspect a rendered configuration
artsrun run -p ferrari-local -b paper-main --dry-run
artsrun run -p junction -b paper-main --detach
artsrun watch                             # live view of the latest campaign
artsrun watch 20260811-220547             # …or of a named one
artsrun report                            # reprint the latest summary

artsrun counters --set perf --enabled     # what a counter set turns on
artsrun counterset list | show X | render X
artsrun run -p ferrari-local -b paper-main -c census
```

### FAM-device runs

`--cxl` is the FAM-device mode: it runs the fabric-attached-memory entries
of the plane (`arts_excl_purge_fam_staged`, `arts_excl_purge_fam_direct` —
the two residencies) on the device library itself, and launches each of
their cells through the root `run_cxl.sh` wrapper, which sets the device's
regions up once around the whole launch (its utilities must be on `PATH` on
the compute nodes). In the TUI, press `5` for the **Run** tab, turn on **FAM
device**, and enter the build tree; for a new build tree fill in **device
include** and **device library** as well. The Coherence screen narrows to
the two FAM entries while the mode is on and restores your previous
selection when you turn it off; the `a` key there toggles only the eligible
entries. From the command line:

```bash
artsrun run -p <profile> -b <benchset> --cxl --build-dir build_fam_device \
  --fam-device-include-dir /path/to/device/include \
  --fam-device-library /path/to/device/libdevice.so
```

A new tree is configured with `-DARTS_FAM_BACKEND=DEVICE
-DARTS_FAM_DEVICE_VENDORED=OFF` and the two paths (the FAM entries are
benchmark variants carrying their own protocol, so the tree's own protocol is
left at its default). An existing tree must have `ARTS_FAM_BACKEND=DEVICE`,
`ARTS_FAM_DEVICE_VENDORED=OFF` and both paths real; artsrun verifies this
before building or running. The vendored emulation of the device library
(`-DARTS_FAM_DEVICE_VENDORED=ON`) is for development runs and is refused for a
measurement, and a `DEVICE` tree is refused to any campaign without `--cxl`.
The old `--cxl-rapid-include-dir` / `--cxl-lib-dir` are errors naming the new
options. Explicit `--entries` and saved selections with non-FAM entries are
rejected; the mode and the paths are saved for `--from` and `--resume`. A one-rank
FAM-device cell's launch environment sets `ARTS_FLUSH_LOG` to
`<cell>.flush.bin` beside the cell's log, where the device library writes its
flush trace; a cell with more than one rank sets it to `/dev/null`, because the
library takes one path for all ranks and every rank on a host would overwrite
the same file. The wrapper locates its preparation script relative to
the checkout and runs in the cell's scratch directory.

## External runtimes

Besides the thirteen-entry coherence plane, `hpx` is an off-plane entry: the
STE||AR HPX runtime (MPI parcelport, static-linked like the other
references), a cross-programming-model reference tied to no coherence
position. It multiplies by nothing, and its scalar joins the ordinary
consensus vote only in the version rows the HPX-origin section defines;
every other catalog row reports the HPX cell as structurally ineligible.

The rows it runs are the HPX-origin section (`hpx_apps.yaml`, names ending
in `_hpx`): each is an HPX program, `<row>_hpx`, and the row's
ARTS/xsocr/ocr-vx binaries are its OCR mirror; `benchmarks/hpx/apps.cmake`
is the build list. The rows sit in the `paper-controls` roster, and the
`controls-gate` roster runs them at small arguments as the gate (the main
roster's gate is `main-gate`). Under `ARTS_STRUCT_MARKER`
each HPX-origin program prints `[PARCELS] sent=… bytes=… wire=…` from
locality 0 immediately after its end stamp, so a timing cell never executes
it; there is no `[STRUCT]` line in this section — that carried the retired
ports' own structural counters and none of these programs has any. The ARTS
arms beside it are built with the
`configs/counters_off.cfg` default, which a `-c`-less campaign checks and
refuses to run against an instrumented tree. Under `launcher=local` every
reference cell — HPX, xsocr and OCR-vx — carries `UCX_TLS=tcp,self` and
`UCX_NET_DEVICES=lo`: a local run simulates a multi-node job, so every
runtime's ranks reach each other through the network stack's loopback, as
ARTS's socket provider already does, and never through shared memory.
Remote launchers keep the site's MPI defaults.

On the screens: `1`–`4` switch surfaces, `5` is the run tab, `space`
toggles, `a` is the shared all/none control for whichever surface is
showing, `r` runs, `d` dry-runs. The run tab is the live table with a
single control row above it; build output appears below only while there is
nothing else to watch and folds away when the table takes over (`l` brings
it back).

Running on a terminal opens the **live view** — one table row per cell (app,
version, runtime, coherence axes, node count, repeat), recolored as cells
queue, run, finish, and vote; `--plain` keeps the old line output. A click
(or `enter`) opens the cell: the exact command it runs under (for Slurm, the
sbatch call and the batch script), its environment, return code, a following
tail of its log (stderr is merged into stdout; stdin is `/dev/null`), and
buttons for the configurations behind the run — the rendered runtime
configuration the cell was handed and, when counters are on, the counter
file the build was configured against. While the pane is open it follows the
cursor; `esc` closes it.
Consensus is re-voted as results land, so a configuration that strays turns
its row `DIFF` the moment it disagrees — not at the end of the campaign.
The `scaling` button on the summary row (or `g`) swaps the table for the
**strong-scaling reading** of this run — scaling belongs to the campaign it
was measured in, so it lives on the same surface: best wall per node
count for every (application, version, configuration), each wider cell
carrying its speedup against the row's smallest measured node count and
coloured by parallel efficiency — an anti-scaling row turns red the moment
its wider run comes back slower. Because the screen already says all of
this, the end of a campaign prints only a one-line verdict and the path to
`summary.txt`; the full tables still land there. `artsrun watch` attaches
the same view to a running (or finished) campaign from any terminal; a
`--detach`ed campaign is watched the same way, and `d` detaches the view
again without touching the run.

The **Profile** screen edits the settings themselves — launcher, workers and
progress threads, provider, ports, hosts, Slurm partitions — and saves under
the same name or a new one; an invalid combination reports the one thing to
fix and writes nothing. Node counts sit in *Run shape* as the list itself: `✕` removes one, the box
at the end adds one. Removing changes the machine's shape (Save writes the
list); unchecking changes only this campaign, so narrowing one run costs
nothing and needs no save.

Only the sections the launcher decides are shown — the SSH roster for ssh,
the Slurm settings for slurm. Every launcher states `connections / node`;
only the remote ones name the ports, and then the list must hold exactly that
many. A local run's ranks share a machine, so the runtime claims its own block
of `node_count × connections` instead.

An ssh profile declares how many nodes it may use and must name exactly that
many hosts. A Slurm profile declares no budget at all: **every cell is
submitted up front** — scheduling the queue is Slurm's whole purpose — and
each job writes its own outcome marker on the shared filesystem as it ends.
The submitting login node is thereby optional: if it dies with the queue
full, nothing is lost — `artsrun watch` and `artsrun report` reconstruct
finished cells from the markers, and `--resume` resubmits only what neither
finished nor still sits in the queue. Only an explicit stop cancels jobs;
a dead process leaves the queue alone. Each job's `--time` comes from the
cell's own timeout, so the backfill scheduler can slot short jobs early;
the in-job `timeout -k` fires first, which is what lets even a timed-out
cell write its marker. `slurm.build_partition` (e.g. a debug partition)
and `slurm.build_cpus` (default 8) shape where and how wide build work
queues. The one thing a site does meter is how many jobs one user may have
submitted, and a full roster is several thousand cells: `slurm.max_queued`
keeps at most that many of a campaign's jobs in the queue, pending or
running, and submits the rest as they end. With a cap the submitter has work
left until the last cell is out, so a campaign whose submitter died is
continued with `--resume` rather than merely watched.

A profile may also name the plane `entries` its campaigns run. The plane
holds every protocol any study uses, and a study that leaves some out — the
DB-WRF section, say — states it once in its profile rather than on every
command line: those are the entries a run with no `-e` takes, and the ones
the Coherence screen starts with checked. Naming an entry with `-e`, or
checking it on the screen, still runs it; a profile without the field runs
everything.

Only what a campaign actually decides is a field. The width a run occupies
follows from workers + progress, so nothing else states a core count: a local
run's ranks are placed by the runtime, and a Slurm job owns its nodes
outright and derives `--cpus-per-task` from that same width.

The **Applications** screen lists the suite in two groups. *Applications* come
first — benchmarks with a provenance, the ones a result is claimed about — and
*Microbenchmarks and fixtures* after, each exercising one runtime mechanism or
carrying no workload at all. Only the first group is enabled by default.

Arguments are edited inline. An empty box means the catalog's own calibration,
shown as the placeholder; type into it and the benchmark set records an
override.

## The cluster selection

`val_*_nocomb` is not part of it. The non-combining twins are an ablation of
one family's read path rather than a position on the plane — selectable build
variants a benchset names directly as an entry — and the question they
answer, what the requester-side combining window buys, is why they stay on
the development host (`-p ferrari-local`) where both settings of one knob fit
in a campaign. A cluster campaign runs the eight plane arms and the
references.

The adversarial plan is the adversarial half of the `paper-controls` roster
under the `dane` profile. Every attack row carries one argument vector that
runs at every geometry: each program derives its own population from the
runtime's rank count, so the per-rank load stays fixed in the program itself,
not in a per-node edition. The columns and the grain dial run the whole node
sweep in the same campaign as `paper-main`, at the profile's own geometry:
there is no separate sweep command, and the attack rows run inside this
roster like any other.

For that same reason an attack row is still **not** a scaling curve: its
comparison is across runtimes within one node count. The scaling table marks
such a row `~` on its name and says so in its legend, computed from the
arguments themselves — any row whose arguments differ across the node counts
shown is marked, whatever its kind.

A catalog row may also declare `width_max` — the instantaneous task width its
calibrated arguments reach at the profile's widest geometry. It is checked
before a campaign builds anything: below the machine's total worker count
(`nodes x workers`) the widest cell would run narrower than the machine, and
an `spmd` or `mw` row, whose width is a fixed team, must reach a whole
multiple of it. The declaration describes **the catalog's own arguments**: it
is a hand-written number, not one derived from them, so a roster that
overrides `args` or `args_by_nodes` has invalidated it — the campaign prints
that the row's width is unchecked instead of judging a shrunken cell against a
full-machine number. The attack rows declare no width on purpose: their chain
count is a designed per-rank load -- 3 or ~15 chains a rank depending on the
column, held fixed across the sweep -- not a machine-filling frontier.

## Where things live

| | |
|---|---|
| Application structure and calibration | `src/artsrun/data/apps.yaml` (committed) |
| Configuration plane | `src/artsrun/data/protocols.yaml` (committed) |
| Configuration templates | `configs/templates/*.j2` (committed) |
| Machine and node settings | `experiments/profiles/*.yaml` (committed) |
| Application rosters and overrides | `experiments/benchsets/*.yaml` (committed) |
| Counter selections | `experiments/countersets/*.yaml` (committed) |
| Counters the runtime defines | `src/artsrun/data/counters.yaml` (committed) |
| Campaign output | `logs/exp/<timestamp>/` |

A campaign directory holds `manifest.json` (every cell with the exact command
it runs under, and the sources it ran from: the checkout's commit, the tracked
paths that differed from it, and every submodule's commit — written before
anything runs), `track.jsonl` (events as they
happen: submitted / started / running / finished, with the final status and
scalar), `selection.yaml` (replayable), per-cell logs under `cells/`, and the
end-of-run `results.csv` / `report.json` / `summary.txt`. The live view is a
pure reader of the first four, which is why it can attach to any campaign
from anywhere.

Each application offers up to three versions: **as-born** (as published,
including whatever hints its authors already gave it), **optimized** (the
same code structure with EDT/DB placement hints added or changed — as
statically optimized as hints alone can make it), and **restructured**
(the decomposition itself redesigned as a separate target, for the cases
hints could not fix).

A benchset carries only its deltas: anything it omits falls through to the
catalog, so a campaign that changes one application's arguments is a two-line
file.

The **Counters** screen selects what the build measures. Each counter takes a
mode (`OFF` / `ONCE` / `PERIODIC`), a level, and a reduction:

| Level | What is written |
|---|---|
| `THREAD` | each worker's own value |
| `NODE` | reduced across a rank's threads |
| `CLUSTER` | reduced across ranks, and the per-rank value is still written |

**Counter selection is a build-time decision.** The set is parsed at configure
time into `Preamble.h`, whose indices are compiled into every file that
touches a counter, so switching sets means a reconfigure and a full rebuild.
The driver refuses to run against a tree configured with a different counter
file rather than measure with the wrong ones, and prints the `cmake` line to
fix it. A campaign that selects no set at all is checked the same way against
the build's default of no counters (`configs/counters_off.cfg`) and refuses a
tree an earlier counted campaign left instrumented, naming the file the cache
points at. The sampling interval and the output folder are the exception — the
runtime reads those from its own configuration, so they move freely.

## Tests

```bash
.venv/bin/python -m pytest tools/artsrun/tests -q
```

This needs the venv from Install with the `dev` extra; a venv installed
without it has no `pytest` module.

The configuration-rendering tests compare against the committed `configs/`
trees, so they fail if a template drifts from what the runtimes have been
running.
