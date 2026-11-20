#!/usr/bin/env python3
"""The ISCA'27 Network series on LLNL Dane (verbs;ofi_rxm), as Slurm jobs.

See experiment_scripts/DANE_NETWORK_PLAN.md.  One exclusive job per (node
count, repetition) runs all apps of that repetition back to back; each app
run is one cell with the testbed's layout:

  <run_dir>/cells/<app>__arts_ocr_excl_retain_owner__n<N>__r<k>/
      arts.cfg  args.json  output.log  counters/  done  job.txt

`done` is "<rc> <start epoch> <end epoch>", written atomically by the job
once the run ends; `job.txt` is "<slurm job id> <comma-separated hosts>".

The record-building helpers come from run_cxl_experiments_optimal.py, so
runs.csv is column-for-column the testbed batches' schema plus the Dane
columns (machine, series, provider, slurm_job_id, hostnames, ...).

Usage (from the repository root, on a Dane login node):

  submit_dane_network.py --run-id dane-net-smoke --smoke --dry-run
  submit_dane_network.py --run-id dane-net-smoke --smoke
  submit_dane_network.py --run-id dane-net-smoke --collect
  submit_dane_network.py --run-id dane-net-1119
  submit_dane_network.py --run-id dane-net-1119 --collect
  submit_dane_network.py --run-id dane-net-1119 --resume
"""

from __future__ import annotations

import argparse
import collections
import csv
import datetime as dt
import hashlib
import importlib.util
import json
import re
import shlex
import socket
import statistics
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_REPO = HERE.parent


def _load_runner():
    """run_cxl_experiments_optimal.py as a module.  Importing it only reads
    its COUNTER_CONFIG; every global this script depends on is set again
    below (REPO_ROOT, EXPECTED_COUNTERS, BASE_COLUMNS)."""
    spec = importlib.util.spec_from_file_location(
        "run_cxl_experiments_optimal", HERE / "run_cxl_experiments_optimal.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


rco = _load_runner()

# =============================================================================
# The matrix (DANE_NETWORK_PLAN.md, "The run matrix")
# =============================================================================

VARIANT = "arts_ocr_excl_retain_owner"
MACHINE = "dane"
SERIES = "network_rdma"
PROVIDER = "verbs;ofi_rxm"
CPUS_PER_TASK = 96          # worker + progress threads, either split
SW = ("{repo}/datasets/smithwaterman/string1-cmp.txt "
      "{repo}/datasets/smithwaterman/string2-cmp.txt "
      "{repo}/datasets/smithwaterman/score-cmp.txt")

# (app, args, workers, progress, per-run timeout s), in table order.
APPS: list[tuple[str, str, int, int, int]] = [
    ("cholesky_dist", "--ds 28800 --ts 300 --places 144", 94, 2, 300),
    ("CoMD_intel_chandra_tiled_hinted",
     "-x 108 -y 72 -z 72 -N 20 -n 10 -i 12 -j 8 -k 8", 94, 2, 300),
    ("fft_dist", "28 768 24", 94, 2, 300),
    ("fibonacci_hinted", "29 14", 94, 2, 300),
    ("graph500_dist", "21 16 128 128", 94, 2, 300),
    ("hpcg_intel_dist", "12 8 6 32 50", 92, 4, 1800),
    ("hpgmg_dist", "4 13824", 94, 2, 600),
    ("LCS_wavefront", "1048576 2048", 94, 2, 300),
    ("mapreduce_wordcount", "16384 2670000 {per_node:1128} 8 100 0 16 1",
     94, 2, 300),
    ("miniAMR_intel_dist",
     "--npx 12 --npy 8 --npz 8 --nx 16 --ny 16 --nz 16 --num_vars 40 "
     "--num_tsteps 10 --stages_per_ts 10 --num_refine 0 --checksum_freq 10 "
     "--report_diffusion 1", 94, 2, 300),
    ("nekbone_dist", "12 8 8 2 2 2 8 60", 94, 2, 300),
    ("nqueens_hinted", "19 15 1 4", 94, 2, 300),
    ("RSBench_intel_sharedDB_hinted", "-s large -l 1728000 -t 216 -p 24",
     94, 2, 300),
    ("smithwaterman_dist", f"100 100 {SW} 24", 92, 4, 1800),
    ("Stencil2D_intel_channelEVTs", "13824 1152 200", 94, 2, 300),
    ("stream_dist", "233280000 3456 100", 94, 2, 300),
    ("triangle_hinted", "7 1 8", 94, 2, 300),
    ("XSBench_intel_sharedDB", "-s large -g 48 -l 25000000 -t 64 -p 24",
     94, 2, 300),
]
APP_NAMES = [a[0] for a in APPS]

SMOKE_TIMEOUT_S = 120
SMOKE_NODES = [1, 2]
JOB_TIME = "02:00:00"
SMOKE_JOB_TIME = "00:30:00"

# A testbed archive is untracked; validation is skipped when it is absent.
TESTBED_DIR = DEFAULT_REPO / "results_isca27" / "arts-cxl-consolidated-20261116"

# Columns added after the shared BASE_COLUMNS of runs.csv.
DANE_COLUMNS = ["machine", "series", "provider", "slurm_job_id", "hostnames",
                "result_signature", "nonfinite_output", "provider_seen"]

# =============================================================================
# Result lines (the testbed consolidation's rule: build_dataset.py of
# arts-cxl-consolidated-20261116, which is untracked; its regexes are
# restated here, plus filters for this launcher's own lines)
# =============================================================================

RANK = re.compile(r"^\s*\[\d+\]\s?")
NONFINITE = re.compile(r"(?<![A-Za-z_])-?(nan|inf)(?![A-Za-z_])", re.I)
VOLATILE = re.compile(
    r"time|rate|MFlops|MB/s|elapsed|seconds|duration|throughput|startTime|perf|speed|/s\b|\bsec\b|usec|msec|"
    r"CXL|FAM|helper|TO RUN|LOCAL RUNNER|DONE!|Status:|affinity|Timer|Workload|pid|0x[0-9a-f]{6,}|"
    r"distributed across|threads per node|nodes?\b.*\bworkers?|GUID|Guid|\.bashrc|No such file|"
    r"^\w{3} \w{3} +\d+ \d\d:\d\d:\d\d \d{4}", re.I)
COMD_STEP = re.compile(r"^\d+\s+\d+\.\d+\s+-?\d+\.\d+\s+-?\d+\.\d+\s+-?\d+\.\d+\s+\d+\.\d+\s+\d+\.\d+\s+\d+$")
RESULT_ONLY = {
    "CoMD_intel_chandra_tiled_hinted": COMD_STEP,
    "graph500_dist": re.compile(r"^BFS_DIGEST|^\d+: \d+$|^nodes \d+ edges"),
    "hpgmg_dist": re.compile(r"\|\|error\|\|"),
    "nekbone_dist": re.compile(r"rnorminit|rnormfinal"),
}
# Runtime, srun and libfabric lines: never application output.
LAUNCHER_LINE = re.compile(
    r"^\[E2E\]|^\[\d+:\d+\] \[|^srun:|^slurmstepd|^libfabric:|^# ")
ANSI = re.compile(r"\x1b\[[0-9;]*m")
# libfabric:<pid>:<ts>:<layer>:<core>:<subsys>: -- a line logged by a core
# provider under a layering one names the stack the endpoint was opened on
# ("ofi_rxm" over "verbs" -> "verbs;ofi_rxm").  Bare registration probes
# (tcp, cxi, ...) log with an empty layer and are not counted.
FI_STACK = re.compile(r"^libfabric:\d+:\d+:(\w+):(\w+):\w+:", re.M)


def result_lines(app: str, text: str) -> list[str]:
    lines = []
    for raw in text.splitlines():
        raw = ANSI.sub("", raw)
        if LAUNCHER_LINE.search(raw):
            continue
        s = RANK.sub("", raw).strip()
        only = RESULT_ONLY.get(app)
        if only is not None:
            if not only.search(s):
                continue
        elif not s or s.startswith("*") or VOLATILE.search(s):
            continue
        if COMD_STEP.match(s):
            f = s.split()
            s = " ".join(f[:6] + f[7:])
        lines.append(s)
    return sorted(lines)


# =============================================================================
# Plan
# =============================================================================

class Campaign:
    def __init__(self, args):
        self.args = args
        self.repo = Path(args.repo_root).resolve()
        self.build = Path(args.build_dir).resolve()
        self.apps_dir = self.build / "benchmarks" / "apps"
        self.run_dir = (Path(args.results_root).resolve() if args.results_root
                        else self.repo / "results" / "dane_network") / args.run_id
        self.jobs_dir = self.run_dir / "jobs"
        rco.REPO_ROOT = self.repo          # expand_args() reads it
        rco.APP_BUILD_DIR = self.apps_dir

    # -- matrix -------------------------------------------------------------
    def matrix(self, smoke: bool, apps: list[str], nodes: list[int],
               reps: int) -> list[dict]:
        cells = []
        for k in range(reps):
            for n in nodes:
                for app, args, w, p, tmo in APPS:
                    if app not in apps:
                        continue
                    if smoke:
                        args, tmo = rco.SMOKE_APPS[app], SMOKE_TIMEOUT_S
                    c = rco._cell(app, args, VARIANT, n, k, warmup=False)
                    c.update(worker_threads=w, progress_threads=p,
                             timeout_s=tmo, job=f"n{n}_r{k}",
                             exe=str(self.apps_dir / c["executable"]))
                    cells.append(c)
        return cells

    def cell_dir(self, cell) -> Path:
        return self.run_dir / "cells" / cell["cell_id"]

    def load_plan(self) -> dict:
        path = self.run_dir / "plan.json"
        if not path.exists():
            sys.exit(f"no campaign at {self.run_dir} (plan.json missing)")
        return json.loads(path.read_text())

    # -- per-cell files -----------------------------------------------------
    def arts_cfg(self, cell) -> str:
        counters = self.cell_dir(cell) / "counters"
        return "\n".join([
            "[ARTS]",
            f"worker_threads={cell['worker_threads']}",
            f"progress_threads={cell['progress_threads']}",
            "launcher=slurm",
            f"node_count={cell['nodes']}",
            "pin=1",
            "route_table_size=16",
            "core_dump=0",
            # artsrun's dane render (configs/templates/arts.cfg.j2) writes the
            # worker stack explicitly: 8 MiB, the profile default.
            "stack_size=8388608",
            # Not a runtime key on cc27 (ignored); kept for parity with the
            # testbed cells.
            "port_count=1",
            "ports=25000",
            f"counter_folder={counters}",
            "counter_capture_interval=250",
            f"provider={PROVIDER}",
            "regpool_slab_mb=2048",
        ]) + "\n"

    def prepare_cell(self, cell):
        d = self.cell_dir(cell)
        (d / "counters").mkdir(parents=True, exist_ok=True)
        (d / "arts.cfg").write_text(self.arts_cfg(cell))
        (d / "args.json").write_text(json.dumps(
            {"app": cell["exe"], "args": cell["args"]}, indent=2) + "\n")

    # -- job scripts --------------------------------------------------------
    def job_script(self, job: str, cells: list[dict], out: Path) -> str:
        n = cells[0]["nodes"]
        a = self.args
        lines = [
            "#!/bin/bash",
            f"#SBATCH --partition={a.partition}",
            f"#SBATCH --nodes={n}",
            "#SBATCH --ntasks-per-node=1",
            f"#SBATCH --cpus-per-task={CPUS_PER_TASK}",
            "#SBATCH --exclusive",
            f"#SBATCH --time={a.time}",
            f"#SBATCH --job-name=arts-net-{job.replace('_', '-')}",
            f"#SBATCH --output={out}",
            "# No set -e: one failing or timed-out app must not stop the job.",
            'HOSTS=$(scontrol show hostnames "$SLURM_JOB_NODELIST" | paste -sd, -)',
            'echo "# slurm_job_id=$SLURM_JOB_ID"',
            'echo "# hostnames=$HOSTS"',
            'echo "# started=$(date -Is)"',
            "export ARTS_E2E_MARKER=1",
            "export ARTS_END_GRACE_S=30",
        ]
        lines += [f"export {k}={shlex.quote(v)}" for k, v in rco.EXTRA_ENV.items()]
        if a.smoke:
            # Release builds compile ARTS_INFO out, so libfabric's own log is
            # what shows the provider in use.  Smoke only: never in data runs.
            lines.append("export FI_LOG_LEVEL=info")
        for c in cells:
            d = self.cell_dir(c)
            argv = " ".join(shlex.quote(t) for t in shlex.split(c["args"]))
            lines += [
                "",
                f"# --- {c['cell_id']}",
                f"cd {shlex.quote(str(d))}",
                # Only unfinished or failed cells are ever in a script, so
                # this never clears a finished run.
                "rm -f done done.tmp counters/*.json output.log",
                'echo "$SLURM_JOB_ID $HOSTS" > job.txt',
                f"export ARTS_CONFIG={shlex.quote(str(d / 'arts.cfg'))}",
                f'echo "== $(date -Is) {c["cell_id"]}"',
                "s=$(date +%s.%N)",
                f"timeout -k 10 {c['timeout_s']} "
                f"srun --ntasks-per-node=1 -N {n} --cpus-per-task={CPUS_PER_TASK} "
                f"--kill-on-bad-exit=0 --wait=30 "
                f"{shlex.quote(c['exe'])} {argv} > output.log 2>&1",
                "rc=$?",
                'echo "$rc $s $(date +%s.%N)" > done.tmp && mv done.tmp done',
                'echo "   rc=$rc"',
            ]
        lines += ["", 'echo "# finished=$(date -Is)"', ""]
        return "\n".join(lines)

    # -- submission ---------------------------------------------------------
    def submissions(self) -> list[dict]:
        path = self.jobs_dir / "submissions.jsonl"
        if not path.exists():
            return []
        return [json.loads(l) for l in path.read_text().splitlines() if l.strip()]

    def submit_jobs(self, by_job: dict[str, list[dict]], dry: bool):
        self.jobs_dir.mkdir(parents=True, exist_ok=True)
        attempt = 1 + max([s["attempt"] for s in self.submissions()] or [0])
        for job, cells in by_job.items():
            for c in cells:
                self.prepare_cell(c)
            suffix = "" if attempt == 1 else f".a{attempt}"
            out = self.jobs_dir / f"{job}{suffix}.out"
            script = self.jobs_dir / f"{job}{suffix}.sh"
            script.write_text(self.job_script(job, cells, out))
            cmd = ["sbatch", "--parsable", str(script)]
            if dry:
                print("DRY:", shlex.join(cmd), f"  ({len(cells)} runs)")
                continue
            proc = subprocess.run(cmd, capture_output=True, text=True)
            if proc.returncode != 0:
                print(f"sbatch failed for {job}: {proc.stderr.strip()}")
                continue
            job_id = proc.stdout.strip().split(";")[0]
            print(f"submitted {job}: job {job_id} ({len(cells)} runs)")
            with open(self.jobs_dir / "submissions.jsonl", "a") as f:
                f.write(json.dumps({
                    "job": job, "job_id": job_id, "attempt": attempt,
                    "script": str(script), "out": str(out),
                    "cells": [c["cell_id"] for c in cells],
                    "submitted_at": dt.datetime.now().isoformat(timespec="seconds"),
                }) + "\n")

    def alive_jobs(self) -> set[str]:
        ids = [s["job_id"] for s in self.submissions()]
        if not ids:
            return set()
        proc = subprocess.run(["squeue", "-h", "-o", "%i", "-j", ",".join(ids)],
                              capture_output=True, text=True)
        return {l.strip() for l in proc.stdout.splitlines() if l.strip()}

    # -- collection ---------------------------------------------------------
    def collect_cell(self, cell) -> dict | None:
        d = self.cell_dir(cell)
        done = d / "done"
        if not done.exists():
            return None
        rc_s, s, e = done.read_text().split()[:3]
        rc, start, end = int(rc_s), float(s), float(e)
        log = d / "output.log"
        text = log.read_text(errors="replace") if log.exists() else ""
        stamps = [int(x) for x in rco.E2E_RE.findall(text)]
        per_rank = rco.read_rank_counters(d / "counters")
        complete = sorted(per_rank) == list(range(cell["nodes"]))
        if rc in (124, 137):
            status = "timeout"
        elif rc != 0:
            status = "failed"
        elif not stamps:
            status = "no_e2e"
        elif not complete:
            status = "counters_incomplete"
        else:
            status = "ok"
        started_at = dt.datetime.fromtimestamp(start).isoformat(timespec="seconds")
        rec = rco.build_record(cell, status, rc, stamps, end - start, per_rank,
                               started_at, wrapper_done=True)
        job_id, hosts = "", []
        jt = d / "job.txt"
        if jt.exists():
            parts = jt.read_text().split()
            job_id = parts[0] if parts else ""
            hosts = parts[1].split(",") if len(parts) > 1 else []
        lines = result_lines(cell["app"], text)
        provs = sorted({f"{core};{layer}" for layer, core in FI_STACK.findall(text)
                        if layer != core and "core" not in (layer, core)})
        rec.update({
            "hosts": hosts,
            "worker_threads": cell["worker_threads"],
            "progress_threads": cell["progress_threads"],
            "timeout_s": cell["timeout_s"],
            "machine": MACHINE,
            "series": SERIES,
            "provider": PROVIDER,
            "slurm_job_id": job_id,
            "hostnames": ",".join(hosts),
            "result_lines": lines,
            "result_signature": (hashlib.sha1("\n".join(lines).encode())
                                 .hexdigest()[:12] if lines else ""),
            "nonfinite_output": "yes" if any(NONFINITE.search(l) for l in lines) else "no",
            # Only populated when FI_LOG_LEVEL was set (smoke runs).
            "provider_seen": ";".join(provs),
            "log": str(log.relative_to(self.run_dir)),
            "counter_dir": str((d / "counters").relative_to(self.run_dir)),
        })
        return rec

    def statuses(self, cells) -> dict[str, str]:
        out = {}
        for c in cells:
            r = self.collect_cell(c)
            out[c["cell_id"]] = r["status"] if r else "not_run"
        return out


# =============================================================================
# Manifest
# =============================================================================

def _sh(cmd: str) -> str:
    try:
        return subprocess.run(["bash", "-lc", cmd], capture_output=True,
                              text=True, timeout=60).stdout.replace(
                                  "logout", "").strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def cmake_cache(build: Path) -> dict:
    keys = ("CMAKE_BUILD_TYPE", "ARTS_COUNTER_CONFIG", "CMAKE_C_COMPILER",
            "CMAKE_CXX_COMPILER", "ARTS_LOG_LEVEL", "ARTS_RELEASE_POLICY")
    out = {}
    path = build / "CMakeCache.txt"
    if path.exists():
        for line in path.read_text().splitlines():
            m = re.match(r"^([A-Za-z_]+):[A-Z]+=(.*)$", line)
            if m and m.group(1) in keys:
                out[m.group(1)] = m.group(2)
    return out


def write_manifest(camp: Campaign, plan: dict, extra: dict | None = None):
    path = camp.run_dir / "manifest.json"
    old = json.loads(path.read_text()) if path.exists() else {}
    cache = cmake_cache(camp.build)
    exe = next((c["exe"] for c in plan["cells"]), "")
    fi_line = _sh("fi_info -p verbs 2>&1 | grep -m3 -E 'provider|fabric|domain' "
                  "| tr -s ' \\n' ' '")
    manifest = {
        "created_at": old.get("created_at",
                              dt.datetime.now().isoformat(timespec="seconds")),
        "updated_at": dt.datetime.now().isoformat(timespec="seconds"),
        "commands": old.get("commands", []) + [shlex.join([sys.executable, *sys.argv])],
        "launch_host": socket.gethostname(),
        "machine": MACHINE,
        "series": SERIES,
        "git": old.get("git") or rco.git_info(),
        "build_dir": str(camp.build),
        "cmake_cache": cache,
        "module_list": _sh("module -t list 2>&1"),
        "fi_info_verbs": fi_line,
        "libfabric_system_version": _sh("fi_info --version 2>&1 | tr '\\n' ' '"),
        # libfabric is the vendored copy, linked statically into every binary.
        "libfabric_linked": " ".join(sorted(p.name for p in camp.build.glob("libfabric-*"))),
        "arts_cfg_vs_plan": {
            "added": {"stack_size": "8388608 (artsrun dane render)"},
            "note": "port_count is not a cc27 runtime key; kept for parity",
        },
        "counter_config_note": ("counters_cxl_ipdps27.cfg used unchanged"
                                if cache.get("ARTS_COUNTER_CONFIG", "").endswith(
                                    "counters_cxl_ipdps27.cfg") else
                                "see cmake_cache.ARTS_COUNTER_CONFIG"),
        "expected_counters": rco.EXPECTED_COUNTERS,
        "plan": {k: v for k, v in plan.items() if k != "cells"},
        "matrix": [{k: c[k] for k in ("cell_id", "app", "args", "nodes", "repeat",
                                      "worker_threads", "progress_threads",
                                      "timeout_s", "job", "exe")}
                   for c in plan["cells"]],
        "files": {
            "runs.jsonl": "one JSON record per finished run (rebuilt by --collect)",
            "runs.csv": "one row per finished run; ctr_<NAME> = sum over ranks",
            "counters_long.csv": "per-rank counter values, long format",
            "summary.csv": "per (app, nodes): e2e median/min/max vs the testbed",
            "validation.txt": "the plan's validation checks",
        },
    }
    if old.get("git") and old["git"] != rco.git_info():
        manifest["git_at_last_command"] = rco.git_info()
    if extra:
        manifest.update(extra)
    path.write_text(json.dumps(manifest, indent=2) + "\n")


# =============================================================================
# Validation
# =============================================================================

def _testbed():
    runs = TESTBED_DIR / "runs.csv"
    five = TESTBED_DIR / "e2e_five_series.csv"
    if not runs.exists() or not five.exists():
        return None, None
    ctr = collections.defaultdict(lambda: collections.defaultdict(list))
    with open(runs, newline="") as f:
        for r in csv.DictReader(f):
            if r["series"] != "network" or r.get("status") != "ok":
                continue
            if r.get("selected", "yes") != "yes":
                continue
            for k in ("ctr_NUM_EDT_FINISH", "ctr_NUM_DB_CREATE", "ctr_BYTES_DB_CREATE"):
                if r.get(k):
                    ctr[(r["app"], int(r["nodes"]))][k].append(float(r[k]))
    med = {}
    with open(five, newline="") as f:
        for r in csv.DictReader(f):
            if r.get("network_median_s"):
                med[(r["app"], int(r["nodes"]))] = float(r["network_median_s"])
    return ctr, med


def validate(camp: Campaign, plan: dict, recs: list[dict]):
    ok = [r for r in recs if r["status"] == "ok"]
    groups = collections.defaultdict(list)
    for r in ok:
        groups[(r["app"], r["nodes"])].append(r)
    # Smoke inputs are not the paper's: nothing to compare with the testbed.
    tb_ctr, tb_med = (None, None) if plan["smoke"] else _testbed()
    report = []
    flags = []

    n_cells = len(plan["cells"])
    counts = collections.Counter(r["status"] for r in recs)
    counts["not_run"] = n_cells - len(recs)
    report.append(f"cells planned: {n_cells}; " +
                  ", ".join(f"{k}={v}" for k, v in sorted(counts.items()) if v))
    reps = plan["reps"]
    short = [(a, n, len(groups.get((a, n), [])))
             for a in plan["apps"] for n in plan["nodes"]
             if len(groups.get((a, n), [])) != reps]
    for a, n, k in short:
        flags.append(f"{a} n{n}: {k}/{reps} ok runs")
    for r in ok:
        if r["ranks_reported"] != r["nodes"]:
            flags.append(f"{r['cell_id']}: ranks_reported={r['ranks_reported']}")

    rows = []
    for a in plan["apps"]:
        for n in plan["nodes"]:
            g = groups.get((a, n), [])
            e2e = [r["e2e_s"] for r in g if r["e2e_s"] is not None]
            sigs = collections.Counter(r["result_signature"] for r in g)
            nonfin = sum(r["nonfinite_output"] == "yes" for r in g)
            if len(sigs) > 1:
                flags.append(f"{a} n{n}: {len(sigs)} result signatures {dict(sigs)}")
            if nonfin:
                flags.append(f"{a} n{n}: {nonfin} runs print nan/inf")
            row = {"app": a, "nodes": n, "ok_runs": len(g),
                   "e2e_median_s": statistics.median(e2e) if e2e else None,
                   "e2e_min_s": min(e2e) if e2e else None,
                   "e2e_max_s": max(e2e) if e2e else None,
                   "result_signatures": len(sigs)}
            tb = tb_med.get((a, n)) if tb_med else None
            row["testbed_network_median_s"] = tb
            row["dane_over_testbed"] = (row["e2e_median_s"] / tb
                                        if tb and row["e2e_median_s"] else None)
            for k, col in (("ctr_NUM_EDT_FINISH", "edt_finish"),
                           ("ctr_NUM_DB_CREATE", "db_create"),
                           ("ctr_BYTES_DB_CREATE", "db_bytes")):
                name = k[4:]
                mine = [r["counters_cluster_sum"].get(name) for r in g]
                mine = [v for v in mine if v is not None]
                ref = tb_ctr.get((a, n), {}).get(k) if tb_ctr else None
                m = statistics.median(mine) if mine else None
                t = statistics.median(ref) if ref else None
                row[f"{col}_median"] = m
                row[f"testbed_{col}_median"] = t
                diff = (abs(m - t) / t if m is not None and t else None)
                row[f"{col}_rel_diff"] = diff
                if diff is not None and diff > 0.01 and col != "edt_finish":
                    flags.append(f"{a} n{n}: {name} differs from testbed by "
                                 f"{diff:.1%} ({m:.0f} vs {t:.0f}) -- input or "
                                 "program changed")
                elif diff is not None and diff > 0.01:
                    report.append(f"note {a} n{n}: {name} differs from testbed "
                                  f"by {diff:.1%}")
            rows.append(row)

    cols = list(rows[0]) if rows else []
    with open(camp.run_dir / "summary.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for row in rows:
            w.writerow({k: (f"{v:.6g}" if isinstance(v, float) else v)
                        for k, v in row.items()})
    if tb_ctr is None and not plan["smoke"]:
        report.append(f"testbed archive not found at {TESTBED_DIR}: "
                      "testbed comparisons skipped")
    text = "\n".join(report + ["", f"FLAGS ({len(flags)}):"] +
                     [f"  - {x}" for x in flags]) + "\n"
    (camp.run_dir / "validation.txt").write_text(text)
    return text


README = """# {run_id}: ISCA'27 Network series on LLNL Dane ({series})

Produced by experiment_scripts/submit_dane_network.py (plan:
experiment_scripts/DANE_NETWORK_PLAN.md).  manifest.json holds the commands,
git revision, build settings, modules, libfabric and the full matrix.

{n_cells} planned runs: {n_apps} apps x nodes {nodes} x {reps} reps{smoke}.

## Caveats

- Code differs from the paper: cc27's `arts_ocr_excl_retain_owner` vs
  counters-v1's `arts_ocr_excl_retain`.  Dane/testbed ratios mix network and
  code changes.
- CPUs differ: Dane is Sapphire Rapids, 2x56 cores; the testbed was Xeon
  Platinum 8568, 2x48 cores.  With pin=1 the 96 threads take cores 0-95: all
  of socket 0 and 40 cores of socket 1 (the testbed filled both sockets).
  The Release build (ARTS_LOG_LEVEL=1) does not print the binding it applies.
- Network differs: Omni-Path (hfi1) through libfabric `verbs;ofi_rxm` (the
  vendored libfabric, statically linked) vs TCP over 10 GbE.
- Threads: launcher=slurm takes the rank's thread total from
  SLURM_CPUS_PER_TASK=96, so workers = 96 - progress.  On 1 node the runtime
  reclaims the progress threads as workers (96 + 0), as on the testbed;
  runs.csv records the configured split.
- arts.cfg vs the testbed cells: no `nodes=` / `cxl_db_allocation_strategy`
  (launcher=slurm reads hosts from Slurm); `stack_size=8388608` added (what
  artsrun's dane profile renders); `port_count=1` kept for parity though cc27
  does not read it; provider=verbs;ofi_rxm instead of tcp.
- Counters: built with configs/counters_cxl_ipdps27.cfg unchanged (the CXL
  flush counters exist on cc27 and read zero in this network build).
- Each job is one (node count, rep) exclusive allocation running all apps of
  that rep in table order; `job.txt` in each cell holds the Slurm job id and
  hosts, also echoed at the top of jobs/<job>.out.

## Files

runs.jsonl, runs.csv, counters_long.csv (testbed schema plus machine,
series, provider, slurm_job_id, hostnames, result_signature,
nonfinite_output, provider_seen); summary.csv (per app and node count: e2e
median/min/max vs the testbed's network median, counter comparison);
validation.txt (the plan's checks; FLAGS lists anything to look at).
"""


def write_readme(camp: Campaign, plan: dict):
    (camp.run_dir / "README.md").write_text(README.format(
        run_id=plan["run_id"], series=SERIES, n_cells=len(plan["cells"]),
        n_apps=len(plan["apps"]), nodes=plan["nodes"], reps=plan["reps"],
        smoke=" (SMOKE inputs: not data)" if plan["smoke"] else ""))


# =============================================================================
# Commands
# =============================================================================

def preflight(camp: Campaign, cells) -> list[str]:
    errs = []
    for exe in sorted({c["exe"] for c in cells}):
        if not Path(exe).exists():
            errs.append(f"missing executable: {exe}")
    cfg = cmake_cache(camp.build).get("ARTS_COUNTER_CONFIG")
    if not cfg:
        errs.append(f"no ARTS_COUNTER_CONFIG in {camp.build}/CMakeCache.txt")
    missing = rco.stage_inputs(cells)
    errs += [f"input not staged: {p}" for p in missing]
    return errs


def set_expected_counters(camp: Campaign):
    cfg = cmake_cache(camp.build).get("ARTS_COUNTER_CONFIG")
    if cfg and Path(cfg).exists():
        rco.COUNTER_CONFIG = Path(cfg)
        rco.EXPECTED_COUNTERS = rco._configured_counters(Path(cfg))


def cmd_submit(camp: Campaign, a):
    resolve_job_settings(a, None)
    if (camp.jobs_dir / "submissions.jsonl").exists():
        sys.exit(f"{camp.run_dir} already has submissions: use --resume or "
                 "--collect, or pick a new --run-id")
    nodes = SMOKE_NODES if a.smoke else a.nodes
    reps = 1 if a.smoke else a.reps
    cells = camp.matrix(a.smoke, a.apps, nodes, reps)
    errs = preflight(camp, cells)
    if errs:
        print("\n".join(errs))
        if not a.dry_run:
            sys.exit("preflight failed")
    camp.run_dir.mkdir(parents=True, exist_ok=True)
    plan = {"run_id": a.run_id, "smoke": a.smoke, "apps": a.apps,
            "nodes": nodes, "reps": reps, "partition": a.partition,
            "time": a.time, "variant": VARIANT, "cpus_per_task": CPUS_PER_TASK,
            "repo_root": str(camp.repo), "build_dir": str(camp.build),
            "cells": cells}
    (camp.run_dir / "plan.json").write_text(json.dumps(plan, indent=2) + "\n")
    write_manifest(camp, plan)
    by_job = collections.OrderedDict()
    for c in cells:
        by_job.setdefault(c["job"], []).append(c)
    camp.submit_jobs(by_job, a.dry_run)
    if a.dry_run:
        print(f"dry run: {len(by_job)} job scripts in {camp.jobs_dir}")
        # A dry run leaves no submission behind, so the real submit may follow.


def cmd_resume(camp: Campaign, a):
    plan = camp.load_plan()
    resolve_job_settings(a, plan)
    alive = camp.alive_jobs()
    busy = {s["job"] for s in camp.submissions() if s["job_id"] in alive}
    st = camp.statuses(plan["cells"])
    by_job = collections.OrderedDict()
    for c in plan["cells"]:
        if st[c["cell_id"]] != "ok" and c["job"] not in busy:
            by_job.setdefault(c["job"], []).append(c)
    if busy:
        print(f"still queued/running, left alone: {sorted(busy)}")
    if not by_job:
        print("nothing to resubmit")
        return
    for job, cs in by_job.items():
        print(f"{job}: rerun {[c['app'] for c in cs]}")
    camp.submit_jobs(by_job, a.dry_run)
    if not a.dry_run:
        write_manifest(camp, plan)


def cmd_collect(camp: Campaign, a):
    plan = camp.load_plan()
    recs = [r for r in (camp.collect_cell(c) for c in plan["cells"]) if r]
    with open(camp.run_dir / "runs.jsonl.tmp", "w") as f:
        for r in recs:
            f.write(json.dumps(r) + "\n")
    (camp.run_dir / "runs.jsonl.tmp").replace(camp.run_dir / "runs.jsonl")
    if recs:
        rco.write_tables(camp.run_dir, recs)
    write_manifest(camp, plan)
    write_readme(camp, plan)
    print(validate(camp, plan, recs))
    print(f"collected {len(recs)} runs into {camp.run_dir}")


def parse_args():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--run-id", required=True)
    p.add_argument("--repo-root", default=str(DEFAULT_REPO))
    p.add_argument("--build-dir", default=None,
                   help="default: <repo>/build_dane_net")
    p.add_argument("--results-root", default=None,
                   help="default: <repo>/results/dane_network")
    p.add_argument("--partition", default=None,
                   help="default: pbatch (pdebug with --smoke)")
    p.add_argument("--time", default=None,
                   help=f"job walltime; default {JOB_TIME} ({SMOKE_JOB_TIME} smoke)")
    p.add_argument("--reps", type=int, default=10)
    p.add_argument("--nodes", default="1,2,3,4")
    p.add_argument("--apps", default=",".join(APP_NAMES))
    p.add_argument("--testbed-dir", default=None)
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--smoke", action="store_true")
    mode = p.add_mutually_exclusive_group()
    mode.add_argument("--resume", action="store_true")
    mode.add_argument("--collect", action="store_true")
    a = p.parse_args()
    a.nodes = [int(x) for x in a.nodes.split(",")]
    a.apps = [x for x in a.apps.split(",") if x]
    bad = [x for x in a.apps if x not in APP_NAMES]
    if bad:
        p.error(f"unknown apps: {bad}")
    a.apps = [x for x in APP_NAMES if x in a.apps]   # keep table order
    a.build_dir = a.build_dir or str(Path(a.repo_root) / "build_dane_net")
    return a


def resolve_job_settings(a, plan: dict | None):
    """Partition, walltime and smoke mode: explicit flags first, then the
    campaign's saved plan (on --resume), then the defaults."""
    if plan is not None:
        a.smoke = plan["smoke"]
        a.partition = a.partition or plan["partition"]
        a.time = a.time or plan["time"]
    a.partition = a.partition or ("pdebug" if a.smoke else "pbatch")
    a.time = a.time or (SMOKE_JOB_TIME if a.smoke else JOB_TIME)


def main():
    global TESTBED_DIR
    a = parse_args()
    if a.testbed_dir:
        TESTBED_DIR = Path(a.testbed_dir)
    camp = Campaign(a)
    set_expected_counters(camp)
    rco.BASE_COLUMNS = rco.BASE_COLUMNS + DANE_COLUMNS
    if a.collect:
        cmd_collect(camp, a)
    elif a.resume:
        cmd_resume(camp, a)
    else:
        cmd_submit(camp, a)


if __name__ == "__main__":
    main()
