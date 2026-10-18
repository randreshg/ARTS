# The OCR-API programs of the benchmark tree, on ARTS (every coherence variant,
# through the shim) and on the reference OCR runtimes where they are built.
# This file holds the factories; each section registers its programs in a file
# of its own, included here so that every program's binary lands in this
# directory:
#   ocr_apps.cmake     the OCR applications (ARTS_BUILD_OCR_APPS)
#   hpx_mirrors.cmake  the HPX applications' OCR mirrors (ARTS_BUILD_HPX_APPS)


# Every coherence configuration the app factory builds an executable for: the
# variants the runtime registered (arts_variant), so no copy of the list can
# omit one.
get_property(ARTS_VARIANT_SUFFIXES GLOBAL PROPERTY ARTS_VARIANTS)
if(NOT ARTS_VARIANT_SUFFIXES)
    message(FATAL_ERROR "no ARTS variant is registered for the benchmark apps")
endif()

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
# add_benchmark_app: builds an OCR app against ARTS and the reference OCR runtimes
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
            target_link_options(${_app}_arts_${sfx} PRIVATE ${ARTS_NON_PIE_LINK_OPTIONS})
            set_target_properties(${_app}_arts_${sfx} PROPERTIES
                C_STANDARD 11 C_EXTENSIONS ON
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
            target_compile_options(${_app}_arts_${sfx} PRIVATE ${BENCH_C_OPT})
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

if(ARTS_BUILD_OCR_APPS)
    include(${CMAKE_CURRENT_SOURCE_DIR}/ocr_apps.cmake)
endif()
if(ARTS_BUILD_HPX_APPS)
    include(${CMAKE_CURRENT_SOURCE_DIR}/hpx_mirrors.cmake)
endif()
