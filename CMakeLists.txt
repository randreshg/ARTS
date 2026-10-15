###############################################################################
## This material was prepared as an account of work sponsored by an agency
## of the United States Government.  Neither the United States Government
## nor the United States Department of Energy, nor Battelle, nor any of
## their employees, nor any jurisdiction or organization that has cooperated
## in the development of these materials, makes any warranty, express or
## implied, or assumes any legal liability or responsibility for the accuracy,
## completeness, or usefulness or any information, apparatus, product,
## software, or process disclosed, or represents that its use would not
## infringe privately owned rights.
##
## Reference herein to any specific commercial product, process, or service
## by trade name, trademark, manufacturer, or otherwise does not necessarily
## constitute or imply its endorsement, recommendation, or favoring by the
## United States Government or any agency thereof, or Battelle Memorial
## Institute. The views and opinions of authors expressed herein do not
## necessarily state or reflect those of the United States Government or
## any agency thereof.
##
##                      PACIFIC NORTHWEST NATIONAL LABORATORY
##                                  operated by
##                                    BATTELLE
##                                    for the
##                      UNITED STATES DEPARTMENT OF ENERGY
##                         under Contract DE-AC05-76RL01830
##
## Copyright 2019 Battelle Memorial Institute
## Licensed under the Apache License, Version 2.0 (the "License");
## you may not use this file except in compliance with the License.
## You may obtain a copy of the License at
##
##    https://www.apache.org/licenses/LICENSE-2.0
##
## Unless required by applicable law or agreed to in writing, software
## distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
## WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
## License for the specific language governing permissions and limitations
###############################################################################

cmake_minimum_required(VERSION 3.22)
project(ARTS
    VERSION 2.0.0
    DESCRIPTION "ARTS - Abstract Runtime System"
    LANGUAGES C CXX
)

# Enforce Ninja generator
if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR
        "ARTS requires the Ninja build generator.\n"
        "  Use: cmake -GNinja <source-dir>")
endif()

# C17 strict mode — no GNU extensions
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Feature test macro: strict -std=c17 hides POSIX/BSD/XSI functions.
# _DEFAULT_SOURCE enables glibc defaults (POSIX.1-2008 + XSI + BSD + extensions)
# while CMAKE_C_EXTENSIONS=OFF keeps the C17 *language* strict.
add_compile_definitions(_DEFAULT_SOURCE _GNU_SOURCE)

# Target processor, and what its 16-byte atomics need.  The supported targets
# are AMD64 and ARM64.
#  - x86-64: -mcx16 enables cmpxchg16b.  Clang inlines `lock cmpxchg16b` for a
#    16-byte load, store or CAS; GCC calls libatomic (__atomic_*_16), which is
#    lock-free on every cx16 host.  That is why libatomic is linked below, and
#    linked dynamically.
#  - AArch64: no -march is forced, so the build runs on every ARMv8-A host.
#    A toolchain that outlines by default (-moutline-atomics) turns the RMWs
#    into helpers that pick LSE or LL/SC from the run host's hwcaps; an older
#    one emits inline LL/SC loops.  Without LSE a 16-byte atomic is still
#    lock-free, not a lock: under GCC every 16-byte access is a call into the
#    run host's libatomic, and under Clang loads and stores are inline
#    (ldaxp/stxp) and the CAS is libgcc's __aarch64_cas16_acq_rel when it
#    outlines.
# artsrun's AMD64_PROCESSOR applies the same expression to a tree's processor
# to tell whether the tree has benchmark variants; the two must stay the same.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    set(ARTS_TARGET_AMD64 ON)
    add_compile_options($<$<COMPILE_LANGUAGE:C>:-mcx16>)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-mcx16>)
else()
    set(ARTS_TARGET_AMD64 OFF)
endif()

# The 16-byte CAS of lockfree_pool.h and the shared-pointer slot must compile
# and link: cmpxchg16b (-mcx16) on AMD64; libatomic, or inline LL/SC or LSE,
# on ARM64.  The check proves that much, not that the CAS is lock-free.  It
# links libatomic because GCC calls it for every 16-byte atomic.
include(CheckCSourceCompiles)
set(_arts_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
set(_arts_saved_required_libs  "${CMAKE_REQUIRED_LIBRARIES}")
if(ARTS_TARGET_AMD64)
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -mcx16")
endif()
# ARTS_ATOMIC_LINK is the single link item every consumer must use; the
# module resolves it (a found library, the runtime SONAME's file, or the bare
# name once the linker has accepted it) so a host that carries libatomic only
# as a runtime object still links.
include(cmake/ArtsAtomicLink.cmake)
arts_resolve_atomic_link(C)
if(ARTS_ATOMIC_LINK)
    set(CMAKE_REQUIRED_LIBRARIES "${ARTS_ATOMIC_LINK}" ${CMAKE_REQUIRED_LIBRARIES})
endif()
check_c_source_compiles("
  #include <stdint.h>
  int main(void) {
    __int128 a __attribute__((aligned(16))) = 0;
    __int128 b __attribute__((aligned(16))) = 0;
    return __atomic_compare_exchange_n(&a, &b, (__int128)1,
                                       0, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
  }
" ARTS_DWCAS_LINKS)
set(CMAKE_REQUIRED_FLAGS "${_arts_saved_required_flags}")
set(CMAKE_REQUIRED_LIBRARIES "${_arts_saved_required_libs}")
unset(_arts_saved_required_flags)
unset(_arts_saved_required_libs)
if(NOT ARTS_DWCAS_LINKS)
    message(FATAL_ERROR
        "ARTS requires a 128-bit compare-and-swap that compiles and links: "
        "cmpxchg16b with -mcx16 on AMD64; libatomic "
        "(__atomic_compare_exchange_16), or the compiler's inline LL/SC or LSE "
        "sequence, on ARM64.")
endif()
# Make libatomic available to all targets that may emit 16-byte atomic
# libcalls (lockfree_pool.h users).  The keyword-less form is transitive, so
# it is also in the link interface of every library target, exported or not.
if(ARTS_ATOMIC_LINK)
    link_libraries(${ARTS_ATOMIC_LINK})
endif()

# Set global properties for all targets
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# A program that runs on ARTS is built non-PIE: every rank execs the same
# binary and an EDT's function pointer crosses ranks as an address, which
# names the same function on every rank only when each loads the program at
# the same address.  The tree's own programs take these options, and the
# installed library targets hand them to every consumer.
set(ARTS_NON_PIE_COMPILE_OPTIONS -fno-pie -fno-PIE)
set(ARTS_NON_PIE_LINK_OPTIONS -no-pie -fno-pie -fno-PIE)

# Set default install prefix if not specified
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH "Install prefix" FORCE)
    message(STATUS "Install prefix not specified, defaulting to ${CMAKE_INSTALL_PREFIX}")
else()
    message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")
endif()

# Set default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")
    message(STATUS "Build type not specified, defaulting to Debug")
endif()
# Two build types: what a Debug build carries and a Release build does not is
# decided below by the type, so no third one can fall between them.
if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release)$")
    message(FATAL_ERROR
        "CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}': ARTS builds Debug or Release")
endif()

# Include utilities for modern CMake
include(GNUInstallDirs)
include(CheckLanguage)

# ═══════════════════════════════════════════════════════════════════════════
# Build-time options  (override on the cmake line with -D<NAME>=<VALUE>)
# ═══════════════════════════════════════════════════════════════════════════
include(CMakeDependentOption)

# ── Library targets ────────────────────────────────────────────────────────
# The static library is unconditional: it is the substrate every other artifact
# is derived from (the benchmark variants, the OCR shim, the installed archive),
# so only the shared object is a choice.
option(ARTS_BUILD_SHARED "Build the shared library (libarts.so)" ON)

# ── Optional components ────────────────────────────────────────────────────
# OFF by default: tests link the static archive (hermetic — see
# tests/CMakeLists.txt), so a full test tree costs real disk and link time
# that a benchmark/experiment build never uses.  Validation builds opt in.
option(ARTS_BUILD_TESTS      "Build test programs (enables ctest)"             OFF)
option(ARTS_BUILD_BENCHMARKS "Build benchmark apps (ARTS + reference runtimes)" ON)
# The OCR shim is one translation unit against the tree's own runtime and the
# vendored OCR headers.  The benchmark apps link it, and a validation tree
# builds it on its own so the shim's tests run against every configuration.
option(ARTS_BUILD_SHIM       "Build the OCR-to-ARTS compatibility shim"        ON)
# The reference runtimes are individually optional under ARTS_BUILD_BENCHMARKS.
option(ARTS_BUILD_XSOCR      "Build the XSOCR reference runtime" ON)
option(ARTS_BUILD_OCRVX      "Build the ocr-vx reference runtime" ON)
option(ARTS_BUILD_HPX        "Build HPX and the HPX benchmark apps" ON)
# The benchmark tree is AMD64-only: the vendored FFTW is built for x86-64-v3
# with x86 SIMD codelets, the pinned fftw-reloc artifact is x86 assembly, and
# HPX is built with -mcx16.  On another target it is skipped for this
# configure, the way the reference runtimes are skipped when no MPI is found,
# and the runtime, the shim and the tests configure and build as usual.
if(NOT ARTS_TARGET_AMD64)
    # HPX is reachable only through the benchmark tree.
    if(ARTS_BUILD_BENCHMARKS)
        message(STATUS
            "Benchmark tree skipped: it is AMD64-only (FFTW's x86-64-v3 and x86 "
            "SIMD build, the x86 fftw-reloc artifact, HPX's -mcx16), and the "
            "target processor is ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    set(ARTS_BUILD_BENCHMARKS OFF)
    set(ARTS_BUILD_HPX OFF)
endif()
if(ARTS_BUILD_BENCHMARKS AND NOT ARTS_BUILD_SHIM)
    message(FATAL_ERROR
        "ARTS_BUILD_BENCHMARKS=ON requires ARTS_BUILD_SHIM=ON: every ARTS "
        "variant of a benchmark app links the OCR shim.")
endif()
option(ARTS_BUILD_DOCS       "Build documentation (requires Doxygen + Sphinx)" OFF)

# ── GPU / CUDA ─────────────────────────────────────────────────────────────
# ARTS_USE_LOCAL_CUDA_ARCHITECTURES only applies when ARTS_USE_GPU=ON;
# cmake_dependent_option forces it OFF (and hides it) otherwise.  The related
# stock CMake variable CMAKE_CUDA_ARCHITECTURES picks the SM targets nvcc emits
# (e.g. "80;86"): when the option below is ON it is auto-filled from nvidia-smi,
# otherwise nvcc's default applies (or your own -DCMAKE_CUDA_ARCHITECTURES=...).
option(ARTS_USE_GPU "Enable CUDA GPU support" OFF)
cmake_dependent_option(ARTS_USE_LOCAL_CUDA_ARCHITECTURES
    "Auto-detect CUDA architectures from local GPUs using nvidia-smi" ON
    "ARTS_USE_GPU" OFF)

# ── Coherence protocol × write policy × release policy ─────────────────────
# Compile-time switches — one binary is exactly one configuration; all ranks in
# a multinode run must use the same build.  Every coherence protocol requires
# a memory model of the program: VAL/INV/EXCL require OCR (the OCR v1.2.0 §1.6
# model: races legal, the runtime orders every conflict it must); FLUSH
# requires DB-WRF (exclusive write acquisition: at most one write-mode
# acquisition of a DataBlock is live at any time, and the program guarantees
# it by happens-before; read acquisitions are free).
#   ARTS_COHERENCE_PROTOCOL  Coherence family — who keeps reader copies valid:
#                            INV (default) — invalidation: reader copies stay
#                            valid until a writer's release retires every
#                            standing copy in an acknowledged invalidation
#                            round.
#                            VAL — validation: each read acquire checks its
#                            cached copy's version at the serving side and
#                            refetches only when stale; readers are never
#                            blocked, tracked, or invalidated.
#                            EXCL — exclusion: queue-fair distributed
#                            reader-writer lock per DB; readers and writers
#                            take turns, so no stale copy ever exists.
#                            FLUSH — fetch at acquire, flush at release: every
#                            remote acquire fetches the current payload and
#                            every remote RW release writes it through; requires
#                            the DB-WRF model (the program keeps every DB's
#                            write acquisitions exclusive), which is what makes
#                            the fetch-then-flush pair correct without any
#                            tracking of readers or writers at the home.
#   ARTS_WRITE_POLICY        WT | WB (default) — write policy of a node's
#                            data-block cache at release granularity: WT =
#                            write-through (payload flushed to the block's home
#                            at every release; home serves reads), WB =
#                            write-back (payload stays with the last writer;
#                            directory forwards on demand). Live in INV/VAL.
#   ARTS_RELEASE_POLICY      PURGE | RETAIN (default) — WHO initiates the
#                            grant's return: PURGE = the holder, unasked, at
#                            its own idle edge (the home sends the holder
#                            nothing); RETAIN = the home, on demand, and an
#                            unasked holder keeps the grant. Live in EXCL and
#                            in WT × {VAL, INV}.
# The OCR-model protocols' axes are nominally independent: 12 combinations, 8
# built, plus the ninth build FLUSH (no write/release policy axis).
# Everything else is a configure error naming the reason:
#   VAL  × {WT, WB} × RETAIN
#   VAL  × WT × PURGE
#   INV  × {WT, WB} × RETAIN
#   INV  × WT × PURGE
#   EXCL × WB × {PURGE, RETAIN}
#   FLUSH (no write/release policy axis)
# Not built: EXCL×WT (the payload rides the permission, so the release policy
# already decides both), INV/VAL×WB×PURGE (a voluntary return hands back only
# the permission while the bytes stay at the ex-owner, so the home still
# cannot serve alone — and shipping them home too is write-through).
# The build's default cell; the test suite's reference tree is the tree of
# this cell.
set(_ARTS_DEFAULT_COHERENCE_PROTOCOL "INV")
set(_ARTS_DEFAULT_WRITE_POLICY "WB")
set(_ARTS_DEFAULT_RELEASE_POLICY "RETAIN")
set(ARTS_COHERENCE_PROTOCOL "${_ARTS_DEFAULT_COHERENCE_PROTOCOL}" CACHE STRING
    "Coherence protocol: INV (default; release-time invalidation rounds), VAL (acquire-time version validation), EXCL (per-DB distributed reader-writer lock) — the three OCR-model protocols — or FLUSH (fetch at acquire, flush at release; requires the DB-WRF model: the program keeps every DB's write acquisitions exclusive)")
set_property(CACHE ARTS_COHERENCE_PROTOCOL PROPERTY STRINGS INV VAL EXCL FLUSH)
set(ARTS_WRITE_POLICY "${_ARTS_DEFAULT_WRITE_POLICY}" CACHE STRING
    "Write policy at release granularity: WT (flush to the home at every release) or WB (resident with the last writer, default). Live in INV/VAL; EXCL requires WB")
set_property(CACHE ARTS_WRITE_POLICY PROPERTY STRINGS WT WB)
set(ARTS_RELEASE_POLICY "${_ARTS_DEFAULT_RELEASE_POLICY}" CACHE STRING
    "Who initiates the grant's return: PURGE (the holder, unasked, at its own idle edge) or RETAIN (the home, on demand; default). Live in EXCL and in WT x {VAL, INV}; INV/VAL x WB require RETAIN")
set_property(CACHE ARTS_RELEASE_POLICY PROPERTY STRINGS PURGE RETAIN)
set(ARTS_MALLOC "mimalloc" CACHE STRING
    "General allocator: mimalloc (default, vendored static) or system (libc malloc)")
set_property(CACHE ARTS_MALLOC PROPERTY STRINGS mimalloc system)

# ── CXL: CXL memory as the payload store of EXCL x WB x PURGE ──
# A data block keeps its GUID, its home and its kind; a CXL build changes only
# where its bytes rest.  The store is a per-target choice like the protocol,
# so ONE benchmark tree holds the two CXL variants beside the nine others,
# and this option says whether the TREE'S OWN library (what tests link) is one.
option(ARTS_USE_CXL
    "This tree's own library keeps every data block's payload in CXL memory (EXCL x WB x PURGE only; needs ARTS_CXL_RESIDENCY)"
    OFF)
set(ARTS_CXL_RESIDENCY "" CACHE STRING
    "Where an EDT's working bytes live under ARTS_USE_CXL=ON: STAGED (a rank-local copy at the two ownership edges) or DIRECT (the CXL slot itself)")
set_property(CACHE ARTS_CXL_RESIDENCY PROPERTY STRINGS "" STAGED DIRECT)
option(ARTS_CXL_REAL
    "Link the device library named by ARTS_CXL_RAPID_INCLUDE_DIR, ARTS_CXL_RAPID_LIB_DIR, ARTS_CXL_LIB_INCLUDE_DIR and ARTS_CXL_LIB instead of the vendored fake"
    OFF)
set(ARTS_CXL_RAPID_INCLUDE_DIR "" CACHE PATH
    "ARTS_CXL_REAL=ON: the directory holding the Rapid SDK headers (MemOps.h, SharedAlloc.h)")
set(ARTS_CXL_RAPID_LIB_DIR "" CACHE PATH
    "ARTS_CXL_REAL=ON: the directory holding the Rapid SDK libraries (librapid.so, libWrapper.so)")
set(ARTS_CXL_LIB_INCLUDE_DIR "" CACHE PATH
    "ARTS_CXL_REAL=ON: the directory holding the arts_cxl_lib installed headers")
set(ARTS_CXL_LIB "" CACHE FILEPATH
    "ARTS_CXL_REAL=ON: the device glue library file (libarts_cxl_lib.so)")
set(ARTS_CXL_DB_ARENA_SIZE_BYTES "5000000000" CACHE STRING
    "Bytes per CXL arena: one data-block arena per device count")
if(NOT ARTS_CXL_DB_ARENA_SIZE_BYTES MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "ARTS_CXL_DB_ARENA_SIZE_BYTES must be a positive integer, got '${ARTS_CXL_DB_ARENA_SIZE_BYTES}'")
endif()

# ── No-hint placement policies (runtime-wide) ───────────────────────────────
# Select what the runtime does when the caller expresses no placement
# preference; an explicit hint always wins.  The defaults differ on purpose:
# an EDT chain seeded on one rank can never bootstrap distribution by staying
# with its creator, so no-hint EDTs round-robin; a DB created by an
# already-placed EDT inherits that distribution, so no-hint DB homes stay
# with the creator (first-touch).
set(ARTS_NOHINT_EDT_PLACEMENT "ROUNDROBIN" CACHE STRING
    "No-hint EDT placement policy (NULL hint or rank == ARTS_HINT_ANY_RANK): \
ROUNDROBIN (default) or CREATOR")
set_property(CACHE ARTS_NOHINT_EDT_PLACEMENT PROPERTY STRINGS ROUNDROBIN CREATOR)
if(NOT ARTS_NOHINT_EDT_PLACEMENT MATCHES "^(ROUNDROBIN|CREATOR)$")
  message(FATAL_ERROR
          "ARTS_NOHINT_EDT_PLACEMENT must be ROUNDROBIN or CREATOR, got: ${ARTS_NOHINT_EDT_PLACEMENT}")
endif()
if(ARTS_NOHINT_EDT_PLACEMENT STREQUAL "CREATOR")
  set(_ARTS_NOHINT_EDT_RR 0)
else()
  set(_ARTS_NOHINT_EDT_RR 1)
endif()

set(ARTS_NOHINT_DB_HOME "CREATOR" CACHE STRING
    "No-hint DB home policy (NULL hint; explicit hint rank or pre-reserved \
GUID always wins): CREATOR (default; first-touch — home stays on the \
creating rank) or ROUNDROBIN (distribute homes across ranks)")
set_property(CACHE ARTS_NOHINT_DB_HOME PROPERTY STRINGS CREATOR ROUNDROBIN)
if(NOT ARTS_NOHINT_DB_HOME MATCHES "^(CREATOR|ROUNDROBIN)$")
  message(FATAL_ERROR
          "ARTS_NOHINT_DB_HOME must be CREATOR or ROUNDROBIN, got: ${ARTS_NOHINT_DB_HOME}")
endif()
if(ARTS_NOHINT_DB_HOME STREQUAL "ROUNDROBIN")
  set(_ARTS_NOHINT_DB_RR 1)
else()
  set(_ARTS_NOHINT_DB_RR 0)
endif()

# ── Diagnostics ────────────────────────────────────────────────────────────
# An empty level is the build type's: Debug -> 3 (DEBUG), Release -> 1 (WARN).
# A level someone set stays as set; a normal variable carries the resolved one.
set(ARTS_LOG_LEVEL "" CACHE STRING
    "Log verbosity: 0=ERROR, 1=+WARN, 2=+INFO, 3=+DEBUG; empty = the build type's (Debug 3, Release 1)")
set_property(CACHE ARTS_LOG_LEVEL PROPERTY STRINGS "" 0 1 2 3)
if("${ARTS_LOG_LEVEL}" STREQUAL "")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(ARTS_LOG_LEVEL 3)
    else()
        set(ARTS_LOG_LEVEL 1)
    endif()
elseif(NOT ARTS_LOG_LEVEL MATCHES "^[0-3]$")
    message(FATAL_ERROR
        "Invalid ARTS_LOG_LEVEL='${ARTS_LOG_LEVEL}'. Expected one of: 0, 1, 2, 3, "
        "or empty for the build type's")
endif()

# ── Debug facilities ───────────────────────────────────────────────────────
# A Release build carries none of them.  A Debug build emits diagnostics by
# ARTS_LOG_LEVEL and checks its assertions (NDEBUG unset); every facility that
# adds cost or changes what a run does is an option of its own, OFF by default,
# refused in a Release build, and free to combine with every other one.

# One choice, because the compiler cannot combine AddressSanitizer with
# ThreadSanitizer in one binary.  C and C++ only: ASan's shadow memory
# conflicts with the CUDA driver's address-space mappings.
set(ARTS_DEBUG_SANITIZER "OFF" CACHE STRING
    "Compiler instrumentation: OFF, ADDRESS (ASan + UBSan + LSan) or THREAD (TSan + UBSan); C/C++ only")
set_property(CACHE ARTS_DEBUG_SANITIZER PROPERTY STRINGS OFF ADDRESS THREAD)
if(NOT ARTS_DEBUG_SANITIZER MATCHES "^(OFF|ADDRESS|THREAD)$")
    message(FATAL_ERROR
        "ARTS_DEBUG_SANITIZER must be OFF, ADDRESS or THREAD, got "
        "'${ARTS_DEBUG_SANITIZER}'")
endif()
# Each hook stays inert until its own environment variable is set, so one
# switch compiles them all and the run chooses which to use.
option(ARTS_DEBUG_INJECTION
    "Compile the run-time injection hooks, each inert until its environment variable is set: ARTS_SCHED_FUZZ, ARTS_WRITE_COMPLETION_DELAY_MS, ARTS_NET_MAX_MSG, ARTS_REGPOOL_FORCE_FULL_NODES" OFF)
# Holds every freed coherent descriptor back, filled, and checks the fill when
# it leaves: active in every run of the tree, since it watches all of them.
option(ARTS_DEBUG_DB_QUARANTINE
    "Fill each freed coherent data-block descriptor and hold it back for the next 1024 frees by the same thread, reporting a write through a reference that outlived it" OFF)

set(ARTS_DEBUG_FACILITIES ARTS_DEBUG_INJECTION ARTS_DEBUG_DB_QUARANTINE)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # INFO and DEBUG messages, and the teardown walk that only DEBUG compiles,
    # are a Debug build's own emission.  Every refusal is named at once.
    set(_refused "")
    if(ARTS_LOG_LEVEL GREATER 1)
        list(APPEND _refused "-DARTS_LOG_LEVEL=1 (or 0, or empty: the build type's)")
    endif()
    if(NOT ARTS_DEBUG_SANITIZER STREQUAL "OFF")
        list(APPEND _refused "-DARTS_DEBUG_SANITIZER=OFF")
    endif()
    foreach(_facility IN LISTS ARTS_DEBUG_FACILITIES)
        if(${_facility})
            list(APPEND _refused "-D${_facility}=OFF")
        endif()
    endforeach()
    if(_refused)
        list(JOIN _refused " " _refused)
        message(FATAL_ERROR
            "a Release build carries no debug facility and emits errors and "
            "warnings only; pass ${_refused}, or configure a separate Debug "
            "tree")
    endif()
endif()
foreach(_facility IN LISTS ARTS_DEBUG_FACILITIES)
    if(${_facility})
        add_compile_definitions(${_facility}=1)
        message(STATUS "Debug facility: ${_facility}")
    endif()
endforeach()

# ── Build tooling ──────────────────────────────────────────────────────────
# (To pick a faster linker, use CMake's own -DCMAKE_LINKER_TYPE=MOLD/LLD/GOLD
#  (cmake >= 3.29) or -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold.)
# A fresh tree measures with every counter compiled out; a counter set is opted
# into per campaign (artsrun -c), never inherited from the tree's default.
set(ARTS_COUNTER_CONFIG
    "${CMAKE_CURRENT_SOURCE_DIR}/configs/counters_off.cfg"
    CACHE FILEPATH "ARTS counter configuration file (parsed to generate introspection macros)")

# ── Protocol × second-axes validation + arm derivation ─────────────────────
# The user-facing axes reduce to one protocol ARM — the TU family that gets
# compiled and the ARTS_PROTOCOL_* macro it defines:
#   (VAL,  WT/WB)  -> arm VAL      (coherence/val/,     ARTS_PROTOCOL_VAL)
#   (INV,  WT/WB)  -> arm INV      (coherence/inv/,     ARTS_PROTOCOL_INV)
#   (EXCL, P/R)    -> arm EXCL     (coherence/excl/,    ARTS_PROTOCOL_EXCL)
#   (FLUSH)        -> arm FLUSH    (coherence/flush/,   ARTS_PROTOCOL_FLUSH)
# ARTS_COHERENCE_ARM is internal — downstream CMake (TU selection, tests)
# branches on the arm, never on the raw axes.
if(NOT ARTS_COHERENCE_PROTOCOL STREQUAL "VAL" AND
   NOT ARTS_COHERENCE_PROTOCOL STREQUAL "INV" AND
   NOT ARTS_COHERENCE_PROTOCOL STREQUAL "EXCL" AND
   NOT ARTS_COHERENCE_PROTOCOL STREQUAL "FLUSH")
  message(FATAL_ERROR
          "ARTS_COHERENCE_PROTOCOL must be VAL, INV, EXCL or FLUSH, got: ${ARTS_COHERENCE_PROTOCOL}")
endif()
if(NOT ARTS_WRITE_POLICY STREQUAL "WT" AND NOT ARTS_WRITE_POLICY STREQUAL "WB")
  message(FATAL_ERROR
          "ARTS_WRITE_POLICY must be WT or WB, got: ${ARTS_WRITE_POLICY}")
endif()
if(NOT ARTS_RELEASE_POLICY STREQUAL "PURGE" AND NOT ARTS_RELEASE_POLICY STREQUAL "RETAIN")
  message(FATAL_ERROR
          "ARTS_RELEASE_POLICY must be PURGE or RETAIN, got: ${ARTS_RELEASE_POLICY}")
endif()
# Per-family live axis: the dead axis must sit at its required value.
if(ARTS_COHERENCE_PROTOCOL STREQUAL "EXCL" AND ARTS_WRITE_POLICY STREQUAL "WT")
  message(FATAL_ERROR
          "EXCL x WT is not built: under exclusion no copy outlives a write "
          "turn, so the payload rides the permission and the release policy "
          "(ARTS_RELEASE_POLICY=PURGE|RETAIN) already decides both. "
          "Use ARTS_WRITE_POLICY=WB.")
endif()
if((ARTS_COHERENCE_PROTOCOL STREQUAL "INV" OR ARTS_COHERENCE_PROTOCOL STREQUAL "VAL")
   AND ARTS_RELEASE_POLICY STREQUAL "PURGE"
   AND ARTS_WRITE_POLICY STREQUAL "WB")
  message(FATAL_ERROR
          "${ARTS_COHERENCE_PROTOCOL} x WB x PURGE is not built: a voluntary "
          "return hands back only the permission while the bytes stay at the "
          "ex-owner, so the home still cannot serve a reader on its own — and "
          "shipping the bytes home too is, by definition, write-through. "
          "Use ARTS_WRITE_POLICY=WT (where the return is metadata-only) or "
          "ARTS_RELEASE_POLICY=RETAIN.")
endif()
if(ARTS_COHERENCE_PROTOCOL STREQUAL "FLUSH")
  if(NOT ARTS_WRITE_POLICY STREQUAL _ARTS_DEFAULT_WRITE_POLICY OR
     NOT ARTS_RELEASE_POLICY STREQUAL _ARTS_DEFAULT_RELEASE_POLICY)
    message(FATAL_ERROR
            "FLUSH has no write-policy or release-policy axis: every remote "
            "acquire fetches and every remote RW release writes through, so "
            "leave ARTS_WRITE_POLICY and ARTS_RELEASE_POLICY at their defaults.")
  endif()
  set(ARTS_COHERENCE_ARM "FLUSH")
  message(STATUS "ARTS: model DB-WRF, protocol FLUSH")
elseif(ARTS_COHERENCE_PROTOCOL STREQUAL "INV")
  # INV (invalidation): readers are never blocked, tracked only as a
  # node-granular copy roster, and every RW release retires every valid copy
  # in an invalidation round whose acks it collects before returning.  WT
  # additionally publishes each release to the home (home-canonical); WB
  # keeps the canonical copy at the owner and moves it owner-to-owner by
  # migration (home is a pure directory that redirects reads), so its publish
  # is control only: it opens the round and carries no payload, and its ack is
  # awaited like the invalidate acks.
  set(ARTS_COHERENCE_ARM "INV")
  message(STATUS "ARTS: model OCR, family INV, write policy ${ARTS_WRITE_POLICY}, release policy ${ARTS_RELEASE_POLICY}")
elseif(ARTS_COHERENCE_PROTOCOL STREQUAL "EXCL")
  # EXCL (exclusion, entry-consistency style): a DB is held in one access mode
  # at a time (RO-shared or RW-exclusive); reader/writer are time-exclusive;
  # writers are passive (park, no invalidate push); all inter-node transitions
  # serialize through the GUID home's single lock_state word.  The release
  # policy decides what the zero edge does: PURGE returns grant and payload to
  # the home, RETAIN keeps both until the home recalls them.
  set(ARTS_COHERENCE_ARM "EXCL")
  message(STATUS "ARTS: model OCR, family EXCL, release policy ${ARTS_RELEASE_POLICY}")
elseif(ARTS_COHERENCE_PROTOCOL STREQUAL "VAL")
  set(ARTS_COHERENCE_ARM "VAL")
  message(STATUS "ARTS: model OCR, family VAL, write policy ${ARTS_WRITE_POLICY}, release policy ${ARTS_RELEASE_POLICY}")
endif()
if(ARTS_MALLOC STREQUAL "mimalloc")
  add_compile_definitions(ARTS_MALLOC_MIMALLOC=1)
  message(STATUS "ARTS allocator: mimalloc (vendored static)")
elseif(ARTS_MALLOC STREQUAL "system")
  message(STATUS "ARTS allocator: system (libc malloc)")
else()
  message(FATAL_ERROR "ARTS_MALLOC must be 'mimalloc' or 'system', got '${ARTS_MALLOC}'")
endif()

# ── CXL: which library, and whether this tree's own library is a CXL one ──
# The two CXL benchmark variants exist in every benchmark tree, so a tree
# needs the library whenever it builds benchmarks or its own library is CXL.
set(ARTS_CXL_NEEDED OFF)
if(ARTS_USE_CXL OR ARTS_BUILD_BENCHMARKS)
    set(ARTS_CXL_NEEDED ON)
endif()

set(ARTS_CXL_TREE_RESIDENCY "")
if(ARTS_USE_CXL)
    if(NOT ARTS_COHERENCE_PROTOCOL STREQUAL "EXCL" OR NOT ARTS_RELEASE_POLICY STREQUAL "PURGE")
        message(FATAL_ERROR
            "ARTS_USE_CXL=ON requires ARTS_COHERENCE_PROTOCOL=EXCL and "
            "ARTS_RELEASE_POLICY=PURGE (EXCL already implies WB): the store is "
            "the cell that moves permission, not payload; got "
            "${ARTS_COHERENCE_PROTOCOL} x ${ARTS_RELEASE_POLICY}")
    endif()
    if(NOT ARTS_CXL_RESIDENCY MATCHES "^(STAGED|DIRECT)$")
        message(FATAL_ERROR
            "ARTS_USE_CXL=ON needs -DARTS_CXL_RESIDENCY=STAGED|DIRECT, got '${ARTS_CXL_RESIDENCY}'")
    endif()
    set(ARTS_CXL_TREE_RESIDENCY "${ARTS_CXL_RESIDENCY}")
    message(STATUS "ARTS CXL: this tree's library keeps payloads in CXL memory (residency ${ARTS_CXL_TREE_RESIDENCY})")
elseif(NOT ARTS_CXL_RESIDENCY STREQUAL "")
    message(FATAL_ERROR "ARTS_CXL_RESIDENCY belongs to ARTS_USE_CXL=ON; this tree has it OFF")
endif()

set(ARTS_CXL_INCLUDE_DIR "")
set(ARTS_CXL_LIB_PATH "")
set(ARTS_CXL_RAPID_LIBS "")
if(ARTS_CXL_NEEDED)
    if(ARTS_CXL_REAL)
        foreach(_v ARTS_CXL_RAPID_INCLUDE_DIR ARTS_CXL_RAPID_LIB_DIR ARTS_CXL_LIB_INCLUDE_DIR ARTS_CXL_LIB)
            if(NOT ${_v} OR NOT EXISTS "${${_v}}")
                message(FATAL_ERROR
                    "ARTS_CXL_REAL=ON needs ${_v} to name an existing path; got '${${_v}}'")
            endif()
        endforeach()
        set(ARTS_CXL_INCLUDE_DIR "${ARTS_CXL_LIB_INCLUDE_DIR}" "${ARTS_CXL_RAPID_INCLUDE_DIR}")
        set(ARTS_CXL_LIB_PATH "${ARTS_CXL_LIB}")
        set(ARTS_CXL_RAPID_LIBS
            "-L${ARTS_CXL_RAPID_LIB_DIR}"
            "-Wl,-rpath,${ARTS_CXL_RAPID_LIB_DIR}"
            "-lrapid")
        message(STATUS "ARTS CXL: device library ${ARTS_CXL_LIB_PATH}, headers ${ARTS_CXL_INCLUDE_DIR}, rapid libs ${ARTS_CXL_RAPID_LIB_DIR}")
    else()
        if(ARTS_CXL_RAPID_INCLUDE_DIR OR ARTS_CXL_RAPID_LIB_DIR OR ARTS_CXL_LIB_INCLUDE_DIR OR ARTS_CXL_LIB)
            message(FATAL_ERROR
                "ARTS_CXL_RAPID_INCLUDE_DIR, ARTS_CXL_RAPID_LIB_DIR, ARTS_CXL_LIB_INCLUDE_DIR, and ARTS_CXL_LIB belong to ARTS_CXL_REAL=ON; "
                "with it OFF the vendored fake library is linked and a path here is a mistake")
        endif()
        # The vendored fake: one host's shared memory mapped at one fixed
        # address, speaking the same MemOps.h/SharedAlloc.h.  Built at
        # configure time so its path exists when the targets below are made.
        set(_fake_cxl_src "${PROJECT_SOURCE_DIR}/third_party/fake_arts_cxl_lib")
        include(${PROJECT_SOURCE_DIR}/cmake/ArtsSubmodule.cmake)
        arts_init_submodule(third_party/fake_arts_cxl_lib CMakeLists.txt)
        set(_fake_cxl_build "${CMAKE_BINARY_DIR}/third_party/fake_arts_cxl_lib-build")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -S ${_fake_cxl_src} -B ${_fake_cxl_build}
                    -G ${CMAKE_GENERATOR} -DCMAKE_BUILD_TYPE=Release
                    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
            RESULT_VARIABLE _fake_cxl_rc
            OUTPUT_VARIABLE _fake_cxl_out ERROR_VARIABLE _fake_cxl_err)
        if(NOT _fake_cxl_rc EQUAL 0)
            message(FATAL_ERROR
                "configuring the vendored fake CXL library failed (${_fake_cxl_rc}):\n"
                "${_fake_cxl_out}\n${_fake_cxl_err}")
        endif()
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${_fake_cxl_build} --target arts_cxl_lib
            RESULT_VARIABLE _fake_cxl_rc
            OUTPUT_VARIABLE _fake_cxl_out ERROR_VARIABLE _fake_cxl_err)
        if(NOT _fake_cxl_rc EQUAL 0)
            message(FATAL_ERROR
                "building the vendored fake CXL library failed (${_fake_cxl_rc}):\n"
                "${_fake_cxl_out}\n${_fake_cxl_err}")
        endif()
        set(ARTS_CXL_INCLUDE_DIR "${_fake_cxl_src}/inc")
        set(ARTS_CXL_LIB_PATH "${_fake_cxl_build}/src/libarts_cxl_lib.so")
        if(NOT EXISTS "${ARTS_CXL_LIB_PATH}")
            message(FATAL_ERROR "the vendored fake CXL library did not produce ${ARTS_CXL_LIB_PATH}")
        endif()
        message(STATUS "ARTS CXL: vendored fake library ${ARTS_CXL_LIB_PATH}")
    endif()
elseif(ARTS_CXL_REAL OR ARTS_CXL_RAPID_INCLUDE_DIR OR ARTS_CXL_RAPID_LIB_DIR OR ARTS_CXL_LIB_INCLUDE_DIR OR ARTS_CXL_LIB)
    message(FATAL_ERROR
        "ARTS_CXL_REAL / ARTS_CXL_RAPID_INCLUDE_DIR / ARTS_CXL_RAPID_LIB_DIR / ARTS_CXL_LIB_INCLUDE_DIR / ARTS_CXL_LIB were given, but this "
        "tree builds nothing that links a CXL library (ARTS_USE_CXL=OFF, ARTS_BUILD_BENCHMARKS=OFF)")
endif()

# Compiler-specific settings
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Configuring Debug build")

    # Common debug flags
    add_compile_options(
        -O0 -g3 -Wall -Wextra
    )

    if(ARTS_DEBUG_SANITIZER STREQUAL "ADDRESS")
        add_compile_options(
            $<$<NOT:$<COMPILE_LANGUAGE:CUDA>>:-fsanitize=address>
            $<$<NOT:$<COMPILE_LANGUAGE:CUDA>>:-fsanitize=undefined>
            $<$<NOT:$<COMPILE_LANGUAGE:CUDA>>:-fsanitize=leak>
        )
        add_link_options(-fsanitize=address -fsanitize=undefined -fsanitize=leak)
        message(STATUS "Debug facility: ARTS_DEBUG_SANITIZER=ADDRESS (ASan + UBSan + LSan)")
    elseif(ARTS_DEBUG_SANITIZER STREQUAL "THREAD")
        add_compile_options(
            $<$<NOT:$<COMPILE_LANGUAGE:CUDA>>:-fsanitize=thread>
            $<$<NOT:$<COMPILE_LANGUAGE:CUDA>>:-fsanitize=undefined>
        )
        add_link_options(-fsanitize=thread -fsanitize=undefined)
        message(STATUS "Debug facility: ARTS_DEBUG_SANITIZER=THREAD (TSan + UBSan)")
    endif()
else()
    message(STATUS "Configuring Release build")
    add_compile_options(-march=native -mtune=native)
endif()

# The unused-* family is suppressed tree-wide: it flags dead locals and
# ignored I/O return values, never a wrong computation, and at this volume
# it drowns the classes that do.  -Wunused covers the group's members;
# unused-parameter and unused-result live outside the group and are named
# separately.  Targets that pass their own -Wall must re-append this list
# AFTER it (a later -Wall re-enables the group).
set(ARTS_WNO_UNUSED
    $<$<NOT:$<COMPILE_LANGUAGE:CUDA>>:-Wno-unused>
    $<$<NOT:$<COMPILE_LANGUAGE:CUDA>>:-Wno-unused-parameter>
    $<$<NOT:$<COMPILE_LANGUAGE:CUDA>>:-Wno-unused-result>)
add_compile_options(${ARTS_WNO_UNUSED})

# CUDA support detection (only when GPU support is requested)
set(CUDA_AVAILABLE OFF)
if(ARTS_USE_GPU)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        set(CMAKE_CUDA_STANDARD 17)
        set(CMAKE_CUDA_STANDARD_REQUIRED ON)
        set(CMAKE_CUDA_SEPARABLE_COMPILATION ON)

        message(STATUS "CUDA compiler detected: ${CMAKE_CUDA_COMPILER}")

        # Try to find CUDA toolkit - required for full CUDA support
        # Fix for CUDA 12.0+ where nvToolsExt is no longer a library but header-only
        if(NOT TARGET CUDA::nvToolsExt)
            add_library(CUDA::nvToolsExt INTERFACE IMPORTED)
        endif()

        find_package(CUDAToolkit QUIET)
        if(CUDAToolkit_FOUND)
            message(STATUS "CUDA toolkit found: ${CUDAToolkit_VERSION}")
            set(CUDA_AVAILABLE ON)
        else()
            message(STATUS "CUDA compiler available but CUDAToolkit not found")
            message(STATUS "  CUDA support disabled - toolkit required for CUDA library")
            set(CUDA_AVAILABLE OFF)
        endif()

        # Set CUDA architectures - auto-detect if explicitly requested
        if(ARTS_USE_LOCAL_CUDA_ARCHITECTURES)
            # Try to auto-detect GPU compute capability using nvidia-smi
            find_program(NVIDIA_SMI nvidia-smi)
            if(NVIDIA_SMI)
                execute_process(
                    COMMAND ${NVIDIA_SMI} --query-gpu=compute_cap --format=csv,noheader,nounits
                    OUTPUT_VARIABLE NVIDIA_SMI_OUTPUT
                    ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )

                if(NVIDIA_SMI_OUTPUT)
                    # Parse compute capabilities for all GPUs (e.g., "8.6\n7.5" -> "86;75")
                    string(REPLACE "\n" ";" GPU_CAPS ${NVIDIA_SMI_OUTPUT})
                    string(REPLACE "." "" CUDA_ARCH_NUMS "${GPU_CAPS}")

                    # Remove duplicates and sort
                    list(REMOVE_DUPLICATES CUDA_ARCH_NUMS)
                    list(SORT CUDA_ARCH_NUMS)

                    set(CMAKE_CUDA_ARCHITECTURES ${CUDA_ARCH_NUMS})
                    message(STATUS "ARTS_USE_LOCAL_CUDA_ARCHITECTURES enabled - detected CUDA architectures: ${CMAKE_CUDA_ARCHITECTURES}")
                else()
                    message(WARNING "ARTS_USE_LOCAL_CUDA_ARCHITECTURES enabled but nvidia-smi detection failed - keeping default CMAKE_CUDA_ARCHITECTURES: ${CMAKE_CUDA_ARCHITECTURES}")
                endif()
            else()
                message(WARNING "ARTS_USE_LOCAL_CUDA_ARCHITECTURES enabled but nvidia-smi not found - keeping default CMAKE_CUDA_ARCHITECTURES: ${CMAKE_CUDA_ARCHITECTURES}")
            endif()
        else()
            message(STATUS "CUDA architectures: ${CMAKE_CUDA_ARCHITECTURES}")
        endif()

        # CUDA compiler flags
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            add_compile_options($<$<COMPILE_LANGUAGE:CUDA>:-G>)
        endif()

        # Ensure NVCC uses the same host compiler and flags as the C++ compiler
        set(CMAKE_CUDA_HOST_COMPILER ${CMAKE_CXX_COMPILER})
        if(CMAKE_CXX_FLAGS)
            string(REPLACE " " "," CUDA_HOST_FLAGS "${CMAKE_CXX_FLAGS}")
            add_compile_options($<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=${CUDA_HOST_FLAGS}>)
        endif()
    else()
        message(STATUS "CUDA compiler not found - CUDA support unavailable")
    endif()
else()
    message(STATUS "GPU support disabled (ARTS_USE_GPU=OFF)")
endif()

# Determine which libraries to build
set(BUILD_CUDA_LIBRARY OFF)
if(ARTS_USE_GPU AND CUDA_AVAILABLE)
    # GPU support enabled and CUDA is available
    set(BUILD_CUDA_LIBRARY ON)
    message(STATUS "Will build both standard ARTS library and CUDA-enabled ARTS library")
else()
    message(STATUS "Will build only standard ARTS library (GPU support disabled)")
endif()

# Find required system libraries
find_package(Threads REQUIRED)
if(NOT APPLE)
    find_library(RT_LIBRARY rt REQUIRED)
    find_library(M_LIBRARY m REQUIRED)
else()
    set(RT_LIBRARY "")
    set(M_LIBRARY "")
endif()

# Add third-party dependencies
add_subdirectory(third_party)

# Hardware locality (hwloc) — always built from source, v2+ required
if(HWLOC_VERSION VERSION_GREATER_EQUAL "2.0")
    message(STATUS "Hardware topology: hwloc v${HWLOC_VERSION} (source build)")
else()
    message(FATAL_ERROR "hwloc v${HWLOC_VERSION} < 2.0 — v2+ required")
endif()

# Build-wide level, overridable per target by setting the target property of
# the same name.  A directory definition cannot be dropped by the targets that
# inherit it, so the override has to be expressed inside the definition itself
# — appending a second -D would leave both on the command line, where the
# outcome rests on their relative order and the preprocessor reports a
# redefinition.  The emptiness test cannot be a boolean one: 0 is a level.
add_compile_definitions(
    ARTS_LOG_LEVEL=$<IF:$<STREQUAL:$<TARGET_PROPERTY:ARTS_LOG_LEVEL>,>,${ARTS_LOG_LEVEL},$<TARGET_PROPERTY:ARTS_LOG_LEVEL>>)
message(STATUS "Log level: ${ARTS_LOG_LEVEL} (0=ERROR, 1=+WARN, 2=+INFO, 3=+DEBUG)")

set(COUNTER_CONFIG_FILE "${ARTS_COUNTER_CONFIG}")

# Create the ARTS library targets (core + extensions)
add_subdirectory(libs)

# Optional components
if(ARTS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ARTS_BUILD_SHIM)
    add_subdirectory(benchmarks/ocr_shim)
endif()

if(ARTS_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

add_subdirectory(configs)

# Documentation (Doxygen XML → Breathe → Sphinx HTML)
if(ARTS_BUILD_DOCS)
    find_package(Doxygen REQUIRED)
    find_program(SPHINX_EXECUTABLE sphinx-build
        HINTS "$ENV{HOME}/.local/bin" "${CMAKE_SOURCE_DIR}/.venv/bin"
        DOC "Sphinx documentation generator"
    )
    if(NOT SPHINX_EXECUTABLE)
        message(FATAL_ERROR
            "sphinx-build not found. Install with:\n"
            "  pip install -r docs/requirements.txt")
    endif()

    # Paths for Doxyfile.in @VAR@ substitution
    set(DOXYGEN_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/docs/doxygen")
    set(DOXYGEN_INPUT_DIRS
        "${CMAKE_SOURCE_DIR}/libs/include/public/arts.h \
         ${CMAKE_SOURCE_DIR}/libs/include/public/arts/graph.h \
         ${CMAKE_SOURCE_DIR}/libs/include/public/arts/gpu.h \
         ${CMAKE_SOURCE_DIR}/libs/include/internal/arts/runtime_types.h \
         ${CMAKE_SOURCE_DIR}/libs/include/internal/arts/coherence/types_common.h \
         ${CMAKE_SOURCE_DIR}/libs/include/internal/arts/gas/guid.h \
         ${CMAKE_SOURCE_DIR}/libs/include/internal/arts/defs.h"
    )
    configure_file(
        "${CMAKE_SOURCE_DIR}/docs/Doxyfile.in"
        "${CMAKE_CURRENT_BINARY_DIR}/docs/Doxyfile"
        @ONLY
    )

    # Target: ninja doxygen  (XML only)
    add_custom_target(doxygen
        COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/docs/Doxyfile"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Generating Doxygen XML..."
        VERBATIM
    )

    # Target: ninja docs  (Sphinx HTML, depends on doxygen)
    set(SPHINX_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/docs/sphinx")
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E env
        "BREATHE_DOXYGEN_XML_DIR=${DOXYGEN_OUTPUT_DIR}/xml"
        ${SPHINX_EXECUTABLE}
        -b html
        -c "${CMAKE_SOURCE_DIR}/docs"
        "${CMAKE_SOURCE_DIR}/docs"
        "${SPHINX_BUILD_DIR}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Building Sphinx HTML documentation..."
        DEPENDS doxygen
        VERBATIM
    )

    message(STATUS "Documentation: enabled (ninja doxygen | ninja docs)")
endif()

# Installation configuration
# Install public headers — all other headers are internal.
# External users: #include "arts.h", #include "arts/graph.h"
install(FILES libs/include/public/arts.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY libs/include/public/arts/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/arts
    FILES_MATCHING PATTERN "*.h"
)

# Install library targets
install(TARGETS arts_static
    EXPORT ARTSTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if(BUILD_CUDA_LIBRARY)
    install(TARGETS arts_cuda_static
        EXPORT ARTSTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

if(ARTS_BUILD_SHARED)
    install(TARGETS arts_shared
        EXPORT ARTSTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    if(BUILD_CUDA_LIBRARY)
        install(TARGETS arts_cuda_shared
            EXPORT ARTSTargets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()
endif()

# Install the interface target
install(TARGETS arts_interface
    EXPORT ARTSTargets
)

# Generate and install CMake config files for find_package support
include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ARTSConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ARTSConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ARTS
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ARTSConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ARTSConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ARTSConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ARTS
)

# Export targets (only export the main libraries, not object libraries)
install(EXPORT ARTSTargets
    FILE ARTSTargets.cmake
    NAMESPACE ARTS::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ARTS
)

# Print configuration summary
message(STATUS "")
message(STATUS "=================== ARTS Configuration Summary ===================")
message(STATUS "Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "Install prefix:    ${CMAKE_INSTALL_PREFIX}")
message(STATUS "C Compiler:        ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "CXX Compiler:      ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
if(CMAKE_CUDA_COMPILER)
    message(STATUS "CUDA Compiler:     ${CMAKE_CUDA_COMPILER_VERSION}")
    message(STATUS "CUDA Architectures: ${CMAKE_CUDA_ARCHITECTURES}")
endif()
message(STATUS "")
message(STATUS "Features:")
message(STATUS "  Standard Library: ON (always built)")
if(BUILD_CUDA_LIBRARY)
    message(STATUS "  CUDA Library:     ON (libarts_cuda)")
else()
    message(STATUS "  CUDA Library:     OFF")
endif()
message(STATUS "  Network:          libfabric (OFI) / TCP bootstrap")
message(STATUS "  Hardware Topo:    hwloc v${HWLOC_VERSION} (source)")
message(STATUS "")
message(STATUS "Build options:")
if(ARTS_BUILD_SHARED)
    message(STATUS "  Shared library:  libarts.so (standard)")
    if(BUILD_CUDA_LIBRARY)
        message(STATUS "                   libarts_cuda.so (CUDA-enabled)")
    endif()
else()
    message(STATUS "  Shared library:  OFF")
endif()
message(STATUS "  Static library:  libarts.a (standard)")
if(BUILD_CUDA_LIBRARY)
    message(STATUS "                   libarts_cuda.a (CUDA-enabled)")
endif()
message(STATUS "  Tests:           ${ARTS_BUILD_TESTS}")
message(STATUS "  Benchmarks:      ${ARTS_BUILD_BENCHMARKS}")
message(STATUS "  OCR shim:        ${ARTS_BUILD_SHIM}")
if(ARTS_BUILD_BENCHMARKS)
    set(_arts_ref_runtimes "")
    if(TARGET ocr_xsocr_static)
        list(APPEND _arts_ref_runtimes "xsocr")
    endif()
    if(TARGET ocr_vdm_mpi)
        list(APPEND _arts_ref_runtimes "ocrvx")
    endif()
    if(TARGET hpx_ep)
        list(APPEND _arts_ref_runtimes "hpx")
    endif()
    if(_arts_ref_runtimes)
        string(REPLACE ";" "+" _arts_ref_runtimes "${_arts_ref_runtimes}")
        message(STATUS "  References:      ${_arts_ref_runtimes}")
    else()
        message(STATUS "  References:      skipped (no MPI)")
    endif()
    unset(_arts_ref_runtimes)
endif()
message(STATUS "  XSOCR:           ${ARTS_BUILD_XSOCR}")
message(STATUS "  OCRVX:           ${ARTS_BUILD_OCRVX}")
message(STATUS "  HPX:             ${ARTS_BUILD_HPX}")
message(STATUS "")
message(STATUS "Installation paths:")
message(STATUS "  Libraries:       ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
message(STATUS "  Headers:         ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}")
message(STATUS "  CMake config:    ${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/cmake/ARTS")
message(STATUS "=================================================================")
message(STATUS "")
message(STATUS "To specify a custom install prefix, use:")
message(STATUS "  cmake -DCMAKE_INSTALL_PREFIX=/path/to/install ..")
message(STATUS "=================================================================")
