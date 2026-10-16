# OCR-to-ARTS compatibility shim
# Implements OCR API functions using ARTS v2 primitives
# Output: object libraries only — apps combine the shim's objects with the
# coherence variant they link (arts_static_<suffix>)

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

# The shim reads ARTS internal headers, whose layouts follow the coherence
# cell, so its objects are compiled once per runtime they are linked with:
# ocr_arts_objects for this tree's own library (the shim's tests), and
# ocr_arts_objects_<suffix> for each benchmark variant.
function(arts_ocr_shim_objects target)
    add_library(${target} OBJECT arts_ocr.c)

    set_target_properties(${target} PROPERTIES
        C_STANDARD 11
    )

    target_compile_options(${target} PRIVATE
        ${SHIM_C_OPT}
        -Wall
        ${ARTS_WNO_UNUSED}
    )

    # OCR extension defines (must come before OCR headers)
    target_compile_definitions(${target} PRIVATE
        ENABLE_EXTENSION_AFFINITY
        ENABLE_EXTENSION_PARAMS_EVT
        ENABLE_EXTENSION_MULTI_OUTPUT_SLOT
        ENABLE_EXTENSION_COLLECTIVE_EVT
        ENABLE_EXTENSION_LABELING
        ENABLE_EXTENSION_RTITF
        ENABLE_EXTENSION_CHANNEL_EVT
        ENABLE_EXTENSION_COUNTED_EVT
        ${SHIM_OCR_TYPE_DEFINE}
    )

    target_include_directories(${target} PRIVATE
        ${OCR_ROOT}/inc                # ocr.h, ocr-types.h, etc.
        ${SHIM_OCR_OPTIONS_DIR}        # generated ocr-options_arts.h
        ${CMAKE_SOURCE_DIR}/libs/include/public
        ${CMAKE_SOURCE_DIR}/libs/include/internal
    )
endfunction()

arts_ocr_shim_objects(ocr_arts_objects)
arts_apply_protocol(ocr_arts_objects
    ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY})
if(NOT ARTS_BUILD_TESTS)
    set_target_properties(ocr_arts_objects PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
get_property(_arts_variants GLOBAL PROPERTY ARTS_VARIANTS)
foreach(_sfx IN LISTS _arts_variants)
    arts_ocr_shim_objects(ocr_arts_objects_${_sfx})
    arts_apply_variant_protocol(ocr_arts_objects_${_sfx} ${_sfx})
endforeach()

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
