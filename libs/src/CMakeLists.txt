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

# Create the main ARTS library interface target
add_library(arts_interface INTERFACE)
target_include_directories(arts_interface INTERFACE
    $<BUILD_INTERFACE:${ARTS_PUBLIC_INCLUDE_DIR}>
    $<BUILD_INTERFACE:${ARTS_INTERNAL_INCLUDE_DIR}>
    $<BUILD_INTERFACE:${ARTS_BUILD_INTERNAL_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# --- Coherence-protocol helpers (shared by main lib + benchmark variants) -----
# The grant plane's release-policy half: WHO initiates a grant's return.  Only
# a write-through arm can hand the permission back without the payload, so a
# write-back grant is always demand-revoked.
function(arts_grant_release_tu out write_policy release_policy)
    if(write_policy STREQUAL "WT" AND release_policy STREQUAL "PURGE")
        set(${out} coherence/grant_purge.c PARENT_SCOPE)
    else()
        set(${out} coherence/grant_retain.c PARENT_SCOPE)
    endif()
endfunction()

# Sets ${out} (caller scope) to the coherence model TU paths for one (arm,
# write policy, release policy), RELATIVE to libs/src/core/; the main library
# (libs/src/core/CMakeLists.txt) and every benchmark variant select through it.
# The dense per-rank version map is the ownership plane's ledger, so it is
# linked by the arms that HAVE one; an arm with no versions carries no ledger
# and must not compile the TU that keeps it.
function(arts_coherence_model_tus out arm write_policy release_policy)
    if(arm STREQUAL "FLUSH")
        set(tus coherence/flush/flush.c)
    elseif(arm STREQUAL "INV")
        if(write_policy STREQUAL "WB")
            set(tus coherence/inv/directory.c coherence/inv/engine.c coherence/inv/wb.c coherence/grant.c coherence/grant_queue.c coherence/grant_wb.c coherence/rank_u64_map.c)
        else()
            set(tus coherence/inv/directory.c coherence/inv/engine.c coherence/inv/wt.c coherence/grant.c coherence/grant_queue.c coherence/grant_wt.c coherence/rank_u64_map.c)
        endif()
        arts_grant_release_tu(rel "${write_policy}" "${release_policy}")
        list(APPEND tus ${rel})
    elseif(arm STREQUAL "EXCL")
        if(release_policy STREQUAL "RETAIN")
            set(tus coherence/excl/directory.c coherence/excl/retain.c)
        else()
            set(tus coherence/excl/directory.c coherence/excl/purge.c)
        endif()
    elseif(arm STREQUAL "VAL")
        if(write_policy STREQUAL "WT")
            set(tus coherence/val/wt.c coherence/grant.c coherence/grant_queue.c coherence/grant_wt.c coherence/val/directory.c coherence/rank_u64_map.c)
        else()
            set(tus coherence/val/wb.c coherence/grant.c coherence/grant_queue.c coherence/grant_wb.c coherence/val/directory.c coherence/rank_u64_map.c)
        endif()
        arts_grant_release_tu(rel "${write_policy}" "${release_policy}")
        list(APPEND tus ${rel})
    else()
        message(FATAL_ERROR "no coherence arm named '${arm}'")
    endif()
    set(${out} ${tus} PARENT_SCOPE)
endfunction()

# Applies the family + second-axis compile definitions to <target> (PRIVATE),
# and the CXL store definitions that travel with them.
# ARTS_PROTOCOL_<arm> is always emitted; VAL/INV/EXCL additionally get one of
# ARTS_WRITE_POLICY_WT/WB and one of ARTS_RELEASE_PURGE/RETAIN (each arm's
# sources test only its live axis: EXCL: ARTS_RELEASE_*; INV/VAL and the
# grant plane: ARTS_WRITE_POLICY_*).  FLUSH has neither axis, so it gets
# ARTS_PROTOCOL_FLUSH alone.
function(arts_apply_protocol target arm write_policy release_policy)
    # A call with no declaration in scope is an error, not a warning.  Each arm
    # compiles a different subset of the sources, so a declaration guarded to
    # the wrong set of arms is invisible in the arm that was tested and leaves
    # the others calling through an implicit int -- which silently narrows a
    # pointer or a bool return.  Compilers that still default this to a warning
    # let it accumulate unseen; the ones that do not simply refuse to build.
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)
    target_compile_definitions(${target} PRIVATE ARTS_PROTOCOL_${arm}=1)
    # The CXL definitions travel with the protocol ones, by the same route:
    # the runtime is assembled from object libraries that each receive their
    # own definitions, and two TUs of one binary that disagree about
    # ARTS_USE_CXL disagree about a packet layout.  An optional 5th argument
    # names this target's residency; OFF means no CXL store for this target,
    # and no argument means "whatever this tree's own library uses".  A
    # variant always names one, so it can never inherit the tree's.  Targets
    # that name it are self-describing: nothing outside their own definition
    # decides what they compile, which is what lets several configurations
    # coexist in one build.
    if(ARGC GREATER 4)
        set(cxl_residency "${ARGV4}")
    else()
        set(cxl_residency "${ARTS_CXL_TREE_RESIDENCY}")
    endif()
    if(NOT cxl_residency STREQUAL "" AND NOT cxl_residency STREQUAL "OFF")
        if(NOT ARTS_CXL_INCLUDE_DIR)
            message(FATAL_ERROR "${target} names residency ${cxl_residency} but this tree resolved no CXL library")
        endif()
        target_compile_definitions(${target} PRIVATE
            ARTS_USE_CXL=1
            ARTS_CXL_${cxl_residency}=1
            ARTS_CXL_DB_ARENA_SIZE_BYTES=${ARTS_CXL_DB_ARENA_SIZE_BYTES}ULL)
        target_include_directories(${target} SYSTEM PRIVATE ${ARTS_CXL_INCLUDE_DIR})
    endif()
    if(arm STREQUAL "FLUSH")
        # FLUSH has neither second axis: every remote acquire fetches and every
        # remote RW release writes through, so no selection chain branches on
        # a write or release policy here.
        return()
    endif()
    if(write_policy STREQUAL "WT")
        target_compile_definitions(${target} PRIVATE ARTS_WRITE_POLICY_WT=1)
    else()
        target_compile_definitions(${target} PRIVATE ARTS_WRITE_POLICY_WB=1)
    endif()
    if(release_policy STREQUAL "PURGE")
        target_compile_definitions(${target} PRIVATE ARTS_RELEASE_PURGE=1)
    else()
        target_compile_definitions(${target} PRIVATE ARTS_RELEASE_RETAIN=1)
    endif()
endfunction()

# --- Vendored dependencies inside the installed libraries --------------------
# An application links the runtime and nothing else: `-larts` alone must
# resolve, from either the archive or the shared object.  The vendored
# dependencies are therefore part of the artifact rather than part of a
# consumer's link line, which also makes the artifact's behaviour independent
# of whatever the consumer's host happens to offer under the same names.
#
# Ordering: the archives are produced by an ExternalProject and a subdirectory
# target.  A link edge already implies both, but the archive merge below runs
# as a POST_BUILD step whose inputs the build system cannot see, so each is
# named explicitly.
function(arts_depend_on_vendored target)
    if(TARGET libfabric_ep)
        add_dependencies(${target} libfabric_ep)
    endif()
    if(TARGET mimalloc-static)
        add_dependencies(${target} mimalloc-static)
    endif()
endfunction()

# Folds the vendored archives into <target>'s own archive and localizes their
# symbols, so a consumer that links its own copy of one of them keeps it.  The
# static-library rule recreates the archive from its objects before POST_BUILD
# runs, so the merge starts from the same inputs on every build instead of
# compounding -- and the archive holds exactly this target's own objects at that
# moment, which is what the script reads its keep-global allowlist from.
function(arts_bundle_vendored_archives target)
    if(NOT ARTS_VENDORED_ARCHIVES)
        return()
    endif()
    arts_depend_on_vendored(${target})
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DMERGE_AR=${CMAKE_AR}
            -DMERGE_RANLIB=${CMAKE_RANLIB}
            -DMERGE_NM=${CMAKE_NM}
            -DMERGE_LD=${CMAKE_LINKER}
            -DMERGE_OBJCOPY=${CMAKE_OBJCOPY}
            -DMERGE_ARCHIVE=$<TARGET_FILE:${target}>
            -P ${PROJECT_SOURCE_DIR}/cmake/MergeStaticArchives.cmake
            -- ${ARTS_VENDORED_ARCHIVES}
        COMMENT "Bundling vendored archives into $<TARGET_FILE_NAME:${target}>"
        VERBATIM)
endfunction()

# An executable linking an installed runtime library is built non-PIE (see
# ARTS_NON_PIE_COMPILE_OPTIONS).  The options reach executables only: a shared
# library or module that links the runtime keeps its own -fPIC, since only the
# executable must load at one address on every rank.  The tree's own programs
# set the options themselves, so only the install side carries them.
function(arts_export_non_pie target)
    set(_exe "$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>")
    target_compile_options(${target} INTERFACE
        "$<INSTALL_INTERFACE:$<${_exe}:${ARTS_NON_PIE_COMPILE_OPTIONS}>>")
    target_link_options(${target} INTERFACE
        "$<INSTALL_INTERFACE:$<${_exe}:${ARTS_NON_PIE_LINK_OPTIONS}>>")
endfunction()

# Embeds the same archives in a shared <target> and keeps their symbols out of
# its dynamic exports: a vendored symbol that stays exported can be interposed
# by a copy in the executable, which would split a library's state between two
# instances of itself.
function(arts_embed_vendored_archives target)
    arts_depend_on_vendored(${target})
    # PRIVATE, not INTERFACE: the archive belongs inside the shared object, not
    # on the line of everything that links it.
    target_link_libraries(${target} PRIVATE arts::libfabric)
    # Both options below are GNU ld / lld spellings, and ELF symbol versioning
    # is the problem only one of them solves.
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif()
    foreach(name IN LISTS ARTS_VENDORED_EXCLUDE_LIBS)
        target_link_options(${target} PRIVATE "LINKER:--exclude-libs,${name}")
    endforeach()
    # libfabric's symbols are version-tagged (fi_getinfo@@FABRIC_1.x) and a
    # version-tagged definition needs its version node declared at this link,
    # which the derived script supplies without hiding anything of ours the way
    # libfabric's own script would.
    target_link_options(${target} PRIVATE
        "LINKER:--version-script=${OFI_VERSION_NODES_MAP}")
endfunction()

# No-hint placement policies, visible to every libarts compile in this
# directory and its subdirectories (edt.c / db.c gate their no-hint branches
# on these; see ARTS_NOHINT_EDT_PLACEMENT / ARTS_NOHINT_DB_HOME at top level).
add_compile_definitions(
    ARTS_NOHINT_EDT_ROUNDROBIN=${_ARTS_NOHINT_EDT_RR}
    ARTS_NOHINT_DB_ROUNDROBIN=${_ARTS_NOHINT_DB_RR})

# Module subdirectories — each defines OBJECT library targets
add_subdirectory(core)
add_subdirectory(graph)

# Main source file (main.c lives in core/; scheduler.c is an OBJECT library)
set(ARTS_MAIN_SOURCES "core/main.c")

#
# Standard ARTS Library (always built, no CUDA)
#

# The static library is the substrate every other artifact derives from.
add_library(arts_static STATIC ${ARTS_MAIN_SOURCES})

target_include_directories(arts_static BEFORE PRIVATE
    ${ARTS_BUILD_INTERNAL_INCLUDE_DIR}
    ${ARTS_PUBLIC_INCLUDE_DIR}
    ${ARTS_INTERNAL_INCLUDE_DIR}
)

# Add hwloc include directories if available
if(HWLOC_INCLUDE_DIRS)
    target_include_directories(arts_static PRIVATE ${HWLOC_INCLUDE_DIRS})
endif()

# Add object library contents directly to avoid export issues
target_sources(arts_static PRIVATE
    $<TARGET_OBJECTS:arts_gas>
    $<TARGET_OBJECTS:arts_transport>
    $<TARGET_OBJECTS:arts_counter>
    $<TARGET_OBJECTS:arts_scheduler>
    $<TARGET_OBJECTS:arts_compute>
    $<TARGET_OBJECTS:arts_memory>
    $<TARGET_OBJECTS:arts_sync>
    $<TARGET_OBJECTS:arts_ooo>
    $<TARGET_OBJECTS:arts_regpool>
    $<TARGET_OBJECTS:arts_system>
    $<TARGET_OBJECTS:arts_utils>
    $<TARGET_OBJECTS:arts_graph>
)
if(ARTS_CXL_TREE_RESIDENCY)
    target_sources(arts_static PRIVATE $<TARGET_OBJECTS:arts_cxl>)
    target_link_libraries(arts_static PUBLIC ${ARTS_CXL_LIB_PATH} ${ARTS_CXL_RAPID_LIBS})
endif()

target_link_libraries(arts_static PUBLIC arts_interface)

# The vendored archives are folded into a runtime archive's one object (see
# arts_bundle_vendored_archives), so an installed consumer links only the
# toolchain libraries that object still references, by name: pthread, dl, rt
# and m (libatomic comes from the directory-wide link_libraries).  The build
# tree keeps the vendored targets, which also carry the include directories
# the sources compile against.
set(ARTS_INSTALLED_TOOLCHAIN_LINK ${CMAKE_DL_LIBS})
if(NOT APPLE)
    list(APPEND ARTS_INSTALLED_TOOLCHAIN_LINK rt m)
endif()
list(TRANSFORM ARTS_INSTALLED_TOOLCHAIN_LINK PREPEND "$<INSTALL_INTERFACE:")
list(TRANSFORM ARTS_INSTALLED_TOOLCHAIN_LINK APPEND ">")

target_link_libraries(arts_static PRIVATE
    Threads::Threads
    $<BUILD_INTERFACE:${RT_LIBRARY}>
    $<BUILD_INTERFACE:${M_LIBRARY}>
    $<BUILD_INTERFACE:${HWLOC_LIBRARIES}>
    ${ARTS_INSTALLED_TOOLCHAIN_LINK}
)

# Add macOS framework dependencies for hwloc
if(APPLE AND HWLOC_FOUND)
    target_link_libraries(arts_static PRIVATE "-framework CoreFoundation" "-framework IOKit" "-framework OpenCL")
endif()

target_link_libraries(arts_static PRIVATE $<BUILD_INTERFACE:arts::libfabric>)

arts_apply_protocol(arts_static ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})

arts_bundle_vendored_archives(arts_static)

# Set library properties
set_target_properties(arts_static PROPERTIES
    OUTPUT_NAME arts
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME static
)
arts_export_non_pie(arts_static)

# Create shared library if requested
if(ARTS_BUILD_SHARED)
    add_library(arts_shared SHARED ${ARTS_MAIN_SOURCES})

    target_include_directories(arts_shared BEFORE PRIVATE
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR}
        ${ARTS_PUBLIC_INCLUDE_DIR}
        ${ARTS_INTERNAL_INCLUDE_DIR}
    )

    # Add hwloc include directories if available
    if(HWLOC_INCLUDE_DIRS)
        target_include_directories(arts_shared PRIVATE ${HWLOC_INCLUDE_DIRS})
    endif()

    # Add object library contents directly to avoid export issues
    target_sources(arts_shared PRIVATE
        $<TARGET_OBJECTS:arts_gas>
        $<TARGET_OBJECTS:arts_transport>
        $<TARGET_OBJECTS:arts_counter>
        $<TARGET_OBJECTS:arts_scheduler>
        $<TARGET_OBJECTS:arts_compute>
        $<TARGET_OBJECTS:arts_memory>
        $<TARGET_OBJECTS:arts_sync>
        $<TARGET_OBJECTS:arts_ooo>
        $<TARGET_OBJECTS:arts_regpool>
        $<TARGET_OBJECTS:arts_system>
        $<TARGET_OBJECTS:arts_utils>
        $<TARGET_OBJECTS:arts_graph>
    )
    if(ARTS_CXL_TREE_RESIDENCY)
        target_sources(arts_shared PRIVATE $<TARGET_OBJECTS:arts_cxl>)
        target_link_libraries(arts_shared PUBLIC ${ARTS_CXL_LIB_PATH} ${ARTS_CXL_RAPID_LIBS})
    endif()

    target_link_libraries(arts_shared PUBLIC arts_interface)

    # Link external dependencies statically into the shared library
    target_link_libraries(arts_shared PRIVATE
        Threads::Threads
        ${RT_LIBRARY}
        ${M_LIBRARY}
        $<BUILD_INTERFACE:${HWLOC_LIBRARIES}>
    )

    arts_embed_vendored_archives(arts_shared)

    # Add macOS framework dependencies for hwloc
    if(APPLE AND HWLOC_FOUND)
        target_link_libraries(arts_shared PRIVATE "-framework CoreFoundation" "-framework IOKit" "-framework OpenCL")
    endif()

    arts_apply_protocol(arts_shared ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})

    # Set library properties
    set_target_properties(arts_shared PROPERTIES
        OUTPUT_NAME arts
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        EXPORT_NAME shared
    )
    arts_export_non_pie(arts_shared)
endif()

#
# CUDA-enabled ARTS Library (built when CUDA available)
#

if(BUILD_CUDA_LIBRARY)
    add_library(arts_cuda_static STATIC ${ARTS_MAIN_SOURCES})

    target_include_directories(arts_cuda_static BEFORE PRIVATE
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR}
        ${ARTS_PUBLIC_INCLUDE_DIR}
        ${ARTS_INTERNAL_INCLUDE_DIR}
    )

    # Add hwloc include directories if available
    if(HWLOC_INCLUDE_DIRS)
        target_include_directories(arts_cuda_static PRIVATE ${HWLOC_INCLUDE_DIRS})
    endif()

    # Add object library contents directly to avoid export issues
    target_sources(arts_cuda_static PRIVATE
        $<TARGET_OBJECTS:arts_gas>
        $<TARGET_OBJECTS:arts_transport>
        $<TARGET_OBJECTS:arts_counter>
        $<TARGET_OBJECTS:arts_scheduler_gpu>
        $<TARGET_OBJECTS:arts_compute_gpu>
        $<TARGET_OBJECTS:arts_memory_gpu>
        $<TARGET_OBJECTS:arts_sync>
        $<TARGET_OBJECTS:arts_ooo>
        $<TARGET_OBJECTS:arts_regpool>
        $<TARGET_OBJECTS:arts_system>
        $<TARGET_OBJECTS:arts_utils>
        $<TARGET_OBJECTS:arts_gpu>
        $<TARGET_OBJECTS:arts_graph>
    )
    if(ARTS_CXL_TREE_RESIDENCY)
        target_sources(arts_cuda_static PRIVATE $<TARGET_OBJECTS:arts_cxl>)
        target_link_libraries(arts_cuda_static PUBLIC ${ARTS_CXL_LIB_PATH} ${ARTS_CXL_RAPID_LIBS})
    endif()

    target_link_libraries(arts_cuda_static PUBLIC arts_interface)

    # Add CUDA compile definition for this target
    target_compile_definitions(arts_cuda_static PRIVATE ARTS_USE_GPU)

    # Link external dependencies and CUDA libraries
    target_link_libraries(arts_cuda_static PRIVATE
        Threads::Threads
        $<BUILD_INTERFACE:${RT_LIBRARY}>
        $<BUILD_INTERFACE:${M_LIBRARY}>
        $<BUILD_INTERFACE:${HWLOC_LIBRARIES}>
        ${ARTS_INSTALLED_TOOLCHAIN_LINK}
        CUDA::cudart
        CUDA::cuda_driver
        CUDA::cublas
    )

    # Add macOS framework dependencies for hwloc
    if(APPLE AND HWLOC_FOUND)
        target_link_libraries(arts_cuda_static PRIVATE "-framework CoreFoundation" "-framework IOKit" "-framework OpenCL")
    endif()

    arts_bundle_vendored_archives(arts_cuda_static)

    arts_apply_protocol(arts_cuda_static ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})

    # Set library properties
    set_target_properties(arts_cuda_static PROPERTIES
        OUTPUT_NAME arts_cuda
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        EXPORT_NAME cuda_static
    )
    arts_export_non_pie(arts_cuda_static)

    # Create shared CUDA library if requested
    if(ARTS_BUILD_SHARED)
        add_library(arts_cuda_shared SHARED ${ARTS_MAIN_SOURCES})

        target_include_directories(arts_cuda_shared BEFORE PRIVATE
            ${ARTS_BUILD_INTERNAL_INCLUDE_DIR}
            ${ARTS_PUBLIC_INCLUDE_DIR}
            ${ARTS_INTERNAL_INCLUDE_DIR}
        )

        # Add hwloc include directories if available
        if(HWLOC_INCLUDE_DIRS)
            target_include_directories(arts_cuda_shared PRIVATE ${HWLOC_INCLUDE_DIRS})
        endif()

        # Add object library contents directly to avoid export issues
        target_sources(arts_cuda_shared PRIVATE
            $<TARGET_OBJECTS:arts_gas>
            $<TARGET_OBJECTS:arts_transport>
            $<TARGET_OBJECTS:arts_counter>
            $<TARGET_OBJECTS:arts_scheduler_gpu>
            $<TARGET_OBJECTS:arts_compute_gpu>
            $<TARGET_OBJECTS:arts_memory_gpu>
            $<TARGET_OBJECTS:arts_sync>
            $<TARGET_OBJECTS:arts_ooo>
            $<TARGET_OBJECTS:arts_regpool>
            $<TARGET_OBJECTS:arts_system>
            $<TARGET_OBJECTS:arts_utils>
            $<TARGET_OBJECTS:arts_gpu>
            $<TARGET_OBJECTS:arts_graph>
        )
        if(ARTS_CXL_TREE_RESIDENCY)
            target_sources(arts_cuda_shared PRIVATE $<TARGET_OBJECTS:arts_cxl>)
            target_link_libraries(arts_cuda_shared PUBLIC ${ARTS_CXL_LIB_PATH} ${ARTS_CXL_RAPID_LIBS})
        endif()

        target_link_libraries(arts_cuda_shared PUBLIC arts_interface)

        # Add CUDA compile definition for this target
        target_compile_definitions(arts_cuda_shared PRIVATE ARTS_USE_GPU)

        # Link external dependencies and CUDA libraries
        target_link_libraries(arts_cuda_shared PRIVATE
            Threads::Threads
            ${RT_LIBRARY}
            ${M_LIBRARY}
            $<BUILD_INTERFACE:${HWLOC_LIBRARIES}>
            CUDA::cudart
            CUDA::cuda_driver
            CUDA::cublas
        )

        arts_embed_vendored_archives(arts_cuda_shared)

        # Add macOS framework dependencies for hwloc
        if(APPLE AND HWLOC_FOUND)
            target_link_libraries(arts_cuda_shared PRIVATE "-framework CoreFoundation" "-framework IOKit" "-framework OpenCL")
        endif()

        arts_apply_protocol(arts_cuda_shared ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})

        # Set library properties
        set_target_properties(arts_cuda_shared PROPERTIES
            OUTPUT_NAME arts_cuda
            VERSION ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR}
            EXPORT_NAME cuda_shared
        )
        arts_export_non_pie(arts_cuda_shared)
    endif()
endif()

# --- Per-protocol variant libraries (benchmarks/harness only) -----------------
# Build a full arts static lib for one (arm, write policy, release policy).  The
# protocol-independent sources are gathered from the CPU object-lib targets
# (their compiled objects cannot be reused — each variant needs its own protocol
# defines), the coherence MODEL TUs are swapped for this variant's, and the
# defines are applied per-target.
if(ARTS_BUILD_BENCHMARKS)
    set(_ARTS_VARIANT_OBJLIBS
        arts_compute arts_memory arts_sync arts_ooo arts_regpool arts_scheduler
        arts_transport arts_gas arts_system arts_utils arts_counter arts_graph)
    if(TARGET arts_cxl)
        list(APPEND _ARTS_VARIANT_OBJLIBS arts_cxl)
    endif()

    # Applies variant <suffix>'s cell to <target>: every translation unit
    # linked into a variant's executable is compiled for that variant's cell,
    # never for the tree's.
    function(arts_apply_variant_protocol target suffix)
        get_property(cell GLOBAL PROPERTY ARTS_VARIANT_CELL_${suffix})
        if(NOT cell)
            message(FATAL_ERROR "no ARTS variant named ${suffix}")
        endif()
        arts_apply_protocol(${target} ${cell})
    endfunction()

    function(arts_variant suffix arm write_policy release_policy)
        # Optional 5th argument: the residency this variant's store uses.  A
        # variant that names one compiles the cxl module; every other variant
        # passes OFF and never sees it -- including in a CXL tree,
        # where the tree's own residency must not leak into wrf_flush or any
        # other arm.  (ARGC counts the declared parameters too.)
        set(cxl_residency "OFF")
        if(ARGC GREATER 4)
            set(cxl_residency "${ARGV4}")
        endif()
        # Gather all protocol-independent sources (absolute paths) from the
        # object libs, dropping any coherence MODEL TU (path contains
        # /coherence/<model>/), which is variant-specific.
        set(srcs "${CMAKE_CURRENT_SOURCE_DIR}/core/main.c")
        foreach(lib ${_ARTS_VARIANT_OBJLIBS})
            get_target_property(lsrcs ${lib} SOURCES)
            get_target_property(ldir  ${lib} SOURCE_DIR)
            foreach(s ${lsrcs})
                if(IS_ABSOLUTE "${s}")
                    set(abs "${s}")
                else()
                    set(abs "${ldir}/${s}")
                endif()
                # Skip variant-specific coherence TUs: the per-arm subdirs, the
                # grant plane (linked only by arms whose ownership migrates), and
                # the version ledger (only by arms that keep versions).
                if(abs MATCHES "/core/cxl/")
                    # The store is a per-variant choice like the protocol, so a
                    # variant that does not name it must not compile it.
                    list(APPEND cxl_srcs "${abs}")
                elseif(NOT abs MATCHES "/coherence/(val|excl|inv|flush)/|/coherence/(grant(_wt|_wb|_queue|_purge|_retain)?|rank_u64_map)\\.c$")
                    list(APPEND srcs "${abs}")
                endif()
            endforeach()
        endforeach()
        # add this variant's coherence model TUs
        arts_coherence_model_tus(model "${arm}" "${write_policy}" "${release_policy}")
        foreach(tu ${model})
            list(APPEND srcs "${CMAKE_CURRENT_SOURCE_DIR}/core/${tu}")
        endforeach()

        if(NOT cxl_residency STREQUAL "OFF")
            if(NOT cxl_srcs)
                message(FATAL_ERROR
                        "arts_variant(${suffix} ... ${cxl_residency}) names a "
                        "residency, but no cxl translation unit was gathered")
            endif()
            list(APPEND srcs ${cxl_srcs})
        endif()

        add_library(arts_static_${suffix} STATIC ${srcs})
        target_include_directories(arts_static_${suffix} BEFORE PRIVATE
            ${ARTS_BUILD_INTERNAL_INCLUDE_DIR}
            ${ARTS_PUBLIC_INCLUDE_DIR}
            ${ARTS_INTERNAL_INCLUDE_DIR})
        if(HWLOC_INCLUDE_DIRS)
            target_include_directories(arts_static_${suffix} PRIVATE ${HWLOC_INCLUDE_DIRS})
        endif()
        target_link_libraries(arts_static_${suffix} PUBLIC arts_interface)
        target_link_libraries(arts_static_${suffix} PRIVATE
            Threads::Threads ${RT_LIBRARY} ${M_LIBRARY}
            $<BUILD_INTERFACE:${HWLOC_LIBRARIES}>)
        if(ARTS_MALLOC STREQUAL "mimalloc")
            target_link_libraries(arts_static_${suffix} PRIVATE mimalloc-static)
        endif()
        # regpool.c (gathered from arts_regpool) includes fabric headers and
        # the static lib resolves fi_* symbols from libfabric.
        target_link_libraries(arts_static_${suffix} PRIVATE arts::libfabric)
        set_property(GLOBAL APPEND PROPERTY ARTS_VARIANTS ${suffix})
        set_property(GLOBAL PROPERTY ARTS_VARIANT_CELL_${suffix}
                     "${arm}" "${write_policy}" "${release_policy}"
                     "${cxl_residency}")
        arts_apply_variant_protocol(arts_static_${suffix} ${suffix})
        if(NOT cxl_residency STREQUAL "OFF")
            target_link_libraries(arts_static_${suffix} PRIVATE ${ARTS_CXL_LIB_PATH} ${ARTS_CXL_RAPID_LIBS})
        endif()
    endfunction()

    # Suffix = <memory model>_<family>_<live second axis>; the second argument
    # is the protocol ARM (the TU family): VAL/INV/EXCL live under the OCR
    # model.  The dead axis sits at its required value (WB for EXCL, RETAIN
    # for INV/VAL).  FLUSH is the DB-WRF model's one protocol and carries no
    # axis token.
    arts_variant(ocr_val_wt        VAL     WT RETAIN)
    arts_variant(ocr_val_wb        VAL     WB RETAIN)
    arts_variant(ocr_val_wt_purge  VAL     WT PURGE)
    arts_variant(ocr_excl_purge    EXCL    WB PURGE)
    arts_variant(ocr_excl_retain   EXCL    WB RETAIN)
    arts_variant(ocr_inv_wt        INV     WT RETAIN)
    arts_variant(ocr_inv_wb        INV     WB RETAIN)
    arts_variant(ocr_inv_wt_purge  INV     WT PURGE)
    arts_variant(wrf_flush         FLUSH   WB RETAIN)

    # The two CXL entries of the EXCL x PURGE x WB cell: one variable changes
    # against ocr_excl_purge -- where a block's bytes rest -- so arm, write
    # policy and release policy are shared with it.  Always built: the library
    # they link is the vendored fake unless ARTS_CXL_REAL names the device's.
    arts_variant(ocr_excl_purge_cxl_staged EXCL WB PURGE STAGED)
    arts_variant(ocr_excl_purge_cxl_direct EXCL WB PURGE DIRECT)
endif()

# Link mimalloc-static when selected.  The OBJECT library arts_utils needs
# the include dir at compile time (for malloc.c's #include <mimalloc.h>);
# target_link_libraries on an OBJECT target propagates INTERFACE properties
# (include dirs) to its compilation unit without introducing a link dep.
if(ARTS_MALLOC STREQUAL "mimalloc")
    target_link_libraries(arts_utils PRIVATE mimalloc-static)
    target_link_libraries(arts_static PRIVATE $<BUILD_INTERFACE:mimalloc-static>)
    if(ARTS_BUILD_SHARED)
        target_link_libraries(arts_shared PRIVATE mimalloc-static)
    endif()
    if(BUILD_CUDA_LIBRARY)
        target_link_libraries(arts_cuda_static PRIVATE $<BUILD_INTERFACE:mimalloc-static>)
        if(ARTS_BUILD_SHARED)
            target_link_libraries(arts_cuda_shared PRIVATE mimalloc-static)
        endif()
    endif()
endif()

# arts_static with the flush-recording adapter in place of its own: the same
# objects, the same link interface and the same vendored bundle, so a test that
# links it runs the runtime under test and differs only in what the adapter
# records.
if(TARGET arts_cxl_flush_recorder)
    add_library(arts_static_cxl_flush_recorder STATIC EXCLUDE_FROM_ALL
                ${ARTS_MAIN_SOURCES})
    get_target_property(_arts_rec_srcs arts_static SOURCES)
    list(REMOVE_ITEM _arts_rec_srcs ${ARTS_MAIN_SOURCES}
         "$<TARGET_OBJECTS:arts_cxl>")
    target_sources(arts_static_cxl_flush_recorder PRIVATE ${_arts_rec_srcs}
                   $<TARGET_OBJECTS:arts_cxl_flush_recorder>)
    foreach(_arts_rec_prop INCLUDE_DIRECTORIES COMPILE_DEFINITIONS
                           COMPILE_OPTIONS LINK_LIBRARIES
                           INTERFACE_LINK_LIBRARIES)
        get_target_property(_arts_rec_val arts_static ${_arts_rec_prop})
        if(_arts_rec_val)
            set_property(TARGET arts_static_cxl_flush_recorder
                         PROPERTY ${_arts_rec_prop} "${_arts_rec_val}")
        endif()
    endforeach()
    arts_bundle_vendored_archives(arts_static_cxl_flush_recorder)
endif()

# The same names the installed package exports.
add_library(ARTS::static ALIAS arts_static)
if(ARTS_BUILD_SHARED)
    add_library(ARTS::shared ALIAS arts_shared)
endif()
if(BUILD_CUDA_LIBRARY)
    add_library(ARTS::cuda_static ALIAS arts_cuda_static)
    if(ARTS_BUILD_SHARED)
        add_library(ARTS::cuda_shared ALIAS arts_cuda_shared)
    endif()
endif()
