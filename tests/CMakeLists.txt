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

# Many tests here are whitebox: they compile runtime TUs directly instead of
# linking an arts target, which is how they reach file-static symbols.  In a
# CXL build those TUs include ARTS internal headers that pull in <MemOps.h>,
# so the header path is declared once for the directory rather than per
# target: the ARTS_USE_CXL definition that demands it is itself directory-wide,
# and a per-target list would go stale the next time a test is added.  The CXL
# library itself (arts_free consults IS_CXL_PTR, the deque and lock headers
# call the flush intrinsics) reaches a test through arts_static's own link
# requirement; only a test that doesn't link an arts target needs it added
# explicitly, at that target.
if(ARTS_USE_CXL)
    include_directories(SYSTEM
        "${ARTS_CXL_INCLUDE_DIR}" "${ARTS_CXL_RAPID_INCLUDE_DIR}")
endif()

# Define test programs to build
set(TEST_PROGRAMS
    # --- Original tests ---
    route_table
    route_table_remote_guid
    coherence_basic
    coherence_rw_chain
    grant_purge_no_acquire_creator
    grant_purge_churn
    grant_purge_handback_regrant
    grant_purge_return
    coherence_rwro_phase
    coherence_rw_ordering
    coherence_rw_ro_rw
    coherence_rw_rw_chain_seal
    coherence_ro_accumulation
    coherence_mixed_local_remote
    coherence_destroy_during_acquire
    coherence_stress_single_node
    coherence_stress_dist
    coherence_payload_large
    coherence_satisfy_before_release
    coherence_multi_writer_same_addr
    coherence_multi_writer_multi_db
    coherence_ro_acquire_stress
    coherence_home_producer_ro
    coherence_zero_size_xfer
    coherence_rw_multihop
    coherence_rw_pipeline
    coherence_owner_ownership_reorder
    coherence_owner_confirm_gate
    lockfree_stack_stress
    lockfree_pool_stress
    route_table_install_race
    event_once_storm
    event_sticky_late_bind
    event_channel_fifo
    event_destroy_race
    pin_db
    active_message_db
    termination_detection
    finish_event_recursive
    remote_db_event
    route_table_iter
    route_table_iter_destroy
    acquire_mode
    # --- New unit tests ---
    edt_create_basic
    edt_chain
    edt_fan_out
    edt_signal
    edt_destroy
    stress_edt
    paramv_memcpy
    db_create
    db_create_with_data
    db_dependence
    db_destroy
    db_null_control
    db_null_alias_release
    event_value_control
    event_scalar_return
    event_payload_policy
    db_local
    db_local_create
    event_basic
    event_chain
    event_create_destroy_basic
    event_satisfy_once
    event_add_dep_idem_fastpath
    event_channel_lifetime
    event_idem_silent_oversatisfy
    event_channel_advanced
    guid_basic
    guid_range
    hint_routing
    node_query
    record_dep_at
    utility_api
    multinode_edt
    edt_create_oversize_guard
    db_no_acquire_remote_first_touch
    db_no_acquire_lazy
    db_no_acquire_wb_first_writer
    db_create_after_dep_asked
    db_first_touch_matrix
    db_acquire_release_semantics
    finish_event_mn_termination
    multinode_event
    multinode_coherence
    multinode_event_types
    # --- Graph tests ---
    csr
    distribution
    # --- Array DB tests ---
    # --- DB type tests ---
    db_pin
    # --- Termination tests ---
    term_shutdown
    term_abort
    term_sigfpe
    term_sigsegv
    # --- Multinode (built here, registered below) ---
    multinode_stdout_forward
    # --- Pure unit tests (no ARTS runtime) ---
    coherence_buffer_test
    grant_purge_word_model
    config_ports_contract
    config_fam_contract
    config_flux_setup
    port_block_relocation
    stdio_forward_scale_test
    regpool_basic
    regpool_guard
    regpool_node_avail
    regpool_node_fallover
    regpool_direct_oversize
    regpool_lazy_carve
    regpool_ladder
    regpool_evict_retired
    regpool_interleave
    regpool_fallback_order
    # --- New unit tests (Tasks 6-12) ---
    counter_smoke
    edt_dep_variants
    event_hint_presets
    db_labeled_guid
    multinode_labeled_guid
    multinode_event_hint
    per_db_buf_pool
    # --- Finish-EDT mechanism tests ---
    finish_event_basic
    finish_event_inherit
    finish_event_multinode
    finish_event_stress
    finish_event_wait
    finish_event_nested
    coherence_multi_writer_dist
    coherence_writer_ro_overlap
)

# CXL datablocks exist only in a CXL build, and there only under EXCL x PURGE
# (the top-level CMakeLists refuses every other pairing), so the test that
# exercises their grant chain is added to the list conditionally rather than
# guarded with #ifdef inside the source.
if(ARTS_USE_CXL)
    list(APPEND TEST_PROGRAMS coherence_cxl_grant_chain)
    list(APPEND TEST_PROGRAMS coherence_cxl_alias_slots)
endif()

# Fabric-attached memory: registered only where this tree's own library is
# FAM-enabled, so an unguarded entry does not need to compile against a tree
# with no fam symbols at all.  fam_slices is further restricted to the SHM
# backend, whose fixed pool address it dereferences.
if(ARTS_FAM_TREE_RESIDENCY)
    list(APPEND TEST_PROGRAMS fam_alloc_dist)
    list(APPEND TEST_PROGRAMS fam_slot_rule)
    if(ARTS_FAM_BACKEND STREQUAL "SHM")
        list(APPEND TEST_PROGRAMS fam_slices)
        # Names ARTS_FAM_POISON_BYTE through a strict-mode behaviour only the
        # inherited-mapping backend has, like fam_slices above it.
        list(APPEND TEST_PROGRAMS fam_strict_flush)
    endif()
endif()

# Function to create a test executable
function(add_arts_test test_name)
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

    # Match CARTS launcher expectations: non-PIE binaries avoid pointer-auth
    # failures when passing raw function pointers across nodes.
    set_property(TARGET ${test_name} PROPERTY POSITION_INDEPENDENT_CODE OFF)
    target_compile_options(${test_name} PRIVATE -fno-pie -fno-PIE)
    target_link_options(${test_name} PRIVATE -no-pie -fno-pie -fno-PIE)

    # Link with runtime libraries
    target_link_libraries(${test_name} PRIVATE Threads::Threads ${RT_LIBRARY} ${M_LIBRARY} ${ANL_LIBRARY})

    # Tests link the static archive, like the benchmarks: the runtime under
    # test is the build tree's by construction, with no dynamic resolution
    # point an environment (LD_LIBRARY_PATH) could redirect to another copy
    # at run time.
    target_link_libraries(${test_name} PRIVATE arts_static)
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
        target_compile_options(${test_name} PRIVATE -fno-pie -fno-PIE)
        target_link_options(${test_name} PRIVATE -no-pie -fno-pie -fno-PIE)

        # Link with CUDA libraries
        target_link_libraries(${test_name} PRIVATE
            Threads::Threads
            ${RT_LIBRARY}
            ${M_LIBRARY}
            ${ANL_LIBRARY}
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
# hands the result down.  So the suites no longer hand out bases by hand, and a
# straggler, an orphan, or an unrelated process squatting a port is absorbed by
# the runtime rather than poisoning the next test.  Do not reintroduce a fixed
# base here: a fixed base is exactly the shared state that made one test's
# leftovers everyone else's problem.

# Where a test's ARTS_CONFIG comes from.  A tree whose own library is
# FAM-enabled reads the fam root: every cfg there is its configs/local/test/
# namesake plus fam_pool_mb and fam_strict, so the whole suite runs with a pool
# sized for the tree instead of the 64 MB default.  ONE variable, used by every
# helper and by every hand-registered test -- a second spelling is how five
# tests end up on the wrong root.  CACHE INTERNAL, not a plain set(): the
# helpers below are called from outside this directory too (benchmarks/ocr_shim
# registers its tests through the same functions), and a plain directory-scoped
# variable is invisible there -- every call would silently resolve against an
# empty prefix instead of refusing to configure.
set(ARTS_TEST_CFG_DIR "${CMAKE_SOURCE_DIR}/configs/local/test"
    CACHE INTERNAL "cfg root every register_*_test call resolves ARTS_CONFIG against" FORCE)
if(ARTS_FAM_TREE_RESIDENCY)
    set(ARTS_TEST_CFG_DIR "${CMAKE_SOURCE_DIR}/configs/local/test/fam"
        CACHE INTERNAL "cfg root every register_*_test call resolves ARTS_CONFIG against" FORCE)
endif()

# A whole-suite pass under the strict oracle, which is what a full validation
# pass of a FAM tree asks for.  OFF by default: the DEFAULT pass must stay
# plain, because strict mode forwards every flush to the oracle and the real
# flush instructions would otherwise never execute.  A topology has a strict
# pass iff its twin exists, so the four twins are the single source of which
# topologies get one -- no list here to drift.
option(ARTS_FAM_TEST_STRICT
       "Point each test at its <variant>_strict.cfg twin where one exists" OFF)
if(ARTS_FAM_TEST_STRICT AND NOT (ARTS_FAM_TREE_RESIDENCY AND
                                 ARTS_FAM_BACKEND STREQUAL "SHM"))
    message(FATAL_ERROR
            "ARTS_FAM_TEST_STRICT needs a tree whose own library is FAM-enabled "
            "over the inherited mapping: the strict twins live only under "
            "configs/local/test/fam/ and the other backend makes fam_strict a "
            "config-load error")
endif()

# Resolves one variant name to the cfg a test should run.  Every site that
# names a cfg calls this; none spells a directory.
function(arts_test_cfg out variant)
    set(_c "${ARTS_TEST_CFG_DIR}/${variant}.cfg")
    if(ARTS_FAM_TEST_STRICT AND
       EXISTS "${ARTS_TEST_CFG_DIR}/${variant}_strict.cfg")
        set(_c "${ARTS_TEST_CFG_DIR}/${variant}_strict.cfg")
    endif()
    set(${out} "${_c}" PARENT_SCOPE)
endfunction()

function(register_single_node_test test_name)
    cmake_parse_arguments(ARG "" "TIMEOUT" "ARGS" ${ARGN})
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 10)
    endif()
    arts_test_cfg(_cfg 1n)
    add_test(
        NAME ${test_name}
        COMMAND ${test_name} ${ARG_ARGS}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(${test_name} PROPERTIES
        LABELS "single_node"
        TIMEOUT ${ARG_TIMEOUT}
        RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg}"
    )
endfunction()

function(register_multinode_test test_name)
    cmake_parse_arguments(ARG "" "TIMEOUT" "ARGS;VARIANTS" ${ARGN})
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 60)
    endif()
    if(NOT ARG_VARIANTS)
        # All four local multinode flavours.  Names map 1:1 to the cfg files in
        # configs/local/; each test's ARTS_CONFIG env points straight at the
        # source cfg (no copy), so ctest runs standalone.
        #   2n     — 2 nodes, 6w/1p per node (worker-heavy default)
        #   3n     — 3 nodes, 3w/1p per node
        #   4n     — 4 nodes, 2w/1p per node
        #   2n_io  — 2 nodes, 5w/2p per node (network-heavy variant:
        #            multiple progress threads break per-peer message FIFO,
        #            exercising concurrent cross-rank handler dispatch)
        set(ARG_VARIANTS 2n 3n 4n 2n_io)
    endif()
    foreach(variant IN LISTS ARG_VARIANTS)
        arts_test_cfg(_cfg ${variant})
        add_test(
            NAME ${test_name}_${variant}
            COMMAND ${test_name} ${ARG_ARGS}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        )
        set_tests_properties(${test_name}_${variant} PROPERTIES
            LABELS "multinode"
            TIMEOUT ${ARG_TIMEOUT}
            RESOURCE_LOCK "arts_runtime"
            ENVIRONMENT "ARTS_CONFIG=${_cfg}"
        )
    endforeach()
endfunction()


function(register_gpu_test test_name)
    if(ARTS_FAM_TREE_RESIDENCY)
        # No fam cfg root exists for the GPU topology, and a FAM tree's test
        # must name a sized pool; a GPU FAM tree is not a configuration this
        # effort validates.  Excluded at registration, not skipped at run time.
        message(STATUS "Skipping GPU test ${test_name}: a FAM-enabled tree has "
                       "no GPU cfg carrying fam_pool_mb")
        return()
    endif()
    cmake_parse_arguments(ARG "" "TIMEOUT" "ARGS" ${ARGN})
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 10)
    endif()
    add_test(
        NAME ${test_name}
        COMMAND ${test_name} ${ARG_ARGS}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(${test_name} PROPERTIES
        LABELS "gpu"
        TIMEOUT ${ARG_TIMEOUT}
        RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${CMAKE_SOURCE_DIR}/configs/local/gpu/1n.cfg;ASAN_OPTIONS=protect_shadow_gap=0:detect_leaks=0:alloc_dealloc_mismatch=0"
    )
endfunction()

# Pure unit tests: no ARTS runtime started, use separate resource lock.
function(register_pure_unit_test test_name)
    cmake_parse_arguments(ARG "" "TIMEOUT" "" ${ARGN})
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 30)
    endif()
    add_test(
        NAME ${test_name}
        COMMAND ${test_name}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
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

# EDT tests
register_single_node_test(edt_create_basic)
register_single_node_test(edt_chain)
register_single_node_test(edt_fan_out)
register_single_node_test(edt_signal)
register_single_node_test(edt_destroy)
register_single_node_test(stress_edt TIMEOUT 30)
register_single_node_test(paramv_memcpy)

# DB tests
register_single_node_test(db_create)
register_single_node_test(db_create_with_data)
register_single_node_test(db_dependence)
register_single_node_test(db_destroy)
register_single_node_test(db_local)
register_single_node_test(db_local_create)
register_single_node_test(db_no_acquire_lazy TIMEOUT 30)
register_single_node_test(db_no_acquire_wb_first_writer TIMEOUT 30)
set_tests_properties(db_no_acquire_wb_first_writer PROPERTIES
    FAIL_REGULAR_EXPRESSION "FAIL")
register_single_node_test(pin_db ARGS 0)

# Array DB tests

# Finish-event / sync tests
register_single_node_test(finish_event_recursive ARGS 3)
register_single_node_test(termination_detection ARGS 3)

# Event tests
register_single_node_test(event_basic)
register_single_node_test(db_null_control TIMEOUT 15)
register_single_node_test(db_null_alias_release TIMEOUT 15)
register_single_node_test(event_value_control TIMEOUT 15)
register_single_node_test(event_scalar_return TIMEOUT 30)
register_single_node_test(event_payload_policy TIMEOUT 15)
register_single_node_test(event_chain)
register_single_node_test(event_create_destroy_basic)
register_single_node_test(event_satisfy_once)
register_single_node_test(event_add_dep_idem_fastpath)
register_single_node_test(event_channel_lifetime)
register_single_node_test(event_idem_silent_oversatisfy)
register_single_node_test(event_channel_advanced)

# GUID tests
register_single_node_test(guid_basic)
register_single_node_test(guid_range)

# Routing / misc
register_single_node_test(hint_routing)
register_single_node_test(node_query)
register_single_node_test(record_dep_at)
register_single_node_test(utility_api)
register_single_node_test(route_table)
register_single_node_test(route_table_iter)
register_single_node_test(route_table_iter_destroy)
register_single_node_test(acquire_mode)

# Strict lock-free reserve_or_lookup stress
register_single_node_test(route_table_install_race TIMEOUT 120)

# Event redesign stress tests
register_single_node_test(event_once_storm TIMEOUT 60)
register_single_node_test(event_sticky_late_bind TIMEOUT 60)
register_single_node_test(event_channel_fifo TIMEOUT 60)
register_single_node_test(event_destroy_race TIMEOUT 60)

# Coherence tests — exercise the OCR-model ownership protocol paths
register_single_node_test(coherence_basic ARGS 32 0 0 0)
register_single_node_test(coherence_rw_chain)
register_single_node_test(coherence_rwro_phase)
register_single_node_test(coherence_rw_ordering)
register_single_node_test(coherence_rw_ro_rw)
register_single_node_test(coherence_rw_rw_chain_seal)
register_single_node_test(coherence_ro_accumulation)

# Graph tests
register_single_node_test(csr)
register_single_node_test(distribution)

# --- DB type tests ---
register_single_node_test(db_pin)

# --- Termination tests ---
register_single_node_test(term_shutdown)
set_tests_properties(term_shutdown PROPERTIES
    PASS_REGULAR_EXPRESSION "TERM_TEST_EXIT")

register_single_node_test(term_abort ARGS 0 42)
set_tests_properties(term_abort PROPERTIES
    PASS_REGULAR_EXPRESSION "TERM_ABORT rc=42")

# term_sigfpe / term_sigsegv crash via signal — CTest Exception short-circuits
# PASS_REGULAR_EXPRESSION before evaluating it.  Use a bash wrapper so the
# shell catches the signal exit and CTest sees exit-code 0; the regex then
# matches [ARTS] Crashed: SIG* in the captured output.  Hand-registered, so
# ARTS_CONFIG has to be set here too: without it the runtime falls back to
# ./arts.cfg in the working directory, which the build tree does not contain.
arts_test_cfg(_cfg 1n)
add_test(
    NAME term_sigfpe
    COMMAND bash -c "ASAN_OPTIONS=handle_sigfpe=0:handle_segv=0 ${CMAKE_CURRENT_BINARY_DIR}/term_sigfpe 2>&1; true"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set_tests_properties(term_sigfpe PROPERTIES
    LABELS "single_node"
    TIMEOUT 10
    RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg}"
    PASS_REGULAR_EXPRESSION "\\[ARTS\\] Crashed: SIGFPE")

arts_test_cfg(_cfg 1n)
add_test(
    NAME term_sigsegv
    COMMAND bash -c "ASAN_OPTIONS=handle_segv=0 ${CMAKE_CURRENT_BINARY_DIR}/term_sigsegv 2>&1; true"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set_tests_properties(term_sigsegv PROPERTIES
    LABELS "single_node"
    TIMEOUT 10
    RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg}"
    PASS_REGULAR_EXPRESSION "\\[ARTS\\] Crashed: SIGSEGV")

# --- Pure unit tests (no ARTS runtime) ---
register_pure_unit_test(coherence_buffer_test TIMEOUT 60)
# Exhaustive model check of the PURGE release policy's grant word decider
# (coherence/grant_purge.c); self-skips (PASS) outside a VAL/INV x PURGE build.
register_pure_unit_test(grant_purge_word_model TIMEOUT 30)
set_tests_properties(grant_purge_word_model PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS grant_purge_word_model")
# Forks to probe the ARTS_ERROR death paths of the port contract; loads cfgs
# only, so it binds nothing and needs no runtime.  Its fixtures name remote
# launchers, which a tree whose own library refuses them at config load (an
# inherited-mapping backend under the arm that uses it) cannot parse as far
# as the port contract, so that tree cannot host the test (the contract
# itself is the same everywhere).
if(NOT (ARTS_FAM_TREE_RESIDENCY AND ARTS_FAM_BACKEND STREQUAL "SHM"))
register_pure_unit_test(config_ports_contract TIMEOUT 30)
set_tests_properties(config_ports_contract PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS config_ports_contract")
endif()
# What the fabric-attached-memory cfg keys accept and refuse.  Unguarded,
# deliberately: it links no fam symbol (arts_config_load alone) and half its
# job is proving the two keys parse and stay inert in a tree with no FAM
# backend, which a guard would delete the evidence for.
register_pure_unit_test(config_fam_contract TIMEOUT 30)
set_tests_properties(config_fam_contract PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS config_fam_contract")
# Forks to probe the flux launcher arm's ARTS_ERROR death paths against a
# stub flux CLI the test writes itself; loads cfgs only, binds nothing.  Same
# exclusion as config_ports_contract: an inherited-mapping tree refuses the
# flux launcher at config load.
if(NOT (ARTS_FAM_TREE_RESIDENCY AND ARTS_FAM_BACKEND STREQUAL "SHM"))
register_pure_unit_test(config_flux_setup TIMEOUT 30)
set_tests_properties(config_flux_setup PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS config_flux_setup")
endif()
# Binds real sockets to occupy a block, then checks the selector moves off it.
register_pure_unit_test(port_block_relocation TIMEOUT 30)
set_tests_properties(port_block_relocation PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS port_block_relocation|SKIP port_block_relocation")
register_pure_unit_test(stdio_forward_scale_test)

# regpool_basic/regpool_guard assert the confinement invariant (every
# returned pointer resolves through arts_regpool_lookup to a registered
# slab), which only the arena-allocator implementation provides.  The
# ARTS_MALLOC=system fallback has no arena/slab concept — it is a plain
# pass-through allocator, so arts_regpool_lookup always misses there by
# design (see regpool.c's non-mimalloc branch).  Still built under every
# ARTS_MALLOC config for compile coverage; only run where the invariant
# they check actually holds.
if(ARTS_MALLOC STREQUAL "mimalloc")
    register_pure_unit_test(regpool_basic TIMEOUT 60)
    register_pure_unit_test(regpool_guard TIMEOUT 60)
    # Fallover needs the arena/slab machinery (a refused node, a fallback
    # arena, and the confinement lookup) — meaningless on the pass-through
    # allocator for the same reason as the two above.  The explicit
    # PASS/SKIP markers keep a skipped environment (one NUMA node) from
    # reading as an exercised pass.
    register_pure_unit_test(regpool_node_fallover TIMEOUT 60)
    set_tests_properties(regpool_node_fallover PROPERTIES
        PASS_REGULAR_EXPRESSION
        "PASS regpool_node_fallover|SKIP regpool_node_fallover")
    register_pure_unit_test(regpool_direct_oversize TIMEOUT 60)
    set_tests_properties(regpool_direct_oversize PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS regpool_direct_oversize")
    # Lazy carving is a property of the arena/slab machinery: exactly one
    # node carved at init, another carved by the first allocation of a
    # thread on it.  Needs >= 2 NUMA nodes and a CPU of a second node in
    # the process's affinity mask; the PASS/SKIP markers keep an
    # environment that has neither from reading as an exercised pass.
    register_pure_unit_test(regpool_lazy_carve TIMEOUT 60)
    set_tests_properties(regpool_lazy_carve PROPERTIES
        PASS_REGULAR_EXPRESSION
        "PASS regpool_lazy_carve|SKIP regpool_lazy_carve")
    # The slab ladder's exact rungs (a node's next slab is half again the
    # last one mapped for it, on the allocator's granule).
    register_pure_unit_test(regpool_ladder TIMEOUT 60)
    set_tests_properties(regpool_ladder PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS regpool_ladder")
    # Eviction of retired mappings and the unplaced fallback behind it,
    # driven by the force-full override (so it needs no memory pressure and
    # no second NUMA node).
    register_pure_unit_test(regpool_evict_retired TIMEOUT 60)
    set_tests_properties(regpool_evict_retired PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS regpool_evict_retired")
    # An oversize mapping is spread over the caller's node set, which needs
    # >= 2 NUMA nodes to be distinguishable from a placed one; the PASS/SKIP
    # markers keep a one-node environment from reading as an exercised pass.
    register_pure_unit_test(regpool_interleave TIMEOUT 60)
    set_tests_properties(regpool_interleave PROPERTIES
        PASS_REGULAR_EXPRESSION
        "PASS regpool_interleave|SKIP regpool_interleave")
    # The candidate order itself, against a supplied distance table — so it
    # asserts the ordering rule and not the machine it runs on.
    register_pure_unit_test(regpool_fallback_order TIMEOUT 60)
    set_tests_properties(regpool_fallback_order PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS regpool_fallback_order")
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
    set_test_log_level(net_rendezvous 2)
    target_link_libraries(net_rendezvous PRIVATE
        Threads::Threads mimalloc-static arts::libfabric ${RT_LIBRARY})
    register_pure_unit_test(net_rendezvous TIMEOUT 60)
    set_tests_properties(net_rendezvous PROPERTIES ENVIRONMENT "FI_PROVIDER=tcp")

    # net_src_bind — whitebox for arts_net_init's steering hints: net_interface
    # source-bind (loopback), fabric_domain pin, and the fatal missing-interface
    # contract.  Same standalone harness shape as net_selfsend.
    add_executable(net_src_bind unit/net_src_bind.c $<TARGET_OBJECTS:arts_regpool>)
    target_include_directories(net_src_bind PRIVATE
        ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
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
target_link_libraries(net_addr_frame_bound PRIVATE Threads::Threads)
register_pure_unit_test(net_addr_frame_bound)

# --- Foundation concurrency primitives (standalone — no ARTS runtime) ---
# Exercise arts_shared_ptr_t (shared.c) and the header-only Vyukov
# arts_mpsc_t in isolation.  These deliberately do NOT link the ARTS
# library: the shared-ptr tests provide their own libc-backed
# arts_calloc/arts_free shims, and the mpsc queue is header-only.  Keeping
# them library-independent lets the foundation be validated regardless of
# the runtime's build state.  libatomic + -mcx16 come from the global
# top-level setup.
set(SYNC_SHARED_TESTS shared_basic shared_atomic_race shared_aba)
set(SYNC_MPSC_TESTS mpsc_concurrent mpsc_drain_race)
foreach(t ${SYNC_SHARED_TESTS})
    add_executable(${t} unit/${t}.c
        ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c)
    target_include_directories(${t} PRIVATE
        ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
    target_link_libraries(${t} PRIVATE Threads::Threads)
    register_pure_unit_test(${t})
endforeach()
foreach(t ${SYNC_MPSC_TESTS})
    add_executable(${t} unit/${t}.c)
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
register_pure_unit_test(event_compute_next)

# --- Route-table slot reclamation (route_table.c) ---
# Standalone like the SYNC_* tests: the test TU includes route_table.c, guid.c
# and shared.c and supplies libc-backed allocator stubs, so no ARTS library
# link is needed.  route_table.c carries no protocol conditionals, so any one
# valid coherence combination satisfies ooo.h's arm check and the test covers
# every arm.  Drives concurrent duplicate destroy against install to assert
# the slot-return is single-flight: a caller that did not win must not free,
# drain or unbind a slot that another GUID now owns.  Also drives late
# re-reservations of destroyed GUIDs against racing lookups — the state in
# which a freed slot's key word can repeat a key it no longer answers for —
# pinning that a lookup verifies the object it pinned, not the word.
add_executable(route_table_reclaim_dupdestroy
    unit/route_table_reclaim_dupdestroy.c)
target_compile_definitions(route_table_reclaim_dupdestroy PRIVATE
    ARTS_WRITE_POLICY_WB=1)
target_include_directories(route_table_reclaim_dupdestroy PRIVATE
    ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
    ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
target_link_libraries(route_table_reclaim_dupdestroy PRIVATE Threads::Threads)
register_pure_unit_test(route_table_reclaim_dupdestroy TIMEOUT 120)

# --- Work-stealing deque (deque.c) + array_list (array_list.c) ---
# Standalone like the SYNC_* tests above: each compiles the target source TU
# directly and provides its own libc-backed arts_calloc_align/arts_free
# (deque) or arts_malloc/arts_free (array_list) + an arts_atomic_cswap_u64
# shim (deque), so no ARTS library link is needed.  The deque RACE tests
# (chaselev/tiebreak/grow) validate the exactly-once Chase-Lev contract;
# they are functionally green under ASan but DELIBERATELY trip ThreadSanitizer
# because deque.c uses the volatile + __sync_synchronize idiom rather than C11
# atomics (suspected bug B052 / weak-mem ordering) — that TSan report is a true
# property of the runtime, not a test defect.
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

# --- OoO defer/replay engine (ooo.c) ---
# Standalone like the SYNC_*/deque tests: each test TU #includes the runtime's
# ooo.c so the file-static g_ooo_table + engine bodies are compiled in, links
# shared.c for the cb shared-ptr slot, and supplies counting handler stubs +
# a single-slot arts_route_table_reserve_or_lookup + libc malloc shims (no ARTS
# runtime).  The per-config protocol macros (ARTS_PROTOCOL_*/ARTS_WRITE_POLICY_*/ARTS_RELEASE_*) are
# inherited from the top-level add_compile_definitions, so the g_ooo_table
# preprocessor arms + the tests' matching handler-stub arms select the same
# kind set for whichever protocol this build dir is configured for.  ooo.c pulls
# the generated counter Preamble.h, hence ARTS_BUILD_INTERNAL_INCLUDE_DIR.
set(OOO_UNIT_TESTS
    ooo_toctou_rescue ooo_drain_repush ooo_table_completeness)
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
register_pure_unit_test(ooo_toctou_rescue TIMEOUT 60)
register_pure_unit_test(ooo_drain_repush TIMEOUT 60)
register_pure_unit_test(ooo_table_completeness)

# --- atomics.c primitives + spin locks (C23p) ---
# Standalone like the SYNC_*/deque tests: link the runtime's atomics.c TU
# directly (libc-only dependency), no ARTS library.  atomics_rmw_conventions
# pins the per-function pre/post return conventions; atomics_rmw_contention
# proves the fetch_add tickets form a gap-free partition (linearizability);
# atomics_locks proves mutual-exclusion (no lost updates), the try_lock state
# machine, and the reader/writer no-overlap invariant, and documents B130
# (writer_try_lock blocks on reader drain).  atomics_locks is functionally
# green under ASan but DELIBERATELY trips ThreadSanitizer: atomics.c's RW lock
# uses plain `volatile` spin-hint reads of *write_lock/*read_lock racing with
# the barrier'd atomic RMWs on the same word — a true C-UB data race property
# of the runtime (benign on x86 TSO), not a test defect.
set(ATOMICS_UNIT_TESTS
    atomics_rmw_conventions atomics_rmw_contention atomics_locks)
foreach(t ${ATOMICS_UNIT_TESTS})
    add_executable(${t} unit/${t}.c
        ${CMAKE_SOURCE_DIR}/libs/src/core/utils/atomics.c)
    target_include_directories(${t} PRIVATE
        ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
    target_link_libraries(${t} PRIVATE Threads::Threads)
endforeach()
register_pure_unit_test(atomics_rmw_conventions)
register_pure_unit_test(atomics_rmw_contention TIMEOUT 60)
register_pure_unit_test(atomics_locks TIMEOUT 60)

# --- malloc.c allocator + footprint, diag.c timestamp/printf (C23p) ---
# Each test #includes the target runtime .c TU directly (with the heavy header
# include guards pre-defined + minimal ARTS_ERROR/ARTS_ALIGNED/footprint stubs
# inside the test), so no extra source and no ARTS library link is needed.
# malloc_alignment is the ASan target (heap safety + a-aligned user pointers);
# malloc_footprint_balance DELIBERATELY fails (B132): realloc shrink+free leaks
# (old-new) bytes from BYTES_MEMORY_FOOTPRINT because free subtracts only the
# shrunken header size — a true runtime accounting bug, left unmasked.
# diag_timestamp pins best-effort per-thread monotonicity + fold sanity and
# documents B133 (arts_printf prefix+body are two non-atomic stdio calls).
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
# malloc.c's ARTS_USE_CXL path calls into the device library directly; these
# tests don't link arts_static, so they need it named explicitly.
if(ARTS_USE_CXL AND TARGET arts_cxl_lib)
    foreach(t malloc_footprint_balance malloc_alignment)
        target_link_libraries(${t} PRIVATE arts_cxl_lib)
    endforeach()
endif()
register_pure_unit_test(malloc_footprint_balance TIMEOUT 60)
register_pure_unit_test(malloc_alignment)
register_pure_unit_test(diag_timestamp TIMEOUT 60)

# Counter smoke test: verifies counter output files are written.
register_single_node_test(counter_smoke TIMEOUT 30)
set_tests_properties(counter_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "COUNTER_SMOKE_PASS")

register_single_node_test(edt_dep_variants)

register_single_node_test(event_hint_presets)

register_single_node_test(db_labeled_guid)

register_pure_unit_test(per_db_buf_pool)

# --- Finish-EDT mechanism tests ---
register_single_node_test(finish_event_basic)
register_single_node_test(finish_event_inherit)
register_single_node_test(finish_event_wait TIMEOUT 30)
set_tests_properties(finish_event_wait PROPERTIES
    PASS_REGULAR_EXPRESSION "OK wait-style")
register_single_node_test(finish_event_nested)
set_tests_properties(finish_event_nested PROPERTIES
    PASS_REGULAR_EXPRESSION "OK nested")
register_multinode_test(finish_event_multinode TIMEOUT 30)
set_tests_properties(finish_event_multinode_2n PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: cross-node finish-scope drained")

register_multinode_test(finish_event_stress TIMEOUT 300)
set_tests_properties(finish_event_stress_2n PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: 100 iterations of recursive cross-node finish-EDT")

# --- Multi-node tests ---

register_multinode_test(multinode_edt)
register_multinode_test(db_null_control TIMEOUT 15)
register_multinode_test(db_null_alias_release TIMEOUT 15)
register_multinode_test(event_value_control TIMEOUT 15)
register_multinode_test(event_scalar_return TIMEOUT 30)
register_multinode_test(event_payload_policy TIMEOUT 15)
register_multinode_test(db_no_acquire_remote_first_touch TIMEOUT 60)
# A block created without an acquire has its payload allocated by its first
# user, so the same program has to hold on one rank (the first user is local)
# and on several (the home materializes the block to serve the request).
register_multinode_test(db_no_acquire_lazy TIMEOUT 60)
# A block whose payload never exists at its home: the first user is a remote
# WRITER, so the home answers the ownership request with no bytes and the
# holder mints the block's first image itself.  The two readers catch the two
# ways that goes wrong — a NULL pointer for a sized block, and a first image
# minted at the stamp the writer's own release uses (which makes the real
# bytes retreat as stale).
register_multinode_test(db_no_acquire_wb_first_writer TIMEOUT 60)
foreach(_v 2n 3n 4n 2n_io)
    set_tests_properties(db_no_acquire_wb_first_writer_${_v} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
# The unordered reader's whole point is a race, so one shape also runs with
# the scheduler-fuzz injection that widens the lock-free decision-commit
# windows (compiled out of Release, where the variable is simply inert).
# Three nodes, because the racing read has to reach the home from a rank that
# is neither the home nor the writer — with two there is no such rank and the
# window cannot be reached at all.
arts_test_cfg(_cfg 3n)
add_test(
    NAME db_no_acquire_wb_first_writer_3n_fuzz
    COMMAND db_no_acquire_wb_first_writer
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set_tests_properties(db_no_acquire_wb_first_writer_3n_fuzz PROPERTIES
    LABELS "multinode"
    TIMEOUT 120
    RESOURCE_LOCK "arts_runtime"
    FAIL_REGULAR_EXPRESSION "FAIL"
    ENVIRONMENT "ARTS_CONFIG=${_cfg};ARTS_SCHED_FUZZ=25")
# A create that lands where a dependence on the same label has already asked
# the block's home about it: the create may install the block, but it may not
# take the write right out from under a request already in flight.  Both
# request kinds get a lane (RW opens an ownership request, RO a fetch or a
# snapshot), so this needs a rank that is not the home.
register_multinode_test(db_create_after_dep_asked TIMEOUT 60)
foreach(_v 2n 3n 4n 2n_io)
    set_tests_properties(db_create_after_dep_asked_${_v} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
register_multinode_test(db_first_touch_matrix TIMEOUT 120)
# The alias dedup with the aliasing EDT OFF the block's home.  On the home rank
# every way of asking for the block answers with the rank's own copy, so a
# second acquisition there is indistinguishable from the first and the
# home-rank test can only state the rule; off the home a second acquisition is
# a second copy at a second address whose release can erase the EDT's write.
# Every arm owes the same answer, so it runs in all of them.
add_arts_test(db_alias_dedup_remote)
register_multinode_test(db_alias_dedup_remote TIMEOUT 60)
foreach(_v 2n 3n 4n 2n_io)
    set_tests_properties(db_alias_dedup_remote_${_v} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL:"
        PASS_REGULAR_EXPRESSION "PASS: db_alias_dedup_remote")
endforeach()
# A second create of a label that already exists, from a rank that is NOT the
# label's home.  There the route-table install cannot arbitrate — the first
# create's descriptor is already there — so the answer comes from the rank's
# own hold word, which every arm keeps in its own state.  First-wins is the
# contract in all of them, so it runs in all of them; the single-node entry
# covers the home-rank arbitration by the same program.
add_arts_test(db_labeled_create_first_wins_remote)
register_single_node_test(db_labeled_create_first_wins_remote TIMEOUT 30)
register_multinode_test(db_labeled_create_first_wins_remote TIMEOUT 60)
foreach(_t db_labeled_create_first_wins_remote
           db_labeled_create_first_wins_remote_2n
           db_labeled_create_first_wins_remote_3n
           db_labeled_create_first_wins_remote_4n
           db_labeled_create_first_wins_remote_2n_io)
    set_tests_properties(${_t} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL:"
        PASS_REGULAR_EXPRESSION "PASS: db_labeled_create_first_wins_remote")
endforeach()
# The same contract one lifetime later: the second create comes after the
# first one's hold was released AND after another rank took the block, the
# state in which a hold answered from a permission word rather than from the
# rank's create mark would be possession the home never granted.  Every arm
# owes the same answer, so it runs in all of them.
add_arts_test(db_labeled_create_second_after_release_remote)
register_single_node_test(db_labeled_create_second_after_release_remote TIMEOUT 30)
register_multinode_test(db_labeled_create_second_after_release_remote TIMEOUT 60)
foreach(_t db_labeled_create_second_after_release_remote
           db_labeled_create_second_after_release_remote_2n
           db_labeled_create_second_after_release_remote_3n
           db_labeled_create_second_after_release_remote_4n
           db_labeled_create_second_after_release_remote_2n_io)
    set_tests_properties(${_t} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL:"
        PASS_REGULAR_EXPRESSION "PASS: db_labeled_create_second_after_release_remote")
endforeach()
register_multinode_test(db_acquire_release_semantics TIMEOUT 120)
# The guard aborts the run by design, so the message is the verdict and
# the exit status is not; one node shape states it as well as four.
register_multinode_test(edt_create_oversize_guard VARIANTS 2n TIMEOUT 60)
set_tests_properties(edt_create_oversize_guard_2n PROPERTIES
    PASS_REGULAR_EXPRESSION "over the [0-9]+-byte control-message bound")
register_multinode_test(finish_event_mn_termination)
register_multinode_test(multinode_event)
register_multinode_test(active_message_db ARGS 64)
register_multinode_test(route_table_remote_guid)
register_multinode_test(remote_db_event)
register_multinode_test(multinode_coherence)
register_multinode_test(multinode_event_types)
register_multinode_test(coherence_mixed_local_remote)
register_multinode_test(coherence_home_producer_ro TIMEOUT 30)
register_multinode_test(coherence_zero_size_xfer TIMEOUT 30)
# These assert what the runtime's exclusive-RW serialization guarantees; under
# DB-WRF that ordering is the program's, and these programs deliberately omit
# it.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_multinode_test(coherence_rw_multihop TIMEOUT 30)
register_multinode_test(coherence_rw_pipeline TIMEOUT 40)
endif()
register_multinode_test(coherence_rw_chain TIMEOUT 30)
# A NO_ACQUIRE create whose creator, home and writer are three different ranks:
# the home-side create handler must end naming ITSELF as the holder on every
# arm, or the writer's request reaches a rank that holds nothing and the run
# times out.  Multinode only — one rank cannot separate the three roles, and
# the 2n variants self-skip for the same reason.
register_multinode_test(grant_purge_no_acquire_creator TIMEOUT 60)
# Write-right churn with home-local turns overlapping remote ones.  It asserts
# an exact sum, which every release policy must compute, so it runs at every
# release policy; it is also where interleaving injection is aimed.
# These assert what the runtime's exclusive-RW serialization guarantees; under
# DB-WRF that ordering is the program's, and these programs deliberately omit
# it.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_single_node_test(grant_purge_churn TIMEOUT 60)
register_multinode_test(grant_purge_churn TIMEOUT 90)
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
register_multinode_test(coherence_rwro_phase TIMEOUT 30)
# RO-side coherence is a MULTINODE-only code path: on one rank home, owner and
# self coincide, so a read acquire never emits a REQUEST/FORWARD/DELIVER at all.
# These three were registered single-node only, which meant they exercised none
# of the read plane they are named for.
# The read-side twin of grant_sticky: a read grant must outlive its readers
# under the retaining policy.  No value oracle can see this — a purging
# implementation computes identical answers and merely re-fetches — so it needs
# an assertion of its own.  Self-skips outside EXCL+RETAIN.
# The silent-wrong-answer gate for retention: an owner writes AFTER lending a
# read copy, and the lender reads again strictly after that write.  A rank that
# keeps a stale copy answers the old value — no crash, no hang.  Portable, so
# it guards every arm, not just the retaining one.
add_arts_test(coherence_owner_write_after_lend)
register_multinode_test(coherence_owner_write_after_lend TIMEOUT 30)

add_arts_test(ro_grant_retained)
register_multinode_test(ro_grant_retained TIMEOUT 30)

register_multinode_test(coherence_rw_ro_rw TIMEOUT 30)
register_multinode_test(coherence_ro_accumulation TIMEOUT 30)
register_multinode_test(coherence_ro_acquire_stress TIMEOUT 90)

register_multinode_test(multinode_stdout_forward)
set_tests_properties(multinode_stdout_forward_2n PROPERTIES
    PASS_REGULAR_EXPRESSION "MULTINODE_STDOUT_FORWARD_SENTINEL 42")

register_multinode_test(multinode_labeled_guid TIMEOUT 30)
register_multinode_test(multinode_event_hint TIMEOUT 30)

# B.3 LOCK_REQ-before-DB_CREATE race regression is registered at 3n/4n (where
# the race is actually exercised) as `excl_req_before_create` in
# generated_tests.cmake.  The former 2n single-entry `coherence_lock_req_before_create`
# here was a no-op (SKIPs at rank<3) over byte-identical source, so it was removed.

# Coherence stress test suite (Spec section 6 B.1/B.4/B.5/B.6 single-node).
# Each adds the "coherence_stress" label on top of "single_node" so the suite
# can be filtered with `ctest -L coherence_stress`.
register_single_node_test(coherence_destroy_during_acquire TIMEOUT 60)
set_property(TEST coherence_destroy_during_acquire APPEND PROPERTY
    LABELS "coherence_stress")
register_single_node_test(coherence_stress_single_node TIMEOUT 120)
set_property(TEST coherence_stress_single_node APPEND PROPERTY
    LABELS "coherence_stress")
register_single_node_test(coherence_multi_writer_same_addr TIMEOUT 60)
set_property(TEST coherence_multi_writer_same_addr APPEND PROPERTY
    LABELS "coherence_stress")
register_single_node_test(coherence_multi_writer_multi_db TIMEOUT 60)
set_property(TEST coherence_multi_writer_multi_db APPEND PROPERTY
    LABELS "coherence_stress")
register_single_node_test(coherence_ro_acquire_stress TIMEOUT 60)
set_property(TEST coherence_ro_acquire_stress APPEND PROPERTY
    LABELS "coherence_stress")

# Lock-free stack stress (Task 4h): 8 producers x 1M nodes, single
# consumer reverse_drain.  Pure unit test — does not start the ARTS
# runtime, but RESOURCE_LOCK is harmless and keeps it serialized with
# other tests under ctest -j.
register_single_node_test(lockfree_stack_stress TIMEOUT 60)

# Lock-free pool stress (Task 4m): DWCAS tagged-head Treiber stack ABA
# scenario + batch fetch / release contention.  Pure unit test, no
# runtime needed.
register_single_node_test(lockfree_pool_stress TIMEOUT 60)

# Distributed coherence stress: B.2.  Uses arts_2n.cfg (the test SKIPs
# cleanly if rank_count < 2).
arts_test_cfg(_cfg 2n)
add_test(
    NAME coherence_stress_dist
    COMMAND coherence_stress_dist
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set_tests_properties(coherence_stress_dist PROPERTIES
    LABELS "coherence_stress;multinode"
    TIMEOUT 120
    RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg}"
)

# Large-payload cross-rank coherence transfer: a ~256 KiB DB is stamped by a
# home-rank writer and RW-verified by workers on other ranks, so every hop ships
# the whole payload over the wire.  This is the guard for the transport's
# message framing — a payload split across frames the connectionless RX cannot
# reassemble corrupts silently (checksum mismatch -> FAIL) or mis-dispatches
# (spurious shutdown -> TIMEOUT); the sizeof(int) DBs elsewhere never reach it.
# Standard MN variants (2n/3n/4n/2n_io); SKIPs cleanly at rank_count < 2.
register_multinode_test(coherence_payload_large TIMEOUT 120)
foreach(_v 2n 3n 4n 2n_io)
    set_property(TEST coherence_payload_large_${_v} APPEND PROPERTY
        LABELS "coherence_stress")
    # A framing bug can mis-dispatch the raw payload frame -> spurious shutdown,
    # ending the run cleanly (exit 0) BEFORE any worker verifies.  Require the
    # end-of-run PASS line so that "verification never completed" fails, and
    # fail on any worker's corruption abort.
    set_tests_properties(coherence_payload_large_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: 4 DBs"
        FAIL_REGULAR_EXPRESSION "FAIL:")
endforeach()

# Add-dependence-then-release hand-off: consumers whose slots satisfied BEFORE
# the producer's release must observe the release-published bytes.  A DB whose
# home is neither the creator nor pre-seeded exposes a serve-early bug as a
# consumer NULL/zero read (FAIL/abort — rank death still exits 0 through the
# survivor's clean shutdown, so require the PASS line) or as a never-woken
# parked acquire (missing PASS -> timeout).
# These assert what the runtime's exclusive-RW serialization guarantees; under
# DB-WRF that ordering is the program's, and these programs deliberately omit
# it.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_multinode_test(coherence_satisfy_before_release TIMEOUT 60)
foreach(_v 2n 3n 4n 2n_io)
    set_property(TEST coherence_satisfy_before_release_${_v} APPEND PROPERTY
        LABELS "coherence_stress")
    set_tests_properties(coherence_satisfy_before_release_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: 4 DBs observed|SKIP"
        FAIL_REGULAR_EXPRESSION "FAIL:")
endforeach()
endif()

# Lazy-protocol ownership-transfer TRANSFER/INVALIDATE two-wire reorder stress.
# MUST use
# a progress_threads>=2 config (arts_2n_io.cfg = 2 nodes, 5w/2p) so the
# TRANSFER_OWNERSHIP(+1) and follow-up INVALIDATE(-1) to the same just-granted
# owner can be dispatched out of order by different progress threads — the
# window the commutative-signed writer_count fix closes.  Drives a few thousand
# ownership transfers and PASSES on chain completion (no transfer lost = no
# stranded RW acquirer); a lost transfer would strand a queued acquirer and hang
# (the regression), caught by the test's watchdog under this TIMEOUT.  The exact
# end-sum is a diagnostic only, not a gate (a cross-rank publish can be dropped
# by a separate FIFO-ordering defect orthogonal to writer_count — see the test
# file header).  SKIPs cleanly when rank_count < 2 or under the RELAXED model
# (no exclusive-RW chain).
# These assert what the runtime's exclusive-RW serialization guarantees; under
# DB-WRF that ordering is the program's, and these programs deliberately omit
# it.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
arts_test_cfg(_cfg 2n_io)
add_test(
    NAME coherence_owner_ownership_reorder
    COMMAND coherence_owner_ownership_reorder
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
set_tests_properties(coherence_owner_ownership_reorder PROPERTIES
    LABELS "coherence_stress;multinode"
    TIMEOUT 120
    RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg}"
)
endif()

# OWNER-placement stale-RO ownership-confirm gate: RO acquire causally ordered after a
# remote RW writer must observe the writer's value, not a stale snapshot from
# the old owner.  The race window is home's rw_holder lag behind INSTALL_ACK.
# The 2n_io config (multiple progress threads -> wire reorder) exposes it;
# 1n passes trivially (no ownership transfer).  RELAXED SKIPs cleanly.
register_single_node_test(coherence_owner_confirm_gate TIMEOUT 120)
register_multinode_test(coherence_owner_confirm_gate TIMEOUT 120)

# coherence_cxl_grant_chain: the CXL permission-only chain — remote write then
# remote read, a concurrent reader cohort, and a writer that must wait the
# cohort out.  On one rank the home is local, but every transition still runs
# through the real arbiter and both flush edges, so this is a genuine
# regression signal and not a no-op.
#
# The multinode variants are registered DISABLED rather than omitted, because
# what blocks them is the emulation and not the protocol.  A non-zero rank
# joins the shared CXL segment through arts_cxl_deque_get(), which is
# LAST_SHARED_MALLOC — "hand me the allocation rank 0 already made".  The
# fake_arts_cxl_lib implements that as a plain bump allocation, so every
# non-zero rank receives a fresh zeroed block instead of rank 0's deque and
# dies in arts_runtime_init with "CXL DB arena is missing or uninitialized",
# before any datablock exists.  Enable these the moment the CXL library in use
# can hand a second process the first process's allocation.
if(ARTS_USE_CXL)
    register_single_node_test(coherence_cxl_grant_chain TIMEOUT 60)
    set_tests_properties(coherence_cxl_grant_chain PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")
    register_multinode_test(coherence_cxl_grant_chain TIMEOUT 120 VARIANTS 2n 3n 4n)
    foreach(_v 2n 3n 4n)
        set_tests_properties(coherence_cxl_grant_chain_${_v} PROPERTIES
            FAIL_REGULAR_EXPRESSION "FAIL"
            DISABLED TRUE)
    endforeach()
    register_single_node_test(coherence_cxl_alias_slots TIMEOUT 60)
    set_tests_properties(coherence_cxl_alias_slots PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")
endif()

# coherence_multi_writer_dist: multinode cross-rank RW same-address.  The
# requester is NOT the home rank, so increments cross the wire (LOCK_REQUEST/
# GRANT or each protocol's equivalent) and the final RO read must observe every
# writer's atomic update.  An ordinary OCR program — registered under every
# protocol; at >=3 ranks a DB homed on a non-zero rank has two distinct remote
# writer ranks, exercising the cross-rank publish handoff that 2n cannot.
# These assert what the runtime's exclusive-RW serialization guarantees; under
# DB-WRF that ordering is the program's, and these programs deliberately omit
# it.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_multinode_test(coherence_multi_writer_dist TIMEOUT 120 VARIANTS 2n 3n 4n)
foreach(_v 2n 3n 4n)
    set_property(TEST coherence_multi_writer_dist_${_v} APPEND PROPERTY
        LABELS "coherence_stress")
endforeach()
endif()

# coherence_writer_ro_overlap: cross-rank RW write write windows OVERLAPPED by
# concurrent RO readers on other ranks — the one regime the rest of the suite
# leaves uncovered (coherence_stress_dist is RW-only across ranks; the
# large-payload and rwro_phase tests phase-separate writers from readers).  Runs
# of same-rank RW EDTs hold a write window open across several releases while readers
# stream in; the run boundaries migrate the write window so redirects race live
# hand-offs.  An ordinary OCR program: the header has a single event-ordered
# writer that every acquire must observe verbatim, and a round's writers all
# stamp the churn region with the same byte, so the only legal observations are
# this round's and the previous round's stamps.  Both assertions survive a
# whole-DB last-writer-wins publish, so it is registered under every arm.
# Single-node registration covers the all-local paths; the
# multinode variants carry the cross-rank ones.
register_single_node_test(coherence_writer_ro_overlap TIMEOUT 120)
register_multinode_test(coherence_writer_ro_overlap TIMEOUT 180)
set(_writer_ro_cases
    coherence_writer_ro_overlap
    coherence_writer_ro_overlap_2n
    coherence_writer_ro_overlap_3n
    coherence_writer_ro_overlap_4n
    coherence_writer_ro_overlap_2n_io)
foreach(_t IN LISTS _writer_ro_cases)
    set_property(TEST ${_t} APPEND PROPERTY LABELS "coherence_stress")
    # A wait-structure that survives to teardown is a lost wake this run's
    # own pass criteria may have masked — the runtime prints the marker,
    # the stress oracles turn it into a failure.
    set_property(TEST ${_t} APPEND PROPERTY
        FAIL_REGULAR_EXPRESSION "QUIESCENCE-DEBUG:")
    # A protocol that ends the run early (spurious shutdown) would exit 0 with
    # nothing verified — require the end-of-run scalar, and fail on any
    # acquirer's violation report.
    set_tests_properties(${_t} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: rw-write window/ro-overlap rounds=16 dbs=8 reads=1024 writes=1152"
        FAIL_REGULAR_EXPRESSION "FAIL:")
endforeach()

# --- DB-WRF / FLUSH arm tests ------------------------------------------------
# Portable OCR programs under ocr/, plus one whitebox program under unit/ that
# reaches the descriptor through the route table.  Their assertions hold only
# where every remote acquire fetches and every remote RW release writes
# through, so the entries are registered for the FLUSH arm; the binaries build
# in every tree.
add_arts_test(wrf_flush_writer_reader)
# A block created away from its home: the creator's hold is a write acquisition
# whose release must reach the home, and the next write acquisition on the
# creating rank must fetch rather than reuse what the create left behind.
add_arts_test(wrf_flush_creator_remote_home)
# A create that takes no hold: the home owes its first asker real storage, and
# the asker's write-back must return every byte.
add_arts_test(wrf_flush_no_acquire_first_touch)
# A block with no bytes still has to resolve both access modes.
add_arts_test(wrf_flush_zero_size)
# A long strictly ordered chain of remote write acquisitions on one block.
add_arts_test(wrf_flush_chain)
# A read acquisition landing inside a live write acquisition of the same block
# on the same rank must not disturb the writer's bytes.
add_arts_test(wrf_flush_private_copy)
# Whitebox: a write-back whose landing credit is gone must ask for one and
# still deliver the bytes.  Self-skips where the field does not exist.
add_arts_test(wrf_flush_announce)
# A block whose name carries no size: the first fetch on a rank is answered
# size-only and re-asked with a landing, in both access modes.
add_arts_test(wrf_flush_size_discovery)
if(ARTS_COHERENCE_ARM STREQUAL "FLUSH")
    register_single_node_test(wrf_flush_writer_reader TIMEOUT 30)
    register_multinode_test(wrf_flush_writer_reader TIMEOUT 60)
    register_single_node_test(wrf_flush_creator_remote_home TIMEOUT 30)
    register_multinode_test(wrf_flush_creator_remote_home TIMEOUT 60)
    register_single_node_test(wrf_flush_no_acquire_first_touch TIMEOUT 30)
    register_multinode_test(wrf_flush_no_acquire_first_touch TIMEOUT 60)
    register_single_node_test(wrf_flush_chain TIMEOUT 60)
    register_multinode_test(wrf_flush_chain TIMEOUT 120)
    register_single_node_test(wrf_flush_private_copy TIMEOUT 30)
    register_multinode_test(wrf_flush_private_copy TIMEOUT 60)
    # Nothing to separate on one rank: the acquirer IS the home, so the
    # zero-size fetch/announce legs are only reachable multinode.
    register_multinode_test(wrf_flush_zero_size TIMEOUT 60 VARIANTS 2n 3n 4n 2n_io)
    # Same reason: the spent credit only matters to a non-home releaser.
    register_multinode_test(wrf_flush_announce TIMEOUT 60 VARIANTS 2n 3n 4n 2n_io)
    # And the size-only round is asked only by a rank that is not the home.
    register_multinode_test(wrf_flush_size_discovery TIMEOUT 60 VARIANTS 2n 3n 4n 2n_io)
    foreach(_base wrf_flush_writer_reader wrf_flush_creator_remote_home
                  wrf_flush_no_acquire_first_touch wrf_flush_chain
                  wrf_flush_private_copy)
        foreach(_t ${_base} ${_base}_2n ${_base}_3n ${_base}_4n ${_base}_2n_io)
            set_tests_properties(${_t} PROPERTIES PASS_REGULAR_EXPRESSION "DB_WRF: PASS"
                                 FAIL_REGULAR_EXPRESSION "FAIL:")
        endforeach()
    endforeach()
    foreach(_base wrf_flush_zero_size wrf_flush_announce wrf_flush_size_discovery)
        foreach(_t ${_base}_2n ${_base}_3n ${_base}_4n ${_base}_2n_io)
            set_tests_properties(${_t} PROPERTIES PASS_REGULAR_EXPRESSION "DB_WRF: PASS"
                                 FAIL_REGULAR_EXPRESSION "FAIL:")
        endforeach()
    endforeach()
endif()

# Fabric-attached memory.  Registered only where this tree's own library is
# FAM-enabled: everywhere else arts_static holds no arts_fam_* at all, and an
# unguarded entry would fail the BUILD of all nine existing validation trees.
if(ARTS_FAM_TREE_RESIDENCY)
    # fam_alloc_dist names no backend-specific symbol, so the residency guard
    # alone is right for it -- deliberately, and the asymmetry with fam_slices
    # below is the point.
    register_multinode_test(fam_alloc_dist TIMEOUT 120 VARIANTS 2n 4n)
    set_tests_properties(fam_alloc_dist_2n fam_alloc_dist_4n PROPERTIES
                         FAIL_REGULAR_EXPRESSION "FAIL")
    # The slot rule holds on one rank and on every multinode flavour: a
    # non-master rank's verdict travels as printed output, the master's as the
    # exit status.
    register_single_node_test(fam_slot_rule TIMEOUT 30)
    register_multinode_test(fam_slot_rule TIMEOUT 60)
    set_tests_properties(fam_slot_rule fam_slot_rule_2n fam_slot_rule_3n
                         fam_slot_rule_4n fam_slot_rule_2n_io PROPERTIES
                         FAIL_REGULAR_EXPRESSION "FAIL")
    if(ARTS_FAM_BACKEND STREQUAL "SHM")
        # Whitebox test of the inherited mapping's fixed address: fam_slices.c
        # dereferences ARTS_FAM_BASE, which only the SHM backend defines --
        # under the device backend the address comes from the device library
        # and a configured value would mean nothing.  Excluded at
        # REGISTRATION with that structural reason, never skipped at run
        # time.
        register_multinode_test(fam_slices TIMEOUT 60 VARIANTS 2n 4n)
        set_tests_properties(fam_slices_2n fam_slices_4n PROPERTIES
                             FAIL_REGULAR_EXPRESSION "FAIL")
    endif()
endif()

if(ARTS_FAM_TREE_RESIDENCY AND ARTS_FAM_BACKEND STREQUAL "SHM")
    # The oracle is what this test IS, so it is registered only where the
    # oracle exists: under the device backend fam_strict is a config-load
    # error, because that memory already has the second coherency domain this
    # emulates.  Structural, at registration, never a run-time skip.
    register_multinode_test(fam_strict_flush TIMEOUT 120 VARIANTS 2n_strict)
    set_tests_properties(fam_strict_flush_2n_strict PROPERTIES
                         FAIL_REGULAR_EXPRESSION "FAIL")
endif()

# --- Mode-mismatch detection binary (no CTest entry — driven by run_mode_mismatch.sh) ---
add_executable(coherence_mode_mismatch ocr/coherence_mode_mismatch.c)
target_include_directories(coherence_mode_mismatch PRIVATE
    ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
    ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
set_property(TARGET coherence_mode_mismatch PROPERTY POSITION_INDEPENDENT_CODE OFF)
target_compile_options(coherence_mode_mismatch PRIVATE -fno-pie -fno-PIE)
target_link_options(coherence_mode_mismatch PRIVATE -no-pie -fno-pie -fno-PIE)
target_link_libraries(coherence_mode_mismatch PRIVATE
    Threads::Threads ${RT_LIBRARY} ${M_LIBRARY} ${ANL_LIBRARY})
if(TARGET arts_shared)
    target_link_libraries(coherence_mode_mismatch PRIVATE arts_shared)
elseif(TARGET arts_static)
    target_link_libraries(coherence_mode_mismatch PRIVATE arts_static)
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

# Wire-shape guard for the create packet's fabric-attached-memory slot and the
# two appended messages.  Registered UNGUARDED (no FAM-tree condition): the
# test's own #ifdef ARTS_FAM branches on the tree it compiles in, and it must
# pass in a FAM tree and in a tree with no FAM backend alike.
add_pure_unit_src(fam_wire_layout PASS_REGEX "PASS fam_wire_layout" TIMEOUT 30)

# runtime_types.h has separate C11-atomic and C++/nvcc declarations for the
# event descriptor.  The generated census owns the C probe; compile the exact
# same source through a C++ wrapper and compare their complete fingerprints.
add_executable(runtime_edt_event_layout_cpp
    unit/runtime_edt_event_layout_cpp.cpp)
target_include_directories(runtime_edt_event_layout_cpp PRIVATE
    ${ARTS_PUBLIC_INCLUDE_DIR}
    ${ARTS_INTERNAL_INCLUDE_DIR}
    ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
arts_apply_protocol(runtime_edt_event_layout_cpp
    ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})
set_property(TARGET runtime_edt_event_layout_cpp
    PROPERTY POSITION_INDEPENDENT_CODE OFF)
target_compile_options(runtime_edt_event_layout_cpp PRIVATE -fno-pie -fno-PIE)
target_link_options(runtime_edt_event_layout_cpp PRIVATE
    -no-pie -fno-pie -fno-PIE)
target_link_libraries(runtime_edt_event_layout_cpp PRIVATE Threads::Threads)

register_pure_unit_test(runtime_edt_event_layout_cpp TIMEOUT 60)
set_tests_properties(runtime_edt_event_layout_cpp PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS runtime_edt_event_layout \\[cpp\\]")

add_test(
    NAME runtime_edt_event_layout_compare
    COMMAND ${CMAKE_COMMAND}
        "-DC_EXECUTABLE=$<TARGET_FILE:runtime_edt_event_layout>"
        "-DCPP_EXECUTABLE=$<TARGET_FILE:runtime_edt_event_layout_cpp>"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/compare_runtime_edt_event_layout.cmake)
set_tests_properties(runtime_edt_event_layout_compare PROPERTIES
    LABELS "single_node"
    TIMEOUT 60
    RESOURCE_LOCK "pure_unit_test"
    PASS_REGULAR_EXPRESSION
        "PASS runtime_edt_event_layout_compare: C/C\\+\\+ fingerprints match")

# excl/types.h maintains two hand-written views of arts_db_cache_s; the FAM
# slot field is the one this arm adds to both.  Registered only where this
# tree's own library is FAM-enabled -- the fingerprint is meaningless with no
# fam_addr member to compare.
if(ARTS_FAM_TREE_RESIDENCY)
    add_pure_unit_src(fam_cache_fields PASS_REGEX "PASS fam_cache_fields \\[c\\]"
                      TIMEOUT 30)

    add_executable(fam_cache_fields_cpp
        unit/fam_cache_fields_cpp.cpp)
    target_include_directories(fam_cache_fields_cpp PRIVATE
        ${ARTS_PUBLIC_INCLUDE_DIR}
        ${ARTS_INTERNAL_INCLUDE_DIR}
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
    arts_apply_protocol(fam_cache_fields_cpp
        ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})
    set_property(TARGET fam_cache_fields_cpp
        PROPERTY POSITION_INDEPENDENT_CODE OFF)
    target_compile_options(fam_cache_fields_cpp PRIVATE -fno-pie -fno-PIE)
    target_link_options(fam_cache_fields_cpp PRIVATE
        -no-pie -fno-pie -fno-PIE)
    target_link_libraries(fam_cache_fields_cpp PRIVATE Threads::Threads)

    register_pure_unit_test(fam_cache_fields_cpp TIMEOUT 60)
    set_tests_properties(fam_cache_fields_cpp PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS fam_cache_fields \\[cpp\\]")

    add_test(
        NAME fam_cache_fields_compare
        COMMAND ${CMAKE_COMMAND}
            "-DC_EXECUTABLE=$<TARGET_FILE:fam_cache_fields>"
            "-DCPP_EXECUTABLE=$<TARGET_FILE:fam_cache_fields_cpp>"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/compare_fam_cache_fields.cmake)
    set_tests_properties(fam_cache_fields_compare PROPERTIES
        LABELS "single_node"
        TIMEOUT 60
        RESOURCE_LOCK "pure_unit_test"
        PASS_REGULAR_EXPRESSION
            "PASS fam_cache_fields_compare: C/C\\+\\+ fingerprints match")
endif()

# ============================================================================
# Cross-rank failure channel.
#
# ctest only ever sees the MASTER rank's exit status: the local launcher reaps
# its children with waitpid but discards their status, so a rank that aborts or
# exits non-zero is indistinguishable from one that finished cleanly.  For a
# multinode test the only verdict that leaves a non-master rank is what it
# PRINTED, which the launcher forwards to the master's stdout — hence this gate.
#
# It doubles as a second net single-node: a check that runs on a worker thread
# and only prints would otherwise pass on main()'s unconditional return 0.  The
# tests themselves carry their verdict in the exit status (see
# tests/test_failure_status.h); this catches what the status cannot reach.
#
# Conservative form (as in the T294 sweep above): the token matched is one these
# tests print ONLY on a failure branch — a clean run never emits it, and the
# runtime's own [ERROR] log aborts, so it cannot appear either.  Each registered
# variant needs its own entry: a multinode test has no bare-name entry.
# ============================================================================
set_tests_properties(await_publish_ack_shutdown PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(await_publish_ack_shutdown_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(await_publish_ack_shutdown_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(await_publish_ack_shutdown_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(await_publish_ack_shutdown_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(cat_c_ref_balance PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(cat_c_ref_balance_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(cat_c_ref_balance_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(cat_c_ref_balance_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(cat_c_ref_balance_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_install_version_monotone PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_install_version_monotone_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_install_version_monotone_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_install_version_monotone_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_install_version_monotone_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
set_tests_properties(coherence_owner_ownership_reorder PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
endif()
set_tests_properties(coherence_rw_chain PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
foreach(_t grant_purge_no_acquire_creator_2n grant_purge_no_acquire_creator_3n
          grant_purge_no_acquire_creator_4n
          grant_purge_no_acquire_creator_2n_io)
    set_tests_properties(${_t} PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
foreach(_t grant_purge_churn grant_purge_churn_2n grant_purge_churn_3n
          grant_purge_churn_4n grant_purge_churn_2n_io
          grant_purge_handback_regrant grant_purge_handback_regrant_2n
          grant_purge_handback_regrant_3n grant_purge_handback_regrant_4n
          grant_purge_handback_regrant_2n_io)
    set_tests_properties(${_t} PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
endif()

# grant_purge_return asserts that the write right is GIVEN BACK at every idle
# edge — true only where the release policy hands it back unasked, and
# deliberately false elsewhere (a retained grant serves the later turns with no
# hand-back at all).  The binary builds in every tree; only the entry is gated.
if((ARTS_COHERENCE_ARM STREQUAL "VAL" OR ARTS_COHERENCE_ARM STREQUAL "INV")
   AND ARTS_RELEASE_POLICY STREQUAL "PURGE")
    register_multinode_test(grant_purge_return TIMEOUT 60 VARIANTS 2n 3n 4n)
    foreach(_t grant_purge_return_2n grant_purge_return_3n
              grant_purge_return_4n)
        set_tests_properties(${_t} PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
    endforeach()
endif()
set_tests_properties(coherence_rw_chain_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_rw_chain_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_rw_chain_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_rw_chain_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_rwro_phase PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_rwro_phase_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_rwro_phase_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_rwro_phase_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_rwro_phase_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_stress_dist PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_zero_size_xfer_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_zero_size_xfer_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_zero_size_xfer_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_zero_size_xfer_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(create_coalesce_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(create_coalesce_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(create_coalesce_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(create_coalesce_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(csr_empty_partition PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(csr_load_args PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(csr_load_csr_format PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(csr_load_edgelist PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(ctx_crossrank_proxy_finish_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(ctx_crossrank_proxy_finish_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(ctx_crossrank_proxy_finish_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(ctx_crossrank_proxy_finish_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(db_acquire_all_bias_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(db_acquire_all_bias_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(db_acquire_all_bias_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(db_acquire_all_bias_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(db_release_alias_slot PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(db_rw_secure_double_fire PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(db_rw_secure_double_fire_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(db_rw_secure_double_fire_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(db_rw_secure_double_fire_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(db_rw_secure_double_fire_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(destroy_before_create_defer_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(destroy_before_create_defer_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(destroy_before_create_defer_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(destroy_before_create_defer_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(dist_args_oob PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(edt_destroy_crossrank_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(edt_destroy_crossrank_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(edt_destroy_crossrank_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(edt_destroy_crossrank_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(edt_output_event PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(edt_output_event_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(edt_output_event_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(edt_output_event_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(edt_output_event_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(edt_satisfy_out_of_range PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(event_destroy_before_create PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(event_destroy_before_create_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(event_destroy_before_create_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(event_destroy_before_create_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(event_destroy_before_create_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(excl_multiconsumer_queue PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(excl_multiconsumer_queue_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(excl_multiconsumer_queue_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(excl_multiconsumer_queue_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(excl_multiconsumer_queue_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(excl_purge_grant_d6_d7 PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(excl_purge_grant_d6_d7_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(excl_purge_grant_d6_d7_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(excl_purge_grant_d6_d7_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(excl_purge_grant_d6_d7_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(excl_samerank_rw_grant_race PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(excl_samerank_rw_grant_race_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(excl_samerank_rw_grant_race_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(excl_samerank_rw_grant_race_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(excl_samerank_rw_grant_race_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(mark_edt_ready_idempotent PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(publish_ack_post_on_miss PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(publish_ack_post_on_miss_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(publish_ack_post_on_miss_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(publish_ack_post_on_miss_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(publish_ack_post_on_miss_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(random_thread_safe PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(self_send_vs_dispatcher_parity PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(self_send_vs_dispatcher_parity_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(self_send_vs_dispatcher_parity_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(self_send_vs_dispatcher_parity_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(self_send_vs_dispatcher_parity_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(snapshot_response_3case PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(snapshot_response_3case_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(snapshot_response_3case_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(snapshot_response_3case_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(snapshot_response_3case_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_baton_invalidate_recheck PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_baton_invalidate_recheck_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_baton_invalidate_recheck_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_baton_invalidate_recheck_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_baton_invalidate_recheck_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_no_acquire PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_no_acquire_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_no_acquire_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_no_acquire_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_no_acquire_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_publish_before_decrement PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_publish_before_decrement_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_publish_before_decrement_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_publish_before_decrement_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_publish_before_decrement_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_rw_drain_count PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_rw_drain_count_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_rw_drain_count_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_rw_drain_count_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_rw_drain_count_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_snapshot_request PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_snapshot_request_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_snapshot_request_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_snapshot_request_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_snapshot_request_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_wb_confirm_ack_round PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_wb_confirm_ack_round_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_wb_confirm_ack_round_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_wb_confirm_ack_round_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_wb_confirm_ack_round_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_writer_count_nonneg PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_writer_count_nonneg_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_writer_count_nonneg_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_writer_count_nonneg_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_writer_count_nonneg_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_wt_publish_ack PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_wt_publish_ack_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_wt_publish_ack_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_wt_publish_ack_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_wt_publish_ack_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_wt_vs_wb_divergence PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_wt_vs_wb_divergence_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_wt_vs_wb_divergence_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_wt_vs_wb_divergence_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(val_wt_vs_wb_divergence_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

