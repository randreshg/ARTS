#!/bin/bash
# Runs a program that must die by a signal and judges how it died.
#   expect_fault.sh <status> <core: yes|no> <program> [args...]
# The program's status must be <status> (128 + the signal's number, as a
# shell reports a death by a signal).  With <core> yes, a core pattern that
# names a file relative to the working directory (no '|', no '/') must have
# produced core* in the working directory, which is then removed; any other
# pattern hands the core elsewhere and is not checked.  The working directory
# is the caller's own, so nothing else's core is removed.  Everything it
# prints, the shell's report of a command a signal ended included, goes to
# its stdout with the program's output, in the order it happens.
exec 2>&1
want=$1
core=$2
shift 2
rm -f core core.*
"$@" 2>&1
rc=$?
if [ "$rc" -ne "$want" ]; then
  echo "expect_fault: status $rc, want $want"
  exit 1
fi
if [ "$core" = yes ]; then
  pattern=$(cat /proc/sys/kernel/core_pattern)
  case $pattern in
  '|'* | */*)
    echo "expect_fault: core_pattern '$pattern' sends the core elsewhere;" \
      "not checked"
    ;;
  *)
    found=""
    for f in core core.*; do
      [ -f "$f" ] && found="$f"
    done
    if [ -z "$found" ]; then
      echo "expect_fault: no core in $(pwd) for core_pattern '$pattern'"
      exit 1
    fi
    echo "expect_fault: core $found ($(stat -c %s "$found") bytes," \
      "$(du -k "$found" | cut -f1) KiB on disk)"
    rm -f core core.*
    ;;
  esac
fi
echo "expect_fault: status $rc"
