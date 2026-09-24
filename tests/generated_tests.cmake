# ============================================================================
# Auto-generated Wave-A pure_unit test registrations.
#
# Generated from research/test-census/waveA-result.json. Do NOT edit by hand;
# regenerate from the census instead.
#
# Protocol selection (ARTS_PROTOCOL_* / ARTS_WRITE_POLICY_* / ARTS_RELEASE_*)
# is supplied GLOBALLY by the build directory's coherence configuration --
# it is deliberately NOT hardcoded here, so each generated test compiles under
# whatever protocol the enclosing build dir selected.
#
# Tests that EXPOSE a runtime bug and are designed to fail/crash are registered
# WITHOUT a PASS_REGULAR_EXPRESSION so they show up as failing (documenting the
# defect). They are NOT masked with WILL_FAIL.
#
# The two if() guards below are hand-maintained: they key on the exclusion list
# tests/CMakeLists.txt sets, and a regeneration from the census must preserve
# them.  No generated registration LINE is edited by them.
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
    target_compile_options(${name} PRIVATE -fno-pie -fno-PIE)
    target_link_options(${name} PRIVATE -no-pie -fno-pie -fno-PIE)
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

add_pure_unit_src(lf_lifo_pop_one PASS_REGEX "PASS lf_lifo_pop_one" TIMEOUT 60)
add_pure_unit_src(lf_lifo_drain PASS_REGEX "PASS lf_lifo_drain" TIMEOUT 60)
add_pure_unit_src(lf_lifo_layout PASS_REGEX "PASS lf_lifo_layout" TIMEOUT 60)
add_pure_unit_src(lf_pool_dwcas PASS_REGEX "PASS lf_pool_dwcas" TIMEOUT 60)
add_pure_unit_src(lf_pool_batch PASS_REGEX "PASS lf_pool_batch" TIMEOUT 60)
add_pure_unit_src(mpsc_drain_remaining PASS_REGEX "PASS mpsc_drain_remaining" TIMEOUT 60)
add_pure_unit_src(mpsc_rethread_stub PASS_REGEX "PASS mpsc_rethread_stub" TIMEOUT 60)
add_pure_unit_src(mpsc_transient_empty PASS_REGEX "PASS mpsc_transient_empty" TIMEOUT 60)
add_pure_unit_src(link_list_mpsc SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/utils/link_list.c PASS_REGEX "PASS link_list_mpsc" TIMEOUT 60)
# T016 EXPOSES B118: expected to FAIL/crash under sanitizer (documents runtime bug, do not mask)
add_pure_unit_src(link_list_lifecycle SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/utils/link_list.c TIMEOUT 60)
add_pure_unit_src(shared_compare_exchange SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c PASS_REGEX "PASS shared_compare_exchange" TIMEOUT 60)
add_pure_unit_src(shared_lifecycle_race SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c PASS_REGEX "PASS shared_lifecycle_race" TIMEOUT 60)
add_pure_unit_src(route_table_install_if_absent PASS_REGEX "PASS route_table_install_if_absent" TIMEOUT 60)
add_pure_unit_src(route_table_segment_grow PASS_REGEX "PASS route_table_segment_grow" TIMEOUT 60)
add_pure_unit_src(route_item_mirror_bridge PASS_REGEX "PASS route_item_mirror_bridge" TIMEOUT 60)
add_pure_unit_src(guid_encoding_roundtrip PASS_REGEX "PASS guid_encoding_roundtrip" TIMEOUT 60)
add_pure_unit_src(guid_from_index_overflow PASS_REGEX "PASS guid_from_index_overflow" TIMEOUT 60)
add_pure_unit_src(guid_reserve_range PASS_REGEX "PASS guid_reserve_range" TIMEOUT 60)
add_pure_unit_src(guid_db_seq_alloc_stress PASS_REGEX "PASS guid_db_seq_alloc_stress" TIMEOUT 120)
add_pure_unit_src(route_table_db_shard PASS_REGEX "PASS route_table_db_shard" TIMEOUT 60)
add_pure_unit_src(db_cache_layout PASS_REGEX "PASS db_cache_layout" TIMEOUT 60)
if(NOT "buffer_payload_roundtrip" IN_LIST ARTS_FAM_DIRECT_EXCLUDED)
add_pure_unit_src(buffer_payload_roundtrip SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/buffer.c ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c PASS_REGEX "PASS buffer_payload_roundtrip" TIMEOUT 60)
add_pure_unit_src(buffer_zero_size SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/buffer.c ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c PASS_REGEX "PASS buffer_zero_size" TIMEOUT 60)
add_pure_unit_src(buffer_version_guard SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/buffer.c ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c PASS_REGEX "PASS buffer_version_guard" TIMEOUT 60)
add_pure_unit_src(buffer_stub_db_size_learn SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/buffer.c ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c PASS_REGEX "PASS buffer_stub_db_size_learn" TIMEOUT 60)
add_pure_unit_src(buffer_destroy_vs_acquire SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/buffer.c ${CMAKE_SOURCE_DIR}/libs/src/core/utils/shared.c PASS_REGEX "PASS buffer_destroy_vs_acquire" TIMEOUT 60)
endif()
add_pure_unit_src(rank_u64_map_roundtrip SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/rank_u64_map.c PASS_REGEX "PASS rank_u64_map_roundtrip" TIMEOUT 60)
add_pure_unit_src(rank_u64_map_advance SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/rank_u64_map.c PASS_REGEX "PASS rank_u64_map_advance" TIMEOUT 60)
# T056 rank_bitset: the arts_rank_bitset_* symbols live in the protocol-specific
# coherence directory.c, so the source/link selection is keyed on the build dir's
# ${ARTS_COHERENCE_ARM}:
#   VAL -> compile val/directory.c standalone (ARTS_UNIT_STANDALONE_SHIMS shims)
#   EXCL  -> excl/directory.c pulls transport/edt deps, so link the full libarts
#            (shims auto-compiled-out via the ARTS_UNIT_STANDALONE_SHIMS gate)
if(ARTS_COHERENCE_ARM STREQUAL "EXCL")
    add_arts_test(rank_bitset)
    register_pure_unit_test(rank_bitset TIMEOUT 60)
    set_tests_properties(rank_bitset PROPERTIES PASS_REGULAR_EXPRESSION "PASS rank_bitset")
elseif(ARTS_COHERENCE_ARM STREQUAL "INV")
    set(_rank_bitset_dir ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/inv/directory.c)
    add_pure_unit_src(rank_bitset SOURCES ${_rank_bitset_dir}
        DEFINES ARTS_UNIT_STANDALONE_SHIMS=1 PASS_REGEX "PASS rank_bitset" TIMEOUT 60)
elseif(ARTS_COHERENCE_ARM STREQUAL "FLUSH")
    # No directory.c at all in this arm: the test source self-skips and there
    # is nothing to compile alongside it.
    add_pure_unit_src(rank_bitset
        DEFINES ARTS_UNIT_STANDALONE_SHIMS=1 PASS_REGEX "PASS rank_bitset" TIMEOUT 60)
else()
    set(_rank_bitset_dir ${CMAKE_SOURCE_DIR}/libs/src/core/coherence/val/directory.c)
    add_pure_unit_src(rank_bitset SOURCES ${_rank_bitset_dir}
        DEFINES ARTS_UNIT_STANDALONE_SHIMS=1 PASS_REGEX "PASS rank_bitset" TIMEOUT 60)
endif()
# T169 EXPOSES B-set-ip-null: expected to FAIL/crash under sanitizer (documents runtime bug, do not mask)
add_pure_unit_src(socket_set_ip_null_ifa LIBS ${CMAKE_DL_LIBS} TIMEOUT 60)
add_pure_unit_src(socket_helpers LIBS ${CMAKE_DL_LIBS} PASS_REGEX "PASS socket_helpers" TIMEOUT 60)
# T180 EXPOSES B071: expected to FAIL/crash under sanitizer (documents runtime bug, do not mask)
# (SEQUENCENUMBERS is an optional SEQ-header ABI variant supplied by the build dir
#  when enabled; not forced here so the default wire layout is exercised.)
add_pure_unit_src(protocol_abi_asserts TIMEOUT 60)
# Real-hardware Dekker litmus for the grant-baton release/re-check pair: the
# fenced shape must never lose an item; a weakened fence turns the runtime's
# 0.2% wedge back into a deterministic failure here.
# T184 VERIFIES B070 fix: dispatcher now floors header.size >= sizeof(packet)
add_pure_unit_src(dispatcher_payload_size_underflow TIMEOUT 60)
add_pure_unit_src(launcher_shell_quote PASS_REGEX "PASS launcher_shell_quote" TIMEOUT 60)
# T192 VERIFIES B110 fix: launcher command builder is overflow-safe (arts_cmd_appendf bounded-append)
add_pure_unit_src(stdio_forward_fidelity PASS_REGEX "PASS stdio_forward_fidelity" TIMEOUT 60)
add_pure_unit_src(stdio_forward_partial_eintr PASS_REGEX "PASS stdio_forward_partial_eintr" TIMEOUT 60)
add_pure_unit_src(stdio_forward_make_pipe_fail PASS_REGEX "PASS stdio_forward_make_pipe_fail" TIMEOUT 60)
add_pure_unit_src(stdio_forward_fileno_negative PASS_REGEX "PASS stdio_forward_fileno_negative" TIMEOUT 60)
add_pure_unit_src(stdio_forward_shutdown_idempotent PASS_REGEX "PASS stdio_forward_shutdown_idempotent" TIMEOUT 60)
add_pure_unit_src(stdio_forward_concurrent_push PASS_REGEX "PASS stdio_forward_concurrent_push" TIMEOUT 60)
add_pure_unit_src(stdio_forward_fflush_no_deadlock PASS_REGEX "PASS stdio_forward_fflush_no_deadlock" TIMEOUT 60)
add_pure_unit_src(stdio_forward_multi_source PASS_REGEX "PASS stdio_forward_multi_source" TIMEOUT 60)
# T203 EXPOSES B097: expected to FAIL/crash under sanitizer (documents runtime bug, do not mask)
add_pure_unit_src(config_routing_table_overflow TIMEOUT 60)
# T204 EXPOSES B098: expected to FAIL/crash under sanitizer (documents runtime bug, do not mask)
add_pure_unit_src(config_lsf_single_host_oob TIMEOUT 60)
# T205 EXPOSES B100: expected to FAIL/crash under sanitizer (documents runtime bug, do not mask)
add_pure_unit_src(config_find_variable TIMEOUT 60)
add_pure_unit_src(config_parse_port_spec PASS_REGEX "PASS config_parse_port_spec" TIMEOUT 60)
add_pure_unit_src(config_count_nodes PASS_REGEX "PASS config_count_nodes" TIMEOUT 60)
add_pure_unit_src(config_get_variables_oob PASS_REGEX "PASS config_get_variables_oob" TIMEOUT 60)
add_pure_unit_src(config_routing_table_ssh_bracket PASS_REGEX "PASS config_routing_table_ssh_bracket" TIMEOUT 60)
add_pure_unit_src(config_routing_table_slurm_hostlist PASS_REGEX "PASS config_routing_table_slurm_hostlist" TIMEOUT 60)
add_pure_unit_src(placement_invariants SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/system/placement.c PASS_REGEX "PASS placement_invariants" TIMEOUT 60)
add_pure_unit_src(config_route_table_size_shift PASS_REGEX "PASS config_route_table_size_shift" TIMEOUT 60)
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
# T250 EXPOSES B138: expected to FAIL/crash under sanitizer (documents runtime bug, do not mask)
add_pure_unit_src(json_writer SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/counter/json.c TIMEOUT 60)
add_pure_unit_src(edge_vector PASS_REGEX "PASS edge_vector" TIMEOUT 60)
# T261 EXPOSES B140: expected to FAIL/crash under sanitizer (documents runtime bug, do not mask)
add_pure_unit_src(block_dist_query TIMEOUT 60)
# T272 EXPOSES B151: expected to FAIL/crash under sanitizer (documents runtime bug, do not mask)
add_pure_unit_src(csr_free_null TIMEOUT 60)

# ============================================================================
# Wave C/D runtime + config_specific tests
#
# Generated from research/test-census/waveCD-result.json (clusters C02r..C27, C12r..C18r,
# C19r/C21r/C22r/C23r/C24r/C25r) plus the C13 EDT cluster (scanned from tests/ocr/
# directly — its census metadata was lost when the authoring agent died).
#
# Harness mapping:
#   runtime_single / config_specific -> register_single_node_test
#   runtime_multinode                -> register_multinode_test (_2n/_3n/_4n/_2n_io)
#   "both" notes                     -> single AND multinode
# Protocol/placement selection is supplied GLOBALLY by the build dir's coherence
# configuration; the test bodies self-skip (print SKIP, exit 0) in non-target
# configs, so config_specific PASS regexes accept the SKIP line too.
#
# Bug-exposing tests (exposes_runtime_bug=true with empty pass_regex) are
# registered WITHOUT a PASS_REGULAR_EXPRESSION so they visibly FAIL/hang in
# their target config. They are NOT masked with WILL_FAIL. Do not weaken.
# ============================================================================

# --- C12: DB lifecycle & acquire/release accounting ---
# dep_alias_classify: pure_unit — the acquire engine's owner/alias rule is a
# pure function of a dependence vector, so it is driven on stack arrays out of
# the per-config static libarts; the runtime is never started and the answers
# are configuration-independent.
add_arts_test(dep_alias_classify)
register_pure_unit_test(dep_alias_classify TIMEOUT 30)
set_tests_properties(dep_alias_classify PROPERTIES PASS_REGULAR_EXPRESSION "PASS dep_alias_classify")

add_arts_test(db_alias_dedup)
register_single_node_test(db_alias_dedup TIMEOUT 30)
set_tests_properties(db_alias_dedup PROPERTIES PASS_REGULAR_EXPRESSION "PASS: db_alias_dedup|SKIP db_alias_dedup")

# config_specific: EXCL-only real body, self-skips elsewhere
add_arts_test(db_excl_creator_release_keeps_joiner_write)
register_single_node_test(db_excl_creator_release_keeps_joiner_write TIMEOUT 30)
set_tests_properties(db_excl_creator_release_keeps_joiner_write PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS: db_excl_creator_release_keeps_joiner_write|SKIP db_excl_creator_release_keeps_joiner_write")

add_arts_test(db_acquire_replay_local)
register_single_node_test(db_acquire_replay_local TIMEOUT 30)
set_tests_properties(db_acquire_replay_local PROPERTIES PASS_REGULAR_EXPRESSION "PASS: db_acquire_replay_local|SKIP db_acquire_replay_local")

# A mid-EDT release of a block one EDT names twice must drop the block's single
# coherence hold exactly once — the owner slot is released and its aliases retire
# with it.  The double drop this was written to expose is fixed, so it is a plain
# green test in every configuration; a printed FAIL, a crash or a hang is what
# fails it (FAIL_REGULAR_EXPRESSION + the TIMEOUT).
add_arts_test(db_release_alias_slot)
register_single_node_test(db_release_alias_slot TIMEOUT 30)
register_multinode_test(db_release_alias_slot TIMEOUT 60)

# both single + multinode meaningful (remote homes force the GRANT_REQUEST/GRANT double-fire path)
add_arts_test(db_rw_secure_double_fire)
register_single_node_test(db_rw_secure_double_fire TIMEOUT 60)
register_multinode_test(db_rw_secure_double_fire TIMEOUT 60)
set_tests_properties(db_rw_secure_double_fire PROPERTIES PASS_REGULAR_EXPRESSION "PASS: db_rw_secure_double_fire|SKIP db_rw_secure_double_fire")
set_tests_properties(db_rw_secure_double_fire_2n PROPERTIES PASS_REGULAR_EXPRESSION "PASS: db_rw_secure_double_fire|SKIP db_rw_secure_double_fire")

add_arts_test(db_acquire_all_bias)
register_multinode_test(db_acquire_all_bias TIMEOUT 60)
set_tests_properties(db_acquire_all_bias_2n PROPERTIES PASS_REGULAR_EXPRESSION "PASS: db_acquire_all_bias|SKIP db_acquire_all_bias")

add_arts_test(db_destroy_implicit_release)
register_single_node_test(db_destroy_implicit_release TIMEOUT 30)
set_tests_properties(db_destroy_implicit_release PROPERTIES PASS_REGULAR_EXPRESSION "PASS: db_destroy_implicit_release|SKIP db_destroy_implicit_release")

# dbcreate_matrix: probe of the DB-create capability matrix --
# (creator-acquisition) x (home-determination) x (locality).  Exercises the
# ARTS core create path directly (arts.h, no OCR shim), so it is a coverage
# regression guard rather than a bug-exposing test: each cell prints its result
# independently, so require only that the scalar appears, not a specific count.
# Cross-runtime capability parity (shim vs the reference runtimes) is compared
# separately through the correctness harness, not here.
add_arts_test(dbcreate_matrix)
register_single_node_test(dbcreate_matrix TIMEOUT 60)
register_multinode_test(dbcreate_matrix TIMEOUT 60)

# --- C14: EDT context save/restore + created-DB / owned-finish cleanup ---
add_arts_test(ctx_save_restore_nested)
register_single_node_test(ctx_save_restore_nested TIMEOUT 30)
set_tests_properties(ctx_save_restore_nested PROPERTIES PASS_REGULAR_EXPRESSION "PASS ctx_save_restore_nested|SKIP ctx_save_restore_nested")

add_arts_test(ctx_created_db_release_order)
register_single_node_test(ctx_created_db_release_order TIMEOUT 30)
set_tests_properties(ctx_created_db_release_order PROPERTIES PASS_REGULAR_EXPRESSION "PASS ctx_created_db_release_order|SKIP ctx_created_db_release_order")

add_arts_test(ctx_owned_finish_cleanup)
register_single_node_test(ctx_owned_finish_cleanup TIMEOUT 30)

add_arts_test(ctx_null_guards)
register_single_node_test(ctx_null_guards TIMEOUT 30)
set_tests_properties(ctx_null_guards PROPERTIES PASS_REGULAR_EXPRESSION "PASS ctx_null_guards|SKIP ctx_null_guards")

add_arts_test(ctx_crossrank_proxy_finish)
register_multinode_test(ctx_crossrank_proxy_finish TIMEOUT 60)
set_tests_properties(ctx_crossrank_proxy_finish_2n PROPERTIES PASS_REGULAR_EXPRESSION "PASS ctx_crossrank_proxy_finish|SKIP ctx_crossrank_proxy_finish")

# ctx_gpu_lib_edt: GPU-only real body (ARTS_TEST_GPU); SKIP stub links CPU libarts otherwise.
if(BUILD_CUDA_LIBRARY)
    add_arts_test(ctx_gpu_lib_edt)
    target_compile_definitions(ctx_gpu_lib_edt PRIVATE ARTS_TEST_GPU=1)
    register_gpu_test(ctx_gpu_lib_edt TIMEOUT 30)
    set_tests_properties(ctx_gpu_lib_edt PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS ctx_gpu_lib_edt|SKIP ctx_gpu_lib_edt")
endif()

# --- C15: event channel / satisfy / destroy / create-collision ---
# These three record into one block from EDTs with no order among them, which
# DB-WRF does not admit.
add_arts_test(event_channel_transient_null)
add_arts_test(event_satisfy_adddep_window)
add_arts_test(event_finish_latch_rearm)
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_single_node_test(event_channel_transient_null TIMEOUT 60)
register_single_node_test(event_satisfy_adddep_window TIMEOUT 60)
set_tests_properties(event_satisfy_adddep_window PROPERTIES PASS_REGULAR_EXPRESSION "event_satisfy_adddep_window:.*PASS|SKIP event_satisfy_adddep_window")
register_single_node_test(event_finish_latch_rearm TIMEOUT 60)
set_tests_properties(event_finish_latch_rearm PROPERTIES PASS_REGULAR_EXPRESSION "event_finish_latch_rearm:.*PASS|SKIP event_finish_latch_rearm")
endif()

add_arts_test(event_error_paths)
register_single_node_test(event_error_paths TIMEOUT 30)
set_tests_properties(event_error_paths PROPERTIES PASS_REGULAR_EXPRESSION "CHANNEL: only DECR|SKIP event_error_paths")

# both single + multinode (OoO defer is home-rank-local but census asks for multinode too)
# COUNTED delivery + reclamation, and that an undeclared ONCE still lingers.
# Whitebox (reads the route table), single-node: the property is local.
add_arts_test(event_counted_reclaim)
register_single_node_test(event_counted_reclaim TIMEOUT 60)

add_arts_test(event_destroy_before_create)
register_single_node_test(event_destroy_before_create TIMEOUT 30)
register_multinode_test(event_destroy_before_create TIMEOUT 60)
set_tests_properties(event_destroy_before_create PROPERTIES PASS_REGULAR_EXPRESSION "event_destroy_before_create:.*PASS|SKIP event_destroy_before_create")
set_tests_properties(event_destroy_before_create_2n PROPERTIES PASS_REGULAR_EXPRESSION "event_destroy_before_create:.*PASS|SKIP event_destroy_before_create")

# event_gpu_force_defer: GPU-only real body (ARTS_USE_GPU); SKIP stub otherwise.
if(BUILD_CUDA_LIBRARY)
    add_arts_test(event_gpu_force_defer)
    target_compile_definitions(event_gpu_force_defer PRIVATE ARTS_USE_GPU)
    register_gpu_test(event_gpu_force_defer TIMEOUT 30)
    set_tests_properties(event_gpu_force_defer PROPERTIES
        PASS_REGULAR_EXPRESSION "event_gpu_force_defer:.*PASS|SKIP event_gpu_force_defer")
endif()

# --- C07: grant retention ---

# grant_sticky: asserts the grant outlives its writers (single-rank only —
# a second rank could revoke it, which is what the test must exclude).
add_arts_test(grant_sticky)
register_single_node_test(grant_sticky TIMEOUT 60)
set_tests_properties(grant_sticky PROPERTIES PASS_REGULAR_EXPRESSION "PASS grant_sticky|SKIP grant_sticky")

# grant_ex_holder_sharer: after a grant moves, the ex-holder must be retired
# by the new owner's rounds.  2+ ranks (the grant has to leave the reader).
add_arts_test(grant_ex_holder_sharer)
register_multinode_test(grant_ex_holder_sharer TIMEOUT 120)
set_tests_properties(grant_ex_holder_sharer_2n PROPERTIES PASS_REGULAR_EXPRESSION "PASS grant_ex_holder_sharer|SKIP grant_ex_holder_sharer")
set_tests_properties(grant_ex_holder_sharer_3n PROPERTIES PASS_REGULAR_EXPRESSION "PASS grant_ex_holder_sharer|SKIP grant_ex_holder_sharer")
set_tests_properties(grant_ex_holder_sharer_4n PROPERTIES PASS_REGULAR_EXPRESSION "PASS grant_ex_holder_sharer|SKIP grant_ex_holder_sharer")
set_tests_properties(grant_ex_holder_sharer_2n_io PROPERTIES PASS_REGULAR_EXPRESSION "PASS grant_ex_holder_sharer|SKIP grant_ex_holder_sharer")

# --- C09: EXCL protocol-specific ---
# T093 EXPOSES B-lock-samerank-rw-grant-loss: expected to FAIL (hang) under EXCL; self-skips
# elsewhere. Do not mask.
add_arts_test(excl_samerank_rw_grant_race)
register_single_node_test(excl_samerank_rw_grant_race TIMEOUT 60)
register_multinode_test(excl_samerank_rw_grant_race TIMEOUT 60)

# T094 EXPOSES B-lock-multiconsumer-queue: expected to FAIL (lost GRANT / stranded writer)
# under EXCL; valid RW-churn correctness check (no skip) elsewhere. Do not mask.
add_arts_test(excl_multiconsumer_queue)
register_single_node_test(excl_multiconsumer_queue TIMEOUT 60)
register_multinode_test(excl_multiconsumer_queue TIMEOUT 60)

# With peers the cohorts' requests cross the wire to the blocks' home.
add_arts_test(excl_request_coalesce)
register_single_node_test(excl_request_coalesce TIMEOUT 60)
register_multinode_test(excl_request_coalesce TIMEOUT 120)

add_arts_test(excl_purge_grant_d6_d7)
register_single_node_test(excl_purge_grant_d6_d7 TIMEOUT 60)
register_multinode_test(excl_purge_grant_d6_d7 TIMEOUT 60)
# The end-of-run line is printed only once both halves have finished, so
# requiring it on every variant turns a stranded writer into a failure.
foreach(_v "" _2n _3n _4n _2n_io)
    set_tests_properties(excl_purge_grant_d6_d7${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION
        "excl_purge_grant_d6_d7 D7: .* D6 chain verified — PASS|SKIP excl_purge_grant_d6_d7")
endforeach()

# excl_req_before_create: protocol-agnostic, needs 3+ ranks (verbatim copy of the old
# coherence_lock_req_before_create into the planned filename); register at 3n/4n.
add_arts_test(excl_req_before_create)
# These assert what the runtime's exclusive-RW serialization guarantees; under
# DB-WRF that ordering is the program's, and these programs deliberately omit
# it.
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_multinode_test(excl_req_before_create TIMEOUT 90 VARIANTS 3n 4n)
set_tests_properties(excl_req_before_create_3n PROPERTIES PASS_REGULAR_EXPRESSION "PASS: [0-9]+ iterations completed|SKIP excl_req_before_create")
set_tests_properties(excl_req_before_create_4n PROPERTIES PASS_REGULAR_EXPRESSION "PASS: [0-9]+ iterations completed|SKIP excl_req_before_create")
endif()

# --- C11: snapshot / publish-ack / destroy-notify / dispatcher-parity (config_specific,
# but census asks for multinode variants to expose the wire reorder; register both) ---
# T108 EXPOSES B014/B028. Non-EXCL (snapshot-bearing); self-skips under EXCL.
add_arts_test(snapshot_response_3case)
register_single_node_test(snapshot_response_3case TIMEOUT 120)
register_multinode_test(snapshot_response_3case TIMEOUT 120)

# T112 EXPOSES B023 (self-send vs dispatcher parity). All protocols; needs 1n AND multinode.
add_arts_test(self_send_vs_dispatcher_parity)
register_single_node_test(self_send_vs_dispatcher_parity TIMEOUT 120)
register_multinode_test(self_send_vs_dispatcher_parity TIMEOUT 120)

# A dependence's first touch installs a stub that the later create claims.
# runtime_multinode (needs >=2 ranks).
add_arts_test(db_create_claims_first_touch_stub)
register_multinode_test(db_create_claims_first_touch_stub TIMEOUT 120)

# T115 EXPOSES B024 (Cat-C ref balance) — normally PASSES (regression guard). 1n + multinode.
add_arts_test(cat_c_ref_balance)
register_single_node_test(cat_c_ref_balance TIMEOUT 120)
register_multinode_test(cat_c_ref_balance TIMEOUT 120)

# T116 EXPOSES B017 (await_publish_ack under shutdown). WT only; self-skips under WB/EXCL.
# Normally PASSES (prints token before shutdown). 1n + multinode.
add_arts_test(await_publish_ack_shutdown)
register_single_node_test(await_publish_ack_shutdown TIMEOUT 120)
register_multinode_test(await_publish_ack_shutdown TIMEOUT 120)
set_tests_properties(await_publish_ack_shutdown PROPERTIES PASS_REGULAR_EXPRESSION "PASS: await_publish_ack_shutdown reached shutdown|SKIP await_publish_ack_shutdown")
set_tests_properties(await_publish_ack_shutdown_2n PROPERTIES PASS_REGULAR_EXPRESSION "PASS: await_publish_ack_shutdown reached shutdown|SKIP await_publish_ack_shutdown")

# --- C03r: route-table / GUID determinism ---

add_arts_test(guid_crossrank_determinism)
register_multinode_test(guid_crossrank_determinism TIMEOUT 120)

add_arts_test(guid_keygen_table_coupling)
register_multinode_test(guid_keygen_table_coupling TIMEOUT 120)

add_arts_test(round_robin_home_distribution)
register_multinode_test(round_robin_home_distribution TIMEOUT 120)

add_arts_test(guid_index_from_mismatch)
register_single_node_test(guid_index_from_mismatch TIMEOUT 60)

# --- C04r: OoO defer/replay engine ---
# Its writers are unordered, which DB-WRF does not admit.
add_arts_test(ooo_concurrent_multidrain)
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_single_node_test(ooo_concurrent_multidrain TIMEOUT 120)
endif()

add_arts_test(ooo_hit_ref_pin)
register_single_node_test(ooo_hit_ref_pin TIMEOUT 120)

# --- C05r: DB create-with-data / stub install ---
add_arts_test(db_create_with_data_bytes)
register_single_node_test(db_create_with_data_bytes TIMEOUT 60)

# T059 EXPOSES B-mark-double-dec: expected to FAIL (delta==2 not 1). runtime_single (boots
# arts_rt for route-table/EDT alloc). Lives in tests/unit/. Do not mask.
if(NOT "mark_edt_ready_idempotent" IN_LIST ARTS_FAM_DIRECT_EXCLUDED)
add_arts_test(mark_edt_ready_idempotent)
register_single_node_test(mark_edt_ready_idempotent TIMEOUT 60)
endif()

add_arts_test(stub_install_winner)
register_multinode_test(stub_install_winner TIMEOUT 180)
foreach(_v 2n 3n 4n 2n_io)
    set_tests_properties(stub_install_winner_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: stub_install_winner|SKIP stub_install_winner")
endforeach()

# --- C02r: scheduler ---
# T024 EXPOSES suspected non-owner-push deque[0] race — normally PASSES (guard). runtime_single.
# Both record into one block from EDTs with no order among them, which DB-WRF
# does not admit.
add_arts_test(scheduler_async_nonowner_push)
add_arts_test(scheduler_acquire_bias)
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_single_node_test(scheduler_async_nonowner_push TIMEOUT 120)
set_tests_properties(scheduler_async_nonowner_push PROPERTIES PASS_REGULAR_EXPRESSION "scheduler_async_nonowner_push: .* each ran once — PASS|SKIP scheduler_async_nonowner_push")
register_single_node_test(scheduler_acquire_bias TIMEOUT 120)
set_tests_properties(scheduler_acquire_bias PROPERTIES PASS_REGULAR_EXPRESSION "scheduler_acquire_bias: .* — PASS|SKIP scheduler_acquire_bias")
endif()

add_arts_test(scheduler_loop_variants)
register_single_node_test(scheduler_loop_variants TIMEOUT 60)
set_tests_properties(scheduler_loop_variants PROPERTIES PASS_REGULAR_EXPRESSION "scheduler_loop_variants: .* — PASS|SKIP scheduler_loop_variants")

# --- C18r: dispatcher ---
add_arts_test(dispatcher_default_fatal)
register_single_node_test(dispatcher_default_fatal TIMEOUT 60)

# --- C19r: launcher ---
add_arts_test(launcher_no_respawn)
register_multinode_test(launcher_no_respawn TIMEOUT 60)

add_arts_test(launcher_orphan_prevention)
register_multinode_test(launcher_orphan_prevention TIMEOUT 60)
foreach(_v 2n 3n 4n 2n_io)
    set_tests_properties(launcher_orphan_prevention_${_v} PROPERTIES PASS_REGULAR_EXPRESSION "PASS: launcher_orphan_prevention|SKIP launcher_orphan_prevention_${_v}")
endforeach()

# --- C21r: config loading ---
add_arts_test(config_load_multinode_port_offset)
register_multinode_test(config_load_multinode_port_offset TIMEOUT 60)
foreach(_v 2n 3n 4n 2n_io)
    set_tests_properties(config_load_multinode_port_offset_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS config_load_multinode_port_offset|SKIP config_load_multinode_port_offset_${_v}")
endforeach()

add_arts_test(config_load_single_reclaim)
register_single_node_test(config_load_single_reclaim TIMEOUT 30)
set_tests_properties(config_load_single_reclaim PROPERTIES PASS_REGULAR_EXPRESSION "PASS config_load_single_reclaim|SKIP config_load_single_reclaim")

# config_override_embedded: pure_unit (links libarts, starts no runtime).
add_arts_test(config_override_embedded)
register_pure_unit_test(config_override_embedded TIMEOUT 30)
set_tests_properties(config_override_embedded PROPERTIES PASS_REGULAR_EXPRESSION "PASS config_override_embedded|SKIP config_override_embedded")

# config_removed_keys_reject: pure_unit (links libarts, starts no runtime;
# forks to probe the sender_threads/receiver_threads ARTS_ERROR death paths).
add_arts_test(config_removed_keys_reject)
register_pure_unit_test(config_removed_keys_reject TIMEOUT 30)
set_tests_properties(config_removed_keys_reject PROPERTIES PASS_REGULAR_EXPRESSION "PASS config_removed_keys_reject|SKIP config_removed_keys_reject")

# config_provider_regpool_parse: pure_unit (links libarts, starts no runtime).
add_arts_test(config_provider_regpool_parse)
register_pure_unit_test(config_provider_regpool_parse TIMEOUT 30)
set_tests_properties(config_provider_regpool_parse PROPERTIES PASS_REGULAR_EXPRESSION "PASS config_provider_regpool_parse|SKIP config_provider_regpool_parse")

# --- C22r: runtime startup/shutdown/threads/signals ---
add_arts_test(runtime_barrier_counts)
register_single_node_test(runtime_barrier_counts TIMEOUT 60)

# T221 EXPOSES B-shutdown-state-dualpath — normally green baseline (documented, not deterministic).
add_arts_test(runtime_concurrent_shutdown)
register_single_node_test(runtime_concurrent_shutdown TIMEOUT 60)
set_tests_properties(runtime_concurrent_shutdown PROPERTIES PASS_REGULAR_EXPRESSION "SHUTDOWN_STATE_OK|SKIP runtime_concurrent_shutdown")

# runtime_gpu_scheduler_promotion: GPU-only (ARTS_USE_GPU); SKIP stub otherwise.
if(BUILD_CUDA_LIBRARY)
    add_arts_test(runtime_gpu_scheduler_promotion)
    target_compile_definitions(runtime_gpu_scheduler_promotion PRIVATE ARTS_USE_GPU)
    register_gpu_test(runtime_gpu_scheduler_promotion TIMEOUT 120)
    set_tests_properties(runtime_gpu_scheduler_promotion PROPERTIES
        PASS_REGULAR_EXPRESSION "GPU_SCHED_PROMOTION_DONE|SKIP runtime_gpu_scheduler_promotion")
endif()

add_arts_test(threads_shutdown_cancel)
register_single_node_test(threads_shutdown_cancel TIMEOUT 60)
set_tests_properties(threads_shutdown_cancel PROPERTIES PASS_REGULAR_EXPRESSION "SHUTDOWN_CANCEL_DONE|SKIP threads_shutdown_cancel")

add_arts_test(threads_pin_affinity)
register_single_node_test(threads_pin_affinity TIMEOUT 60)

add_arts_test(signals_sigterm_graceful)
register_single_node_test(signals_sigterm_graceful TIMEOUT 60)
set_tests_properties(signals_sigterm_graceful PROPERTIES PASS_REGULAR_EXPRESSION "SIGTERM_GRACEFUL_OK|SKIP signals_sigterm_graceful")

# signals_double_sigterm: success criterion is exit code 143 (= 128+SIGTERM), not a stdout
# token; pass_regex empty per the census. The DOUBLE_SIGTERM_UNEXPECTED_CLEAN_EXIT token marks
# a regression. Exit-code based: WILL_FAIL accepts the non-zero (143) escalation exit; the
# FAIL_REGULAR_EXPRESSION catches a regression that exits 0 after printing the clean token.
add_arts_test(signals_double_sigterm)
register_single_node_test(signals_double_sigterm TIMEOUT 60)
set_tests_properties(signals_double_sigterm PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "DOUBLE_SIGTERM_UNEXPECTED_CLEAN_EXIT")

# main_rank_overflow_abort: success is abort with exit 1; exit-code based.  WILL_FAIL accepts
# the non-zero (abort) exit; the FAIL_REGULAR_EXPRESSION catches a regression that returns 0
# after printing RANK_OVERFLOW_NO_ABORT (the guard failed to fire).
add_arts_test(main_rank_overflow_abort)
register_single_node_test(main_rank_overflow_abort TIMEOUT 60)
set_tests_properties(main_rank_overflow_abort PROPERTIES
    WILL_FAIL TRUE
    FAIL_REGULAR_EXPRESSION "RANK_OVERFLOW_NO_ABORT")

add_arts_test(shutdown_abort_eof)
register_multinode_test(shutdown_abort_eof TIMEOUT 120)
foreach(_v 2n 3n 4n 2n_io)
    set_tests_properties(shutdown_abort_eof_${_v} PROPERTIES PASS_REGULAR_EXPRESSION "ABORT_EOF_EXIT|SKIP shutdown_abort_eof_${_v}")
endforeach()

add_arts_test(shutdown_idempotent_concurrent_init)
register_multinode_test(shutdown_idempotent_concurrent_init TIMEOUT 120)
foreach(_v 2n 3n 4n 2n_io)
    set_tests_properties(shutdown_idempotent_concurrent_init_${_v} PROPERTIES PASS_REGULAR_EXPRESSION "IDEMPOTENT_INIT_EXIT|SKIP shutdown_idempotent_concurrent_init_${_v}")
endforeach()

# --- C23r: random / system-info ---
# T244 EXPOSES B129 (sign-extended jrand48): expected to FAIL (high-bits set). Do not mask.
add_arts_test(random_thread_safe)
register_single_node_test(random_thread_safe TIMEOUT 60)

add_arts_test(system_info_per_rank)
register_multinode_test(system_info_per_rank TIMEOUT 120)

add_arts_test(system_info_in_edt)
register_single_node_test(system_info_in_edt TIMEOUT 60)

# --- C24r: counters ---
# counter_smoke_value reads NUM_EDT_CREATE back from the counter files, which
# exist only where the tree's counter config captures it; everywhere else there
# is nothing for the program to read, so it is built but not registered.
add_arts_test(counter_smoke_value)
file(STRINGS "${COUNTER_CONFIG_FILE}" _num_edt_create REGEX "^NUM_EDT_CREATE=")
if(_num_edt_create AND NOT _num_edt_create MATCHES "^NUM_EDT_CREATE=OFF")
    register_single_node_test(counter_smoke_value TIMEOUT 30)
    set_tests_properties(counter_smoke_value PROPERTIES PASS_REGULAR_EXPRESSION "PASS counter_smoke_value")
endif()

add_arts_test(counter_timer_balance)
register_single_node_test(counter_timer_balance TIMEOUT 30)
set_tests_properties(counter_timer_balance PROPERTIES PASS_REGULAR_EXPRESSION "PASS counter_timer_balance|SKIP counter_timer_balance")

# --- C25r: block dist / CSR ---
# T262 EXPOSES B-args-oob: expected to FAIL (ASan OOB). runtime_single (needs arts_rt). Do not mask.
add_arts_test(dist_args_oob)
register_single_node_test(dist_args_oob TIMEOUT 10)

add_arts_test(block_dist_roundrobin)
register_multinode_test(block_dist_roundrobin TIMEOUT 60)

# T264 EXPOSES B-csr-empty-ub: expected to FAIL (NULL deref). Do not mask.
add_arts_test(csr_empty_partition)
register_single_node_test(csr_empty_partition TIMEOUT 10)

add_arts_test(csr_wellformed)
register_single_node_test(csr_wellformed TIMEOUT 10)

add_arts_test(csr_from_guid_refcount)
register_single_node_test(csr_from_guid_refcount TIMEOUT 10)

add_arts_test(csr_get_neighbors)
register_single_node_test(csr_get_neighbors TIMEOUT 10)

# T268 EXPOSES B-csr-leak: expected to FAIL (LSan leak). Do not mask.
add_arts_test(csr_load_edgelist)
register_single_node_test(csr_load_edgelist TIMEOUT 10)

# T269 EXPOSES B-csr-leak + B-csr-token-zero: expected to FAIL (LSan leak). Do not mask.
add_arts_test(csr_load_csr_format)
register_single_node_test(csr_load_csr_format TIMEOUT 10)

# T270 EXPOSES B-args-oob (+ B-csr-leak): expected to FAIL (ASan OOB). Do not mask.
add_arts_test(csr_load_args)
register_single_node_test(csr_load_args TIMEOUT 10)

add_arts_test(csr_remote_null)
register_multinode_test(csr_remote_null TIMEOUT 60)

# --- C27: public-API surface ---
add_arts_test(api_init_per_node_worker)
register_single_node_test(api_init_per_node_worker TIMEOUT 10)
set_tests_properties(api_init_per_node_worker PROPERTIES PASS_REGULAR_EXPRESSION "PASS api_init_per_node_worker|SKIP api_init_per_node_worker")

add_arts_test(api_current_finish_event_null)
register_single_node_test(api_current_finish_event_null TIMEOUT 10)
set_tests_properties(api_current_finish_event_null PROPERTIES PASS_REGULAR_EXPRESSION "PASS api_current_finish_event_null|SKIP api_current_finish_event_null")

add_arts_test(api_latch_incr_slot)
register_single_node_test(api_latch_incr_slot TIMEOUT 10)
set_tests_properties(api_latch_incr_slot PROPERTIES PASS_REGULAR_EXPRESSION "PASS api_latch_incr_slot|SKIP api_latch_incr_slot")

# api_gpu_context_accessors: GPU-only real body (ARTS_TEST_GPU); SKIP stub otherwise.
if(BUILD_CUDA_LIBRARY)
    add_arts_test(api_gpu_context_accessors)
    target_compile_definitions(api_gpu_context_accessors PRIVATE ARTS_TEST_GPU=1)
    register_gpu_test(api_gpu_context_accessors TIMEOUT 10)
    set_tests_properties(api_gpu_context_accessors PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS api_gpu_context_accessors|SKIP api_gpu_context_accessors")
endif()

# --- C13: EDT semantics (census metadata lost; classified by scanning tests/ocr/) ---
# Its writers are unordered, which DB-WRF does not admit.
add_arts_test(edt_sentinel_single_fire)
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_single_node_test(edt_sentinel_single_fire TIMEOUT 30)
set_tests_properties(edt_sentinel_single_fire PROPERTIES PASS_REGULAR_EXPRESSION "PASS edt_sentinel_single_fire|SKIP edt_sentinel_single_fire")
endif()

# edt_satisfy_out_of_range EXPOSES a satisfy-out-of-range bug: prints no PASS token (FAIL+abort
# or hang). Registered WITHOUT a PASS_REGULAR_EXPRESSION. Do not mask.
add_arts_test(edt_satisfy_out_of_range)
register_single_node_test(edt_satisfy_out_of_range TIMEOUT 30)

# edt_output_event: picks consumer_rank 1 when available; runs on 1n too.
# single + multinode.  Its writers are unordered, which DB-WRF does not admit.
add_arts_test(edt_output_event)
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
register_single_node_test(edt_output_event TIMEOUT 30)
register_multinode_test(edt_output_event TIMEOUT 60)
foreach(_v "" _2n _3n _4n _2n_io)
    set_tests_properties(edt_output_event${_v} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
endif()

# edt_finish_scope_balance: round-robins members across all ranks; runs on 1n too. single + multinode.
add_arts_test(edt_finish_scope_balance)
register_single_node_test(edt_finish_scope_balance TIMEOUT 30)
register_multinode_test(edt_finish_scope_balance TIMEOUT 60)
foreach(_v "" _2n _3n _4n _2n_io)
    set_tests_properties(edt_finish_scope_balance${_v} PROPERTIES PASS_REGULAR_EXPRESSION "PASS edt_finish_scope_balance|SKIP edt_finish_scope_balance")
endforeach()

# edt_remote_create_reordered_satisfy: needs 2+ ranks; SKIPs cleanly on 1n.
add_arts_test(edt_remote_create_reordered_satisfy)
register_multinode_test(edt_remote_create_reordered_satisfy TIMEOUT 60)
foreach(_v 2n 3n 4n 2n_io)
    set_tests_properties(edt_remote_create_reordered_satisfy_${_v} PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS edt_remote_create_reordered_satisfy|SKIP edt_remote_create_reordered_satisfy")
endforeach()

# edt_size_offsets: pure_unit, but calls libarts symbols (arts_get_depv, check_out_edts) so it
# links libarts via add_arts_test + register_pure_unit_test (starts no runtime).
add_arts_test(edt_size_offsets)
register_pure_unit_test(edt_size_offsets TIMEOUT 30)
set_tests_properties(edt_size_offsets PROPERTIES PASS_REGULAR_EXPRESSION "PASS edt_size_offsets|SKIP edt_size_offsets")

# --- Wave C/D pure_unit tests (C14 T150 / C15 T162) ---
# vector_basic: #includes vector.c directly (libc shims); pins the contracts
# the per-worker EDT-context lists lean on (lazy alloc, doubling, swap-remove,
# clear-keeps-block, free-keeps-sentinel).
add_pure_unit_src(vector_basic PASS_REGEX "PASS vector_basic" TIMEOUT 30)

# ============================================================================
# C06 per-protocol pure_unit tests
#
# These compile specific protocol-coherence source TUs directly (NOT linking
# libarts) via add_pure_unit_src with ARTS_UNIT_STANDALONE_SHIMS, and self-adapt
# to the build dir's protocol macro (self-skip / print SKIP in non-target
# configs).
# ============================================================================

# directory_grantreq_queue: VAL/EXCL (the .c #if-guards select the protocol directory.c).
add_pure_unit_src(directory_grantreq_queue DEFINES ARTS_UNIT_STANDALONE_SHIMS PASS_REGEX "PASS directory_grantreq_queue:|SKIP" TIMEOUT 60)

# pending_rw_treiber: VAL only (self-skips else).
add_pure_unit_src(pending_rw_treiber DEFINES ARTS_UNIT_STANDALONE_SHIMS PASS_REGEX "PASS pending_rw_treiber:|SKIP" TIMEOUT 60)

# excl_compute_next: EXCL only (self-skips else).
add_pure_unit_src(excl_compute_next DEFINES ARTS_UNIT_STANDALONE_SHIMS PASS_REGEX "PASS excl_compute_next:|SKIP" TIMEOUT 60)

# inv_compute_next: INV only (self-skips else).  One truth table covers both
# placements — the arbiters are placement-independent.
add_pure_unit_src(inv_compute_next DEFINES ARTS_UNIT_STANDALONE_SHIMS PASS_REGEX "PASS inv_compute_next:|SKIP" TIMEOUT 60)

# acquire_is_serialized (T068): needs_full_build — it does NOT #include a coherence .c; it
# links the real arts_db_acquire_is_serialized symbol out of the per-config static libarts and
# never starts the runtime. So it uses add_arts_test (links libarts) + register_pure_unit_test.
# Runs (and asserts the protocol-correct answer) under all 6 configs.
add_arts_test(acquire_is_serialized)
register_pure_unit_test(acquire_is_serialized TIMEOUT 30)
set_tests_properties(acquire_is_serialized PROPERTIES PASS_REGULAR_EXPRESSION "PASS acquire_is_serialized|SKIP acquire_is_serialized")

# ============================================================================
# C13 — EDT lifecycle, satisfy, finish-scope (T139-T144)
#
# Each links libarts and starts the runtime (except the GPU one, which
# self-skips out of config).  Single-node ones gate on "PASS <name>|SKIP"; the
# GPU one is wrapped in BUILD_CUDA_LIBRARY with the ARTS_USE_GPU define so its
# real body activates only in GPU builds.
# ============================================================================

# T139 — arts_edt_register_cb_deleter constructor-order vs route_table.
add_arts_test(edt_register_cb_deleter)
register_single_node_test(edt_register_cb_deleter TIMEOUT 30)
set_tests_properties(edt_register_cb_deleter PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS edt_register_cb_deleter|SKIP")

# T143 — DB_MODE_NULL raw-uint64 dep delivered to depv[slot].
add_arts_test(edt_value)
register_single_node_test(edt_value TIMEOUT 30)
set_tests_properties(edt_value PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS edt_value|SKIP")

# T140 — GPU LC drain force-defer (config_specific: GPU). The .c self-skips when
# ARTS_USE_GPU is undefined; in a CUDA build the define activates the real body.
if(BUILD_CUDA_LIBRARY)
    add_arts_test(edt_gpu_lc_force_defer)
    target_compile_definitions(edt_gpu_lc_force_defer PRIVATE ARTS_USE_GPU)
    register_gpu_test(edt_gpu_lc_force_defer TIMEOUT 30)
    set_tests_properties(edt_gpu_lc_force_defer PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS edt_gpu_lc_force_defer|SKIP")
endif()

# ============================================================================
# T294 — PASS-gate sweep: add FAIL_REGULAR_EXPRESSION to pre-existing OCR/unit
# tests that print a failure token (FAIL/ERROR/MISMATCH) on an internal failure
# but still exit 0, so a silent failure would pass ctest unnoticed.  This is the
# CONSERVATIVE form: a FAIL_REGULAR_EXPRESSION only catches the test's actual
# printed failure token, so it never false-fails a clean run (unlike a PASS
# regex, which would need an exact, always-present success token).  Every token
# matched here is printed ONLY on a failure branch (the runtime's own [ERROR]
# log always aborts, so it never appears on a clean run either).  Tests that
# rely solely on a nonzero exit code, the term_* signal tests, GPU .cu tests,
# and the Wave-A/C/D generated tests (already gated or intentional bug-exposers)
# are intentionally NOT included here.
# ============================================================================
set_tests_properties(acquire_mode PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|ERROR")
set_tests_properties(array_list_basic PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(atomics_locks PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(atomics_rmw_contention PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(atomics_rmw_conventions PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_home_producer_ro_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_home_producer_ro_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_home_producer_ro_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_home_producer_ro_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_owner_confirm_gate PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_owner_confirm_gate_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_owner_confirm_gate_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_owner_confirm_gate_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_owner_confirm_gate_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
if(NOT ARTS_COHERENCE_ARM STREQUAL "FLUSH")
foreach(_v 2n 3n 4n)
    set_tests_properties(coherence_multi_writer_dist_${_v} PROPERTIES
        FAIL_REGULAR_EXPRESSION "FAIL"
        PASS_REGULAR_EXPRESSION "PASS: EXCL distributed arbitration|SKIP coherence_multi_writer_dist")
endforeach()
endif()
set_tests_properties(coherence_ro_acquire_stress PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(coherence_stress_single_node PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(db_create PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(db_dependence PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(db_destroy PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(db_local_create PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(db_pin PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(deque_chaselev_race PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(deque_grow_during_steal PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(deque_last_element_tiebreak PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(deque_single_thread PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(diag_timestamp PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(edt_create_basic PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(edt_dep_variants PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(edt_fan_out PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(event_chain PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(event_channel_lifetime PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(event_idem_silent_oversatisfy PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(finish_event_mn_termination_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(finish_event_mn_termination_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(finish_event_mn_termination_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(finish_event_mn_termination_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(hint_routing PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(malloc_alignment PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|ERROR")
set_tests_properties(malloc_footprint_balance PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL|ERROR")
set_tests_properties(multinode_edt_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(multinode_edt_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(multinode_edt_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(multinode_edt_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(ooo_drain_repush PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(ooo_table_completeness PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(ooo_toctou_rescue PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(paramv_memcpy PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(event_channel_advanced PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(route_table_install_race PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(route_table_remote_guid_2n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(route_table_remote_guid_3n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(route_table_remote_guid_4n PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(route_table_remote_guid_2n_io PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(stdio_forward_scale_test PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
set_tests_properties(stress_edt PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Fabric-attached memory.  The bootstrap address frame carries {base,size}
# whatever backend (if any) a tree configures, and its layout half asserts the
# frame against the transport header alone -- no fam TU, no FAM define -- so
# it is asserted in EVERY tree.  Only its second half, gated in the source by
# #ifdef ARTS_FAM_BACKEND_DEVICE, compiles for the one backend no tree here
# can configure.
#
# Everything after it is registered only where the tree has the backend the
# test is about: the pool's address is an SHM concern, and a tree that never
# names the option must be untouched by this module.
if(ARTS_FAM_BACKEND STREQUAL "DEVICE" AND ARTS_FAM_TREE_RESIDENCY)
    # The recorder half compiles here, so the backend it asserts is linked in.
    set(_fam_frame_sources ${CMAKE_SOURCE_DIR}/libs/src/core/fam/device.c
                           unit/fam_stubs.c)
    if(ARTS_FAM_DEVICE_VENDORED)
        list(APPEND _fam_frame_sources
             ${CMAKE_SOURCE_DIR}/libs/src/core/fam/strict.c)
    endif()
    add_pure_unit_src(fam_device_frame PASS_REGEX "PASS fam_device_frame"
                      TIMEOUT 30 SOURCES ${_fam_frame_sources}
                      LIBS ${ARTS_FAM_DEVICE_LIBRARY})
    set_tests_properties(fam_device_frame PROPERTIES
                         RESOURCE_LOCK "arts_runtime")
else()
    add_pure_unit_src(fam_device_frame PASS_REGEX "PASS fam_device_frame"
                      TIMEOUT 30)
endif()
if(ARTS_FAM_BACKEND STREQUAL "SHM")
    add_pure_unit_src(fam_base_address PASS_REGEX "PASS fam_base_address"
                      DEFINES ARTS_FAM_BASE=${ARTS_FAM_BASE}ULL TIMEOUT 60)
    # These three COMPILE libs/src/core/fam/pool.c into the test binary, so they
    # need ARTS_FAM whatever the enclosing tree's protocol is -- and a tree can
    # legitimately have the backend with a non-EXCL protocol (a benchmark tree
    # carries the two variants and keeps its own default arm).  In such a tree
    # add_pure_unit_src's four-argument arts_apply_protocol resolves the
    # residency to the TREE's, which is empty, and pool.c would then compile
    # against pool.h's non-FAM arm: a `static inline` arts_fam_contains followed
    # by pool.c's non-static definition, and an undeclared arts_fam_backend_map
    # under the -Werror=implicit-function-declaration arts_apply_protocol itself
    # adds.  Naming the backend definitions per target is what makes these
    # independent of the tree's arm; the residency, if the tree has one, is
    # the tree's own.  They are the ONLY fam targets that do, because they
    # are the only ones that compile a fam TU.
    add_pure_unit_src(fam_pool_alloc PASS_REGEX "PASS fam_pool_alloc" TIMEOUT 120
                      DEFINES ARTS_FAM=1 ARTS_FAM_BACKEND_SHM=1
                      SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/fam/pool.c
                              unit/fam_stubs.c)
    add_pure_unit_src(fam_contains_preinit PASS_REGEX "PASS fam_contains_preinit"
                      TIMEOUT 30
                      DEFINES ARTS_FAM=1 ARTS_FAM_BACKEND_SHM=1
                      SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/fam/pool.c
                              unit/fam_stubs.c)
    add_pure_unit_src(fam_double_free PASS_REGEX "PASS fam_double_free"
                      TIMEOUT 60
                      DEFINES ARTS_FAM=1 ARTS_FAM_BACKEND_SHM=1
                      SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/fam/pool.c
                              unit/fam_stubs.c)
    # Links nothing of the module -- it performs the pool object's own
    # create/handoff sequence itself -- so it needs neither pool.c nor
    # fam_stubs.c.
    add_pure_unit_src(fam_shm_handoff PASS_REGEX "PASS fam_shm_handoff"
                      TIMEOUT 60)
    # The conformance probe builds its own struct arts_config_s and never
    # links a runtime -- so it is the one registered test that runs the
    # PLAIN backend whatever the tree's ARTS_FAM_TEST_STRICT says, and the
    # only test of arts_fam_strict()'s false case.  strict.c is in SOURCES
    # all the same: shm.c names it, so a probe without it does not link even
    # though it never takes the branch.
    add_pure_unit_src(fam_conformance PASS_REGEX "PASS fam_conformance"
                      TIMEOUT 120
                      DEFINES ARTS_FAM=1 ARTS_FAM_BACKEND_SHM=1
                              ARTS_FAM_BASE=${ARTS_FAM_BASE}ULL
                      SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/fam/pool.c
                              ${CMAKE_SOURCE_DIR}/libs/src/core/fam/shm.c
                              ${CMAKE_SOURCE_DIR}/libs/src/core/fam/strict.c
                              unit/fam_stubs.c)
endif()
# The device backend over the vendored fake library: the same probe, which
# takes the arena rank 0 names over its rendezvous as the runtime takes it over
# the address exchange.  Its default run is the tree's default, strict; the
# plain twin keeps the library's own flush path under test.  Only a tree whose
# own library is FAM-enabled carries the backend definitions these need.
if(ARTS_FAM_DEVICE_VENDORED AND ARTS_FAM_TREE_RESIDENCY)
    foreach(_fam_mode "" _plain)
        set(_fam_probe fam_conformance${_fam_mode})
        if(_fam_mode STREQUAL "")
            set(_fam_args)
        else()
            set(_fam_args --strict 0)
        endif()
        add_executable(${_fam_probe} unit/fam_conformance.c
                       ${CMAKE_SOURCE_DIR}/libs/src/core/fam/pool.c
                       ${CMAKE_SOURCE_DIR}/libs/src/core/fam/device.c
                       ${CMAKE_SOURCE_DIR}/libs/src/core/fam/strict.c
                       unit/fam_stubs.c)
        target_include_directories(${_fam_probe} PRIVATE
            ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
            ${ARTS_BUILD_INTERNAL_INCLUDE_DIR})
        arts_apply_protocol(${_fam_probe} ${ARTS_COHERENCE_ARM}
                            ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})
        target_link_libraries(${_fam_probe} PRIVATE Threads::Threads
                              ${ARTS_FAM_DEVICE_LIBRARY})
        add_test(NAME ${_fam_probe} COMMAND ${_fam_probe} ${_fam_args}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        set_tests_properties(${_fam_probe} PROPERTIES
            LABELS "single_node" TIMEOUT 120
            RESOURCE_LOCK "arts_runtime"
            PASS_REGULAR_EXPRESSION "PASS fam_conformance"
            FAIL_REGULAR_EXPRESSION "FAIL")
    endforeach()
endif()
# The device backend as a real device library compiles it: the vendored
# flag undefined, so the rule that library is held to -- fam_strict refused,
# and off by default -- is checked in a tree that can link it.
if(ARTS_FAM_BACKEND STREQUAL "DEVICE" AND ARTS_FAM_TREE_RESIDENCY)
    add_pure_unit_src(fam_strict_device_rule
                      PASS_REGEX "PASS fam_strict_device_rule" TIMEOUT 30
                      SOURCES ${CMAKE_SOURCE_DIR}/libs/src/core/fam/pool.c
                              ${CMAKE_SOURCE_DIR}/libs/src/core/fam/device.c
                              unit/fam_stubs.c
                      LIBS ${ARTS_FAM_DEVICE_LIBRARY})
    target_compile_options(fam_strict_device_rule PRIVATE
                           -UARTS_FAM_DEVICE_VENDORED)
    # The library maps its one named region at load, like every rank does.
    set_tests_properties(fam_strict_device_rule PROPERTIES
                         RESOURCE_LOCK "arts_runtime")
endif()
