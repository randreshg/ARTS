cmake_minimum_required(VERSION 3.18)

project(arts_hpx_apps VERSION 1.0 LANGUAGES CXX)

find_package(HPX 1.11.0 EXACT CONFIG REQUIRED)
enable_testing()

# The 16-byte operations the 128-bit queue backends compile to are resolved
# from libatomic, and a static link resolves a library only against what
# precedes it — so the entry has to follow every HPX archive, which is what
# the standard-libraries list is.
set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} -latomic")

# Settings every consumer of this HPX install needs, whatever it is, and
# every one of them stated here: this project is configurable on its own
# against the install, and inherits nothing from the tree that built it.
# The macro selects which thread-queue backends the headers declare, and
# the installed defines.hpp cannot carry it (see third_party/CMakeLists.txt),
# so a consumer that omits it declares different backends than the archives
# hold.
function(arts_hpx_configure target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_options(${target} PRIVATE -mcx16)
    target_compile_definitions(${target} PRIVATE HPX_HAVE_CXX11_STD_ATOMIC_128BIT)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${target} PRIVATE HPX::hpx)
    # The embedded hwloc must stay out of the dynamic symbol table: an
    # MPI library in the same process loads the host's hwloc for
    # itself, and an exported copy would preempt it.
    target_link_options(${target} PRIVATE "LINKER:--exclude-libs,libhwloc.a")
endfunction()

# The vendored HDF5 and FFTW, handed in by the outer project as install
# prefixes.  A program that needs one names it in add_hpx_origin_app's LINK
# list; a tree configured without them has no such target, and the program
# that needs it is not built.
set(ARTS_HDF5_DIR "" CACHE PATH "Install prefix of the vendored HDF5")
set(ARTS_FFTW_DIR "" CACHE PATH "Install prefix of the vendored FFTW")
set(ARTS_MINIAPPS_ROOT "" CACHE PATH "Pristine miniapps checkout")
set(ARTS_NBODY_ROOT "" CACHE PATH "Pristine NBody checkout")
set(ARTS_HPXFFT_ROOT "" CACHE PATH "Pristine hpx-fft sources")
if(ARTS_HDF5_DIR AND EXISTS "${ARTS_HDF5_DIR}/lib/libhdf5_cpp.a")
    find_package(Threads REQUIRED)
    add_library(arts_hpx_hdf5 STATIC IMPORTED GLOBAL)
    set_target_properties(arts_hpx_hdf5 PROPERTIES
        IMPORTED_LOCATION "${ARTS_HDF5_DIR}/lib/libhdf5.a"
        INTERFACE_INCLUDE_DIRECTORIES "${ARTS_HDF5_DIR}/include"
        INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS};m")
    add_library(arts_hpx_hdf5_cpp STATIC IMPORTED GLOBAL)
    set_target_properties(arts_hpx_hdf5_cpp PROPERTIES
        IMPORTED_LOCATION "${ARTS_HDF5_DIR}/lib/libhdf5_cpp.a"
        INTERFACE_INCLUDE_DIRECTORIES "${ARTS_HDF5_DIR}/include"
        INTERFACE_LINK_LIBRARIES "arts_hpx_hdf5")
    add_library(arts_hpx::hdf5_cpp ALIAS arts_hpx_hdf5_cpp)
endif()
if(ARTS_FFTW_DIR AND EXISTS "${ARTS_FFTW_DIR}/lib/libfftw3.a")
    add_library(arts_hpx_fftw3 STATIC IMPORTED GLOBAL)
    set_target_properties(arts_hpx_fftw3 PROPERTIES
        IMPORTED_LOCATION "${ARTS_FFTW_DIR}/lib/libfftw3.a"
        INTERFACE_INCLUDE_DIRECTORIES "${ARTS_FFTW_DIR}/include"
        INTERFACE_LINK_LIBRARIES "m")
    add_library(arts_hpx::fftw3 ALIAS arts_hpx_fftw3)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/apps.cmake")

# The pristine sources every program is diffed against: the vendored HPX
# tree by default, forwarded by the outer project; a program whose origin
# is elsewhere names its own root in ORIGIN.md's `origin:` line and the
# checker is pointed at it through ORIGIN_ROOT.
set(ARTS_HPX_ORIGIN_ROOT "" CACHE PATH
    "Root the HPX-origin programs' ORIGIN.md file paths are relative to")
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(WARNING "no Python 3 interpreter: the check_origin_* disclosure "
                    "tests and check_origin_selftest are not registered")
endif()

# One HPX-origin program: its sources under <name>/, its executable
# <name>_hpx in the output directory the driver reads, and a test that its
# whole difference from the origin is the committed patch.
function(add_hpx_origin_app)
    cmake_parse_arguments(P "" "NAME;ORIGIN_ROOT" "SOURCES;LINK" ${ARGN})
    if(P_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "add_hpx_origin_app: unexpected arguments: ${P_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT P_NAME IN_LIST ARTS_HPX_APPS)
        message(FATAL_ERROR "add_hpx_origin_app: ${P_NAME} is not listed in apps.cmake")
    endif()
    set(_t ${P_NAME}_hpx)
    set(_srcs)
    foreach(_s IN LISTS P_SOURCES)
        list(APPEND _srcs "${CMAKE_CURRENT_SOURCE_DIR}/${P_NAME}/${_s}")
    endforeach()
    add_executable(${_t} ${_srcs})
    arts_hpx_configure(${_t})
    # A component's HPX_REGISTER_COMPONENT_MODULE() mangles its exported
    # plugin-list symbols from HPX_COMPONENT_NAME, which defaults to "hpx" —
    # the same default the core runtime's own registry translation unit
    # carries. Two archive members exporting the same default-named symbols
    # into one static-linked executable is a duplicate definition, so a
    # program with its own component needs a component name distinct from
    # the default before it ever reaches the link.
    target_compile_definitions(${_t} PRIVATE
        "HPX_COMPONENT_NAME=hpx_${P_NAME}" "HPX_COMPONENT_STRING=\"hpx_${P_NAME}\"")
    target_include_directories(${_t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${P_NAME}")
    if(P_LINK)
        target_link_libraries(${_t} PRIVATE ${P_LINK})
    endif()
    if(HPX_APPS_RUNTIME_OUTPUT_DIRECTORY)
        set_target_properties(${_t} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${HPX_APPS_RUNTIME_OUTPUT_DIRECTORY}")
    endif()
    if(P_ORIGIN_ROOT)
        set(_root "${P_ORIGIN_ROOT}")
    else()
        set(_root "${ARTS_HPX_ORIGIN_ROOT}")
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(NAME check_origin_${P_NAME}
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_origin.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/${P_NAME}" "${_root}")
    endif()
endfunction()

add_hpx_origin_app(NAME fib_hpx
    SOURCES fibonacci_futures_distributed.cpp)

add_hpx_origin_app(NAME pi_hpx
    SOURCES distributed_pi.cpp)

add_hpx_origin_app(NAME random_mem_access_hpx
    SOURCES random_mem_access_client.cpp random_mem_access/random_mem_access.cpp)

add_hpx_origin_app(NAME stencil1d_hpx
    SOURCES 1d_stencil_8.cpp)

add_hpx_origin_app(NAME transpose_hpx
    SOURCES transpose_block.cpp)

# The component this program carries declares a dependency on the runtime's
# iostreams component (its own build file names it), and the runtime keeps
# that component's stream objects in a library of its own — so the
# dependency is named here rather than edited out of the sources.
add_hpx_origin_app(NAME jacobi_hpx
    SOURCES jacobi.cpp
            jacobi_component/grid.cpp
            jacobi_component/jacobi_component.cpp
            jacobi_component/server/row.cpp
            jacobi_component/server/solver.cpp
            jacobi_component/server/stencil_iterator.cpp
            jacobi_component/stencil_iterator.cpp
    LINK HPX::iostreams_component)

# The program's own build file declares the same dependency on the runtime's
# iostreams component, for the same reason.
add_hpx_origin_app(NAME network_storage_hpx
    SOURCES network_storage.cpp
    LINK HPX::iostreams_component)

# The first program whose origin is not the vendored HPX tree: its pristine
# sources live in another submodule, named here through ORIGIN_ROOT, and its
# ORIGIN.md file paths are relative to that root.
add_hpx_origin_app(NAME mini_ghost_hpx
    ORIGIN_ROOT "${ARTS_MINIAPPS_ROOT}"
    SOURCES mini_ghost.cpp stepper.cpp write_grid.cpp)

# Its origin lives in another submodule as well.  The origin path of its one
# source contains a space; the disclosure carries that path and the checker
# passes it to diff as a single list element, so nothing has to quote it.
add_hpx_origin_app(NAME nbody_hpx
    ORIGIN_ROOT "${ARTS_NBODY_ROOT}"
    SOURCES main.cpp)

# The third origin outside the vendored HPX tree, and the first program here
# that needs a numerical library: its transform kernel is FFTW.  A tree-driven
# build always has the prefix (the outer project's app subproject is added
# only where the dependency is built, and its external project depends on it),
# so the target is guaranteed there.  This project is also configurable on its
# own against an install, where the prefix can be absent -- and there the
# program is skipped rather than failing the generate step, which is what the
# LINK list's contract at the top of this file promises.
if(TARGET arts_hpx::fftw3)
    add_hpx_origin_app(NAME fft_hpx
        ORIGIN_ROOT "${ARTS_HPXFFT_ROOT}"
        SOURCES fft_hpx_loop.cpp
        LINK arts_hpx::fftw3)
else()
    message(STATUS "Skipping fft_hpx: the vendored FFTW is not configured")
endif()

# The one program here whose input is a dataset rather than its arguments: it
# reads hyperslabs of a tabulated equation of state through the HDF5 C++ API.
# A tree-driven build always has the prefix (the outer project's app
# subproject is added only where the dependency is built, and its external
# project depends on it), so the target is guaranteed there.  This project is
# also configurable on its own against an install, where the prefix can be
# absent -- and there the program is skipped rather than failing the generate
# step, which is what the LINK list's contract at the top of this file
# promises.  Its component lives in a sub-library of its own in the origin;
# here the whole program is the executable's six translation units.
if(TARGET arts_hpx::hdf5_cpp)
    add_hpx_origin_app(NAME sheneos_hpx
        SOURCES sheneos_test.cpp
                sheneos/interpolator.cpp
                sheneos/read_values.cpp
                sheneos/dimension.cpp
                sheneos/server/partition3d.cpp
                sheneos/server/configuration.cpp
        LINK arts_hpx::hdf5_cpp)
else()
    message(STATUS "Skipping sheneos_hpx: the vendored HDF5 is not configured")
endif()

# The disclosure tests are only as good as the checker: this drives every
# exit code it owes its caller out of a fixture it builds itself.
if(Python3_Interpreter_FOUND)
    add_test(NAME check_origin_selftest
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_origin.py"
            --selftest)
endif()

# A probe is not a program of the roster: it measures one runtime
# primitive, carries no application, and is never a campaign cell.  It keeps
# its own output directory so a driver enumerating the programs cannot pick
# it up.
function(add_hpx_probe NAME)
    add_executable(${NAME} probes/${NAME}.cpp)
    arts_hpx_configure(${NAME})
    if(HPX_APPS_RUNTIME_OUTPUT_DIRECTORY)
        set_target_properties(${NAME} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${HPX_APPS_RUNTIME_OUTPUT_DIRECTORY}/probes")
    endif()
endfunction()

add_hpx_probe(action_rtt)

# Two-locality smoke of the static build: components (collectives), a plain
# action into a receive buffer, the counter registry, and the hpx_main
# shape.  Needs an MPI launcher, which the outer project hands in together
# with the switch that leaves the smoke out on a host whose launcher cannot
# start a two-rank job.
if(ARTS_MPIEXEC AND ARTS_HPX_SMOKE)
    add_executable(static_smoke tests/static_smoke.cpp)
    arts_hpx_configure(static_smoke)
    add_test(NAME static_smoke
        COMMAND ${ARTS_MPIEXEC} -n 2 $<TARGET_FILE:static_smoke> --hpx:threads=2)
    # The exit status is the verdict; a pass expression would make ctest
    # ignore it and call a crash after the printed line a pass.
    set_tests_properties(static_smoke PROPERTIES
        FAIL_REGULAR_EXPRESSION "SMOKE FAIL" LABELS mpi
        ENVIRONMENT "ARTS_E2E_MARKER=1;ARTS_STRUCT_MARKER=1" TIMEOUT 120)
    # The same smoke choosing a policy on the command line.  The scheduling
    # entry the programs install is deliberately non-forcing, and only a run
    # that overrides it proves that: a forcing entry passes the first test
    # exactly as well as this one fails.  Naming a policy also cannot
    # pass quietly on a build whose thread-queue backends were not compiled
    # in: the runtime reports the policy as unconfigured and then dies.
    add_test(NAME static_smoke_queuing_override
        COMMAND ${ARTS_MPIEXEC} -n 2 $<TARGET_FILE:static_smoke> --hpx:threads=2
            --hpx:queuing=local-priority-fifo)
    set_tests_properties(static_smoke_queuing_override PROPERTIES
        FAIL_REGULAR_EXPRESSION "SMOKE FAIL" LABELS mpi
        ENVIRONMENT "ARTS_E2E_MARKER=1;ARTS_STRUCT_MARKER=1;ARTS_HPX_EXPECT_SCHEDULER=local-priority-fifo"
        TIMEOUT 120)
endif()
