# OCR benchmark applications
# Each app is built against the ARTS shim backend only.
#
# Helper libraries (reduction, timer, ocrAppUtils) are compiled from
# third_party/ocr-apps/apps/libs/src/ and linked into apps that need them.

# Every coherence configuration the app factory builds an executable for.  One
# list, referenced everywhere: a per-app copy of it silently omits new
# configurations from whichever apps forgot to grow.
set(ARTS_VARIANT_SUFFIXES
    ocr_val_wt ocr_val_wb ocr_val_wt_purge ocr_val_wt_nocomb ocr_val_wb_nocomb
    ocr_val_wt_purge_nocomb
    ocr_excl_purge ocr_excl_retain ocr_inv_wt ocr_inv_wb ocr_inv_wt_purge
    wrf_flush)

set(OCR_APPS_ROOT "${CMAKE_SOURCE_DIR}/third_party/ocr-apps/apps")
set(OCR_LIBS_ROOT "${OCR_APPS_ROOT}/libs/src")
set(OCR_INC_DIR "${CMAKE_SOURCE_DIR}/third_party/ocr/ocr/inc")

# Common OCR extension defines (needed by most apps via OCR headers)
set(OCR_EXT_DEFINES
    ENABLE_EXTENSION_AFFINITY
    ENABLE_EXTENSION_PARAMS_EVT
    ENABLE_EXTENSION_LABELING
    ENABLE_EXTENSION_RTITF
    ENABLE_EXTENSION_CHANNEL_EVT
    ENABLE_EXTENSION_COUNTED_EVT
    # Gates the sharedDB benchmark's PD-distributed per-thread EDT affinity
    # (#if defined(...) in its main.c); the app's own Makefile defines it but
    # ARTS's CMake had omitted it, pinning every thread-EDT to rank0.
    # Referenced by RSBench_intel_sharedDB only (xsbench_dist distributes via
    # its SPMD fork instead).
    SINGLE_RUN_ACROSS_PD
)

# ==========================================================================
# Helper libraries (compiled as OBJECT libraries, linked into both backends)
# ==========================================================================

# --- reduction library ---
add_library(ocr_lib_reduction OBJECT
    ${OCR_LIBS_ROOT}/reduction/reduction.c
)
target_include_directories(ocr_lib_reduction PUBLIC
    ${OCR_LIBS_ROOT}/reduction
)
target_include_directories(ocr_lib_reduction PRIVATE
    ${OCR_LIBS_ROOT}/macros/macros
    ${OCR_INC_DIR}
)
# target_link_libraries(ocr_lib_reduction PRIVATE ocr_xsocr_static)
set_target_properties(ocr_lib_reduction PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(ocr_lib_reduction PRIVATE ${BENCH_C_OPT})
target_compile_definitions(ocr_lib_reduction PRIVATE ${OCR_EXT_DEFINES} ${BENCH_OCR_TYPE_DEFINE})
target_include_directories(ocr_lib_reduction PRIVATE ${BENCH_OCR_OPTIONS_DIR})

# --- reductionTwoLevel library ---
# The same interface as `reduction`, with the tree built in two levels around
# a place-major numbering instead of flat over participant indices.  A separate
# library rather than a switch inside the shared one: an application links one
# of them.
add_library(ocr_lib_reductionTwoLevel OBJECT
    ${OCR_LIBS_ROOT}/reductionTwoLevel/reductionTwoLevel.c
)
target_include_directories(ocr_lib_reductionTwoLevel PUBLIC
    ${OCR_LIBS_ROOT}/reductionTwoLevel
)
target_include_directories(ocr_lib_reductionTwoLevel PRIVATE
    ${OCR_LIBS_ROOT}/macros/macros
    ${OCR_INC_DIR}
)
set_target_properties(ocr_lib_reductionTwoLevel PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(ocr_lib_reductionTwoLevel PRIVATE ${BENCH_C_OPT})
target_compile_definitions(ocr_lib_reductionTwoLevel PRIVATE ${OCR_EXT_DEFINES} ${BENCH_OCR_TYPE_DEFINE})
target_include_directories(ocr_lib_reductionTwoLevel PRIVATE ${BENCH_OCR_OPTIONS_DIR})

# --- timer library (x86) ---
add_library(ocr_lib_timer OBJECT
    ${OCR_LIBS_ROOT}/timer/x86/timer.c
)
target_include_directories(ocr_lib_timer PUBLIC
    ${OCR_LIBS_ROOT}/timer/x86
)
target_include_directories(ocr_lib_timer PRIVATE
    ${OCR_INC_DIR}
)
# target_link_libraries(ocr_lib_timer PRIVATE ocr_xsocr_static)
set_target_properties(ocr_lib_timer PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(ocr_lib_timer PRIVATE ${BENCH_C_OPT})
target_compile_definitions(ocr_lib_timer PRIVATE ${OCR_EXT_DEFINES} ${BENCH_OCR_TYPE_DEFINE})
target_include_directories(ocr_lib_timer PRIVATE ${BENCH_OCR_OPTIONS_DIR})

# --- reduction_v1 library (used by reduction/intel-chandra) ---
add_library(ocr_lib_reduction_v1 OBJECT
    ${OCR_LIBS_ROOT}/reduction/reduction_v1.c
)
target_include_directories(ocr_lib_reduction_v1 PUBLIC
    ${OCR_LIBS_ROOT}/reduction
)
target_include_directories(ocr_lib_reduction_v1 PRIVATE
    ${OCR_INC_DIR}
)
# target_link_libraries(ocr_lib_reduction_v1 PRIVATE ocr_xsocr_static)
set_target_properties(ocr_lib_reduction_v1 PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(ocr_lib_reduction_v1 PRIVATE ${BENCH_C_OPT})
target_compile_definitions(ocr_lib_reduction_v1 PRIVATE ${OCR_EXT_DEFINES} ${BENCH_OCR_TYPE_DEFINE})
target_include_directories(ocr_lib_reduction_v1 PRIVATE ${BENCH_OCR_OPTIONS_DIR})

# --- ocrAppUtils library ---
add_library(ocr_lib_ocrAppUtils OBJECT
    ${OCR_LIBS_ROOT}/ocrAppUtils/ocrAppUtils.c
)
target_include_directories(ocr_lib_ocrAppUtils PUBLIC
    ${OCR_LIBS_ROOT}/ocrAppUtils
)
target_include_directories(ocr_lib_ocrAppUtils PRIVATE
    ${OCR_INC_DIR}
)
# target_link_libraries(ocr_lib_ocrAppUtils PRIVATE ocr_xsocr_static)
set_target_properties(ocr_lib_ocrAppUtils PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(ocr_lib_ocrAppUtils PRIVATE ${BENCH_C_OPT})
target_compile_definitions(ocr_lib_ocrAppUtils PRIVATE ${OCR_EXT_DEFINES} ${BENCH_OCR_TYPE_DEFINE})
target_include_directories(ocr_lib_ocrAppUtils PRIVATE ${BENCH_OCR_OPTIONS_DIR})

# ==========================================================================
# OCR-VX helper library variants (compiled against OCR-VX headers / vdm.h)
# Only created when the ocr_vdm_mpi target exists.
# ==========================================================================
if(TARGET ocr_vdm_mpi)

add_library(ocr_lib_reduction_ocrvx OBJECT
    ${OCR_LIBS_ROOT}/reduction/reduction.c
)
target_include_directories(ocr_lib_reduction_ocrvx PUBLIC
    ${OCR_LIBS_ROOT}/reduction
)
target_include_directories(ocr_lib_reduction_ocrvx PRIVATE
    ${OCR_LIBS_ROOT}/macros/macros
    ${OCRVX_OCR_INC}
)
set_target_properties(ocr_lib_reduction_ocrvx PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(ocr_lib_reduction_ocrvx PRIVATE ${BENCH_C_OPT}
    "-include" "${OCRVX_COMPAT_H}")
target_compile_definitions(ocr_lib_reduction_ocrvx PRIVATE ${OCR_EXT_DEFINES} "OCR_TYPE_H=vdm.h")

add_library(ocr_lib_reductionTwoLevel_ocrvx OBJECT
    ${OCR_LIBS_ROOT}/reductionTwoLevel/reductionTwoLevel.c
)
target_include_directories(ocr_lib_reductionTwoLevel_ocrvx PUBLIC
    ${OCR_LIBS_ROOT}/reductionTwoLevel
)
target_include_directories(ocr_lib_reductionTwoLevel_ocrvx PRIVATE
    ${OCR_LIBS_ROOT}/macros/macros
    ${OCRVX_OCR_INC}
)
set_target_properties(ocr_lib_reductionTwoLevel_ocrvx PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(ocr_lib_reductionTwoLevel_ocrvx PRIVATE ${BENCH_C_OPT}
    "-include" "${OCRVX_COMPAT_H}")
target_compile_definitions(ocr_lib_reductionTwoLevel_ocrvx PRIVATE ${OCR_EXT_DEFINES} "OCR_TYPE_H=vdm.h")


add_library(ocr_lib_timer_ocrvx OBJECT
    ${OCR_LIBS_ROOT}/timer/x86/timer.c
)
target_include_directories(ocr_lib_timer_ocrvx PUBLIC
    ${OCR_LIBS_ROOT}/timer/x86
)
target_include_directories(ocr_lib_timer_ocrvx PRIVATE
    ${OCRVX_OCR_INC}
)
set_target_properties(ocr_lib_timer_ocrvx PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(ocr_lib_timer_ocrvx PRIVATE ${BENCH_C_OPT}
    "-include" "${OCRVX_COMPAT_H}")
target_compile_definitions(ocr_lib_timer_ocrvx PRIVATE ${OCR_EXT_DEFINES} "OCR_TYPE_H=vdm.h")

add_library(ocr_lib_reduction_v1_ocrvx OBJECT
    ${OCR_LIBS_ROOT}/reduction/reduction_v1.c
)
target_include_directories(ocr_lib_reduction_v1_ocrvx PUBLIC
    ${OCR_LIBS_ROOT}/reduction
)
target_include_directories(ocr_lib_reduction_v1_ocrvx PRIVATE
    ${OCRVX_OCR_INC}
)
set_target_properties(ocr_lib_reduction_v1_ocrvx PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(ocr_lib_reduction_v1_ocrvx PRIVATE ${BENCH_C_OPT}
    "-include" "${OCRVX_COMPAT_H}")
target_compile_definitions(ocr_lib_reduction_v1_ocrvx PRIVATE ${OCR_EXT_DEFINES} "OCR_TYPE_H=vdm.h")

add_library(ocr_lib_ocrAppUtils_ocrvx OBJECT
    ${OCR_LIBS_ROOT}/ocrAppUtils/ocrAppUtils.c
)
target_include_directories(ocr_lib_ocrAppUtils_ocrvx PUBLIC
    ${OCR_LIBS_ROOT}/ocrAppUtils
)
target_include_directories(ocr_lib_ocrAppUtils_ocrvx PRIVATE
    ${OCRVX_OCR_INC}
)
set_target_properties(ocr_lib_ocrAppUtils_ocrvx PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(ocr_lib_ocrAppUtils_ocrvx PRIVATE ${BENCH_C_OPT}
    "-include" "${OCRVX_COMPAT_H}")
target_compile_definitions(ocr_lib_ocrAppUtils_ocrvx PRIVATE ${OCR_EXT_DEFINES} "OCR_TYPE_H=vdm.h")

endif() # TARGET ocr_vdm_mpi

# ==========================================================================
# add_benchmark_app: builds an OCR app against both XSOCR and ARTS backends
#
# Usage:
#   add_benchmark_app(NAME <name>
#       DIR <relative-path-under-ocr-apps/apps>
#       SOURCES <file1.c> [file2.c ...]
#       [SRC_SUBDIR <subdir>]           # source files are in DIR/subdir/
#       [LIBS reduction timer ...]      # OCR helper library OBJECT targets
#       [EXTRA_INCLUDES <dir> ...]      # additional include dirs
#       [EXTRA_DEFINES <def> ...]       # additional compile definitions
#       [EXTRA_LINK_LIBS <lib> ...]     # extra link libraries (e.g. m)
#       [OCRVX_SKIP]                    # suppress _ocrvx target (app uses unsupported OCR extensions)
#       [HINTED_PLACEMENT]           # source carries a placement hint layer
#       [ROOT <abs-dir>]  # DIR is relative to this instead of ocr-apps/apps
#       [ARTS_ONLY]  # ARTS variants only (a probe that exists to separate the plane's arms)
#   )
#
# Creates, per backend, <name>_arts_<suffix> (one per coherence configuration),
# <name>_xsocr and (when compatible) <name>_ocrvx.  With HINTED_PLACEMENT the
# same set is created a second time under the name <name>_hinted, compiled with the
# hint layer enabled — so both placement settings coexist in one build tree and
# neither is a property of the build directory.
#
# A registration naming a source file the checkout does not carry creates no
# target at all and says so with a status line.
# ==========================================================================
function(add_benchmark_app)
    cmake_parse_arguments(APP "OCRVX_SKIP;HINTED_PLACEMENT;ARTS_ONLY" "NAME;DIR;SRC_SUBDIR;ROOT" "SOURCES;LIBS;EXTRA_INCLUDES;EXTRA_DEFINES;EXTRA_LINK_LIBS" ${ARGN})

    if(APP_ROOT)
        set(APP_DIR "${APP_ROOT}/${APP_DIR}")
    else()
        set(APP_DIR "${OCR_APPS_ROOT}/${APP_DIR}")
    endif()
    if(APP_SRC_SUBDIR)
        set(SRC_DIR "${APP_DIR}/${APP_SRC_SUBDIR}")
    else()
        set(SRC_DIR "${APP_DIR}")
    endif()

    # Build absolute source paths
    set(ABS_SOURCES "")
    foreach(src ${APP_SOURCES})
        list(APPEND ABS_SOURCES "${SRC_DIR}/${src}")
    endforeach()

    # One absent file would fail the whole generation, and with it every
    # program that is present.
    foreach(src ${ABS_SOURCES})
        if(NOT EXISTS "${src}")
            message(STATUS "Skipping ${APP_NAME}: ${src} is not in this checkout")
            return()
        endif()
    endforeach()

    # Collect OBJECT library objects and include dirs for helper libs
    set(LIB_OBJECTS "")
    set(LIB_INCLUDES "")
    foreach(lib ${APP_LIBS})
        list(APPEND LIB_OBJECTS $<TARGET_OBJECTS:ocr_lib_${lib}>)
        get_target_property(inc ocr_lib_${lib} INTERFACE_INCLUDE_DIRECTORIES)
        if(inc)
            list(APPEND LIB_INCLUDES ${inc})
        endif()
    endforeach()

    # Common compile settings
    set(COMMON_INCLUDES
        ${APP_DIR}
        ${SRC_DIR}
        ${OCR_LIBS_ROOT}/macros/macros
        ${APP_EXTRA_INCLUDES}
        ${LIB_INCLUDES}
        ${BENCH_OCR_OPTIONS_DIR}
    )
    set(COMMON_DEFINES ${OCR_EXT_DEFINES} ${BENCH_OCR_TYPE_DEFINE} ${APP_EXTRA_DEFINES})

    # Placement flavors.  The base name compiles the application as born, with
    # its hint layer left out; the _hinted name compiles that layer in.  A flavor
    # differs only by a define in the shared application source, so every
    # backend below sees the same setting — placement is a property of the
    # program, never of one runtime.
    set(_flavors base)
    if(APP_HINTED_PLACEMENT)
        list(APPEND _flavors hinted)
    endif()

    foreach(_flavor ${_flavors})
        if(_flavor STREQUAL hinted)
            set(_app "${APP_NAME}_hinted")
            set(_flavor_define OCR_APP_OPTIMIZED_PLACEMENT)
        else()
            set(_app "${APP_NAME}")
            set(_flavor_define "")
        endif()

        # --- XSOCR variant (only when the ocr_xsocr_static target exists) ---
        if(TARGET ocr_xsocr_static AND NOT APP_ARTS_ONLY)
            add_executable(${_app}_xsocr ${ABS_SOURCES} ${LIB_OBJECTS})
            target_include_directories(${_app}_xsocr PRIVATE ${COMMON_INCLUDES})
            target_compile_definitions(${_app}_xsocr PRIVATE ${COMMON_DEFINES} ${_flavor_define})
            target_link_libraries(${_app}_xsocr PRIVATE ocr_xsocr_static pthread ${APP_EXTRA_LINK_LIBS})
            # Non-PIE: OCR's distributed mode serializes function pointers as raw
            # addresses across MPI ranks.  With PIE + ASLR each rank gets a
            # different base address and deserialized funcPtrs become invalid.
            target_link_options(${_app}_xsocr PRIVATE -no-pie -fno-pie)
            # A vendored static libnuma/hwloc inside the executable would
            # otherwise be exported: the loader then binds the SYSTEM libnuma's
            # references -- and those of anything MPI drags in -- to this
            # executable's copy, leaving two libraries sharing half a state
            # each.  The topology query returns zero nodes and the run dies in
            # numa_bitmask_alloc before main.  Keep archive symbols local.
            target_link_options(${_app}_xsocr PRIVATE -Wl,--exclude-libs,ALL)
            set_target_properties(${_app}_xsocr PROPERTIES
                C_STANDARD 11 C_EXTENSIONS ON
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            )
            target_compile_options(${_app}_xsocr PRIVATE ${BENCH_C_OPT})
        endif()

        # --- ARTS variants (one per coherence configuration) ---
        foreach(sfx ${ARTS_VARIANT_SUFFIXES})
            add_executable(${_app}_arts_${sfx} ${ABS_SOURCES} ${LIB_OBJECTS}
                           $<TARGET_OBJECTS:ocr_arts_objects>)
            target_include_directories(${_app}_arts_${sfx} PRIVATE
                ${COMMON_INCLUDES} ${OCR_INC_DIR})
            target_compile_definitions(${_app}_arts_${sfx} PRIVATE
                ${COMMON_DEFINES} ${_flavor_define})
            target_link_libraries(${_app}_arts_${sfx} PRIVATE
                arts_static_${sfx} pthread ${APP_EXTRA_LINK_LIBS})
            target_link_options(${_app}_arts_${sfx} PRIVATE -no-pie -fno-pie)
            set_target_properties(${_app}_arts_${sfx} PROPERTIES
                C_STANDARD 11 C_EXTENSIONS ON
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
            target_compile_options(${_app}_arts_${sfx} PRIVATE ${BENCH_C_OPT})
            set_property(GLOBAL APPEND PROPERTY BENCHMARK_ARTS_TARGETS ${_app}_arts_${sfx})
        endforeach()

        # --- OCR-VX variant (only when ocr_vdm_mpi target exists, OCRVX_SKIP not set,
        #     and all required helper libs have an _ocrvx variant compiled against
        #     vdm.h headers) ---
        if(TARGET ocr_vdm_mpi AND NOT APP_OCRVX_SKIP AND NOT APP_ARTS_ONLY)
            set(_ocrvx_ok TRUE)
            foreach(lib ${APP_LIBS})
                if(NOT TARGET ocr_lib_${lib}_ocrvx)
                    set(_ocrvx_ok FALSE)
                endif()
            endforeach()
            if(_ocrvx_ok)
                set(LIB_OBJECTS_OCRVX "")
                foreach(lib ${APP_LIBS})
                    list(APPEND LIB_OBJECTS_OCRVX $<TARGET_OBJECTS:ocr_lib_${lib}_ocrvx>)
                endforeach()

                add_executable(${_app}_ocrvx ${ABS_SOURCES} ${LIB_OBJECTS_OCRVX})
                target_include_directories(${_app}_ocrvx PRIVATE
                    ${APP_DIR}
                    ${SRC_DIR}
                    ${OCR_LIBS_ROOT}/macros/macros
                    ${APP_EXTRA_INCLUDES}
                    ${LIB_INCLUDES}
                    ${OCRVX_OCR_INC}
                )
                target_compile_definitions(${_app}_ocrvx PRIVATE
                    ${OCR_EXT_DEFINES}
                    "OCR_TYPE_H=vdm.h"
                    OCR_ENABLE_EDT_NAMING
                    ${APP_EXTRA_DEFINES}
                    ${_flavor_define}
                )
                target_link_libraries(${_app}_ocrvx PRIVATE
                    ocr_vdm_mpi ${APP_EXTRA_LINK_LIBS})
                target_link_options(${_app}_ocrvx PRIVATE -no-pie -fno-pie)
                # Same reason as the sibling above: archive symbols stay local
                # so a system copy of the same library keeps its own state.
                target_link_options(${_app}_ocrvx PRIVATE -Wl,--exclude-libs,ALL)
                set_target_properties(${_app}_ocrvx PROPERTIES
                    C_STANDARD 11 C_EXTENSIONS ON
                    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
                )
                target_compile_options(${_app}_ocrvx PRIVATE ${BENCH_C_OPT}
                    "-include" "${OCRVX_COMPAT_H}")
            endif()
        endif()
    endforeach()
endfunction()

# A program written in this repository rather than carried by the
# application submodule: the same backends and flags as every other
# application, with the sources named by path.
function(add_bespoke_app)
    cmake_parse_arguments(B "ARTS_ONLY;OCRVX_SKIP" "NAME;DIR" "SOURCES;EXTRA_LINK_LIBS;EXTRA_DEFINES;EXTRA_INCLUDES" ${ARGN})
    if(B_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "add_bespoke_app: unexpected arguments: ${B_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT B_DIR)
        set(B_DIR ".")
    endif()
    set(_flags)
    if(B_ARTS_ONLY)
        list(APPEND _flags ARTS_ONLY)
    endif()
    if(B_OCRVX_SKIP)
        list(APPEND _flags OCRVX_SKIP)
    endif()
    add_benchmark_app(NAME ${B_NAME} ROOT "${CMAKE_CURRENT_SOURCE_DIR}" DIR "${B_DIR}"
        SOURCES ${B_SOURCES} EXTRA_LINK_LIBS ${B_EXTRA_LINK_LIBS}
        EXTRA_DEFINES ${B_EXTRA_DEFINES} EXTRA_INCLUDES ${B_EXTRA_INCLUDES} ${_flags})
endfunction()

# ==========================================================================
# Tier 1: Simple single-file apps
# ==========================================================================

add_benchmark_app(NAME fibonacci
    DIR fibonacci/ocr
    SOURCES fib.c
    HINTED_PLACEMENT
)

add_benchmark_app(NAME printf
    DIR printf/ocr
    SOURCES printf.c
)

add_benchmark_app(NAME quicksort
    DIR quicksort/ocr
    SOURCES quicksort.c
    # The recursion sorts one shared block; the layer places a subarray's
    # tasks by the middle of the range they sort, so the block stops
    # following the recursion around.
    HINTED_PLACEMENT
)

# --- Restructured: quicksort_dist ---
# A sample-splitter sort: the decomposition is buckets and chunks rather than
# the base's recursive split, and it takes their counts as arguments.
add_benchmark_app(NAME quicksort_dist
    DIR quicksort/ocr
    SOURCES quicksort_dist.c
)

add_benchmark_app(NAME nqueens
    DIR nqueens/refactored/ocr
    SOURCES nqueens.c
    LIBS timer
    HINTED_PLACEMENT
)

add_benchmark_app(NAME smithwaterman
    DIR smithwaterman/ocr
    SOURCES smithwaterman.c
    HINTED_PLACEMENT
)

# --- Restructured: smithwaterman_dist ---
# Same wavefront and the same kernel; each place builds the tasks for the tiles
# it owns instead of one rank prescribing the whole W^2 graph.
add_benchmark_app(NAME smithwaterman_dist
    DIR smithwaterman/ocr
    SOURCES smithwaterman_dist.c
)

# --- Restructured: tempest_dist ---
# The same cube-sphere exchange and the same cross-check, with the halo
# batched per rank pair instead of one datablock per patch edge.
add_benchmark_app(NAME tempest_dist
    DIR tempest/refactored/ocr/intel-bryan
    SOURCES tempest_dist.c
)

add_benchmark_app(NAME basicIO
    DIR basicIO/ocr
    SOURCES basicIO.c
)

# ==========================================================================
# Tier 2: Examples
# ==========================================================================

add_benchmark_app(NAME cache_offset
    DIR examples/cache-offset
    SOURCES cacheOffset.c
)

add_benchmark_app(NAME highbw
    DIR examples/highbw
    SOURCES highbw.c
    EXTRA_DEFINES NTHREADS=2 DBSIZE=1024 ITER=2
)

add_benchmark_app(NAME multigen
    DIR examples/multigen
    SOURCES multigen.c
)

add_benchmark_app(NAME multigen_2
    DIR examples/multigen_2
    SOURCES multigen.c
)

add_benchmark_app(NAME task_priorities
    DIR examples/task-priorities
    SOURCES task-priorities.c
)

add_benchmark_app(NAME testlibs
    DIR examples/testlibs
    SOURCES testlibs.c
    EXTRA_LINK_LIBS m
)

add_benchmark_app(NAME xeonNumaSize
    DIR examples/xeonNumaSize
    SOURCES numaHelpers.c actions.c utils.c xeonNumaSize.c myOcrHelpers.c
    EXTRA_LINK_LIBS arts::numa
)

# ==========================================================================
# Tier 3: Kernels and utilities
# ==========================================================================

add_benchmark_app(NAME dbctrl
    DIR kernels/dbctrl/ocr
    SOURCES dbctrl.c
)

add_benchmark_app(NAME prodcon
    DIR kernels/prodcon/ocr
    SOURCES prodcon.c
    EXTRA_DEFINES NUM_THREADS=4 ARRAY_SIZE=4096 NTIMES=3
)

add_benchmark_app(NAME curvefit
    DIR curvefit/ocr/intel
    SOURCES curveFit.c
    EXTRA_LINK_LIBS m
)

add_benchmark_app(NAME triangle
    DIR triangle/refactored/ocr/intel
    SOURCES triangle.c
    HINTED_PLACEMENT
    # COUNTED_EVENTS states the completion events' true consumer counts (the
    # program knows both before creating them).  The OCR spec destroys a ONCE
    # once its consumers are satisfied, so declaring the count changes nothing
    # a spec-conforming runtime does -- it hands a runtime that keeps fired
    # events for late binders the reclaim point the spec's wording assumes.
    # (The board destroys are unconditional in the source: freeing an
    # unreachable block is the program's own obligation, not an option.)
    EXTRA_DEFINES OCR_APP_COUNTED_EVENTS
)

add_benchmark_app(NAME tempest
    DIR tempest/refactored/ocr/intel-bryan
    SOURCES tempestCommunication.c
    HINTED_PLACEMENT
)

# ==========================================================================
# Tier 4: Medium-complexity apps
# ==========================================================================

# The hand-written-kernel port is not registered: it shares this row's exact
# decomposition -- same tiles, same DAG, same acquires -- and at eight nodes the
# two are indistinguishable (15.48 s against 15.54 s).  What differs is kernel
# speed, and the slower kernels MASK the effect this campaign exists to show:
# on the identical DAG the hand-written port anti-scales 1.55x and this one
# 4.97x, because 6x faster kernels stop hiding the acquires.  Its source stays
# in the submodule -- it is where this port's --fib reader came from.
# The app tree's standalone input/output converters — plain host C, no OCR.
# convertData turns a whitespace-text matrix into the tile-stream binary the
# app's --fib path (and the TG build) reads; convertOut turns the app's
# binary result file back into readable text.
add_executable(cholesky_convertData
    "${OCR_APPS_ROOT}/cholesky/ocr/convertData.c")
add_executable(cholesky_convertOut
    "${OCR_APPS_ROOT}/cholesky/ocr/convertOut.c")
set_target_properties(cholesky_convertData cholesky_convertOut PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# Tiled Cholesky whose per-tile kernels are the standard CBLAS/LAPACKE calls
# (dpotrf/dtrsm/dsyrk/dgemm), served by the third_party OpenBLAS submodule.
# --- Restructured: cholesky_dist ---
# Same factorisation, same 2-D block-cyclic ownership and the same kernels;
# what changes is that each place builds the tasks for the tiles it owns, on
# itself, instead of one rank prescribing the whole t^3/6 graph.
add_benchmark_app(NAME cholesky_dist
    DIR cholesky/ocr-mkl
    SOURCES cholesky_dist.c
    EXTRA_LINK_LIBS arts::openblas m
)

add_benchmark_app(NAME cholesky_blas
    DIR cholesky/ocr-mkl
    SOURCES ocr_mkl_cholesky.c
    EXTRA_LINK_LIBS arts::openblas m
    HINTED_PLACEMENT
)

add_benchmark_app(NAME fft
    DIR fft/ocr
    SOURCES fft.c verify.c
    # The source carries a placement layer, so the hinted tier is a target.
    HINTED_PLACEMENT
    EXTRA_LINK_LIBS m
)

# --- Restructured: fft_dist ---
# Bailey's four-step transpose in place of the recursive form: the tile count
# is the decomposition, and it is an argument.
add_benchmark_app(NAME fft_dist
    DIR fft/ocr
    SOURCES fft_dist.c
    EXTRA_LINK_LIBS m
)

add_benchmark_app(NAME graph500
    DIR graph500
    SOURCES graph500.c
    EXTRA_DEFINES NO_FILES NO_MAP
    # The shipped worker-to-place map indexes by worker id alone; the hinted
    # flavour tiles the R x C worker grid into one tile per place.  On a square
    # grid the two are the same partition regrouped, so the tier is not
    # registered in the catalog -- the flag is kept only so the guarded arm
    # stays compiled and checked by every build.
    HINTED_PLACEMENT
    EXTRA_LINK_LIBS m
)

# --- Restructured: graph500_dist ---
# The same edge stream, the same search from the same root and the same
# answer digest; what changes is the decomposition.  Generation is a
# partition of the stream reached by jumping its recurrence, every edge is
# routed once to the owner of its source, a level exchanges one block per
# node pair carrying only the frontier, and each node runs a persistent
# chain instead of one thread pre-creating the whole task lattice.
# Placement is part of the design, so there is no _hinted family.
add_benchmark_app(NAME graph500_dist
    DIR graph500/dist
    SOURCES graph500_dist.c
    EXTRA_LINK_LIBS m
)

# GlobalSum - 3 variants (different main files, shared gsLib.c)
add_benchmark_app(NAME globalsum_cgShim
    DIR globalsum/refactored/ocr/intel
    SOURCES cgShim.c gsLib.c
)

add_benchmark_app(NAME globalsum_cgNoShim
    DIR globalsum/refactored/ocr/intel
    SOURCES cgNoShim.c gsLib.c
)

add_benchmark_app(NAME globalsum_pcg
    DIR globalsum/refactored/ocr/intel
    SOURCES pcg.c gsLib.c
)

# ==========================================================================
# Tier 5: CoMD variants
# ==========================================================================

add_benchmark_app(NAME CoMD_intel_chandra
    DIR CoMD/refactored/ocr/intel-chandra
    SOURCES CoMD.c wtime.c command.c lj.c eam.c timers.c
    LIBS reduction
    EXTRA_LINK_LIBS m
)

add_benchmark_app(NAME CoMD_intel_chandra_tiled
    DIR CoMD/refactored/ocr/intel-chandra-tiled
    SOURCES CoMD.c mycommand.c cmdLineParser.c eam.c ljForce.c decomposition.c
    linkCells.c initAtoms.c random.c timestep.c performanceTimers.c
    haloExchange.c
    LIBS reduction ocrAppUtils
    # One EXTRA_DEFINES list rather than two: cmake_parse_arguments appends on
    # a repeated multi-value keyword, which is a property nobody reading the
    # row should have to know.  The set is the published Makefile.x86-base's
    # plus the app-supplied COUNTED output events.
    EXTRA_DEFINES OCR_APP_COUNTED_OEVT WITH_COUNTED_EVT
    CHANNEL_EVENTS_AT_RECEIVER DOUBLE_BUFFERED_EVTS
    EXTRA_LINK_LIBS m
    # The rank grid is already the program's own placement; the one thing it
    # leaves to a hint is which policy-domain grid the rank grid is blocked
    # over, and the generic near-cubic factorisation is not the one that
    # crosses the fewest halo faces on an elongated rank grid.
    HINTED_PLACEMENT
)

add_benchmark_app(NAME CoMD_sdsc
    DIR CoMD/refactored/ocr/sdsc
    SOURCES atoms.c cells.c comd.c command.c eam.c lj.c simulation.c timers.c
    timestep.c
    LIBS ocrAppUtils
    EXTRA_DEFINES OCR_APP_COUNTED_OEVT
    EXTRA_LINK_LIBS m
    HINTED_PLACEMENT
)

# --- Restructured: CoMD_sdsc_dist ---
# The same LJ physics and the same instance; what changes is the wiring plane
# and the step: spawn and reduction trees per phase, one task per cell per
# step over two generations of the cell's block (the exchange rides the
# step's neighbour reads), in-place generation from an index-seeded
# generator, and per-owner release.  Placement is part of the design, so
# there is no _hinted family.  ocrAppUtils supplies createEventHelper and
# splitDimension_Cart3D.
add_benchmark_app(NAME CoMD_sdsc_dist
    DIR CoMD/refactored/ocr/sdsc-dist
    SOURCES atoms.c cells.c comd.c command.c fan.c lj.c simulation.c timers.c
    timestep.c
    LIBS ocrAppUtils
    EXTRA_DEFINES OCR_APP_COUNTED_OEVT
    EXTRA_LINK_LIBS m
)

add_benchmark_app(NAME CoMD_sdsc2
    DIR CoMD/refactored/ocr/sdsc2
    SOURCES comd.c command.c eam.c lj.c reductions.c simulation.c timers.c
    timestep.c
    EXTRA_LINK_LIBS m
)

# ==========================================================================
# Tier 6: HPCG variants
# ==========================================================================

add_benchmark_app(NAME hpcg_intel
    DIR hpcg/refactored/ocr/intel
    SOURCES hpcg.c timers.c
    LIBS reduction
    EXTRA_LINK_LIBS m
    HINTED_PLACEMENT
)

# --- Restructured: hpcg_intel_dist ---
# Same conjugate gradient, same multigrid, same rank grid and bisection; what
# changes is what a boundary costs.  The base creates one datablock per
# direction per exchange -- 26 x 11 creates and as many destroys per tile per
# iteration, three quarters of them carrying 92 B -- so the cross-node message
# count is set by the tile adjacency and does not fall with the node count.
# This program keeps one persistent, double-buffered block per (tile,
# destination policy domain) per multigrid level: every direction packs its own
# slice, the 26-way join is untouched, the bytes are identical, and a payload
# crosses once per reading domain instead of once per reading tile.  Its
# reduction tree is built in two levels around a domain-major numbering
# (reductionTwoLevel in place of reduction), which is the same substitution
# nekbone_dist makes.  Placement is part of the design, so there is no _hinted
# family.
add_benchmark_app(NAME hpcg_intel_dist
    DIR hpcg/refactored/ocr/intel-dist
    SOURCES hpcg.c timers.c
    LIBS reductionTwoLevel
    EXTRA_LINK_LIBS m
)

# ==========================================================================
# Tier 7: Stencil variants
# ==========================================================================

# The chandra stencil source is dimension-parameterized (PROBLEM_TYPE 1|2)
# and the upstream Stencil1D port left the header's default (2) in place, so
# this target used to build the SAME 2-D problem as Stencil2D_intel_chandra.
# Select the real 1-D problem here.
add_benchmark_app(NAME Stencil1D_intel_chandra
    DIR Stencil1D/refactored/ocr/intel-chandra
    SOURCES stencil_1d.c timers.c
    LIBS reduction
    EXTRA_DEFINES PROBLEM_TYPE=1
    EXTRA_LINK_LIBS m
)

# Stencil1D/intel-david: multiple code variants (each is a standalone app),
# one per event/dependence idiom (sticky / stickyLG labeled-guid / channel /
# once / oncePI / guid / guidPI).  All ported to the current OCR API
# (ocrGuidIsEq/ocrGuidIsNull instead of struct ==/!=, hint-bearing
# ocrEdtCreate/ocrDbCreate signatures).
add_benchmark_app(NAME stencil1D_sticky
    DIR Stencil1D/refactored/ocr/intel-david
    SOURCES stencil1Dsticky.c
)


add_benchmark_app(NAME stencil1D_channel
    DIR Stencil1D/refactored/ocr/intel-david
    SOURCES stencil1Dchannel.c
)

add_benchmark_app(NAME stencil1D_stickyLG
    DIR Stencil1D/refactored/ocr/intel-david
    SOURCES stencil1DstickyLG.c
)

add_benchmark_app(NAME stencil1D_guid
    DIR Stencil1D/refactored/ocr/intel-david
    SOURCES stencil1Dguid.c
)

add_benchmark_app(NAME stencil1D_guidPI
    DIR Stencil1D/refactored/ocr/intel-david
    SOURCES stencil1DguidPI.c
)

add_benchmark_app(NAME stencil1D_once
    DIR Stencil1D/refactored/ocr/intel-david
    SOURCES stencil1Donce.c
)

add_benchmark_app(NAME stencil1D_oncePI
    DIR Stencil1D/refactored/ocr/intel-david
    SOURCES stencil1DoncePI.c
)





add_benchmark_app(NAME Stencil2D_intel_chandra
    DIR Stencil2D/refactored/ocr/intel-chandra
    SOURCES stencil_2d.c timers.c
    LIBS reduction
    EXTRA_LINK_LIBS m
)

# The guarded layer overrides one thing: the home of a tile's four halo send
# buffers.  The tile grid is already mapped onto the domain grid in contiguous
# 2-D blocks (the SPMD fork's own map, which is the minimum-cut factorisation
# at every node count), and every EDT and every other block of a tile is pinned
# to that map, so the one placement left free is where a send buffer's
# directory entry lives.  A buffer has exactly one reader for the whole run --
# the neighbour's receive task on the opposite face -- so base homes it where
# it is packed and the guarded flavour homes it where it is read, and only for
# a face whose neighbour is on another node.
add_benchmark_app(NAME Stencil2D_intel_channelEVTs
    DIR Stencil2D/refactored/ocr/intel-channelEVTs
    SOURCES stencil_2d.c timers.c
    LIBS reduction ocrAppUtils timer
    EXTRA_DEFINES STENCIL_WITH_DBUF_CHRECV CHANNEL_EVENTS_AT_RECEIVER
    EXTRA_LINK_LIBS m
    HINTED_PLACEMENT
)

# ==========================================================================
# Tier 8: P2P and Reduction
# ==========================================================================

# The guarded layer overrides one thing: the rank -> policy-domain map.  base
# deals the ranks out in contiguous blocks (the source's own `#define BLOCK`),
# which lands the wavefront's contiguous runnable interval on a contiguous
# domain interval, so a domain holding more runnable ranks than it has workers
# runs `workers` of them while the rest of the machine idles.  The guarded map
# deals runs of `chunk` ranks (argv[6], parsed in both flavours, honoured only
# here) round-robin over the domains, which costs p/chunk boundary crossings
# instead of one per domain and keeps every engaged domain saturated.
add_benchmark_app(NAME p2p
    DIR p2p/refactored/ocr/intel
    SOURCES p2p.c wtime.c
    LIBS reduction
    HINTED_PLACEMENT
)

add_benchmark_app(NAME reduction_intel
    DIR reduction/refactored/ocr/intel
    SOURCES driver.c
    LIBS reduction timer
    EXTRA_LINK_LIBS m
)

add_benchmark_app(NAME reduction_intel_chandra
    DIR reduction/refactored/ocr/intel-chandra
    SOURCES driver.c
    LIBS reduction_v1
    EXTRA_LINK_LIBS m
)

# ==========================================================================
# Tier 9: HPGMG
# ==========================================================================

add_benchmark_app(NAME hpgmg
    DIR hpgmg/refactored/ocr/sdsc
    SOURCES hpgmg.c init.c utils.c mg.c mg_edt.c solve_edt.c test_edt.c
    residual_edt.c restrict_edt.c smooth_edt.c interpolate_edt.c
    EXTRA_LINK_LIBS m
    HINTED_PLACEMENT
)

# Restructured decomposition: a separate program (its own main, lattice
# driver and face-slab exchange) that links the base port's kernel and
# per-box task sources unchanged — rank-persistent data plane, point-to-point
# phase chains, distributed initialization.  Placement is part of the design,
# so the layer is compiled in unconditionally (no _hinted family).
add_benchmark_app(NAME hpgmg_dist
    DIR hpgmg/refactored/ocr/sdsc
    SOURCES hpgmg_dist_main.c mg_dist.c exchange_dist.c
    init.c utils.c mg_edt.c solve_edt.c
    residual_edt.c restrict_edt.c smooth_edt.c interpolate_edt.c
    EXTRA_DEFINES OCR_APP_OPTIMIZED_PLACEMENT
    EXTRA_LINK_LIBS m
)


# ==========================================================================
# Tier 10: MiniAMR variants
# ==========================================================================

add_benchmark_app(NAME miniAMR_forkbomb
    DIR miniAMR/refactored/ocr/forkbomb
    SOURCES mainOCR.c
    EXTRA_DEFINES FORKBOMB_MAX_TIMESTEPS=3
    EXTRA_LINK_LIBS m
)

add_benchmark_app(NAME miniAMR_intel
    DIR miniAMR/refactored/ocr/intel
    SOURCES block.c chksum.c comm.c clone.c init.c move.c parent.c plot.c
    stencil.c refine.c root.c util.c wtime.c
    # root.h guards the block-grid capacity with #ifndef, so without a -D the
    # base block grid could not exceed 1000 blocks from a build.  The width
    # rule needs npx*npy*npz = 24*24*12 = 6912 = 2 x 3456; raise this further
    # only together with the halo-range envelope the program now loud-fails on.
    EXTRA_DEFINES MAX_NUM_UNREFINED_BLOCKS=6912
    EXTRA_LINK_LIBS m
    HINTED_PLACEMENT
)

# --- Restructured: miniAMR_intel_dist ---
# The same stencil, refinement rules and checksums; what changes is the control
# plane.  A stage of a block is one task over the strips its neighbours wrote,
# phases are emitted and collected by spawn trees indexed by rank, the mesh
# every epoch runs on is a directory one task derives from the object
# trajectory (the refinement decision never reads a cell), and the checksum
# reduces over the block forest.  There is no block-grid cap: no task holds a
# dependence per block.  Placement is part of the design, so no _hinted family.
add_benchmark_app(NAME miniAMR_intel_dist
    DIR miniAMR/refactored/ocr/intel-dist
    SOURCES amr_dist.c command.c fan.c mesh.c physics.c
    LIBS ocrAppUtils
    EXTRA_DEFINES OCR_APP_COUNTED_OEVT
    EXTRA_LINK_LIBS m
)

add_benchmark_app(NAME miniAMR_intel_bryan
    DIR miniAMR/refactored/ocr/intel-bryan
    SOURCES mainOCR.c
    LIBS ocrAppUtils
    EXTRA_LINK_LIBS m
)

add_benchmark_app(NAME miniAMR_intel_chandra
    DIR miniAMR/refactored/ocr/intel-chandra
    SOURCES main.c init.c check_sum.c refine.c move.c block.c comm_refine.c
    comm_parent.c driver.c comm.c comm_utils.c stencil.c util.c
    load_balance.c profile.c
    LIBS reduction ocrAppUtils timer
    # OCR_APP_COUNTED_OEVT: every create that asks for an output event supplies
    # its own COUNTED event, so the runtime mints and retains nothing extra.
    # CHANNEL_EVENTS_AT_RECEIVER: the port's published choice (Makefile.x86-base)
    # of which side creates, and so homes, each halo channel event.
    # VERIFICATION_RUN is gone: it reached exactly one site, the cell fill, which
    # is now an unconditional index-seeded generator — deterministic across runs,
    # runtimes and node counts, and unlike the constant field that macro produced,
    # not a fixed point of the stencil.
    EXTRA_DEFINES OCR_APP_COUNTED_OEVT CHANNEL_EVENTS_AT_RECEIVER
    EXTRA_LINK_LIBS m
    HINTED_PLACEMENT
)

# ==========================================================================
# Tier 11: Nekbone, NPB-CG
# ==========================================================================

add_benchmark_app(NAME nekbone
    HINTED_PLACEMENT
    DIR nekbone/refactored/ocr_src
    SOURCES fastfcn.c neko_reduction.c timing.c nekbone_cg.c ax.c local_grad.c
    set_f.c blas1.c blas3.c polybasis.c set_vert_box.c cubic.c
    init_mesh.c nekbone_setup.c app_ocr_util.c integer_log2.c
    neko_globals.c nekos_triplet.c nekos_cubiclattice.c nekos_tools.c
    neko_halo_util.c neko_halo.c spmd_global_data.c tailrecursion.c
    treeForkJoin.c z_nekbone_inOcr.c
    LIBS reduction
    EXTRA_LINK_LIBS m
)

# --- Restructured: nekbone_dist ---
# Same sources; what it redesigns is the reduction's own decomposition.  The
# tree is a flat ARITY tree over participant indices and the program hands it
# the geometric rank id, so its edges are unrelated to where a rank runs --
# with the halo made local by placement, that tree is what stops scaling.
# This build numbers the participants place-major AND makes the tree two-level
# around that numbering: a place reduces among its own participants, and only
# the places' roots reduce with each other.  Renumbering alone is worse than
# doing nothing -- a flat tree's internal nodes are its low indices, and
# place-major numbering puts every one of them on the first place.
add_benchmark_app(NAME nekbone_dist
    DIR nekbone/refactored/ocr_src
    EXTRA_DEFINES OCR_APP_OPTIMIZED_PLACEMENT
    # The restructured tier's own sources replace the shared ones file by file:
    # ../ocr_dist/neko_reduction.c in place of neko_reduction.c, and the
    # reductionTwoLevel library in place of reduction.
    SOURCES fastfcn.c ../ocr_dist/neko_reduction.c timing.c nekbone_cg.c ax.c local_grad.c
    set_f.c blas1.c blas3.c polybasis.c set_vert_box.c cubic.c
    init_mesh.c nekbone_setup.c app_ocr_util.c integer_log2.c
    neko_globals.c nekos_triplet.c nekos_cubiclattice.c nekos_tools.c
    neko_halo_util.c neko_halo.c spmd_global_data.c tailrecursion.c
    treeForkJoin.c z_nekbone_inOcr.c
    LIBS reductionTwoLevel
    EXTRA_LINK_LIBS m
)

add_benchmark_app(NAME npb_cg
    DIR npb-cg/sdsc-ocr
    SOURCES cg_edt.c cg_graph.c cg_ocr.c main_edt.c makea_ocr.c util_ocr.c
    EXTRA_LINK_LIBS m
)

# Restructured decomposition: a separate program (row-band SPMD chains,
# channel-event fragment exchange, library reductions) sharing the matrix
# generator and utilities; its placement is internal, so no _hinted family.
add_benchmark_app(NAME npb_cg_dist
    DIR npb-cg/sdsc-ocr
    SOURCES cg_dist_edt.c cg_dist_makea.c makea_ocr.c util_ocr.c
    LIBS reduction
    EXTRA_LINK_LIBS m
)

# ==========================================================================
# Tier 12: RSBench, XSBench
# ==========================================================================

add_benchmark_app(NAME RSBench_intel
    HINTED_PLACEMENT
    DIR RSBench/refactored/ocr/intel
    SRC_SUBDIR src
    SOURCES main.c init.c material.c io.c timers.c utils.c rs_kernel.c
    LIBS timer
    EXTRA_LINK_LIBS m
)

# --- Restructured: RSBench_intel_dist ---
# The published per-lookup chain kept intact over a lookup range cut into
# places, each with its own producer and its own shard of the dataset; its
# placement is internal, so no _hinted family.
add_benchmark_app(NAME RSBench_intel_dist
    DIR RSBench/refactored/ocr/intel-dist
    SRC_SUBDIR src
    SOURCES main.c init.c material.c io.c timers.c utils.c rs_kernel.c
    LIBS timer
    EXTRA_LINK_LIBS m
)

# The hint layer lives inside main.c behind OCR_APP_OPTIMIZED_PLACEMENT: a lane
# is placed by its index among ALL p*t lanes rather than by its index within its
# own instance, and the per-lane datablocks take the hint the program already
# computes.  No new source, include or define.
add_benchmark_app(NAME RSBench_intel_sharedDB
    HINTED_PLACEMENT
    DIR RSBench/refactored/ocr/intel-sharedDB
    SRC_SUBDIR src
    SOURCES main.c init.c material.c io.c utils.c rs_kernel.c timers.c
    LIBS timer reduction ocrAppUtils
    EXTRA_LINK_LIBS m
)

add_benchmark_app(NAME XSBench_intel
    HINTED_PLACEMENT
    DIR XSBench/refactored/ocr/intel
    SRC_SUBDIR src
    SOURCES Main.c io.c CalculateXS.c XSutils.c Materials.c qsort.c timers.c
    LIBS timer
    EXTRA_LINK_LIBS m
)

# --- Restructured: XSBench_intel_dist ---
# A separate program that cuts the lookup range into places, each with its own
# producer and its own replica of the physics table, and keeps the per-lookup
# task chain; its placement is internal, so no _hinted family.  qsort.c is not
# in SOURCES: that file is entirely `#ifdef TG_ARCH` and the program uses the
# libc qsort.
add_benchmark_app(NAME XSBench_intel_dist
    DIR XSBench/refactored/ocr/intel-dist
    SRC_SUBDIR src
    SOURCES Main.c io.c CalculateXS.c XSutils.c Materials.c timers.c
    LIBS timer
    EXTRA_LINK_LIBS m
)

add_benchmark_app(NAME XSBench_intel_sharedDB
    DIR XSBench/refactored/ocr/intel-sharedDB
    SRC_SUBDIR src
    SOURCES Main.c io.c GridInit.c CalculateXS.c XSutils.c Materials.c timers.c
    LIBS timer reduction ocrAppUtils
    EXTRA_LINK_LIBS m
)

# ==========================================================================
# Tier 13: LCS variants
# ==========================================================================

add_benchmark_app(NAME LCS_distributed_ST
    DIR LCS/refactored/ocr/intel-jesmin-lcs_distributed_ST_datablocks
    SOURCES lcs_ST_distributed.c
    LIBS reduction timer
    EXTRA_LINK_LIBS m
    HINTED_PLACEMENT
)
# LCS uses bare "inline" (C11 = no external def) → fails at -O0; force GNU89 semantics
foreach(flavor LCS_distributed_ST LCS_distributed_ST_hinted)
    if(TARGET ${flavor}_xsocr)
        target_compile_options(${flavor}_xsocr PRIVATE -fgnu89-inline)
    endif()
    foreach(sfx ${ARTS_VARIANT_SUFFIXES})
        if(TARGET ${flavor}_arts_${sfx})
            target_compile_options(${flavor}_arts_${sfx} PRIVATE -fgnu89-inline)
        endif()
    endforeach()
    if(TARGET ${flavor}_ocrvx)
        target_compile_options(${flavor}_ocrvx PRIVATE -fgnu89-inline)
    endif()
endforeach()

add_benchmark_app(NAME LCS_shared
    DIR LCS/refactored/ocr/intel-jesmin-lcs_shared_datablocks
    SOURCES lcs.c
    LIBS reduction timer
    EXTRA_LINK_LIBS m
    HINTED_PLACEMENT
)
foreach(flavor LCS_shared LCS_shared_hinted)
    if(TARGET ${flavor}_xsocr)
        target_compile_options(${flavor}_xsocr PRIVATE -fgnu89-inline)
    endif()
    foreach(sfx ${ARTS_VARIANT_SUFFIXES})
        if(TARGET ${flavor}_arts_${sfx})
            target_compile_options(${flavor}_arts_${sfx} PRIVATE -fgnu89-inline)
        endif()
    endforeach()
    if(TARGET ${flavor}_ocrvx)
        target_compile_options(${flavor}_ocrvx PRIVATE -fgnu89-inline)
    endif()
endforeach()

# LCS C++ variant: standard OCR API written in C++.  mainEdt is declared
# extern "C" and ocr.h is extern "C"-guarded, so CMake compiles the .cpp with
# the C++ toolchain (project enables CXX17) and links the C OCR libraries.
# cilktime.h here is a plain gettimeofday timer, not the Cilk runtime.
add_benchmark_app(NAME LCS_all_db_distributed
    DIR LCS/refactored/ocr/intel-jesmin-lcs_all_db_distributed
    SOURCES lcs_distributed.cpp
    EXTRA_LINK_LIBS m
    HINTED_PLACEMENT
)

# Wavefront restructuring of the same computation: boundary strips travel on
# labeled counted events, leaves spawn per row and run in anti-diagonal
# order, memory follows the frontier.
add_benchmark_app(NAME LCS_wavefront
    DIR LCS/refactored/ocr/intel-jesmin-lcs_all_db_distributed
    SOURCES lcs_wavefront.c
    LIBS timer
    EXTRA_LINK_LIBS m
)

# ==========================================================================
# Tier 14: SAR (problem_size_scaling, over sources shared from sar/ocr/src/)
# ==========================================================================
#
#   1. crlibm: extract tarball → configure → RAG patches → compile
#   2. Link the SAR executable against crlibm
#
# The one variant reads its data files and its parameter file at runtime, so
# the problem size is an argument rather than a build.

set(SAR_ROOT "${OCR_APPS_ROOT}/sar")
set(SAR_SRC_DIR "${SAR_ROOT}/ocr/src")
set(CRLIBM_PKG_DIR "${OCR_LIBS_ROOT}/crlibm-1.0beta5")
set(CRLIBM_TARBALL "${CRLIBM_PKG_DIR}/tar/crlibm-1.0beta5.tgz")
set(CRLIBM_DIR "${CMAKE_CURRENT_BINARY_DIR}/crlibm")

# ---------------------------------------------------------------------------
# Step 1: Extract and configure crlibm at configure time
# ---------------------------------------------------------------------------
# The tarball is in the submodule; x86_64/ only has RAG-install.sh tracked.
# We extract into the build directory so the source tree stays clean.
if(EXISTS "${CRLIBM_TARBALL}")
    if(NOT EXISTS "${CRLIBM_DIR}/crlibm.h")
        message(STATUS "SAR: Extracting crlibm tarball...")
        file(MAKE_DIRECTORY "${CRLIBM_DIR}")
        execute_process(
            COMMAND tar --gzip --extract --strip-components=1
            --file=${CRLIBM_TARBALL}
            WORKING_DIRECTORY "${CRLIBM_DIR}"
            RESULT_VARIABLE _tar_result
        )
        if(NOT _tar_result EQUAL 0)
            message(FATAL_ERROR "SAR: Failed to extract crlibm tarball")
        endif()

        # Run autotools configure (generates crlibm_config.h)
        message(STATUS "SAR: Configuring crlibm...")
        execute_process(
            COMMAND ./configure
            --without-gmp --without-mpfr --without-ultim --without-mcr
            --disable-gmp --disable-mpfr --disable-ultim --disable-mcr
            --enable-sse2 --disable-dependency-tracking
            --disable-double-extended --prefix=/dev/null
            WORKING_DIRECTORY "${CRLIBM_DIR}"
            RESULT_VARIABLE _cfg_result
        )
        if(NOT _cfg_result EQUAL 0)
            message(FATAL_ERROR "SAR: Failed to configure crlibm")
        endif()

        # Apply RAG patches to crlibm_config.h (disable system headers for
        # portability — matches x86_64/RAG-install.sh)
        message(STATUS "SAR: Applying RAG patches to crlibm_config.h...")
        set(_config_h "${CRLIBM_DIR}/crlibm_config.h")
        file(READ "${_config_h}" _cfg_content)
        string(REPLACE "#define CRLIBM_HAS_FPU_CONTROL 1" "/* #undef CRLIBM_HAS_FPU_CONTROL -- RAG */" _cfg_content "${_cfg_content}")
        string(REPLACE "#define HAS_SSE2 1" "/* #undef HAS_SSE2 -- RAG */" _cfg_content "${_cfg_content}")
        string(REPLACE "#define HAVE_FENV_H 1" "/* #undef HAVE_FENV_H -- RAG */" _cfg_content "${_cfg_content}")
        string(REPLACE "#define HAVE_FLOAT_H 1" "/* #undef HAVE_FLOAT_H -- RAG */" _cfg_content "${_cfg_content}")
        string(REPLACE "#define HAVE_LIBM 1" "/* #undef HAVE_LIBM -- RAG */" _cfg_content "${_cfg_content}")
        string(REPLACE "#define HAVE_LONG_DOUBLE 1" "/* #undef HAVE_LONG_DOUBLE -- RAG */" _cfg_content "${_cfg_content}")
        string(REPLACE "#define HAVE_LONG_DOUBLE_WIDER 1" "/* #undef HAVE_LONG_DOUBLE_WIDER -- RAG */" _cfg_content "${_cfg_content}")
        string(REPLACE "#define HAVE_MEMORY_H 1" "/* #undef HAVE_MEMORY_H -- RAG */" _cfg_content "${_cfg_content}")
        string(REPLACE "#define HAVE_STRINGS_H 1" "/* #undef HAVE_STRINGS_H -- RAG */" _cfg_content "${_cfg_content}")
        string(REPLACE "#define HAVE_STRING_H 1" "/* #undef HAVE_STRING_H -- RAG */" _cfg_content "${_cfg_content}")
        string(REPLACE "#define HAVE_SYS_STAT_H 1" "/* #undef HAVE_SYS_STAT_H -- RAG */" _cfg_content "${_cfg_content}")
        string(REPLACE "#define HAVE_SYS_TYPES_H 1" "/* #undef HAVE_SYS_TYPES_H -- RAG */" _cfg_content "${_cfg_content}")
        string(REPLACE "#define HAVE_UNISTD_H 1" "/* #undef HAVE_UNISTD_H -- RAG */" _cfg_content "${_cfg_content}")
        file(WRITE "${_config_h}" "${_cfg_content}")

        # Patch crlibm_private.c: guard FPU control with CRLIBM_HAS_FPU_CONTROL
        set(_priv_c "${CRLIBM_DIR}/crlibm_private.c")
        file(READ "${_priv_c}" _priv_content)
        string(REPLACE
            "#if (defined(CRLIBM_TYPECPU_X86) || defined(CRLIBM_TYPECPU_AMD64))"
            "#if defined(CRLIBM_HAS_FPU_CONTROL) && (defined(CRLIBM_TYPECPU_X86) || defined(CRLIBM_TYPECPU_AMD64)) // RAG -- added CRLIBM_HAS_FPU_CONTROL"
            _priv_content "${_priv_content}")
        file(WRITE "${_priv_c}" "${_priv_content}")

        # Patch asincos.c: silence unused variable warning
        set(_asin_c "${CRLIBM_DIR}/asincos.c")
        file(READ "${_asin_c}" _asin_content)
        string(REPLACE
            "  double deltatemp, deltah, deltal;"
            "  double deltatemp,deltah, __attribute__ ((unused)) deltal; // RAG -- avoid fatal warning"
            _asin_content "${_asin_content}")
        file(WRITE "${_asin_c}" "${_asin_content}")

        message(STATUS "SAR: crlibm extraction and patching complete")
    endif()
endif()

# ---------------------------------------------------------------------------
# Step 1b: Build crlibm static library
# ---------------------------------------------------------------------------
if(EXISTS "${CRLIBM_DIR}/crlibm.h")
    # Explicit source list — file(GLOB) picks up Itanium/double-extended files
    set(CRLIBM_SRCS
        ${CRLIBM_DIR}/crlibm_private.c
        ${CRLIBM_DIR}/triple-double.c
        ${CRLIBM_DIR}/exp-td.c
        ${CRLIBM_DIR}/exp-td-standalone.c
        ${CRLIBM_DIR}/expm1-standalone.c
        ${CRLIBM_DIR}/expm1.c
        ${CRLIBM_DIR}/log.c
        ${CRLIBM_DIR}/log1p.c
        ${CRLIBM_DIR}/rem_pio2_accurate.c
        ${CRLIBM_DIR}/trigo_fast.c
        ${CRLIBM_DIR}/trigo_accurate.c
        ${CRLIBM_DIR}/trigpi.c
        ${CRLIBM_DIR}/asincos.c
        ${CRLIBM_DIR}/pow.c
        ${CRLIBM_DIR}/atan_fast.c
        ${CRLIBM_DIR}/atan_accurate.c
        ${CRLIBM_DIR}/csh_fast.c
    )
    add_library(ocr_lib_crlibm STATIC ${CRLIBM_SRCS})
    target_include_directories(ocr_lib_crlibm PUBLIC ${CRLIBM_DIR})
    set_target_properties(ocr_lib_crlibm PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    target_compile_definitions(ocr_lib_crlibm PRIVATE HAVE_CONFIG_H)
    target_compile_options(ocr_lib_crlibm PRIVATE ${BENCH_C_OPT})
    # crlibm needs scs_lib
    set(SCS_DIR "${CRLIBM_DIR}/scs_lib")
    if(EXISTS "${SCS_DIR}")
        file(GLOB SCS_SRCS "${SCS_DIR}/*.c")
        target_sources(ocr_lib_crlibm PRIVATE ${SCS_SRCS})
        target_include_directories(ocr_lib_crlibm PRIVATE ${SCS_DIR})
    endif()
    set(HAS_CRLIBM TRUE)
else()
    message(WARNING "SAR: crlibm tarball not found at ${CRLIBM_TARBALL} — "
        "SAR benchmarks will be skipped")
    set(HAS_CRLIBM FALSE)
endif()

# ---------------------------------------------------------------------------
# The RAG_IMPLICIT_INPUTS sizes are gone (2026-08-24).  tiny/small/medium/large
# compiled their datasets into the binary through a datagen -> dataImage ->
# .incbin pipeline, which is why they took no arguments: each was one rung of
# the problem_size_scaling ladder frozen at build time.  The ladder itself is
# Parameter files differing only in the image grid (Ix = 400*(n+1)), so the pss
# target with a parameter file covers every one of them and the four binaries,
# their host utilities and their generated data objects buy nothing.  The same
# rule extends the ladder: Parameter45 is 18400 a side (the restructured
# row's rung), Parameter10 is 4400, Parameter14 is 6000 and Parameter15 is
# 6400 (the 32-rank rung of the restructured row at its default blocks).
# (The submodule holds Parameter0..9, 10, 14, 15 and 45; there is no
# Parameter41.)
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Step 4: Build SAR executables
# ---------------------------------------------------------------------------
set(SAR_SOURCES
    main.c inputs.c digspot.c registration.c cfar.c sinc_interp.c sinc.c
    image_formation.c back_proj.c ccd.c fftw3.c rag_ocr.c
)

# DEBUG_SSCP (per-variant upstream Makefile default for every size but
# problem_size_scaling) is left out for ALL sizes: it dumps the full
# image/correlation planes to cwd-relative .bin files (hundreds of MB per
# run) and routes shutdown through the dump EDT — debug-only output that has
# no place in measured runs.  Define it manually when the dumps are needed.
set(SAR_DEFINES
    RAG_TG_ARCH_NULL_GUID_WORKAROUND
    RAG_NEW_BLK_SIZE=32
    TRACE
    RAG_DIG_SPOT_ON=0
    RAG_AFFINE_ON=1
    RAG_QSORT_ON=0
    RAG_PURE_FLOAT
    RAG_SINCOS
    RAG_CRLIBM
    RAG_PETER_DIST_AND_TRIG
    RAG_SPAD
    RAG_DRAM
    GANESH_STRENGTH_RED_OPT
)

if(HAS_CRLIBM)
    set(SAR_DIST_DEFINES ${SAR_DEFINES})
    list(REMOVE_ITEM SAR_DIST_DEFINES RAG_NEW_BLK_SIZE=32)
    list(APPEND SAR_DIST_DEFINES RAG_NEW_BLK_SIZE=50)
    # The detection pass computes the correlation values its own windows need
    # instead of reading them from a whole-image map, so its blocking factor
    # is its own: the halo it recomputes is (blk+Ncfar-1)^2/blk^2.
    # This is the DEFAULT and the MAXIMUM of the run-time knob (argv[7]): the
    # detection task's window buffer is automatic storage sized from it, so a
    # larger value is refused at start-up.  Raise it here to widen the range.
    list(APPEND SAR_DIST_DEFINES RAG_CFAR_BLK_SIZE=100)

    # The resample interpolates one pixel per iteration, so a projection-sized
    # block holds microseconds of work; this phase blocks coarser.  Default and
    # maximum of argv[8], for the same reason: the block's source window is
    # automatic storage sized from it.
    list(APPEND SAR_DIST_DEFINES RAG_AFFINE_BLK_SIZE=250)

    # --- Restructured: sar_dist ---
    # It owns only the files whose decomposition it changes; the rest of the
    # pipeline is the shared source, compiled once more.  ccd.c is absent
    # because the correlation pass is fused into the detection pass.
    set(SAR_DIST_SOURCES
        main.c cfar.c back_proj.c registration.c image_formation.c blocks.c
        ../src/inputs.c ../src/digspot.c ../src/sinc_interp.c ../src/sinc.c
        ../src/fftw3.c ../src/rag_ocr.c
    )
    add_benchmark_app(NAME sar_dist
        DIR sar/ocr/dist
        SRC_SUBDIR .
        SOURCES ${SAR_DIST_SOURCES}
        EXTRA_INCLUDES ${SAR_ROOT}/ocr/problem_size_scaling ${SAR_SRC_DIR} ${CRLIBM_DIR}
        EXTRA_DEFINES ${SAR_DIST_DEFINES}
        EXTRA_LINK_LIBS m ocr_lib_crlibm
    )

    # --- Base: the one runtime-input variant ---
    # sar/ocr/huge carries byte-identical sources (main.c, common.h, argv.h) and
    # differs solely in the Parameters.txt it ships, whose grid is this ladder's
    # tenth rung, so it is not a target of its own either.
    # This variant's own Makefile.x86 sets the blocking factor to 50 (the
    # shared list carries the other variants' 32).
    set(SAR_RT_DEFINES ${SAR_DEFINES})
    list(REMOVE_ITEM SAR_RT_DEFINES RAG_NEW_BLK_SIZE=32)
    list(APPEND SAR_RT_DEFINES RAG_NEW_BLK_SIZE=50)
    # No HINTED_PLACEMENT flavour: there is no OCR_APP_OPTIMIZED_PLACEMENT
    # guard in the SAR sources.  Measured, a placement layer for this program
    # either collapses the pipeline onto one rank (97% of the work on one of
    # four) or, written honestly, runs 23-36% SLOWER than no hints at all --
    # the serialisation is on a shared whole image block, which affinity
    # cannot address.  The one affinity hint the base program does carry pins
    # the input read's slabs to a single node, which is a correctness
    # precondition of writing one datablock from many tasks, not a
    # performance layer.
    add_benchmark_app(NAME sar_problem_size_scaling
        DIR sar/ocr/problem_size_scaling
        SRC_SUBDIR ../src
        SOURCES ${SAR_SOURCES}
        EXTRA_INCLUDES ${SAR_SRC_DIR} ${CRLIBM_DIR}
        EXTRA_DEFINES ${SAR_RT_DEFINES}
        EXTRA_LINK_LIBS m ocr_lib_crlibm
    )
endif()


# ==========================================================================
# Tier 15: STREAM memory bandwidth, UTS tree search
# ==========================================================================

add_benchmark_app(NAME stream
    DIR stream/ocr
    SOURCES stream.c
    # Argument-free defaults only; every campaign run passes all three.
    # NTIMES is #ifndef-guarded in stream.c now (it was an unconditional
    # #define), so the define reaches it as it already does in stream_dist.c.
    EXTRA_DEFINES STREAM_ARRAY_SIZE=1000 NUM_THREADS=2 NTIMES=3
    # The source carries a placement layer; without this it was never built.
    HINTED_PLACEMENT
    EXTRA_LINK_LIBS m
)

add_benchmark_app(NAME stream_dist
    DIR stream/ocr
    SOURCES stream_dist.c
    EXTRA_DEFINES STREAM_ARRAY_SIZE=1000 NUM_THREADS=2 NTIMES=3
    EXTRA_LINK_LIBS m
)



add_benchmark_app(NAME uts
    DIR uts-1.1/ocr
    SOURCES uts.c brg_sha1.c
    EXTRA_DEFINES BRG_RNG STATS
    EXTRA_LINK_LIBS m
)

# ==========================================================================
# release_sync_stress  (bespoke: source lives in this repo, not in ocr-apps)
# Probes whether ocrDbRelease is a synchronizing operation across runtimes.
# ==========================================================================
add_bespoke_app(NAME release_sync_stress SOURCES release_sync_stress.c)

# ==========================================================================
# dbcreate_matrix  (bespoke: source lives in this repo, not in ocr-apps)
# Probes the DB-create capability matrix -- (creator-acquisition) x (home-
# determination: affinity / labeled / no-hint) x (locality) -- through the
# portable OCR API, so the SAME source builds for every runtime and the
# correctness harness can compare their create-path capability.  Prints
# CELLS_OK=<k>/12.
# ==========================================================================
add_bespoke_app(NAME dbcreate_matrix SOURCES dbcreate_matrix.c)

# ==========================================================================
# Meta targets
# ==========================================================================

# get_property(ALL_XSOCR GLOBAL PROPERTY BENCHMARK_XSOCR_TARGETS)
get_property(ALL_ARTS GLOBAL PROPERTY BENCHMARK_ARTS_TARGETS)

# add_custom_target(benchmark_xsocr DEPENDS ${ALL_XSOCR})
add_custom_target(benchmark_arts DEPENDS ${ALL_ARTS})
add_custom_target(benchmark_apps DEPENDS benchmark_arts)

# ==========================================================================
# The adversarial suite: one program per attack over two shared engines
# (attack/chain.c: closed-loop chains; attack/handoff.c: event-ordered
# lattices).  Populations follow the rank count inside each program.
# The apps root is on the include path so a program names its engine by the
# directory it lives in ("attack/chain.h").
# The suite compares the plane's own coherence arms against each other, so
# every program here builds for the ARTS variants only.
# ==========================================================================
add_bespoke_app(NAME own_reread DIR attack SOURCES own_reread.c chain.c
    EXTRA_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" EXTRA_LINK_LIBS m ARTS_ONLY)
add_bespoke_app(NAME own_rewrite DIR attack SOURCES own_rewrite.c chain.c
    EXTRA_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" EXTRA_LINK_LIBS m ARTS_ONLY)
add_bespoke_app(NAME read_funnel DIR attack SOURCES read_funnel.c chain.c
    EXTRA_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" EXTRA_LINK_LIBS m ARTS_ONLY)
add_bespoke_app(NAME audienceless_publish DIR attack SOURCES audienceless_publish.c chain.c
    EXTRA_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" EXTRA_LINK_LIBS m ARTS_ONLY)
add_bespoke_app(NAME freerun_mix DIR attack SOURCES freerun_mix.c chain.c
    EXTRA_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" EXTRA_LINK_LIBS m ARTS_ONLY)
add_bespoke_app(NAME pipeline_converge DIR attack SOURCES pipeline_converge.c chain.c
    EXTRA_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" EXTRA_LINK_LIBS m ARTS_ONLY)
add_bespoke_app(NAME alternating_bomb DIR attack SOURCES alternating_bomb.c chain.c
    EXTRA_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" EXTRA_LINK_LIBS m ARTS_ONLY)
add_bespoke_app(NAME resident_mill DIR attack SOURCES resident_mill.c chain.c
    EXTRA_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" EXTRA_LINK_LIBS m ARTS_ONLY)
add_bespoke_app(NAME migration_gauntlet DIR attack SOURCES migration_gauntlet.c chain.c
    EXTRA_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" EXTRA_LINK_LIBS m ARTS_ONLY)
add_bespoke_app(NAME handoff_lattice DIR attack SOURCES handoff_lattice.c handoff.c
    EXTRA_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" ARTS_ONLY)
add_bespoke_app(NAME serial_rounds DIR attack SOURCES serial_rounds.c ARTS_ONLY)

# ==========================================================================
# mapreduce_wordcount  (bespoke: source lives in this repo, not in ocr-apps)
# Canonical MapReduce word count in native OCR, weak-scaling form: the
# corpus is synthesized per map tile from its global id (closed
# vocabulary, no ingest I/O), reduced over a K-ary spawner-built tree
# whose lower levels stay node-local.  Portable OCR API only, so all
# three runtimes build it.
# ==========================================================================
add_bespoke_app(NAME mapreduce_wordcount SOURCES mapreduce_wordcount.c EXTRA_LINK_LIBS m)

# HPX-origin mirrors (sources under hpx_origin/)
add_bespoke_app(NAME fib_hpx DIR hpx_origin SOURCES fib_hpx.c
    EXTRA_LINK_LIBS arts_fib_kernel)
add_bespoke_app(NAME stencil1d_hpx DIR hpx_origin SOURCES stencil1d_hpx.c)
add_bespoke_app(NAME network_storage_hpx DIR hpx_origin SOURCES network_storage_hpx.c)
# Retained library pointers use acquired object identities across tasks.
# The vendored library is built only by a tree that configured it, so the row
# is conditioned on the target existing rather than on the options that
# happen to create it: a tree without it still generates, and every other
# row still builds every one of its variants.  The mirror links the
# relocation library, a pinned translation of the same FFTW source that the
# tree's own compiler assembles (hpx_origin/fftw_reloc/), so the row asks
# nothing more of the host than the vendored FFTW does.
if(TARGET arts::fftw3)
    add_subdirectory(hpx_origin/fftw_reloc)
    add_bespoke_app(NAME fft_hpx DIR hpx_origin SOURCES fft_hpx.c
        EXTRA_LINK_LIBS arts::fftw_reloc m)
else()
    message(STATUS "Skipping fft_hpx: the vendored FFTW is not configured")
endif()
