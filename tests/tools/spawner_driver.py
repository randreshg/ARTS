#!/usr/bin/env python3
"""Runs a multi-rank test program under its spawner and judges the run.

    spawner_driver.py [options] -- <program> [args...]

The program's combined output is read line by line, each line stamped with
this process's monotonic clock on arrival and written to this process's
stdout as it arrives, so a caller that matches output sees every line.  The
ranks name their pids in lines matching --pid-regex (two groups: the rank,
the pid); the driver polls every known rank pid every --poll-ms milliseconds.
A rank's end lies between the last poll that saw it alive and the first that
saw it gone (a zombie counts as gone): a lower bound on a time measured from
an end is taken from the former, an upper bound from the latter.

One action may be given: when its trigger fires (--on pids: every one of
--ranks ranks named its pid; --on line:<regex>: a line matches; --on launch:
at once), the driver sends --signal to --target (spawner, or rank:<i> once
that rank named its pid).  Each --then <SIGNAL>:<target>:<when> adds a step
after it, in order: the driver sends that signal to that target <when> ms
after the previous step fired, or, with <when> others-gone, once every rank
but the target's is gone (the target a rank).

The run passes when the spawner's status (an exit status, or 128 + the signal
that ended it) is one of --want (or --want any), the action fired if one was
given, and every requested check holds:
  --dead-after-ms <n>      n ms after the action, no rank pid is alive;
  --line-window <regex> <lo> <hi>
                           a line matches, lo..hi ms after the first rank end;
  --absent <path>          once the run ended, path does not exist;
  --bound-ms <n>           the time from --bound-from (launch, the driver's
                           start of the program; signal:<NAME>, the driver's
                           sending of that signal; first-end, the first rank
                           end) to the end of the last rank of --bound-ranks
                           (all, or except:<i>; with no rank pid named, the
                           spawner's end, which follows every rank's) is at
                           most n ms.  The default start is the action's
                           signal when one is given, else first-end.
--repeat <n> runs the program n times in sequence, judging each run.  On every
exit path the driver kills the spawner if it still runs and then every process
still running the program's executable, so a stopped rank never outlives the
test.  Exit status 0 when every run passed, 1 otherwise.
"""

import argparse
import atexit
import os
import queue
import re
import signal
import subprocess
import sys
import threading
import time

PROCS = []
EXE = None


def say(text):
    sys.stdout.write("spawner_driver: " + text + "\n")
    sys.stdout.flush()


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    try:
        with open("/proc/%d/stat" % pid) as f:
            stat = f.read()
    except OSError:
        return False
    return stat[stat.rindex(")") + 2] not in "ZX"


def runs_exe(pid):
    try:
        return os.path.realpath("/proc/%d/exe" % pid) == EXE
    except OSError:
        return False


def leftovers():
    found = []
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        pid = int(name)
        if pid != os.getpid() and runs_exe(pid) and alive(pid):
            found.append(pid)
    return found


def reap_everything():
    for proc in PROCS:
        if proc.poll() is None:
            try:
                proc.kill()
            except OSError:
                pass
    if EXE is None:
        return
    for pid in leftovers():
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


def on_signal(signum, frame):
    reap_everything()
    os._exit(128 + signum)


def signal_name(name):
    return name[3:] if name.startswith("SIG") else name


def status_of(returncode):
    return 128 - returncode if returncode < 0 else returncode


def reader(stream, lines):
    for raw in iter(stream.readline, b""):
        now = time.monotonic()
        text = raw.decode("utf-8", "replace")
        sys.stdout.write(text)
        sys.stdout.flush()
        lines.put((now, text.rstrip("\n")))
    lines.put((time.monotonic(), None))


def target_pid(target, proc, ranks):
    if target == "spawner":
        return proc.pid
    r = int(target.split(":", 1)[1])
    return ranks[r].pid if r in ranks else None


def send(run, name, target, pid):
    try:
        os.kill(pid, getattr(signal, "SIG" + name))
    except OSError:
        pass
    say("run %d: sent SIG%s to %s (pid %d)" % (run, name, target, pid))


class Rank:
    def __init__(self, pid):
        self.pid = pid
        self.last_alive = None
        self.first_gone = None


def one_run(args, run):
    pid_re = re.compile(args.pid_regex)
    on_line = None
    if args.on and args.on.startswith("line:"):
        on_line = re.compile(args.on[len("line:"):])
    window_re = re.compile(args.line_window[0]) if args.line_window else None
    ranks = {}
    failures = []
    fired_at = None
    signal_at = {}
    window_at = None

    launch = time.monotonic()
    proc = subprocess.Popen(args.command, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    PROCS.append(proc)
    lines = queue.Queue()
    thread = threading.Thread(target=reader, args=(proc.stdout, lines),
                              daemon=True)
    thread.start()

    triggered = False
    eof = False
    ended_at = None
    step = 0
    last_step_at = None
    while True:
        try:
            stamp, text = lines.get(timeout=args.poll_ms / 1000.0)
        except queue.Empty:
            stamp, text = None, None
        if stamp is not None and text is None:
            eof = True
        elif text is not None:
            m = pid_re.search(text)
            if m:
                r, pid = int(m.group(1)), int(m.group(2))
                if r not in ranks:
                    ranks[r] = Rank(pid)
                    ranks[r].last_alive = stamp
            if on_line is not None and on_line.search(text):
                triggered = True
            if window_re is not None and window_at is None \
                    and window_re.search(text):
                window_at = stamp
        if args.on == "pids" and len(ranks) >= args.ranks:
            triggered = True

        now = time.monotonic()
        for rank in ranks.values():
            if rank.first_gone is None:
                if alive(rank.pid):
                    rank.last_alive = now
                else:
                    rank.first_gone = now

        if args.on == "launch":
            triggered = True
        if triggered and fired_at is None and args.on:
            target = target_pid(args.target, proc, ranks)
            if target is not None:
                send(run, args.signal, args.target, target)
                fired_at = time.monotonic()
                signal_at[args.signal] = fired_at
                last_step_at = fired_at
        if fired_at is not None and step < len(args.then):
            name, tgt, when = args.then[step]
            ready = False
            if when == "others-gone":
                me = int(tgt.split(":", 1)[1])
                ready = bool(ranks) and all(
                    k.first_gone is not None
                    for r, k in ranks.items() if r != me)
            else:
                ready = time.monotonic() - last_step_at >= int(when) / 1000.0
            target = target_pid(tgt, proc, ranks) if ready else None
            if target is not None:
                send(run, name, tgt, target)
                last_step_at = time.monotonic()
                signal_at.setdefault(name, last_step_at)
                step += 1

        if ended_at is None and proc.poll() is not None:
            ended_at = time.monotonic()
        if ended_at is not None:
            ranks_done = all(r.first_gone is not None for r in ranks.values())
            if eof and ranks_done:
                break
            if time.monotonic() - ended_at > args.settle_s:
                break

    proc.wait()
    status = status_of(proc.returncode)
    say("run %d: spawner status %d" % (run, status))
    if args.want != ["any"] and str(status) not in args.want:
        failures.append("spawner status %d, not one of %s"
                        % (status, ",".join(args.want)))
    if args.on and fired_at is None:
        failures.append("the trigger %s never fired" % args.on)
    if step < len(args.then):
        failures.append("step %d of --then never fired" % (step + 1))

    if args.dead_after_ms is not None and fired_at is not None:
        deadline = fired_at + args.dead_after_ms / 1000.0
        live = [r for r, k in sorted(ranks.items())
                if k.first_gone is None or k.last_alive > deadline]
        if live:
            failures.append("rank(s) %s seen alive %d ms after the signal"
                            % (live, args.dead_after_ms))
        elif not ranks:
            failures.append("no rank named its pid")

    ends = [k for k in ranks.values() if k.first_gone is not None]
    if args.line_window:
        lo, hi = float(args.line_window[1]), float(args.line_window[2])
        if window_at is None:
            failures.append("no line matched %s" % args.line_window[0])
        elif not ends:
            failures.append("no rank end was seen")
        else:
            first = min(ends, key=lambda k: k.first_gone)
            low_ms = (window_at - first.first_gone) * 1000.0
            high_ms = ((window_at - first.last_alive) * 1000.0
                       if first.last_alive is not None else None)
            say("run %d: the line arrived %.0f-%s ms after the first rank end"
                % (run, low_ms, "?" if high_ms is None else "%.0f" % high_ms))
            if high_ms is None or high_ms < lo or low_ms > hi:
                failures.append("the line arrived outside %g-%g ms of the "
                                "first rank end" % (lo, hi))

    if args.bound_ms is not None:
        start = None
        frm = args.bound_from
        if frm is None:
            frm = "signal:" + args.signal if args.on else "first-end"
        if frm == "launch":
            start = launch
        elif frm == "first-end":
            start = min((k.first_gone for k in ends), default=None)
        elif frm.startswith("signal:"):
            start = signal_at.get(signal_name(frm.split(":", 1)[1]))
        members = dict(ranks)
        if args.bound_ranks.startswith("except:"):
            members.pop(int(args.bound_ranks.split(":", 1)[1]), None)
        last = [k.first_gone for k in members.values()]
        if not ranks and args.bound_ranks == "all":
            last = [ended_at]
        if start is None or not last or None in last:
            say("run %d: bound not measured (start or end unseen)" % run)
            failures.append("the bound could not be measured")
        else:
            took = (max(last) - start) * 1000.0
            say("run %d: bound %.0f ms from %s (limit %d ms)"
                % (run, took, frm, args.bound_ms))
            if took > args.bound_ms:
                failures.append("the run took %.0f ms from %s, over %d ms"
                                % (took, frm, args.bound_ms))

    if args.absent and os.path.exists(args.absent):
        failures.append("%s exists after the run" % args.absent)

    stray = leftovers()
    if stray:
        failures.append("process(es) %s still run the program after the run"
                        % stray)
    reap_everything()
    for reason in failures:
        say("run %d: FAIL %s" % (run, reason))
    return not failures


def main():
    global EXE
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("--want", required=True)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--pid-regex", default=r"RANK_PID ([0-9]+) ([0-9]+)")
    parser.add_argument("--ranks", type=int, default=0)
    parser.add_argument("--on")
    parser.add_argument("--signal", default="KILL")
    parser.add_argument("--target", default="spawner")
    parser.add_argument("--then", action="append", default=[])
    parser.add_argument("--dead-after-ms", type=int)
    parser.add_argument("--line-window", nargs=3)
    parser.add_argument("--absent")
    parser.add_argument("--bound-ms", type=int)
    parser.add_argument("--bound-from")
    parser.add_argument("--bound-ranks", default="all")
    parser.add_argument("--poll-ms", type=int, default=10)
    parser.add_argument("--settle-s", type=float, default=5.0)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command and args.command[0] == "--":
        args.command = args.command[1:]
    if not args.command:
        parser.error("no program after --")
    if args.on and args.on not in ("pids", "launch") \
            and not args.on.startswith("line:"):
        parser.error("--on takes pids, launch or line:<regex>")
    steps = []
    for spec in args.then:
        m = re.fullmatch(r"([A-Z0-9]+):(spawner|rank:[0-9]+):"
                         r"([0-9]+|others-gone)", spec)
        if not m or not args.on:
            parser.error("--then takes <SIGNAL>:<target>:<ms|others-gone> "
                         "after an --on action, not %s" % spec)
        name = signal_name(m.group(1))
        if not hasattr(signal, "SIG" + name):
            parser.error("no signal SIG%s" % name)
        if m.group(3) == "others-gone" and m.group(2) == "spawner":
            parser.error("--then ...:others-gone needs a rank target")
        steps.append((name, m.group(2), m.group(3)))
    args.then = steps
    if args.on == "pids" and args.ranks < 1:
        parser.error("--on pids needs --ranks")
    if args.target != "spawner" and not re.fullmatch(r"rank:[0-9]+",
                                                     args.target):
        parser.error("--target takes spawner or rank:<i>")
    args.signal = signal_name(args.signal)
    if not hasattr(signal, "SIG" + args.signal):
        parser.error("no signal SIG%s" % args.signal)
    args.want = args.want.split(",")
    EXE = os.path.realpath(args.command[0])

    folder = os.environ.get("counter_folder")
    if folder:
        os.makedirs(folder, exist_ok=True)

    atexit.register(reap_everything)
    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    passed = 0
    try:
        for run in range(1, args.repeat + 1):
            if one_run(args, run):
                passed += 1
    finally:
        reap_everything()
    say("%d of %d run(s) passed" % (passed, args.repeat))
    return 0 if passed == args.repeat else 1


if __name__ == "__main__":
    sys.exit(main())
