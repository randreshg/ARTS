"""Run cells on this machine, one at a time.

A local campaign's ranks share one host's cores, so there is nothing to
overlap: the next cell starts when the last one has been reaped.
"""

from __future__ import annotations

import codecs
import os
import struct
import subprocess
import time
from enum import Enum
from pathlib import Path

from artsrun.check import measured_and_complete
from artsrun.model.profile import CxlLibrary, Profile
from artsrun.paths import scratch_dir
from artsrun.run.command import (build_command, build_env, cxl_wrap, render,
                                 with_post_verify, with_timeout)
from artsrun.run.markers import marker_status
from artsrun.run.types import Cell, CellResult, Status

# How long a cell may go on after it has produced its whole result.  Once the
# completion marker and the runtime's own end-to-end stamp are both in the log
# the campaign has everything it will read from that cell, so what follows is
# teardown and cannot change the verdict; a teardown that will not end is
# therefore not worth the rest of the cell's budget.  The grace has to clear
# the slowest honest teardown -- returning a large heap to the system and
# writing a counter set -- while staying far below any cell timeout, so an
# early reap only ever replaces waiting.
TEARDOWN_GRACE_S = 120

# The log is read while the cell runs, so each poll reads only what was
# appended since the last one; a file that has not grown cannot have gained a
# marker.
_POLL_S = 2.0

# A cell whose log grows past this is ended and fails.  No program's verdict
# needs that much output, and a runaway one (a rank looping on garbage after
# its peer died) would otherwise fill the disk, and the runner's memory, long
# before its budget ran out.
LOG_CAP_BYTES = 64 << 20


# The vendored fake CXL library keeps its one region in /dev/shm, which is RAM,
# and only the run that created it unlinks it, at a normal exit: a run that
# crashed or was reaped leaves it behind, holding every page it touched.  Its
# header opens with the library's magic, the bump cursor and the owner's pid.
FAKE_CXL_REGION = Path("/dev/shm/arts_fake_cxl")
_REGION_OWNER_WAIT_S = 2.0
_FAKE_CXL_MAGIC = 0x4152545343584C31


def _alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except (PermissionError, OverflowError):
        return True
    return True


def _try_drop(region: Path) -> bool | None:
    """True once unlinked, False when the region is not an abandoned one, None
    while its owner is still alive."""
    try:
        with region.open("rb") as f:
            opened = os.fstat(f.fileno())
            head = f.read(24)
            if len(head) < 24:
                return False
            magic, _cursor, owner = struct.unpack("<QQQ", head)
            if magic != _FAKE_CXL_MAGIC:
                return False
            if _alive(owner):
                return None
            named = os.stat(region)
            if (named.st_ino, named.st_dev) != (opened.st_ino, opened.st_dev):
                return False
            region.unlink()
    except OSError:
        return False
    return True


def drop_abandoned_fake_cxl_region(region: Path = FAKE_CXL_REGION,
                                   wait_s: float = 0.0) -> bool:
    """Unlink the fake library's region once no live process owns it.

    The header is read from the opened file and the name is unlinked only while
    it still refers to that file, which leaves a run that re-created the region
    since the check its own.  A process that was just killed stays visible to
    `kill(pid, 0)` until it is gone, so a live owner is re-checked, header and
    inode included, for up to `wait_s` seconds.
    """
    deadline = time.monotonic() + wait_s
    while True:
        done = _try_drop(region)
        if done is not None:
            return done
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.01)


class _End(Enum):
    EXITED = "exited"
    REAPED = "reaped"
    OVERFLOWED = "overflowed"


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
                end = self._wait(proc, cell, log_path)
            finally:
                self._current = None
        wall = time.monotonic() - started

        # A timed-out run can leave ranks behind: a wedged rank survives the
        # graceful path, and a survivor holds cores and ports for whatever runs
        # next.
        reap(cell.binary)
        if cell.cxl is CxlLibrary.FAKE:
            drop_abandoned_fake_cxl_region(wait_s=_REGION_OWNER_WAIT_S)

        # An early reap is a budget expiry like any other -- the cell was ended
        # by the runner, not by itself -- so it is reported as a timeout and
        # the checker decides, from the log alone, whether the run had already
        # measured itself.
        note = ""
        if end is _End.OVERFLOWED:
            status = Status.FAIL
            note = f"log passed {LOG_CAP_BYTES >> 20} MiB; the cell was ended"
        elif end is _End.REAPED:
            status = Status.TIMEOUT
        else:
            status = marker_status(proc.returncode, wall, cell.timeout_s)
        return CellResult(
            cell=cell, status=status, rc=proc.returncode,
            wall_s=wall, log_path=log_path, note=note,
        )

    def _wait(self, proc: subprocess.Popen, cell: Cell, log_path: Path) -> _End:
        """Wait for a cell, ending it once it is only tearing down, or once
        its log has passed the cap.

        A cell with a post-verify hook is never reaped early: its hook runs
        after the program and is part of the answer, so cutting the program
        short would decide the cell on a check that never ran.  The cap holds
        for every cell.
        """
        early = not cell.app.post_verify
        measured_at: float | None = None
        decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        text = ""
        offset = 0
        while True:
            try:
                proc.wait(timeout=_POLL_S)
                return _End.EXITED
            except subprocess.TimeoutExpired:
                pass
            try:
                size = log_path.stat().st_size
            except OSError:
                continue
            if size > LOG_CAP_BYTES:
                self._end(proc)
                return _End.OVERFLOWED
            if size < offset:
                offset = 0
                text = ""
                decoder.reset()
            if not early:
                continue
            if measured_at is None:
                if size > offset:
                    with log_path.open("rb") as log:
                        log.seek(offset)
                        chunk = log.read(size - offset)
                    offset += len(chunk)
                    text += decoder.decode(chunk)
                    if measured_and_complete(text, cell.app):
                        measured_at = time.monotonic()
            elif time.monotonic() - measured_at >= TEARDOWN_GRACE_S:
                self._end(proc)
                return _End.REAPED

    def _end(self, proc: subprocess.Popen) -> None:
        self.abort()
        # abort() escalates to a kill without collecting the child, so claim
        # the status here: the caller reads it.
        proc.wait()

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
