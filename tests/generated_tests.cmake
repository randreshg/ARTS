# ============================================================================
# Unit, whitebox and OCR-program test registrations, maintained by hand;
# tests/CMakeLists.txt includes this file.  add_arts_test takes a test's source
# from tests/ocr or tests/unit, so programs of both kinds are registered here.
#
# Protocol selection (ARTS_PROTOCOL_* / ARTS_WRITE_POLICY_* / ARTS_RELEASE_*)
# is applied per target from the build directory's coherence configuration
# (arts_apply_protocol, in add_arts_test and add_pure_unit_src) -- it is
# deliberately NOT hardcoded here, so each test compiles under whatever
# protocol the enclosing build dir selected.  A test whose outcome no protocol
# cell can change is built and registered in the reference tree only
# (ARTS_TEST_REFERENCE_TREE, tests/CMakeLists.txt).
# ============================================================================

# Helper: build a standalone pure_unit test that compiles specific libs/src .c
# TUs directly (it does NOT link libarts) with the canonical include dirs +
# Threads. libatomic + -mcx16 come from the top-level setup (not re-added here).
function(add_pure_unit_src name)
    cmake_parse_arguments(PU "" "TIMEOUT;PASS_REGEX" "SOURCES;DEFINES;LIBS" ${ARGN})
    add_executable(${name} unit/${name}.c ${PU_SOURCES})
    target_include_directories(${name} PRIVATE
        ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR}
        # Some pure_unit TUs '#include' a runtime .c by a path relative to a
        # source root (e.g. "transport/launcher.c", "counter.c") to pull in
        # file-static symbols without de-static-ing the runtime.  Expose the
        # source roots so those includes resolve.
        ${CMAKE_SOURCE_DIR}/libs/src
        ${CMAKE_SOURCE_DIR}/libs/src/core
        ${CMAKE_SOURCE_DIR}/libs/src/core/counter)
    if(PU_DEFINES)
        target_compile_definitions(${name} PRIVATE ${PU_DEFINES})
    endif()
    # Some pure_unit TUs pull arts/ooo.h (directly or via route_table.c) which
    # requires exactly one compile-time coherence-protocol selection.  Apply it
    # unconditionally — tests that do not include protocol-sensitive headers
    # simply leave the macros unused, so this is harmless for all others.
    arts_apply_protocol(${name} ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})
    set_property(TARGET ${name} PROPERTY POSITION_INDEPENDENT_CODE OFF)
    target_compile_options(${name} PRIVATE ${ARTS_NON_PIE_COMPILE_OPTIONS})
    target_link_options(${name} PRIVATE ${ARTS_NON_PIE_LINK_OPTIONS})
    target_link_libraries(${name} PRIVATE Threads::Threads ${PU_LIBS})
    if(NOT PU_TIMEOUT)
        set(PU_TIMEOUT 60)
    endif()
    register_pure_unit_test(${name} TIMEOUT ${PU_TIMEOUT})
    if(PU_PASS_REGEX)
        set_tests_properties(${name} PROPERTIES
            PASS_REGULAR_EXPRESSION "${PU_PASS_REGEX}")
    endif()
endfunction()

# --- Pure units of the cell's own coherence layout ---
add_pure_unit_src(db_cache_layout PASS_REGEX "PASS db_cache_layout" TIMEOUT 60)
if(NOT "buffer_payload_roundtrip" IN_LIST ARTS_CXL_DIRECT_EXCLUDED)
add_pure_unit_src(buffer_payload_roundtrip SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/buffer.c ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c PASS_REGEX "PASS buffer_payload_roundtrip" TIMEOUT 60)
add_pure_unit_src(buffer_zero_size SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/buffer.c ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c PASS_REGEX "PASS buffer_zero_size" TIMEOUT 60)
add_pure_unit_src(buffer_version_guard SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/buffer.c ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c PASS_REGEX "PASS buffer_version_guard" TIMEOUT 60)
add_pure_unit_src(buffer_stub_db_size_learn SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/buffer.c ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c PASS_REGEX "PASS buffer_stub_db_size_learn" TIMEOUT 60)
add_pure_unit_src(buffer_destroy_vs_acquire SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/buffer.c ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c PASS_REGEX "PASS buffer_destroy_vs_acquire" TIMEOUT 60)
endif()
# rank_bitset: one definition, which no cell compiles differently, compiled
# standalone.
if(ARTS_TEST_REFERENCE_TREE)
    add_pure_unit_src(rank_bitset
        SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/rank_bitset.c
        PASS_REGEX "PASS rank_bitset" TIMEOUT 60)
endif()
# The wire's frozen message ordinals, packet headers and sizes, including the
# CXL slot of the create packet where this tree's library keeps one.
add_pure_unit_src(protocol_abi_asserts TIMEOUT 60)

# --- Pure units with no protocol cell to depend on ---
if(ARTS_TEST_REFERENCE_TREE)
add_pure_unit_src(lf_lifo_drain PASS_REGEX "PASS lf_lifo_drain" TIMEOUT 60)
add_pure_unit_src(lf_pool_dwcas PASS_REGEX "PASS lf_pool_dwcas" TIMEOUT 60)
add_pure_unit_src(lf_pool_batch PASS_REGEX "PASS lf_pool_batch" TIMEOUT 60)
add_pure_unit_src(mpsc_drain_remaining PASS_REGEX "PASS mpsc_drain_remaining" TIMEOUT 60)
add_pure_unit_src(mpsc_rethread_stub PASS_REGEX "PASS mpsc_rethread_stub" TIMEOUT 60)
add_pure_unit_src(mpsc_transient_empty PASS_REGEX "PASS mpsc_transient_empty" TIMEOUT 60)
add_pure_unit_src(shared_compare_exchange SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c PASS_REGEX "PASS shared_compare_exchange" TIMEOUT 60)
add_pure_unit_src(shared_lifecycle_race SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c PASS_REGEX "PASS shared_lifecycle_race" TIMEOUT 60)
add_pure_unit_src(route_table_install_if_absent PASS_REGEX "PASS route_table_install_if_absent" TIMEOUT 60)
add_pure_unit_src(ooo_park_install_retire PASS_REGEX "PASS ooo_park_install_retire" TIMEOUT 120)
add_pure_unit_src(route_table_segment_grow PASS_REGEX "PASS route_table_segment_grow" TIMEOUT 60)
add_pure_unit_src(route_table_growth_residency PASS_REGEX "PASS route_table_growth_residency" TIMEOUT 60)
add_pure_unit_src(route_item_mirror_bridge PASS_REGEX "PASS route_item_mirror_bridge" TIMEOUT 60)
add_pure_unit_src(guid_encoding_roundtrip PASS_REGEX "PASS guid_encoding_roundtrip" TIMEOUT 60)
add_pure_unit_src(guid_from_index_overflow PASS_REGEX "PASS guid_from_index_overflow" TIMEOUT 60)
add_pure_unit_src(guid_reserve_range PASS_REGEX "PASS guid_reserve_range" TIMEOUT 60)
add_pure_unit_src(guid_db_seq_alloc_stress PASS_REGEX "PASS guid_db_seq_alloc_stress" TIMEOUT 120)
add_pure_unit_src(route_table_db_shard PASS_REGEX "PASS route_table_db_shard" TIMEOUT 60)
add_pure_unit_src(rank_u64_map_roundtrip SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/rank_u64_map.c PASS_REGEX "PASS rank_u64_map_roundtrip" TIMEOUT 60)
add_pure_unit_src(rank_u64_map_advance SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/rank_u64_map.c PASS_REGEX "PASS rank_u64_map_advance" TIMEOUT 60)
# An interface entry with no address (which getifaddrs may return) is skipped
# by the rank-matching scan rather than dereferenced.
add_pure_unit_src(socket_set_ip_null_ifa LIBS ${CMAKE_DL_LIBS} PASS_REGEX "PASS socket_set_ip_null_ifa" TIMEOUT 60)
add_pure_unit_src(socket_helpers PASS_REGEX "PASS socket_helpers" TIMEOUT 60)
add_pure_unit_src(socket_net_interface_remap PASS_REGEX "PASS socket_net_interface_remap" TIMEOUT 60)
add_pure_unit_src(launcher_shell_quote PASS_REGEX "PASS launcher_shell_quote" TIMEOUT 60)
add_pure_unit_src(stdio_forward_fidelity PASS_REGEX "PASS stdio_forward_fidelity" TIMEOUT 60)
add_pure_unit_src(stdio_forward_partial_eintr PASS_REGEX "PASS stdio_forward_partial_eintr" TIMEOUT 60)
add_pure_unit_src(stdio_forward_make_pipe_fail PASS_REGEX "PASS stdio_forward_make_pipe_fail" TIMEOUT 60)
add_pure_unit_src(stdio_forward_fileno_negative PASS_REGEX "PASS stdio_forward_fileno_negative" TIMEOUT 60)
add_pure_unit_src(stdio_forward_shutdown_idempotent PASS_REGEX "PASS stdio_forward_shutdown_idempotent" TIMEOUT 60)
add_pure_unit_src(stdio_forward_concurrent_push PASS_REGEX "PASS stdio_forward_concurrent_push" TIMEOUT 60)
add_pure_unit_src(stdio_forward_fflush_no_deadlock PASS_REGEX "PASS stdio_forward_fflush_no_deadlock" TIMEOUT 60)
add_pure_unit_src(stdio_forward_multi_source PASS_REGEX "PASS stdio_forward_multi_source" TIMEOUT 60)
# Configuration parsing stays inside what it allocated: a routing table
# holds no more rows than it was sized for, a single-host LSF list is refused,
# and an environment override spliced into the variable list leaves it well
# formed.
add_pure_unit_src(config_routing_table_overflow PASS_REGEX "PASS config_routing_table_overflow" TIMEOUT 60)
add_pure_unit_src(config_lsf_single_host_oob PASS_REGEX "PASS config_lsf_single_host_oob" TIMEOUT 60)
add_pure_unit_src(config_find_variable PASS_REGEX "PASS config_find_variable" TIMEOUT 60)
add_pure_unit_src(config_parse_port_spec PASS_REGEX "PASS config_parse_port_spec" TIMEOUT 60)
add_pure_unit_src(config_count_nodes PASS_REGEX "PASS config_count_nodes" TIMEOUT 60)
add_pure_unit_src(config_get_variables_oob PASS_REGEX "PASS config_get_variables_oob" TIMEOUT 60)
add_pure_unit_src(config_routing_table_ssh_bracket PASS_REGEX "PASS config_routing_table_ssh_bracket" TIMEOUT 60)
add_pure_unit_src(config_routing_table_slurm_hostlist PASS_REGEX "PASS config_routing_table_slurm_hostlist" TIMEOUT 60)
add_pure_unit_src(placement_invariants SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/system/placement.c PASS_REGEX "PASS placement_invariants" TIMEOUT 60)
add_pure_unit_src(config_route_table_size_shift PASS_REGEX "PASS config_route_table_size_shift" TIMEOUT 60)
add_pure_unit_src(config_deque_size_range PASS_REGEX "PASS config_deque_size_range" TIMEOUT 60)
add_pure_unit_src(config_thread_count_underflow PASS_REGEX "PASS config_thread_count_underflow" TIMEOUT 60)
add_pure_unit_src(config_launcher_env_precedence PASS_REGEX "PASS config_launcher_env_precedence" TIMEOUT 60)
add_pure_unit_src(config_destroy_leak PASS_REGEX "PASS config_destroy_leak" TIMEOUT 60)
add_pure_unit_src(runtime_edt_event_layout PASS_REGEX "PASS runtime_edt_event_layout" TIMEOUT 60)
# topology.c #includes <hwloc.h> and calls hwloc_* — the vendored hwloc
# target carries the headers and the archive together; a host hwloc must
# never satisfy either.
add_pure_unit_src(topology_thread_mask SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/system/placement.c LIBS arts::hwloc PASS_REGEX "PASS topology_thread_mask" TIMEOUT 60)
add_pure_unit_src(threads_worker_underflow PASS_REGEX "PASS threads_worker_underflow" TIMEOUT 60)
add_pure_unit_src(signals_formatters PASS_REGEX "PASS signals_formatters" TIMEOUT 60)
# The handlers' backtrace() must not load the unwinder in signal context: the
# install loads it first.  A sanitizer runtime loads the unwinder itself before
# the program starts, so the property is observable only without one.
if(NOT ARTS_DEBUG_SANITIZER)
add_pure_unit_src(signals_unwinder_preload PASS_REGEX "PASS signals_unwinder_preload" TIMEOUT 30)
endif()
# Well-formed output and escaping, and a document that stays well formed when
# its nesting passes the writer's depth bound.
add_pure_unit_src(json_writer SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/counter/json.c PASS_REGEX "PASS json_writer" TIMEOUT 60)
add_pure_unit_src(edge_vector PASS_REGEX "PASS edge_vector" TIMEOUT 60)
# The block distribution's arithmetic, its edge cases included: more blocks
# than vertices, no blocks, no vertices.
add_pure_unit_src(block_dist_query PASS_REGEX "PASS block_dist_query" TIMEOUT 60)
# arts_csr_free(NULL) is a no-op.
add_pure_unit_src(csr_free_null PASS_REGEX "PASS csr_free_null" TIMEOUT 60)
# vector_basic: #includes vector.c directly (libc shims); pins the contracts
# the per-worker EDT-context lists lean on (lazy alloc, doubling, swap-remove,
# clear-keeps-block, free-keeps-sentinel).
add_pure_unit_src(vector_basic PASS_REGEX "PASS vector_basic" TIMEOUT 30)
endif()

# ============================================================================
# Runtime and configuration-specific tests.
#
# A single-node test registers through register_single_node_test, a multinode
# one through register_multinode_test (VARIANTS names the shapes that each
# exercise something the others do not), and a test that holds on both shapes
# through both.  Protocol selection is applied per target, as above.  A test
# whose body applies to some protocol cells only is built and registered in
# those trees alone, never compiled to a stub that reports SKIP.
# ============================================================================

# --- DB lifecycle & acquire/release accounting ---
# The exclusion arm's lock state under a creator's release with nothing to
# publish.
if(ARTS_COHERENCE_ARM STREQUAL "EXCL")
add_arts_test(db_excl_creator_release_keeps_joiner_write)
register_single_node_test(db_excl_creator_release_keeps_joiner_write TIMEOUT 30)
set_tests_properties(db_excl_creator_release_keeps_joiner_write PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: db_excl_creator_release_keeps_joiner_write")
endif()

# An EDT holding several blocks for writing, all of them homed off its rank,
# with read-only ones beside them: it must not run before every block arrived.
# Ranks 0 and 1 hold every turn; the two progress threads deliver the grants
# in any order.
add_arts_test(db_acquire_all_bias)
register_multinode_test(db_acquire_all_bias TIMEOUT 60 VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_v 2n_9w_1p 2n_8w_2p)
    set_tests_properties(db_acquire_all_bias_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: db_acquire_all_bias")
endforeach()

add_arts_test(db_destroy_implicit_release)
register_single_node_test(db_destroy_implicit_release TIMEOUT 30)
set_tests_properties(db_destroy_implicit_release PROPERTIES PASS_REGULAR_EXPRESSION "PASS: db_destroy_implicit_release")

# dbcreate_matrix: probe of the DB-create capability matrix --
# (creator-acquisition) x (home-determination) x (locality).  Exercises the
# ARTS core create path directly (arts.h, no OCR shim), so it is a coverage
# regression guard: each cell reports its result independently, and a failed
# cell fails the run.  Cross-runtime capability parity (shim vs the reference
# runtimes) is compared separately through the correctness harness, not here.
add_arts_test(dbcreate_matrix)
register_single_node_test(dbcreate_matrix TIMEOUT 60)
register_multinode_test(dbcreate_matrix TIMEOUT 60)

# --- EDT context: created-DB list and owned finish scopes ---
add_arts_test(ctx_created_db_release_order)
register_single_node_test(ctx_created_db_release_order TIMEOUT 30)
set_tests_properties(ctx_created_db_release_order PROPERTIES PASS_REGULAR_EXPRESSION "PASS ctx_created_db_release_order")

# These two record into one block from EDTs with no order among them, which
# DB-WRF does not admit.  The cross-rank one uses ranks 0 and 1 only, and its
# scope's proxy is fed by messages local to each rank.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
add_arts_test(ctx_owned_finish_cleanup)
add_arts_test(ctx_crossrank_proxy_finish)
register_single_node_test(ctx_owned_finish_cleanup TIMEOUT 30)
register_multinode_test(ctx_crossrank_proxy_finish TIMEOUT 60 VARIANTS 2n_9w_1p)
set_tests_properties(ctx_crossrank_proxy_finish_2n_9w_1p PROPERTIES PASS_REGULAR_EXPRESSION "PASS ctx_crossrank_proxy_finish")
endif()

# ctx_gpu_lib_edt: GPU-only real body (ARTS_TEST_GPU); SKIP stub links CPU libarts otherwise.
if(BUILD_CUDA_LIBRARY)
    add_arts_test(ctx_gpu_lib_edt)
    target_compile_definitions(ctx_gpu_lib_edt PRIVATE ARTS_TEST_GPU=1)
    register_gpu_test(ctx_gpu_lib_edt TIMEOUT 30)
    set_tests_properties(ctx_gpu_lib_edt PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS ctx_gpu_lib_edt|SKIP ctx_gpu_lib_edt")
endif()

# --- event channel / satisfy / destroy / create-collision ---
# These three record into one block from EDTs with no order among them, which
# DB-WRF does not admit.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
add_arts_test(event_channel_transient_null)
add_arts_test(event_satisfy_adddep_window)
add_arts_test(event_finish_latch_rearm)
register_single_node_test(event_channel_transient_null TIMEOUT 60)
register_single_node_test(event_satisfy_adddep_window TIMEOUT 60)
set_tests_properties(event_satisfy_adddep_window PROPERTIES PASS_REGULAR_EXPRESSION "event_satisfy_adddep_window:.*PASS")
register_single_node_test(event_finish_latch_rearm TIMEOUT 60)
set_tests_properties(event_finish_latch_rearm PROPERTIES PASS_REGULAR_EXPRESSION "event_finish_latch_rearm:.*PASS")
endif()

if(ARTS_TEST_REFERENCE_TREE)
add_arts_test(event_error_paths)
register_single_node_test(event_error_paths TIMEOUT 30 STATUS 1)
set_tests_properties(event_error_paths PROPERTIES PASS_REGULAR_EXPRESSION "CHANNEL: only DECR")

# event_counted_reclaim: COUNTED delivery + reclamation, and that an
# undeclared ONCE still lingers.  Whitebox (reads the route table),
# single-node: the property is local.
add_arts_test(event_counted_reclaim)
register_single_node_test(event_counted_reclaim TIMEOUT 60)

# event_destroy_before_create: a destroy that reaches an event's home before
# its create defers there and is applied at the install.  Its whole body runs
# on rank 0, so peers add nothing.
add_arts_test(event_destroy_before_create)
register_single_node_test(event_destroy_before_create TIMEOUT 30)
set_tests_properties(event_destroy_before_create PROPERTIES PASS_REGULAR_EXPRESSION "event_destroy_before_create:.*PASS")
endif()

# event_gpu_force_defer: GPU-only real body (ARTS_USE_GPU); SKIP stub otherwise.
if(BUILD_CUDA_LIBRARY)
    add_arts_test(event_gpu_force_defer)
    target_compile_definitions(event_gpu_force_defer PRIVATE ARTS_USE_GPU)
    register_gpu_test(event_gpu_force_defer TIMEOUT 30)
    set_tests_properties(event_gpu_force_defer PROPERTIES
        PASS_REGULAR_EXPRESSION "event_gpu_force_defer:.*PASS")
endif()

# --- grant retention ---

# grant_sticky: asserts the grant outlives its writers (single-rank only —
# a second rank could revoke it, which is what the test must exclude).  Not
# FLUSH, which holds no write right, and not EXCL x PURGE, whose permission
# goes back at the zero edge.  Under the write-through arms' PURGE a grant goes
# back to its home at the zero edge, and on one rank it is already there, so
# there the test checks only the holds counted under it.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH" AND
   NOT (ARTS_COHERENCE_ARM STREQUAL "EXCL" AND
        ARTS_RELEASE_POLICY STREQUAL "PURGE"))
add_arts_test(grant_sticky)
register_single_node_test(grant_sticky TIMEOUT 60)
set_tests_properties(grant_sticky PROPERTIES PASS_REGULAR_EXPRESSION "PASS grant_sticky")
endif()

# grant_purge_possession: where the holder hands the write right back at its
# idle edge, the home's own possession is its whole directory — nothing
# possessed, no round open and nothing parked at the home while another rank
# holds the right, and the right back at the home, idle, after every turn.
# Only VAL and INV under PURGE hand a migrating grant back.  2n_8w_2p is the
# one shape with two progress threads per rank, so the home's handlers for one
# transfer run on either.  The shutdown waits on a finish scope that covers
# every task, so the teardown walk must stay silent.
if((ARTS_COHERENCE_ARM STREQUAL "VAL" OR ARTS_COHERENCE_ARM STREQUAL "INV")
   AND ARTS_RELEASE_POLICY STREQUAL "PURGE")
add_arts_test(grant_purge_possession)
register_multinode_test(grant_purge_possession TIMEOUT 120
                        VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_v 2n_9w_1p 2n_8w_2p)
    set_tests_properties(grant_purge_possession_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS grant_purge_possession"
        FAIL_REGULAR_EXPRESSION "FAIL;QUIESCENCE-DEBUG:")
endforeach()
endif()

# A GRANT_RESPONSE shorter than its header, or declaring a version map longer
# than the bytes that follow the header, is fatal on the receiving rank.  The
# migrating grant exists on VAL and INV, with one handler per write policy.
if(ARTS_COHERENCE_ARM STREQUAL "VAL" OR ARTS_COHERENCE_ARM STREQUAL "INV")
add_arts_test(grant_response_bounds)
arts_test_cfg(_cfg 1n_20w)
arts_add_test(grant_response_bounds_short STATUS 1
              COMMAND grant_response_bounds short)
arts_add_test(grant_response_bounds_map STATUS 1
              COMMAND grant_response_bounds map)
set_tests_properties(grant_response_bounds_short PROPERTIES
    PASS_REGULAR_EXPRESSION "GRANT_RESPONSE of [0-9]+ bytes is shorter than")
set_tests_properties(grant_response_bounds_map PROPERTIES
    PASS_REGULAR_EXPRESSION
    "declares a [0-9]+-byte version map, but [0-9]+ bytes follow it")
set_tests_properties(grant_response_bounds_short grant_response_bounds_map
    PROPERTIES LABELS "single_node" TIMEOUT 30 RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg}")
endif()

# --- EXCL protocol-specific ---
# The exclusion arm's own cache and home paths, built and registered in its
# trees only.
if(ARTS_COHERENCE_ARM STREQUAL "EXCL")
# A same-rank cohort of write acquisitions of one block, every one of which
# must be granted: a lost wakeup leaves the cohort short and the run hangs.
# Two ranks already put the home off the cohort's rank.
add_arts_test(excl_samerank_rw_grant_race)
register_single_node_test(excl_samerank_rw_grant_race TIMEOUT 60)
register_multinode_test(excl_samerank_rw_grant_race TIMEOUT 60
                        VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_v "" _2n_9w_1p _2n_8w_2p)
    set_tests_properties(excl_samerank_rw_grant_race${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "excl_samerank_rw_grant_race: .* PASS")
endforeach()

# With peers the cohorts' requests cross the wire to the blocks' home; two
# ranks already do.
add_arts_test(excl_request_coalesce)
register_single_node_test(excl_request_coalesce TIMEOUT 60)
register_multinode_test(excl_request_coalesce TIMEOUT 120
                        VARIANTS 2n_9w_1p 2n_8w_2p)

# A request homed on the requesting rank advertises no landing, one homed on
# another rank does, and a payload a remote owner owes a home requester still
# arrives.  Two ranks show all three; one brings up no fabric, and where grants
# carry no payload (the CXL store) no request advertises a landing.  Every turn
# is ordered after the previous one, so one two-rank shape covers the scenario.
if(NOT ARTS_USE_CXL)
add_arts_test(excl_self_home_no_landing)
register_multinode_test(excl_self_home_no_landing TIMEOUT 120
                        VARIANTS 2n_8w_2p)
set_tests_properties(excl_self_home_no_landing_2n_8w_2p PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS excl_self_home_no_landing")
endif()

add_arts_test(excl_purge_grant_d6_d7)
register_single_node_test(excl_purge_grant_d6_d7 TIMEOUT 60)
register_multinode_test(excl_purge_grant_d6_d7 TIMEOUT 60)
# The end-of-run line is printed only once both halves have finished, so
# requiring it on every variant turns a stranded writer into a failure.
foreach(_v "" _2n_9w_1p _4n_4w_1p _2n_8w_2p)
    set_tests_properties(excl_purge_grant_d6_d7${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION
        "excl_purge_grant_d6_d7 D7: .* D6 chain verified — PASS")
endforeach()
endif()

# Write churn against one home with read turns flipping its phase, so the
# home's grant queue is popped from every handler that grants: every writer
# must run exactly once.  A valid write-churn check on every arm whose
# runtime orders writers; under DB-WRF that ordering is the program's, and
# this program omits it.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
add_arts_test(excl_multiconsumer_queue)
register_single_node_test(excl_multiconsumer_queue TIMEOUT 60)
register_multinode_test(excl_multiconsumer_queue TIMEOUT 60)
foreach(_v "" _2n_9w_1p _4n_4w_1p _2n_8w_2p)
    set_tests_properties(excl_multiconsumer_queue${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "excl_multiconsumer_queue: .* PASS")
endforeach()
endif()

# excl_req_before_create: protocol-agnostic, needs 3+ ranks; registered at
# 4n_4w_1p.
# These assert what the runtime's exclusive-RW serialization guarantees; under
# DB-WRF that ordering is the program's, and these programs deliberately omit
# it.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
add_arts_test(excl_req_before_create)
register_multinode_test(excl_req_before_create TIMEOUT 90 VARIANTS 4n_4w_1p)
set_tests_properties(excl_req_before_create_4n_4w_1p PROPERTIES PASS_REGULAR_EXPRESSION "PASS: [0-9]+ iterations completed")
endif()

# --- self-send vs dispatch, first-touch claim, Cat-C references, publish-ack
# escape ---
# A message a rank sends itself and the same message from a peer must have one
# effect: every message family on one rank and across ranks, with one oracle.
add_arts_test(self_send_vs_dispatcher_parity)
register_single_node_test(self_send_vs_dispatcher_parity TIMEOUT 120)
register_multinode_test(self_send_vs_dispatcher_parity TIMEOUT 120)
foreach(_v "" _2n_9w_1p _4n_4w_1p _2n_8w_2p)
    set_tests_properties(self_send_vs_dispatcher_parity${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: self_send_vs_dispatcher_parity")
endforeach()

# A dependence's first touch installs a stub that the later create claims.
# Ranks 0 and 1 are the whole case; the two progress threads reorder the
# create against the first touch.
add_arts_test(db_create_claims_first_touch_stub)
register_multinode_test(db_create_claims_first_touch_stub TIMEOUT 120
                        VARIANTS 2n_9w_1p 2n_8w_2p)

# Every coherence handler that pins a block's descriptor releases it on every
# path, so a create-touch-destroy churn on every rank leaves no descriptor
# pinned.  Its write turns on each block are unordered, which DB-WRF does not
# admit.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
add_arts_test(cat_c_ref_balance)
register_single_node_test(cat_c_ref_balance TIMEOUT 120)
register_multinode_test(cat_c_ref_balance TIMEOUT 120)
foreach(_v "" _2n_9w_1p _4n_4w_1p _2n_8w_2p)
    set_tests_properties(cat_c_ref_balance${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: cat_c_ref_balance")
endforeach()
endif()

# A release that waits for its publish or write-back to be acknowledged, or
# for the landing it asks the home for, escapes that wait when the shutdown
# overtakes it.  A write-through release, the invalidation arm's release round
# and FLUSH's write-back wait for an acknowledgement; a release whose bytes
# travel with it, from a rank that holds no landing in the home's copy (a
# creator away from the block's home, among others), asks for one first, and
# so does EXCL x PURGE's, except in the store-backed trees, which write the
# store instead and wait for nothing.  One rank answers itself without
# waiting.  Ranks 0 and 1 carry every turn; the
# two progress threads let the shutdown overtake an answer in flight.  Its
# writes of each block are ordered, so FLUSH runs it too.  The contended form
# (every writer of one block at once, so that a release joins its rank's
# publish already in flight) has unordered writers and runs where a release
# publishes: the write-through and invalidation arms.  A releaser that never
# escapes keeps its rank from exiting, which the launcher reports before it
# stops the rank; the report fails the test.
set(_await_publishes OFF)
if(ARTS_COHERENCE_ARM MATCHES "^(VAL|INV)$" AND
   (ARTS_WRITE_POLICY STREQUAL "WT" OR ARTS_COHERENCE_ARM STREQUAL "INV"))
    set(_await_publishes ON)
endif()
if(_await_publishes OR ARTS_COHERENCE_ARM STREQUAL "FLUSH" OR
   (ARTS_COHERENCE_ARM STREQUAL "EXCL" AND ARTS_RELEASE_POLICY STREQUAL "PURGE"
    AND NOT ARTS_USE_CXL))
add_arts_test(await_publish_ack_shutdown)
register_multinode_test(await_publish_ack_shutdown TIMEOUT 120
                        VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_v 2n_9w_1p 2n_8w_2p)
    set_tests_properties(await_publish_ack_shutdown_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: await_publish_ack_shutdown reached shutdown")
endforeach()
endif()
if(_await_publishes)
foreach(_v 2n_9w_1p 2n_8w_2p)
    arts_test_cfg(_cfg ${_v})
    arts_add_test(await_publish_ack_shutdown_contended_${_v}
                  COMMAND await_publish_ack_shutdown contended)
    set_tests_properties(await_publish_ack_shutdown_contended_${_v} PROPERTIES
        LABELS "multinode"
        TIMEOUT 120
        RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg}"
        PASS_REGULAR_EXPRESSION "PASS: await_publish_ack_shutdown reached shutdown")
endforeach()
endif()

# --- route-table / GUID determinism ---
if(ARTS_TEST_REFERENCE_TREE)
# Reservations minted on workers concurrently; the key generator and the
# route table agree on every rank.  Its reservers record into one block with
# no order among them.
add_arts_test(guid_keygen_table_coupling)
register_multinode_test(guid_keygen_table_coupling TIMEOUT 120 VARIANTS 2n_9w_1p)

# Every rank computes the same home for the same create, round-robin over the
# ranks, and the GUID encodes it; four ranks are where the rotation shows.
add_arts_test(round_robin_home_distribution)
register_multinode_test(round_robin_home_distribution TIMEOUT 120
                        VARIANTS 2n_9w_1p 4n_4w_1p)
endif()

# --- DB create-with-data / stub install ---
add_arts_test(db_create_with_data_bytes)
register_single_node_test(db_create_with_data_bytes TIMEOUT 60)
set_tests_properties(db_create_with_data_bytes PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: db_create_with_data_bytes")

# A ready EDT is claimed once whichever wake reaches it: the claim is one CAS
# on the EDT, and the wakes come from the arm's cache slots (whitebox, one
# rank; not under FLUSH, where every acquirer owns its own bytes and no cache
# slot wakes anything).
if(NOT "mark_edt_ready_idempotent" IN_LIST ARTS_CXL_DIRECT_EXCLUDED AND
   NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
add_arts_test(mark_edt_ready_idempotent)
register_single_node_test(mark_edt_ready_idempotent TIMEOUT 60)
set_tests_properties(mark_edt_ready_idempotent PROPERTIES PASS_REGULAR_EXPRESSION
    "PASS: mark_edt_ready_idempotent")
endif()

# Each round's block lives off rank 0 and every rank's readers race its
# first-touch stub install, on every arm; at two ranks rank 0 is that racing
# consumer, and four add only more of the same race.
add_arts_test(stub_install_winner)
register_multinode_test(stub_install_winner TIMEOUT 180 VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_v 2n_9w_1p 2n_8w_2p)
    set_tests_properties(stub_install_winner_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: stub_install_winner")
endforeach()

# --- scheduler ---
# The acquire bias keeps an EDT with many dependences from running before its
# last slot resolves, and an RW hand-off chain's resumes push onto the resuming
# thread's own deque; every EDT runs exactly once.  One rank.
# It records into one block from EDTs with no order among them, which DB-WRF
# does not admit.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
add_arts_test(scheduler_acquire_bias)
register_single_node_test(scheduler_acquire_bias TIMEOUT 120)
set_tests_properties(scheduler_acquire_bias PROPERTIES PASS_REGULAR_EXPRESSION "scheduler_acquire_bias: .* — PASS")
endif()

if(ARTS_TEST_REFERENCE_TREE)
add_arts_test(scheduler_loop_variants)
register_single_node_test(scheduler_loop_variants TIMEOUT 60)
set_tests_properties(scheduler_loop_variants PROPERTIES PASS_REGULAR_EXPRESSION "scheduler_loop_variants: .* — PASS")

# --- dispatcher ---
# dispatcher_default_fatal: success is the dispatcher's own abort on a message
# type the build does not know: exit status 1, with its report.
add_arts_test(dispatcher_default_fatal)
register_single_node_test(dispatcher_default_fatal STATUS 1 TIMEOUT 60)
set_tests_properties(dispatcher_default_fatal PROPERTIES
    PASS_REGULAR_EXPRESSION "received a message of unknown type [0-9]+"
    FAIL_REGULAR_EXPRESSION "FAIL")

# --- launcher ---
# The launcher spawns each rank once, every spawned rank knows its rank and the
# total, and every one has a live parent; four ranks make the spawn loop start
# more than one child.
add_arts_test(launcher_no_respawn)
register_multinode_test(launcher_no_respawn TIMEOUT 60 VARIANTS 2n_9w_1p 4n_4w_1p)

# --- config loading ---
add_arts_test(config_load_single_reclaim)
register_single_node_test(config_load_single_reclaim TIMEOUT 30)
set_tests_properties(config_load_single_reclaim PROPERTIES PASS_REGULAR_EXPRESSION "PASS config_load_single_reclaim")

# config_scheduler_range: pure_unit (links libarts, starts no runtime; forks
# to probe the refusal of a scheduler index past this build's loop table).  The
# refused child's report and the parent's verdict reach the log in either order.
add_arts_test(config_scheduler_range)
register_pure_unit_test(config_scheduler_range TIMEOUT 30)
set_tests_properties(config_scheduler_range PROPERTIES PASS_REGULAR_EXPRESSION
    "names no scheduler loop.*PASS config_scheduler_range|PASS config_scheduler_range.*names no scheduler loop")

# config_provider_regpool_parse: pure_unit (links libarts, starts no runtime).
add_arts_test(config_provider_regpool_parse)
register_pure_unit_test(config_provider_regpool_parse TIMEOUT 30)
set_tests_properties(config_provider_regpool_parse PROPERTIES PASS_REGULAR_EXPRESSION "PASS config_provider_regpool_parse")

# dep_alias_classify: pure_unit — the acquire engine's owner/alias rule is a
# pure function of a dependence vector, so it is driven on stack arrays out of
# the per-config static libarts; the runtime is never started and the answers
# are configuration-independent.
add_arts_test(dep_alias_classify)
register_pure_unit_test(dep_alias_classify TIMEOUT 30)
set_tests_properties(dep_alias_classify PROPERTIES PASS_REGULAR_EXPRESSION "PASS dep_alias_classify")

# --- runtime startup/shutdown/threads/signals ---
add_arts_test(runtime_barrier_counts)
register_single_node_test(runtime_barrier_counts TIMEOUT 60)

add_arts_test(threads_shutdown_cancel)
register_single_node_test(threads_shutdown_cancel TIMEOUT 60)
set_tests_properties(threads_shutdown_cancel PROPERTIES PASS_REGULAR_EXPRESSION "SHUTDOWN_CANCEL_DONE")

add_arts_test(threads_pin_affinity)
register_single_node_test(threads_pin_affinity TIMEOUT 60)

add_arts_test(signals_sigterm_graceful)
register_single_node_test(signals_sigterm_graceful TIMEOUT 60)
set_tests_properties(signals_sigterm_graceful PROPERTIES PASS_REGULAR_EXPRESSION "SIGTERM_GRACEFUL_OK")

# signals_double_sigterm: success is exit code 143 (= 128+SIGTERM), the escalation's own exit.
add_arts_test(signals_double_sigterm)
register_single_node_test(signals_double_sigterm STATUS 143 TIMEOUT 60)

# main_rank_overflow_abort: success is the overflow guard's own abort: exit
# code 1, with the guard's message (any other fatal error exits 1 too).  A
# launched rank, or an arts_rt that returned, reports RANK_OVERFLOW_NO_ABORT.
add_arts_test(main_rank_overflow_abort)
register_single_node_test(main_rank_overflow_abort STATUS 1 TIMEOUT 60)
set_tests_properties(main_rank_overflow_abort PROPERTIES
    PASS_REGULAR_EXPRESSION "exceeds GUID layout limit"
    FAIL_REGULAR_EXPRESSION "RANK_OVERFLOW_NO_ABORT")

# A rank that exits mid-run ends every peer's run, by the end of its stream;
# the two progress threads race the probes of that end.
add_arts_test(shutdown_abort_eof)
register_multinode_test(shutdown_abort_eof TIMEOUT 120)
foreach(_v 2n_9w_1p 4n_4w_1p 2n_8w_2p)
    set_tests_properties(shutdown_abort_eof_${_v} PROPERTIES PASS_REGULAR_EXPRESSION "ABORT_EOF_EXIT"
        ARTS_EXPECTS_RANK_FAILURE 37)
endforeach()

# Concurrent initialization calls and shutdowns on every rank: one wins on each
# node, and four ranks add peers whose initializations race the first's.
add_arts_test(shutdown_idempotent_concurrent_init)
register_multinode_test(shutdown_idempotent_concurrent_init TIMEOUT 120
                        VARIANTS 2n_9w_1p 4n_4w_1p)
foreach(_v 2n_9w_1p 4n_4w_1p)
    set_tests_properties(shutdown_idempotent_concurrent_init_${_v} PROPERTIES PASS_REGULAR_EXPRESSION "IDEMPOTENT_INIT_EXIT")
endforeach()

# A SIGTERM to rank 0 while every other rank keeps satisfies in flight to it:
# the signal watcher initiates the shutdown and waits for its broadcast's sends
# to complete while those satisfies arrive, and the run ends gracefully.  Where
# the build prints INFO, the wait's own completion line is part of the oracle.
add_arts_test(shutdown_sigterm_inflight_drain)
register_multinode_test(shutdown_sigterm_inflight_drain TIMEOUT 120)
if(ARTS_LOG_LEVEL GREATER_EQUAL 2)
    set(_drain_pass "shutdown TX drain complete.*\\[rank 0\\] SIGTERM_DRAIN_EXIT")
else()
    set(_drain_pass "\\[rank 0\\] SIGTERM_DRAIN_EXIT")
endif()
foreach(_v 2n_9w_1p 4n_4w_1p 2n_8w_2p)
    set_tests_properties(shutdown_sigterm_inflight_drain_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "${_drain_pass}"
        FAIL_REGULAR_EXPRESSION "shutdown TX drain timeout")
endforeach()

# --- random ---
# Concurrent draws from the runtime's generator stay in range and do not
# collide.  Its draws are recorded into one block with no order among them.
add_arts_test(random_thread_safe)
register_single_node_test(random_thread_safe TIMEOUT 60)
set_tests_properties(random_thread_safe PROPERTIES PASS_REGULAR_EXPRESSION "PASS random_thread_safe")

add_arts_test(counter_timer_balance)
register_single_node_test(counter_timer_balance TIMEOUT 30)
set_tests_properties(counter_timer_balance PROPERTIES PASS_REGULAR_EXPRESSION "PASS counter_timer_balance")

# --- public-API surface ---
add_arts_test(api_init_per_node_worker)
register_single_node_test(api_init_per_node_worker TIMEOUT 10)
set_tests_properties(api_init_per_node_worker PROPERTIES PASS_REGULAR_EXPRESSION "PASS api_init_per_node_worker")

add_arts_test(api_current_finish_event_null)
register_single_node_test(api_current_finish_event_null TIMEOUT 10)
set_tests_properties(api_current_finish_event_null PROPERTIES PASS_REGULAR_EXPRESSION "PASS api_current_finish_event_null")
endif()

# --- counters ---
# The counter tests read their values back from the counter files, which carry
# a counter only where the tree's counter config captures it; everywhere else
# there is nothing for a program to read, so it is built (and kept compiling)
# but not registered.  arts_counters_captured(out NAME...) is TRUE when the
# config captures every NAME.
function(arts_counters_captured out)
    set(_all TRUE)
    foreach(_c IN LISTS ARGN)
        file(STRINGS "${COUNTER_CONFIG_FILE}" _line REGEX "^${_c}=")
        if(NOT _line OR _line MATCHES "^${_c}=OFF")
            set(_all FALSE)
        endif()
    endforeach()
    set(${out} ${_all} PARENT_SCOPE)
endfunction()

# counter_smoke_value: the EDTs a program creates and runs are counted exactly,
# main EDT included.  No cell changes how EDTs are counted.
if(ARTS_TEST_REFERENCE_TREE)
    add_arts_test(counter_smoke_value)
    arts_counters_captured(_c NUM_EDT_CREATE NUM_EDT_FINISH)
    if(_c)
        register_single_node_test(counter_smoke_value TIMEOUT 30)
        set_tests_properties(counter_smoke_value PROPERTIES PASS_REGULAR_EXPRESSION "PASS counter_smoke_value")
    endif()
endif()

# counter_exact: exact values of known programs (tests/ocr/counter_exact.c).
# Every cell classifies acquisitions in its own arm, so the acquisition census
# of the one- and two-rank programs registers in every tree, each cell with
# its own values; what no cell changes (creates, destroys, EDTs, the network
# of a one-rank run) is checked in the reference tree only, and so is the
# network of the two-rank exchange below.
add_arts_test(counter_exact)
arts_counters_captured(_c NUM_DB_ACQUIRE_READ NUM_DB_ACQUIRE_WRITE
                       NUM_DB_ACQUIRE_LOCAL_HIT NUM_DB_ACQUIRE_REMOTE)
if(_c)
    # 10 blocks: 4 with the creator's hold, 4 without (one on a reserved
    # GUID), 2 of size zero with the hold; one read and one write each.
    set(_one 0:NUM_DB_ACQUIRE_READ=10 0:NUM_DB_ACQUIRE_WRITE=16
             0:NUM_DB_ACQUIRE_LOCAL_HIT=26 0:NUM_DB_ACQUIRE_REMOTE=0)
    if(ARTS_TEST_REFERENCE_TREE)
        list(APPEND _one 0:NUM_DB_CREATE=10 0:NUM_DB_CREATE_NO_ACQUIRE=4
                         0:BYTES_DB_CREATE=2048 0:NUM_DB_DESTROY=10
                         0:NUM_EDT_CREATE=22 0:NUM_EDT_FINISH=22
                         0:NUM_REMOTE_SEND=0 0:NUM_REMOTE_RECEIVE=0
                         0:BYTES_REMOTE_SENT=0 0:BYTES_REMOTE_RECEIVED=0
                         0:NUM_DB_PAYLOAD_SENT=0 0:BYTES_DB_PAYLOAD_SENT=0)
    endif()
    arts_counters_captured(_c NUM_PURGE_RETURN)
    if(_c AND ARTS_RELEASE_POLICY STREQUAL "PURGE"
       AND NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
        list(APPEND _one 0:NUM_PURGE_RETURN=0)
    endif()
    arts_add_test(counter_exact_one COMMAND counter_exact one ${_one})
    arts_test_cfg(_cfg 1n_20w)
    set_tests_properties(counter_exact_one PROPERTIES
        LABELS "single_node" TIMEOUT 30 RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg}"
        PASS_REGULAR_EXPRESSION "PASS: counter_exact one"
        FAIL_REGULAR_EXPRESSION "FAIL:")

    # The two-rank program, per cell: rank 1 reads 3 times, writes, reads;
    # then rank 0, the home, reads.  Rank 0 also holds two creates.
    set(_two 0:NUM_DB_ACQUIRE_READ=1 0:NUM_DB_ACQUIRE_WRITE=2
             1:NUM_DB_ACQUIRE_READ=4 1:NUM_DB_ACQUIRE_WRITE=1)
    set(_cell "${ARTS_COHERENCE_ARM}_${ARTS_WRITE_POLICY}_${ARTS_RELEASE_POLICY}")
    if(ARTS_COHERENCE_ARM STREQUAL "FLUSH")
        set(_cell "FLUSH")
    endif()
    # rank 0 local, rank 0 remote, rank 1 local, rank 1 remote
    set(_census_VAL_WT_RETAIN 3 0 1 4)
    set(_census_VAL_WT_PURGE 3 0 0 5)
    set(_census_VAL_WB_RETAIN 2 1 1 4)
    set(_census_INV_WT_RETAIN 3 0 3 2)
    set(_census_INV_WT_PURGE 3 0 3 2)
    set(_census_INV_WB_RETAIN 2 1 3 2)
    set(_census_EXCL_WB_PURGE 3 0 0 5)
    set(_census_EXCL_WB_RETAIN 2 1 3 2)
    set(_census_FLUSH 3 0 0 5)
    list(GET _census_${_cell} 0 _l0)
    list(GET _census_${_cell} 1 _r0)
    list(GET _census_${_cell} 2 _l1)
    list(GET _census_${_cell} 3 _r1)
    list(APPEND _two 0:NUM_DB_ACQUIRE_LOCAL_HIT=${_l0}
                     0:NUM_DB_ACQUIRE_REMOTE=${_r0}
                     1:NUM_DB_ACQUIRE_LOCAL_HIT=${_l1}
                     1:NUM_DB_ACQUIRE_REMOTE=${_r1})
    arts_counters_captured(_c NUM_SNAPSHOT_REQUEST NUM_RO_COMBINE_WINDOW
                           NUM_RO_COMBINE_JOINED NUM_SNAPSHOT_HEADER_ONLY)
    if(_c AND _cell STREQUAL "VAL_WT_RETAIN")
        list(APPEND _two 1:NUM_SNAPSHOT_REQUEST=3 1:NUM_RO_COMBINE_WINDOW=3
                         1:NUM_RO_COMBINE_JOINED=0 0:NUM_SNAPSHOT_REQUEST=0
                         0:NUM_SNAPSHOT_HEADER_ONLY=2)
    elseif(_c AND _cell STREQUAL "VAL_WT_PURGE")
        list(APPEND _two 1:NUM_SNAPSHOT_REQUEST=4 1:NUM_RO_COMBINE_WINDOW=4
                         1:NUM_RO_COMBINE_JOINED=0 0:NUM_SNAPSHOT_REQUEST=0
                         0:NUM_SNAPSHOT_HEADER_ONLY=3)
    elseif(_c AND _cell STREQUAL "VAL_WB_RETAIN")
        list(APPEND _two 1:NUM_SNAPSHOT_REQUEST=3 1:NUM_RO_COMBINE_WINDOW=3
                         1:NUM_RO_COMBINE_JOINED=0 0:NUM_SNAPSHOT_REQUEST=1
                         0:NUM_RO_COMBINE_WINDOW=1 0:NUM_SNAPSHOT_HEADER_ONLY=2
                         1:NUM_SNAPSHOT_HEADER_ONLY=0)
    endif()
    arts_counters_captured(_c NUM_EXCL_RECALL_SENT)
    if(_c AND _cell STREQUAL "EXCL_WB_RETAIN")
        list(APPEND _two 0:NUM_EXCL_RECALL_SENT=1 1:NUM_EXCL_RECALL_SENT=0)
    endif()
    # Hand-backs: rank 0's hold of the block homed on rank 1; on rank 1 the
    # write turn, and under exclusion every read turn too.
    arts_counters_captured(_purge NUM_PURGE_RETURN)
    if(_purge AND _cell STREQUAL "EXCL_WB_PURGE")
        list(APPEND _two 0:NUM_PURGE_RETURN=1 1:NUM_PURGE_RETURN=5)
    elseif(_purge AND _cell MATCHES "_PURGE$")
        list(APPEND _two 0:NUM_PURGE_RETURN=1 1:NUM_PURGE_RETURN=1)
    endif()
    if(ARTS_TEST_REFERENCE_TREE)
        list(APPEND _two 0:NUM_DB_CREATE=3 0:NUM_DB_CREATE_NO_ACQUIRE=1
                         0:BYTES_DB_CREATE=768 0:NUM_DB_DESTROY=3
                         1:NUM_DB_CREATE=0 1:NUM_DB_DESTROY=0)
    endif()

    # The home program, per cell: rank 1 writes, then rank 0, the home, reads
    # and writes.  Rank 0's read is local where the home holds the bytes, and
    # its write where the write right belongs to the home (the voluntary-return
    # policy); each is remote where rank 1 has to send it back.
    set(_home 0:NUM_DB_ACQUIRE_READ=1 0:NUM_DB_ACQUIRE_WRITE=2
              1:NUM_DB_ACQUIRE_READ=0 1:NUM_DB_ACQUIRE_WRITE=1
              1:NUM_DB_ACQUIRE_LOCAL_HIT=0 1:NUM_DB_ACQUIRE_REMOTE=1)
    # rank 0 local, rank 0 remote
    set(_home_VAL_WT_RETAIN 2 1)
    set(_home_VAL_WT_PURGE 3 0)
    set(_home_VAL_WB_RETAIN 1 2)
    set(_home_INV_WT_RETAIN 2 1)
    set(_home_INV_WT_PURGE 3 0)
    set(_home_INV_WB_RETAIN 1 2)
    set(_home_EXCL_WB_PURGE 3 0)
    set(_home_EXCL_WB_RETAIN 1 2)
    set(_home_FLUSH 3 0)
    list(GET _home_${_cell} 0 _l0)
    list(GET _home_${_cell} 1 _r0)
    list(APPEND _home 0:NUM_DB_ACQUIRE_LOCAL_HIT=${_l0}
                      0:NUM_DB_ACQUIRE_REMOTE=${_r0})
    if(_purge AND _cell MATCHES "_PURGE$")
        list(APPEND _home 0:NUM_PURGE_RETURN=0 1:NUM_PURGE_RETURN=1)
    endif()

    # The recall program: rank 1 reads, then rank 0, the home and creator,
    # writes.  The write right is rank 0's throughout, so its write is local on
    # every cell, though under exclusion it recalls rank 1's read turn first.
    set(_recall 0:NUM_DB_ACQUIRE_READ=0 0:NUM_DB_ACQUIRE_WRITE=2
                0:NUM_DB_ACQUIRE_LOCAL_HIT=2 0:NUM_DB_ACQUIRE_REMOTE=0
                1:NUM_DB_ACQUIRE_READ=1 1:NUM_DB_ACQUIRE_WRITE=0
                1:NUM_DB_ACQUIRE_LOCAL_HIT=0 1:NUM_DB_ACQUIRE_REMOTE=1)
    arts_counters_captured(_c NUM_EXCL_RECALL_SENT)
    if(_c AND _cell STREQUAL "EXCL_WB_RETAIN")
        list(APPEND _recall 0:NUM_EXCL_RECALL_SENT=1 1:NUM_EXCL_RECALL_SENT=0)
    endif()
    if(_purge AND _cell STREQUAL "EXCL_WB_PURGE")
        list(APPEND _recall 0:NUM_PURGE_RETURN=0 1:NUM_PURGE_RETURN=1)
    elseif(_purge AND _cell MATCHES "_PURGE$")
        list(APPEND _recall 0:NUM_PURGE_RETURN=0 1:NUM_PURGE_RETURN=0)
    endif()

    # The claim program: rank 1's reader acquires the block before rank 1
    # creates it, and the create's claim of rank 1's first-touch cache serves
    # it, except where every acquisition fetches from the home.
    set(_claim 0:NUM_DB_ACQUIRE_READ=0 0:NUM_DB_ACQUIRE_WRITE=0
               0:NUM_DB_ACQUIRE_LOCAL_HIT=0 0:NUM_DB_ACQUIRE_REMOTE=0
               1:NUM_DB_ACQUIRE_READ=1 1:NUM_DB_ACQUIRE_WRITE=1)
    if(_cell STREQUAL "FLUSH")
        list(APPEND _claim 1:NUM_DB_ACQUIRE_LOCAL_HIT=1
                           1:NUM_DB_ACQUIRE_REMOTE=1)
    else()
        list(APPEND _claim 1:NUM_DB_ACQUIRE_LOCAL_HIT=2
                           1:NUM_DB_ACQUIRE_REMOTE=0)
    endif()
    if(_purge AND _cell MATCHES "_PURGE$")
        list(APPEND _claim 0:NUM_PURGE_RETURN=0 1:NUM_PURGE_RETURN=1)
    endif()

    # The pub program: rank 0's reader acquires at the home before rank 1
    # creates the block, so it waits for rank 1's create and first
    # publication, whichever drain then wakes it.  It is remote wherever the
    # copy comes from rank 1, and local where the home holds the write right
    # (the voluntary-return policy) or every home acquisition is local.
    set(_pub 0:NUM_DB_ACQUIRE_READ=1 0:NUM_DB_ACQUIRE_WRITE=0
             1:NUM_DB_ACQUIRE_READ=0 1:NUM_DB_ACQUIRE_WRITE=1
             1:NUM_DB_ACQUIRE_LOCAL_HIT=1 1:NUM_DB_ACQUIRE_REMOTE=0)
    if(_cell MATCHES "_PURGE$" OR _cell STREQUAL "FLUSH")
        list(APPEND _pub 0:NUM_DB_ACQUIRE_LOCAL_HIT=1
                         0:NUM_DB_ACQUIRE_REMOTE=0)
    else()
        list(APPEND _pub 0:NUM_DB_ACQUIRE_LOCAL_HIT=0
                         0:NUM_DB_ACQUIRE_REMOTE=1)
    endif()
    if(_purge AND _cell MATCHES "_PURGE$")
        list(APPEND _pub 0:NUM_PURGE_RETURN=0 1:NUM_PURGE_RETURN=1)
    endif()

    # The join program, its read requests held so the second reader parks
    # while the first's is out: both wait on another rank, and the third
    # reader, made by the first while it holds its own, reads the copy or
    # read turn rank 1 holds, except where a read off the home always asks
    # the home (there each later reader waits for the next request).
    set(_join 0:NUM_DB_ACQUIRE_READ=0 0:NUM_DB_ACQUIRE_WRITE=1
              0:NUM_DB_ACQUIRE_LOCAL_HIT=1 0:NUM_DB_ACQUIRE_REMOTE=0
              1:NUM_DB_ACQUIRE_READ=3 1:NUM_DB_ACQUIRE_WRITE=0)
    if(_cell MATCHES "^(VAL_|FLUSH)")
        list(APPEND _join 1:NUM_DB_ACQUIRE_LOCAL_HIT=0
                          1:NUM_DB_ACQUIRE_REMOTE=3)
    else()
        list(APPEND _join 1:NUM_DB_ACQUIRE_LOCAL_HIT=1
                          1:NUM_DB_ACQUIRE_REMOTE=2)
    endif()
    if(_purge AND _cell STREQUAL "EXCL_WB_PURGE")
        list(APPEND _join 0:NUM_PURGE_RETURN=0 1:NUM_PURGE_RETURN=1)
    elseif(_purge AND _cell MATCHES "_PURGE$")
        list(APPEND _join 0:NUM_PURGE_RETURN=0 1:NUM_PURGE_RETURN=0)
    endif()

    # The gate program, where the write right stays with the rank it moved
    # to: rank 1's writer and the two readers its request serves are remote,
    # and the write the first reader makes once it runs is local, the home's
    # confirmation of rank 1's right still out or not.
    set(_gate 0:NUM_DB_ACQUIRE_READ=0 0:NUM_DB_ACQUIRE_WRITE=1
              0:NUM_DB_ACQUIRE_LOCAL_HIT=1 0:NUM_DB_ACQUIRE_REMOTE=0
              1:NUM_DB_ACQUIRE_READ=2 1:NUM_DB_ACQUIRE_WRITE=2
              1:NUM_DB_ACQUIRE_LOCAL_HIT=1 1:NUM_DB_ACQUIRE_REMOTE=3)
    arts_counters_captured(_c NUM_RO_COMBINE_WINDOW NUM_RO_COMBINE_JOINED)
    if(_c AND _cell MATCHES "^VAL_")
        list(APPEND _join 1:NUM_RO_COMBINE_WINDOW=3 1:NUM_RO_COMBINE_JOINED=2)
        list(APPEND _gate 1:NUM_RO_COMBINE_WINDOW=1 1:NUM_RO_COMBINE_JOINED=1)
    endif()

    # join and gate hold every read request: join's for 100 ms, so its
    # answer serves both readers; gate's past the run, so the write turn
    # serves them.
    set(_shapes two home recall claim pub)
    if(ARTS_DEBUG_INJECTION)
        list(APPEND _shapes join)
        if(_cell MATCHES "_RETAIN$")
            list(APPEND _shapes gate)
        endif()
    endif()
    set(_hook_join "ARTS_REQUEST_DELAY_MS=100:ro")
    set(_hook_gate "ARTS_REQUEST_DELAY_MS=60000:ro")
    foreach(_v 2n_9w_1p 2n_8w_2p)
        arts_test_cfg(_cfg ${_v})
        foreach(_shape IN LISTS _shapes)
            set(_env "ARTS_CONFIG=${_cfg}")
            if(DEFINED _hook_${_shape})
                list(APPEND _env "${_hook_${_shape}}")
            endif()
            arts_add_test(counter_exact_${_shape}_${_v}
                          COMMAND counter_exact ${_shape} ${_${_shape}})
            set_tests_properties(counter_exact_${_shape}_${_v} PROPERTIES
                LABELS "multinode" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
                ENVIRONMENT "${_env}"
                PASS_REGULAR_EXPRESSION "PASS: counter_exact ${_shape}"
                FAIL_REGULAR_EXPRESSION "FAIL:")
        endforeach()
    endforeach()
endif()
# A two-rank exchange: rank 0 creates a 64 KiB block with the hold and an EDT
# on rank 1 that reads it and shuts the run down.  Rank 1 dispatches every
# message rank 0 sends, and takes in the payload whole however many pieces it
# travels in (the split variant cuts it into sixteen); whether rank 0
# dispatches the shutdown message is not fixed (a receiver also stops when it
# sees the initiator's connection close), so its receives are left out.  The
# read is served by one one-sided payload from the home under this tree's
# cell.
arts_counters_captured(_c NUM_REMOTE_SEND NUM_REMOTE_RECEIVE BYTES_REMOTE_SENT
                       BYTES_REMOTE_RECEIVED NUM_DB_PAYLOAD_SENT
                       BYTES_DB_PAYLOAD_SENT)
if(_c AND ARTS_TEST_REFERENCE_TREE)
    set(_ping 1:NUM_REMOTE_RECEIVE=0:NUM_REMOTE_SEND
              1:BYTES_REMOTE_RECEIVED=0:BYTES_REMOTE_SENT
              1:NUM_REMOTE_SEND=2
              0:NUM_DB_PAYLOAD_SENT=1 0:BYTES_DB_PAYLOAD_SENT=65536
              1:NUM_DB_PAYLOAD_SENT=0 1:BYTES_DB_PAYLOAD_SENT=0)
    set(_ping_runs 2n_9w_1p 2n_8w_2p)
    if(ARTS_DEBUG_INJECTION)
        list(APPEND _ping_runs split)
    endif()
    foreach(_v IN LISTS _ping_runs)
        if(_v STREQUAL "split")
            arts_test_cfg(_cfg 2n_9w_1p)
            set(_env "ARTS_CONFIG=${_cfg}" "ARTS_NET_MAX_MSG=4096")
        else()
            arts_test_cfg(_cfg ${_v})
            set(_env "ARTS_CONFIG=${_cfg}")
        endif()
        arts_add_test(counter_exact_ping_${_v}
                      COMMAND counter_exact ping ${_ping})
        set_tests_properties(counter_exact_ping_${_v} PROPERTIES
            LABELS "multinode" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
            ENVIRONMENT "${_env}"
            PASS_REGULAR_EXPRESSION "PASS: counter_exact ping"
            FAIL_REGULAR_EXPRESSION "FAIL:")
    endforeach()
endif()

# --- graph library ---
# The partitions are coherent blocks under startup-reserved GUIDs: each rank
# loads its own, and every rank reads every partition through an RO slot, so a
# multinode run reads partitions homed on other ranks besides its own.
# Distribution, CSR build, both loaders and their argument dispatch.
add_arts_test(csr_graph)
register_single_node_test(csr_graph TIMEOUT 60)
register_multinode_test(csr_graph TIMEOUT 120)
foreach(_t csr_graph csr_graph_2n_9w_1p csr_graph_4n_4w_1p csr_graph_2n_8w_2p)
    set_tests_properties(${_t} PROPERTIES PASS_REGULAR_EXPRESSION "PASS csr_graph")
endforeach()

# Many EDTs race into arts_shutdown: one wins the shutdown gate, the run ends,
# and the state word reads as set.  Its initiators acquire one block RW with no
# order among them, which DB-WRF does not admit.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
add_arts_test(runtime_concurrent_shutdown)
register_single_node_test(runtime_concurrent_shutdown TIMEOUT 60)
set_tests_properties(runtime_concurrent_shutdown PROPERTIES PASS_REGULAR_EXPRESSION "SHUTDOWN_STATE_OK")
endif()

# runtime_gpu_scheduler_promotion: GPU-only (ARTS_USE_GPU); SKIP stub otherwise.
if(BUILD_CUDA_LIBRARY)
    add_arts_test(runtime_gpu_scheduler_promotion)
    target_compile_definitions(runtime_gpu_scheduler_promotion PRIVATE ARTS_USE_GPU)
    register_gpu_test(runtime_gpu_scheduler_promotion TIMEOUT 120)
    set_tests_properties(runtime_gpu_scheduler_promotion PROPERTIES
        PASS_REGULAR_EXPRESSION "GPU_SCHED_PROMOTION_DONE")
endif()

# Its fires record into one block with no order among them, which DB-WRF does
# not admit.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
add_arts_test(api_latch_incr_slot)
register_single_node_test(api_latch_incr_slot TIMEOUT 10)
set_tests_properties(api_latch_incr_slot PROPERTIES PASS_REGULAR_EXPRESSION "PASS api_latch_incr_slot")
endif()

# api_gpu_context_accessors: GPU-only real body (ARTS_TEST_GPU); SKIP stub otherwise.
if(BUILD_CUDA_LIBRARY)
    add_arts_test(api_gpu_context_accessors)
    target_compile_definitions(api_gpu_context_accessors PRIVATE ARTS_TEST_GPU=1)
    register_gpu_test(api_gpu_context_accessors TIMEOUT 10)
    set_tests_properties(api_gpu_context_accessors PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS api_gpu_context_accessors|SKIP api_gpu_context_accessors")
endif()

# --- EDT semantics ---
# Its writers are unordered, which DB-WRF does not admit.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
add_arts_test(edt_sentinel_single_fire)
register_single_node_test(edt_sentinel_single_fire TIMEOUT 30)
set_tests_properties(edt_sentinel_single_fire PROPERTIES PASS_REGULAR_EXPRESSION "PASS edt_sentinel_single_fire")
endif()

# edt_output_event: the consumer sits on rank 1 when there is one, and the
# release that precedes the output event sends nothing a second peer would
# see.  Its writers are unordered, which DB-WRF does not admit.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
add_arts_test(edt_output_event)
register_single_node_test(edt_output_event TIMEOUT 30)
register_multinode_test(edt_output_event TIMEOUT 60 VARIANTS 2n_9w_1p)
foreach(_v "" _2n_9w_1p)
    set_tests_properties(edt_output_event${_v} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
endif()

# edt_finish_scope_balance: round-robins members across all ranks; one peer
# already puts members off the scope's rank.
add_arts_test(edt_finish_scope_balance)
register_single_node_test(edt_finish_scope_balance TIMEOUT 30)
register_multinode_test(edt_finish_scope_balance TIMEOUT 60 VARIANTS 2n_9w_1p)
foreach(_v "" _2n_9w_1p)
    set_tests_properties(edt_finish_scope_balance${_v} PROPERTIES PASS_REGULAR_EXPRESSION "PASS edt_finish_scope_balance")
endforeach()

# edt_remote_create_reordered_satisfy: needs 2+ ranks, so it is multinode
# only; the create and its satisfies go to one rank, and the two progress
# threads let the satisfies overtake the create.
add_arts_test(edt_remote_create_reordered_satisfy)
register_multinode_test(edt_remote_create_reordered_satisfy TIMEOUT 60
                        VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_v 2n_9w_1p 2n_8w_2p)
    set_tests_properties(edt_remote_create_reordered_satisfy_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS edt_remote_create_reordered_satisfy")
endforeach()

if(ARTS_TEST_REFERENCE_TREE)
# edt_size_offsets: pure_unit, but calls libarts symbols (arts_get_depv, check_out_edts) so it
# links libarts via add_arts_test + register_pure_unit_test (starts no runtime).
add_arts_test(edt_size_offsets)
register_pure_unit_test(edt_size_offsets TIMEOUT 30)
set_tests_properties(edt_size_offsets PROPERTIES PASS_REGULAR_EXPRESSION "PASS edt_size_offsets")

# DB_MODE_NULL raw-uint64 dep delivered to depv[slot].
add_arts_test(edt_value)
register_single_node_test(edt_value TIMEOUT 30)
set_tests_properties(edt_value PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS edt_value")
endif()

# GPU LC drain force-defer (GPU builds only).  The registration defines
# ARTS_USE_GPU, which selects the real body over the .c's skip stub.
if(BUILD_CUDA_LIBRARY)
    add_arts_test(edt_gpu_lc_force_defer)
    target_compile_definitions(edt_gpu_lc_force_defer PRIVATE ARTS_USE_GPU)
    register_gpu_test(edt_gpu_lc_force_defer TIMEOUT 30)
    set_tests_properties(edt_gpu_lc_force_defer PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS edt_gpu_lc_force_defer")
endif()

# ============================================================================
# Per-protocol pure_unit tests
#
# These compile specific protocol-coherence source TUs directly (NOT linking
# libarts) via add_pure_unit_src with ARTS_UNIT_STANDALONE_SHIMS, and are built
# and registered only in the trees whose cell compiles the code they test.
# ============================================================================

# directory_grantreq_queue: VAL/EXCL/INV link the one queue body; FLUSH has no
# home request queue.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
add_pure_unit_src(directory_grantreq_queue
    SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/grantreq_queue.c
    DEFINES ARTS_UNIT_STANDALONE_SHIMS PASS_REGEX "PASS directory_grantreq_queue:" TIMEOUT 60)
endif()

# pending_rw_treiber: the grant's cache-side waiter queue, one body for the
# VAL and INV arms.
if(ARTS_TEST_REFERENCE_TREE)
add_pure_unit_src(pending_rw_treiber DEFINES ARTS_UNIT_STANDALONE_SHIMS PASS_REGEX "PASS pending_rw_treiber:" TIMEOUT 60)
endif()

# excl_compute_next: EXCL only, each release policy's arbiters.
if(ARTS_COHERENCE_ARM STREQUAL "EXCL")
add_pure_unit_src(excl_compute_next DEFINES ARTS_UNIT_STANDALONE_SHIMS PASS_REGEX "PASS excl_compute_next:" TIMEOUT 60)
endif()

# val_request_word: VAL only (the request word's arbiter, both write policies
# and release policies alike).
if(ARTS_COHERENCE_ARM STREQUAL "VAL")
add_pure_unit_src(val_request_word DEFINES ARTS_UNIT_STANDALONE_SHIMS PASS_REGEX "PASS val_request_word:" TIMEOUT 60)
endif()

# inv_compute_next: INV only.  One truth table covers both write policies —
# the arbiters do not depend on the write policy.
if(ARTS_COHERENCE_ARM STREQUAL "INV")
add_pure_unit_src(inv_compute_next DEFINES ARTS_UNIT_STANDALONE_SHIMS PASS_REGEX "PASS inv_compute_next:" TIMEOUT 60)
endif()

# db_reader_pool: the parked-reader pool on a cache of the cell, with no
# runtime started (links libarts for the cell's arbiter and destructor).  The
# arms that chain readers by index only; each cell's destructor is its own.
if(ARTS_COHERENCE_ARM STREQUAL "VAL" OR ARTS_COHERENCE_ARM STREQUAL "INV")
add_arts_test(db_reader_pool)
register_pure_unit_test(db_reader_pool TIMEOUT 120)
set_tests_properties(db_reader_pool PROPERTIES PASS_REGULAR_EXPRESSION "PASS db_reader_pool:")
endif()

# acquire_is_serialized: it does NOT #include a coherence .c; it links the real
# arts_db_acquire_is_serialized symbol out of the per-config static libarts and
# never starts the runtime, so it uses add_arts_test (links libarts) +
# register_pure_unit_test.  Runs (and asserts the protocol-correct answer)
# under every configuration.
add_arts_test(acquire_is_serialized)
register_pure_unit_test(acquire_is_serialized TIMEOUT 30)
set_tests_properties(acquire_is_serialized PROPERTIES PASS_REGULAR_EXPRESSION "PASS acquire_is_serialized")

# ============================================================================
# PASS-gate sweep: FAIL_REGULAR_EXPRESSION for OCR/unit tests that print a
# failure token (FAIL/ERROR/MISMATCH) on an internal failure but still exit 0,
# so a silent failure would pass ctest unnoticed.  This is the CONSERVATIVE
# form: a FAIL_REGULAR_EXPRESSION only catches the test's actual printed
# failure token, so it never false-fails a clean run (unlike a PASS regex,
# which would need an exact, always-present success token).  Every token
# matched here is printed ONLY on a failure branch (the runtime's own [ERROR]
# log always aborts, so it never appears on a clean run either).  Tests that
# rely solely on a nonzero exit code, the term_* signal tests and GPU .cu
# tests are intentionally NOT included here.  Every variant a tree registers
# gets the token.
# ============================================================================
set(_arts_pass_gate_fail_tests
    array_list_basic coherence_owner_confirm_gate
    coherence_ro_acquire_stress coherence_stress_single_node
    deque_chaselev_race deque_grow_during_steal deque_last_element_tiebreak
    deque_single_thread diag_timestamp edt_create_basic event_chain
    event_channel_fifo event_channel_lifetime multinode_edt ooo_drain_repush
    ooo_table_completeness ooo_toctou_rescue route_table_remote_guid stress_edt)
set(_arts_pass_gate_fail_error_tests malloc_alignment malloc_footprint_balance)
foreach(_n IN LISTS _arts_pass_gate_fail_tests _arts_pass_gate_fail_error_tests)
    set(_token "FAIL")
    if(_n IN_LIST _arts_pass_gate_fail_error_tests)
        set(_token "FAIL|ERROR")
    endif()
    foreach(_v "" _2n_9w_1p _4n_4w_1p _2n_8w_2p)
        if(TEST ${_n}${_v})
            set_tests_properties(${_n}${_v} PROPERTIES
                FAIL_REGULAR_EXPRESSION "${_token}")
        endif()
    endforeach()
endforeach()
