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

# Programs here check their verdicts with assert(): a build type that
# defines NDEBUG must not compile those checks out and let a test pass with
# nothing checked.
add_compile_options(-UNDEBUG)

add_compile_options(${ARTS_WERROR_IMPLICIT_SEQ_CST})

# The reference tree: the protocol cell of the build defaults, with payloads in
# rank memory.  A test whose outcome no coherence protocol, write or release
# policy or payload store can change is built and registered there only;
# every other tree runs only what its cell can change.  The cell alone decides
# it, so a sanitizer, injection or quarantine tree of this cell is a reference
# tree too.  CACHE INTERNAL, like the helpers' settings below: the examples
# register their smoke tests by the same rule from their own directory.
set(_reference_tree OFF)
if(ARTS_COHERENCE_ARM STREQUAL _ARTS_DEFAULT_COHERENCE_PROTOCOL AND
   ARTS_WRITE_POLICY STREQUAL _ARTS_DEFAULT_WRITE_POLICY AND
   ARTS_RELEASE_POLICY STREQUAL _ARTS_DEFAULT_RELEASE_POLICY AND
   NOT ARTS_USE_CXL)
    set(_reference_tree ON)
endif()
set(ARTS_TEST_REFERENCE_TREE ${_reference_tree}
    CACHE INTERNAL "whether this tree registers the tests no cell can change" FORCE)
unset(_reference_tree)

# Test programs whose outcome depends on the tree's protocol cell.
set(TEST_PROGRAMS
    # --- Coherence ---
    route_table_remote_guid
    grant_purge_no_acquire_creator
    grant_purge_churn
    grant_purge_handback_regrant
    coherence_rwro_phase
    coherence_stress_single_node
    coherence_payload_large
    coherence_satisfy_before_release
    coherence_multi_writer_multi_db
    coherence_multi_writer_dist
    coherence_ro_acquire_stress
    coherence_owner_ownership_reorder
    coherence_owner_confirm_gate
    coherence_writer_ro_overlap
    coherence_grant_chain_visibility
    event_channel_fifo
    # --- EDT and DB API ---
    edt_create_basic
    edt_dep_slot_handoff
    db_null_control
    db_no_acquire_lazy
    db_no_acquire_wb_first_writer
    db_create_after_dep_asked
    db_create_after_first_touch
    db_first_touch_matrix
)

# The PURGE release policy's grant word, which only a VAL/INV x PURGE build
# has (EXCL arbitrates a word of its own).
if(ARTS_RELEASE_POLICY STREQUAL "PURGE" AND NOT ARTS_COHERENCE_ARM STREQUAL "EXCL")
    list(APPEND TEST_PROGRAMS grant_purge_word_model)
endif()

# Test programs registered in the reference tree only.
set(ARTS_REFERENCE_TREE_PROGRAMS
    stress_edt
    event_scalar_return
    event_payload_policy
    event_chain
    event_channel_lifetime
    multinode_edt
    multinode_event_types
    multinode_stdout_forward
    edt_create_oversize_guard
    finish_event_stress
    term_shutdown
    term_abort
    edt_satisfy_out_of_range
    edt_satisfy_over_count
    term_sigfpe
    term_sigsegv
    config_ports_contract
    config_flux_setup
    port_block_relocation
    regpool_basic
    regpool_node_avail
    regpool_lazy_carve
    regpool_ladder
    regpool_fallback_order
    regpool_spill_cycle
    regpool_spill_target
    regpool_direct_oversize
)

# Pins an assertion of the runtime (edt.c's remote finish-scope guard), which
# a Debug build checks and a Release build compiles out: built and registered
# only in a Debug tree.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND ARTS_REFERENCE_TREE_PROGRAMS
         edt_create_remote_finish_scope_rejected)
endif()

# Driven by the pool's injected exhaustion (ARTS_REGPOOL_FORCE_FULL_NODES and
# arts_regpool_set_forced_full), which only ARTS_DEBUG_INJECTION compiles.
# regpool_direct_oversize compiles its full-node legs only there and runs the
# rest in every mimalloc reference tree.
set(ARTS_REGPOOL_INJECTION_TESTS regpool_evict_retired regpool_evict_wait
    regpool_lookup_vs_rewrite)
if(ARTS_DEBUG_INJECTION)
    list(APPEND ARTS_REFERENCE_TREE_PROGRAMS ${ARTS_REGPOOL_INJECTION_TESTS})
endif()

if(ARTS_TEST_REFERENCE_TREE)
    list(APPEND TEST_PROGRAMS ${ARTS_REFERENCE_TREE_PROGRAMS})
endif()

# Judged by the teardown quiescence walk, which exists only where
# ARTS_LOG_LEVEL compiles DEBUG messages: built and registered only there.
if(ARTS_LOG_LEVEL GREATER_EQUAL 3)
    list(APPEND TEST_PROGRAMS quiescent_shutdown_no_residue)
endif()

# FLUSH is correct only for DB-WRF programs, so a program with a block whose
# write acquisitions no happens-before orders is neither built nor registered
# in the FLUSH tree: those of TEST_PROGRAMS by this list, the rest by the
# guard around their own add_arts_test.
set(ARTS_FLUSH_EXCLUDED
    coherence_stress_single_node
    coherence_satisfy_before_release
    coherence_multi_writer_multi_db
    coherence_multi_writer_dist
    coherence_owner_ownership_reorder
    coherence_writer_ro_overlap
    edt_create_basic
    event_channel_fifo
    db_create_after_dep_asked
    grant_purge_churn
    grant_purge_handback_regrant
    quiescent_shutdown_no_residue)
if(ARTS_COHERENCE_ARM STREQUAL "FLUSH")
    list(REMOVE_ITEM TEST_PROGRAMS ${ARTS_FLUSH_EXCLUDED})
endif()

# Whitebox tests of the INLINE payload layout.  Where a block's bytes live in
# the CXL store and an EDT works in its slot, a buffer descriptor refers to
# storage it does not own: the payload is not inline after the header, no
# payload is ever published into it, and one descriptor serves the block for
# its whole life.  These programs assert that representation itself, so in
# such a tree they describe something that is not built -- a structural
# exclusion, like an application that cannot run multinode, never a masked
# failure.  buffer_external_payload is the descriptor's own test there.
#
# The two CXL trees are one compile-time switch (ARTS_CXL_TREE_RESIDENCY)
# apart: the slot rule, the wire, the arbiter actions, the funnel positions,
# the zero-size exemption, the two edge flushes and the two tallies are
# identical on both; the only differences are residency itself -- the two
# body definitions and the buffer representation -- and the test surface that
# describes that representation:
#
#   Test                                 | STAGED tree | DIRECT tree              | Why
#   ------------------------------------ | ----------- | ------------------------ | ---
#   the names in ARTS_CXL_DIRECT_EXCLUDED | registered | excluded at registration | they assert the inline representation
#   buffer_external_payload              | no          | runs                     | the external descriptor's own contract
#   cxl_slot_discard_unadopt             | no          | 1n_20w                   | the withdrawal it asserts exists only where a descriptor adopts the slot
#   cxl_direct_payload_identity          | no          | 1n_20w/2n_9w_1p/4n_4w_1p | false by construction under STAGED
#   buffer_outlives_cache                | runs        | no                       | the late drop it asserts is a buffer's, and the slot is not one
#   everything else                      | registered  | registered               | one behaviour, two representations
set(ARTS_TESTS_INLINE_PAYLOAD_WHITEBOX
    buffer_payload_roundtrip
    buffer_zero_size
    buffer_version_guard
    buffer_stub_db_size_learn
    buffer_destroy_vs_acquire
    mark_edt_ready_idempotent)
set(ARTS_CXL_DIRECT_EXCLUDED "")
if(ARTS_CXL_TREE_RESIDENCY STREQUAL "DIRECT")
    set(ARTS_CXL_DIRECT_EXCLUDED ${ARTS_TESTS_INLINE_PAYLOAD_WHITEBOX})
    list(REMOVE_ITEM TEST_PROGRAMS ${ARTS_CXL_DIRECT_EXCLUDED})
    message(STATUS "ARTS: excluding ${ARTS_CXL_DIRECT_EXCLUDED} — whitebox tests of the inline payload layout")
    list(APPEND TEST_PROGRAMS cxl_slot_discard_unadopt)
    list(APPEND TEST_PROGRAMS cxl_direct_payload_identity)
endif()

# Portable OCR programs that assert coherence alone: arts.h and nothing else,
# so every arm builds and runs them and they are the same oracle everywhere.
list(APPEND TEST_PROGRAMS cxl_ro_cohort)
list(APPEND TEST_PROGRAMS cxl_ro_turn_overlap)

# The CXL store: registered only where this tree's own library keeps payloads
# in it, so an unguarded entry never has to compile against a tree with no
# store symbols at all.
if(ARTS_CXL_TREE_RESIDENCY)
    list(APPEND TEST_PROGRAMS cxl_store_alloc)
    list(APPEND TEST_PROGRAMS cxl_slot_rule)
    list(APPEND TEST_PROGRAMS cxl_creator_roster)
    if(ARTS_LOG_LEVEL GREATER_EQUAL 3)
        list(APPEND TEST_PROGRAMS cxl_quiescence_marker)
    endif()
    list(APPEND TEST_PROGRAMS cxl_no_acquire_first_use)
    list(APPEND TEST_PROGRAMS cxl_labeled_create)
    list(APPEND TEST_PROGRAMS cxl_home_turn_costs)
endif()

# Function to create a test executable.  RUNTIME names the runtime library it
# links, arts_static unless a test needs a twin of it.
function(add_arts_test test_name)
    cmake_parse_arguments(ARG "" "RUNTIME" "" ${ARGN})
    if(NOT ARG_RUNTIME)
        set(ARG_RUNTIME arts_static)
    endif()
    # Sources live under one of the ocr/ unit/ subdirectories.
    #   ocr/  — portable OCR programs (arts_rt main, no globals, no protocol
    #           #if; valid under any coherence protocol)
    #   unit/ — whitebox tests (internal headers, global state, or protocol-#if
    #           guards; exercise ARTS-internal mechanisms)
    set(_src "")
    foreach(_sub ocr unit)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_sub}/${test_name}.c)
            set(_src ${_sub}/${test_name}.c)
        endif()
    endforeach()
    add_executable(${test_name} ${_src})

    # Set include directories
    target_include_directories(${test_name} PRIVATE ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR} ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})

    # Whitebox tests include protocol-sensitive internal headers (ooo.h,
    # coherence/types.h, protocol.h) that require exactly one compile-time
    # coherence-protocol selection.  The selection is applied per-target (not
    # globally) so it cannot collide with the per-variant benchmark libraries;
    # apply this build's option-selected protocol to the test the same way the
    # ARTS library targets receive it.
    arts_apply_protocol(${test_name} ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})

    # Non-PIE, like every program that runs on ARTS
    # (ARTS_NON_PIE_COMPILE_OPTIONS).
    set_property(TARGET ${test_name} PROPERTY POSITION_INDEPENDENT_CODE OFF)
    target_compile_options(${test_name} PRIVATE ${ARTS_NON_PIE_COMPILE_OPTIONS})
    target_link_options(${test_name} PRIVATE ${ARTS_NON_PIE_LINK_OPTIONS})

    # Link with runtime libraries
    target_link_libraries(${test_name} PRIVATE Threads::Threads ${RT_LIBRARY} ${M_LIBRARY})

    # Tests link the static archive, like the benchmarks: the runtime under
    # test is the build tree's by construction, with no dynamic resolution
    # point an environment (LD_LIBRARY_PATH) could redirect to another copy
    # at run time.
    target_link_libraries(${test_name} PRIVATE ${ARG_RUNTIME})
endfunction()

# Create test executables
foreach(test_program ${TEST_PROGRAMS})
    add_arts_test(${test_program})
endforeach()

# CUDA test programs (only built if CUDA is available)
if(BUILD_CUDA_LIBRARY)
    set(CUDA_TEST_PROGRAMS
        gpu_stream
        gpu_for_all
        lc_sync
        gpu_lc_red
        gpu_lib
        gpu_ring
        gpu_edt_basic
        gpu_edt_dep
        gpu_edt_with_guid
        gpu_edt_passthrough
        gpu_memory
        gpu_memset
        gpu_db
        gpu_lc_sync_basic
        gpu_multi_kernel
    )

    # Function to create a CUDA test executable
    function(add_cuda_test test_name)
        add_executable(${test_name} ${test_name}.cu)

        # Set include directories
        target_include_directories(${test_name} PRIVATE ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR} ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})

        # Protocol-sensitive internal headers need the compile-time coherence
        # selection applied per-target (see add_arts_test).
        arts_apply_protocol(${test_name} ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})

        set_property(TARGET ${test_name} PROPERTY POSITION_INDEPENDENT_CODE OFF)
        target_compile_options(${test_name} PRIVATE ${ARTS_NON_PIE_COMPILE_OPTIONS})
        target_link_options(${test_name} PRIVATE ${ARTS_NON_PIE_LINK_OPTIONS})

        # Link with CUDA libraries
        target_link_libraries(${test_name} PRIVATE
            Threads::Threads
            ${RT_LIBRARY}
            ${M_LIBRARY}
            CUDA::cudart
            CUDA::cuda_driver
            CUDA::cublas
        )

        # Same static-only rule as the C tests.
        if(TARGET arts_cuda_static)
            target_link_libraries(${test_name} PRIVATE arts_cuda_static)
        else()
            message(FATAL_ERROR "No CUDA ARTS library target found")
        endif()
    endfunction()

    # Create CUDA test executables
    foreach(cuda_test ${CUDA_TEST_PROGRAMS})
        add_cuda_test(${cuda_test})
    endforeach()

    message(STATUS "CUDA test programs will be built")
else()
    message(STATUS "CUDA test programs will not be built (CUDA not available)")
endif()

###############################################################################
# CTest registration
#
# ARTS runtime occupies the entire process (binds ports, spawns worker threads).
# RESOURCE_LOCK ensures only one test runs at a time, even with ctest -j.
###############################################################################

# --- Helper functions ---

# A local multinode run claims its own port block: before spawning the peers
# the runtime bind-probes a pid-seeded block, slides past whatever holds it, and
# hands the result down.  The suites hand out no port base: a straggler, an
# orphan or an unrelated process holding a port is absorbed by the runtime's
# probe instead of failing the next test.  A fixed base here would be state
# shared between tests, so one test's leftovers would fail the next.

# Where a test's ARTS_CONFIG comes from.  ONE variable, used by every
# helper and by every hand-registered test -- a second spelling is how five
# tests end up on the wrong root.  CACHE INTERNAL, not a plain set(): the
# helpers below are called from outside this directory too (benchmarks/ocr_shim
# registers its tests through the same functions), and a plain directory-scoped
# variable is invisible there -- every call would silently resolve against an
# empty prefix instead of refusing to configure.
set(ARTS_TEST_CFG_DIR "${CMAKE_SOURCE_DIR}/configs/local/test"
    CACHE INTERNAL "cfg root every register_*_test call resolves ARTS_CONFIG against" FORCE)

# Resolves one variant name to the cfg a test should run.  Every site that
# names a cfg calls this; none spells a directory.
function(arts_test_cfg out variant)
    set(${out} "${ARTS_TEST_CFG_DIR}/${variant}.cfg" PARENT_SCOPE)
endfunction()

# Every program test runs under run_test.cmake, which judges it by how it
# ends: its exit status must be STATUS (0 unless given), and its output must
# match the pass regex the test sets, if any.  CTest itself ignores the exit
# status of a test it gives a pass regex, so arts_test_finalize hands that
# regex to the runner.  The helpers below register through this one;
# CACHE INTERNAL because they are called from other directories too.  A
# COMMAND element is one argument: a ';' inside one splits it in two.
set(ARTS_TEST_RUNNER "${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_test.cmake"
    CACHE INTERNAL "the script every program test runs under" FORCE)

function(arts_add_test test_name)
    cmake_parse_arguments(ARG "" "STATUS" "COMMAND" ${ARGN})
    if(NOT DEFINED ARG_STATUS)
        set(ARG_STATUS 0)
    endif()
    list(POP_FRONT ARG_COMMAND _program)
    if(TARGET ${_program})
        set(_program $<TARGET_FILE:${_program}>)
    endif()
    add_test(
        NAME ${test_name}
        COMMAND ${CMAKE_COMMAND} -DSTATUS=${ARG_STATUS} -P ${ARTS_TEST_RUNNER}
                -- ${_program} ${ARG_COMMAND}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(${test_name} PROPERTIES ARTS_TEST_RUN TRUE)
    # The first registration in a directory schedules its finalize for the
    # directory's end, so every test it registers, by any route, is covered.
    get_property(_scheduled DIRECTORY PROPERTY ARTS_TEST_FINALIZE_SCHEDULED)
    if(NOT _scheduled)
        set_property(DIRECTORY PROPERTY ARTS_TEST_FINALIZE_SCHEDULED TRUE)
        cmake_language(DEFER CALL arts_test_finalize)
    endif()
endfunction()

function(register_single_node_test test_name)
    cmake_parse_arguments(ARG "" "TIMEOUT;STATUS" "ARGS" ${ARGN})
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 10)
    endif()
    if(NOT DEFINED ARG_STATUS)
        set(ARG_STATUS 0)
    endif()
    arts_test_cfg(_cfg 1n_20w)
    arts_add_test(${test_name} STATUS ${ARG_STATUS}
                  COMMAND ${test_name} ${ARG_ARGS})
    set_tests_properties(${test_name} PROPERTIES
        LABELS "single_node"
        TIMEOUT ${ARG_TIMEOUT}
        RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg}"
    )
endfunction()

# The three local multinode flavours.  A variant name is the cfg's file name in
# configs/local/test/ (<nodes>n_<workers>w[_<progress>p]); each test's
# ARTS_CONFIG env points straight at the source cfg (no copy), so ctest runs
# standalone.
#   2n_9w_1p — 2 nodes, 9 workers + 1 progress thread per node
#   4n_4w_1p — 4 nodes, 4 workers + 1 progress thread per node
#   2n_8w_2p — 2 nodes, 8 workers + 2 progress threads per node
#              (multiple progress threads break per-peer message FIFO,
#              exercising concurrent cross-rank handler dispatch)
set(ARTS_TEST_MULTINODE_VARIANTS 2n_9w_1p 4n_4w_1p 2n_8w_2p
    CACHE INTERNAL "the multinode variants a test registers unless it names its own" FORCE)

function(register_multinode_test test_name)
    cmake_parse_arguments(ARG "" "TIMEOUT;STATUS" "ARGS;VARIANTS" ${ARGN})
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 60)
    endif()
    if(NOT ARG_VARIANTS)
        set(ARG_VARIANTS ${ARTS_TEST_MULTINODE_VARIANTS})
    endif()
    if(NOT DEFINED ARG_STATUS)
        set(ARG_STATUS 0)
    endif()
    foreach(variant IN LISTS ARG_VARIANTS)
        arts_test_cfg(_cfg ${variant})
        arts_add_test(${test_name}_${variant} STATUS ${ARG_STATUS}
                      COMMAND ${test_name} ${ARG_ARGS})
        set_tests_properties(${test_name}_${variant} PROPERTIES
            LABELS "multinode"
            TIMEOUT ${ARG_TIMEOUT}
            RESOURCE_LOCK "arts_runtime"
            ENVIRONMENT "ARTS_CONFIG=${_cfg}"
        )
    endforeach()
endfunction()

# Every test of a CXL tree links the library, and the library reads its
# environment when it loads, before the runtime parses a cfg:
#   - ARTS_FLUSH_LOG: the vendored fake library dumps a flush trace at exit into
#     the working directory unless this names another path, and a test's
#     working directory is the build tree, so the trace goes nowhere.
#   - ARTS_FAKE_CXL_REGION_SIZE (fake library only): the size of the one region
#     it maps, which must hold the build-time arena plus the library's own
#     structures.  One value for the tree (ARTS_CXL_TEST_REGION_BYTES, derived
#     from ARTS_CXL_DB_ARENA_SIZE_BYTES below); the region is sparse, so a
#     test pays only for the pages it touches.
# The region has one name and one address per host, so two runs on one host
# must not overlap: RESOURCE_LOCK serializes one ctest's tests, and the
# one-runner rule covers everything else.  A test's properties can be set
# only from the directory that added it, so arts_test_finalize applies this
# at the end of each directory that registers tests.
# One data-block arena (the fake library reports one device), and headroom for
# the arena table, its lock and the library's own structures.
math(EXPR ARTS_CXL_TEST_REGION_BYTES "${ARTS_CXL_DB_ARENA_SIZE_BYTES} + 134217728")
set(ARTS_CXL_TEST_REGION_BYTES "${ARTS_CXL_TEST_REGION_BYTES}" CACHE INTERNAL
    "ARTS_FAKE_CXL_REGION_SIZE every test of a fake-library CXL tree runs under" FORCE)
function(arts_cxl_test_env)
    if(NOT ARTS_CXL_TREE_RESIDENCY)
        return()
    endif()
    get_property(_tests DIRECTORY PROPERTY TESTS)
    if(NOT _tests)
        return()
    endif()
    set_property(TEST ${_tests} APPEND PROPERTY ENVIRONMENT "ARTS_FLUSH_LOG=/dev/null")
    if(NOT ARTS_CXL_REAL)
        set_property(TEST ${_tests} APPEND PROPERTY ENVIRONMENT
                     "ARTS_FAKE_CXL_REGION_SIZE=${ARTS_CXL_TEST_REGION_BYTES}")
    endif()
    # Every test of a CXL tree loads the fake library, whose constructor maps
    # and may own the host's one shm region: no two may run at once, pure-unit
    # ones included.
    set_property(TEST ${_tests} APPEND PROPERTY RESOURCE_LOCK "arts_runtime")
endfunction()

# An ADDRESS sanitizer tree runs every test, and every rank a test spawns,
# under the one leak suppression of lsan_suppressions.txt: memory a vendored
# library leaks in a frame of its own that nothing outside it can reach.
function(arts_lsan_test_env)
    if(NOT ARTS_DEBUG_SANITIZER STREQUAL "ADDRESS")
        return()
    endif()
    get_property(_tests DIRECTORY PROPERTY TESTS)
    if(NOT _tests)
        return()
    endif()
    set_property(TEST ${_tests} APPEND PROPERTY ENVIRONMENT
                 "LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/lsan_suppressions.txt")
endfunction()

# A ThreadSanitizer tree runs the multinode variants of each audited test of
# tests/tsan/residue_tests.cmake, in the cells whose arm reaches that test's
# residue shapes, with the suppressions of those shapes: residue_send.supp when
# every shape is a send-side copy of the provider (R1, R2, R4), and
# residue_send_land.supp, which adds its receive-side copies, when a shape is a
# landing (R3, R5).  An exclusion arm has no residue.  Every other test keeps
# the provider's frames visible.  The suppressions are appended to the
# caller's TSAN_OPTIONS, and the launcher's ranks inherit the test's
# environment.
#
# The arm of a cell, and the shapes it can reach: write-back serves a read
# from the owner (R1); write-through also publishes from a writer's rank (R2)
# and lands in the home's buffer (R3); FLUSH fetches the home's line (R4) and
# writes it back in place (R5).
function(arts_tsan_residue_arm out_arm out_shapes cell)
    if(cell MATCHES "^(VAL|INV)/WB/RETAIN$")
        set(${out_arm} WB PARENT_SCOPE)
        set(${out_shapes} R1 PARENT_SCOPE)
    elseif(cell MATCHES "^(VAL|INV)/WT/(RETAIN|PURGE)$")
        set(${out_arm} WT PARENT_SCOPE)
        set(${out_shapes} R1 R2 R3 PARENT_SCOPE)
    elseif(cell STREQUAL "FLUSH")
        set(${out_arm} FLUSH PARENT_SCOPE)
        set(${out_shapes} R4 R5 PARENT_SCOPE)
    else()
        set(${out_arm} "" PARENT_SCOPE)
        set(${out_shapes} "" PARENT_SCOPE)
    endif()
endfunction()

function(arts_tsan_residue test_name)
    cmake_parse_arguments(ARG "" "" "CELLS;SHAPES;VARIANTS" ${ARGN})
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "arts_tsan_residue(${test_name}): unexpected arguments "
                "'${ARG_UNPARSED_ARGUMENTS}'")
    endif()
    if(NOT ARG_CELLS OR NOT ARG_SHAPES)
        message(FATAL_ERROR "arts_tsan_residue(${test_name}): give its CELLS and SHAPES")
    endif()
    foreach(_c IN LISTS ARG_CELLS)
        arts_tsan_residue_arm(_arm _reachable "${_c}")
        if(NOT _arm)
            message(FATAL_ERROR "arts_tsan_residue(${test_name}): '${_c}' is not a cell "
                    "whose arm has a residue")
        endif()
        foreach(_s IN LISTS ARG_SHAPES)
            if(NOT _s IN_LIST _reachable)
                message(FATAL_ERROR "arts_tsan_residue(${test_name}): shape '${_s}' cannot "
                        "occur in ${_c} (its arm reaches ${_reachable})")
            endif()
        endforeach()
    endforeach()
    if(NOT ARG_VARIANTS)
        set(ARG_VARIANTS ${ARTS_TEST_MULTINODE_VARIANTS})
    endif()
    foreach(_v IN LISTS ARG_VARIANTS)
        if(NOT _v IN_LIST ARTS_TEST_MULTINODE_VARIANTS)
            message(FATAL_ERROR "arts_tsan_residue(${test_name}): '${_v}' is not a multinode "
                    "variant (${ARTS_TEST_MULTINODE_VARIANTS})")
        endif()
    endforeach()
    list(JOIN ARG_CELLS "," _cells)
    list(JOIN ARG_SHAPES "," _shapes)
    list(JOIN ARG_VARIANTS "|" _variants)
    set_property(GLOBAL APPEND PROPERTY ARTS_TSAN_RESIDUE_ENTRIES
                 "${test_name}:${_cells}:${_shapes}:${_variants}")
endfunction()
include(${CMAKE_CURRENT_SOURCE_DIR}/tsan/residue_tests.cmake)

function(arts_tsan_residue_test_env)
    if(NOT ARTS_DEBUG_SANITIZER STREQUAL "THREAD")
        return()
    endif()
    if(ARTS_COHERENCE_ARM STREQUAL "FLUSH")
        set(_cell FLUSH)
    else()
        set(_cell "${ARTS_COHERENCE_ARM}/${ARTS_WRITE_POLICY}/${ARTS_RELEASE_POLICY}")
    endif()
    get_property(_tests DIRECTORY PROPERTY TESTS)
    get_property(_entries GLOBAL PROPERTY ARTS_TSAN_RESIDUE_ENTRIES)
    foreach(_e IN LISTS _entries)
        string(REPLACE ":" ";" _f "${_e}")
        list(GET _f 0 _name)
        list(GET _f 1 _cells)
        list(GET _f 2 _shapes)
        list(GET _f 3 _variants)
        string(REPLACE "," ";" _cells "${_cells}")
        string(REPLACE "," ";" _shapes "${_shapes}")
        if(NOT _cell IN_LIST _cells)
            continue()
        endif()
        if("R3" IN_LIST _shapes OR "R5" IN_LIST _shapes)
            set(_supp "${CMAKE_SOURCE_DIR}/tests/tsan/residue_send_land.supp")
        else()
            set(_supp "${CMAKE_SOURCE_DIR}/tests/tsan/residue_send.supp")
        endif()
        foreach(_t IN LISTS _tests)
            if(_t MATCHES "^${_name}_(${_variants})(_fuzz)?$")
                set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT_MODIFICATION
                             "TSAN_OPTIONS=string_append::suppressions=${_supp}")
            endif()
        endforeach()
    endforeach()
endfunction()

# Run at the end of each directory that registers tests (arts_add_test
# schedules it).  A test the runner judges has its pass regex moved into the
# runner, since CTest would otherwise ignore its exit status; one without a
# regex is given an empty one, so a value in the caller's environment never
# applies.  Every test then has two kinds of output matched as well, being
# failures its own exit status may not carry: a sanitizer's report -- UBSan's
# leaves the status unchanged, and a forked child's reaches only its parent --
# and the local launcher's verdict on a rank it spawned, which reaches the
# master's status only where the test passes arts_rt's result on: a rank that
# died on a signal always, one that exited non-zero unless the status is the
# one the test sets in ARTS_EXPECTS_RANK_FAILURE, a rank's exit it provokes,
# and one that did not exit after the run ended, which the launcher stops
# without counting it failed.  That last verdict is a warning printed once the
# launcher's grace period has passed: a tree that compiles warnings out never
# prints it, and a rank still escaping the waits it was blocked in at the
# shutdown (each escape re-checks once a second) has only a few seconds of
# that period to spare.  WILL_FAIL is refused: CTest would invert the
# verdict, a late failure's included; an expected exit status is the test's
# STATUS.  A CXL tree's tests get the store's environment too, an ADDRESS
# sanitizer tree's the leak suppression, and a THREAD tree's audited tests
# their arm's residue suppressions.
function(arts_test_finalize)
    arts_cxl_test_env()
    arts_lsan_test_env()
    arts_tsan_residue_test_env()
    get_property(_tests DIRECTORY PROPERTY TESTS)
    foreach(_t IN LISTS _tests)
        get_test_property(${_t} WILL_FAIL _will_fail)
        if(_will_fail)
            message(FATAL_ERROR "${_t}: WILL_FAIL inverts every verdict, a "
                    "late failure's included; give the expected exit status "
                    "as the test's STATUS instead")
        endif()
        get_test_property(${_t} ARTS_TEST_RUN _run)
        get_test_property(${_t} PASS_REGULAR_EXPRESSION _pass)
        if(_run)
            if(_pass STREQUAL "NOTFOUND")
                set(_pass "")
            endif()
            list(REMOVE_ITEM _pass "")
            if(_pass)
                list(JOIN _pass "|" _pass)
            endif()
            set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT
                         "ARTS_TEST_PASS_REGEX=${_pass}")
            set_property(TEST ${_t} PROPERTY PASS_REGULAR_EXPRESSION)
        endif()
        get_test_property(${_t} FAIL_REGULAR_EXPRESSION _fail)
        if(_fail STREQUAL "NOTFOUND")
            set(_fail "")
        endif()
        list(APPEND _fail "ERROR: (Address|Leak)Sanitizer"
             "WARNING: ThreadSanitizer" ": runtime error: "
             "Local launcher: rank [0-9]+ \\(pid [0-9]+\\) died on signal"
             "Local launcher: rank [0-9]+ \\(pid [0-9]+\\) did not exit within")
        get_test_property(${_t} ARTS_EXPECTS_RANK_FAILURE _expects)
        if(NOT _expects STREQUAL "NOTFOUND" AND
           NOT _expects MATCHES "^([1-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5])$")
            message(FATAL_ERROR "${_t}: ARTS_EXPECTS_RANK_FAILURE names the exit "
                    "status a rank is provoked into (1-255), not '${_expects}'")
        endif()
        if(_expects STREQUAL "NOTFOUND")
            list(APPEND _fail
                 "Local launcher: rank [0-9]+ \\(pid [0-9]+\\) exited with status")
        else()
            set(_others "")
            foreach(_s RANGE 0 255)
                if(NOT _s EQUAL _expects)
                    list(APPEND _others ${_s})
                endif()
            endforeach()
            list(JOIN _others "|" _others)
            list(APPEND _fail
                 "Local launcher: rank [0-9]+ \\(pid [0-9]+\\) exited with status (${_others})([^0-9]|$)")
        endif()
        set_tests_properties(${_t} PROPERTIES FAIL_REGULAR_EXPRESSION "${_fail}")
    endforeach()
endfunction()

# The store-backed arm's own entries.  They DELEGATE to
# register_single_node_test / register_multinode_test rather than resolving a
# cfg of their own: ARTS_TEST_CFG_DIR is the one root and arts_test_cfg is the
# one resolver, so these tests run in the mode the rest of the suite runs in
# instead of being the set a second spelling leaves behind.  What they add is
# a second verdict channel -- the FAIL token their failure branches print,
# which fails the run even where a rank's status does not reach the master --
# and a longer default timeout for one-rank runs.
function(register_cxl_single_node_test test_name)
    cmake_parse_arguments(ARG "" "TIMEOUT;FAIL_REGEX" "" ${ARGN})
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 30)
    endif()
    if(NOT ARG_FAIL_REGEX)
        set(ARG_FAIL_REGEX "FAIL")
    endif()
    register_single_node_test(${test_name} TIMEOUT ${ARG_TIMEOUT})
    set_tests_properties(${test_name} PROPERTIES
        FAIL_REGULAR_EXPRESSION "${ARG_FAIL_REGEX}")
endfunction()

function(register_cxl_multinode_test test_name)
    cmake_parse_arguments(ARG "" "TIMEOUT;FAIL_REGEX" "VARIANTS" ${ARGN})
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 60)
    endif()
    if(NOT ARG_FAIL_REGEX)
        set(ARG_FAIL_REGEX "FAIL")
    endif()
    if(NOT ARG_VARIANTS)
        set(ARG_VARIANTS 2n_9w_1p 4n_4w_1p 2n_8w_2p)
    endif()
    register_multinode_test(${test_name} TIMEOUT ${ARG_TIMEOUT}
                            VARIANTS ${ARG_VARIANTS})
    foreach(variant IN LISTS ARG_VARIANTS)
        set_tests_properties(${test_name}_${variant} PROPERTIES
            FAIL_REGULAR_EXPRESSION "${ARG_FAIL_REGEX}")
    endforeach()
endfunction()

function(register_gpu_test test_name)
    if(ARTS_CXL_TREE_RESIDENCY)
        # A GPU CXL tree is not a configuration this suite validates.
        # Excluded at registration, not skipped at run time.
        message(STATUS "Skipping GPU test ${test_name}: a CXL tree has no GPU cfg")
        return()
    endif()
    cmake_parse_arguments(ARG "" "TIMEOUT" "ARGS" ${ARGN})
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 10)
    endif()
    arts_test_cfg(_cfg 1n_20w)
    arts_add_test(${test_name} COMMAND ${test_name} ${ARG_ARGS})
    set_tests_properties(${test_name} PROPERTIES
        LABELS "gpu"
        TIMEOUT ${ARG_TIMEOUT}
        RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg};gpu=1;ASAN_OPTIONS=protect_shadow_gap=0:detect_leaks=0:alloc_dealloc_mismatch=0"
    )
endfunction()

# Pure unit tests: no ARTS runtime started, use separate resource lock.
function(register_pure_unit_test test_name)
    cmake_parse_arguments(ARG "" "TIMEOUT" "" ${ARGN})
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 30)
    endif()
    arts_add_test(${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES
        LABELS "single_node"
        TIMEOUT ${ARG_TIMEOUT}
        RESOURCE_LOCK "pure_unit_test"
    )
endfunction()

# Raise or lower one target's log level off the build-wide default; the
# top-level ARTS_LOG_LEVEL definition reads this property back per target.
function(set_test_log_level test_name level)
    set_target_properties(${test_name} PROPERTIES ARTS_LOG_LEVEL ${level})
endfunction()

# --- Single-node tests ---

# Its subtests record into one block with no order among them, which DB-WRF
# does not admit.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_single_node_test(edt_create_basic)
endif()

register_single_node_test(db_no_acquire_lazy TIMEOUT 30)
register_single_node_test(db_no_acquire_wb_first_writer TIMEOUT 30)
set_tests_properties(db_no_acquire_wb_first_writer PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL")
register_single_node_test(db_null_control TIMEOUT 15)

# Its consumers record into one block with no order among them, which DB-WRF
# does not admit.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_single_node_test(event_channel_fifo TIMEOUT 60)
endif()

register_single_node_test(coherence_rwro_phase)
# edt_dep_slot_handoff: the slots an EDT's acquire phase resolves reach its
# body with their payload, and a slot it does not acquire keeps its satisfy's
# value; on two ranks the read resolves on a wake, and two progress threads
# let that wake run beside the walk.
register_single_node_test(edt_dep_slot_handoff TIMEOUT 30)
register_multinode_test(edt_dep_slot_handoff TIMEOUT 60
                        VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_t edt_dep_slot_handoff edt_dep_slot_handoff_2n_9w_1p
        edt_dep_slot_handoff_2n_8w_2p)
    set_tests_properties(${_t} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: edt_dep_slot_handoff"
        FAIL_REGULAR_EXPRESSION "FAIL:")
endforeach()

if(ARTS_TEST_REFERENCE_TREE)
register_single_node_test(stress_edt TIMEOUT 30)
register_single_node_test(event_scalar_return TIMEOUT 30)
register_single_node_test(event_payload_policy TIMEOUT 15)
register_single_node_test(event_chain)
register_single_node_test(event_channel_lifetime)

# term_shutdown's own run is the runtime's ordinary shutdown, which every
# other test ends with; the program hosts the hook refusals below.
#
# A Debug tree without the injection hooks refuses a run that sets any
# hook's variable, rather than running as if it had exercised the hook.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ARTS_DEBUG_INJECTION)
    arts_test_cfg(_cfg 1n_20w)
    foreach(_hook
            ARTS_SCHED_FUZZ
            ARTS_WRITE_COMPLETION_DELAY_MS
            ARTS_NET_MAX_MSG
            ARTS_REGPOOL_FORCE_FULL_NODES
            ARTS_REQUEST_DELAY_MS)
        arts_add_test(injection_hook_refused_${_hook} STATUS 1
                      COMMAND term_shutdown)
        set_tests_properties(injection_hook_refused_${_hook} PROPERTIES
            LABELS "single_node"
            TIMEOUT 30
            RESOURCE_LOCK "arts_runtime"
            ENVIRONMENT "ARTS_CONFIG=${_cfg};${_hook}=1"
            PASS_REGULAR_EXPRESSION
            "${_hook} is set, but this build compiles no injection hooks")
    endforeach()
endif()
# Where the hooks are compiled, a value a hook cannot use stops the run
# rather than leaving the hook off.  The provider ceiling and the two delays
# are read when the fabric comes up, so those three run on several ranks.
if(ARTS_DEBUG_INJECTION)
    arts_test_cfg(_cfg 1n_20w)
    arts_test_cfg(_mn_cfg 2n_9w_1p)
    arts_add_test(injection_value_refused_sched_fuzz STATUS 1
                  COMMAND term_shutdown)
    set_tests_properties(injection_value_refused_sched_fuzz PROPERTIES
        LABELS "single_node"
        TIMEOUT 30
        RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg};ARTS_SCHED_FUZZ=fast"
        PASS_REGULAR_EXPRESSION "is not a percentage")
    arts_add_test(injection_value_refused_net_max_msg STATUS 1
                  COMMAND multinode_edt)
    set_tests_properties(injection_value_refused_net_max_msg PROPERTIES
        LABELS "multinode"
        TIMEOUT 30
        RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_mn_cfg};ARTS_NET_MAX_MSG=0"
        PASS_REGULAR_EXPRESSION "is not a positive byte count")
    arts_add_test(injection_value_refused_write_delay STATUS 1
                  COMMAND multinode_edt)
    set_tests_properties(injection_value_refused_write_delay PROPERTIES
        LABELS "multinode"
        TIMEOUT 30
        RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_mn_cfg};ARTS_WRITE_COMPLETION_DELAY_MS=soon"
        PASS_REGULAR_EXPRESSION "is not a delay")
    arts_add_test(injection_value_refused_request_delay STATUS 1
                  COMMAND multinode_edt)
    set_tests_properties(injection_value_refused_request_delay PROPERTIES
        LABELS "multinode"
        TIMEOUT 30
        RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_mn_cfg};ARTS_REQUEST_DELAY_MS=5:rw"
        PASS_REGULAR_EXPRESSION
        "ARTS_REQUEST_DELAY_MS=\"5:rw\" is not a delay of 0-60000 ms")
    # A one-rank run brings no fabric up, so it refuses a fabric hook.
    foreach(_hook
            ARTS_NET_MAX_MSG
            ARTS_WRITE_COMPLETION_DELAY_MS
            ARTS_REQUEST_DELAY_MS)
        arts_add_test(injection_fabric_hook_refused_${_hook} STATUS 1
                      COMMAND term_shutdown)
        set_tests_properties(injection_fabric_hook_refused_${_hook} PROPERTIES
            LABELS "single_node"
            TIMEOUT 30
            RESOURCE_LOCK "arts_runtime"
            ENVIRONMENT "ARTS_CONFIG=${_cfg};${_hook}=1"
            PASS_REGULAR_EXPRESSION
            "${_hook} is set, but a one-rank run has no fabric")
    endforeach()
    # Only the arena allocator's pool parses the force-full list; the system
    # allocator's refuses any.
    if(ARTS_MALLOC STREQUAL "mimalloc")
        arts_add_test(injection_value_refused_force_full STATUS 1
                      COMMAND term_shutdown)
        set_tests_properties(injection_value_refused_force_full PROPERTIES
            LABELS "single_node"
            TIMEOUT 30
            RESOURCE_LOCK "arts_runtime"
            ENVIRONMENT "ARTS_CONFIG=${_cfg};ARTS_REGPOOL_FORCE_FULL_NODES=x"
            PASS_REGULAR_EXPRESSION "is not a node number")
        arts_add_test(injection_value_refused_force_full_node STATUS 1
                      COMMAND term_shutdown)
        set_tests_properties(injection_value_refused_force_full_node PROPERTIES
            LABELS "single_node"
            TIMEOUT 30
            RESOURCE_LOCK "arts_runtime"
            ENVIRONMENT "ARTS_CONFIG=${_cfg};ARTS_REGPOOL_FORCE_FULL_NODES=64"
            PASS_REGULAR_EXPRESSION "names node 64, but this machine has")
    endif()
endif()

register_single_node_test(term_abort ARGS 0 42 STATUS 42)
set_tests_properties(term_abort PROPERTIES
    PASS_REGULAR_EXPRESSION "TERM_ABORT rc=42")

# A satisfy naming a slot past an EDT's depc is fatal on the rank holding the
# EDT: in the caller's own process on one rank, and on two in rank 1, whose
# exit the master's arts_rt reports.
register_single_node_test(edt_satisfy_out_of_range TIMEOUT 30 STATUS 1)
set_tests_properties(edt_satisfy_out_of_range PROPERTIES
    PASS_REGULAR_EXPRESSION "names slot 5, but the EDT has 2 dependence slot")
register_multinode_test(edt_satisfy_out_of_range TIMEOUT 30 STATUS 1
                        VARIANTS 2n_9w_1p)
set_tests_properties(edt_satisfy_out_of_range_2n_9w_1p PROPERTIES
    PASS_REGULAR_EXPRESSION "names slot 5, but the EDT has 2 dependence slot"
    ARTS_EXPECTS_RANK_FAILURE 1)
# A satisfy of an in-range slot once every dependence is satisfied: fatal on
# the rank that holds the EDT, the same way.
register_single_node_test(edt_satisfy_over_count TIMEOUT 30 STATUS 1)
set_tests_properties(edt_satisfy_over_count PROPERTIES
    PASS_REGULAR_EXPRESSION "slot 1 exceeds its 2 dependence")
register_multinode_test(edt_satisfy_over_count TIMEOUT 30 STATUS 1
                        VARIANTS 2n_9w_1p)
set_tests_properties(edt_satisfy_over_count_2n_9w_1p PROPERTIES
    PASS_REGULAR_EXPRESSION "slot 1 exceeds its 2 dependence"
    ARTS_EXPECTS_RANK_FAILURE 1)

# Debug-only (see the program list's guard above): rank 1's fatal error ends
# that rank, so the master's arts_rt() reports a failed rank and the test
# exits 1, and the error text names the guard that ended it.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    register_multinode_test(edt_create_remote_finish_scope_rejected
        VARIANTS 2n_9w_1p STATUS 1)
    set_tests_properties(edt_create_remote_finish_scope_rejected_2n_9w_1p PROPERTIES
        PASS_REGULAR_EXPRESSION "must be homed on the creating rank"
        ARTS_EXPECTS_RANK_FAILURE 1)
endif()

# term_sigfpe / term_sigsegv crash via signal.  A death by a signal is not an
# exit status the runner can be told to expect, so a bash wrapper turns the
# expected one (128 + the signal's number) into status 0 and any other end
# into a failure, and the crash handler's line is the pass regex.
# Hand-registered, so ARTS_CONFIG has to be set here too: without it the
# runtime falls back to ./arts.cfg in the working directory, which the build
# tree does not contain.
arts_test_cfg(_cfg 1n_20w)
arts_add_test(term_sigfpe COMMAND bash -c
    "ASAN_OPTIONS=handle_sigfpe=0:handle_segv=0 ${CMAKE_CURRENT_BINARY_DIR}/term_sigfpe 2>&1\ntest $? -eq 136")
set_tests_properties(term_sigfpe PROPERTIES
    LABELS "single_node"
    TIMEOUT 10
    RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg}"
    PASS_REGULAR_EXPRESSION "\\[ARTS\\] Crashed: SIGFPE")

arts_test_cfg(_cfg 1n_20w)
arts_add_test(term_sigsegv COMMAND bash -c
    "ASAN_OPTIONS=handle_segv=0 ${CMAKE_CURRENT_BINARY_DIR}/term_sigsegv 2>&1\ntest $? -eq 139")
set_tests_properties(term_sigsegv PROPERTIES
    LABELS "single_node"
    TIMEOUT 10
    RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg}"
    PASS_REGULAR_EXPRESSION "\\[ARTS\\] Crashed: SIGSEGV")
endif()

# --- Pure unit tests (no ARTS runtime) ---
# Exhaustive model check of the PURGE release policy's grant word decider
# (coherence/grant_purge.c).
if(TARGET grant_purge_word_model)
register_pure_unit_test(grant_purge_word_model TIMEOUT 30)
set_tests_properties(grant_purge_word_model PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS grant_purge_word_model")
endif()
# The PURGE home's return-latch hand-off, driven on one thread through the
# real serving tail (the test TU #includes coherence/grant_purge.c and stubs
# its callees, so it links no runtime).
if(ARTS_RELEASE_POLICY STREQUAL "PURGE" AND NOT ARTS_COHERENCE_ARM STREQUAL "EXCL")
    add_executable(grant_purge_latch_handoff unit/grant_purge_latch_handoff.c)
    target_include_directories(grant_purge_latch_handoff PRIVATE
        ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/libs/src/core)
    arts_apply_protocol(grant_purge_latch_handoff ${ARTS_COHERENCE_ARM}
                        ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})
    target_link_libraries(grant_purge_latch_handoff PRIVATE Threads::Threads)
    register_pure_unit_test(grant_purge_latch_handoff TIMEOUT 30)
    set_tests_properties(grant_purge_latch_handoff PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS grant_purge_latch_handoff")
endif()

# --- OoO defer/replay engine (ooo.c) ---
# Standalone like the SYNC_*/deque tests: each test TU #includes the runtime's
# ooo.c so the file-static g_ooo_table + engine bodies are compiled in, links
# shared.c for the cb shared-ptr slot, and supplies counting handler stubs +
# a single-slot arts_route_table_reserve_or_lookup + libc malloc shims (no ARTS
# runtime).  ooo.c pulls the generated counter Preamble.h, hence
# ARTS_BUILD_INTERNAL_INCLUDE_DIR.  The table's DB arms follow the tree's
# protocol, so the completeness check runs in every tree; the engine's drain
# and rescue paths are the same in every tree.
set(OOO_UNIT_TESTS ooo_table_completeness)
if(ARTS_TEST_REFERENCE_TREE)
    list(APPEND OOO_UNIT_TESTS ooo_toctou_rescue ooo_drain_repush)
endif()
foreach(t ${OOO_UNIT_TESTS})
    add_executable(${t} unit/${t}.c
        ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c)
    target_include_directories(${t} PRIVATE
        ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
    # ooo.c (included directly by each test TU) pulls arts/ooo.h which requires
    # exactly one compile-time protocol selection — same reason add_arts_test
    # calls arts_apply_protocol.
    arts_apply_protocol(${t} ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})
    target_link_libraries(${t} PRIVATE Threads::Threads)
endforeach()
register_pure_unit_test(ooo_table_completeness)
if(ARTS_TEST_REFERENCE_TREE)
    register_pure_unit_test(ooo_toctou_rescue TIMEOUT 60)
    register_pure_unit_test(ooo_drain_repush TIMEOUT 60)
endif()

# Everything below up to the end of this section is protocol-independent
# (no data block, or code that carries no protocol branch) and runs in the
# reference tree only.
if(ARTS_TEST_REFERENCE_TREE)
# Forks to probe the ARTS_ERROR death paths of the port contract; loads cfgs
# only, so it binds nothing and needs no runtime.
register_pure_unit_test(config_ports_contract TIMEOUT 30)
set_tests_properties(config_ports_contract PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS config_ports_contract")
# Forks to probe the flux launcher arm's ARTS_ERROR death paths against a
# stub flux CLI the test writes itself; loads cfgs only, binds nothing.
register_pure_unit_test(config_flux_setup TIMEOUT 30)
set_tests_properties(config_flux_setup PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS config_flux_setup")
# Occupies one port of a block, declared through its own bind and by a real
# listener, and checks the selector moves off the block.  It defines bind and
# fopen (the ephemeral port range) and finds the C library's own at run time.
register_pure_unit_test(port_block_relocation TIMEOUT 30)
target_link_libraries(port_block_relocation PRIVATE ${CMAKE_DL_LIBS})
set_tests_properties(port_block_relocation PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS port_block_relocation")

# Programs including unit/regpool_numa_sim.h find the C library's own
# syscall and fopen at run time.
set(_regpool_sim_tests regpool_basic regpool_lazy_carve
    regpool_ladder regpool_fallback_order regpool_spill_cycle
    regpool_spill_target regpool_direct_oversize)
if(ARTS_DEBUG_INJECTION)
    list(APPEND _regpool_sim_tests ${ARTS_REGPOOL_INJECTION_TESTS})
endif()
foreach(_regpool_sim_test IN LISTS _regpool_sim_tests)
    target_link_libraries(${_regpool_sim_test} PRIVATE ${CMAKE_DL_LIBS})
endforeach()

# The pool tests below assert the arena/slab machinery and the confinement
# invariant (every returned pointer resolves through arts_regpool_lookup to a
# registered slab), which only the arena-allocator implementation provides.
# The ARTS_MALLOC=system fallback has no arena/slab concept — it is a plain
# pass-through allocator, so arts_regpool_lookup always misses there by
# design (see regpool.c's non-mimalloc branch).  Those in TEST_PROGRAMS are
# still built under every ARTS_MALLOC config for compile coverage; they run
# only where the invariant they check holds.  Each one runs on a machine it
# describes to the pool (tests/unit/regpool_numa_sim.h) — node count, the
# calling thread's node, each node's free memory, what the machine can still
# give — so it takes the same path and asserts the same thing on every host,
# and none has a SKIP.
if(ARTS_MALLOC STREQUAL "mimalloc")
    register_pure_unit_test(regpool_basic TIMEOUT 60)
    # Lazy carving: exactly one node carved at init, another carved by the
    # first allocation of a thread on it.
    register_pure_unit_test(regpool_lazy_carve TIMEOUT 60)
    set_tests_properties(regpool_lazy_carve PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS regpool_lazy_carve")
    # The slab ladder's exact rungs (a node's next slab is half again the
    # largest one mapped for it, on the allocator's granule).
    register_pure_unit_test(regpool_ladder TIMEOUT 60)
    set_tests_properties(regpool_ladder PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS regpool_ladder")
    # The candidate order itself, against a supplied distance table.
    register_pure_unit_test(regpool_fallback_order TIMEOUT 60)
    set_tests_properties(regpool_fallback_order PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS regpool_fallback_order")
    # Every node full to its own grow and roomy to every other reader, so the
    # node asked in a full node's stead finds itself full too: init and the
    # exhaustion cascade must still grow.
    register_pure_unit_test(regpool_spill_cycle TIMEOUT 60)
    set_tests_properties(regpool_spill_cycle PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS regpool_spill_cycle")
    # What serves a full node's demand, in order: other nodes' arenas, then
    # slabs placed on other nodes (the caller's set first), then a slab the
    # kernel places.
    register_pure_unit_test(regpool_spill_target TIMEOUT 60)
    set_tests_properties(regpool_spill_target PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS regpool_spill_target")
    # Oversize requests take mappings of their own, retired at free and
    # handed back to the same shape; where the injected exhaustion is
    # compiled, a full node moves their placement.
    register_pure_unit_test(regpool_direct_oversize TIMEOUT 60)
    set_tests_properties(regpool_direct_oversize PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS regpool_direct_oversize")
    set(_regpool_numa_tests regpool_basic regpool_lazy_carve
        regpool_ladder regpool_fallback_order regpool_spill_cycle
        regpool_spill_target regpool_direct_oversize)
    if(ARTS_DEBUG_INJECTION)
        # Eviction of retired mappings and the unplaced fallback behind it,
        # driven by the force-full override on one node and on two.
        register_pure_unit_test(regpool_evict_retired TIMEOUT 60)
        set_tests_properties(regpool_evict_retired PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS regpool_evict_retired")
        # Retired oversize mappings are torn down outside the pool's lock,
        # and a request that reaches their hand-back waits for a teardown in
        # flight.
        register_pure_unit_test(regpool_evict_wait TIMEOUT 60)
        set_tests_properties(regpool_evict_wait PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS regpool_evict_wait")
        # Lock-free lookups and arena sweeps while oversize requests rewrite
        # a freed table entry under them: a live block always resolves to its
        # own record, and under ThreadSanitizer neither reader races the
        # rewrite.
        register_pure_unit_test(regpool_lookup_vs_rewrite TIMEOUT 60)
        set_tests_properties(regpool_lookup_vs_rewrite PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS regpool_lookup_vs_rewrite")
        # A full node's demand served from the other node, and the pool's end
        # at the allocator's arena table: it fills the table itself, so it
        # links the pool's object and the allocator directly (one table
        # between them), never the runtime archive, whose allocator copy is
        # its own.
        add_executable(regpool_spill_and_latch unit/regpool_spill_and_latch.c
            $<TARGET_OBJECTS:arts_regpool>)
        target_include_directories(regpool_spill_and_latch PRIVATE
            ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
            ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
        arts_apply_protocol(regpool_spill_and_latch ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})
        target_link_libraries(regpool_spill_and_latch PRIVATE
            Threads::Threads mimalloc-static arts::libfabric ${RT_LIBRARY}
            ${CMAKE_DL_LIBS})
        register_pure_unit_test(regpool_spill_and_latch TIMEOUT 60)
        set_tests_properties(regpool_spill_and_latch PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS regpool_spill_and_latch")
        list(APPEND _regpool_numa_tests ${ARTS_REGPOOL_INJECTION_TESTS}
            regpool_spill_and_latch)
    endif()
    # The allocator asks the kernel for the calling thread's node on a
    # machine of several; held to one node, every node query these programs
    # answer is the pool's own.
    set_property(TEST ${_regpool_numa_tests}
        APPEND PROPERTY ENVIRONMENT "MIMALLOC_USE_NUMA_NODES=1")
endif()
# The availability parser is allocator-independent (pure over a stream).
register_pure_unit_test(regpool_node_avail TIMEOUT 30)

# net_selfsend — whitebox self-send exercise for the libfabric transport core
# (transport/net.c).  It #includes net.c directly (statics under test) with a
# stubbed dispatch symbol, so it must NOT link the ARTS library (that would
# duplicate net.o / the dispatcher).  It links the real registered-pool object
# (bounce-buffer path needs registered memory) + mimalloc + libfabric, and
# builds only when the arena allocator is selected (registered slabs require
# it).  No ARTS runtime, no ports -> a pure unit test.
if(ARTS_MALLOC STREQUAL "mimalloc")
    add_executable(net_selfsend unit/net_selfsend.c $<TARGET_OBJECTS:arts_regpool>)
    target_include_directories(net_selfsend PRIVATE
        ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
    arts_apply_protocol(net_selfsend ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})
    set_test_log_level(net_selfsend 2)
    target_link_libraries(net_selfsend PRIVATE
        Threads::Threads mimalloc-static arts::libfabric ${RT_LIBRARY})
    register_pure_unit_test(net_selfsend TIMEOUT 60)
    set_tests_properties(net_selfsend PROPERTIES ENVIRONMENT "FI_PROVIDER=tcp")

    # net_rendezvous — whitebox one-sided data plane (fi_writedata PUT into a
    # pre-registered landing buffer + txid immediate pairing).  Same standalone
    # harness shape as net_selfsend (includes net.c; must not link libarts).
    add_executable(net_rendezvous unit/net_rendezvous.c $<TARGET_OBJECTS:arts_regpool>)
    target_include_directories(net_rendezvous PRIVATE
        ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
    arts_apply_protocol(net_rendezvous ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})
    set_test_log_level(net_rendezvous 2)
    target_link_libraries(net_rendezvous PRIVATE
        Threads::Threads mimalloc-static arts::libfabric ${RT_LIBRARY})
    register_pure_unit_test(net_rendezvous TIMEOUT 60)
    set_tests_properties(net_rendezvous PROPERTIES ENVIRONMENT "FI_PROVIDER=tcp")

    # net_src_bind — whitebox for arts_net_init's steering hints: net_interface
    # source-bind (loopback), fabric_domain pin, the fatal missing-interface
    # contract and a one-host run's loopback bind.  Same standalone harness
    # shape as net_selfsend.
    add_executable(net_src_bind unit/net_src_bind.c $<TARGET_OBJECTS:arts_regpool>)
    target_include_directories(net_src_bind PRIVATE
        ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
    arts_apply_protocol(net_src_bind ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})
    set_test_log_level(net_src_bind 2)
    target_link_libraries(net_src_bind PRIVATE
        Threads::Threads mimalloc-static arts::libfabric ${RT_LIBRARY})
    register_pure_unit_test(net_src_bind TIMEOUT 60)
endif()

# net_addr_frame_bound — whitebox for the bootstrap address-frame length bound
# (arts_net_addr_frame_ok in net.h).  Header-only: it exercises the pure
# predicate, so it needs the internal include dir but no libfabric/runtime link.
add_executable(net_addr_frame_bound unit/net_addr_frame_bound.c)
target_include_directories(net_addr_frame_bound PRIVATE
    ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
    ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
arts_apply_protocol(net_addr_frame_bound ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})
target_link_libraries(net_addr_frame_bound PRIVATE Threads::Threads)
register_pure_unit_test(net_addr_frame_bound)

# --- Foundation concurrency primitives (standalone — no ARTS runtime) ---
# Exercise arts_shared_ptr_t (shared.c) in isolation.  These deliberately do
# NOT link the ARTS library: they provide their own libc-backed
# arts_calloc/arts_free shims, so the foundation is validated regardless of
# the runtime's build state.  libatomic + -mcx16 come from the global
# top-level setup.
set(SYNC_SHARED_TESTS shared_basic shared_aba)
foreach(t ${SYNC_SHARED_TESTS})
    add_executable(${t} unit/${t}.c
        ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c)
    target_include_directories(${t} PRIVATE
        ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
    target_link_libraries(${t} PRIVATE Threads::Threads)
    register_pure_unit_test(${t})
endforeach()

# --- Event state decider (event_arbiter.c) ---
# Pure function of the packed event word, so the table is driven directly with
# no runtime and no link.  Same standalone shape as the SYNC_* tests.
add_executable(event_compute_next unit/event_compute_next.c)
target_include_directories(event_compute_next PRIVATE
    ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
    ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
arts_apply_protocol(event_compute_next ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})
register_pure_unit_test(event_compute_next)

# --- Route-table slot reclamation (route_table.c) ---
# Standalone like the SYNC_* tests: the test TU includes route_table.c, guid.c
# and shared.c and supplies libc-backed allocator stubs, so no ARTS library
# link is needed.  route_table.c carries no protocol conditionals, so the run
# in the tree's own cell covers every arm.  Drives concurrent duplicate destroy
# against install to assert the slot-return is single-flight: a caller that did
# not win must not free, drain or unbind a slot that another GUID now owns.
# Also drives late re-reservations of destroyed GUIDs against racing lookups —
# the state in which a freed slot's key word can repeat a key it no longer
# answers for — pinning that a lookup verifies the object it pinned, not the
# word.
add_executable(route_table_reclaim_dupdestroy
    unit/route_table_reclaim_dupdestroy.c)
target_include_directories(route_table_reclaim_dupdestroy PRIVATE
    ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
    ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
arts_apply_protocol(route_table_reclaim_dupdestroy ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})
target_link_libraries(route_table_reclaim_dupdestroy PRIVATE Threads::Threads)
register_pure_unit_test(route_table_reclaim_dupdestroy TIMEOUT 120)

# route_table_dup_reserve: two reservers of one key across the return of the
# key's first probe slot must end with one published slot, the one holding the
# object, which never moves; a create of the next generation during the later
# claim loses to the live one; a retiring slot is waited out (deterministic,
# via the reservation's after-miss and after-claim injection points).
add_executable(route_table_dup_reserve unit/route_table_dup_reserve.c)
target_include_directories(route_table_dup_reserve PRIVATE
    ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
    ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
arts_apply_protocol(route_table_dup_reserve ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})
target_link_libraries(route_table_dup_reserve PRIVATE Threads::Threads)
register_pure_unit_test(route_table_dup_reserve TIMEOUT 30)
set_tests_properties(route_table_dup_reserve PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS route_table_dup_reserve")

# --- Work-stealing deque (deque.c) + array_list (array_list.c) ---
# Standalone like the SYNC_* tests above: each compiles the target source TU
# directly and provides its own libc-backed arts_calloc_aligned/arts_free
# (deque) or arts_malloc/arts_free (array_list), so no ARTS library link is
# needed.  deque_single_thread pins the owner-side contract (LIFO at the
# owner's end, FIFO at the steal end, growth and index wrap); the race tests
# check that the owner and its stealers take every pushed item exactly once:
# deque_chaselev_race under concurrent steals and growth,
# deque_last_element_tiebreak on the single remaining element, and
# deque_grow_during_steal that a stealer never reads a slot the grow has not
# copied.  Each hand-off from the owner to a stealer is a happens-before edge
# ThreadSanitizer sees, so a report there is a defect.
set(DEQUE_UNIT_TESTS
    deque_single_thread deque_chaselev_race deque_last_element_tiebreak
    deque_grow_during_steal)
foreach(t ${DEQUE_UNIT_TESTS})
    add_executable(${t} unit/${t}.c
        ${CMAKE_SOURCE_DIR}/libs/src/core/utils/deque.c)
    target_include_directories(${t} PRIVATE
        ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
    target_link_libraries(${t} PRIVATE Threads::Threads)
endforeach()
register_pure_unit_test(deque_single_thread)
register_pure_unit_test(deque_chaselev_race TIMEOUT 60)
register_pure_unit_test(deque_last_element_tiebreak TIMEOUT 60)
register_pure_unit_test(deque_grow_during_steal TIMEOUT 60)

add_executable(array_list_basic unit/array_list_basic.c
    ${CMAKE_SOURCE_DIR}/libs/src/core/utils/array_list.c)
target_include_directories(array_list_basic PRIVATE
    ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
    ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
target_link_libraries(array_list_basic PRIVATE Threads::Threads)
register_pure_unit_test(array_list_basic)

# --- malloc.c allocator + footprint, diag.c timestamp/printf ---
# Each test #includes the target runtime .c TU directly (with the heavy header
# include guards pre-defined + minimal ARTS_ERROR/ARTS_ALIGNED/footprint stubs
# inside the test), so no extra source and no ARTS library link is needed.
# malloc_alignment is the ASan target (heap safety + a-aligned user pointers);
# malloc_footprint_balance asserts that matched alloc/realloc/free sequences
# leave BYTES_MEMORY_FOOTPRINT at net zero, single-threaded and concurrently.
# diag_timestamp pins best-effort per-thread monotonicity + fold sanity and
# that arts_printf's prefix and body reach stdout as one line.
set(MISC_UTIL_UNIT_TESTS
    malloc_footprint_balance malloc_alignment diag_timestamp)
foreach(t ${MISC_UTIL_UNIT_TESTS})
    add_executable(${t} unit/${t}.c)
    target_include_directories(${t} PRIVATE
        ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
    target_link_libraries(${t} PRIVATE Threads::Threads)
endforeach()
# These tests #include malloc.c directly; when mimalloc is selected the TU
# needs <mimalloc.h> on its include path, inherited from mimalloc-static.
if(ARTS_MALLOC STREQUAL "mimalloc")
    foreach(t malloc_footprint_balance malloc_alignment)
        target_link_libraries(${t} PRIVATE mimalloc-static)
    endforeach()
endif()
register_pure_unit_test(malloc_footprint_balance TIMEOUT 60)
register_pure_unit_test(malloc_alignment)
register_pure_unit_test(diag_timestamp TIMEOUT 60)

endif()


# A create racing a first touch of its label: the race is the creator rank's
# against the home, which two ranks already separate.
register_single_node_test(db_create_after_first_touch TIMEOUT 60)
register_multinode_test(db_create_after_first_touch TIMEOUT 120 VARIANTS 2n_9w_1p)
foreach(_t db_create_after_first_touch db_create_after_first_touch_2n_9w_1p)
    set_tests_properties(${_t} PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()

if(ARTS_TEST_REFERENCE_TREE)
# One body of the per-DB free-list serves every arm that keeps one.
add_arts_test(per_db_buf_pool)
register_pure_unit_test(per_db_buf_pool)
set_tests_properties(per_db_buf_pool PROPERTIES
    PASS_REGULAR_EXPRESSION "RECYCLE_OK")

# Finish scopes nested and crossing ranks, in a loop.  Two ranks and four
# separate a scope's members by one and by several hops.
register_multinode_test(finish_event_stress TIMEOUT 60
                        VARIANTS 2n_9w_1p 4n_4w_1p)
foreach(_v 2n_9w_1p 4n_4w_1p)
    set_tests_properties(finish_event_stress_${_v} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()

register_multinode_test(multinode_edt VARIANTS 2n_9w_1p)
set_tests_properties(multinode_edt_2n_9w_1p PROPERTIES
    PASS_REGULAR_EXPRESSION
    "PASS: multinode_edt every check reported before the finish scope fired")
register_multinode_test(event_scalar_return TIMEOUT 30 VARIANTS 2n_9w_1p)
# Four ranks put the creator, the home and the sink on three different ranks.
register_multinode_test(event_payload_policy TIMEOUT 15
                        VARIANTS 2n_9w_1p 4n_4w_1p)
# The guard aborts the run by design: the run exits 1 and the message names
# the guard; one node shape states it as well as four.
register_multinode_test(edt_create_oversize_guard VARIANTS 2n_9w_1p TIMEOUT 60
                        STATUS 1)
set_tests_properties(edt_create_oversize_guard_2n_9w_1p PROPERTIES
    PASS_REGULAR_EXPRESSION "over the [0-9]+-byte control-message bound")
register_multinode_test(multinode_event_types VARIANTS 2n_9w_1p)
register_multinode_test(multinode_stdout_forward VARIANTS 2n_9w_1p)
set_tests_properties(multinode_stdout_forward_2n_9w_1p PROPERTIES
    PASS_REGULAR_EXPRESSION "MULTINODE_STDOUT_FORWARD_SENTINEL 42")
# The startup check of the receive side's multi-recv threshold holds only if
# the endpoint answers for the queue its messages land in; ofi_rxm in
# passthru mode does not, so a run on it ends at startup with exit 1.
arts_test_cfg(_mn_cfg 2n_9w_1p)
arts_add_test(net_rxm_passthru_refused STATUS 1 COMMAND multinode_edt)
set_tests_properties(net_rxm_passthru_refused PROPERTIES
    LABELS "multinode"
    TIMEOUT 30
    RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT
    "ARTS_CONFIG=${_mn_cfg};provider=tcp\\;ofi_rxm;FI_OFI_RXM_ENABLE_PASSTHRU=1"
    PASS_REGULAR_EXPRESSION "runs ofi_rxm in passthru mode")
endif()

# --- Multi-node tests ---

register_multinode_test(db_null_control TIMEOUT 15 VARIANTS 2n_9w_1p 2n_8w_2p)
# A block created without an acquire has its payload allocated by its first
# user, so the same program has to hold on one rank (the first user is local)
# and on several (the home materializes the block to serve the request); four
# ranks put the requester, the home and the first user apart.
register_multinode_test(db_no_acquire_lazy TIMEOUT 60 VARIANTS 2n_9w_1p 4n_4w_1p)
# A block whose payload never exists at its home: the first user is a remote
# WRITER, so the home answers the ownership request with no bytes and the
# holder mints the block's first image itself.  The two readers catch the two
# ways that goes wrong — a NULL pointer for a sized block, and a first image
# minted at the stamp the writer's own release uses (which makes the real
# bytes retreat as stale).  The racing reads need a rank that is neither the
# home nor the writer, which two ranks do not have.
register_multinode_test(db_no_acquire_wb_first_writer TIMEOUT 60
                        VARIANTS 2n_9w_1p 4n_4w_1p)
foreach(_v 2n_9w_1p 4n_4w_1p)
    set_tests_properties(db_no_acquire_wb_first_writer_${_v} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
# The unordered reader's whole point is a race, so where the injection hooks
# are compiled one shape also runs with the scheduler fuzz, which widens the
# lock-free decision-commit windows.  At least three ranks are needed,
# because the racing read has to reach the home from a rank that is neither
# the home nor the writer — with two there is no such rank and the window
# cannot be reached at all.
if(ARTS_DEBUG_INJECTION)
    arts_test_cfg(_cfg 4n_4w_1p)
    arts_add_test(db_no_acquire_wb_first_writer_4n_4w_1p_fuzz
                  COMMAND db_no_acquire_wb_first_writer)
    set_tests_properties(db_no_acquire_wb_first_writer_4n_4w_1p_fuzz PROPERTIES
        LABELS "multinode"
        TIMEOUT 120
        RESOURCE_LOCK "arts_runtime"
        FAIL_REGULAR_EXPRESSION "FAIL"
        ENVIRONMENT "ARTS_CONFIG=${_cfg};ARTS_SCHED_FUZZ=25")
endif()
# A create that lands where a dependence on the same label has already asked
# the block's home about it: the create may install the block, but it may not
# take the write right out from under a request already in flight.  Both
# request kinds get a lane (RW opens an ownership request, RO a fetch or a
# snapshot), so this needs a rank that is not the home; the two progress
# threads let the home take the create and the request in either order.  The
# create's hold is a write acquisition the program orders against neither the
# early RW dependence nor the ordered writer, so the program is outside DB-WRF
# and the FLUSH tree does not register it.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
    register_multinode_test(db_create_after_dep_asked TIMEOUT 60
                            VARIANTS 2n_9w_1p 2n_8w_2p)
    foreach(_v 2n_9w_1p 2n_8w_2p)
        set_tests_properties(db_create_after_dep_asked_${_v} PROPERTIES
            FAIL_REGULAR_EXPRESSION "FAIL")
    endforeach()
endif()
# Every case of the matrix that two ranks can form is also formed on four.
register_multinode_test(db_first_touch_matrix TIMEOUT 120 VARIANTS 4n_4w_1p)
# One EDT naming one block in several slots acquires it once.  On the home
# every way of asking for the block answers with the rank's own copy, so a
# second acquisition shows only in the hold's accounting; off the home a
# second acquisition is a second copy at a second address whose release can
# erase the EDT's write.  Every arm owes the same answer, so it runs in all
# of them.
add_arts_test(db_alias_dedup_remote)
register_single_node_test(db_alias_dedup_remote TIMEOUT 30)
register_multinode_test(db_alias_dedup_remote TIMEOUT 60
                        VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_t db_alias_dedup_remote db_alias_dedup_remote_2n_9w_1p
        db_alias_dedup_remote_2n_8w_2p)
    set_tests_properties(${_t} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL:"
        PASS_REGULAR_EXPRESSION "PASS: db_alias_dedup_remote")
endforeach()
# Every rank's work is the same at any rank count, and four ranks feed one EDT
# from three homes.
register_multinode_test(route_table_remote_guid VARIANTS 4n_4w_1p)
# A NO_ACQUIRE create whose creator, home and writer are three different ranks:
# the home-side create handler must end naming ITSELF as the holder on every
# arm, or the writer's request reaches a rank that holds nothing and the run
# times out.  One rank cannot separate the three roles, and two cannot either.
register_multinode_test(grant_purge_no_acquire_creator TIMEOUT 60
                        VARIANTS 4n_4w_1p)
# Write-right churn with home-local turns overlapping remote ones.  It asserts
# an exact sum, which every release policy must compute, so it runs at every
# release policy.  On the arms that share the grant plane (VAL, INV), whose
# decision points carry the scheduler fuzz, the shape with two progress
# threads also runs under it where the injection hooks are compiled.
# These assert what the runtime's exclusive-RW serialization guarantees; under
# DB-WRF that ordering is the program's, and these programs deliberately omit
# it.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_single_node_test(grant_purge_churn TIMEOUT 60)
register_multinode_test(grant_purge_churn TIMEOUT 90)
if(ARTS_DEBUG_INJECTION AND ARTS_COHERENCE_ARM MATCHES "^(VAL|INV)$")
    arts_test_cfg(_cfg 2n_8w_2p)
    arts_add_test(grant_purge_churn_2n_8w_2p_fuzz COMMAND grant_purge_churn)
    set_tests_properties(grant_purge_churn_2n_8w_2p_fuzz PROPERTIES
        LABELS "multinode"
        TIMEOUT 120
        RESOURCE_LOCK "arts_runtime"
        FAIL_REGULAR_EXPRESSION "FAIL"
        ENVIRONMENT "ARTS_CONFIG=${_cfg};ARTS_SCHED_FUZZ=25")
endif()
endif()
# Independent writer chains can re-request a handed-back grant while the
# previous release is settling. Atomic increments cover concurrent same-rank
# RW holders.
# These assert what the runtime's exclusive-RW serialization guarantees; under
# DB-WRF that ordering is the program's, and these programs deliberately omit
# it.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_single_node_test(grant_purge_handback_regrant TIMEOUT 60)
register_multinode_test(grant_purge_handback_regrant TIMEOUT 120)
endif()
# Where the right is handed back unasked, a hand-back can reach the home while
# another thread still holds its serving baton, and only that thread's re-check
# of the return latch accepts it.  The schedule fuzz can delay a serve between
# its grant and its baton release, so the two-progress-thread shape meets that
# window in most runs.
if(ARTS_DEBUG_INJECTION AND ARTS_COHERENCE_ARM MATCHES "^(VAL|INV)$" AND
   ARTS_RELEASE_POLICY STREQUAL "PURGE")
    arts_test_cfg(_cfg 2n_8w_2p)
    arts_add_test(grant_purge_handback_regrant_2n_8w_2p_fuzz
                  COMMAND grant_purge_handback_regrant)
    set_tests_properties(grant_purge_handback_regrant_2n_8w_2p_fuzz PROPERTIES
        LABELS "multinode"
        TIMEOUT 120
        RESOURCE_LOCK "arts_runtime"
        FAIL_REGULAR_EXPRESSION "FAIL"
        ENVIRONMENT "ARTS_CONFIG=${_cfg};ARTS_SCHED_FUZZ=25")
endif()
register_multinode_test(coherence_rwro_phase TIMEOUT 30)
foreach(_t coherence_rwro_phase coherence_rwro_phase_2n_9w_1p
        coherence_rwro_phase_4n_4w_1p coherence_rwro_phase_2n_8w_2p)
    set_tests_properties(${_t} PROPERTIES
        PASS_REGULAR_EXPRESSION "coherence_rwro_phase: .* — PASS")
endforeach()
# The silent-wrong-answer gate for retention: an owner writes AFTER lending a
# read copy, and the lender reads again strictly after that write.  A rank that
# keeps a stale copy answers the old value — no crash, no hang.  Portable, so
# it guards every arm, not just the retaining one.  It uses ranks 0 and 1
# only; the two progress threads reorder the hand-over's messages.
add_arts_test(coherence_owner_write_after_lend)
register_multinode_test(coherence_owner_write_after_lend TIMEOUT 30
                        VARIANTS 2n_9w_1p 2n_8w_2p)

# The read-side twin of grant_sticky: a read grant must outlive its readers
# under the retaining policy.  No value oracle can see this — a purging
# implementation computes identical answers and merely re-fetches — so it needs
# an assertion of its own, and only the exclusion arm's retaining policy keeps
# read grants.
if(ARTS_COHERENCE_ARM STREQUAL "EXCL" AND ARTS_RELEASE_POLICY STREQUAL "RETAIN")
    add_arts_test(ro_grant_retained)
    register_multinode_test(ro_grant_retained TIMEOUT 30 VARIANTS 2n_9w_1p)
endif()

# grant_release_keeps_sharer: a rank that holds a durable copy, takes the
# write right and releases stays a sharer — the next owner's first release
# retires it.  The next requester asks while the holder still holds, so the
# hand-over is decided before the holder's own release round; a registration
# that round could erase is what this catches.  2n_9w_1p puts the home as the next
# owner; 4n_4w_1p puts another non-home rank there.  Portable OCR shape, no
# timing knob (the holder's 3 ms hold only has to outlast one request hop).
# Its two writers are ordered only by the runtime's exclusive RW turn, never
# by happens-before, so the program is outside DB-WRF and the FLUSH tree
# does not register it.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
    add_arts_test(grant_release_keeps_sharer)
    register_multinode_test(grant_release_keeps_sharer TIMEOUT 60
                            VARIANTS 2n_9w_1p 4n_4w_1p)
endif()

register_multinode_test(coherence_ro_acquire_stress TIMEOUT 90)

# Coherence stress test suite.  Each adds the "coherence_stress" label on top
# of its node-count label so the suite can be filtered with
# `ctest -L coherence_stress`.
# Read and write turns of every block contend at once; its write acquisitions
# are unordered, which DB-WRF does not admit.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_single_node_test(coherence_stress_single_node TIMEOUT 120)
set_property(TEST coherence_stress_single_node APPEND PROPERTY
    LABELS "coherence_stress")
set_tests_properties(coherence_stress_single_node PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: [0-9]+ EDTs completed across")
endif()
# Writers holding several blocks at once: DB-WRF does not admit them, so every
# other arm runs them.  With peers the turns come from every rank.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
    register_single_node_test(coherence_multi_writer_multi_db TIMEOUT 60)
    register_multinode_test(coherence_multi_writer_multi_db TIMEOUT 120)
    foreach(_v "" _2n_9w_1p _4n_4w_1p _2n_8w_2p)
        set_property(TEST coherence_multi_writer_multi_db${_v} APPEND PROPERTY
            LABELS "coherence_stress")
        set_tests_properties(coherence_multi_writer_multi_db${_v} PROPERTIES
            FAIL_REGULAR_EXPRESSION "FAIL")
    endforeach()
endif()
register_single_node_test(coherence_ro_acquire_stress TIMEOUT 60)
set_property(TEST coherence_ro_acquire_stress APPEND PROPERTY
    LABELS "coherence_stress")

# Runtime memory comes from the arts_malloc family, never the process C
# allocator: an interposed or exhausted one must not be reachable from it.
# A scan of the runtime source, which every tree shares.
if(ARTS_TEST_REFERENCE_TREE)
    add_test(NAME runtime_no_libc_alloc
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_no_libc_alloc.sh
                     ${CMAKE_SOURCE_DIR})
    set_tests_properties(runtime_no_libc_alloc PROPERTIES
        LABELS "single_node"
        TIMEOUT 60
        PASS_REGULAR_EXPRESSION "PASS runtime_no_libc_alloc")
endif()

# ARTS_USE_CXL=ON on a target other than AMD64 is a configure error raised
# before anything vendored is configured.  A nested configure of this source,
# which every tree shares.
if(ARTS_TEST_REFERENCE_TREE)
    add_test(NAME cxl_arm64_refusal
             COMMAND ${CMAKE_COMMAND}
                     -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
                     -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/cxl_arm64_refusal
                     -DC_COMPILER=${CMAKE_C_COMPILER}
                     -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
                     -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
                     -P
                     ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cxl_arm64_refusal.cmake)
    set_tests_properties(cxl_arm64_refusal PROPERTIES
        LABELS "single_node"
        TIMEOUT 300
        PASS_REGULAR_EXPRESSION "PASS cxl_arm64_refusal"
        FAIL_REGULAR_EXPRESSION "FAIL cxl_arm64_refusal")
endif()

# Every access to an _Atomic object names its memory order.  clang-tidy judges
# every C unit of this tree's runtime, shim, tests and examples with the tree's
# own compile commands, so each cell's conditional code is judged in the tree
# that compiles it.  Registered where clang-tidy is found.
find_program(ARTS_CLANG_TIDY clang-tidy)
if(ARTS_CLANG_TIDY)
    set(_script
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/atomics_implicit_seq_cst.cmake)
    add_test(NAME atomics_implicit_seq_cst
             COMMAND ${CMAKE_COMMAND}
                     -DCLANG_TIDY=${ARTS_CLANG_TIDY}
                     -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
                     -DBUILD_DIR=${CMAKE_BINARY_DIR}
                     -P ${_script})
    set_tests_properties(atomics_implicit_seq_cst PROPERTIES
        LABELS "single_node"
        TIMEOUT 120
        PASS_REGULAR_EXPRESSION "PASS atomics_implicit_seq_cst"
        FAIL_REGULAR_EXPRESSION "FAIL atomics_implicit_seq_cst")
else()
    message(STATUS "atomics_implicit_seq_cst not registered: no clang-tidy")
endif()

# Blocks destroyed while the one-sided writes that moved their payloads may
# still be awaiting their local completions.  It runs with those completions
# held back (ARTS_WRITE_COMPLETION_DELAY_MS), so where payloads travel through
# the fabric a completion that drops a buffer's last reference after its
# block's descriptor is gone is the common case, and the descriptor
# quarantine reports a write that lands in a freed descriptor; without the
# quarantine such a write lands silently, so the test registers only where
# both the injection hooks and the quarantine are compiled.  Where payloads
# reach the next holder through the CXL store, the fabric moves none, so there
# is no write completion to hold back and the test is not registered.
# Its writes are ordered, so every arm that moves payloads registers it, and its
# shutdown is quiescent.  The end-of-run line guards against a shutdown that
# came before the work; the descriptor check's abort at teardown reaches the
# exit status, and teardown work left undone because a thread never joined is
# matched in its output (a rank that had to be stopped is matched as in every
# test).
if(ARTS_DEBUG_INJECTION AND ARTS_DEBUG_DB_QUARANTINE AND
   NOT ARTS_CXL_TREE_RESIDENCY)
    add_arts_test(coherence_late_write_completion)
    register_multinode_test(coherence_late_write_completion TIMEOUT 120)
    foreach(_v 2n_9w_1p 4n_4w_1p 2n_8w_2p)
        set_property(TEST coherence_late_write_completion_${_v} APPEND PROPERTY
            LABELS "coherence_stress")
        set_property(TEST coherence_late_write_completion_${_v} APPEND PROPERTY
            ENVIRONMENT "ARTS_WRITE_COMPLETION_DELAY_MS=20")
        set_tests_properties(coherence_late_write_completion_${_v} PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS: coherence_late_write_completion"
            FAIL_REGULAR_EXPRESSION
            "FAIL:;QUIESCENCE-DEBUG:;left unchecked;left unrun;left unfreed")
    endforeach()
endif()

# The freed-descriptor quarantine itself (ARTS_DEBUG_DB_QUARANTINE): a write
# through a stale reference is reported when the descriptor leaves it and at
# the teardown drain, a second free is refused, and clean frees report
# nothing.  The descriptor free carries no protocol branch.
if(ARTS_DEBUG_DB_QUARANTINE AND ARTS_TEST_REFERENCE_TREE)
    add_arts_test(db_descriptor_quarantine)
    register_pure_unit_test(db_descriptor_quarantine TIMEOUT 60)
    set_tests_properties(db_descriptor_quarantine PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS db_descriptor_quarantine"
        FAIL_REGULAR_EXPRESSION "FAIL")
endif()

# Large-payload cross-rank coherence transfer: a ladder of DBs from 256 KiB to
# 8 MiB, each stamped by a home-rank writer and then read by RO readers on other
# ranks, so every hop moves the whole payload across ranks.  Each reader checks
# every element, which catches bytes landed at the wrong offset or only partly
# landed (FAIL); the sizeof(int) DBs elsewhere never reach these sizes.
# Standard MN variants (2n_9w_1p/4n_4w_1p/2n_8w_2p).
register_multinode_test(coherence_payload_large TIMEOUT 120)
foreach(_v 2n_9w_1p 4n_4w_1p 2n_8w_2p)
    set_property(TEST coherence_payload_large_${_v} APPEND PROPERTY
        LABELS "coherence_stress")
    # Require the end-of-run PASS line, so a run that ends before every reader
    # has verified fails, and fail on any reader's corruption abort.
    set_tests_properties(coherence_payload_large_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: 4 DBs"
        FAIL_REGULAR_EXPRESSION "FAIL:")
endforeach()

# The same ladder with the provider's one-sided ceiling lowered to 3 MiB by
# the injected override (ARTS_NET_MAX_MSG), so the 8 MiB rung can only arrive
# as several transfers (3 + 3 + 2 MiB) paired on the last one's immediate.
# Whole-payload verification is what catches a piece landing at the wrong
# offset, or a pairing that fires before the last piece has landed.  Where
# payloads reach the next holder through the CXL store, the fabric moves none
# and there is nothing to split.
if(ARTS_DEBUG_INJECTION AND NOT ARTS_CXL_TREE_RESIDENCY)
    foreach(_v 2n_9w_1p 4n_4w_1p 2n_8w_2p)
        arts_test_cfg(_cfg ${_v})
        arts_add_test(coherence_payload_split_${_v}
                      COMMAND coherence_payload_large)
        set_tests_properties(coherence_payload_split_${_v} PROPERTIES
            LABELS "multinode;coherence_stress"
            TIMEOUT 120
            RESOURCE_LOCK "arts_runtime"
            ENVIRONMENT "ARTS_CONFIG=${_cfg};ARTS_NET_MAX_MSG=3145728"
            PASS_REGULAR_EXPRESSION "PASS: 4 DBs"
            FAIL_REGULAR_EXPRESSION "FAIL:")
    endforeach()
endif()

# Add-dependence-then-release hand-off: consumers whose slots satisfied BEFORE
# the producer's release must observe the release-published bytes, also for a
# DB whose home is neither the creator nor pre-seeded.  A read served before
# the publication shows as a consumer's NULL/zero read (FAIL: and an abort); a
# parked acquire that is never woken leaves the PASS line unprinted.
# These assert what the runtime's exclusive-RW serialization guarantees; under
# DB-WRF that ordering is the program's, and these programs deliberately omit
# it.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_multinode_test(coherence_satisfy_before_release TIMEOUT 60)
foreach(_v 2n_9w_1p 4n_4w_1p 2n_8w_2p)
    set_property(TEST coherence_satisfy_before_release_${_v} APPEND PROPERTY
        LABELS "coherence_stress")
    set_tests_properties(coherence_satisfy_before_release_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: 4 DBs observed"
        FAIL_REGULAR_EXPRESSION "FAIL:")
endforeach()
endif()

# coherence_owner_ownership_reorder: unordered write turns from every rank on
# one block homed on rank 0, in deep batches and in short steps with a home-rank
# contender; the exact end-sum is the gate, and a stranded requester times out.
# 2n_8w_2p is the variant whose two progress threads may dispatch one peer's
# hand-over messages out of order.  Its writers are unordered, which DB-WRF
# does not admit.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_multinode_test(coherence_owner_ownership_reorder TIMEOUT 120)
foreach(_v 2n_9w_1p 4n_4w_1p 2n_8w_2p)
    set_property(TEST coherence_owner_ownership_reorder_${_v} APPEND PROPERTY
        LABELS "coherence_stress")
    set_tests_properties(coherence_owner_ownership_reorder_${_v} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
endif()

# coherence_owner_confirm_gate: a read ordered after a remote writer must see
# the writer's value, not a copy left at the previous holder.  Under
# write-back the home names a new holder only when its MSG_DB_GRANT_CONFIRM
# arrives, and the writer runs only after the home's CONFIRM_ACK, which closes
# that window.  Ranks 0 and 1 hold every turn; the two progress threads let the
# transfer's messages overtake each other.  One rank never transfers anything.
register_multinode_test(coherence_owner_confirm_gate TIMEOUT 120
                        VARIANTS 2n_9w_1p 2n_8w_2p)

# coherence_grant_chain_visibility: a grant chain — remote write then remote
# read, one holding reader per rank, and a writer whose RW request is issued
# while that cohort still holds (it waits on a latch the readers count down
# once they hold, not on their completion).  Under exclusion the request waits
# at the home behind the cohort and is granted by the cohort's last read
# release; the verdict is on values only (readers see the pre-write value, the
# increment lands exactly once).  A portable OCR program — no storage kind, no
# write policy — registered under every arm and rank count; valid under DB-WRF
# too, since its writers are ordered.
register_single_node_test(coherence_grant_chain_visibility TIMEOUT 60)
register_multinode_test(coherence_grant_chain_visibility TIMEOUT 120 VARIANTS 2n_9w_1p 4n_4w_1p)
set_tests_properties(coherence_grant_chain_visibility PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL")
foreach(_v 2n_9w_1p 4n_4w_1p)
    set_tests_properties(coherence_grant_chain_visibility_${_v} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()

if(ARTS_TEST_REFERENCE_TREE)
# A create of a live label parks on the slot's OoO list and installs when the
# occupant retires: an event's at its destroy, one per destroy, and an EDT's at
# its completion, whose signal is ordered after that install (whitebox, one
# rank).  Its shutdown leaves nothing parked.
add_arts_test(create_parks_until_retire)
register_single_node_test(create_parks_until_retire TIMEOUT 30)
set_tests_properties(create_parks_until_retire PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: create_parks_until_retire"
    FAIL_REGULAR_EXPRESSION "FAIL:;QUIESCENCE-DEBUG:")

# label_reuse_ring_event: one event label reused across 64 lifetimes, each next
# generation's create issued while the current one is live, so it parks at the
# home until the destroy.  Every generation's value must reach its consumer in
# order, and the shutdown must leave no create parked.  It uses ranks 0 and the
# last; 2n_8w_2p is the variant whose two progress threads dispatch one peer's
# messages out of order.
add_arts_test(label_reuse_ring_event)
register_single_node_test(label_reuse_ring_event TIMEOUT 60)
register_multinode_test(label_reuse_ring_event TIMEOUT 120
                        VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_t label_reuse_ring_event label_reuse_ring_event_2n_9w_1p
        label_reuse_ring_event_2n_8w_2p)
    set_tests_properties(${_t} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: label_reuse_ring_event 64 generations in order"
        FAIL_REGULAR_EXPRESSION "FAIL:;QUIESCENCE-DEBUG:")
endforeach()

# edt_destroy_before_create: a destroy that reaches an EDT's home before its
# create parks, and the create's install replays it, retiring the EDT inside
# the install; on the home the GUID's next generation then installs and runs.
# The destroyed EDT's messages from another rank are fire-and-forget, so the
# shutdown is not covered by a finish scope and markers are counted from -V.
# Two progress threads would let the destroy reach the home after the create,
# which edt_destroy_pending_scope drives.
add_arts_test(edt_destroy_before_create)
register_single_node_test(edt_destroy_before_create TIMEOUT 30)
register_multinode_test(edt_destroy_before_create TIMEOUT 60 VARIANTS 2n_9w_1p)
foreach(_t edt_destroy_before_create edt_destroy_before_create_2n_9w_1p)
    set_tests_properties(${_t} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: edt_destroy_before_create"
        FAIL_REGULAR_EXPRESSION "FAIL:")
endforeach()

# edt_destroy_pending_scope: destroying an EDT that never ran sends the
# finish-scope DECR its completion would have, directly or through the remote
# scope's proxy, so the scope drains and the shutdown is quiescent.  Two
# progress threads are what let the destroy reach the home before the create.
add_arts_test(edt_destroy_pending_scope)
register_single_node_test(edt_destroy_pending_scope TIMEOUT 30)
register_multinode_test(edt_destroy_pending_scope TIMEOUT 60
                        VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_t edt_destroy_pending_scope edt_destroy_pending_scope_2n_9w_1p
        edt_destroy_pending_scope_2n_8w_2p)
    set_tests_properties(${_t} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: edt_destroy_pending_scope"
        FAIL_REGULAR_EXPRESSION "FAIL:;QUIESCENCE-DEBUG:")
endforeach()

# edt_create_destroy_proxy: a destroy on another thread of an EDT's home,
# while the create body that installed the EDT is held right after the
# install by the runtime's injection hook, finds the remote finish scope's
# proxy already in place and the proxy fires.  The hook exists only where the
# injection hooks are compiled; one variant also runs under the scheduler
# fuzz.
if(ARTS_DEBUG_INJECTION AND ARTS_TEST_REFERENCE_TREE)
    add_arts_test(edt_create_destroy_proxy)
    register_multinode_test(edt_create_destroy_proxy TIMEOUT 90
                            VARIANTS 2n_9w_1p 2n_8w_2p)
    arts_test_cfg(_cfg 2n_8w_2p)
    arts_add_test(edt_create_destroy_proxy_2n_8w_2p_fuzz
                  COMMAND edt_create_destroy_proxy)
    set_tests_properties(edt_create_destroy_proxy_2n_8w_2p_fuzz PROPERTIES
        LABELS "multinode"
        TIMEOUT 90
        RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg};ARTS_SCHED_FUZZ=25")
    foreach(_t edt_create_destroy_proxy_2n_9w_1p
            edt_create_destroy_proxy_2n_8w_2p
            edt_create_destroy_proxy_2n_8w_2p_fuzz)
        set_tests_properties(${_t} PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS: edt_create_destroy_proxy"
            FAIL_REGULAR_EXPRESSION "FAIL:;QUIESCENCE-DEBUG:")
    endforeach()
endif()

# finish_scope_creator_token: a finish scope whose creator hands its token back
# (arts_event_release_creator_token) fires when its members, a remote worker
# and that worker's child, finish, while the creator is still running.
add_arts_test(finish_scope_creator_token)
register_single_node_test(finish_scope_creator_token TIMEOUT 60)
register_multinode_test(finish_scope_creator_token TIMEOUT 90 VARIANTS 2n_9w_1p)
foreach(_t finish_scope_creator_token finish_scope_creator_token_2n_9w_1p)
    set_tests_properties(${_t} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: finish_scope_creator_token"
        FAIL_REGULAR_EXPRESSION "FAIL:;QUIESCENCE-DEBUG:")
endforeach()

endif()

# label_reuse_ring_edt: one reserved EDT GUID reused across 64 lifetimes, each
# generation retired by its completion, in two lanes: the next generation
# created after that completion is observed, or created early (it parks at the
# home behind the live generation).  Either way its satisfy and dependence
# follow the completion.  Every generation's value must be recorded in order,
# and the shutdown must leave no create parked.  It uses ranks 0 and the last;
# 2n_8w_2p is the variant whose two progress threads may dispatch one peer's
# messages out of order.
add_arts_test(label_reuse_ring_edt)
register_single_node_test(label_reuse_ring_edt TIMEOUT 60)
register_multinode_test(label_reuse_ring_edt TIMEOUT 120
                        VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_t label_reuse_ring_edt label_reuse_ring_edt_2n_9w_1p
        label_reuse_ring_edt_2n_8w_2p)
    set_tests_properties(${_t} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: label_reuse_ring_edt 64 generations in order, late and early creates"
        FAIL_REGULAR_EXPRESSION "FAIL:;QUIESCENCE-DEBUG:")
endforeach()

# db_destroy_roster: a block written in turn by two remote ranks is destroyed,
# and each writer rank then creates the label again; a writer's cache the
# destroy did not reach and that kept a right to the block would hold its slot
# against that create (one that kept no right is claimed, unseen).  Four ranks
# give the two writers (its first writer's turn is the whole two-rank case).
# Not on 2n_8w_2p: there a writer rank's two progress threads can run the next
# generation's create before the destroy's notice, which then retires that
# create's descriptor, since generations of a reused label are not told apart.
add_arts_test(db_destroy_roster)
register_multinode_test(db_destroy_roster TIMEOUT 60 VARIANTS 4n_4w_1p)
set_tests_properties(db_destroy_roster_4n_4w_1p PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: db_destroy_roster"
    FAIL_REGULAR_EXPRESSION "FAIL:;QUIESCENCE-DEBUG:")

# db_destroy_reaches_cachers: every rank that held a block's descriptor, the
# write right, a read copy or nothing, has it retired by the block's destroy;
# each rank polls its own route table for the blocks it held, so a missed
# destroy shows whatever the descriptor kept, with no label reused.
add_arts_test(db_destroy_reaches_cachers)
register_multinode_test(db_destroy_reaches_cachers TIMEOUT 90)
foreach(_v IN LISTS ARTS_TEST_MULTINODE_VARIANTS)
    set_tests_properties(db_destroy_reaches_cachers_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: db_destroy_reaches_cachers 8 rounds x 24 blocks"
        FAIL_REGULAR_EXPRESSION "FAIL:;QUIESCENCE-DEBUG:")
endforeach()

# db_destroy_untouched_rank: a block homed on the first rank is destroyed by a
# task on the last rank that never acquired it, and the home then creates the
# label again; a destroy that never left the last rank would leave the second
# create parked behind a live first generation.  On one rank the destroying
# task runs on the home and the path is never taken.  Not on 2n_8w_2p: there
# the home's two progress threads can dispatch the destroying task's finish
# signal before its destroy, so the second create is issued while the first
# generation is live and, under invalidation, the creator's release meets the
# first generation.
add_arts_test(db_destroy_untouched_rank)
register_multinode_test(db_destroy_untouched_rank TIMEOUT 60 VARIANTS 2n_9w_1p)
set_tests_properties(db_destroy_untouched_rank_2n_9w_1p PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: db_destroy_untouched_rank"
    FAIL_REGULAR_EXPRESSION "FAIL:;QUIESCENCE-DEBUG:")

# Covered requests: a request whose waiters something other than its own
# answer served -- its rank's write turn, or a create's hold on the cache its
# rank made by a first touch -- is still travelling when the program destroys
# the block.  The home learns of each before the turn that covers it can end
# and tears the block down only once every one has arrived.  Requests combine
# per rank on every arm but FLUSH, whose fetch is answered before its EDT
# runs, so no cover exists there and these are neither built nor registered
# in that tree.  A variant that sets an injection hook (ARTS_REQUEST_DELAY_MS
# reorders requests on the wire or holds one kind of them; ARTS_SCHED_FUZZ
# widens the decision windows) registers only where the hooks are compiled.
# Each name_<variant> runs PROGRAM with ARGS on that variant's cfg, the HOOK
# variable added to its environment.
function(arts_cover_test name program)
    cmake_parse_arguments(ARG "" "TIMEOUT;PASS;HOOK" "ARGS;VARIANTS" ${ARGN})
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "arts_cover_test(${name}): unexpected arguments "
                "'${ARG_UNPARSED_ARGUMENTS}'")
    endif()
    if(NOT ARG_VARIANTS)
        set(ARG_VARIANTS ${ARTS_TEST_MULTINODE_VARIANTS})
    endif()
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 120)
    endif()
    foreach(_v IN LISTS ARG_VARIANTS)
        arts_test_cfg(_cfg ${_v})
        set(_env "ARTS_CONFIG=${_cfg}")
        if(ARG_HOOK)
            list(APPEND _env "${ARG_HOOK}")
        endif()
        arts_add_test(${name}_${_v} COMMAND ${program} ${ARG_ARGS})
        set_tests_properties(${name}_${_v} PROPERTIES
            LABELS "multinode;coherence_stress"
            TIMEOUT ${ARG_TIMEOUT}
            RESOURCE_LOCK "arts_runtime"
            ENVIRONMENT "${_env}"
            PASS_REGULAR_EXPRESSION "${ARG_PASS}"
            FAIL_REGULAR_EXPRESSION "FAIL:;QUIESCENCE-DEBUG:")
    endforeach()
endfunction()

if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
    # db_destroy_covered_read: rounds of blocks whose read request a write
    # turn of its own rank served, each block destroyed once its holds are
    # released; with the hook, requests overtake each other.
    add_arts_test(db_destroy_covered_read)
    arts_cover_test(db_destroy_covered_read db_destroy_covered_read
                    PASS "PASS: db_destroy_covered_read 8 rounds x 32 blocks")
    # db_destroy_claimed_first_touch: a create claims the cache its rank's
    # consumers made by a first touch -- read-only, read-write and both --
    # while their requests are on their way; wave2 adds consumers after the
    # claim, whose requests travel beside the earlier ones.
    add_arts_test(db_destroy_claimed_first_touch)
    arts_cover_test(db_destroy_claimed_first_touch
                    db_destroy_claimed_first_touch
                    PASS "PASS: db_destroy_claimed_first_touch 64 blocks")
    arts_cover_test(db_destroy_claimed_first_touch_wave2
                    db_destroy_claimed_first_touch ARGS wave2
                    PASS "PASS: db_destroy_claimed_first_touch 64 blocks \\(second wave\\)")
    # db_read_dismissed_before_grant: the requester, the write turn's holder
    # and the home are three distinct ranks.  Without the hook a
    # reader may legally see the creator's value; with every read request
    # held 100 ms the dismiss overtakes the grant and the readers ride the
    # write request, so the strict oracle applies.
    add_arts_test(db_read_dismissed_before_grant)
    arts_cover_test(db_read_dismissed_before_grant
                    db_read_dismissed_before_grant VARIANTS 4n_4w_1p
                    PASS "PASS: db_read_dismissed_before_grant 8 blocks")
    # db_destroy_cover_settles and db_create_races_acquire wait after their
    # last destroy for every home of their blocks to be torn down and, where
    # the build counts rendezvous landings, for every landing to end: an
    # over-count at a home is a liveness failure no shutdown check sees.
    add_arts_test(db_destroy_cover_settles)
    arts_cover_test(db_destroy_cover_settles_t2 db_destroy_cover_settles
                    ARGS t2 PASS "PASS: db_destroy_cover_settles 8 rounds x 32 blocks \\(t2\\)")
    arts_cover_test(db_destroy_cover_settles_t1 db_destroy_cover_settles
                    ARGS t1 PASS "PASS: db_destroy_cover_settles 8 rounds x 32 blocks \\(t1\\)")
    add_arts_test(db_create_races_acquire)
    arts_cover_test(db_create_races_acquire_writers db_create_races_acquire
                    ARGS 0 6 VARIANTS 2n_9w_1p 2n_8w_2p
                    PASS "PASS: db_create_races_acquire 32 blocks")
    arts_cover_test(db_create_races_acquire_mixed db_create_races_acquire
                    ARGS 3 3 VARIANTS 2n_9w_1p 2n_8w_2p
                    PASS "PASS: db_create_races_acquire 32 blocks")
    if(ARTS_DEBUG_INJECTION)
        arts_cover_test(db_destroy_covered_read_delay db_destroy_covered_read
                        HOOK "ARTS_REQUEST_DELAY_MS=5"
                        PASS "PASS: db_destroy_covered_read 8 rounds x 32 blocks")
        arts_cover_test(db_destroy_claimed_first_touch_delay
                        db_destroy_claimed_first_touch
                        HOOK "ARTS_REQUEST_DELAY_MS=5"
                        PASS "PASS: db_destroy_claimed_first_touch 64 blocks")
        arts_cover_test(db_destroy_claimed_first_touch_wave2_delay
                        db_destroy_claimed_first_touch ARGS wave2
                        HOOK "ARTS_REQUEST_DELAY_MS=5"
                        PASS "PASS: db_destroy_claimed_first_touch 64 blocks \\(second wave\\)")
        # db_read_rides_write: every read request held 60 s and no write
        # request, so the write turn answers first; a reader left waiting for
        # its own answer would outlast the budget.
        add_arts_test(db_read_rides_write)
        arts_cover_test(db_read_rides_write db_read_rides_write TIMEOUT 45
                        HOOK "ARTS_REQUEST_DELAY_MS=60000:ro"
                        PASS "PASS: db_read_rides_write 16 blocks")
        arts_cover_test(db_read_dismissed_before_grant_strict
                        db_read_dismissed_before_grant ARGS strict
                        VARIANTS 4n_4w_1p HOOK "ARTS_REQUEST_DELAY_MS=100:ro"
                        PASS "PASS: db_read_dismissed_before_grant 8 blocks")
        arts_cover_test(db_destroy_cover_settles_t2_delay
                        db_destroy_cover_settles ARGS t2
                        HOOK "ARTS_REQUEST_DELAY_MS=5"
                        PASS "PASS: db_destroy_cover_settles 8 rounds x 32 blocks \\(t2\\)")
        arts_cover_test(db_destroy_cover_settles_t1_delay
                        db_destroy_cover_settles ARGS t1
                        HOOK "ARTS_REQUEST_DELAY_MS=5"
                        PASS "PASS: db_destroy_cover_settles 8 rounds x 32 blocks \\(t1\\)")
        arts_cover_test(db_create_races_acquire_writers_fuzz
                        db_create_races_acquire ARGS 0 6
                        VARIANTS 2n_9w_1p 2n_8w_2p HOOK "ARTS_SCHED_FUZZ=40"
                        PASS "PASS: db_create_races_acquire 32 blocks")
        arts_cover_test(db_create_races_acquire_mixed_fuzz
                        db_create_races_acquire ARGS 3 3
                        VARIANTS 2n_9w_1p 2n_8w_2p HOOK "ARTS_SCHED_FUZZ=40"
                        PASS "PASS: db_create_races_acquire 32 blocks")
        # A hop -- a write-back home's redirect, a holder's bounce, a
        # retaining home's forward -- continues a request the home took in,
        # so it is never an early arrival: held 200 ms it lands after the
        # teardown and is answered (or dropped) rather than parked.  Only
        # the arms whose home hops have one; four ranks give the holder a
        # rank of its own.
        if((ARTS_COHERENCE_ARM MATCHES "^(VAL|INV)$" AND
            ARTS_WRITE_POLICY STREQUAL "WB") OR
           (ARTS_COHERENCE_ARM STREQUAL "EXCL" AND
            ARTS_RELEASE_POLICY STREQUAL "RETAIN"))
            arts_cover_test(db_destroy_cover_settles_hop
                            db_destroy_cover_settles ARGS t2 hop
                            VARIANTS 4n_4w_1p HOOK "ARTS_REQUEST_DELAY_MS=200:hop"
                            PASS "PASS: db_destroy_cover_settles 8 rounds x 32 blocks \\(t2, hop\\)")
        endif()
    endif()
endif()

# db_size_round_once reads the size rounds the home sent back from the counter
# file, which carries them only where the tree's counter config captures the
# arm's counters; elsewhere it is built and not registered.  A store whose
# grants carry no payload sends no size round.
add_arts_test(db_size_round_once)
if(ARTS_COHERENCE_ARM STREQUAL "EXCL")
    set(_size_counters NUM_EXCL_SIZE_CTS)
elseif(ARTS_COHERENCE_ARM STREQUAL "VAL")
    set(_size_counters NUM_GRANT_SIZE_CTS NUM_SNAPSHOT_SIZE_CTS)
elseif(ARTS_COHERENCE_ARM STREQUAL "FLUSH")
    set(_size_counters NUM_FLUSH_SIZE_CTS)
else()
    set(_size_counters NUM_GRANT_SIZE_CTS NUM_INV_SIZE_CTS)
endif()
set(_size_captured ON)
foreach(_c IN LISTS _size_counters)
    file(STRINGS "${COUNTER_CONFIG_FILE}" _line REGEX "^${_c}=")
    if(NOT _line OR _line MATCHES "^${_c}=OFF")
        set(_size_captured OFF)
    endif()
endforeach()
if(_size_captured AND NOT ARTS_CXL_TREE_RESIDENCY)
    arts_test_cfg(_cfg 2n_9w_1p)
    foreach(_g reserved minted)
        foreach(_m writes reads)
            arts_add_test(db_size_round_once_${_g}_${_m}
                          COMMAND db_size_round_once ${_g} ${_m})
            set_tests_properties(db_size_round_once_${_g}_${_m} PROPERTIES
                LABELS "multinode"
                TIMEOUT 60
                RESOURCE_LOCK "arts_runtime"
                ENVIRONMENT "ARTS_CONFIG=${_cfg}"
                PASS_REGULAR_EXPRESSION "PASS: db_size_round_once"
                FAIL_REGULAR_EXPRESSION "FAIL:")
        endforeach()
    endforeach()
endif()

# db_create_minted_guid_refused: a create handed a GUID an earlier create
# minted refuses it and the run ends with the refusal.  The check precedes
# every cell's create path.
if(ARTS_TEST_REFERENCE_TREE)
    add_arts_test(db_create_minted_guid_refused)
    register_single_node_test(db_create_minted_guid_refused TIMEOUT 30 STATUS 1)
    set_tests_properties(db_create_minted_guid_refused PROPERTIES
        PASS_REGULAR_EXPRESSION "was minted by a create")
endif()

# quiescent_shutdown_no_residue: a shutdown that leaves no work undone -- every
# EDT touching a block inside a finish scope the shutting-down EDT waits on,
# and that EDT holding nothing -- must leave the teardown walk silent, so any
# QUIESCENCE-DEBUG marker fails it.  The shapes are the ones whose interrupted
# forms leave residue: on one rank, home-local write turns and write holders
# sharing a turn; with peers, every rank writes both counters, so each is
# written off its home (a release off the home waits for it where the arm
# publishes, requesters park, the home runs hand-overs), plus a remote write
# first touch.  Its writers are unordered, which DB-WRF does not admit, so
# every other arm runs it, wherever the walk is compiled.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH" AND ARTS_LOG_LEVEL GREATER_EQUAL 3)
register_single_node_test(quiescent_shutdown_no_residue TIMEOUT 60)
register_multinode_test(quiescent_shutdown_no_residue TIMEOUT 120)
foreach(_t quiescent_shutdown_no_residue quiescent_shutdown_no_residue_2n_9w_1p
        quiescent_shutdown_no_residue_4n_4w_1p
        quiescent_shutdown_no_residue_2n_8w_2p)
    # The quiescence walk only prints, so its markers are matched; a check's
    # FAIL: line is matched as a second channel beside the exit status.
    set_tests_properties(${_t} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL:;QUIESCENCE-DEBUG:")
endforeach()
endif()

# grant_one_request_per_grant: rounds of concurrent write acquisitions of one
# block off its home, each round starting with no write right there.  Every
# waiter is served (the count), and where a grant is handed back at release
# (write-through with PURGE) a Debug build asserts that no grant arrives to an
# empty waiting set.  Its writers are unordered, which DB-WRF does not admit,
# so every other arm runs it.  On the arms that share the grant plane (VAL,
# INV), whose decision points carry the scheduler fuzz, the shape with two
# progress threads also runs under it where the injection hooks are compiled,
# which widens the window between a waiter's park and the grant that takes the
# waiting set.
# The write right leaves rank 0 for rank 1 only, so a third rank adds nothing.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
add_arts_test(grant_one_request_per_grant)
register_multinode_test(grant_one_request_per_grant TIMEOUT 120
                        VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_v 2n_9w_1p 2n_8w_2p)
    set_tests_properties(grant_one_request_per_grant_${_v} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL:;QUIESCENCE-DEBUG:")
endforeach()
if(ARTS_DEBUG_INJECTION AND ARTS_COHERENCE_ARM MATCHES "^(VAL|INV)$")
    arts_test_cfg(_cfg 2n_8w_2p)
    arts_add_test(grant_one_request_per_grant_2n_8w_2p_fuzz
                  COMMAND grant_one_request_per_grant)
    set_tests_properties(grant_one_request_per_grant_2n_8w_2p_fuzz PROPERTIES
        LABELS "multinode"
        TIMEOUT 120
        RESOURCE_LOCK "arts_runtime"
        FAIL_REGULAR_EXPRESSION "FAIL:;QUIESCENCE-DEBUG:"
        ENVIRONMENT "ARTS_CONFIG=${_cfg};ARTS_SCHED_FUZZ=25")
endif()
endif()

# coherence_multi_writer_dist: write turns on many blocks from every rank at
# once, half the blocks homed off rank 0 and half on every rank, and an exact
# count per block read on rank 0.  At >=3 ranks a block homed on a non-zero
# rank has two distinct remote writer ranks; the two progress threads reorder
# one peer's hand-over messages.  Its writers are unordered, which DB-WRF does
# not admit.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_multinode_test(coherence_multi_writer_dist TIMEOUT 120)
foreach(_v 2n_9w_1p 4n_4w_1p 2n_8w_2p)
    set_property(TEST coherence_multi_writer_dist_${_v} APPEND PROPERTY
        LABELS "coherence_stress")
    set_tests_properties(coherence_multi_writer_dist_${_v} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL"
        PASS_REGULAR_EXPRESSION "PASS: coherence_multi_writer_dist")
endforeach()
endif()

# coherence_writer_ro_overlap: cross-rank write windows OVERLAPPED by
# concurrent RO readers on other ranks — the one regime the rest of the suite
# leaves uncovered (coherence_multi_writer_dist is RW-only across ranks; the
# large-payload and rwro_phase tests phase-separate writers from readers).  Runs
# of same-rank RW EDTs hold a write window open across several releases while readers
# stream in; the run boundaries migrate the write window so redirects race live
# hand-offs.  An ordinary OCR program: the header has a single event-ordered
# writer that every acquire must observe verbatim, and a round's writers all
# stamp the churn region with the same byte, so the only legal observations are
# this round's and the previous round's stamps.  A round's writers are
# unordered, which DB-WRF does not admit, so every other arm runs it.
# Single-node registration covers the all-local paths; the
# multinode variants carry the cross-rank ones.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_single_node_test(coherence_writer_ro_overlap TIMEOUT 120)
register_multinode_test(coherence_writer_ro_overlap TIMEOUT 180)
# Under ThreadSanitizer a multinode run's time goes to the reports its racing
# words raise against the provider's copies, each built and then suppressed,
# not to the protocol, so a THREAD tree budgets for that cost; the other trees
# keep the budget that detects a hang.
if(ARTS_DEBUG_SANITIZER STREQUAL "THREAD")
    foreach(_v IN LISTS ARTS_TEST_MULTINODE_VARIANTS)
        set_tests_properties(coherence_writer_ro_overlap_${_v} PROPERTIES
            TIMEOUT 330)
    endforeach()
endif()
set(_writer_ro_cases
    coherence_writer_ro_overlap
    coherence_writer_ro_overlap_2n_9w_1p
    coherence_writer_ro_overlap_4n_4w_1p
    coherence_writer_ro_overlap_2n_8w_2p)
foreach(_t IN LISTS _writer_ro_cases)
    set_property(TEST ${_t} APPEND PROPERTY LABELS "coherence_stress")
    # A protocol that ends the run early (spurious shutdown) would exit 0 with
    # nothing verified — require the end-of-run scalar.  Fail on any
    # acquirer's violation report, and on a wait-structure that survives to
    # teardown: a lost wake this run's own pass criteria may have masked.
    # One property, both patterns: a later set of FAIL_REGULAR_EXPRESSION
    # replaces an earlier one.
    set_tests_properties(${_t} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: rw-write window/ro-overlap rounds=16 dbs=8 reads=1024 writes=1152"
        FAIL_REGULAR_EXPRESSION "FAIL:;QUIESCENCE-DEBUG:")
endforeach()
endif()

# --- DB-WRF / FLUSH arm tests ------------------------------------------------
# What every remote acquire fetching and every remote RW release writing
# through owe: the home sees each ordered write, on one block and on many
# blocks homed on every rank at once, a creator away from the home fetches
# rather than reuses its create's bytes, a zero-size block resolves in both
# modes, a read inside a live write on the same rank leaves the writer's bytes
# alone, a size-less name is answered size-only and re-asked, a write-back
# with no credit asks for one, and reads racing write-backs are all served
# without disturbing them.  Whitebox (the spent credit is reached through the
# route table), and only this arm has those paths, so it is built and
# registered in the FLUSH tree alone.  Four ranks put the size-only asker and
# the racing readers on a rank that is neither the home nor the writer; two
# progress threads reorder the credit-less write-back's announce, landing, put
# and commit.
if(ARTS_COHERENCE_ARM STREQUAL "FLUSH")
    add_arts_test(wrf_flush)
    register_single_node_test(wrf_flush TIMEOUT 60)
    register_multinode_test(wrf_flush TIMEOUT 120)
    foreach(_t wrf_flush wrf_flush_2n_9w_1p wrf_flush_4n_4w_1p
            wrf_flush_2n_8w_2p)
        set_tests_properties(${_t} PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS: wrf_flush"
            FAIL_REGULAR_EXPRESSION "FAIL:")
    endforeach()
endif()

# The portable programs, as an OCR program sees the two funnels: a reader
# cohort with two readers on one rank, after a value written on one rank, and
# read turns of one block ending and beginning back to back on its home rank.
# UNGUARDED, because each asserts coherence alone: every arm answers the same
# oracle, and an arm that answers it differently is the thing worth catching.
# The cohort needs a rank that is not the block's home; the home-rank turns
# are all local, so peers add nothing to them.
register_cxl_multinode_test(cxl_ro_cohort TIMEOUT 60)
register_cxl_single_node_test(cxl_ro_turn_overlap TIMEOUT 60)

# The CXL store.  Registered only where this tree's own library keeps payloads
# in it: everywhere else arts_static holds no store symbols at all, and an
# unguarded entry would fail the BUILD of every other validation tree.
if(ARTS_CXL_TREE_RESIDENCY)
    # The seam over the shared arena: alignment, disjointness, containment,
    # and a fatal, named exhaustion.  One rank: the arena is one object.
    register_cxl_single_node_test(cxl_store_alloc TIMEOUT 60 FAIL_REGEX "FAIL")
    # The static strategy with a device other than 0: the one arena is index
    # 0 whatever device the cfg named.  The two cfg keys come through the
    # environment, which overrides the file.
    arts_test_cfg(_dev3_cfg 1n_20w)
    arts_add_test(cxl_store_alloc_static_dev3 COMMAND cxl_store_alloc)
    set_tests_properties(cxl_store_alloc_static_dev3 PROPERTIES
        LABELS "single_node" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_dev3_cfg};cxl_db_allocation_strategy=static;cxl_db_allocation_device=3"
        PASS_REGULAR_EXPRESSION "PASS cxl_store_alloc" FAIL_REGULAR_EXPRESSION "FAIL")
    # The slot rule holds on one rank and on every multinode flavour.  A
    # check's failure reaches the exit status and a FAIL line from any rank
    # fails it too, and the home exits non-zero when the deciding check never
    # ran, so a run that shut down before its checks cannot pass.
    register_cxl_single_node_test(cxl_slot_rule TIMEOUT 30)
    register_cxl_multinode_test(cxl_slot_rule TIMEOUT 60)
    if(ARTS_CXL_TREE_RESIDENCY STREQUAL "DIRECT")
        # A discarded slot leaves nothing naming it.  Where a descriptor
        # carries its payload it names no slot that can be discarded, so the
        # assertions hold there by address disjointness rather than by the
        # withdrawal they are written for -- a residency this test cannot
        # fail.  One rank: the cache it asserts on is private to the rank that
        # makes it, so peers add nothing.
        register_cxl_single_node_test(cxl_slot_discard_unadopt TIMEOUT 30)
        # A holder's working bytes ARE the block's slot: one address from the
        # create through every turn, and the bytes a turn writes are the bytes
        # the next turn reads.  Where each turn works in its own copy the
        # addresses differ by construction, so this asserts a residency rather
        # than a protocol.
        register_cxl_single_node_test(cxl_direct_payload_identity TIMEOUT 60)
        # The no-copy half of the same oracle: that address is the SAME on
        # every rank that holds it.  Needs more than one rank to say anything.
        register_cxl_multinode_test(cxl_direct_payload_identity TIMEOUT 120 VARIANTS 2n_9w_1p 4n_4w_1p)
    endif()
    # A cache made by an acquiring create is on the block's destroy roster, so
    # it stops naming the slot with the block instead of outliving it.  Needs
    # a rank that is not the block's home; ranks 0 and 1 are the whole case.
    register_cxl_multinode_test(cxl_creator_roster TIMEOUT 60
                                VARIANTS 2n_9w_1p 2n_8w_2p)
    # A block nobody has written is acquirable by a first user on every rank,
    # and one write turn then defines what all of them see.
    register_cxl_multinode_test(cxl_no_acquire_first_use TIMEOUT 60)
    # One label names one block and therefore one slot.  The timeout covers
    # eighty create/destroy cycles with the creator rotating over the ranks.
    register_cxl_multinode_test(cxl_labeled_create TIMEOUT 180)
    # One fetch and one purge per turn, on either residency.  One rank,
    # because that is where every turn is home-local.
    register_cxl_single_node_test(cxl_home_turn_costs TIMEOUT 30)
    # The arm's teardown walk: six blocks, every one released and every
    # dependence satisfied, must leave the walk silent -- a QUIESCENCE-DEBUG
    # marker on this shape is the oracle catching a real defect, not a test
    # artifact.  Registered wherever the walk is compiled.
    if(ARTS_LOG_LEVEL GREATER_EQUAL 3)
        register_cxl_multinode_test(cxl_quiescence_marker TIMEOUT 60 FAIL_REGEX "QUIESCENCE-DEBUG|FAIL")
    endif()
    # The store's two flushes, each at its one edge of a turn: a consumer
    # flush when a rank's first acquire is admitted, a producer flush when its
    # last write holder releases, nothing between and no producer flush after
    # a read turn.  Linked with the runtime whose adapter records its flushes.
    add_arts_test(cxl_edge_flush_order RUNTIME arts_static_cxl_flush_recorder)
    target_compile_definitions(cxl_edge_flush_order PRIVATE ARTS_CXL_FLUSH_RECORDER=1)
    register_cxl_single_node_test(cxl_edge_flush_order TIMEOUT 30)
    register_cxl_multinode_test(cxl_edge_flush_order TIMEOUT 60
                                VARIANTS 2n_9w_1p 2n_8w_2p)
endif()

# --- GPU tests ---

if(BUILD_CUDA_LIBRARY)
    register_gpu_test(gpu_edt_basic)
    register_gpu_test(gpu_edt_dep)
    register_gpu_test(gpu_edt_with_guid)
    register_gpu_test(gpu_edt_passthrough)
    register_gpu_test(gpu_memory)
    register_gpu_test(gpu_memset)
    register_gpu_test(gpu_db)
    register_gpu_test(gpu_lc_sync_basic)
    register_gpu_test(lc_sync)
    register_gpu_test(gpu_multi_kernel)
    register_gpu_test(gpu_for_all)
    register_gpu_test(gpu_lib)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/generated_tests.cmake)

# inv_round_snapshot: the pure per-word decision of an invalidation round's
# roster snapshot (who is retired, who stays).  INV only.
if(ARTS_COHERENCE_ARM STREQUAL "INV")
add_pure_unit_src(inv_round_snapshot DEFINES ARTS_UNIT_STANDALONE_SHIMS PASS_REGEX "PASS inv_round_snapshot:" TIMEOUT 60)
endif()

# config_cxl_alloc_strategy: the cxl_db_allocation_strategy handler, registered
# only in a CXL tree: the handler and the fields it sets exist only there, and
# the two residencies share it, so it runs in the STAGED tree.
if(ARTS_CXL_TREE_RESIDENCY STREQUAL "STAGED")
    add_arts_test(config_cxl_alloc_strategy)
    register_pure_unit_test(config_cxl_alloc_strategy TIMEOUT 60)
    # It links arts_static and so the store library, which maps its one
    # region when it loads.
    set_tests_properties(config_cxl_alloc_strategy PROPERTIES
        RESOURCE_LOCK "arts_runtime"
        PASS_REGULAR_EXPRESSION "PASS config_cxl_alloc_strategy: round_robin / static\\+dev3 / NULL->static,0")
endif()

# Layout probes.  A C++ translation unit (the CUDA sources, and the C sources
# a GPU build compiles as CUDA) shares the runtime's structs with the C
# runtime, and sees each _Atomic(T) member as std::atomic<T>.  A probe source
# prints one fingerprint line of sizes and member offsets; it is built once as
# C (registered with the tests above) and once as C++ through
# unit/<probe>_cpp.cpp, and <probe>_compare requires the two lines to match.
function(arts_layout_probe_cpp probe tag)
    add_executable(${probe}_cpp unit/${probe}_cpp.cpp)
    target_include_directories(${probe}_cpp PRIVATE
        ${ARTS_PUBLIC_INCLUDE_DIR}
        ${ARTS_INTERNAL_INCLUDE_DIR}
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
    arts_apply_protocol(${probe}_cpp
        ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})
    set_property(TARGET ${probe}_cpp PROPERTY POSITION_INDEPENDENT_CODE OFF)
    target_compile_options(${probe}_cpp PRIVATE ${ARTS_NON_PIE_COMPILE_OPTIONS})
    target_link_options(${probe}_cpp PRIVATE ${ARTS_NON_PIE_LINK_OPTIONS})
    target_link_libraries(${probe}_cpp PRIVATE Threads::Threads)
    register_pure_unit_test(${probe}_cpp TIMEOUT 60)
    set_tests_properties(${probe}_cpp PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS ${probe} \\[cpp\\]")
    add_test(
        NAME ${probe}_compare
        COMMAND ${CMAKE_COMMAND}
            "-DC_EXECUTABLE=$<TARGET_FILE:${probe}>"
            "-DCPP_EXECUTABLE=$<TARGET_FILE:${probe}_cpp>"
            "-DTAG=${tag}" "-DNAME=${probe}_compare"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/compare_layout_probes.cmake)
    set_tests_properties(${probe}_compare PROPERTIES
        LABELS "single_node"
        TIMEOUT 60
        RESOURCE_LOCK "pure_unit_test"
        PASS_REGULAR_EXPRESSION
            "PASS ${probe}_compare: C/C\\+\\+ fingerprints match")
endfunction()

# The cell's DB descriptors: every tree.
arts_layout_probe_cpp(db_cache_layout DB_LAYOUT)

# The protocol-independent runtime structs, so the reference tree alone.  The
# ARTS_USE_CXL member of the runtime state is therefore not fingerprinted.
if(ARTS_TEST_REFERENCE_TREE)
    arts_layout_probe_cpp(runtime_edt_event_layout EDT_EVENT_LAYOUT)
endif()

# The CXL slot field of the EXCL cache.  Registered only where this tree's
# own library keeps payloads in the store -- the fingerprint is meaningless
# with no cxl_addr member to compare.
if(ARTS_CXL_TREE_RESIDENCY)
    add_pure_unit_src(cxl_cache_fields PASS_REGEX "PASS cxl_cache_fields \\[c\\]"
                      TIMEOUT 30)
    arts_layout_probe_cpp(cxl_cache_fields CXL_LAYOUT)

    # Grades this tree's OWN objects, so it is registered only where the
    # library is store-backed: it asserts that the payload plane is compiled
    # out, which is true of no other tree.  CMake, not the script, says which
    # arms this tree builds -- the store-backed variants to grade beside the
    # tree's library, and whether the non-CXL build of the same source that
    # gives the check its control exists here at all.
    set(_cxl_graded "")
    foreach(_v ocr_excl_purge_cxl_staged ocr_excl_purge_cxl_direct)
        if(TARGET arts_static_${_v})
            list(APPEND _cxl_graded "arts_static_${_v}")
        endif()
    endforeach()
    set(_cxl_require_control OFF)
    if(TARGET arts_static_ocr_excl_purge)
        set(_cxl_require_control ON)
    endif()
    add_test(
        NAME cxl_no_payload_plane
        COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR}
                "-DGRADED_TARGETS=${_cxl_graded}"
                "-DREQUIRE_CONTROL=${_cxl_require_control}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cxl_no_payload_plane.cmake)
    set_tests_properties(cxl_no_payload_plane PROPERTIES
        LABELS "single_node"
        TIMEOUT 60
        RESOURCE_LOCK "pure_unit_test"
        PASS_REGULAR_EXPRESSION "PASS cxl_no_payload_plane")
endif()

# ============================================================================
# Cross-rank failure channel.
#
# ctest sees the MASTER rank's exit status, and the local launcher folds every
# rank it spawned into it: a rank that dies on a signal or exits non-zero makes
# the master's arts_rt() return non-zero.  A rank's own verdict therefore
# reaches ctest when that rank's main() returns arts_test_status() (see
# tests/test_failure_status.h).
#
# This gate is the second net, on every rank and single-node alike: a check
# whose failure branch only prints -- on any rank, since the launcher forwards
# every rank's stdout to the master's -- still fails the test by the token it
# prints.
#
# Conservative form (as in the PASS-gate sweep of generated_tests.cmake): the
# token matched is one these tests print ONLY on a failure branch — a clean run
# never emits it, and the runtime's own [ERROR] log aborts, so it cannot appear
# either.  Every variant a tree registers gets it; an explicit
# FAIL_REGULAR_EXPRESSION set above is left as it is.
# ============================================================================
set(_arts_fail_token_tests
    cat_c_ref_balance coherence_rwro_phase
    csr_graph ctx_created_db_release_order
    ctx_crossrank_proxy_finish ctx_owned_finish_cleanup db_acquire_all_bias
    db_create_claims_first_touch_stub db_create_with_data_bytes
    dbcreate_matrix edt_create_basic edt_sentinel_single_fire event_chain
    event_channel_fifo event_channel_lifetime event_channel_transient_null
    event_destroy_before_create excl_multiconsumer_queue excl_purge_grant_d6_d7
    excl_request_coalesce excl_samerank_rw_grant_race finish_event_stress
    grant_purge_churn grant_purge_handback_regrant
    grant_purge_no_acquire_creator guid_keygen_table_coupling
    launcher_no_respawn mark_edt_ready_idempotent multinode_edt
    multinode_event_types random_thread_safe round_robin_home_distribution
    runtime_barrier_counts scheduler_acquire_bias self_send_vs_dispatcher_parity
    stress_edt threads_pin_affinity)
foreach(_n IN LISTS _arts_fail_token_tests)
    foreach(_v "" _2n_9w_1p _4n_4w_1p _2n_8w_2p)
        if(TEST ${_n}${_v})
            get_test_property(${_n}${_v} FAIL_REGULAR_EXPRESSION _f)
            if(NOT _f)
                set_tests_properties(${_n}${_v} PROPERTIES
                    FAIL_REGULAR_EXPRESSION "FAIL")
            endif()
        endif()
    endforeach()
endforeach()

# The external descriptor's own test, in the one tree whose payload store is
# external: the representation it describes exists only there.
if(ARTS_CXL_TREE_RESIDENCY STREQUAL "DIRECT")
add_pure_unit_src(buffer_external_payload
    SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/buffer.c
            ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c
    PASS_REGEX "PASS buffer_external_payload"
    TIMEOUT 60)
else()
# A buffer's last drop after its block's descriptor is destroyed, in every tree
# whose payload is a buffer's.
add_pure_unit_src(buffer_outlives_cache
    SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/buffer.c
            ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c
    PASS_REGEX "PASS buffer_outlives_cache"
    TIMEOUT 60)
endif()

# Every test target that links the compiled ARTS library compiles against
# the tree's protocol too: a helper that registers such a target without
# applying the definitions would build its own translation units against a
# library it disagrees with, which is a silent ABI mismatch (a protocol-
# conditional struct laid out one way in the library and another way in the
# test) rather than a failed test.  Scoped to library-linking targets: a
# translation unit that includes a coherence layout without a complete cell
# does not compile, so the standalone tests that compile runtime sources or
# objects of their own are held to the same rule by the headers.
get_property(_arts_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_t IN LISTS _arts_test_targets)
    get_target_property(_kind ${_t} TYPE)
    if(NOT _kind STREQUAL "EXECUTABLE")
        continue()
    endif()
    get_target_property(_libs ${_t} LINK_LIBRARIES)
    if(NOT _libs MATCHES "arts_static|arts_cuda_static|arts_shared")
        continue()
    endif()
    get_target_property(_defs ${_t} COMPILE_DEFINITIONS)
    if(NOT _defs MATCHES "ARTS_PROTOCOL_")
        message(FATAL_ERROR
                "test target ${_t} was registered without "
                "arts_apply_protocol(): it would compile against a "
                "protocol the library does not implement")
    endif()
endforeach()
