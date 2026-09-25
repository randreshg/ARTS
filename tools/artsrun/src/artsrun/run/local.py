"""Run cells on this machine, one at a time.

A local campaign's ranks share one host's cores, so there is nothing to
overlap: the next cell starts when the last one has been reaped.
"""

from __future__ import annotations

import os
import subprocess
import time
from pathlib import Path

from artsrun.check import measured_and_complete
from artsrun.model.profile import Profile
from artsrun.paths import scratch_dir
from artsrun.run.command import (build_command, build_env, cxl_wrap, render,
                                 with_post_verify, with_timeout)
from artsrun.run.types import Cell, CellResult, Status

TIMEOUT_RC = 124

# How long a cell may go on after it has produced its whole result.  Once the
# completion marker and the runtime's own end-to-end stamp are both in the log
# the campaign has everything it will read from that cell, so what follows is
# teardown and cannot change the verdict; a teardown that will not end is
# therefore not worth the rest of the cell's budget.  The grace has to clear
# the slowest honest teardown -- returning a large heap to the system and
# writing a counter set -- while staying far below any cell timeout, so an
# early reap only ever replaces waiting.
TEARDOWN_GRACE_S = 120

# Re-reading the log has to be cheap enough to do while a cell runs; a file
# that has not grown cannot have gained a marker, so its size gates the read.
_POLL_S = 2.0


def reap(binary: Path) -> int:
    """Kill leftovers of one executable.

    Matched by /proc/<pid>/exe, never by name: comm is truncated at 15
    characters and a name match can also hit the caller.
    """
    killed = 0
    target = binary.resolve()
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if entry.joinpath("exe").resolve() != target:
                continue
            os.kill(int(entry.name), 9)
            killed += 1
        except (OSError, PermissionError):
            continue
    return killed


class LocalBackend:
    """Foreground execution; `submit` returns only when the cell is done."""

    def __init__(self, profile: Profile, log_dir: Path):
        self.profile = profile
        self.log_dir = log_dir
        self.log_dir.mkdir(parents=True, exist_ok=True)
        self.capacity = 1
        # submit() blocks for the whole run, so a start can only be announced
        # from inside it; whoever drives the backend hangs a callback here.
        self.notify = lambda kind, result: None
        # The cell in progress, so a stop from another thread can end it.
        # submit() blocks for the whole run, so there is nowhere else to reach
        # the process from.
        self._current: subprocess.Popen | None = None

    def cost(self, cell: Cell) -> int:
        return 1

    def submit(self, cell: Cell) -> CellResult:
        argv = with_timeout(
            with_post_verify(cxl_wrap(build_command(cell, self.profile), cell, self.profile), cell),
            cell.timeout_s)
        env = os.environ.copy()
        env.update(build_env(cell, self.profile, self.log_dir))
        log_path = self.log_dir / cell.log_name
        cwd = scratch_dir()
        cwd.mkdir(parents=True, exist_ok=True)

        started = time.monotonic()
        with log_path.open("w", encoding="utf-8", errors="replace") as log:
            log.write(f"$ {render(argv)}\n")
            log.flush()
            proc = subprocess.Popen(
                argv, cwd=cwd, env=env, stdout=log,
                stderr=subprocess.STDOUT,
            )
            self._current = proc
            self.notify("started", CellResult(
                cell=cell, status=Status.RUNNING, log_path=log_path,
                extra={"pid": str(proc.pid)},
            ))
            try:
                reaped = self._wait(proc, cell, log_path)
            finally:
                self._current = None
        wall = time.monotonic() - started

        # A timed-out run can leave ranks behind: a wedged rank survives the
        # graceful path, and a survivor holds cores and ports for whatever runs
        # next.
        reap(cell.binary)

        # An early reap is a budget expiry like any other -- the cell was ended
        # by the runner, not by itself -- so it is reported as a timeout and
        # the checker decides, from the log alone, whether the run had already
        # measured itself.
        status = Status.TIMEOUT if (reaped or proc.returncode == TIMEOUT_RC) else (
            Status.OK if proc.returncode == 0 else Status.FAIL
        )
        return CellResult(
            cell=cell, status=status, rc=proc.returncode,
            wall_s=wall, log_path=log_path,
        )

    def _wait(self, proc: subprocess.Popen, cell: Cell, log_path: Path) -> bool:
        """Wait for a cell, ending it once it is only tearing down.

        Returns whether the cell was reaped rather than having exited.  A cell
        with a post-verify hook is never reaped early: its hook runs after the
        program and is part of the answer, so cutting the program short would
        decide the cell on a check that never ran.
        """
        early = not cell.app.post_verify
        measured_at: float | None = None
        size = -1
        while True:
            try:
                proc.wait(timeout=_POLL_S)
                return False
            except subprocess.TimeoutExpired:
                pass
            if not early:
                continue
            if measured_at is None:
                try:
                    grew = log_path.stat().st_size != size
                except OSError:
                    continue
                if grew:
                    size = log_path.stat().st_size
                    text = log_path.read_text(encoding="utf-8", errors="replace")
                    if measured_and_complete(text, cell.app):
                        measured_at = time.monotonic()
            elif time.monotonic() - measured_at >= TEARDOWN_GRACE_S:
                self.abort()
                # abort() escalates to a kill without collecting the child, so
                # claim the status here: the caller reads it.
                proc.wait()
                return True

    def poll(self, result: CellResult) -> CellResult:
        return result

    def abort(self) -> None:
        """End the cell in progress, from whichever thread asks.

        The command runs under `timeout`, which forwards a term to the
        program, so terminating it stops the run the way a budget expiry
        would; the reap that follows every cell then clears any rank that
        outlived it.
        """
        proc = self._current
        if proc is None or proc.poll() is not None:
            return
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    def shutdown(self) -> None:
        self.abort()
