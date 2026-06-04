#!/bin/bash
# Robust launch for ARTS-on-GASNet (ucx-conduit, RoCE RDMA) on a Slurm cluster.
#
#   Usage (inside an sbatch/salloc): gasnet_slurm_launch.sh <pmi2_libdir> <arts_config> <nranks> <binary> [args...]
#
# Uses Slurm PMI2 (srun --mpi=pmi2) for process launch and wireup. This is the
# durable fix for the GASNet ssh-spawner intermittency: PMI2 does the rank
# exchange through Slurm, so there is NO ssh fan-out and NO TCP connect-back to
# the master — the race that made the ssh-spawner flaky (worker "died before
# setup completed") cannot occur. Validated reliably (incl. nodes where the
# ssh-spawner failed 100% of the time); inter-node transport is rc_verbs over
# RoCE (real RDMA).
#
# Prerequisites (one-time):
#   - GASNet built with: --enable-ucx --enable-pmi --with-pmi-version=2
#       --with-pmi-home=<dir with include/pmi2.h + lib/libpmi2.so>
#     (libpmi2 from `apt install libpmi2-0t64`; pmi2.h from the Slurm source;
#      the 23.11 client interoperates with the cluster's 25.11 pmi2 server.)
#   - ARTS built non-PIE against that GASNet (ARTS_GASNET_CONDUIT=ucx).
#   - libpmi2.so.0 and the ARTS config reachable on EVERY node — put them on
#     shared storage (/home), since srun tasks run on remote nodes that do not
#     share /tmp.
set -u
PMI2_LIBDIR="$1"; ARTS_CFG="$2"; NRANKS="$3"; BIN="$4"; shift 4
export LD_LIBRARY_PATH="$PMI2_LIBDIR:${LD_LIBRARY_PATH:-}"
EXP="ALL"
EXP="$EXP,ARTS_CONFIG=$ARTS_CFG"
EXP="$EXP,LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
EXP="$EXP,GASNET_UCX_SPAWNER=pmi"      # use the PMI spawner, not ssh
EXP="$EXP,GASNET_TMPDIR=/tmp"          # PSHM scratch (node-local is fine)
EXP="$EXP,GASNET_HOST_DETECT=hostname" # distinct nodes -> distinct supernodes
EXP="$EXP,GASNET_SUPERNODE_MAXSIZE=1"
EXP="$EXP,UCX_TLS=rc,ud,sm,self"       # RoCE RDMA transports
exec srun --mpi=pmi2 -N"$NRANKS" -n"$NRANKS" --ntasks-per-node=1 \
  --export="$EXP" "$BIN" "$@"
