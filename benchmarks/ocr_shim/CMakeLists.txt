# OCR-to-ARTS compatibility shim
# Implements OCR API functions using ARTS v2 primitives
# Output: one object library — apps combine its objects with the runtime they
# link (arts_static or arts_static_<suffix>)

set(OCR_ROOT "${CMAKE_SOURCE_DIR}/third_party/ocr/ocr")

add_compile_options(${ARTS_WERROR_IMPLICIT_SEQ_CST})

# Added from the top level, so this directory states its own compile setting.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(SHIM_C_OPT -O0 -g)
else()
    set(SHIM_C_OPT -O3)
endif()
# Defining OCR_TYPE_H makes ocr.h include "ocr-options_<value>.h"; the one it
# names here is a generated no-op, which silences ocr.h's pragma note.
arts_write_if_changed("${CMAKE_CURRENT_BINARY_DIR}/ocr-options_arts.h"
     "/* No-op OCR options for ARTS builds. */\n")
set(SHIM_OCR_TYPE_DEFINE "OCR_TYPE_H=arts.h")
set(SHIM_OCR_OPTIONS_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# The shim includes no coherence layout (the public header and internal
# headers no cell changes; coherence/types_common.h refuses a unit with no cell),
# so one object library serves this tree's own library, for the shim's tests,
# and every variant of a benchmark tree.
add_library(ocr_arts_objects OBJECT arts_ocr.c)
set_target_properties(ocr_arts_objects PROPERTIES C_STANDARD 11)
target_compile_options(ocr_arts_objects PRIVATE
    ${SHIM_C_OPT}
    -Wall
    ${ARTS_WNO_UNUSED}
)
# OCR extension defines (must come before OCR headers)
target_compile_definitions(ocr_arts_objects PRIVATE
    ENABLE_EXTENSION_AFFINITY
    ENABLE_EXTENSION_PARAMS_EVT
    ENABLE_EXTENSION_LABELING
    ENABLE_EXTENSION_RTITF
    ENABLE_EXTENSION_CHANNEL_EVT
    ENABLE_EXTENSION_COUNTED_EVT
    ${SHIM_OCR_TYPE_DEFINE}
)
target_include_directories(ocr_arts_objects PRIVATE
    ${OCR_ROOT}/inc                # ocr.h, ocr-types.h, etc.
    ${SHIM_OCR_OPTIONS_DIR}        # generated ocr-options_arts.h
    ${CMAKE_SOURCE_DIR}/libs/include/public
    ${CMAKE_SOURCE_DIR}/libs/include/internal
)
if(NOT ARTS_BUILD_TESTS)
    set_target_properties(ocr_arts_objects PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

add_executable(ocr_shim_event_payload EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/shim/event_payload.c
    $<TARGET_OBJECTS:ocr_arts_objects>)
target_include_directories(ocr_shim_event_payload PRIVATE
    ${OCR_ROOT}/inc ${SHIM_OCR_OPTIONS_DIR})
target_compile_definitions(ocr_shim_event_payload PRIVATE
    ENABLE_EXTENSION_AFFINITY ENABLE_EXTENSION_PARAMS_EVT ${SHIM_OCR_TYPE_DEFINE})
set_property(TARGET ocr_shim_event_payload PROPERTY POSITION_INDEPENDENT_CODE OFF)
target_compile_options(ocr_shim_event_payload PRIVATE ${ARTS_NON_PIE_COMPILE_OPTIONS})
target_link_options(ocr_shim_event_payload PRIVATE ${ARTS_NON_PIE_LINK_OPTIONS})
target_link_libraries(ocr_shim_event_payload PRIVATE arts_static)
if(ARTS_BUILD_TESTS)
    set_target_properties(ocr_shim_event_payload PROPERTIES EXCLUDE_FROM_ALL FALSE)
    register_single_node_test(ocr_shim_event_payload TIMEOUT 15)
    register_multinode_test(ocr_shim_event_payload TIMEOUT 30)
endif()

add_executable(ocr_shim_finish_release EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/shim/finish_release.c
    $<TARGET_OBJECTS:ocr_arts_objects>)
target_include_directories(ocr_shim_finish_release PRIVATE
    ${OCR_ROOT}/inc ${SHIM_OCR_OPTIONS_DIR})
target_compile_definitions(ocr_shim_finish_release PRIVATE
    ENABLE_EXTENSION_AFFINITY ${SHIM_OCR_TYPE_DEFINE})
set_property(TARGET ocr_shim_finish_release PROPERTY
    POSITION_INDEPENDENT_CODE OFF)
target_compile_options(ocr_shim_finish_release PRIVATE ${ARTS_NON_PIE_COMPILE_OPTIONS})
target_link_options(ocr_shim_finish_release PRIVATE ${ARTS_NON_PIE_LINK_OPTIONS})
target_link_libraries(ocr_shim_finish_release PRIVATE arts_static)
if(ARTS_BUILD_TESTS)
    set_target_properties(ocr_shim_finish_release PROPERTIES
        EXCLUDE_FROM_ALL FALSE)
    register_single_node_test(ocr_shim_finish_release TIMEOUT 60)
    register_multinode_test(ocr_shim_finish_release TIMEOUT 90)
    foreach(_t ocr_shim_finish_release ocr_shim_finish_release_2n_9w_1p
            ocr_shim_finish_release_4n_4w_1p
            ocr_shim_finish_release_2n_8w_2p)
        set_tests_properties(${_t} PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS: finish EDT output fired"
            FAIL_REGULAR_EXPRESSION "FAIL")
    endforeach()
endif()

# An EW dependence is refused with an abort, a death by a signal the runner
# cannot be told to expect, so a bash wrapper turns SIGABRT's status (134) into
# 0 and any other end into a failure; the refusal line is the pass regex.  No
# cell changes it, so it is built and registered in the reference tree only.
add_executable(ocr_shim_ew_refused EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/shim/ew_refused.c
    $<TARGET_OBJECTS:ocr_arts_objects>)
target_include_directories(ocr_shim_ew_refused PRIVATE
    ${OCR_ROOT}/inc ${SHIM_OCR_OPTIONS_DIR})
target_compile_definitions(ocr_shim_ew_refused PRIVATE ${SHIM_OCR_TYPE_DEFINE})
set_property(TARGET ocr_shim_ew_refused PROPERTY POSITION_INDEPENDENT_CODE OFF)
target_compile_options(ocr_shim_ew_refused PRIVATE ${ARTS_NON_PIE_COMPILE_OPTIONS})
target_link_options(ocr_shim_ew_refused PRIVATE ${ARTS_NON_PIE_LINK_OPTIONS})
target_link_libraries(ocr_shim_ew_refused PRIVATE arts_static)
if(ARTS_BUILD_TESTS AND ARTS_TEST_REFERENCE_TREE)
    set_target_properties(ocr_shim_ew_refused PROPERTIES EXCLUDE_FROM_ALL FALSE)
    arts_test_cfg(_cfg 1n_20w)
    arts_add_test(ocr_shim_ew_refused COMMAND bash -c
        "$<TARGET_FILE:ocr_shim_ew_refused> 2>&1\ntest $? -eq 134")
    set_tests_properties(ocr_shim_ew_refused PROPERTIES
        LABELS "single_node"
        TIMEOUT 15
        RESOURCE_LOCK "arts_runtime"
        ENVIRONMENT "ARTS_CONFIG=${_cfg}"
        PASS_REGULAR_EXPRESSION "\\[ARTS\\] unsupported OCR access mode DB_MODE_EW"
        FAIL_REGULAR_EXPRESSION "FAIL")
endif()

# A dependence that carries NULL_GUID in the default (read-write) mode: an
# event satisfied with NULL_GUID on a create-time slot, and an output event
# satisfied with no result.  Neither slot names a data block, so the consumer
# acquires and releases neither.  No cell changes it, so it is built and
# registered in the reference tree only.
add_executable(ocr_shim_null_guid_dep EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/shim/null_guid_dep.c
    $<TARGET_OBJECTS:ocr_arts_objects>)
target_include_directories(ocr_shim_null_guid_dep PRIVATE
    ${OCR_ROOT}/inc ${SHIM_OCR_OPTIONS_DIR})
target_compile_definitions(ocr_shim_null_guid_dep PRIVATE
    ${SHIM_OCR_TYPE_DEFINE})
set_property(TARGET ocr_shim_null_guid_dep PROPERTY
    POSITION_INDEPENDENT_CODE OFF)
target_compile_options(ocr_shim_null_guid_dep PRIVATE ${ARTS_NON_PIE_COMPILE_OPTIONS})
target_link_options(ocr_shim_null_guid_dep PRIVATE ${ARTS_NON_PIE_LINK_OPTIONS})
target_link_libraries(ocr_shim_null_guid_dep PRIVATE arts_static)
if(ARTS_BUILD_TESTS AND ARTS_TEST_REFERENCE_TREE)
    set_target_properties(ocr_shim_null_guid_dep PROPERTIES
        EXCLUDE_FROM_ALL FALSE)
    register_single_node_test(ocr_shim_null_guid_dep TIMEOUT 30)
    set_tests_properties(ocr_shim_null_guid_dep PROPERTIES
        PASS_REGULAR_EXPRESSION "PASS: NULL_GUID dependences in the default mode"
        FAIL_REGULAR_EXPRESSION "FAIL")
endif()
