# OCR-to-ARTS compatibility shim
# Implements OCR API functions using ARTS v2 primitives
# Output: ocr_arts_objects — apps combine these objects with the coherence
# variant they link (arts_static_<suffix>), so no library target exists here

set(OCR_ROOT "${CMAKE_SOURCE_DIR}/third_party/ocr/ocr")

# Added from the top level, so this directory states its own compile setting.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(SHIM_C_OPT -O0 -g)
else()
    set(SHIM_C_OPT -O3)
endif()
# Defining OCR_TYPE_H makes ocr.h include "ocr-options_<value>.h"; the one it
# names here is a generated no-op, which silences ocr.h's pragma note.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ocr-options_arts.h"
     "/* No-op OCR options for ARTS builds. */\n")
set(SHIM_OCR_TYPE_DEFINE "OCR_TYPE_H=arts.h")
set(SHIM_OCR_OPTIONS_DIR "${CMAKE_CURRENT_BINARY_DIR}")

add_library(ocr_arts_objects OBJECT arts_ocr.c)

# The shim file uses both OCR headers and ARTS internal headers.
# C_EXTENSIONS ON because the shim uses VLAs, __sync builtins, typeof, etc.
set_target_properties(ocr_arts_objects PROPERTIES
    C_STANDARD 11
    C_EXTENSIONS ON
)

target_compile_options(ocr_arts_objects PRIVATE
    ${SHIM_C_OPT}
    -Wall
    ${ARTS_WNO_UNUSED}
)

# OCR extension defines (must come before OCR headers)
target_compile_definitions(ocr_arts_objects PRIVATE
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

# No-hint DB home policy selector (see top-level ARTS_SHIM_NOHINT_DB_HOME).
target_compile_definitions(ocr_arts_objects PRIVATE
    ARTS_SHIM_NOHINT_DB_HOME_ROUNDROBIN=${_ARTS_SHIM_NOHINT_RR})

# No-hint EDT placement policy selector (see top-level ARTS_SHIM_NOHINT_EDT_PLACE).
target_compile_definitions(ocr_arts_objects PRIVATE
    ARTS_SHIM_NOHINT_EDT_PLACE_ROUNDROBIN=${_ARTS_SHIM_NOHINT_EDT_RR})

# Include: OCR public headers + extension headers
target_include_directories(ocr_arts_objects PRIVATE
    ${OCR_ROOT}/inc                # ocr.h, ocr-types.h, etc.
    ${SHIM_OCR_OPTIONS_DIR}       # generated ocr-options_arts.h
)

# Include: ARTS public + internal headers (shim needs route table, runtime_types)
target_include_directories(ocr_arts_objects PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/include/public
    ${CMAKE_SOURCE_DIR}/libs/include/internal
)

add_executable(ocr_shim_event_payload EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/shim/event_payload.c
    $<TARGET_OBJECTS:ocr_arts_objects>)
target_include_directories(ocr_shim_event_payload PRIVATE
    ${OCR_ROOT}/inc ${SHIM_OCR_OPTIONS_DIR})
target_compile_definitions(ocr_shim_event_payload PRIVATE
    ENABLE_EXTENSION_AFFINITY ENABLE_EXTENSION_PARAMS_EVT ${SHIM_OCR_TYPE_DEFINE})
set_property(TARGET ocr_shim_event_payload PROPERTY POSITION_INDEPENDENT_CODE OFF)
target_compile_options(ocr_shim_event_payload PRIVATE -fno-pie -fno-PIE)
target_link_options(ocr_shim_event_payload PRIVATE -no-pie -fno-pie -fno-PIE)
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
target_compile_options(ocr_shim_finish_release PRIVATE -fno-pie -fno-PIE)
target_link_options(ocr_shim_finish_release PRIVATE -no-pie -fno-pie -fno-PIE)
target_link_libraries(ocr_shim_finish_release PRIVATE arts_static)
if(ARTS_BUILD_TESTS)
    set_target_properties(ocr_shim_finish_release PROPERTIES
        EXCLUDE_FROM_ALL FALSE)
    register_single_node_test(ocr_shim_finish_release TIMEOUT 60)
    register_multinode_test(ocr_shim_finish_release TIMEOUT 90)
    foreach(_t ocr_shim_finish_release ocr_shim_finish_release_2n
            ocr_shim_finish_release_3n ocr_shim_finish_release_4n
            ocr_shim_finish_release_2n_io)
        set_tests_properties(${_t} PROPERTIES
            PASS_REGULAR_EXPRESSION "PASS: finish EDT output fired"
            FAIL_REGULAR_EXPRESSION "FAIL")
    endforeach()
    arts_fam_device_test_env()
endif()
