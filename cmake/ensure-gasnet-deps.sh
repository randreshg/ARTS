#!/usr/bin/env bash
# Ensure the prerequisites for building GASNet-EX from its release tarball, plus
# the libraries a specific conduit needs, auto-installing them on Debian/Ubuntu
# when possible.
#
#   Usage: ensure-gasnet-deps.sh <conduit>   # conduit: udp|smp|ofi|ibv|ucx|mpi
#
# Invoked by cmake/GasnetBootstrap.cmake before configuring GASNet. Exit 0 when
# every dependency is present (or was installed); exit 1 with guidance otherwise
# so the caller can fall back to the dependency-free udp conduit.
#
# Build prereqs (from the GASNet README): GNU make, perl, and a C/C++ compiler.
# autoconf/automake are NOT needed — the release tarball ships a pre-generated
# configure. Conduit libraries map to apt -dev packages below.
set -u

conduit="${1:-udp}"

# Build tools probed by command; mapped to the apt package that provides them.
declare -A BUILD_CMD_PKG=( [make]=make [perl]=perl [cc]=build-essential [c++]=build-essential )

# Conduit -> pkg-config modules -> apt dev packages (transitive deps included).
case "$conduit" in
  ucx)  MODULES=(ucx librdmacm libibverbs); PACKAGES=(libucx-dev librdmacm-dev libibverbs-dev) ;;
  ofi)  MODULES=(libfabric);                PACKAGES=(libfabric-dev) ;;
  ibv)  MODULES=(libibverbs librdmacm);     PACKAGES=(libibverbs-dev librdmacm-dev) ;;
  udp|smp|mpi|*) MODULES=(); PACKAGES=() ;;
esac

missing_packages() {
  local out=()
  # Build tools.
  local cmd
  for cmd in make perl cc c++; do
    command -v "$cmd" >/dev/null 2>&1 || out+=("${BUILD_CMD_PKG[$cmd]}")
  done
  # Conduit libraries (need pkg-config to probe; if absent, request everything).
  if [ "${#MODULES[@]}" -gt 0 ]; then
    if ! command -v pkg-config >/dev/null 2>&1; then
      out+=("pkg-config" "${PACKAGES[@]}")
    else
      local i
      for i in "${!MODULES[@]}"; do
        pkg-config --exists "${MODULES[$i]}" >/dev/null 2>&1 || out+=("${PACKAGES[$i]}")
      done
    fi
  fi
  # De-duplicate.
  printf '%s\n' "${out[@]}" | awk 'NF && !seen[$0]++'
}

try_apt_install() {
  command -v apt-get >/dev/null 2>&1 || return 1
  local cmd=(apt-get install -y "$@")
  if [ "$(id -u)" -ne 0 ]; then
    command -v sudo >/dev/null 2>&1 || return 1
    cmd=(sudo -n "${cmd[@]}")
  fi
  echo "Installing GASNet ($conduit) dependencies: $*"
  "${cmd[@]}"
}

mapfile -t MISSING < <(missing_packages)
[ "${#MISSING[@]}" -eq 0 ] && exit 0

if try_apt_install "${MISSING[@]}"; then
  mapfile -t MISSING < <(missing_packages)
  [ "${#MISSING[@]}" -eq 0 ] && { echo "GASNet ($conduit) dependencies installed."; exit 0; }
fi

echo "ERROR: GASNet ($conduit) dependencies are missing: ${MISSING[*]}." >&2
echo "Automatic install was unavailable or incomplete (needs apt-get with" >&2
echo "passwordless sudo). Install them manually, set ARTS_GASNET_CONDUIT=udp" >&2
echo "for a dependency-free build, or point ARTS_GASNET_PREFIX at a prebuilt" >&2
echo "GASNet install." >&2
exit 1
