#!/bin/bash
# Run the ARTS ctest suites across ALL ELEVEN build configurations
# (<memory model>_<family>_<live second axis>), each in its own build_<config>
# tree, then run the application matrix once over the single benchmark build
# (which holds every coherence configuration at once):
#
#   config                      cmake flags
#   --------------------------  ---------------------------------------------------------
#   ocr_val_wt                  -DARTS_MEMORY_MODEL=OCR    -DARTS_COHERENCE_PROTOCOL=VAL  -DARTS_WRITE_POLICY=WT
#   ocr_val_wb                  -DARTS_MEMORY_MODEL=OCR    -DARTS_COHERENCE_PROTOCOL=VAL  -DARTS_WRITE_POLICY=WB
#   ocr_val_wt_purge            -DARTS_MEMORY_MODEL=OCR    -DARTS_COHERENCE_PROTOCOL=VAL  -DARTS_WRITE_POLICY=WT -DARTS_RELEASE_POLICY=PURGE
#   ocr_excl_purge              -DARTS_MEMORY_MODEL=OCR    -DARTS_COHERENCE_PROTOCOL=EXCL -DARTS_RELEASE_POLICY=PURGE
#   ocr_excl_retain             -DARTS_MEMORY_MODEL=OCR    -DARTS_COHERENCE_PROTOCOL=EXCL -DARTS_RELEASE_POLICY=RETAIN
#   ocr_inv_wt                  -DARTS_MEMORY_MODEL=OCR    -DARTS_COHERENCE_PROTOCOL=INV  -DARTS_WRITE_POLICY=WT
#   ocr_inv_wb                  -DARTS_MEMORY_MODEL=OCR    -DARTS_COHERENCE_PROTOCOL=INV  -DARTS_WRITE_POLICY=WB
#   ocr_inv_wt_purge            -DARTS_MEMORY_MODEL=OCR    -DARTS_COHERENCE_PROTOCOL=INV  -DARTS_WRITE_POLICY=WT -DARTS_RELEASE_POLICY=PURGE
#   wrf_flush                   -DARTS_COHERENCE_PROTOCOL=FLUSH
#   ocr_excl_purge_cxl_staged   -DARTS_MEMORY_MODEL=OCR    -DARTS_COHERENCE_PROTOCOL=EXCL -DARTS_RELEASE_POLICY=PURGE -DARTS_USE_CXL=ON -DARTS_CXL_RESIDENCY=STAGED -DARTS_CXL_DB_ARENA_SIZE_BYTES=268435456
#   ocr_excl_purge_cxl_direct   -DARTS_MEMORY_MODEL=OCR    -DARTS_COHERENCE_PROTOCOL=EXCL -DARTS_RELEASE_POLICY=PURGE -DARTS_USE_CXL=ON -DARTS_CXL_RESIDENCY=DIRECT -DARTS_CXL_DB_ARENA_SIZE_BYTES=268435456
#
# The last-but-two row names no memory model: FLUSH derives DB_WRF, and naming
# the model as well only asserts what the protocol already implies.  It also
# has no second axis, so its cache check compares model and protocol alone.
# The two `_cxl_*` rows name a canonical-store axis on the OCR×EXCL×WB×PURGE
# cell, not a tenth and eleventh protocol combination, and are launcher: local
# only.
#
# Usage:
#   bash tests/run_all_models.sh                                  # ctest + applications
#   bash tests/run_all_models.sh --models ocr_val_wt,ocr_val_wb  # subset
#   bash tests/run_all_models.sh --no-harness                     # ctest only
#   bash tests/run_all_models.sh --no-ctest                       # applications only
#   bash tests/run_all_models.sh --no-build                       # skip reconfigure/rebuild
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO" || exit 1

MODELS="ocr_val_wt ocr_val_wb ocr_val_wt_purge ocr_excl_purge ocr_excl_retain ocr_inv_wt ocr_inv_wb ocr_inv_wt_purge wrf_flush ocr_excl_purge_cxl_staged ocr_excl_purge_cxl_direct"
DO_CTEST=1
DO_HARNESS=1
DO_BUILD=1
while [ $# -gt 0 ]; do
  case "$1" in
    --models)     MODELS="$(echo "$2" | tr ',' ' ')"; shift 2 ;;
    --no-harness) DO_HARNESS=0; shift ;;
    --no-ctest)   DO_CTEST=0; shift ;;
    --no-build)   DO_BUILD=0; shift ;;
    *) echo "unknown arg: $1"; exit 2 ;;
  esac
done

# protocol → ctest build dir / cmake flags
ctest_dir()   { echo "build_$1"; }
model_label() { echo "$1" | tr '[:lower:]' '[:upper:]'; }
model_cmake_flags() {
  case "$1" in
    ocr_val_wt)       echo "-DARTS_MEMORY_MODEL=OCR -DARTS_COHERENCE_PROTOCOL=VAL -DARTS_WRITE_POLICY=WT" ;;
    ocr_val_wb)       echo "-DARTS_MEMORY_MODEL=OCR -DARTS_COHERENCE_PROTOCOL=VAL -DARTS_WRITE_POLICY=WB" ;;
    ocr_val_wt_purge) echo "-DARTS_MEMORY_MODEL=OCR -DARTS_COHERENCE_PROTOCOL=VAL -DARTS_WRITE_POLICY=WT -DARTS_RELEASE_POLICY=PURGE" ;;
    ocr_excl_purge)   echo "-DARTS_MEMORY_MODEL=OCR -DARTS_COHERENCE_PROTOCOL=EXCL -DARTS_RELEASE_POLICY=PURGE" ;;
    ocr_excl_retain)  echo "-DARTS_MEMORY_MODEL=OCR -DARTS_COHERENCE_PROTOCOL=EXCL -DARTS_RELEASE_POLICY=RETAIN" ;;
    ocr_inv_wt)       echo "-DARTS_MEMORY_MODEL=OCR -DARTS_COHERENCE_PROTOCOL=INV -DARTS_WRITE_POLICY=WT" ;;
    ocr_inv_wb)       echo "-DARTS_MEMORY_MODEL=OCR -DARTS_COHERENCE_PROTOCOL=INV -DARTS_WRITE_POLICY=WB" ;;
    ocr_inv_wt_purge) echo "-DARTS_MEMORY_MODEL=OCR -DARTS_COHERENCE_PROTOCOL=INV -DARTS_WRITE_POLICY=WT -DARTS_RELEASE_POLICY=PURGE" ;;
    wrf_flush)        echo "-DARTS_COHERENCE_PROTOCOL=FLUSH" ;;
    ocr_excl_purge_cxl_staged) echo "-DARTS_MEMORY_MODEL=OCR -DARTS_COHERENCE_PROTOCOL=EXCL -DARTS_RELEASE_POLICY=PURGE -DARTS_USE_CXL=ON -DARTS_CXL_RESIDENCY=STAGED -DARTS_CXL_DB_ARENA_SIZE_BYTES=268435456" ;;
    ocr_excl_purge_cxl_direct) echo "-DARTS_MEMORY_MODEL=OCR -DARTS_COHERENCE_PROTOCOL=EXCL -DARTS_RELEASE_POLICY=PURGE -DARTS_USE_CXL=ON -DARTS_CXL_RESIDENCY=DIRECT -DARTS_CXL_DB_ARENA_SIZE_BYTES=268435456" ;;
  esac
}
# expected CMakeCache values per config.  model_model needs no arm for the
# two ocr_excl_purge_cxl_* configs: every non-FLUSH config already answers
# OCR through the trailing wildcard, cxl included.  model_proto keeps an
# explicit cxl arm alongside its ocr_excl_* wildcard for the same EXCL
# answer, paired with model_timing's arm below (which IS load-bearing: with
# no trailing wildcard of its own, model_timing would otherwise answer the
# empty string for a cxl config and silently skip its RELEASE_POLICY check).
model_model()  { case "$1" in wrf_flush) echo DB_WRF;; *) echo OCR;; esac; }
model_proto()  { case "$1" in wrf_flush) echo FLUSH;; ocr_excl_*) echo EXCL;; ocr_inv_*) echo INV;; *) echo VAL;; esac; }
# The live second axis, read off the suffix.  ocr_{val,inv}_wt_purge pin BOTH
# ARTS_WRITE_POLICY and ARTS_RELEASE_POLICY away from their defaults, so their
# "timing" is the pair, not a single value.  A config with no live second axis
# answers the empty string, which the cache check below skips.  The two
# ocr_excl_purge_cxl_* configs carry ARTS_RELEASE_POLICY=PURGE like their
# non-CXL namesake; ARTS_USE_CXL/ARTS_CXL_RESIDENCY are the separate
# canonical-store axis, tracked by model_cxl() below and checked on its own
# in ensure_build.
model_timing() { case "$1" in wrf_flush) echo "" ;; ocr_excl_purge_cxl_*) echo PURGE ;; *_wt_purge) echo "WT+PURGE" ;; *_wt) echo WT;; *_wb) echo WB;; ocr_excl_purge) echo PURGE;; ocr_excl_retain) echo RETAIN;; esac; }
# The canonical-store axis: ARTS_USE_CXL and ARTS_CXL_RESIDENCY, expected
# as one space-separated pair.  Every non-cxl config is OFF with no
# residency; the two cxl configs are ON with their own residency.
model_cxl() {
  case "$1" in
    ocr_excl_purge_cxl_staged) echo "ON STAGED" ;;
    ocr_excl_purge_cxl_direct) echo "ON DIRECT" ;;
    *)                         echo "OFF" ;;
  esac
}

# Configure a build dir to the requested configuration if its cache does not
# match, then build.  Reconfigure forces a full rebuild (compile-flag change).
# The cache check compares ARTS_MEMORY_MODEL, ARTS_COHERENCE_PROTOCOL, the
# live second axis (ARTS_WRITE_POLICY / ARTS_RELEASE_POLICY, or both for the
# ocr_{val,inv}_wt_purge configs, which pin both away from default; a config
# with no live second axis compares model and protocol alone), and the
# canonical-store axis (ARTS_USE_CXL / ARTS_CXL_RESIDENCY — OFF/empty on
# every non-cxl config).
ensure_build() {
  local dir="$1" model="$2" wantgpu="$3" extra="${4:-}"
  local want_model; want_model="$(model_model "$model")"
  local want_proto; want_proto="$(model_proto "$model")"
  local want_timing; want_timing="$(model_timing "$model")"
  local want_use_cxl want_cxl_residency
  read -r want_use_cxl want_cxl_residency <<< "$(model_cxl "$model")"
  local have_model; have_model="$(grep -E '^ARTS_MEMORY_MODEL:STRING=' "$dir/CMakeCache.txt" 2>/dev/null | cut -d= -f2)"
  local have_proto; have_proto="$(grep -E '^ARTS_COHERENCE_PROTOCOL:STRING=' "$dir/CMakeCache.txt" 2>/dev/null | cut -d= -f2)"
  local have_use_cxl; have_use_cxl="$(grep -E '^ARTS_USE_CXL:BOOL=' "$dir/CMakeCache.txt" 2>/dev/null | cut -d= -f2)"
  local have_cxl_residency; have_cxl_residency="$(grep -E '^ARTS_CXL_RESIDENCY:STRING=' "$dir/CMakeCache.txt" 2>/dev/null | cut -d= -f2)"
  # The model is DERIVED from the protocol, so a tree configured without the
  # option carries an empty cache entry and still implements one; read it off
  # the protocol rather than calling the tree misconfigured.
  if [ -z "$have_model" ]; then
    case "$have_proto" in
      FLUSH) have_model=DB_WRF ;;
      ?*)    have_model=OCR ;;
    esac
  fi
  local have_timing
  case "$model" in
    wrf_flush)  have_timing="" ;; # no second axis: nothing to read or compare
    ocr_excl_*) have_timing="$(grep -E '^ARTS_RELEASE_POLICY:STRING=' "$dir/CMakeCache.txt" 2>/dev/null | cut -d= -f2)" ;;
    *_wt_purge)
      local have_write have_release
      have_write="$(grep -E '^ARTS_WRITE_POLICY:STRING=' "$dir/CMakeCache.txt" 2>/dev/null | cut -d= -f2)"
      have_release="$(grep -E '^ARTS_RELEASE_POLICY:STRING=' "$dir/CMakeCache.txt" 2>/dev/null | cut -d= -f2)"
      have_timing="${have_write}+${have_release}"
      ;;
    *)          have_timing="$(grep -E '^ARTS_WRITE_POLICY:STRING=' "$dir/CMakeCache.txt" 2>/dev/null | cut -d= -f2)" ;;
  esac
  # A tree that is going to be tested must carry the test programs; a tree
  # configured without them reports nothing to run, not a failure, so the
  # option is part of what the cache must match.
  local want_tests=OFF
  case " $extra " in *"-DARTS_BUILD_TESTS=ON"*) want_tests=ON ;; esac
  local have_tests; have_tests="$(grep -E '^ARTS_BUILD_TESTS:BOOL=' "$dir/CMakeCache.txt" 2>/dev/null | cut -d= -f2)"
  local mismatch=0
  [ "$have_model" != "$want_model" ] && mismatch=1
  [ "$have_proto" != "$want_proto" ] && mismatch=1
  [ -n "$want_timing" ] && [ "$have_timing" != "$want_timing" ] && mismatch=1
  [ "$have_use_cxl" != "$want_use_cxl" ] && mismatch=1
  [ "$have_cxl_residency" != "${want_cxl_residency:-}" ] && mismatch=1
  [ "$want_tests" = ON ] && [ "$have_tests" != ON ] && mismatch=1
  if [ ! -d "$dir" ] || [ "$mismatch" = 1 ]; then
    echo "  [cfg] $dir → $(model_cmake_flags "$model") (was MODEL='${have_model:-none}' PROTO='${have_proto:-none}' TIMING='${have_timing:-none}' CXL='${have_use_cxl:-none}/${have_cxl_residency:-none}')"
    # shellcheck disable=SC2086
    cmake -GNinja -B "$dir" -DCMAKE_BUILD_TYPE=Release \
          $(model_cmake_flags "$model") -DARTS_USE_GPU="$wantgpu" $extra >/dev/null 2>&1 \
      || { echo "  [cfg] FAILED for $dir"; return 1; }
  fi
  ninja -C "$dir" >/dev/null 2>&1 || { echo "  [build] FAILED for $dir"; return 1; }
  return 0
}

# drain TCP TIME_WAIT/LISTEN leftovers on the ctest port range (bases 20000+,
# below the kernel ephemeral range) before a multinode ctest run
drain_ports() { local n=0; until ! ss -tan 2>/dev/null | grep -qE 'LISTEN.*:2[0-9]{4}\b'; do sleep 0.2; n=$((n+1)); [ $n -gt 100 ] && break; done; }

LOGDIR="$REPO/tests/logs"
mkdir -p "$LOGDIR"

declare -A RESULT
for m in $MODELS; do
  echo "================= PROTOCOL: $(model_label "$m") ================="

  if [ "$DO_CTEST" = 1 ]; then
    cd="$(ctest_dir "$m")"
    [ "$DO_BUILD" = 1 ] && ensure_build "$cd" "$m" OFF "-DARTS_BUILD_TESTS=ON"
    # No config copying: CTest sets each test's ARTS_CONFIG env straight at
    # the source cfg (tests/CMakeLists.txt).
    s=$( cd "$cd" && ctest -L single_node 2>&1 | grep -oE '[0-9]+% tests passed[^.]*' | head -1 )
    drain_ports
    mn=$( cd "$cd" && ctest -L multinode 2>&1 | grep -oE '[0-9]+% tests passed[^.]*' | head -1 )
    RESULT["$m,ctest_single"]="${s:-NORUN}"
    RESULT["$m,ctest_multi"]="${mn:-NORUN}"
    echo "  ctest single : ${s:-NORUN}"
    echo "  ctest multi  : ${mn:-NORUN}"
  fi

done

# The application matrix is no longer per-configuration: one benchmark build
# holds every coherence configuration, so it runs once for all of them rather
# than once inside the loop above.
if [ "$DO_HARNESS" = 1 ]; then
  hd="build_release"
  [ "$DO_BUILD" = 1 ] && ensure_build "$hd" ocr_val_wb OFF "-DARTS_BUILD_BENCHMARKS=ON"
  drain_ports
  echo "================= APPLICATION MATRIX ================="
  timeout -k 30 2400 artsrun run -p ferrari -b paper-main --nodes 1 \
      --build-dir "$hd" >"$LOGDIR/apps.log" 2>&1
  tally=$( grep -E 'MINORITY REPORT|No disagreement' "$LOGDIR/apps.log" | head -1 )
  APPS_RESULT="${tally:-NORUN}"
  echo "  applications : ${APPS_RESULT}  (full log: $LOGDIR/apps.log)"
fi

echo
echo "===================== SUMMARY (OCR-model configs must be clean; so is the DB_WRF tree, whose test set is restricted to DB-WRF-valid programs) ====================="
for m in $MODELS; do
  M="$(model_label "$m")"
  echo "[$M]"
  [ "$DO_CTEST" = 1 ]   && echo "   ctest single : ${RESULT[$m,ctest_single]:-skip}"
  [ "$DO_CTEST" = 1 ]   && echo "   ctest multi  : ${RESULT[$m,ctest_multi]:-skip}"
done
