#!/usr/bin/env python3
"""Failure and performance analysis of run_cxl_experiments.py campaigns.

Compares network ARTS (arts_ocr_excl_purge) against CXL-DIRECT ARTS
(arts_ocr_excl_purge_cxl_direct) from one or more campaign directories
(each holding manifest.json, runs.jsonl and cells/<cell_id>/output.log).

Part 1, failures: every run that did not finish cleanly, with its mode of
failure read from the log (timeout split into "killed mid-run" and "hung in
shutdown", crash signal and the crashing frames, abnormal exit), plus cells
that were interrupted or never run because a campaign stopped early.

Part 2, metrics: every completed run's e2e and wall time, the figures the
applications print themselves (MTEPS, lookups/s, MFLOPS, ...) and, for runs
built with counters, the cluster-summed ARTS counters and the metrics
run_cxl_experiments.derive() builds from them (CXL protocol bandwidth, flush
latencies, network payload rate, ...), in one pandas DataFrame, aggregated
per (app, variant, nodes).  Per-rank counter values go to a second, long-format
DataFrame.

Part 3, plots: end-to-end time, strong scaling, CXL-vs-network speedup,
application-reported throughput, wall-clock overhead, a failure map, and from
the counters: data-movement rate, CXL fetch/purge bandwidth and flush latency.

Counters are optional.  Counter files are read from each cell's counters/
directory (falling back to the record in runs.jsonl); a run whose ranks did
not all write one has no cluster sums.  The runner's status "check_failed" is
ignored when its only failed check is missing counter files
("rank_files:..."), as in campaigns built without counters.

A run present in several campaign directories (same variant, cell and start
time, e.g. a campaign directory and a later copy extended with --resume) is
counted once, keeping the copy that has counters.

Usage:

  python experiment_scripts/analyze_cxl_experiments.py prelim_results/*/
  python experiment_scripts/analyze_cxl_experiments.py \
      prelim_results/working-no-cxl prelim_results/medium-20261003-132959 \
      prelim_results/counting-20261006-073226
  python experiment_scripts/analyze_cxl_experiments.py NET_DIR CXL_DIR -o out/
"""

from __future__ import annotations

import argparse
import json
import re
import signal
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402
from matplotlib.colors import BoundaryNorm, ListedColormap, TwoSlopeNorm  # noqa: E402
from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter  # noqa: E402

# Counter reading and the derived metrics are the campaign runner's own, so the
# two scripts cannot disagree on what a metric means.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_cxl_experiments import (  # noqa: E402
    DERIVED_COLUMNS, EXPECTED_COUNTERS, derive, read_rank_counters)

NET, CXL = "arts_ocr_excl_purge", "arts_ocr_excl_purge_cxl_direct"
LABEL = {NET: "Network", CXL: "CXL"}
COLOR = {NET: "#2a78d6", CXL: "#eb6834"}  # categorical slots 1 and 2
MARKER = {NET: "o", CXL: "s"}
INK, INK2, MUTED, GRID = "#0b0b0b", "#52514e", "#898781", "#e1e0d9"
# Fetch (consumer flush) and purge (producer flush): categorical slots 3 and 7,
# so they never collide with the Network/CXL colours.
FETCH_COLOR, PURGE_COLOR = "#1baf7a", "#4a3aa7"

# A timeout whose E2E stamp lands this many seconds before the kill finished
# the application and then hung in shutdown; otherwise it was killed mid-run
# (the runtime stamps E2E as it goes down on SIGTERM).
SHUTDOWN_HANG_MARGIN_S = 10.0

E2E_RE = re.compile(r"^\[E2E\]\s+(\d+)\s*$", re.M)
DONE_RE = re.compile(r"^DONE!\s+(-?\d+)", re.M)
CRASH_RE = re.compile(r"^\[ARTS\] Crashed: (\w+) \(rank (\d+)\)", re.M)
FRAME_RE = re.compile(r"\(([A-Za-z_][\w]*)\+0x[0-9a-f]+\)")
RANK_PREFIX_RE = re.compile(r"\s*\[\d+\]\s*")

# Lines that are the site banner or runtime chatter, not application output.
NOISE_RE = re.compile(
    r"^(\*+|TO RUN:|Status: FamStatus|LOCAL RUNNER:|#|\[E2E\]|DONE!|$)"
    r"|CXL FAM|CXL DB allocation")
BANNER_END = "By logging on to this system you agree"

# Application-reported metrics: column -> (regex, unit, higher_is_better).
APP_METRICS = {
    "mteps": (r"mean MTEPS\s+([\d.]+)", "MTEPS", True),
    "g500_kernel1_s": (r"\[kernel1 time ([\d.]+)\]", "s", False),
    "g500_kernel2_s": (r"\[kernel2 time ([\d.]+)\]", "s", False),
    "lookups_per_s": (r"Lookups/s:\s+(?:\[\d+\]\s*)?([\d.]+)", "lookups/s", True),
    "mflops": (r"MFLOPS total\s*=\s*([\d.]+)", "MFLOPS", True),
    "cg_time_s": (r"Time in seconds\s*=\s*([\d.]+)", "s", False),
    "atoms_per_us": (r"Average atom rate:\s+([\d.]+) atoms/us", "atoms/us", True),
    "nqueens_ops": (r"Throughput\s+\(op/s\):\s+([\d.]+)", "op/s", True),
    "lcs_runtime_s": (r"runtime:\s+([\d.]+)", "s", False),
    "amr_compute_s": (r"Compute phase elapsed time is ([\d.]+) seconds", "s", False),
    "hpgmg_time_s": (r"Total Time =\s+([\d.]+)", "s", False),
    "bandwidth_GBps": (r"([\d.]+)\s*GB/s", "GB/s", True),
    "bandwidth_MBps": (r"([\d.]+)\s*MB/s", "MB/s", True),
}
WORDCOUNT_RE = re.compile(r"TOKENS=(\d+)")

# Lines carrying an application's answer; they must agree across runs and
# across variants for the same input.
ANSWER_RES = [re.compile(p) for p in (
    r"checksum\s*[=:].*", r"CHECKSUM=\d+", r"BFS_DIGEST \d+", r"sols: \d+",
    r"answer is \d+", r"^score: \d+", r"LCS length: \d+", r"trace = [\d.]+",
    r"VERIFICATION \w+", r"final residual norm = \S+", r"rnormfinal =\s*\S+",
    r"XS Checksum:\s+\d+", r"Verification\s*=\s*\w+",
)]


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

def load_campaign(run_dir: Path) -> tuple[dict, list[dict]]:
    manifest = json.loads((run_dir / "manifest.json").read_text())
    with open(run_dir / "runs.jsonl") as f:
        records = [json.loads(line) for line in f if line.strip()]
    latest = {}
    for r in records:  # a retried cell keeps its latest record
        latest[r["cell_id"]] = r
    return manifest, list(latest.values())


def read_log(run_dir: Path, rec: dict) -> str:
    path = run_dir / rec.get("log", f"cells/{rec['cell_id']}/output.log")
    try:
        return path.read_text(errors="replace")
    except OSError:
        return ""


def app_lines(text: str) -> list[str]:
    """Application output: after the site banner, without runtime chatter or
    rank prefixes."""
    i = text.find(BANNER_END)
    body = text[i:].split("\n", 1)[1] if i >= 0 else text
    out = []
    for line in body.splitlines():
        line = RANK_PREFIX_RE.sub(" ", line).strip()
        if line and not NOISE_RE.search(line) and not line.startswith("[ARTS]") \
                and not line.startswith("/"):
            out.append(line)
    return out


# ---------------------------------------------------------------------------
# Part 1: failures
# ---------------------------------------------------------------------------

def signal_name(rc) -> str | None:
    """Signal behind a return code: negative (killed by Python) or the shell's
    256-sig / 128+sig encodings."""
    if rc is None:
        return None
    for sig in (-rc, 256 - rc, rc - 128):
        if 0 < sig < 65:
            try:
                return signal.Signals(sig).name
            except ValueError:
                pass
    return None


def classify(rec: dict, text: str, timeout_s: float) -> dict:
    """Outcome of one run: category ('ok', 'timeout', 'crash', 'abnormal_exit',
    'no_e2e'), a finer mode, and a human-readable detail."""
    status, rc = rec["status"], rec["return_code"]
    stamps = [int(s) for s in E2E_RE.findall(text)]
    e2e_s = stamps[0] / 1e9 if stamps else None
    done = DONE_RE.search(text)
    app_rc = int(done.group(1)) if done else None
    crash = CRASH_RE.search(text)
    lines = app_lines(text)
    out = {"e2e_s": e2e_s, "app_rc": app_rc, "app_output_lines": len(lines),
           "last_app_line": lines[-1][:120] if lines else ""}

    # check_failed is ignored when the only failed checks are counter files.
    other_checks = [c for c in rec.get("checks_failed", [])
                    if not c.startswith("rank_files:")]

    if status == "timeout":
        out["category"] = "timeout"
        if e2e_s is not None and rec["wall_s"] - e2e_s > SHUTDOWN_HANG_MARGIN_S:
            out["mode"] = "timeout: hung in shutdown"
            out["detail"] = (f"app finished at e2e={e2e_s:.1f}s, still alive at "
                             f"the {timeout_s:.0f}s budget")
        elif len(lines) == 0:
            out["mode"] = "timeout: killed mid-run, no app output"
            out["detail"] = "nothing printed by the application before the kill"
        else:
            out["mode"] = "timeout: killed mid-run"
            out["detail"] = f"last output: {out['last_app_line']}"
        return out

    if crash or (app_rc is not None and app_rc < 0) or (rc not in (0, None)):
        sig = (crash.group(1) if crash else None) or signal_name(app_rc) \
            or signal_name(rc)
        frames = FRAME_RE.findall(text[crash.end():]) if crash else []
        frames = [f for f in frames if f not in ("__libc_start_main",)][:3]
        phase = "after E2E (teardown)" if e2e_s is not None else "before E2E"
        out["category"] = "crash" if sig else "abnormal_exit"
        out["mode"] = f"{sig or f'exit {rc}'} {phase}"
        where = " <- ".join(frames) if frames else "no stack trace"
        out["detail"] = (f"rank {crash.group(2)}: {where}" if crash else where)
        if e2e_s is not None:
            out["detail"] += f"; e2e={e2e_s:.1f}s wall={rec['wall_s']:.1f}s"
        return out

    if e2e_s is None:
        out.update(category="no_e2e", mode="no E2E stamp",
                   detail="exited 0 without an [E2E] line")
        return out
    if other_checks:
        out.update(category="check_failed", mode="sanity check failed",
                   detail=";".join(other_checks))
        return out
    out.update(category="ok", mode="ok", detail="")
    return out


def answers(text: str) -> str:
    """The answer lines of a run, normalised, as one signature string."""
    found = []
    for line in app_lines(text):
        for rx in ANSWER_RES:
            m = rx.search(line)
            if m:
                found.append(re.sub(r"\s+", " ", m.group(0)))
    return " | ".join(dict.fromkeys(found))


# ---------------------------------------------------------------------------
# Part 2: metrics
# ---------------------------------------------------------------------------

def app_metrics(text: str, e2e_s: float | None) -> dict:
    out = {}
    for col, (rx, _, _) in APP_METRICS.items():
        vals = [float(v) for v in re.findall(rx, text)]
        out[col] = vals[-1] if vals else np.nan
    m = WORDCOUNT_RE.search(text)
    out["wordcount_tokens_per_s"] = (int(m.group(1)) / e2e_s
                                     if m and e2e_s else np.nan)
    return out


METRIC_INFO = {**{k: (u, hib) for k, (_, u, hib) in APP_METRICS.items()},
               "wordcount_tokens_per_s": ("tokens/s (from e2e)", True)}


def run_counters(run_dir: Path, rec: dict) -> dict[int, dict[str, int]]:
    """{rank: {counter: value}}: the cell's counter files, else the record's."""
    counter_dir = run_dir / rec.get("counter_dir",
                                    f"cells/{rec['cell_id']}/counters")
    per_rank = read_rank_counters(counter_dir) if counter_dir.is_dir() else {}
    if not per_rank:
        per_rank = {int(r): v for r, v in
                    rec.get("counters_per_rank", {}).items()}
    return per_rank


def imbalance(per_rank: dict, name: str) -> float:
    """max / mean of one counter over ranks (1.0 = perfectly balanced)."""
    vals = [v.get(name, 0) for v in per_rank.values()]
    if len(vals) < 2 or not sum(vals):
        return np.nan
    return max(vals) / (sum(vals) / len(vals))


def counter_columns(per_rank: dict, nodes: int, e2e_s) -> dict:
    """Cluster sums (ctr_<NAME>) and derived metrics of one run.  As in the
    runner, sums are only published when every rank reported."""
    complete = bool(per_rank) and sorted(per_rank) == list(range(nodes))
    names = sorted({n for v in per_rank.values() for n in v})
    sums = ({n: sum(v.get(n, 0) for v in per_rank.values()) for n in names}
            if complete else {})
    out = {
        "has_counters": bool(per_rank),
        "ranks_reported": len(per_rank),
        "counters_complete": complete,
        "missing_counters": (";".join(c for c in EXPECTED_COUNTERS
                                      if c not in names) if per_rank else ""),
        "edt_exec_imbalance": imbalance(per_rank, "TIME_EDT_EXEC"),
        "cxl_fetch_bytes_imbalance": imbalance(per_rank,
                                               "BYTES_CXL_FLUSH_CONSUMER"),
    }
    derived = derive(sums, e2e_s)
    out.update({k: np.nan if v is None else v for k, v in derived.items()})
    out.update({f"ctr_{n}": v for n, v in sums.items()})
    return out


def build_frames(run_dirs: list[Path], include_warmup: bool):
    rows, missing, long_rows = [], [], []
    for run_dir in run_dirs:
        manifest, records = load_campaign(run_dir)
        timeout_s = manifest["config"].get("cell_timeout_s", np.nan)
        campaign = run_dir.name
        recorded = {r["cell_id"] for r in records}
        # Cells a campaign never finished: a directory without a record was
        # running when the campaign stopped; the rest never started.
        for cell_id in manifest["cells"]:
            if cell_id in recorded:
                continue
            app, variant, n, rep = cell_id.split("__")
            started = (run_dir / "cells" / cell_id).is_dir()
            missing.append({
                "campaign": campaign, "cell_id": cell_id, "app": app,
                "variant": variant, "nodes": int(n[1:]), "repeat": int(rep[1:]),
                "category": "interrupted" if started else "not_run",
                "mode": ("interrupted: campaign stopped while running"
                         if started else "not run: campaign stopped early"),
            })
        for rec in records:
            if rec.get("is_warmup") and not include_warmup:
                continue
            text = read_log(run_dir, rec)
            cls = classify(rec, text, timeout_s)
            row = {
                "campaign": campaign, "cell_id": rec["cell_id"],
                "app": rec["app"], "variant": rec["variant"],
                "system": LABEL.get(rec["variant"], rec["variant"]),
                "args": rec["args"], "nodes": rec["nodes"],
                "repeat": rec["repeat"], "is_warmup": rec["is_warmup"],
                "hosts": ",".join(rec.get("hosts", [])),
                "worker_threads": rec.get("worker_threads"),
                "started_at": rec["started_at"], "runner_status": rec["status"],
                "return_code": rec["return_code"], "wall_s": rec["wall_s"],
                "timeout_s": timeout_s, **cls,
                "answer": answers(text) if cls["category"] != "timeout" else "",
                "log": str(run_dir / rec.get("log", "")),
            }
            row["overhead_s"] = (row["wall_s"] - row["e2e_s"]
                                 if row["e2e_s"] is not None else np.nan)
            row.update(app_metrics(text, cls["e2e_s"]))
            per_rank = run_counters(run_dir, rec)
            row.update(counter_columns(per_rank, rec["nodes"], cls["e2e_s"]))
            rows.append(row)
            for rank, vals in sorted(per_rank.items()):
                for name, value in sorted(vals.items()):
                    long_rows.append({
                        "campaign": campaign, "cell_id": rec["cell_id"],
                        "app": rec["app"], "variant": rec["variant"],
                        "system": row["system"], "nodes": rec["nodes"],
                        "repeat": rec["repeat"],
                        "started_at": rec["started_at"],
                        "category": cls["category"], "rank": rank,
                        "counter": name, "value": value})
    runs = pd.DataFrame(rows)
    runs["e2e_s"] = runs["e2e_s"].astype(float)
    counters_long = pd.DataFrame(long_rows, columns=[
        "campaign", "cell_id", "app", "variant", "system", "nodes", "repeat",
        "started_at", "category", "rank", "counter", "value"])

    # One run seen through several campaign directories counts once; keep the
    # copy with counters, then the one from the later directory.
    key = ["variant", "cell_id", "started_at"]
    runs["_order"] = range(len(runs))
    runs = runs.sort_values(["has_counters", "_order"])
    dup = runs.duplicated(key, keep="last")
    if dup.any():
        print(f"note: {dup.sum()} runs appear in more than one campaign "
              f"directory and are counted once")
    runs = runs[~dup].sort_values("_order").drop(columns="_order")
    keep = set(zip(runs.campaign, runs.cell_id, runs.started_at))
    counters_long = counters_long[[
        k in keep for k in zip(counters_long.campaign, counters_long.cell_id,
                               counters_long.started_at)]]
    return runs.reset_index(drop=True), pd.DataFrame(missing), counters_long


def aggregate(ok: pd.DataFrame) -> pd.DataFrame:
    metric_cols = [c for c in [*METRIC_INFO, *DERIVED_COLUMNS,
                               "edt_exec_imbalance",
                               "cxl_fetch_bytes_imbalance"]
                   if ok[c].notna().any()]
    agg = ok.groupby(["app", "variant", "system", "nodes"]).agg(
        runs=("e2e_s", "size"),
        e2e_median_s=("e2e_s", "median"), e2e_mean_s=("e2e_s", "mean"),
        e2e_std_s=("e2e_s", "std"), e2e_min_s=("e2e_s", "min"),
        e2e_max_s=("e2e_s", "max"), wall_median_s=("wall_s", "median"),
        overhead_median_s=("overhead_s", "median"),
        counter_runs=("counters_complete", "sum"),
        **{f"{c}_median": (c, "median") for c in metric_cols},
    ).reset_index()
    agg["e2e_cv"] = agg["e2e_std_s"] / agg["e2e_mean_s"]
    # Strong scaling relative to the same variant's 1-node median.
    base = agg[agg["nodes"] == 1].set_index(["app", "variant"])["e2e_median_s"]
    key = list(zip(agg["app"], agg["variant"]))
    agg["scaling_speedup"] = [base.get(k, np.nan) for k in key] / agg["e2e_median_s"]
    agg["parallel_efficiency"] = agg["scaling_speedup"] / agg["nodes"]
    return agg


def compare(agg: pd.DataFrame) -> pd.DataFrame:
    """CXL against network at matched (app, nodes); >1 means CXL is faster."""
    wide = agg.pivot_table(index=["app", "nodes"], columns="variant",
                           values="e2e_median_s")
    if NET not in wide or CXL not in wide:
        return pd.DataFrame()
    wide = wide[[NET, CXL]].dropna().rename(
        columns={NET: "net_e2e_median_s", CXL: "cxl_e2e_median_s"})
    wide["cxl_speedup"] = wide["net_e2e_median_s"] / wide["cxl_e2e_median_s"]
    return wide.reset_index()


# ---------------------------------------------------------------------------
# Part 3: plots
# ---------------------------------------------------------------------------

def style():
    plt.rcParams.update({
        "figure.facecolor": "#fcfcfb", "axes.facecolor": "#fcfcfb",
        "axes.edgecolor": "#c3c2b7", "axes.labelcolor": INK2,
        "axes.titlecolor": INK, "axes.titlesize": 9, "axes.labelsize": 8,
        "xtick.color": MUTED, "ytick.color": MUTED, "xtick.labelsize": 7,
        "ytick.labelsize": 7, "axes.grid": True, "grid.color": GRID,
        "grid.linewidth": 0.6, "axes.spines.top": False,
        "axes.spines.right": False, "legend.frameon": False,
        "legend.fontsize": 8, "font.family": "sans-serif",
        "savefig.dpi": 150, "savefig.bbox": "tight",
    })


def plain_log(axis):
    """Log scale with plain-number ticks (no 2.8x10^2 labels)."""
    axis.set_major_locator(LogLocator(subs=(1, 2, 5)))
    axis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}"))
    axis.set_minor_formatter(NullFormatter())


def grid_axes(n, ncols=6, w=2.6, h=2.1):
    nrows = int(np.ceil(n / ncols))
    fig, axes = plt.subplots(nrows, ncols, figsize=(w * ncols, h * nrows),
                             squeeze=False)
    for ax in axes.flat[n:]:
        ax.set_visible(False)
    return fig, axes.flat


def legend_top(fig, title, series=None, failures=True):
    """Title plus a one-row legend; series = [(label, color, marker), ...]
    (default: the two systems)."""
    series = series or [(LABEL[v], COLOR[v], MARKER[v]) for v in (NET, CXL)]
    handles = [plt.Line2D([], [], color=c, marker=m, lw=2, ms=6, label=lab)
               for lab, c, m in series]
    if failures:
        handles.append(plt.Line2D([], [], color="#d03b3b", marker="x", lw=0,
                                  ms=7, mew=2, label="failed / timed out run"))
    fig.suptitle(title, x=0.01, ha="left", fontsize=12, color=INK, y=1.0)
    fig.legend(handles=handles, loc="upper right", ncol=3,
               bbox_to_anchor=(0.99, 1.01))


def plot_e2e(runs, agg, out):
    apps = sorted(runs["app"].unique(), key=str.lower)
    fig, axes = grid_axes(len(apps))
    for ax, app in zip(axes, apps):
        for v in (NET, CXL):
            a = agg[(agg.app == app) & (agg.variant == v)].sort_values("nodes")
            if not a.empty:
                ax.errorbar(a.nodes, a.e2e_median_s,
                            yerr=np.vstack([a.e2e_median_s - a.e2e_min_s,
                                            a.e2e_max_s - a.e2e_median_s]),
                            color=COLOR[v], marker=MARKER[v], ms=5, lw=2,
                            capsize=2, elinewidth=1)
            bad = runs[(runs.app == app) & (runs.variant == v)
                       & (runs.category != "ok")]
            if not bad.empty:
                y = bad.e2e_s.fillna(bad.wall_s)
                ax.scatter(bad.nodes, y, marker="x", color="#d03b3b", s=30,
                           lw=1.5, zorder=5)
        ax.set_title(app, loc="left")
        ax.set_yscale("log")
        plain_log(ax.yaxis)
        ax.set_xticks(sorted(runs.nodes.unique()))
        ax.set_xlabel("nodes")
        ax.set_ylabel("e2e (s)")
    legend_top(fig, "End-to-end time (median, whiskers = min/max), log scale")
    fig.tight_layout()
    fig.savefig(out / "e2e_time.png")
    plt.close(fig)


def plot_scaling(agg, out):
    apps = sorted(agg["app"].unique(), key=str.lower)
    nmax = agg.nodes.max()
    fig, axes = grid_axes(len(apps))
    for ax, app in zip(axes, apps):
        ax.plot([1, nmax], [1, nmax], color=MUTED, lw=1, ls="--")
        ax.axhline(1, color="#c3c2b7", lw=1)
        for v in (NET, CXL):
            a = agg[(agg.app == app) & (agg.variant == v)].sort_values("nodes")
            if a.scaling_speedup.notna().any():
                ax.plot(a.nodes, a.scaling_speedup, color=COLOR[v],
                        marker=MARKER[v], ms=5, lw=2)
        ax.set_title(app, loc="left")
        ax.set_xticks(range(1, nmax + 1))
        ax.set_xlabel("nodes")
        ax.set_ylabel("speedup vs 1 node")
    legend_top(fig, "Strong scaling: T(1 node) / T(n nodes); dashed = ideal",
               failures=False)
    fig.tight_layout()
    fig.savefig(out / "strong_scaling.png")
    plt.close(fig)


def plot_speedup_heatmap(cmp, out):
    if cmp.empty:
        return
    wide = cmp.pivot(index="app", columns="nodes", values="cxl_speedup")
    wide = wide.loc[sorted(wide.index, key=str.lower)]
    logv = np.log2(wide.values)
    lim = max(1.0, np.nanmax(np.abs(logv)))
    # Diverging blue (network faster) <-> red (CXL faster), gray midpoint.
    cmap = matplotlib.colors.LinearSegmentedColormap.from_list(
        "div", ["#1c5cab", "#86b6ef", "#f0efec", "#f09a99", "#b02a2a"])
    fig, ax = plt.subplots(figsize=(1.3 * wide.shape[1] + 3.5,
                                    0.32 * len(wide) + 1.4))
    im = ax.imshow(logv, cmap=cmap, norm=TwoSlopeNorm(0, -lim, lim),
                   aspect="auto")
    for (i, j), val in np.ndenumerate(wide.values):
        if not np.isnan(val):
            ax.text(j, i, f"{val:.2f}x", ha="center", va="center", fontsize=7,
                    color=INK if abs(np.log2(val)) < lim * 0.6 else "white")
    ax.set_xticks(range(wide.shape[1]), [f"{n} node{'s' * (int(n) > 1)}"
                                         for n in wide.columns])
    ax.set_yticks(range(len(wide)), wide.index)
    ax.grid(False)
    cb = fig.colorbar(im, ax=ax, shrink=0.6)
    ticks = [t for t in (-4, -3, -2, -1, 0, 1, 2, 3, 4) if abs(t) <= lim]
    cb.set_ticks(ticks, labels=[f"{2.0 ** t:g}x" for t in ticks])
    cb.set_label("network e2e / CXL e2e")
    ax.set_title("CXL speedup over network (median e2e); >1x (red) = CXL "
                 "faster, <1x (blue) = network faster", loc="left")
    fig.savefig(out / "cxl_vs_network_speedup.png")
    plt.close(fig)


def plot_e2e_bars(agg, cmp, out):
    """Paired bars at each matched node count."""
    if cmp.empty:
        return
    nodes = sorted(cmp.nodes.unique())
    fig, axes = plt.subplots(1, len(nodes), figsize=(5 * len(nodes), 8),
                             sharey=True, squeeze=False)
    apps = sorted(cmp.app.unique(), key=str.lower)
    y = np.arange(len(apps))
    for ax, n in zip(axes[0], nodes):
        c = cmp[cmp.nodes == n].set_index("app").reindex(apps)
        ax.barh(y - 0.2, c.net_e2e_median_s, 0.38, color=COLOR[NET],
                label=LABEL[NET])
        ax.barh(y + 0.2, c.cxl_e2e_median_s, 0.38, color=COLOR[CXL],
                label=LABEL[CXL])
        ax.set_xscale("log")
        plain_log(ax.xaxis)
        ax.set_title(f"{n} node{'s' * (int(n) > 1)}", loc="left")
        ax.set_xlabel("median e2e (s, log)")
        ax.grid(axis="y", visible=False)
    axes[0][0].set_yticks(y, apps)
    axes[0][0].invert_yaxis()
    axes[0][-1].legend(loc="lower right")
    fig.suptitle("Median e2e time, apps with completed runs on both systems",
                 x=0.01, ha="left", fontsize=12)
    fig.tight_layout()
    fig.savefig(out / "e2e_paired_bars.png")
    plt.close(fig)


def plot_app_metrics(agg, out):
    """Application-reported rates (the only throughput/bandwidth figures in
    these runs, since they carry no counters)."""
    panels = []
    for col, (unit, hib) in METRIC_INFO.items():
        mcol = f"{col}_median"
        if mcol not in agg or not hib:
            continue
        for app in sorted(agg.loc[agg[mcol].notna(), "app"].unique(),
                          key=str.lower):
            panels.append((app, col, unit))
    if not panels:
        return
    fig, axes = grid_axes(len(panels), ncols=4, w=3.0, h=2.3)
    for ax, (app, col, unit) in zip(axes, panels):
        for v in (NET, CXL):
            a = agg[(agg.app == app) & (agg.variant == v)].sort_values("nodes")
            a = a[a[f"{col}_median"].notna()]
            if not a.empty:
                ax.plot(a.nodes, a[f"{col}_median"], color=COLOR[v],
                        marker=MARKER[v], ms=5, lw=2)
        ax.set_title(f"{app}", loc="left")
        ax.set_ylabel(f"{unit} (higher = better)")
        ax.set_xlabel("nodes")
        ax.set_xticks(sorted(agg.nodes.unique()))
        ax.set_ylim(bottom=0)
    legend_top(fig, "Application-reported throughput (median)",
               failures=False)
    fig.tight_layout()
    fig.savefig(out / "app_throughput.png")
    plt.close(fig)


def plot_overhead(ok, out):
    """wall - e2e: launch, CXL region setup/teardown and runtime init/exit."""
    nodes = sorted(ok.nodes.unique())
    fig, ax = plt.subplots(figsize=(6.5, 3.2))
    for k, v in enumerate((NET, CXL)):
        data = [ok[(ok.variant == v) & (ok.nodes == n)].overhead_s.dropna()
                for n in nodes]
        pos = [n + (k - 0.5) * 0.3 for n in nodes]
        keep = [(p, d) for p, d in zip(pos, data) if len(d)]
        if not keep:
            continue
        bp = ax.boxplot([d for _, d in keep], positions=[p for p, _ in keep],
                        widths=0.25, patch_artist=True, showfliers=True,
                        medianprops={"color": INK})
        for b in bp["boxes"]:
            b.set(facecolor=COLOR[v], edgecolor=COLOR[v], alpha=0.85)
        for el in ("whiskers", "caps"):
            for line in bp[el]:
                line.set_color(COLOR[v])
        for f in bp["fliers"]:
            f.set(marker=MARKER[v], markeredgecolor=COLOR[v], markersize=4)
        ax.plot([], [], color=COLOR[v], lw=6, label=LABEL[v])
    ax.set_xticks(nodes, [str(n) for n in nodes])
    ax.set_xlabel("nodes")
    ax.set_ylabel("wall - e2e (s)")
    ax.set_title("Per-run overhead outside the E2E window (launch, init, "
                 "teardown) - completed runs", loc="left")
    ax.legend(loc="upper left")
    fig.savefig(out / "overhead.png")
    plt.close(fig)


def counter_grid(agg, panels, title, ylabel, out, name, log=False,
                 series=None):
    """Small multiples, one per app with data: panels = [(column, label,
    color, marker, variant or None), ...] drawn against nodes."""
    have = [f"{c}_median" for c, *_ in panels if f"{c}_median" in agg]
    if not have:
        return
    apps = sorted(agg.loc[agg[have].notna().any(axis=1), "app"].unique(),
                  key=str.lower)
    if not apps:
        return
    fig, axes = grid_axes(len(apps))
    for ax, app in zip(axes, apps):
        for col, _, color, marker, variant in panels:
            mcol = f"{col}_median"
            if mcol not in agg:
                continue
            a = agg[(agg.app == app)
                    & ((agg.variant == variant) if variant else True)]
            a = a[a[mcol].notna()].sort_values("nodes")
            if a.empty:
                continue
            ax.plot(a.nodes, a[mcol], color=color, marker=marker, ms=5, lw=2)
        ax.set_title(app, loc="left")
        ax.set_xticks(sorted(agg.nodes.unique()))
        ax.set_xlabel("nodes")
        ax.set_ylabel(ylabel)
        if log:
            ax.set_yscale("log")
            plain_log(ax.yaxis)
        else:
            ax.set_ylim(bottom=0)
    legend_top(fig, title, series=series or [(lab, c, m)
                                            for _, lab, c, m, _ in panels],
               failures=False)
    fig.tight_layout()
    fig.savefig(out / name)
    plt.close(fig)


def plot_counters(agg, out):
    """Counter-derived plots; each is skipped when no run has its counters."""
    # Bytes/s columns are plotted in GB/s.
    agg = agg.copy()
    for c in agg.columns:
        if c.endswith("Bps_median"):
            agg[c] = agg[c] / 1e9
    for c in ("mean_cxl_flush_consumer_ns_median",
              "mean_cxl_flush_producer_ns_median",
              "mean_db_payload_put_ns_median"):
        if c in agg:
            agg[c] = agg[c] / 1e3
    counter_grid(
        agg, [("cxl_protocol_total_Bps", "CXL: fetch+purge bytes / e2e",
               COLOR[CXL], MARKER[CXL], CXL),
              ("network_payload_Bps", "Network: DB payload bytes / e2e",
               COLOR[NET], MARKER[NET], NET)],
        "Aggregate data-movement rate (cluster bytes moved / e2e, median)",
        "GB/s", out, "data_rate.png")
    counter_grid(
        agg, [("cxl_protocol_read_Bps", "fetch (consumer flush) bytes / e2e",
               FETCH_COLOR, "o", CXL),
              ("cxl_protocol_write_Bps", "purge (producer flush) bytes / e2e",
               PURGE_COLOR, "s", CXL)],
        "CXL protocol bandwidth by direction (median)", "GB/s", out,
        "cxl_read_write_bandwidth.png")
    counter_grid(
        agg, [("mean_cxl_flush_consumer_ns", "consumer flush (fetch)",
               FETCH_COLOR, "o", CXL),
              ("mean_cxl_flush_producer_ns", "producer flush (purge)",
               PURGE_COLOR, "s", CXL)],
        "Mean CXL flush latency per operation (median over runs), log scale",
        "us per flush", out, "cxl_flush_latency.png", log=True)
    counter_grid(
        agg, [("cxl_flush_consumer_op_Bps", "consumer flush (fetch)",
               FETCH_COLOR, "o", CXL),
              ("cxl_flush_producer_op_Bps", "producer flush (purge)",
               PURGE_COLOR, "s", CXL)],
        "Per-operation CXL flush throughput (bytes / flush time), log scale",
        "GB/s", out, "cxl_flush_op_throughput.png", log=True)


CATEGORY_ORDER = ["ok", "crash", "abnormal_exit", "timeout", "no_e2e",
                  "check_failed", "interrupted", "not_run"]
CATEGORY_COLOR = {"ok": "#0ca30c", "crash": "#d03b3b",
                  "abnormal_exit": "#d03b3b", "timeout": "#ec835a",
                  "no_e2e": "#fab219", "check_failed": "#fab219",
                  "interrupted": "#c3c2b7", "not_run": "#f0efec"}


def plot_failure_map(runs, missing, out):
    """Worst outcome per (app, variant, nodes) cell; text = failed/total."""
    allr = pd.concat([runs[["app", "variant", "nodes", "category"]],
                      missing[["app", "variant", "nodes", "category"]]
                      if not missing.empty else None])
    rank = {c: i for i, c in enumerate(CATEGORY_ORDER)}
    allr["cat_rank"] = allr.category.map(rank)

    def worst(g):
        bad = g[~g.category.isin(["ok", "not_run"])]
        pick = bad if not bad.empty else g
        return pd.Series({
            "cat": pick.loc[pick.cat_rank.idxmin() if bad.empty else
                            pick.cat_rank.idxmin(), "category"],
            "n_bad": len(g[~g.category.isin(["ok", "not_run"])]),
            "n": len(g[g.category != "not_run"]),
        })

    cells = allr.groupby(["app", "variant", "nodes"]).apply(
        worst, include_groups=False).reset_index()
    # Ranks to show the most severe present: crash before timeout etc.
    cells["col"] = [f"{LABEL.get(v, v)}\n{n}n" for v, n in
                    zip(cells.variant, cells.nodes)]
    col_order = [f"{LABEL[v]}\n{n}n" for v in (NET, CXL)
                 for n in sorted(cells.nodes.unique())
                 if ((cells.variant == v) & (cells.nodes == n)).any()]
    apps = sorted(cells.app.unique(), key=str.lower)
    present = [c for c in CATEGORY_ORDER if c in set(cells.cat)]
    idx = {c: i for i, c in enumerate(present)}
    grid = np.full((len(apps), len(col_order)), np.nan)
    text = [[""] * len(col_order) for _ in apps]
    for r in cells.itertuples():
        i, j = apps.index(r.app), col_order.index(r.col)
        grid[i, j] = idx[r.cat]
        if r.n:
            text[i][j] = f"{r.n_bad}/{r.n}" if r.n_bad else ""
    cmap = ListedColormap([CATEGORY_COLOR[c] for c in present])
    cmap.set_bad("#fcfcfb")
    fig, ax = plt.subplots(figsize=(0.75 * len(col_order) + 4,
                                    0.3 * len(apps) + 1.6))
    ax.imshow(np.ma.masked_invalid(grid), cmap=cmap, aspect="auto",
              norm=BoundaryNorm(np.arange(len(present) + 1) - 0.5, len(present)))
    for i, row in enumerate(text):
        for j, t in enumerate(row):
            if t:
                ax.text(j, i, t, ha="center", va="center", fontsize=7,
                        color="white")
    ax.set_xticks(range(len(col_order)), col_order)
    ax.set_yticks(range(len(apps)), apps)
    ax.set_xticks(np.arange(len(col_order) + 1) - 0.5, minor=True)
    ax.set_yticks(np.arange(len(apps) + 1) - 0.5, minor=True)
    ax.grid(which="minor", color="#fcfcfb", lw=2)
    ax.grid(which="major", visible=False)
    ax.tick_params(which="minor", length=0)
    handles = [plt.Rectangle((0, 0), 1, 1, color=CATEGORY_COLOR[c],
                             label=c.replace("_", " ")) for c in present]
    ax.legend(handles=handles, loc="upper left", bbox_to_anchor=(1.01, 1))
    ax.set_title("Outcome per cell (worst run; label = failed/recorded runs; "
                 "blank = no such cell)", loc="left")
    fig.savefig(out / "failure_map.png")
    plt.close(fig)


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

def hr(title):
    print(f"\n{'=' * 78}\n{title}\n{'=' * 78}")


def report_failures(runs, missing, out):
    hr("PART 1: FAILURES")
    for campaign, g in runs.groupby("campaign"):
        m = missing[missing.campaign == campaign] if not missing.empty else missing
        print(f"\n{campaign} [{', '.join(sorted(g.system.unique()))}]: "
              f"{len(g)} recorded runs; "
              + ", ".join(f"{k}={v}" for k, v in
                          g.category.value_counts().items())
              + (f"; {len(m[m.category == 'interrupted'])} interrupted, "
                 f"{len(m[m.category == 'not_run'])} never run"
                 if len(m) else ""))
    fails = runs[runs.category != "ok"].sort_values(
        ["system", "category", "app", "nodes", "repeat"])
    cols = ["system", "app", "nodes", "repeat", "category", "mode",
            "return_code", "e2e_s", "wall_s", "detail", "log"]
    fails[cols].to_csv(out / "failures.csv", index=False)
    print(f"\nFailed runs ({len(fails)}):")
    with pd.option_context("display.max_rows", None, "display.width", 250,
                           "display.max_colwidth", 95):
        print(fails[cols[:-1]].to_string(index=False,
                                         float_format=lambda x: f"{x:.1f}"))
    print("\nBy mode of failure:")
    print(fails.groupby(["system", "mode"]).size().to_string())
    if not missing.empty:
        missing.to_csv(out / "missing_cells.csv", index=False)
        for campaign, m in missing.groupby("campaign"):
            inter = m[m.category == "interrupted"].cell_id.tolist()
            never = m[m.category == "not_run"]
            print(f"\n{campaign}: campaign stopped early.")
            if inter:
                print(f"  interrupted while running: {', '.join(inter)}")
            print(f"  {len(never)} planned cells never ran; apps with no run "
                  f"at all: {', '.join(sorted(set(never.app) - set(runs[runs.campaign == campaign].app)))}")
            print(f"  repeats never reached: "
                  f"{sorted(set(never.repeat) - set(runs[runs.campaign == campaign].repeat))}")


def report_answers(runs, out):
    """Same input at the same node count should print the same answer on
    every run and on both systems.  (Node counts are compared separately:
    some inputs, e.g. mapreduce_wordcount, scale the problem with nodes.)"""
    done = runs[(runs.category == "ok") | (runs.e2e_s.notna()
                                           & (runs.category == "crash"))]
    rows = []
    for (app, args, nodes), g in done.groupby(["app", "args", "nodes"]):
        sigs = g[g.answer != ""].groupby("answer")
        if sigs.ngroups > 1:
            for sig, s in sigs:
                rows.append({"app": app, "nodes": nodes, "answer": sig,
                             "runs": ", ".join(f"{a}/n{n}/r{r}" for a, n, r in
                                               zip(s.system, s.nodes, s.repeat))})
    hr("ANSWER CONSISTENCY (same input and node count, different answer)")
    if not rows:
        (out / "answer_mismatches.csv").unlink(missing_ok=True)
        print("Every completed run printed the same answer lines as the other "
              "runs (either system) of its app at its node count.")
        return
    df = pd.DataFrame(rows)
    df.to_csv(out / "answer_mismatches.csv", index=False)
    for (app, nodes), g in df.groupby(["app", "nodes"]):
        print(f"\n{app} @ {nodes} nodes:")
        for r in g.itertuples():
            print(f"  [{r.runs[:110]}]\n      {r.answer[:150]}")


def report_metrics(ok, agg, cmp):
    hr("PART 2: PERFORMANCE (completed runs only)")
    print(f"{len(ok)} completed runs -> {len(agg)} (app, system, nodes) cells")
    with pd.option_context("display.max_rows", None, "display.width", 200):
        if not cmp.empty:
            print("\nCXL speedup over network at matched node counts "
                  "(network e2e / CXL e2e; >1 = CXL faster):")
            wide = cmp.pivot(index="app", columns="nodes", values="cxl_speedup")
            wide["geomean"] = np.exp(np.log(wide).mean(axis=1))
            print(wide.round(2).to_string())
            print("\nGeomean CXL speedup per node count: " + ", ".join(
                f"{n}n={np.exp(np.log(g.cxl_speedup).mean()):.2f}x "
                f"({len(g)} apps)" for n, g in cmp.groupby("nodes")))
        print("\nStrong-scaling speedup T(1)/T(max common n):")
        sc = agg.pivot_table(index="app", columns=["system", "nodes"],
                             values="scaling_speedup")
        print(sc.round(2).to_string())
        print("\nRun-to-run variability (cells with CV > 10%):")
        noisy = agg[agg.e2e_cv > 0.10][["app", "system", "nodes", "runs",
                                        "e2e_median_s", "e2e_cv"]]
        print(noisy.round(3).to_string(index=False) if len(noisy) else "none")
        print("\nMedian overhead (wall - e2e) by system and nodes, s:")
        print(ok.groupby(["system", "nodes"]).overhead_s.median()
              .unstack().round(1).to_string())


def report_counters(runs, ok, agg, counters_long):
    hr("COUNTERS")
    cov = runs.groupby(["campaign", "system"]).agg(
        runs=("cell_id", "size"), with_counter_files=("has_counters", "sum"),
        all_ranks_reported=("counters_complete", "sum"))
    print(cov.to_string())
    if not runs.counters_complete.any():
        print("No run has a complete set of counter files.")
        return
    partial = runs[runs.has_counters & ~runs.counters_complete]
    if len(partial):
        print(f"\n{len(partial)} runs with counter files from only some ranks "
              f"(no cluster sums): " + ", ".join(partial.cell_id[:8])
              + (" ..." if len(partial) > 8 else ""))
    missing = runs[runs.counters_complete & (runs.missing_counters != "")]
    if len(missing):
        print(f"\n{len(missing)} runs lack expected counters, e.g. "
              f"{missing.iloc[0].cell_id}: {missing.iloc[0].missing_counters}")

    cols = {"e2e_median_s": "e2e s",
            "cxl_protocol_total_bytes_median": "CXL GB",
            "cxl_protocol_total_Bps_median": "CXL GB/s",
            "mean_cxl_flush_consumer_ns_median": "fetch us",
            "mean_cxl_flush_producer_ns_median": "purge us",
            "network_payload_bytes_median": "net GB",
            "network_payload_Bps_median": "net GB/s",
            "edt_exec_imbalance_median": "EDT imbal"}
    t = agg[agg.counter_runs > 0][["app", "system", "nodes",
                                   *[c for c in cols if c in agg]]].copy()
    for c in t.columns:
        if c.endswith(("bytes_median", "Bps_median")):
            t[c] = t[c] / 1e9
        if c.endswith("ns_median"):
            t[c] = t[c] / 1e3
    t = t.rename(columns=cols)
    print("\nPer cell, completed runs with counters (medians; GB = 1e9 B; "
          "imbal = max/mean rank EDT time):")
    with pd.option_context("display.max_rows", None, "display.width", 200):
        print(t.to_string(index=False, float_format=lambda x: f"{x:.2f}"))
    net_cxl = runs[(runs.variant == CXL) & runs.counters_complete
                   & (runs.network_payload_bytes > 0)]
    if len(net_cxl):
        print(f"\n{len(net_cxl)} CXL runs still sent datablock payloads over "
              f"the network: " + ", ".join(sorted(set(net_cxl.app))))
    print(f"\nPer-rank counters: {len(counters_long)} rows "
          f"({counters_long.counter.nunique()} counters) in counters_long.csv")


def report_campaign_timing(ok):
    """The same variant measured in several campaigns (e.g. with and without
    counters compiled in): geomean e2e ratio at shared (app, nodes) cells."""
    med = ok.groupby(["variant", "campaign", "app", "nodes"]).e2e_s.median()
    lines = []
    for variant, g in med.groupby(level="variant"):
        camps = sorted(g.index.get_level_values("campaign").unique())
        for i, a in enumerate(camps):
            for b in camps[i + 1:]:
                x = g.xs(a, level="campaign").droplevel("variant")
                y = g.xs(b, level="campaign").droplevel("variant")
                both = x.index.intersection(y.index)
                if len(both):
                    r = np.exp(np.log(y[both] / x[both]).mean())
                    lines.append(f"  {LABEL.get(variant, variant)}: {b} / {a} "
                                 f"= {r:.2f}x geomean e2e over {len(both)} "
                                 f"shared cells")
    if lines:
        hr("SAME VARIANT ACROSS CAMPAIGNS (timings are pooled; check they agree)")
        print("\n".join(lines))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("run_dirs", nargs="+", type=Path,
                   help="campaign directories (each with manifest.json and "
                        "runs.jsonl); network and CXL may be in different ones")
    p.add_argument("-o", "--out", type=Path,
                   help="output directory (default: <parent of first "
                        "run_dir>/analysis)")
    p.add_argument("--include-warmup", action="store_true",
                   help="keep warm-up runs (dropped by default)")
    args = p.parse_args()

    run_dirs = [d.resolve() for d in args.run_dirs]
    for d in run_dirs:
        if not (d / "runs.jsonl").exists():
            sys.exit(f"not a campaign directory: {d}")
    out = (args.out or run_dirs[0].parent / "analysis").resolve()
    out.mkdir(parents=True, exist_ok=True)

    runs, missing, counters_long = build_frames(run_dirs, args.include_warmup)
    ok = runs[runs.category == "ok"].copy()
    agg = aggregate(ok)
    cmp = compare(agg)

    runs.to_csv(out / "runs_all.csv", index=False)
    runs.to_pickle(out / "runs_all.pkl")
    agg.to_csv(out / "summary_by_cell.csv", index=False)
    cmp.to_csv(out / "cxl_vs_network.csv", index=False)
    counters_long.to_csv(out / "counters_long.csv", index=False)
    counters_long.to_pickle(out / "counters_long.pkl")

    report_failures(runs, missing, out)
    report_answers(runs, out)
    report_metrics(ok, agg, cmp)
    report_campaign_timing(ok)
    report_counters(runs, ok, agg, counters_long)

    style()
    plot_failure_map(runs, missing, out)
    plot_e2e(runs, agg, out)
    plot_e2e_bars(agg, cmp, out)
    plot_scaling(agg, out)
    plot_speedup_heatmap(cmp, out)
    plot_app_metrics(agg, out)
    plot_overhead(ok, out)
    plot_counters(agg, out)
    hr("OUTPUT")
    for f in sorted(out.iterdir()):
        print(f"  {f}")


if __name__ == "__main__":
    main()
