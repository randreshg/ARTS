# Benchmarks: OCR apps running on XSOCR (reference) and ARTS (via shim)
#
# Directory structure:
#   xsocr/     - Native XSOCR library (reference OCR x86 implementation)
#   ocr_shim/  - ARTS-backed OCR compatibility shim
#   apps/      - OCR benchmark apps built against both backends
#   hpx/       - Standalone HPX reference apps

# Benchmark optimization flags — controlled by CMAKE_BUILD_TYPE
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(BENCH_C_OPT -O0 -g)
else()
    set(BENCH_C_OPT -O3)
endif()

# Suppress the "#pragma message: OCR_TYPE_H not defined" note from ocr.h.
# Defining OCR_TYPE_H causes ocr.h to #include "ocr-options_<value>.h",
# so we generate a no-op header in the build directory.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ocr-options_arts.h"
     "/* No-op OCR options for ARTS benchmark builds. */\n")
set(BENCH_OCR_TYPE_DEFINE "OCR_TYPE_H=arts.h")
set(BENCH_OCR_OPTIONS_DIR "${CMAKE_CURRENT_BINARY_DIR}")

if(ARTS_BUILD_XSOCR)
    add_subdirectory(xsocr)
endif()
if(ARTS_BUILD_OCRVX)
    add_subdirectory(ocrvx)
endif()
if(ARTS_BUILD_HPX AND ARTS_HPX_AVAILABLE)
    include(ExternalProject)

    option(ARTS_HPX_SMOKE "Register the two-locality static-linking smoke as the HPX app project's test step (needs a working mpiexec on the build host)" ON)

    set(ARTS_HPX_APPS_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/hpx-build")
    set(ARTS_HPX_APPS_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/hpx")
    include("${CMAKE_CURRENT_SOURCE_DIR}/hpx/apps.cmake")
    set(_hpx_byproducts "")
    foreach(_app IN LISTS ARTS_HPX_APPS)
        list(APPEND _hpx_byproducts "${ARTS_HPX_APPS_OUTPUT_DIR}/${_app}_hpx")
    endforeach()
    ExternalProject_Add(hpx_apps_ep
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/hpx"
        BINARY_DIR "${ARTS_HPX_APPS_BUILD_DIR}"
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
        CMAKE_GENERATOR "${CMAKE_GENERATOR}"
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            # Same MPI wrapper the HPX build compiled with: the static HPX
            # package expects MPI from the toolchain, not from a resolved
            # library list.
            -DCMAKE_CXX_COMPILER=${MPI_CXX_COMPILER}
            # The same 128-bit-atomic flags the app project sets on each of
            # its targets: passing them here as well keeps the two projects'
            # translation units identical whichever way this one is
            # configured, and neither project depends on the other for them.
            # The native-ISA pair is the outer tree's own Release setting,
            # repeated because an external project does not inherit it: the
            # programs compared against the OCR mirrors must target the same
            # instruction set the mirrors do.
            "-DCMAKE_CXX_FLAGS=-mcx16 -march=native -mtune=native -DHPX_HAVE_CXX11_STD_ATOMIC_128BIT"
            -DCMAKE_PREFIX_PATH=${ARTS_HPX_INSTALL_DIR}
            # The consumer package re-finds hwloc, and a find through
            # NAMES/paths can land on a host copy; the exact archive the
            # HPX build embedded is the only correct answer.
            -DHwloc_INCLUDE_DIR=${HPX_HWLOC_INSTALL_DIR}/include
            -DHwloc_LIBRARY=${HPX_HWLOC_INSTALL_DIR}/lib/libhwloc.a
            -DHPX_APPS_RUNTIME_OUTPUT_DIRECTORY=${ARTS_HPX_APPS_OUTPUT_DIR}
            -DARTS_HPX_ORIGIN_ROOT=${CMAKE_SOURCE_DIR}/third_party/hpx
            # The vendored dependencies and the pristine origin trees the
            # HPX-origin programs are built from and diffed against, handed
            # in as paths: the app project configures standalone against an
            # install, so it takes prefixes from outside, never targets.
            -DARTS_HDF5_DIR=${ARTS_HDF5_INSTALL_DIR}
            -DARTS_FFTW_DIR=${ARTS_FFTW_INSTALL_DIR}
            -DARTS_MINIAPPS_ROOT=${CMAKE_SOURCE_DIR}/third_party/miniapps
            -DARTS_NBODY_ROOT=${CMAKE_SOURCE_DIR}/third_party/NBody
            -DARTS_HPXFFT_ROOT=${CMAKE_SOURCE_DIR}/third_party/hpx-fft
            -DARTS_MPIEXEC=${MPIEXEC_EXECUTABLE}
            -DARTS_HPX_SMOKE=${ARTS_HPX_SMOKE}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR>
            --parallel ${CMAKE_BUILD_PARALLEL_LEVEL}
        INSTALL_COMMAND ""
        # Running the app project's own tests as the test step keeps a
        # program whose difference from its origin is no longer the committed
        # patch, or a static link that cannot start two ranks, from reaching
        # a benchmark binary unnoticed.
        TEST_COMMAND ctest --test-dir "${ARTS_HPX_APPS_BUILD_DIR}" --output-on-failure
        BUILD_ALWAYS TRUE
        BUILD_BYPRODUCTS ${_hpx_byproducts}
        DEPENDS hpx_ep hdf5_ep fftw_ep
        LOG_CONFIGURE 1
        LOG_BUILD 1
    )
    foreach(_app IN LISTS ARTS_HPX_APPS)
        add_custom_target(${_app}_hpx DEPENDS hpx_apps_ep)
    endforeach()
endif()
add_subdirectory(ocr_shim)
add_subdirectory(apps)
if(ARTS_BUILD_BASELINES)
    add_subdirectory(baseline)
endif()
