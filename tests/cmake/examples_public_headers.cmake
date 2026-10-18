# Run by ctest, not compiled.  An example is a program on the public API
# alone, so each of its sources compiles with libs/include/public as the only
# project include directory, in the tree's own language modes and with its
# feature-test macros.  INPUTS names the file that sets the compilers, their
# flags, that directory and the sources.
if(NOT DEFINED INPUTS OR NOT EXISTS "${INPUTS}")
    message(FATAL_ERROR "FAIL examples_public_headers: no inputs file '${INPUTS}'")
endif()
include("${INPUTS}")
foreach(_v C_COMPILER CXX_COMPILER C_FLAGS CXX_FLAGS INCLUDE_DIR SOURCES)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "FAIL examples_public_headers: ${INPUTS} sets no ${_v}")
    endif()
endforeach()

set(_failed "")
set(_n 0)
foreach(_src IN LISTS SOURCES)
    if(_src MATCHES "\\.c$")
        set(_compile "${C_COMPILER}" ${C_FLAGS})
    else()
        set(_compile "${CXX_COMPILER}" ${CXX_FLAGS})
    endif()
    execute_process(
        COMMAND ${_compile} "-I${INCLUDE_DIR}" -fsyntax-only "${_src}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    math(EXPR _n "${_n} + 1")
    if(NOT _rc EQUAL 0)
        list(APPEND _failed "${_src}")
        message("${_src}:\n${_out}")
    endif()
endforeach()
if(_failed)
    list(JOIN _failed "\n  " _failed)
    message(FATAL_ERROR "FAIL examples_public_headers: these sources need more "
            "than the public headers:\n  ${_failed}")
endif()
message(STATUS "PASS examples_public_headers: ${_n} sources")
