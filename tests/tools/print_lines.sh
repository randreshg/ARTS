#!/bin/sh
# Prints each argument on a line of its own.  No process runs this file as
# its executable, so a driver that ends every process running its program's
# executable ends none through it.
for line in "$@"; do
  printf '%s\n' "$line"
done
