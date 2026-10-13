# Runs one test program and judges it by how it ends:
#   cmake [-DSTATUS=<n>] -P run_test.cmake -- <program> [args...]
# The program must exit with status STATUS (0 unless given); execute_process
# reports a death by a signal as text, never as a number, so no signal passes
# for an exit status.  When ARTS_TEST_PASS_REGEX is non-empty, the program's
# output must also match it.  CTest ignores the exit status of a test it gives
# a pass regex, which is why the regex is checked here; it is taken out of the
# environment before the program starts, which therefore sees what a bare run
# sees.  The output is echoed as it arrives, so a test that times out still
# leaves it in the log.  An argument is passed as one list element: it cannot
# hold a ';', and an empty one is dropped.
set(_cmd "")
set(_seen FALSE)
math(EXPR _last "${CMAKE_ARGC} - 1")
foreach(_i RANGE 0 ${_last})
    if(_seen)
        list(APPEND _cmd "${CMAKE_ARGV${_i}}")
    elseif(CMAKE_ARGV${_i} STREQUAL "--")
        set(_seen TRUE)
    endif()
endforeach()
if(NOT _cmd)
    message(FATAL_ERROR "run_test.cmake: no program after --")
endif()
if(NOT DEFINED STATUS)
    set(STATUS 0)
endif()
set(_pass "$ENV{ARTS_TEST_PASS_REGEX}")
unset(ENV{ARTS_TEST_PASS_REGEX})

execute_process(COMMAND ${_cmd}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out ERROR_VARIABLE _out
    ECHO_OUTPUT_VARIABLE ECHO_ERROR_VARIABLE)

if(NOT "${_rc}" STREQUAL "${STATUS}")
    message(FATAL_ERROR "run_test: the program ended with \"${_rc}\", not exit status ${STATUS}")
endif()
if(NOT _pass STREQUAL "" AND NOT _out MATCHES "${_pass}")
    message(FATAL_ERROR "run_test: no output matched the pass regex ${_pass}")
endif()
