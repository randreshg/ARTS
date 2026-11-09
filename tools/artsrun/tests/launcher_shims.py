"""Fake sbatch, srun, squeue, sacct, scancel and flux on PATH.

`sbatch` and `flux batch` run the submitted script at once, with its output
in the file `--output` names; the script's own `srun` / `flux run` is the
step launch the plan scripts: one `{"out": str, "rc": int}` per launch, the
last one repeating.  Nothing is ever queued, so the queue and the accounting
answer empty and the job's own marker is the whole outcome.
"""

from __future__ import annotations

import json
import os
import stat
import sys
from dataclasses import dataclass
from pathlib import Path

_SHIM = r'''
import json, os, subprocess, sys
state = os.environ["SHIM_STATE"]
name, argv = sys.argv[1], sys.argv[2:]
with open(os.path.join(state, "calls.jsonl"), "a") as fh:
    fh.write(json.dumps({"cmd": name, "argv": argv}) + "\n")

def counter(which):
    path = os.path.join(state, which)
    n = int(open(path).read()) if os.path.exists(path) else 0
    open(path, "w").write(str(n + 1))
    return n

def step():
    plan = json.load(open(os.path.join(state, "plan.json")))
    one = plan[min(counter("steps"), len(plan) - 1)]
    sys.stdout.write(one["out"])
    sys.exit(one["rc"])

def output(args):
    for a in args:
        if a.startswith("--output="):
            return a[len("--output="):]

def run(script, out, prefix):
    n = counter("jobs")
    with open(out, "w") as fh:
        subprocess.run(["bash", script], stdout=fh, stderr=subprocess.STDOUT)
    print(prefix + str(n + 1))

if name == "srun" or (name == "flux" and argv[:1] == ["run"]):
    step()
elif name == "sbatch":
    n = counter("scripts")
    script = os.path.join(state, f"job{n}.sh")
    open(script, "w").write(sys.stdin.read())
    run(script, output(argv), "")
elif name == "flux" and argv[:1] == ["batch"]:
    run(argv[-1], output(argv), "f")
sys.exit(0)
'''


@dataclass
class Shims:
    state: Path

    def argv(self, cmd: str, sub: str | None = None) -> list[list[str]]:
        """Every argument list `cmd` (or `flux <sub>`) was called with."""
        out = []
        path = self.state / "calls.jsonl"
        for line in path.read_text().splitlines() if path.exists() else []:
            call = json.loads(line)
            if call["cmd"] != cmd:
                continue
            if sub is not None and call["argv"][:1] != [sub]:
                continue
            out.append(call["argv"])
        return out


def install(tmp_path: Path, monkeypatch, plan: list[dict]) -> Shims:
    state = tmp_path / "shim-state"
    binary = tmp_path / "shim-bin"
    state.mkdir()
    binary.mkdir()
    (state / "plan.json").write_text(json.dumps(plan))
    (state / "shim.py").write_text(_SHIM)
    for cmd in ("sbatch", "srun", "squeue", "sacct", "scancel", "flux"):
        path = binary / cmd
        path.write_text(
            f'#!/bin/sh\nexec {sys.executable} "{state}/shim.py" {cmd} "$@"\n')
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("SHIM_STATE", str(state))
    monkeypatch.setenv("PATH", f"{binary}{os.pathsep}{os.environ['PATH']}")
    return Shims(state)
