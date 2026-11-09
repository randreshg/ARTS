# ============================================================================
# Unit, whitebox and OCR-program test registrations, maintained by hand;
# tests/CMakeLists.txt includes this file.  add_arts_test takes a test's source
# from tests/ocr or tests/unit, so programs of both kinds are registered here.
#
# Protocol selection (ARTS_VALIDITY_* / ARTS_RETENTION_* / ARTS_READ_SOURCE_*)
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
        # source root (e.g. "transport/spawner.c", "counter.c") to pull in
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
    arts_apply_protocol(${name} ${ARTS_VALIDITY} ${ARTS_RETENTION} ${ARTS_READ_SOURCE})
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
add_pure_unit_src(shared_slot_dwcas PASS_REGEX "PASS shared_slot_dwcas" TIMEOUT 60)
add_pure_unit_src(mpsc_drain_remaining PASS_REGEX "PASS mpsc_drain_remaining" TIMEOUT 60)
add_pure_unit_src(mpsc_rethread_stub PASS_REGEX "PASS mpsc_rethread_stub" TIMEOUT 60)
add_pure_unit_src(mpsc_transient_empty PASS_REGEX "PASS mpsc_transient_empty" TIMEOUT 60)
# The channel event's gate, compiled with its hooks replaced: a producer held
# inside its push's link window, a nested satisfy, a threaded storm whose
# drain-owned bookkeeping ThreadSanitizer judges, the word's counts at rest
# and its field limit.
add_pure_unit_src(event_channel_gate PASS_REGEX "PASS event_channel_gate" TIMEOUT 60)
add_pure_unit_src(shared_compare_exchange SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c PASS_REGEX "PASS shared_compare_exchange" TIMEOUT 60)
add_pure_unit_src(shared_lifecycle_race SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c PASS_REGEX "PASS shared_lifecycle_race" TIMEOUT 60)
add_pure_unit_src(route_table_install_retire PASS_REGEX "PASS route_table_install_retire" TIMEOUT 60)
# A control block records the slot it was installed into: liveness, the
# one-shot install into a known slot, find-or-install, borrow-or-lookup and
# the identity retire through the recorded slot, with the probes and pins each
# spends where the tree captures those counters.
add_pure_unit_src(route_handle_live PASS_REGEX "PASS route_handle_live" TIMEOUT 60)
add_pure_unit_src(ooo_park_install_retire PASS_REGEX "PASS ooo_park_install_retire" TIMEOUT 120)
add_pure_unit_src(route_table_segment_grow PASS_REGEX "PASS route_table_segment_grow" TIMEOUT 60)
add_pure_unit_src(route_table_growth_residency PASS_REGEX "PASS route_table_growth_residency" TIMEOUT 60)
# The route-table growth tests observe each segment's mapping through
# route_table.c's own mmap/munmap, wrapped at link time: --wrap rewrites only
# the test object's references, and it is the one object that includes
# route_table.c.
target_link_options(route_table_growth_residency PRIVATE -Wl,--wrap=mmap,--wrap=munmap)
# Reservations of one key decide among their claims without waiting on each
# other: a kill, a help while the helped claim's owner is held, a stale help,
# a retire of the key being reserved and a taken LOST word, each driven
# through route_table.c's hooks.
add_pure_unit_src(route_table_claim_assist PASS_REGEX "PASS route_table_claim_assist" TIMEOUT 60)
# A racer whose segment mapping fails re-reads the link once and goes on in a
# published segment, or ends the process naming the mapping, never waiting.
add_pure_unit_src(route_table_growth_mapfail PASS_REGEX "PASS route_table_growth_mapfail" TIMEOUT 60)
target_link_options(route_table_growth_mapfail PRIVATE -Wl,--wrap=mmap,--wrap=munmap)
# Claims of one key, the growth of their chain and retires of the keys being
# reserved race for 200 rounds from fresh roots; in an injection tree also
# under the scheduling fuzz.
add_pure_unit_src(route_table_growth_race
    SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/system/schedfuzz.c
            ${CMAKE_SOURCE_DIR}/libs/src/core/utils/random.c
    PASS_REGEX "PASS route_table_growth_race" TIMEOUT 300)
target_link_options(route_table_growth_race PRIVATE -Wl,--wrap=mmap,--wrap=munmap)
if(ARTS_DEBUG_INJECTION)
    set_tests_properties(route_table_growth_race PROPERTIES
        ENVIRONMENT "ARTS_SCHED_FUZZ=0")
    foreach(_pct 10 40)
        arts_add_test(route_table_growth_race_fuzz${_pct}
                      COMMAND route_table_growth_race)
        set_tests_properties(route_table_growth_race_fuzz${_pct} PROPERTIES
            LABELS "single_node"
            TIMEOUT 300
            RESOURCE_LOCK "pure_unit_test"
            ENVIRONMENT "ARTS_SCHED_FUZZ=${_pct}"
            PASS_REGULAR_EXPRESSION "PASS route_table_growth_race")
    endforeach()
endif()
# The reservation of a freshly minted GUID: one CAS of the first free word of
# its window, past other keys' claims and a held retire, with no wait; a
# fresh reservation of a GUID something already named is caught in Debug.
add_pure_unit_src(route_table_fresh PASS_REGEX "PASS route_table_fresh" TIMEOUT 60)
add_pure_unit_src(route_item_mirror_bridge PASS_REGEX "PASS route_item_mirror_bridge" TIMEOUT 60)
add_pure_unit_src(guid_encoding_roundtrip PASS_REGEX "PASS guid_encoding_roundtrip" TIMEOUT 60)
add_pure_unit_src(guid_from_index_overflow PASS_REGEX "PASS guid_from_index_overflow" TIMEOUT 60)
add_pure_unit_src(guid_reserve_range PASS_REGEX "PASS guid_reserve_range" TIMEOUT 60)
add_pure_unit_src(guid_db_seq_alloc_stress PASS_REGEX "PASS guid_db_seq_alloc_stress" TIMEOUT 120)
# A key's table is a pure function of the key and the node-init geometry,
# agreed on by every thread, entry point and rank id, with the reciprocal
# divisions exact and a large single-source set spread over every table.
add_pure_unit_src(route_table_placement PASS_REGEX "PASS route_table_placement" TIMEOUT 120)
add_pure_unit_src(rank_u64_map_roundtrip SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/rank_u64_map.c PASS_REGEX "PASS rank_u64_map_roundtrip" TIMEOUT 60)
add_pure_unit_src(rank_u64_map_advance SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/rank_u64_map.c PASS_REGEX "PASS rank_u64_map_advance" TIMEOUT 60)
# An interface entry with no address (which getifaddrs may return) is skipped
# by the rank-matching scan rather than dereferenced.
add_pure_unit_src(socket_set_ip_null_ifa PASS_REGEX "PASS socket_set_ip_null_ifa" TIMEOUT 60)
add_pure_unit_src(socket_helpers PASS_REGEX "PASS socket_helpers" TIMEOUT 60)
# Every mesh socket is close-on-exec and kept alive at both ends, and a
# peer's closed or reset connection reads as gone once the mesh is armed.
add_pure_unit_src(socket_mesh_liveness PASS_REGEX "PASS socket_mesh_liveness" TIMEOUT 60)
# The startup failure line: one write of at most 2048 bytes, cut on an entry
# boundary, the two lists parallel; and the budget each process reads.
add_pure_unit_src(mesh_line PASS_REGEX "PASS mesh_line" TIMEOUT 60)
set_tests_properties(mesh_line PROPERTIES
    ENVIRONMENT_MODIFICATION "ARTS_STARTUP_TIMEOUT_S=unset:")
arts_add_test(mesh_line_budget_set COMMAND mesh_line 37)
set_tests_properties(mesh_line_budget_set PROPERTIES
    LABELS "single_node" TIMEOUT 60 RESOURCE_LOCK "pure_unit_test"
    ENVIRONMENT "ARTS_STARTUP_TIMEOUT_S=37"
    PASS_REGULAR_EXPRESSION "PASS mesh_line")
add_pure_unit_src(socket_net_interface_remap PASS_REGEX "PASS socket_net_interface_remap" TIMEOUT 60)
add_pure_unit_src(launcher_shell_quote PASS_REGEX "PASS launcher_shell_quote" TIMEOUT 60)
# Configuration parsing stays inside what it allocated: a routing table
# holds no more rows than it was sized for, a single-host LSF list and a Slurm
# environment that yields no full table are refused, and an environment
# override spliced into the variable list leaves it well formed.
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
add_pure_unit_src(config_value_syntax PASS_REGEX "PASS config_value_syntax" TIMEOUT 60)
add_pure_unit_src(config_thread_count_underflow PASS_REGEX "PASS config_thread_count_underflow" TIMEOUT 60)
add_pure_unit_src(config_launcher_env_precedence PASS_REGEX "PASS config_launcher_env_precedence" TIMEOUT 60)
add_pure_unit_src(config_slurm_setup PASS_REGEX "PASS config_slurm_setup" TIMEOUT 60)
add_pure_unit_src(config_destroy_leak PASS_REGEX "PASS config_destroy_leak" TIMEOUT 60)
add_pure_unit_src(runtime_edt_event_layout PASS_REGEX "PASS runtime_edt_event_layout" TIMEOUT 60)
# topology.c #includes <hwloc.h> and calls hwloc_* — the vendored hwloc
# target carries the headers and the archive together; a host hwloc must
# never satisfy either.
add_pure_unit_src(topology_thread_mask SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/system/placement.c LIBS arts::hwloc PASS_REGEX "PASS topology_thread_mask" TIMEOUT 60)
add_pure_unit_src(threads_worker_underflow PASS_REGEX "PASS threads_worker_underflow" TIMEOUT 60)
add_pure_unit_src(signals_formatters PASS_REGEX "PASS signals_formatters" TIMEOUT 60)
# Which shutdown origins post SHUTDOWN to the peers, and their names.
add_pure_unit_src(exit_origin_posts PASS_REGEX "PASS exit_origin_posts" TIMEOUT 30)
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
# the per-worker created-DB list leans on (lazy alloc, doubling, swap-remove,
# free-keeps-sentinel).
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
if(ARTS_VALIDITY STREQUAL "EXCL")
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
    arts_test_properties(db_acquire_all_bias_${_v} PROPERTIES
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

# --- EDT context: created-DB list ---
add_arts_test(ctx_created_db_release_order)
register_single_node_test(ctx_created_db_release_order TIMEOUT 30)
set_tests_properties(ctx_created_db_release_order PROPERTIES PASS_REGULAR_EXPRESSION "PASS ctx_created_db_release_order")

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
# CDAG does not admit.
if(NOT ARTS_VALIDITY STREQUAL "FLUSH")
add_arts_test(event_channel_transient_null)
add_arts_test(event_satisfy_adddep_window)
add_arts_test(group_latch_rearm)
register_single_node_test(event_channel_transient_null TIMEOUT 60)
# On several ranks its satisfies and dependences also reach the channel's home
# as messages, dispatched by one progress thread or two, which can then hold
# the drain.  No cell changes the channel gate, so the shapes register in the
# reference tree; the program's own block traffic is what changes per cell,
# and the coherence tests check that in every tree.
if(ARTS_TEST_REFERENCE_TREE)
register_multinode_test(event_channel_transient_null TIMEOUT 60)
endif()
register_single_node_test(event_satisfy_adddep_window TIMEOUT 60)
set_tests_properties(event_satisfy_adddep_window PROPERTIES PASS_REGULAR_EXPRESSION "event_satisfy_adddep_window:.*PASS")
register_single_node_test(group_latch_rearm TIMEOUT 60)
set_tests_properties(group_latch_rearm PROPERTIES PASS_REGULAR_EXPRESSION "group_latch_rearm:.*PASS")
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
# FLUSH, which holds no write right, and not EXCL·PURGE, whose permission
# goes back at the zero edge.  Under {VAL,INV}·PURGE·HOME a grant goes
# back to its home at the zero edge, and on one rank it is already there, so
# there the test checks only the holds counted under it.
if(NOT ARTS_VALIDITY STREQUAL "FLUSH" AND
   NOT (ARTS_VALIDITY STREQUAL "EXCL" AND
        ARTS_RETENTION STREQUAL "PURGE"))
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
# transfer run on either.  The shutdown waits on a group that covers
# every task, so the teardown walk must stay silent.
if((ARTS_VALIDITY STREQUAL "VAL" OR ARTS_VALIDITY STREQUAL "INV")
   AND ARTS_RETENTION STREQUAL "PURGE")
add_arts_test(grant_purge_possession)
register_multinode_test(grant_purge_possession TIMEOUT 120
                        VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_v 2n_9w_1p 2n_8w_2p)
    arts_test_properties(grant_purge_possession_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS grant_purge_possession"
        FAIL_REGULAR_EXPRESSION "FAIL;QUIESCENCE-DEBUG:")
endforeach()
endif()

# A GRANT_RESPONSE shorter than its header, or declaring a version map other
# than the bytes that follow the header, is fatal on the receiving rank.  The
# migrating grant exists on VAL and INV, with one handler per read source.
if(ARTS_VALIDITY STREQUAL "VAL" OR ARTS_VALIDITY STREQUAL "INV")
add_arts_test(grant_response_bounds)
arts_test_cfg(_cfg 1n_20w)
arts_add_test(grant_response_bounds_short STATUS 1
              COMMAND grant_response_bounds short)
arts_add_test(grant_response_bounds_map STATUS 1
              COMMAND grant_response_bounds map)
arts_add_test(grant_response_bounds_trail STATUS 1
              COMMAND grant_response_bounds trail)
set_tests_properties(grant_response_bounds_short PROPERTIES
    PASS_REGULAR_EXPRESSION "GRANT_RESPONSE of [0-9]+ bytes is shorter than")
set_tests_properties(grant_response_bounds_map grant_response_bounds_trail
    PROPERTIES PASS_REGULAR_EXPRESSION
    "declares a [0-9]+-byte version map, but [0-9]+ bytes follow it")
set_tests_properties(grant_response_bounds_short grant_response_bounds_map
    grant_response_bounds_trail
    PROPERTIES LABELS "single_node" TIMEOUT 30 RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg}")
endif()

# --- EXCL protocol-specific ---
# The exclusion arm's own cache and home paths, built and registered in its
# trees only.
if(ARTS_VALIDITY STREQUAL "EXCL")
# A same-rank cohort of write acquisitions of one block, every one of which
# must be granted: a lost wakeup leaves the cohort short and the run hangs.
# Two ranks already put the home off the cohort's rank.
add_arts_test(excl_samerank_rw_grant_race)
register_single_node_test(excl_samerank_rw_grant_race TIMEOUT 60)
register_multinode_test(excl_samerank_rw_grant_race TIMEOUT 60
                        VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_v "" _2n_9w_1p _2n_8w_2p)
    arts_test_properties(excl_samerank_rw_grant_race${_v} PROPERTIES
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
arts_test_properties(excl_self_home_no_landing_2n_8w_2p PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS excl_self_home_no_landing")
endif()

add_arts_test(excl_purge_grant_d6_d7)
register_single_node_test(excl_purge_grant_d6_d7 TIMEOUT 60)
register_multinode_test(excl_purge_grant_d6_d7 TIMEOUT 60)
# The end-of-run line is printed only once both halves have finished, so
# requiring it on every variant turns a stranded writer into a failure.
foreach(_v "" _2n_9w_1p _4n_4w_1p _2n_8w_2p)
    arts_test_properties(excl_purge_grant_d6_d7${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION
        "excl_purge_grant_d6_d7 D7: .* D6 chain verified — PASS")
endforeach()
endif()

# Write churn against one home with read turns flipping its phase, so the
# home's grant queue is popped from every handler that grants: every writer
# must run exactly once.  A valid write-churn check on every arm whose
# runtime orders writers; under CDAG that ordering is the program's, and
# this program omits it.
if(NOT ARTS_VALIDITY STREQUAL "FLUSH")
add_arts_test(excl_multiconsumer_queue)
register_single_node_test(excl_multiconsumer_queue TIMEOUT 60)
register_multinode_test(excl_multiconsumer_queue TIMEOUT 60)
foreach(_v "" _2n_9w_1p _4n_4w_1p _2n_8w_2p)
    arts_test_properties(excl_multiconsumer_queue${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "excl_multiconsumer_queue: .* PASS")
endforeach()
endif()

# excl_req_before_create: protocol-agnostic, needs 3+ ranks; registered at
# 4n_4w_1p.
# These assert what the runtime's exclusive-RW serialization guarantees; under
# CDAG that ordering is the program's, and these programs deliberately omit
# it.
if(NOT ARTS_VALIDITY STREQUAL "FLUSH")
add_arts_test(excl_req_before_create)
register_multinode_test(excl_req_before_create TIMEOUT 90 VARIANTS 4n_4w_1p)
arts_test_properties(excl_req_before_create_4n_4w_1p PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: [0-9]+ iterations completed")
endif()

# --- self-send vs dispatch, first-touch claim, Cat-C references, publish-ack
# escape ---
# A message a rank sends itself and the same message from a peer must have one
# effect: every message family on one rank and across ranks, with one oracle.
add_arts_test(self_send_vs_dispatcher_parity)
register_single_node_test(self_send_vs_dispatcher_parity TIMEOUT 120)
register_multinode_test(self_send_vs_dispatcher_parity TIMEOUT 120)
foreach(_v "" _2n_9w_1p _4n_4w_1p _2n_8w_2p)
    arts_test_properties(self_send_vs_dispatcher_parity${_v} PROPERTIES
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
# pinned.  Its write turns on each block are unordered, which CDAG does not
# admit.
if(NOT ARTS_VALIDITY STREQUAL "FLUSH")
add_arts_test(cat_c_ref_balance)
register_single_node_test(cat_c_ref_balance TIMEOUT 120)
register_multinode_test(cat_c_ref_balance TIMEOUT 120)
foreach(_v "" _2n_9w_1p _4n_4w_1p _2n_8w_2p)
    arts_test_properties(cat_c_ref_balance${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: cat_c_ref_balance")
endforeach()
endif()

# A release that waits for its publish or write-back to be acknowledged, or
# for the landing it asks the home for, stays blocked when the shutdown
# overtakes the answer, until the process ends: nothing on the way out waits
# for it, so every rank still ends with status 0.  A release under HOME,
# the invalidation arm's release round and FLUSH's write-back wait for an
# acknowledgement; a release whose bytes travel with it, from a rank that
# holds no landing in the home's copy (a creator away from the block's home,
# among others), asks for one first, and so does EXCL·PURGE's, except in
# the store-backed trees, which write the store instead and wait for nothing.
# One rank answers itself without waiting.  Ranks 0 and 1 carry every turn;
# the two progress threads let the shutdown overtake an answer in flight.
# Its writes of each block are ordered, so FLUSH runs it too.  The contended
# form (every writer of one block at once, so that a release joins its rank's
# publish already in flight) has unordered writers and runs where a release
# publishes: {VAL,INV}·HOME and INV.  The teardown oracle
# sees a releaser still blocked as a thread that did not stop and skips with
# the running-threads line; the other skip lines name a defect and fail the
# test.  The shutdown leaves work undone by design, so the walk's markers do
# not judge it.
set(_blocked_publishes OFF)
if(ARTS_VALIDITY MATCHES "^(VAL|INV)$" AND
   (ARTS_READ_SOURCE STREQUAL "HOME" OR ARTS_VALIDITY STREQUAL "INV"))
    set(_blocked_publishes ON)
endif()
set(_blocked_fail
    "teardown skipped: (origin|the node|[0-9]+ thread\\(s\\) stopped mid-transition)")
if(_blocked_publishes OR ARTS_VALIDITY STREQUAL "FLUSH" OR
   (ARTS_VALIDITY STREQUAL "EXCL" AND ARTS_RETENTION STREQUAL "PURGE"
    AND NOT ARTS_USE_CXL))
add_arts_test(shutdown_blocked_release_ends)
register_multinode_test(shutdown_blocked_release_ends TIMEOUT 30
                        VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_v 2n_9w_1p 2n_8w_2p)
    arts_test_properties(shutdown_blocked_release_ends_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION
        "PASS: shutdown_blocked_release_ends reached shutdown"
        FAIL_REGULAR_EXPRESSION "${_blocked_fail}")
endforeach()
endif()
if(_blocked_publishes)
foreach(_v 2n_9w_1p 2n_8w_2p)
    arts_test_cfg(_cfg ${_v})
    arts_add_multinode_test(shutdown_blocked_release_ends_contended_${_v}
                            COMMAND shutdown_blocked_release_ends contended)
    arts_test_properties(shutdown_blocked_release_ends_contended_${_v}
        PROPERTIES
        LABELS "multinode"
        TIMEOUT 30
        RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg}"
        PASS_REGULAR_EXPRESSION
        "PASS: shutdown_blocked_release_ends reached shutdown"
        FAIL_REGULAR_EXPRESSION "${_blocked_fail}")
endforeach()
endif()

# --- route-table / GUID determinism ---
if(ARTS_TEST_REFERENCE_TREE)
# Reservations minted on workers concurrently, for every home: no two of all
# the ranks' GUIDs collide.  Its reservers record into one block with no order
# among them.
add_arts_test(guid_keygen_table_coupling)
register_multinode_test(guid_keygen_table_coupling TIMEOUT 120
                        VARIANTS 2n_9w_1p 4n_4w_1p)

# Every rank derives the same members of a round-robin range, DBs and events
# alike, and each is created and lives at the home its GUID encodes; four ranks
# are where the rotation shows.
add_arts_test(round_robin_home_distribution)
register_multinode_test(round_robin_home_distribution TIMEOUT 120
                        VARIANTS 2n_9w_1p 4n_4w_1p)

# A local create of a GUID it minted reserves the GUID's slot by the fresh
# path and every other create by the general one, which the slot's claim_key
# tells apart while the object lives.
add_arts_test(route_fresh_path_taken)
register_single_node_test(route_fresh_path_taken TIMEOUT 60)
register_multinode_test(route_fresh_path_taken TIMEOUT 90
                        VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_t route_fresh_path_taken route_fresh_path_taken_2n_9w_1p
        route_fresh_path_taken_2n_8w_2p)
    arts_test_properties(${_t} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: route_fresh_path_taken"
        FAIL_REGULAR_EXPRESSION "FAIL:;QUIESCENCE-DEBUG:")
endforeach()

# Every worker creates events of one reserved range, so their creates, the
# satisfies racing them and their consumers' bindings reserve keys of the same
# chains at once; the smallest route_table_size makes those chains grow under
# the race (and the scheduling fuzz, in an injection tree).
add_arts_test(route_growth_labeled)
register_single_node_test(route_growth_labeled TIMEOUT 120)
register_multinode_test(route_growth_labeled TIMEOUT 180
                        VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_t route_growth_labeled route_growth_labeled_2n_9w_1p
        route_growth_labeled_2n_8w_2p)
    arts_test_properties(${_t} APPEND PROPERTIES
        ENVIRONMENT "route_table_size=1")
    if(ARTS_DEBUG_INJECTION)
        arts_test_properties(${_t} APPEND PROPERTIES ENVIRONMENT
                     "ARTS_SCHED_FUZZ=20")
    endif()
    arts_test_properties(${_t} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: route_growth_labeled 2048 events fired once each"
        FAIL_REGULAR_EXPRESSION "FAIL:;QUIESCENCE-DEBUG:")
endforeach()
endif()

# --- DB create-with-data / stub install ---
add_arts_test(db_create_with_data_bytes)
register_single_node_test(db_create_with_data_bytes TIMEOUT 60)
set_tests_properties(db_create_with_data_bytes PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: db_create_with_data_bytes")

# A slot the acquire phase handed to an arm has one resolver, and a second
# resolution of it ends the run (whitebox, one rank).  The check is a Debug
# assertion, and the resolver runs no cell-dependent code before it.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND ARTS_TEST_REFERENCE_TREE)
add_arts_test(dep_slot_resolved_once)
register_single_node_test(dep_slot_resolved_once TIMEOUT 60 STATUS 1)
set_tests_properties(dep_slot_resolved_once PROPERTIES PASS_REGULAR_EXPRESSION
    "acquire_remaining: before=100 after=99 delta=1.*resolved twice")
endif()

# Each round's block lives off rank 0 and every rank's readers race its
# first-touch stub install, on every arm; at two ranks rank 0 is that racing
# consumer, and four add only more of the same race.
add_arts_test(stub_install_winner)
register_multinode_test(stub_install_winner TIMEOUT 180 VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_v 2n_9w_1p 2n_8w_2p)
    arts_test_properties(stub_install_winner_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: stub_install_winner")
endforeach()

# --- scheduler ---
# The acquire bias keeps an EDT with many dependences from running before its
# last slot resolves, and an RW hand-off chain's resumes push onto the resuming
# thread's own deque; every EDT runs exactly once.  One rank.
# It records into one block from EDTs with no order among them, which CDAG
# does not admit.
if(NOT ARTS_VALIDITY STREQUAL "FLUSH")
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

# --- the spawner ---
# <out> holds the given driver options in the one tree that measures the
# product path by itself, the reference cell's Release tree, and nothing
# elsewhere: a sanitizer, a Debug build or the orderly teardown adds time of
# its own to an end.
function(arts_product_bound out)
    if(ARTS_TEST_REFERENCE_TREE AND CMAKE_BUILD_TYPE STREQUAL "Release")
        set(${out} ${ARGN} PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

# The driver's line check counts the program's lines by regex.
arts_add_test(spawner_driver_lines
    COMMAND ${ARTS_SPAWNER_DRIVER} --want 0 --lines "^A$" 2 2
            -- ${CMAKE_CURRENT_SOURCE_DIR}/tools/print_lines.sh A B A)
set_tests_properties(spawner_driver_lines PROPERTIES
    LABELS "single_node" TIMEOUT 30 RESOURCE_LOCK "arts_runtime")
arts_add_test(spawner_driver_lines_miss STATUS 1
    COMMAND ${ARTS_SPAWNER_DRIVER} --want 0 --lines "^A$" 1 1
            -- ${CMAKE_CURRENT_SOURCE_DIR}/tools/print_lines.sh A A)
set_tests_properties(spawner_driver_lines_miss PROPERTIES
    LABELS "single_node" TIMEOUT 30 RESOURCE_LOCK "arts_runtime"
    PASS_REGULAR_EXPRESSION "spawner_driver: --lines .* matched 2 lines, want 1..1")

# The spawner starts each rank once, rank 0 included, as its own child: every
# rank knows its rank and the total, and every one has the same live parent;
# four ranks make the spawn loop start more than one child past rank 0.
add_arts_test(launcher_orphan_prevention)
register_multinode_test(launcher_orphan_prevention TIMEOUT 60
                        VARIANTS 2n_9w_1p 4n_4w_1p)

# The spawner killed outright takes every rank with it: each rank dies by its
# parent-death signal, so none is alive a second after the kill.
arts_test_cfg(_cfg 2n_9w_1p)
arts_add_multinode_test(launcher_master_killed
    COMMAND ${ARTS_SPAWNER_DRIVER} --want any --on pids --ranks 2
            --signal KILL --target spawner --dead-after-ms 1000
            -- $<TARGET_FILE:spawner_rank_probe> 30000)
arts_test_properties(launcher_master_killed PROPERTIES
    LABELS "multinode" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg}")

# A hundred four-rank startups in a row, each ending with status 0.
arts_test_cfg(_cfg 4n_4w_1p)
arts_product_bound(_bound --bound-ms 10000 --bound-from launch)
arts_add_multinode_test(shutdown_startup_storm
    COMMAND ${ARTS_SPAWNER_DRIVER} --want 0 --repeat 100 ${_bound}
            -- $<TARGET_FILE:term_shutdown>)
arts_test_properties(shutdown_startup_storm PROPERTIES
    LABELS "multinode" TIMEOUT 1200 RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg}")

# A local run whose ranks would need more CPUs than the process may run on is
# refused before any rank starts.
arts_test_cfg(_cfg 1n_20w)
arts_add_test(spawner_oversubscription_refused STATUS 1
              COMMAND spawner_rank_probe)
set_tests_properties(spawner_oversubscription_refused PROPERTIES
    LABELS "single_node" TIMEOUT 5 RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg};node_count=64;SPAWNER_PROBE_NO_RANK=1"
    PASS_REGULAR_EXPRESSION
    "launcher=local: 64 ranks x [0-9]+ threads exceed the [0-9]+ CPUs"
    FAIL_REGULAR_EXPRESSION "SPAWNER_PROBE_RANK_STARTED")

# An end grace that is not a whole number of seconds in 1-86400, or that is
# below the run's startup budget plus the shutdown bound, is refused before
# any rank starts.
arts_test_cfg(_cfg 2n_9w_1p)
math(EXPR _below "20 + ${ARTS_EXIT_WATCHDOG_S} - 1")
foreach(_case "zero_2n_9w_1p;ARTS_END_GRACE_S=0"
              "below_2n_9w_1p;ARTS_STARTUP_TIMEOUT_S=20,ARTS_END_GRACE_S=${_below}")
    list(GET _case 0 _name)
    list(GET _case 1 _vars)
    string(REPLACE "," ";" _vars "${_vars}")
    arts_add_multinode_test(spawner_end_grace_invalid_${_name} STATUS 1
                            COMMAND spawner_rank_probe)
    arts_test_properties(spawner_end_grace_invalid_${_name} PROPERTIES
        LABELS "multinode" TIMEOUT 30 RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg};SPAWNER_PROBE_NO_RANK=1;${_vars}"
        PASS_REGULAR_EXPRESSION "ARTS_END_GRACE_S="
        FAIL_REGULAR_EXPRESSION "SPAWNER_PROBE_RANK_STARTED")
endforeach()
unset(_below)

# A rank stopped for good is killed the end grace after the first rank ended:
# a 10 s startup budget and a grace of that plus the shutdown bound, the
# smallest the spawner accepts.  Only in a Release tree, whose bound is 10 s,
# so the grace is 20 s; the kill's status is the run's, and the grace kills
# rank 2 alone.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    arts_test_cfg(_cfg 4n_4w_1p)
    math(EXPR _grace "10 + ${ARTS_EXIT_WATCHDOG_S}")
    math(EXPR _lo "${_grace} * 1000")
    math(EXPR _hi "${_grace} * 1000 + 2000")
    arts_add_multinode_test(spawner_end_grace
        COMMAND ${ARTS_SPAWNER_DRIVER} --want 137 --on line:ALL_RANKS_READY
                --signal STOP --target rank:2
                --line-window "end-of-step grace of ${_grace} s expired. killing rank 2 "
                              ${_lo} ${_hi}
                --bound-ms 30000 --bound-from launch
                -- $<TARGET_FILE:spawner_rank_probe> 3000)
    arts_test_properties(spawner_end_grace PROPERTIES
        LABELS "multinode" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg};node_count=3;ARTS_STARTUP_TIMEOUT_S=10;ARTS_END_GRACE_S=${_grace}"
        ARTS_RANK_STATUSES 137
        ARTS_END_GRACE_EXPIRES TRUE
        FAIL_REGULAR_EXPRESSION "killing rank [01] ")
    unset(_grace)
    unset(_lo)
    unset(_hi)
endif()

# A rank that ended is reaped once and never signalled again: rank 2 aborts
# with 3, and the spawner, signalled once that rank's end was reported,
# forwards the signal to the ranks still running only.  The run's status is
# rank 2's 3, or 143 where a rank the signal reached ends by it.
arts_test_cfg(_cfg 4n_4w_1p)
arts_add_multinode_test(spawner_signal_after_reap
    COMMAND ${ARTS_SPAWNER_DRIVER} --want 3,143
            "--on=line:\\[ARTS\\] spawner: rank 2 \\(pid [0-9]+\\) ended with status 3"
            --signal TERM --target spawner
            -- $<TARGET_FILE:spawner_rank_probe> 30000 2:3)
arts_test_properties(spawner_signal_after_reap PROPERTIES
    LABELS "multinode" TIMEOUT 120 RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg};node_count=3"
    ARTS_RANK_STATUSES "3;143"
    FAIL_REGULAR_EXPRESSION "spawner: forward [A-Z]+ to rank 2 ")
if(ARTS_LOG_LEVEL GREATER_EQUAL 3)
    arts_test_properties(spawner_signal_after_reap PROPERTIES
        PASS_REGULAR_EXPRESSION "spawner: rank 2 \\(pid [0-9]+\\) reaped, status 3")
endif()

# Every worker of rank 0 streams to rank 1, and rank 1 is killed: rank 0
# learns of the end and ends, and the kill's status is the run's.
arts_test_cfg(_cfg 2n_9w_1p)
arts_product_bound(_bound --bound-ms 2000)
arts_add_multinode_test(shutdown_spin_peer_killed
    COMMAND ${ARTS_SPAWNER_DRIVER} --want 137 --on line:FLOODING
            --signal KILL --target rank:1 ${_bound}
            -- $<TARGET_FILE:shutdown_spin_peer_killed>)
arts_test_properties(shutdown_spin_peer_killed PROPERTIES
    LABELS "multinode" TIMEOUT 120 RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg}"
    ARTS_RANK_STATUSES 137)

# The startup budget is ASCII digits in 10-86400; any other set value is
# refused by the process you start, before any rank starts.
arts_test_cfg(_cfg 2n_9w_1p)
foreach(_case "low;9" "high;86401" "text;10s" "zero;0" "empty;")
    list(GET _case 0 _name)
    list(GET _case 1 _value)
    arts_add_multinode_test(startup_budget_refused_${_name}_2n_9w_1p STATUS 1
                            COMMAND spawner_rank_probe)
    arts_test_properties(startup_budget_refused_${_name}_2n_9w_1p PROPERTIES
        LABELS "multinode" TIMEOUT 30 RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg};SPAWNER_PROBE_NO_RANK=1;ARTS_STARTUP_TIMEOUT_S=${_value}"
        PASS_REGULAR_EXPRESSION "ARTS_STARTUP_TIMEOUT_S=\"${_value}\" is not a whole number of seconds in 10-86400"
        FAIL_REGULAR_EXPRESSION "SPAWNER_PROBE_RANK_STARTED")
endforeach()
# The startup bound ends at the arm on every rank and before the spawn in
# the spawner: a run that outlives budget + 1 s ends 0 with no line.
arts_add_multinode_test(startup_bound_ends_at_arm_2n_9w_1p
    COMMAND spawner_rank_probe 13000)
arts_test_properties(startup_bound_ends_at_arm_2n_9w_1p PROPERTIES
    LABELS "multinode" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg};ARTS_STARTUP_TIMEOUT_S=10"
    PASS_REGULAR_EXPRESSION "ALL_RANKS_READY"
    FAIL_REGULAR_EXPRESSION "ARTS_MESH_FAILED")
# A one-rank run ends its bound once its configuration is loaded.
arts_test_cfg(_cfg1 1n_20w)
arts_add_test(startup_bound_one_rank COMMAND spawner_rank_probe 13000)
set_tests_properties(startup_bound_one_rank PROPERTIES
    LABELS "single_node" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg1};ARTS_STARTUP_TIMEOUT_S=10"
    PASS_REGULAR_EXPRESSION "ALL_RANKS_READY"
    FAIL_REGULAR_EXPRESSION "ARTS_MESH_FAILED")
if(ARTS_DEBUG_INJECTION)
    if(ARTS_DEBUG_SANITIZER)
        set(_mesh_bound 30000)
    else()
        set(_mesh_bound 16000)
    endif()
    # A rank stuck in its configuration step is ended by the startup timer
    # 1 s after its deadline, with its own line naming the step and no rank.
    arts_test_cfg(_cfg3 4n_4w_1p)
    arts_add_multinode_test(mesh_stall_config_3n
        COMMAND ${ARTS_SPAWNER_DRIVER} --want 75 --bound-ms ${_mesh_bound}
                --bound-from launch
                --lines "^ARTS_MESH_FAILED rank=- host=[^ ]+ phase=config missing=- missing_hosts=- more=0$" 1 1
                -- $<TARGET_FILE:spawner_rank_probe>)
    arts_test_properties(mesh_stall_config_3n PROPERTIES
        LABELS "multinode" TIMEOUT 90 RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg3};node_count=3;ARTS_STARTUP_TIMEOUT_S=10;ARTS_MESH_FAULT=1:stall:config"
        ARTS_RANK_STATUSES 75)
    # A malformed hook value is refused by the process you start.
    arts_add_multinode_test(mesh_fault_malformed_2n_9w_1p STATUS 1
                            COMMAND spawner_rank_probe)
    arts_test_properties(mesh_fault_malformed_2n_9w_1p PROPERTIES
        LABELS "multinode" TIMEOUT 30 RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg};SPAWNER_PROBE_NO_RANK=1;ARTS_MESH_FAULT=1:stall:nowhere"
        PASS_REGULAR_EXPRESSION "ARTS_MESH_FAULT=\"1:stall:nowhere\""
        FAIL_REGULAR_EXPRESSION "SPAWNER_PROBE_RANK_STARTED")
    # A hook naming a rank outside the run is refused by the process you
    # start.
    arts_add_multinode_test(mesh_fault_rank_outside_2n_9w_1p STATUS 1
                            COMMAND spawner_rank_probe)
    arts_test_properties(mesh_fault_rank_outside_2n_9w_1p PROPERTIES
        LABELS "multinode" TIMEOUT 30 RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg};SPAWNER_PROBE_NO_RANK=1;ARTS_MESH_FAULT=2:stall:config"
        PASS_REGULAR_EXPRESSION "ARTS_MESH_FAULT names rank 2, but the run's ranks are 0-1"
        FAIL_REGULAR_EXPRESSION "SPAWNER_PROBE_RANK_STARTED")
    # A run of one rank refuses the hook before any step acts.
    arts_add_test(mesh_fault_one_rank_refused STATUS 1
                  COMMAND spawner_rank_probe)
    set_tests_properties(mesh_fault_one_rank_refused PROPERTIES
        LABELS "single_node" TIMEOUT 30 RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg1};ARTS_RANK=0;ARTS_MESH_FAULT=0:stall:config"
        PASS_REGULAR_EXPRESSION "ARTS_MESH_FAULT is set, but a one-rank run"
        FAIL_REGULAR_EXPRESSION "ALL_RANKS_READY")
    # Under lsf no variable names the rank, so the hook is refused there (a
    # run of two ranks: the host list's first entry is the batch host).
    arts_add_test(mesh_fault_lsf_refused STATUS 1 COMMAND spawner_rank_probe)
    set_tests_properties(mesh_fault_lsf_refused PROPERTIES
        LABELS "single_node" TIMEOUT 30 RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg};launcher=lsf;LSB_HOSTS=batch 127.0.0.1 localhost;ports=25000;ARTS_MESH_FAULT=0:stall:config"
        PASS_REGULAR_EXPRESSION "ARTS_MESH_FAULT is set, but launcher=lsf"
        FAIL_REGULAR_EXPRESSION "ALL_RANKS_READY")
    unset(_mesh_bound)
    unset(_cfg3)
endif()
unset(_cfg1)

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
# A verdict recorded by a task on the last rank is the exit status arts_rt
# returns, through the program's arts_app_exit_status.  The shared-library
# twin pins that the library's weak default yields to the program's definition.
add_arts_test(app_status_reaches_exit)
register_single_node_test(app_status_reaches_exit TIMEOUT 30 STATUS 1)
set_tests_properties(app_status_reaches_exit PROPERTIES
    PASS_REGULAR_EXPRESSION "APP_STATUS_RECORDED app_status_reaches_exit")
register_multinode_test(app_status_reaches_exit TIMEOUT 60 STATUS 1
                        VARIANTS 2n_9w_1p)
arts_test_properties(app_status_reaches_exit_2n_9w_1p PROPERTIES
    PASS_REGULAR_EXPRESSION "APP_STATUS_RECORDED app_status_reaches_exit")
if(TARGET arts_shared)
    add_arts_test(app_status_reaches_exit_shared
                  SOURCE app_status_reaches_exit RUNTIME arts_shared)
    register_single_node_test(app_status_reaches_exit_shared TIMEOUT 30
                              STATUS 1)
    set_tests_properties(app_status_reaches_exit_shared PROPERTIES
        PASS_REGULAR_EXPRESSION "APP_STATUS_RECORDED app_status_reaches_exit")
endif()

add_arts_test(runtime_barrier_counts)
register_single_node_test(runtime_barrier_counts TIMEOUT 60)

# A shutdown while an EDT body runs on and never returns: the run still ends
# with status 0, well within its 10 s.  Where the teardown runs, its stop
# cannot stop that worker, so the teardown frees nothing and says so in a
# DEBUG line.
add_arts_test(shutdown_cuts_running_edt)
register_single_node_test(shutdown_cuts_running_edt TIMEOUT 10)
if(ARTS_DEBUG_TEARDOWN AND ARTS_LOG_LEVEL GREATER_EQUAL 3)
    arts_test_cfg(_cfg 1n_20w)
    arts_add_test(teardown_stop_skips_running
                  COMMAND shutdown_cuts_running_edt)
    set_tests_properties(teardown_stop_skips_running PROPERTIES
        LABELS "single_node"
        TIMEOUT 10
        RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg}"
        PASS_REGULAR_EXPRESSION
        "teardown skipped: 1 thread\\(s\\) still running")
endif()

# The shutdown comes while workers steal EDTs from one another's deques and
# the progress threads'; the teardown frees the deques only after every thief
# has stopped, which ThreadSanitizer judges (its reports are in every fail
# list).  The scheduler fuzz widens the windows a thief spends between its
# loads.
if(ARTS_DEBUG_TEARDOWN AND ARTS_DEBUG_SANITIZER STREQUAL "THREAD" AND
   ARTS_DEBUG_INJECTION)
    add_arts_test(teardown_deques_after_stop)
    register_single_node_test(teardown_deques_after_stop TIMEOUT 60)
    register_multinode_test(teardown_deques_after_stop TIMEOUT 120
                            VARIANTS 2n_9w_1p)
    foreach(_t teardown_deques_after_stop teardown_deques_after_stop_2n_9w_1p)
        arts_test_properties(${_t} APPEND PROPERTIES
            ENVIRONMENT "ARTS_SCHED_FUZZ=25")
        arts_test_properties(${_t} PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS: teardown_deques_after_stop")
    endforeach()
endif()

add_arts_test(threads_pin_affinity)
register_single_node_test(threads_pin_affinity TIMEOUT 60)

# One SIGTERM the program sends its own process ends the rank with 143,
# after the watcher's line naming it.
add_arts_test(signals_sigterm_status)
register_single_node_test(signals_sigterm_status TIMEOUT 60 STATUS 143)
set_tests_properties(signals_sigterm_status PROPERTIES PASS_REGULAR_EXPRESSION
    "\\[ARTS\\] Received SIGTERM \\(rank 0\\); ending the run")

# main_rank_overflow_abort: success is the overflow guard's own abort: exit
# code 1, with the guard's message (any other fatal error exits 1 too).  A
# launched rank reports RANK_OVERFLOW_NO_ABORT.
add_arts_test(main_rank_overflow_abort)
register_single_node_test(main_rank_overflow_abort STATUS 1 TIMEOUT 60)
arts_test_cfg(_cfg 1n_20w)
set_tests_properties(main_rank_overflow_abort PROPERTIES
    ENVIRONMENT "ARTS_CONFIG=${_cfg};launcher=local"
    PASS_REGULAR_EXPRESSION "exceeds GUID layout limit"
    FAIL_REGULAR_EXPRESSION "RANK_OVERFLOW_NO_ABORT")

# A rank that aborts mid-run ends every peer's run, by its post or by the end
# of its stream; the two progress threads race to learn of that end.  The
# aborted rank's status is the run's, and every other rank ends with 0 (no
# other rank status is allowed).
add_arts_test(shutdown_abort_status)
register_multinode_test(shutdown_abort_status TIMEOUT 120 STATUS 37)
foreach(_v 2n_9w_1p 4n_4w_1p 2n_8w_2p)
    arts_test_properties(shutdown_abort_status_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "ABORT_STATUS: [0-9]+ ranks")
endforeach()

# Concurrent initialization calls and shutdowns on every rank: one wins on each
# node, and four ranks add peers whose initializations race the first's.
add_arts_test(shutdown_idempotent_concurrent_init)
register_multinode_test(shutdown_idempotent_concurrent_init TIMEOUT 120
                        VARIANTS 2n_9w_1p 4n_4w_1p)
foreach(_v 2n_9w_1p 4n_4w_1p)
    arts_test_properties(shutdown_idempotent_concurrent_init_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "IDEMPOTENT_INIT_EXIT")
endforeach()

# A SIGTERM to rank 0 while every other rank keeps satisfies in flight to it:
# the signal watcher ends rank 0, closing its send gate and posting SHUTDOWN
# while those satisfies arrive, and the run's status is rank 0's 143.
add_arts_test(shutdown_sigterm_inflight)
register_multinode_test(shutdown_sigterm_inflight TIMEOUT 120 STATUS 143)
foreach(_v 2n_9w_1p 4n_4w_1p 2n_8w_2p)
    arts_test_properties(shutdown_sigterm_inflight_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "\\[rank 0\\] SIGTERM_INFLIGHT_EXIT")
endforeach()

# Every rank still sending when the shutdown begins: each rank drops its sends
# after its shutdown point, so none waits on a peer that has gone, a refused
# inject, a transfer lost on a live run and a send by the end's owner after
# its post being the failures.
add_arts_test(shutdown_sends_after_shutdown)
register_multinode_test(shutdown_sends_after_shutdown TIMEOUT 60)
foreach(_v 2n_9w_1p 4n_4w_1p 2n_8w_2p)
    arts_test_properties(shutdown_sends_after_shutdown_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "SENDS_AFTER_SHUTDOWN: ending the run"
        FAIL_REGULAR_EXPRESSION
        "fi_inject failed;cq error on a live run;after this thread's shutdown post")
endforeach()

# Two ranks other than rank 0 end the run at once, on four ranks and on three
# (the four-rank cfg with node_count=3).
add_arts_test(shutdown_initiators_off_zero)
register_multinode_test(shutdown_initiators_off_zero TIMEOUT 60
                        VARIANTS 4n_4w_1p)
arts_test_cfg(_cfg 4n_4w_1p)
arts_add_multinode_test(shutdown_initiators_off_zero_3n_4w_1p
                        COMMAND shutdown_initiators_off_zero)
arts_test_properties(shutdown_initiators_off_zero_3n_4w_1p PROPERTIES
    LABELS "multinode" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg};node_count=3")

# A rank posts SHUTDOWN only to the peers it contacted: rank 1 ends the run
# once ranks 0 and 2, the two peers it sent to, have answered, so rank 3
# learns of the end from a closed connection.  Where the build prints INFO,
# rank 3's entry must name that origin and rank 1's post its two peers, in
# either order, and a post naming any other count fails the run.
add_arts_test(shutdown_first_contact)
register_multinode_test(shutdown_first_contact TIMEOUT 60 VARIANTS 4n_4w_1p)
if(ARTS_LOG_LEVEL GREATER_EQUAL 2)
    set(_fc_r3 "\\[3:[0-9]+\\] \\[INFO\\] arts_exit: rank=3 origin=SENTINEL")
    set(_fc_r1 "\\[1:[0-9]+\\] \\[INFO\\] arts_exit: posted SHUTDOWN to")
    arts_test_properties(shutdown_first_contact_4n_4w_1p PROPERTIES
        PASS_REGULAR_EXPRESSION "${_fc_r3}.*${_fc_r1} 2 peer|${_fc_r1} 2 peer.*${_fc_r3}"
        FAIL_REGULAR_EXPRESSION "${_fc_r1} ([013-9]|[0-9][0-9]+) peer")
else()
    arts_test_properties(shutdown_first_contact_4n_4w_1p PROPERTIES
        PASS_REGULAR_EXPRESSION "FIRST_CONTACT: rank 1 ends the run")
endif()

# Every worker of rank 1 floods rank 2, which never contacted rank 1, while
# rank 2 ends the run: rank 1 learns of the end from rank 2's closed
# connection, at a probe of its progress loop or of a send held in the
# ring-full spin, and drops its sends from then on, within the liveness bound
# of startup, 10 s and teardown.  Where the build prints INFO, rank 1's entry
# must name an origin of a rank that learned of the end.
add_arts_test(shutdown_under_flood)
register_multinode_test(shutdown_under_flood TIMEOUT 40 VARIANTS 4n_4w_1p)
set(_uf_end "SHUTDOWN_UNDER_FLOOD: rank 2 ends the run")
set(_uf_fail
    "fi_inject failed;cq error on a live run;after this thread's shutdown post")
if(ARTS_LOG_LEVEL GREATER_EQUAL 2)
    set(_uf_r1 "\\[1:[0-9]+\\] \\[INFO\\] arts_exit: rank=1 origin=")
    arts_test_properties(shutdown_under_flood_4n_4w_1p PROPERTIES
        PASS_REGULAR_EXPRESSION
        "${_uf_end}.*${_uf_r1}(WIRE|SENTINEL|BACKPRESSURE)|${_uf_r1}(WIRE|SENTINEL|BACKPRESSURE).*${_uf_end}"
        FAIL_REGULAR_EXPRESSION
        "${_uf_fail};${_uf_r1}(APP|SIGNAL|FAULT|RUNTIME_ERROR)")
else()
    arts_test_properties(shutdown_under_flood_4n_4w_1p PROPERTIES
        PASS_REGULAR_EXPRESSION "${_uf_end}"
        FAIL_REGULAR_EXPRESSION "${_uf_fail}")
endif()

# --- the rank's end (arts_exit) ---
# Records exist where some counter is captured at NODE level; a test that
# reads one registers only there.
arts_counters_node_level(_records)

# Every rank ends the run at once: each takes its own shutdown point or learns
# of another's first, every rank ends with 0, and rank 0 prints one marker.
# On four ranks and on three (the four-rank cfg with node_count=3).
add_arts_test(shutdown_concurrent_initiators)
arts_test_cfg(_cfg 4n_4w_1p)
foreach(_case "4n_4w_1p;4;" "3n_4w_1p;3;node_count=3")
    list(GET _case 0 _v)
    list(GET _case 1 _n)
    list(GET _case 2 _extra)
    set(_rec "")
    if(_records)
        set(_rec RECORDS ${_n})
    endif()
    arts_add_multinode_test(shutdown_concurrent_initiators_${_v} ${_rec}
                            COMMAND shutdown_concurrent_initiators)
    arts_test_properties(shutdown_concurrent_initiators_${_v} PROPERTIES
        LABELS "multinode" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg};ARTS_E2E_MARKER=1;${_extra}"
        PASS_REGULAR_EXPRESSION "\\[E2E\\] [0-9]+"
        FAIL_REGULAR_EXPRESSION "\\[E2E\\].*\\[E2E\\];\\[SHUTDOWN\\]")
endforeach()

# A stack overflow on rank 1: no alternate signal stack exists, so the kernel
# ends that rank by SIGSEGV under the default action (139, the run's status),
# and rank 0 learns of the end from its closed connection.  A sanitizer's own
# SIGSEGV handler is turned off, so the kernel's action is the one judged.
add_arts_test(term_stack_overflow)
register_multinode_test(term_stack_overflow TIMEOUT 60 STATUS 139 ARGS 1
                        VARIANTS 2n_9w_1p)
arts_test_properties(term_stack_overflow_2n_9w_1p PROPERTIES
    ENVIRONMENT_MODIFICATION
    "ASAN_OPTIONS=path_list_append:handle_segv=0;TSAN_OPTIONS=path_list_append:handle_segv=0"
    PASS_REGULAR_EXPRESSION "STACK_OVERFLOW: rank 1 recurses")

# A rank killed outright mid-run: the others learn of the end from its closed
# connections and end with 0, and the kill's 137 is the run's status.
arts_test_cfg(_cfg 4n_4w_1p)
arts_product_bound(_bound --bound-ms 1000)
arts_add_multinode_test(shutdown_peer_killed
    COMMAND ${ARTS_SPAWNER_DRIVER} --want 137 --on line:ALL_RANKS_READY
            --signal KILL --target rank:1 ${_bound}
            -- $<TARGET_FILE:spawner_rank_probe> 30000)
arts_test_properties(shutdown_peer_killed PROPERTIES
    LABELS "multinode" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg}"
    ARTS_RANK_STATUSES 137)

# A rank whose program calls _exit(0) mid-run: no runtime code runs on its way
# out, every other rank learns of the end from its closed connections, and
# the run's status is 0.
add_arts_test(shutdown_rank_calls_exit)
arts_test_cfg(_cfg 4n_4w_1p)
arts_product_bound(_bound --bound-ms 1000)
arts_add_multinode_test(shutdown_rank_calls_exit
    COMMAND ${ARTS_SPAWNER_DRIVER} --want 0 ${_bound}
            -- $<TARGET_FILE:shutdown_rank_calls_exit>)
arts_test_properties(shutdown_rank_calls_exit PROPERTIES
    LABELS "multinode" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg}"
    PASS_REGULAR_EXPRESSION "RANK_CALLS_EXIT: rank 1")

# Rank 1 floods rank 0, which the driver stops; the last rank, signalled,
# ends with 143, every rank but 0 learns of the end and ends (rank 1's
# senders dropping their sends at its gate), and rank 0, continued once they
# are gone, learns of it and ends with 0.
add_arts_test(shutdown_peer_stopped)
arts_product_bound(_bound --bound-ms 3000 --bound-from signal:SIGTERM
                   --bound-ranks except:0)
foreach(_v 2n_9w_1p 4n_4w_1p)
    string(REGEX MATCH "^[0-9]+" _n ${_v})
    math(EXPR _last "${_n} - 1")
    arts_test_cfg(_cfg ${_v})
    arts_add_multinode_test(shutdown_peer_stopped_${_v}
        COMMAND ${ARTS_SPAWNER_DRIVER} --want 143 --on line:FLOODING
                --signal STOP --target rank:0
                --then TERM:rank:${_last}:200
                --then CONT:rank:0:others-gone ${_bound}
                -- $<TARGET_FILE:shutdown_peer_stopped>)
    arts_test_properties(shutdown_peer_stopped_${_v} PROPERTIES
        LABELS "multinode" TIMEOUT 90 RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg}"
        ARTS_RANK_STATUSES 143
        FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
unset(_last)
unset(_bound)

# A child a rank forks that calls arts_abort ends with its code alone: the
# parent sees 5, and the run ends with 0 and one marker.
add_arts_test(shutdown_fork_child_abort)
register_single_node_test(shutdown_fork_child_abort TIMEOUT 30)
set_tests_properties(shutdown_fork_child_abort PROPERTIES
    PASS_REGULAR_EXPRESSION
    "FORK_CHILD_ABORT: the child ended with 5.*\\[E2E\\] [0-9]+|\\[E2E\\] [0-9]+.*FORK_CHILD_ABORT: the child ended with 5"
    FAIL_REGULAR_EXPRESSION "FAIL;\\[E2E\\].*\\[E2E\\];\\[SHUTDOWN\\]")
set_property(TEST shutdown_fork_child_abort APPEND PROPERTY ENVIRONMENT
             "ARTS_E2E_MARKER=1")

# A run whose standard output's reader has gone (`| head -1`): its writes and
# its end's flush fail, and its status is still 0, with no hang.
add_arts_test(shutdown_stdout_closed)
arts_test_cfg(_cfg 1n_20w)
arts_add_test(shutdown_stdout_closed COMMAND bash -o pipefail -c
    "$<TARGET_FILE:shutdown_stdout_closed> | head -1")
set_tests_properties(shutdown_stdout_closed PROPERTIES
    LABELS "single_node" TIMEOUT 30 RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg}"
    PASS_REGULAR_EXPRESSION "STDOUT_CLOSED: first line")

# A SIGTERM to the spawner at once, while its ranks start: each rank ends by
# it (143), or by its mesh's give-up (75) where a peer it waits for ended
# first, and nothing is left running.
arts_test_cfg(_cfg 2n_9w_1p)
arts_add_multinode_test(shutdown_sigterm_during_bootstrap
    COMMAND ${ARTS_SPAWNER_DRIVER} --want 143,75 --on launch
            --signal TERM --target spawner
            -- $<TARGET_FILE:spawner_rank_probe> 30000)
arts_test_properties(shutdown_sigterm_during_bootstrap PROPERTIES
    LABELS "multinode" TIMEOUT 90 RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg}"
    ARTS_RANK_STATUSES "143;75")

# A fault after rank 1 wrote its record: the fault's 139 is the run's status,
# and the record keeps the rc 0 it was written with.
if(_records)
    add_arts_test(shutdown_status_above_record)
    register_multinode_test(shutdown_status_above_record TIMEOUT 60
        STATUS 139 VARIANTS 2n_9w_1p RECORDS 2
        RECORD_CHECK record_meta.cmake rc@1=0 rc@0=0)
    arts_test_properties(shutdown_status_above_record_2n_9w_1p PROPERTIES
        PASS_REGULAR_EXPRESSION "STATUS_ABOVE_RECORD: rank 1 waits"
        FAIL_REGULAR_EXPRESSION "FAIL")
endif()

# A step of the rank's end that never finishes is ended by the watchdog, no
# sooner than the bound less a second after the take and no later than the
# bound plus five: the end's last flush waits for stdout, which an EDT holds
# (status 71, also in a process started with the watchdog's signal blocked;
# the 9 of a late entry that parks holding it; or, for an end a fault began
# inside its crash handler, death by the fault's signal again), or the node
# record's open waits for a reader of a FIFO (the record is lost and the
# status 0 kept; where records exist).
add_arts_test(shutdown_watchdog)
math(EXPR _wd_timeout "${ARTS_EXIT_WATCHDOG_S} + 30")
math(EXPR _wd_alive "${ARTS_EXIT_WATCHDOG_S} - 1")
set(_wd_line "WATCHDOG_TEST: running ${_wd_alive} s after the take.*\\[ARTS\\] shutdown watchdog: rank 0 stuck in phase")
arts_test_cfg(_cfg 1n_20w)
foreach(_case "shutdown_watchdog_fires;fires;71" "shutdown_late_failure;late;9"
              "shutdown_watchdog_blocked;blocked;71")
    list(GET _case 0 _name)
    list(GET _case 1 _mode)
    list(GET _case 2 _status)
    arts_add_test(${_name} STATUS ${_status}
                  COMMAND shutdown_watchdog ${_mode})
    set_tests_properties(${_name} PROPERTIES
        LABELS "single_node" TIMEOUT ${_wd_timeout}
        RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg}"
        PASS_REGULAR_EXPRESSION "${_wd_line} flush\n"
        FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
# The fault's ends judged as term_sigsegv's are (expect_fault.sh, no core
# check), each in its own working directory, and the death by the signal
# itself too: bash reports a command a signal ended ("Segmentation fault",
# in the C locale) and not one that exited with 139, and expect_fault.sh
# keeps that report in order with the program's output.  The end runs
# inside the crash handler, its record's stdio included, so a
# ThreadSanitizer tree turns off the report of an allocation inside a signal
# handler for the run.  In fault_spin the end is stuck in the crash handler
# on a CPU it keeps, so on a kernel that hands a process timer's signal to
# the thread it interrupts (Linux 6.4 on) the watchdog's signal reaches the
# thread whose SIGSEGV is blocked (elsewhere it runs as fault does);
# ThreadSanitizer defers a signal to a thread spinning outside its
# interceptors, so a ThreadSanitizer tree does not register it.
foreach(_case "shutdown_watchdog_fault;fault;\\[ARTS\\] Crashed: SIGSEGV.*${_wd_line} flush"
              "shutdown_watchdog_fault_spin;fault_spin;${_wd_line} post")
    list(GET _case 0 _name)
    list(GET _case 1 _mode)
    list(GET _case 2 _head)
    if(_mode STREQUAL "fault_spin" AND ARTS_DEBUG_SANITIZER STREQUAL "THREAD")
        continue()
    endif()
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/work/${_name}")
    file(MAKE_DIRECTORY "${_dir}")
    arts_add_test(${_name} COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/expect_fault.sh" 139 no
        env ASAN_OPTIONS=handle_segv=0 $<TARGET_FILE:shutdown_watchdog> ${_mode})
    set_tests_properties(${_name} PROPERTIES
        LABELS "single_node" TIMEOUT ${_wd_timeout}
        RESOURCE_LOCK "arts_runtime"
        WORKING_DIRECTORY "${_dir}"
        ENVIRONMENT "ARTS_CONFIG=${_cfg};LC_ALL=C"
        PASS_REGULAR_EXPRESSION
        "${_head}\n.*Segmentation fault.*expect_fault: status 139"
        FAIL_REGULAR_EXPRESSION "FAIL")
    if(ARTS_DEBUG_SANITIZER STREQUAL "THREAD")
        set_tests_properties(${_name} PROPERTIES
            ENVIRONMENT_MODIFICATION
            "TSAN_OPTIONS=path_list_append:report_signal_unsafe=0")
    endif()
endforeach()
unset(_dir)
unset(_head)
# A stderr nobody drains, filled before the end begins: the watchdog skips
# its line and still ends the rank (71) inside the window; the line could not
# reach the test's output anyway, so the status is what shows the skip (a
# written line would leave the write blocked and the run alive past the
# bound).  The program's stderr is its own pipe, which would hide a
# sanitizer's report, so a sanitizer tree does not register it.
if(ARTS_DEBUG_SANITIZER STREQUAL "OFF")
    arts_add_test(shutdown_watchdog_stderr_full STATUS 71
                  COMMAND shutdown_watchdog stderr)
    set_tests_properties(shutdown_watchdog_stderr_full PROPERTIES
        LABELS "single_node" TIMEOUT ${_wd_timeout}
        RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg}"
        PASS_REGULAR_EXPRESSION
        "WATCHDOG_TEST: running ${_wd_alive} s after the take"
        FAIL_REGULAR_EXPRESSION "FAIL")
endif()
if(_records)
    arts_add_test(shutdown_watchdog_record_phase
        RECORD_CHECK record_lost.cmake n0.json
        COMMAND shutdown_watchdog record)
    set_tests_properties(shutdown_watchdog_record_phase PROPERTIES
        LABELS "single_node" TIMEOUT ${_wd_timeout}
        RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg}"
        PASS_REGULAR_EXPRESSION
        "${_wd_line} record. the counter record is lost\n"
        FAIL_REGULAR_EXPRESSION "FAIL")
endif()
unset(_wd_timeout)
unset(_wd_alive)
unset(_wd_line)

# The rank's end runs the leak checker: a block an EDT leaked is reported, and
# the process ends with ASan's exit status 1.  Only in the AddressSanitizer
# tree on the system allocator, the one whose leak checker sees the
# runtime's allocations; the driver keeps the report out of the test's
# output, whose fail list matches every sanitizer report.
if(ARTS_DEBUG_SANITIZER STREQUAL "ADDRESS" AND ARTS_MALLOC STREQUAL "system")
    add_arts_test(lsan_check_runs)
    arts_test_cfg(_cfg 1n_20w)
    arts_add_test(lsan_check_runs COMMAND ${CMAKE_COMMAND}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/expect_lsan_report.cmake
        -- $<TARGET_FILE:lsan_check_runs>)
    set_tests_properties(lsan_check_runs PROPERTIES
        LABELS "single_node" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg}"
        PASS_REGULAR_EXPRESSION "PASS lsan_check_runs")
endif()

# The pool's report at the rank's end, from every rank (ARTS_REGPOOL_REPORT):
# its totals line, or the one line saying it was skipped, legal where another
# thread held the pool's lock at the end.
arts_test_cfg(_cfg 2n_9w_1p)
set(_rp "\\[REGPOOL\\] (total:|report skipped: the pool lock is held)")
arts_add_multinode_test(regpool_report_at_exit_2n_9w_1p COMMAND term_shutdown)
arts_test_properties(regpool_report_at_exit_2n_9w_1p PROPERTIES
    LABELS "multinode" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
    ENVIRONMENT "ARTS_CONFIG=${_cfg};ARTS_REGPOOL_REPORT=1"
    PASS_REGULAR_EXPRESSION "${_rp}.*${_rp}")
unset(_rp)

# A thread the program starts, not the runtime, ends the run: where the
# teardown runs, it stops every runtime thread, runs whole and finds the
# shutdown quiescent.
if(ARTS_LOG_LEVEL GREATER_EQUAL 3)
    add_arts_test(shutdown_from_user_thread)
    register_single_node_test(shutdown_from_user_thread TIMEOUT 30)
    register_multinode_test(shutdown_from_user_thread TIMEOUT 60
                            VARIANTS 2n_9w_1p)
    arts_test_properties(shutdown_from_user_thread
        shutdown_from_user_thread_2n_9w_1p PROPERTIES
        PASS_REGULAR_EXPRESSION "USER_THREAD: a program thread ends the run"
        FAIL_REGULAR_EXPRESSION "FAIL;teardown skipped:;QUIESCENCE-DEBUG:")
endif()

# A counter folder that cannot be made: one WARN naming it per file the
# record would write, status 0, and no temporary file left under the test's
# own working directory.  The program alone sees that folder; the test's
# runner keeps the test's own.
if(_records)
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/work/record_folder_unwritable")
    file(MAKE_DIRECTORY "${_dir}")
    arts_test_cfg(_cfg 1n_20w)
    arts_add_test(record_folder_unwritable
        RECORD_CHECK record_no_tmp.cmake ${_dir}
        COMMAND env counter_folder=/dev/null/x $<TARGET_FILE:term_shutdown>)
    set_tests_properties(record_folder_unwritable PROPERTIES
        LABELS "single_node" TIMEOUT 10 RESOURCE_LOCK "arts_runtime"
        WORKING_DIRECTORY "${_dir}"
        ENVIRONMENT "ARTS_CONFIG=${_cfg}"
        PASS_REGULAR_EXPRESSION "counters: cannot create /dev/null/x"
        FAIL_REGULAR_EXPRESSION
        "cannot create /dev/null/x.*cannot create /dev/null/x")
    unset(_dir)
endif()

# Rank 0's marker names a failed end: rank 0's own abort prints
# "[SHUTDOWN] rc=5" and no span; rank 1's abort leaves rank 0's end, which it
# learns of, a success, so rank 0 prints its span.  The run's status is 5
# either way.
arts_test_cfg(_cfg 2n_9w_1p)
foreach(_case "rank0;0;\\[SHUTDOWN\\] rc=5;\\[E2E\\]"
              "rank1;1;\\[E2E\\] [0-9]+;\\[SHUTDOWN\\]")
    list(GET _case 0 _name)
    list(GET _case 1 _rank)
    list(GET _case 2 _pass)
    list(GET _case 3 _fail)
    arts_add_multinode_test(e2e_marker_on_failure_${_name} STATUS 5
                            COMMAND term_abort ${_rank} 5)
    arts_test_properties(e2e_marker_on_failure_${_name} PROPERTIES
        LABELS "multinode" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg};ARTS_E2E_MARKER=1"
        PASS_REGULAR_EXPRESSION "${_pass}"
        FAIL_REGULAR_EXPRESSION "${_fail}")
endforeach()
unset(_records)

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
# An argument vector packed into parameters reads back the same on the last
# rank, and every malformed block is refused without a read past it.
add_arts_test(argv_params)
register_single_node_test(argv_params TIMEOUT 30)
register_multinode_test(argv_params TIMEOUT 60 VARIANTS 2n_9w_1p)
arts_test_properties(argv_params argv_params_2n_9w_1p PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS argv_params"
    FAIL_REGULAR_EXPRESSION "FAIL")

add_arts_test(api_current_group_null)
register_single_node_test(api_current_group_null TIMEOUT 10)
set_tests_properties(api_current_group_null PROPERTIES PASS_REGULAR_EXPRESSION "PASS api_current_group_null")
endif()

# A block longer than ARTS_DB_SIZE_MAX ends the run with status 1 and names the
# size and the bound, before the create mints, sends or allocates anything; no
# cell changes the check.
if(ARTS_TEST_REFERENCE_TREE)
    foreach(_mode over huge)
        add_arts_test(db_size_bound_${_mode} SOURCE db_size_bound)
        register_single_node_test(db_size_bound_${_mode} TIMEOUT 30 STATUS 1
                                  ARGS ${_mode})
        set_tests_properties(db_size_bound_${_mode} PROPERTIES
            PASS_REGULAR_EXPRESSION
            "exceeds the largest block, ARTS_DB_SIZE_MAX = 4260607557632 bytes"
            FAIL_REGULAR_EXPRESSION "the create returned")
    endforeach()
endif()

# --- counters ---
# counter_smoke_value: the EDTs a program creates are counted exactly, main
# EDT included.  No cell changes how EDTs are counted.  The one that calls
# arts_shutdown (done_edt) never returns from its body, so it is created and
# not finished; main_edt's own FINISH increment comes after its body, which
# may make done_edt runnable, and nothing orders it before the record's live
# read, so the record holds 64 or 65 finished EDTs.
if(ARTS_TEST_REFERENCE_TREE)
    add_arts_test(counter_smoke_value)
    arts_counters_captured(_c NUM_EDT_CREATE NUM_EDT_FINISH)
    if(_c)
        register_single_node_test(counter_smoke_value TIMEOUT 30 RECORDS 1
            RECORD_CHECK record_values.cmake 0:NUM_EDT_CREATE=66
                                             0:NUM_EDT_FINISH=64..65)
    endif()
endif()

# The record of a run's end: every rank's counter file names the run, its
# rank's end and the time it began.  A run on one rank, on two and on four
# ends at rank 0's arts_shutdown; each file names one run and status 0, and
# rank 0's names the program as the origin.
if(ARTS_TEST_REFERENCE_TREE)
    arts_counters_captured(_c NUM_EDT_CREATE)
    if(_c)
        arts_test_cfg(_cfg 1n_20w)
        arts_add_test(record_written RECORDS 1
                      RECORD_CHECK record_meta.cmake origin@0=APP rc=0
                      COMMAND term_shutdown)
        set_tests_properties(record_written PROPERTIES
            LABELS "single_node" TIMEOUT 30 RESOURCE_LOCK "arts_runtime"
            ENVIRONMENT "ARTS_CONFIG=${_cfg}")
        foreach(_v 2n_9w_1p 4n_4w_1p)
            string(REGEX MATCH "^[0-9]+" _ranks ${_v})
            arts_test_cfg(_cfg ${_v})
            arts_add_multinode_test(record_written_${_v} RECORDS ${_ranks}
                RECORD_CHECK record_meta.cmake origin@0=APP rc=0
                COMMAND term_shutdown)
            arts_test_properties(record_written_${_v} PROPERTIES
                LABELS "multinode" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
                ENVIRONMENT "ARTS_CONFIG=${_cfg}")
        endforeach()
    endif()
endif()

# Rank files of two runs in one counter folder are refused, not reduced as one
# run: a run on two ranks and then one on one rank write into one folder that
# nothing clears between them, so rank 0's file names the second run and rank
# 1's the first; artsrun's reducer reads the folder as one run after the
# first and exits with status 2 and "mixed run ids" after the second.  The
# reducer imports the standard library alone and runs isolated (-I).
# Registered where some counter is captured at NODE level and Python 3.11 or
# later is found.
if(ARTS_TEST_REFERENCE_TREE)
    arts_counters_node_level(_n)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(_n AND Python3_Interpreter_FOUND AND
       NOT Python3_VERSION VERSION_LESS 3.11)
        set(_reducer "${CMAKE_SOURCE_DIR}/tools/artsrun/src/artsrun/reduce.py")
        set(_script
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/record_foreign_run_id.cmake")
        arts_test_cfg(_mn_cfg 2n_9w_1p)
        arts_test_cfg(_cfg 1n_20w)
        arts_add_multinode_test(record_foreign_run_id
            COMMAND ${CMAKE_COMMAND}
                    -DPROGRAM=$<TARGET_FILE:term_shutdown> -DRANKS=2
                    -DCFG_RANKS=${_mn_cfg} -DCFG_ONE=${_cfg}
                    -DPYTHON=${Python3_EXECUTABLE} -DREDUCER=${_reducer}
                    -P ${_script})
        arts_test_properties(record_foreign_run_id PROPERTIES
            LABELS "multinode" TIMEOUT 90 RESOURCE_LOCK "arts_runtime"
            PASS_REGULAR_EXPRESSION "PASS record_foreign_run_id")
    elseif(_n)
        message(STATUS "record_foreign_run_id is not registered: it needs "
                "Python 3.11 or later, and none was found")
    endif()
endif()

# The stop wakes the capture thread: with a capture interval of a day, a run
# that ends 200 ms after its EDT starts, when the capture thread is waiting for
# its first capture, still ends well within the test's 10 s, since the stop
# signals the thread's wait instead of leaving it to sleep out the interval.
if(ARTS_TEST_REFERENCE_TREE)
    arts_counters_periodic(_p)
    if(_p)
        arts_test_cfg(_cfg 1n_20w)
        arts_add_test(capture_stop_wakes COMMAND term_shutdown 0 200)
        set_tests_properties(capture_stop_wakes PROPERTIES
            LABELS "single_node" TIMEOUT 10 RESOURCE_LOCK "arts_runtime"
            ENVIRONMENT
            "ARTS_CONFIG=${_cfg};counter_capture_interval=86400000")
    endif()
endif()

# A stop that comes first against the capture start, registered in the
# reference tree where the injection hooks and a periodic counter are on: the
# hook, which sits between the start's handle store and its transition of the
# capture word, sends a SIGTERM and holds until the stop it causes has marked
# the word stopped.  The test passes on the starter's join line, a skipped
# teardown fails it, and the run ends with the signal's status.  Only the
# teardown's stop can show the starter parked once the start returned, so it
# registers only where the teardown runs.
if(ARTS_TEST_REFERENCE_TREE AND ARTS_DEBUG_TEARDOWN AND ARTS_DEBUG_INJECTION)
    arts_counters_periodic(_p)
    if(_p)
        arts_test_cfg(_cfg 1n_20w)
        add_arts_test(capture_start_stop_race)
        arts_add_test(capture_start_stop_race STATUS 143
                      COMMAND capture_start_stop_race)
        set_tests_properties(capture_start_stop_race PROPERTIES
            LABELS "single_node" TIMEOUT 30 RESOURCE_LOCK "arts_runtime"
            ENVIRONMENT
            "ARTS_CONFIG=${_cfg};counter_capture_interval=86400000"
            PASS_REGULAR_EXPRESSION "joined by its starter"
            FAIL_REGULAR_EXPRESSION
            "teardown skipped:;Failed to join capture thread")
    endif()
endif()

# How soon every rank learns of the end: rank 2 has contacted every other rank
# when it ends the run, while each worker of rank 1 floods rank 0 with
# satisfies, and the four ranks' shutdown stamps lie within one second.
if(ARTS_TEST_REFERENCE_TREE)
    add_arts_test(shutdown_learn_latency)
    arts_counters_captured(_c NUM_EDT_CREATE)
    if(_c)
        register_multinode_test(shutdown_learn_latency TIMEOUT 60
            VARIANTS 4n_4w_1p RECORDS 4
            RECORD_CHECK record_meta.cmake t0_spread_ms<=1000)
    endif()
endif()

# How far apart the ranks end when nothing follows the record, in the same
# shape: the driver polls each rank's pid, and the last rank's end lies within
# 2 s of the first's.  Only where the teardown does not run, since its span is
# its own.
if(ARTS_TEST_REFERENCE_TREE AND NOT ARTS_DEBUG_TEARDOWN)
    arts_test_cfg(_cfg 4n_4w_1p)
    arts_add_multinode_test(shutdown_end_spread
        COMMAND ${ARTS_SPAWNER_DRIVER} --want 0 --ranks 4 --end-spread-ms 2000
                -- $<TARGET_FILE:shutdown_learn_latency>)
    arts_test_properties(shutdown_end_spread PROPERTIES
        LABELS "multinode" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg}"
        PASS_REGULAR_EXPRESSION "end spread [0-9]+ ms within 2000 ms"
        FAIL_REGULAR_EXPRESSION "FAIL")
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
    # GUID), 2 of size zero with the hold; one read and one write each.  Of
    # the 22 EDTs, the one that calls arts_shutdown (terminal) never returns
    # from its body, so 21 finish.  main_edt's last act is the reserved
    # block's create, whose install makes that block's readers runnable; a
    # create counts before its install, so the record holds all 10, but
    # main_edt's FINISH increment comes after its body and nothing orders it
    # before the record's live read, so 20 or 21 EDTs finished.
    set(_one 0:NUM_DB_ACQUIRE_READ=10 0:NUM_DB_ACQUIRE_WRITE=16
             0:NUM_DB_ACQUIRE_LOCAL_HIT=26 0:NUM_DB_ACQUIRE_REMOTE=0)
    if(ARTS_TEST_REFERENCE_TREE)
        list(APPEND _one 0:NUM_DB_CREATE=10 0:NUM_DB_CREATE_NO_ACQUIRE=4
                         0:BYTES_DB_CREATE=2048 0:NUM_DB_DESTROY=10
                         0:NUM_EDT_CREATE=22 0:NUM_EDT_FINISH=20..21
                         0:NUM_REMOTE_SEND=0 0:NUM_REMOTE_RECEIVE=0
                         0:BYTES_REMOTE_SENT=0 0:BYTES_REMOTE_RECEIVED=0
                         0:NUM_DB_PAYLOAD_SENT=0 0:BYTES_DB_PAYLOAD_SENT=0)
    endif()
    arts_counters_captured(_c NUM_PURGE_RETURN)
    if(_c AND ARTS_RETENTION STREQUAL "PURGE"
       AND NOT ARTS_VALIDITY STREQUAL "FLUSH")
        list(APPEND _one 0:NUM_PURGE_RETURN=0)
    endif()
    arts_add_test(counter_exact_one RECORDS 1
                  RECORD_CHECK record_values.cmake ${_one}
                  COMMAND counter_exact one)
    arts_test_cfg(_cfg 1n_20w)
    set_tests_properties(counter_exact_one PROPERTIES
        LABELS "single_node" TIMEOUT 30 RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg}"
        FAIL_REGULAR_EXPRESSION "FAIL:")

    # The two-rank program, per cell: rank 1 reads 3 times, writes, reads;
    # then rank 0, the home, reads.  Rank 0 also holds two creates.
    set(_two 0:NUM_DB_ACQUIRE_READ=1 0:NUM_DB_ACQUIRE_WRITE=2
             1:NUM_DB_ACQUIRE_READ=4 1:NUM_DB_ACQUIRE_WRITE=1)
    set(_cell "${ARTS_VALIDITY}_${ARTS_RETENTION}_${ARTS_READ_SOURCE}")
    if(ARTS_VALIDITY STREQUAL "FLUSH")
        set(_cell "FLUSH")
    endif()
    # rank 0 local, rank 0 remote, rank 1 local, rank 1 remote
    set(_census_VAL_RETAIN_HOME 3 0 1 4)
    set(_census_VAL_PURGE_HOME 3 0 0 5)
    set(_census_VAL_RETAIN_OWNER 2 1 1 4)
    set(_census_INV_RETAIN_HOME 3 0 3 2)
    set(_census_INV_PURGE_HOME 3 0 3 2)
    set(_census_INV_RETAIN_OWNER 2 1 3 2)
    set(_census_EXCL_PURGE_HOME 3 0 0 5)
    set(_census_EXCL_RETAIN_OWNER 2 1 3 2)
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
    if(_c AND _cell STREQUAL "VAL_RETAIN_HOME")
        list(APPEND _two 1:NUM_SNAPSHOT_REQUEST=3 1:NUM_RO_COMBINE_WINDOW=3
                         1:NUM_RO_COMBINE_JOINED=0 0:NUM_SNAPSHOT_REQUEST=0
                         0:NUM_SNAPSHOT_HEADER_ONLY=2)
    elseif(_c AND _cell STREQUAL "VAL_PURGE_HOME")
        list(APPEND _two 1:NUM_SNAPSHOT_REQUEST=4 1:NUM_RO_COMBINE_WINDOW=4
                         1:NUM_RO_COMBINE_JOINED=0 0:NUM_SNAPSHOT_REQUEST=0
                         0:NUM_SNAPSHOT_HEADER_ONLY=3)
    elseif(_c AND _cell STREQUAL "VAL_RETAIN_OWNER")
        list(APPEND _two 1:NUM_SNAPSHOT_REQUEST=3 1:NUM_RO_COMBINE_WINDOW=3
                         1:NUM_RO_COMBINE_JOINED=0 0:NUM_SNAPSHOT_REQUEST=1
                         0:NUM_RO_COMBINE_WINDOW=1 0:NUM_SNAPSHOT_HEADER_ONLY=2
                         1:NUM_SNAPSHOT_HEADER_ONLY=0)
    endif()
    arts_counters_captured(_c NUM_EXCL_RECALL_SENT)
    if(_c AND _cell STREQUAL "EXCL_RETAIN_OWNER")
        list(APPEND _two 0:NUM_EXCL_RECALL_SENT=1 1:NUM_EXCL_RECALL_SENT=0)
    endif()
    # Hand-backs: rank 0's hold of the block homed on rank 1; on rank 1 the
    # write turn, and under exclusion every read turn too.
    arts_counters_captured(_purge NUM_PURGE_RETURN)
    if(_purge AND _cell STREQUAL "EXCL_PURGE_HOME")
        list(APPEND _two 0:NUM_PURGE_RETURN=1 1:NUM_PURGE_RETURN=5)
    elseif(_purge AND _cell MATCHES "_PURGE_")
        list(APPEND _two 0:NUM_PURGE_RETURN=1 1:NUM_PURGE_RETURN=1)
    endif()
    if(ARTS_TEST_REFERENCE_TREE)
        list(APPEND _two 0:NUM_DB_CREATE=3 0:NUM_DB_CREATE_NO_ACQUIRE=1
                         0:BYTES_DB_CREATE=768 0:NUM_DB_DESTROY=3
                         1:NUM_DB_CREATE=0 1:NUM_DB_DESTROY=0)
    endif()

    # The home program, per cell: rank 1 writes, then rank 0, the home, reads
    # and writes.  Rank 0's read is local where the home holds the bytes, and
    # its write where the write right belongs to the home (under PURGE); each
    # is remote where rank 1 has to send it back.
    set(_home 0:NUM_DB_ACQUIRE_READ=1 0:NUM_DB_ACQUIRE_WRITE=2
              1:NUM_DB_ACQUIRE_READ=0 1:NUM_DB_ACQUIRE_WRITE=1
              1:NUM_DB_ACQUIRE_LOCAL_HIT=0 1:NUM_DB_ACQUIRE_REMOTE=1)
    # rank 0 local, rank 0 remote
    set(_home_VAL_RETAIN_HOME 2 1)
    set(_home_VAL_PURGE_HOME 3 0)
    set(_home_VAL_RETAIN_OWNER 1 2)
    set(_home_INV_RETAIN_HOME 2 1)
    set(_home_INV_PURGE_HOME 3 0)
    set(_home_INV_RETAIN_OWNER 1 2)
    set(_home_EXCL_PURGE_HOME 3 0)
    set(_home_EXCL_RETAIN_OWNER 1 2)
    set(_home_FLUSH 3 0)
    list(GET _home_${_cell} 0 _l0)
    list(GET _home_${_cell} 1 _r0)
    list(APPEND _home 0:NUM_DB_ACQUIRE_LOCAL_HIT=${_l0}
                      0:NUM_DB_ACQUIRE_REMOTE=${_r0})
    if(_purge AND _cell MATCHES "_PURGE_")
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
    if(_c AND _cell STREQUAL "EXCL_RETAIN_OWNER")
        list(APPEND _recall 0:NUM_EXCL_RECALL_SENT=1 1:NUM_EXCL_RECALL_SENT=0)
    endif()
    if(_purge AND _cell STREQUAL "EXCL_PURGE_HOME")
        list(APPEND _recall 0:NUM_PURGE_RETURN=0 1:NUM_PURGE_RETURN=1)
    elseif(_purge AND _cell MATCHES "_PURGE_")
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
    if(_purge AND _cell MATCHES "_PURGE_")
        list(APPEND _claim 0:NUM_PURGE_RETURN=0 1:NUM_PURGE_RETURN=1)
    endif()

    # The pub program: rank 0's reader acquires at the home before rank 1
    # creates the block, so it waits for rank 1's create and first
    # publication, whichever drain then wakes it.  It is remote wherever the
    # copy comes from rank 1, and local where the home holds the write right
    # (under PURGE) or every home acquisition is local.
    set(_pub 0:NUM_DB_ACQUIRE_READ=1 0:NUM_DB_ACQUIRE_WRITE=0
             1:NUM_DB_ACQUIRE_READ=0 1:NUM_DB_ACQUIRE_WRITE=1
             1:NUM_DB_ACQUIRE_LOCAL_HIT=1 1:NUM_DB_ACQUIRE_REMOTE=0)
    if(_cell MATCHES "_PURGE_" OR _cell STREQUAL "FLUSH")
        list(APPEND _pub 0:NUM_DB_ACQUIRE_LOCAL_HIT=1
                         0:NUM_DB_ACQUIRE_REMOTE=0)
    else()
        list(APPEND _pub 0:NUM_DB_ACQUIRE_LOCAL_HIT=0
                         0:NUM_DB_ACQUIRE_REMOTE=1)
    endif()
    if(_purge AND _cell MATCHES "_PURGE_")
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
    if(_purge AND _cell STREQUAL "EXCL_PURGE_HOME")
        list(APPEND _join 0:NUM_PURGE_RETURN=0 1:NUM_PURGE_RETURN=1)
    elseif(_purge AND _cell MATCHES "_PURGE_")
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

    # join and gate hold every read request.  join's leaves once the EDT
    # bodies runnable at its hold have returned: the one that sent it, after
    # the next reader parked on it, and on VAL the first reader's, after the
    # third parked on the next window; so each later reader parks on the
    # request out whatever the timing.  gate's are held past the run, so the
    # write turn serves them.
    set(_shapes two home recall claim pub)
    if(ARTS_DEBUG_INJECTION)
        list(APPEND _shapes join)
        if(_cell MATCHES "_RETAIN_")
            list(APPEND _shapes gate)
        endif()
    endif()
    set(_hook_join "ARTS_REQUEST_DELAY_MS=60000:drain")
    set(_hook_gate "ARTS_REQUEST_DELAY_MS=60000:ro")
    foreach(_v 2n_9w_1p 2n_8w_2p)
        arts_test_cfg(_cfg ${_v})
        foreach(_shape IN LISTS _shapes)
            set(_env "ARTS_CONFIG=${_cfg}")
            if(DEFINED _hook_${_shape})
                list(APPEND _env "${_hook_${_shape}}")
            endif()
            arts_add_multinode_test(counter_exact_${_shape}_${_v} RECORDS 2
                RECORD_CHECK record_values.cmake ${_${_shape}}
                COMMAND counter_exact ${_shape})
            arts_test_properties(counter_exact_${_shape}_${_v} PROPERTIES
                LABELS "multinode" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
                ENVIRONMENT "${_env}"
                FAIL_REGULAR_EXPRESSION "FAIL:")
        endforeach()
    endforeach()
endif()
# A two-rank exchange: rank 0 creates a 64 KiB block with the hold and an EDT
# on rank 1 that reads it and shuts the run down.  Rank 1 dispatches every
# message rank 0 sends before its end, and takes in the payload whole however
# many pieces it travels in (the split variant cuts it into sixteen); whether
# rank 0 takes in the shutdown message is not fixed (a receiver also stops
# when it sees the initiator's connection close), so its receives are left
# out.  The read is served by one one-sided payload from the home under this
# tree's cell.  Rank 1 sends its read request and the SHUTDOWN post and, where
# the tree captures a PERIODIC counter, its time-sync request, which leaves
# before any EDT of rank 1 can run and which rank 0 answers: 3 sends there,
# else 2.  Nothing rank 1 does waits for that answer, so it may land after
# rank 1's record reads its counters: there rank 1 receives every message of
# rank 0 or every one but the answer (one message of 32 bytes).  The post is
# best effort and counts only when the transport accepts it; the connection
# is up here, since rank 1 has just sent its read request, so a refused post
# would read as one send less.
arts_counters_captured(_c NUM_REMOTE_SEND NUM_REMOTE_RECEIVE BYTES_REMOTE_SENT
                       BYTES_REMOTE_RECEIVED NUM_DB_PAYLOAD_SENT
                       BYTES_DB_PAYLOAD_SENT)
if(_c AND ARTS_TEST_REFERENCE_TREE)
    arts_counters_periodic(_p)
    if(_p)
        set(_ping 1:NUM_REMOTE_RECEIVE=0:NUM_REMOTE_SEND-0..1
                  1:BYTES_REMOTE_RECEIVED=0:BYTES_REMOTE_SENT-0..32
                  1:NUM_REMOTE_SEND=3)
    else()
        set(_ping 1:NUM_REMOTE_RECEIVE=0:NUM_REMOTE_SEND
                  1:BYTES_REMOTE_RECEIVED=0:BYTES_REMOTE_SENT
                  1:NUM_REMOTE_SEND=2)
    endif()
    list(APPEND _ping 0:NUM_DB_PAYLOAD_SENT=1 0:BYTES_DB_PAYLOAD_SENT=65536
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
        arts_add_multinode_test(counter_exact_ping_${_v} RECORDS 2
            RECORD_CHECK record_values.cmake ${_ping}
            COMMAND counter_exact ping)
        arts_test_properties(counter_exact_ping_${_v} PROPERTIES
            LABELS "multinode" TIMEOUT 60 RESOURCE_LOCK "arts_runtime"
            ENVIRONMENT "${_env}"
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
    arts_test_properties(${_t} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS csr_graph")
endforeach()

# Many EDTs race into arts_shutdown: one wins the shutdown gate and the run
# ends with status 0; where the tree writes counter files, the rank's file
# names the program as the origin and status 0.  Its initiators acquire one
# block RW with no order among them, which CDAG does not admit.
if(NOT ARTS_VALIDITY STREQUAL "FLUSH")
add_arts_test(runtime_concurrent_shutdown)
arts_counters_captured(_c NUM_EDT_CREATE)
if(_c)
    register_single_node_test(runtime_concurrent_shutdown TIMEOUT 60 RECORDS 1
        RECORD_CHECK record_meta.cmake origin=APP rc=0)
else()
    register_single_node_test(runtime_concurrent_shutdown TIMEOUT 60)
endif()
set_tests_properties(runtime_concurrent_shutdown PROPERTIES
    FAIL_REGULAR_EXPRESSION "shutdown watchdog:")
endif()

# runtime_gpu_scheduler_promotion: GPU-only (ARTS_USE_GPU); SKIP stub otherwise.
if(BUILD_CUDA_LIBRARY)
    add_arts_test(runtime_gpu_scheduler_promotion)
    target_compile_definitions(runtime_gpu_scheduler_promotion PRIVATE ARTS_USE_GPU)
    register_gpu_test(runtime_gpu_scheduler_promotion TIMEOUT 120)
    set_tests_properties(runtime_gpu_scheduler_promotion PROPERTIES
        PASS_REGULAR_EXPRESSION "GPU_SCHED_PROMOTION_DONE")
endif()

# Its fires record into one block with no order among them, which CDAG does
# not admit.
if(NOT ARTS_VALIDITY STREQUAL "FLUSH")
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
# Its writers are unordered, which CDAG does not admit.
if(NOT ARTS_VALIDITY STREQUAL "FLUSH")
add_arts_test(edt_create_single_fire)
register_single_node_test(edt_create_single_fire TIMEOUT 30)
set_tests_properties(edt_create_single_fire PROPERTIES PASS_REGULAR_EXPRESSION "PASS edt_create_single_fire")
endif()

# edt_completion_event: the consumer sits on rank 1 when there is one, and the
# release that precedes the completion event sends nothing a second peer would
# see.  Its writers are unordered, which CDAG does not admit.
if(NOT ARTS_VALIDITY STREQUAL "FLUSH")
add_arts_test(edt_completion_event)
register_single_node_test(edt_completion_event TIMEOUT 30)
register_multinode_test(edt_completion_event TIMEOUT 60 VARIANTS 2n_9w_1p)
foreach(_v "" _2n_9w_1p)
    arts_test_properties(edt_completion_event${_v} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
endif()

# edt_group_balance: round-robins members across all ranks; one peer
# already puts members off the group's rank.
add_arts_test(edt_group_balance)
register_single_node_test(edt_group_balance TIMEOUT 30)
register_multinode_test(edt_group_balance TIMEOUT 60 VARIANTS 2n_9w_1p)
foreach(_v "" _2n_9w_1p)
    arts_test_properties(edt_group_balance${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS edt_group_balance")
endforeach()

# edt_remote_create_reordered_satisfy: needs 2+ ranks, so it is multinode
# only; the create and its satisfies go to one rank, and the two progress
# threads let the satisfies overtake the create.
add_arts_test(edt_remote_create_reordered_satisfy)
register_multinode_test(edt_remote_create_reordered_satisfy TIMEOUT 60
                        VARIANTS 2n_9w_1p 2n_8w_2p)
foreach(_v 2n_9w_1p 2n_8w_2p)
    arts_test_properties(edt_remote_create_reordered_satisfy_${_v} PROPERTIES
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
if(NOT ARTS_VALIDITY STREQUAL "FLUSH")
add_pure_unit_src(directory_grantreq_queue
    SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/grantreq_queue.c
    DEFINES ARTS_UNIT_STANDALONE_SHIMS PASS_REGEX "PASS directory_grantreq_queue:" TIMEOUT 60)
endif()

# pending_rw_treiber: the grant's cache-side waiter queue, one body for the
# VAL and INV arms.
if(ARTS_TEST_REFERENCE_TREE)
add_pure_unit_src(pending_rw_treiber DEFINES ARTS_UNIT_STANDALONE_SHIMS PASS_REGEX "PASS pending_rw_treiber:" TIMEOUT 60)
endif()

# excl_compute_next: EXCL only, each retention's arbiters.
if(ARTS_VALIDITY STREQUAL "EXCL")
add_pure_unit_src(excl_compute_next DEFINES ARTS_UNIT_STANDALONE_SHIMS PASS_REGEX "PASS excl_compute_next:" TIMEOUT 60)
endif()

# val_request_word: VAL only (the request word's arbiter, both read sources
# and retentions alike).
if(ARTS_VALIDITY STREQUAL "VAL")
add_pure_unit_src(val_request_word DEFINES ARTS_UNIT_STANDALONE_SHIMS PASS_REGEX "PASS val_request_word:" TIMEOUT 60)
endif()

# val_release_buffer_check: a VAL write release bumps its write turn's own
# buffer, and a Debug build ends a release that names another one in the
# named fatal error.  Each read source has its own release and its own check
# (the PURGE release shares the HOME one).
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND ARTS_VALIDITY STREQUAL "VAL" AND
   ARTS_RETENTION STREQUAL "RETAIN")
add_arts_test(val_release_buffer_check)
register_single_node_test(val_release_buffer_check TIMEOUT 30 STATUS 1)
set_tests_properties(val_release_buffer_check PROPERTIES
    PASS_REGULAR_EXPRESSION "write turn's buffer is not the installed one")
endif()

# inv_compute_next: INV only.  One truth table covers both read sources —
# the arbiters do not depend on the read source.
if(ARTS_VALIDITY STREQUAL "INV")
add_pure_unit_src(inv_compute_next DEFINES ARTS_UNIT_STANDALONE_SHIMS PASS_REGEX "PASS inv_compute_next:" TIMEOUT 60)
endif()

# db_reader_pool: the parked-reader pool on a cache of the cell, with no
# runtime started (links libarts for the cell's arbiter and destructor).  The
# arms that chain readers by index only; each cell's destructor is its own.
if(ARTS_VALIDITY STREQUAL "VAL" OR ARTS_VALIDITY STREQUAL "INV")
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

# route_budget: the route table's probes and pins per unit of a program shape,
# judged by the difference of two runs (N and 2N units) so what a run does
# once cancels.  arts_route_budget_test(<shape> <n> <probes> <pins>
# [RANK <r>] [CFG <cfg>] [REFERENCE_ONLY]) registers one shape: <probes> and
# <pins> are the judged rank's NUM_ROUTE_PROBE and NUM_ROUTE_PIN per unit.
# REFERENCE_ONLY marks a shape no cell can change; it registers in the
# reference tree only.  Both counters must be captured.  A shape whose cfg
# runs more than one rank is a multinode test, given a multinode run's budget
# for each of its two runs.
add_arts_test(route_budget)
function(arts_route_budget_test shape n probes pins)
    cmake_parse_arguments(ARG "REFERENCE_ONLY" "RANK;CFG" "" ${ARGN})
    if(ARG_REFERENCE_ONLY AND NOT ARTS_TEST_REFERENCE_TREE)
        return()
    endif()
    arts_counters_captured(_c NUM_ROUTE_PIN NUM_ROUTE_PROBE)
    if(NOT _c)
        return()
    endif()
    if(NOT DEFINED ARG_RANK)
        set(ARG_RANK 0)
    endif()
    if(NOT ARG_CFG)
        set(ARG_CFG 1n_20w)
    endif()
    arts_test_cfg(_cfg ${ARG_CFG})
    set(_label single_node)
    set(_timeout 60)
    if(ARG_CFG MATCHES "^([0-9]+)n_" AND CMAKE_MATCH_1 GREATER 1)
        set(_label multinode)
        set(_timeout 120)
    endif()
    set(_add arts_add_test)
    if(_label STREQUAL "multinode")
        set(_add arts_add_multinode_test)
    endif()
    cmake_language(CALL ${_add} route_budget_${shape}
        COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:route_budget> -DSHAPE=${shape}
                -DN=${n} -DRANK=${ARG_RANK}
                -DPROBES_PER_UNIT=${probes} -DPINS_PER_UNIT=${pins}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/route_budget_diff.cmake)
    arts_test_properties(route_budget_${shape} PROPERTIES
        LABELS ${_label} TIMEOUT ${_timeout} RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg}"
        PASS_REGULAR_EXPRESSION "PASS: route_budget_diff ${shape}"
        FAIL_REGULAR_EXPRESSION "FAIL:")
endfunction()

arts_route_budget_test(none 8 0 0 REFERENCE_ONLY)
# create: each of the unit's three creates mints its GUID, reserves its slot
# on the fresh path (one probe) and installs into the slot the engine found
# there (none); nothing is parked, so the install's drain pins nothing, and
# the EDT's completion retires it through its control block's home (none).
# No cell changes a create that acquires nothing.
arts_route_budget_test(create 64 3 0 REFERENCE_ONLY)
# rw: the unit's create reserves its minted GUID's slot on the fresh path (one
# probe) and installs into it (none); the destroy releases the creator's hold
# and runs on the descriptor that hold names (none), whose teardown tells no
# other rank and retires through the control block's home (none).  What the
# release does in between is the cell's, and resolves nothing on any cell:
# VAL's home publishes nothing (its buffer is the canonical copy under HOME, the
# bytes stay with the owner under OWNER, and under PURGE the home keeps the
# right it holds); INV's release sends its PUBLISH to this rank, which runs on
# the releaser's own descriptor, and the round, with no copy to retire, acks it
# on the same descriptor; EXCL·PURGE commits the home's release in place (the
# CXL residencies after the purge into the block's store); EXCL·RETAIN's
# creator stays the sticky owner and sends nothing; FLUSH's home writes nothing
# back.  1 probe, 0 pins, every cell.
arts_route_budget_test(rw 64 1 0)
# firsttouch and rwremote, judged on rank 1, per unit: the EDT create's
# reservation of its GUID (1 probe), the satisfy's dispatch onto the
# installed EDT (1 probe, 1 pin) and the acquire's one reservation of the
# block, which installs the first-touch cache into the slot it found (1
# probe).  Everything else rank 1 receives is an answer to its own request,
# which acts on the descriptor its token names, and nothing rank 1 sends
# resolves anything on rank 1:
#   VAL, read: SNAPSHOT_RESPONSE (from the home, or under OWNER from the owner a
#     redirect names, which is the home here), its payload paired
#     one-sided.
#   VAL, write: GRANT_RESPONSE (the holder's ship under RETAIN, the home's
#     serve under PURGE) and, under OWNER, CONFIRM_ACK; the release's publish
#     under HOME: PUBLISH_CTS (paired by its rendezvous pointer) and
#     PUBLISH_ACK, which under PURGE also acknowledges the hand-back riding
#     the commit.  VAL·OWNER's release sends nothing.
#   INV, read: INV_DELIVER; write: GRANT_RESPONSE, CONFIRM_ACK under OWNER, and
#     the release's round, acknowledged by PUBLISH_ACK (after PUBLISH_CTS
#     under HOME).  No round targets rank 1: it is the round's releaser, and no
#     other rank writes the block.
#   EXCL·PURGE, both modes: EXCL_GRANT from the home (one-sided bytes in
#     rank memory; on the CXL stores a grant naming the block's slot, which
#     the grantee reads itself).  The release is an EXCL_RELEASE to the home,
#     which nothing answers.
#   EXCL·RETAIN, read: EXCL_DELIVER from the owner the home forwarded the
#     request to (the creator, rank 0), whose read grant rank 1 retains; its
#     release sends nothing.  Write: the migration's EXCL_DELIVER, after which
#     rank 1 CONFIRMs to the home and keeps the write grant; its release sends
#     nothing.
#   FLUSH, both modes: FETCH_RESPONSE, which reaches the block through the
#     parked EDT its cookie names; under rwremote the release's write-back,
#     with the credit that response carried, is answered by FLUSH_ACK, which
#     only wakes the releaser.
# 3 probes, 1 pin in every cell.  Two ranks move one-sided payloads, which an
# ARTS_MALLOC=system tree refuses.
if(ARTS_MALLOC STREQUAL "mimalloc")
    arts_route_budget_test(firsttouch 64 3 1 RANK 1 CFG 2n_9w_1p)
    arts_route_budget_test(rwremote 64 3 1 RANK 1 CFG 2n_9w_1p)
endif()
# group: each of the unit's two EDTs is created inside the group its
# creator holds a counted reference to: the create reserves its minted GUID's
# slot on the fresh path (one probe), the INCR runs on the creator's
# reference and the new EDT keeps a copy of it (none), and the completion
# DECRs through that copy and retires through its control block's home
# (none).  Joining the parent to the group resolves it once per run.  No cell
# changes an EDT that acquires nothing.
arts_route_budget_test(group 64 2 0 REFERENCE_ONLY)
