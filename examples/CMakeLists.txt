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

# The native ARTS examples: programs on the public API alone.  Every tree
# builds them for its own cell, and a benchmark tree also builds them on every
# ARTS variant, as its examples section.

add_compile_options(${ARTS_WERROR_IMPLICIT_SEQ_CST})

# One directory for every example binary, whichever subdirectory registers it.
set(ARTS_EXAMPLES_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# The settings every example executable takes: non-PIE, like every program
# that runs on ARTS, and the shared binary directory.  No coherence layout is
# applied: an example sees only the public headers.
function(arts_example_executable target runtime)
    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE OFF
        RUNTIME_OUTPUT_DIRECTORY "${ARTS_EXAMPLES_BINARY_DIR}")
    target_compile_options(${target} PRIVATE ${ARTS_NON_PIE_COMPILE_OPTIONS})
    target_link_options(${target} PRIVATE ${ARTS_NON_PIE_LINK_OPTIONS})
    target_link_libraries(${target} PRIVATE ${runtime})
endfunction()

# arts_example(NAME <stem> SOURCES <source>... PASS_REGEX <regex>
#              [ARGS_SMOKE <arg>...] [PASS_REGEX_SINGLE <regex>]
#              [SMOKE_DEFINES <definition>...] [BLOCK_FREE]
#              [TIMEOUT_SINGLE <s>] [TIMEOUT_MULTI <s>]
#              [TSAN_TIMEOUT_SINGLE <s>] [TSAN_TIMEOUT_MULTI <s>])
#
# Builds <stem>_example against this tree's runtime and, in a benchmark tree,
# <stem>_example_arts_<suffix> against each variant.  With ARTS_BUILD_TESTS it
# registers the smoke test, single-node and multinode, labelled examples, by
# the rule of tests/: a BLOCK_FREE program, one no coherence cell or payload
# store can change, registers in the reference tree only, every other one in
# every tree.  The test runs <stem>_example with ARGS_SMOKE or, given
# SMOKE_DEFINES, a smaller instance of the same program, <stem>_example_smoke,
# compiled with those definitions.  Its output must match PASS_REGEX;
# PASS_REGEX_SINGLE, when given, replaces it for the single-node test.  The
# timeouts default to 10 s single-node and 60 s multinode, and a
# ThreadSanitizer tree takes the TSAN_TIMEOUT_* budgets, by default three times
# the plain ones.
function(arts_example)
    cmake_parse_arguments(EX "BLOCK_FREE"
        "NAME;PASS_REGEX;PASS_REGEX_SINGLE;TIMEOUT_SINGLE;TIMEOUT_MULTI;TSAN_TIMEOUT_SINGLE;TSAN_TIMEOUT_MULTI"
        "SOURCES;ARGS_SMOKE;SMOKE_DEFINES" ${ARGN})
    if(EX_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "arts_example(${EX_NAME}): unexpected arguments '${EX_UNPARSED_ARGUMENTS}'")
    endif()
    foreach(_required NAME SOURCES PASS_REGEX)
        if(NOT EX_${_required})
            message(FATAL_ERROR "arts_example(${EX_NAME}): give its ${_required}")
        endif()
    endforeach()

    set(_sources "")
    foreach(_s IN LISTS EX_SOURCES)
        get_filename_component(_abs "${_s}" ABSOLUTE)
        list(APPEND _sources "${_abs}")
    endforeach()
    set(_compiled ${_sources})
    list(FILTER _compiled INCLUDE REGEX "\\.(c|cc|cpp|cxx)$")
    set_property(GLOBAL APPEND PROPERTY ARTS_EXAMPLE_SOURCES ${_compiled})

    set(_target ${EX_NAME}_example)
    add_executable(${_target} ${_sources})
    arts_example_executable(${_target} arts_static)
    if(ARTS_BUILD_BENCHMARKS)
        get_property(_variants GLOBAL PROPERTY ARTS_VARIANTS)
        foreach(_sfx IN LISTS _variants)
            add_executable(${_target}_arts_${_sfx} ${_sources})
            arts_example_executable(${_target}_arts_${_sfx} arts_static_${_sfx})
        endforeach()
    endif()

    if(NOT ARTS_BUILD_TESTS OR (EX_BLOCK_FREE AND NOT ARTS_TEST_REFERENCE_TREE))
        return()
    endif()
    set(_registered ${_target})
    if(EX_SMOKE_DEFINES)
        set(_registered ${_target}_smoke)
        add_executable(${_registered} ${_sources})
        arts_example_executable(${_registered} arts_static)
        target_compile_definitions(${_registered} PRIVATE ${EX_SMOKE_DEFINES})
    endif()

    if(NOT EX_TIMEOUT_SINGLE)
        set(EX_TIMEOUT_SINGLE 10)
    endif()
    if(NOT EX_TIMEOUT_MULTI)
        set(EX_TIMEOUT_MULTI 60)
    endif()
    if(ARTS_DEBUG_SANITIZER STREQUAL "THREAD")
        if(EX_TSAN_TIMEOUT_SINGLE)
            set(EX_TIMEOUT_SINGLE ${EX_TSAN_TIMEOUT_SINGLE})
        else()
            math(EXPR EX_TIMEOUT_SINGLE "${EX_TIMEOUT_SINGLE} * 3")
        endif()
        if(EX_TSAN_TIMEOUT_MULTI)
            set(EX_TIMEOUT_MULTI ${EX_TSAN_TIMEOUT_MULTI})
        else()
            math(EXPR EX_TIMEOUT_MULTI "${EX_TIMEOUT_MULTI} * 3")
        endif()
    endif()
    if(NOT EX_PASS_REGEX_SINGLE)
        set(EX_PASS_REGEX_SINGLE "${EX_PASS_REGEX}")
    endif()

    register_single_node_test(${_registered} TIMEOUT ${EX_TIMEOUT_SINGLE}
                              ARGS ${EX_ARGS_SMOKE})
    register_multinode_test(${_registered} TIMEOUT ${EX_TIMEOUT_MULTI}
                            ARGS ${EX_ARGS_SMOKE})
    set_tests_properties(${_registered} PROPERTIES
        PASS_REGULAR_EXPRESSION "${EX_PASS_REGEX_SINGLE}")
    set(_tests ${_registered})
    foreach(_v IN LISTS ARTS_TEST_MULTINODE_VARIANTS)
        set_tests_properties(${_registered}_${_v} PROPERTIES
            PASS_REGULAR_EXPRESSION "${EX_PASS_REGEX}")
        list(APPEND _tests ${_registered}_${_v})
    endforeach()
    set_property(TEST ${_tests} APPEND PROPERTY LABELS examples)
endfunction()

add_subdirectory(basics)
add_subdirectory(dense)
add_subdirectory(graph)
add_subdirectory(stream)
add_subdirectory(random_access)
add_subdirectory(proxy)

# An example includes the public headers alone.  Linking the runtime cannot
# show it, since the runtime's targets export the internal include
# directories too, so every example source is compiled once more with only
# the public ones, in the tree's language modes and with its feature-test
# macros.  No cell can change the outcome: the reference tree runs it.  The
# source list is read here, so every example subdirectory is added above.
if(ARTS_BUILD_TESTS AND ARTS_TEST_REFERENCE_TREE)
    get_property(_sources GLOBAL PROPERTY ARTS_EXAMPLE_SOURCES)
    set(_c_std "${CMAKE_C${CMAKE_C_STANDARD}_STANDARD_COMPILE_OPTION}")
    if(CMAKE_C_EXTENSIONS)
        set(_c_std "${CMAKE_C${CMAKE_C_STANDARD}_EXTENSION_COMPILE_OPTION}")
    endif()
    set(_cxx_std "${CMAKE_CXX${CMAKE_CXX_STANDARD}_STANDARD_COMPILE_OPTION}")
    if(CMAKE_CXX_EXTENSIONS)
        set(_cxx_std "${CMAKE_CXX${CMAKE_CXX_STANDARD}_EXTENSION_COMPILE_OPTION}")
    endif()
    set(_macros "")
    foreach(_m IN LISTS ARTS_FEATURE_TEST_MACROS)
        list(APPEND _macros "-D${_m}")
    endforeach()
    set(_inputs "${CMAKE_CURRENT_BINARY_DIR}/examples_public_headers.inputs.cmake")
    arts_write_if_changed("${_inputs}"
        "set(C_COMPILER [[${CMAKE_C_COMPILER}]])
set(CXX_COMPILER [[${CMAKE_CXX_COMPILER}]])
set(C_FLAGS [[${_c_std};${_macros}]])
set(CXX_FLAGS [[${_cxx_std};${_macros}]])
set(INCLUDE_DIR [[${CMAKE_SOURCE_DIR}/libs/include/public]])
set(SOURCES [[${_sources}]])
")
    add_test(NAME examples_public_headers
             COMMAND ${CMAKE_COMMAND} -DINPUTS=${_inputs}
                     -P ${CMAKE_SOURCE_DIR}/tests/cmake/examples_public_headers.cmake)
    set_tests_properties(examples_public_headers PROPERTIES
        LABELS "single_node;examples"
        TIMEOUT 120
        PASS_REGULAR_EXPRESSION "PASS examples_public_headers"
        FAIL_REGULAR_EXPRESSION "FAIL examples_public_headers")
endif()
