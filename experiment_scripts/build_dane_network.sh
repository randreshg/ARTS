#!/bin/bash
# Configure and build the 18 Network-series binaries on Dane (see
# DANE_NETWORK_PLAN.md, Step 0).  Run inside an allocation, e.g.
#   srun -N1 -n1 -c 56 -p pdebug --time=60 experiment_scripts/build_dane_network.sh
# Toolchain matches build_release/ (gcc 12.1.1, ninja 1.11.1).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$REPO/build_dane_net}
COUNTERS=${COUNTERS:-$REPO/configs/counters_cxl_ipdps27.cfg}
GCC=/usr/tce/packages/gcc/gcc-12.1.1/bin
NINJA_DIR=/usr/tce/packages/ninja/ninja-1.11.1/bin
export PATH="$NINJA_DIR:$GCC:$PATH"
NINJA=$(command -v ninja || command -v ninja-build)
SUFFIX=arts_ocr_excl_retain_owner
APPS="cholesky_dist CoMD_intel_chandra_tiled_hinted fft_dist fibonacci_hinted
graph500_dist hpcg_intel_dist hpgmg_dist LCS_wavefront mapreduce_wordcount
miniAMR_intel_dist nekbone_dist nqueens_hinted RSBench_intel_sharedDB_hinted
smithwaterman_dist Stencil2D_intel_channelEVTs stream_dist triangle_hinted
XSBench_intel_sharedDB"

cmake -S "$REPO" -B "$BUILD" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$NINJA" \
  -DCMAKE_C_COMPILER="$GCC/gcc" -DCMAKE_CXX_COMPILER="$GCC/g++" \
  -DCMAKE_BUILD_TYPE=Release \
  -DARTS_BUILD_BENCHMARKS=ON -DARTS_BUILD_OCR_APPS=ON \
  -DARTS_COUNTER_CONFIG="$COUNTERS"

targets=()
for a in $APPS; do targets+=("${a}_${SUFFIX}"); done
"$NINJA" -C "$BUILD" -j "${SLURM_CPUS_PER_TASK:-16}" "${targets[@]}"
ls -l "$BUILD/benchmarks/apps/"*_"$SUFFIX"
