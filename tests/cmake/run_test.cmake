# Runs one test program and judges it by how it ends:
#   cmake [-DSTATUS=<n>] [-DRECORDS=<n>]
#         [-DRECORD_CHECK=<script> -DRECORD_ARGC=<k> -DRECORD_ARG<i>=<arg>...]
#         -P run_test.cmake -- <program> [args...]
# The program must exit with status STATUS (0 unless given); execute_process
# reports a death by a signal as text, never as a number, so no signal passes
# for an exit status.  When ARTS_TEST_PASS_REGEX is non-empty, the program's
# output must also match it.  CTest ignores the exit status of a test it gives
# a pass regex, which is why the regex is checked here; it is taken out of the
# environment before the program starts, which therefore sees what a bare run
# sees.  The output is echoed as it arrives, so a test that times out still
# leaves it in the log.  An argument is passed as one list element: it cannot
# hold a ';', and an empty one is dropped.
#
# Before the program starts, the counter folder ($ENV{counter_folder}) is
# made, parents included, and the rank files (n*.json) an earlier run left
# there are removed, so none stands in for this run's.  Once the program has
# ended as required, RECORDS requires n0.json ... n<RECORDS-1>.json there,
# all naming one non-empty run, and RECORD_CHECK runs
#   cmake -DRECORD_DIR=<folder> -P <script> -- <arg0> ... <arg(k-1)>
# whose failure fails the test.
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

set(_folder "$ENV{counter_folder}")
if((DEFINED RECORDS OR DEFINED RECORD_CHECK) AND _folder STREQUAL "")
    message(FATAL_ERROR "run_test: RECORDS and RECORD_CHECK read the counter "
            "folder, and counter_folder is not set")
endif()
if(NOT _folder STREQUAL "")
    file(GLOB _stale "${_folder}/n*.json")
    if(_stale)
        file(REMOVE ${_stale})
    endif()
    file(MAKE_DIRECTORY "${_folder}")
endif()

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

if(DEFINED RECORDS)
    if(NOT RECORDS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
                "run_test: RECORDS is a rank count, not '${RECORDS}'")
    endif()
    set(_run "")
    math(EXPR _top "${RECORDS} - 1")
    foreach(_r RANGE 0 ${_top})
        set(_file "${_folder}/n${_r}.json")
        if(NOT EXISTS "${_file}")
            message(FATAL_ERROR "run_test: rank ${_r} wrote no ${_file}")
        endif()
        file(READ "${_file}" _json)
        string(JSON _id ERROR_VARIABLE _err GET "${_json}" metadata run)
        if(_err OR _id STREQUAL "")
            message(FATAL_ERROR "run_test: ${_file} names no run")
        endif()
        if(_run STREQUAL "")
            set(_run "${_id}")
        elseif(NOT _id STREQUAL _run)
            message(FATAL_ERROR "run_test: ${_file} names run ${_id}, rank 0's "
                    "names ${_run}")
        endif()
    endforeach()
    message("run_test: ${RECORDS} record(s) of run ${_run}")
endif()

if(DEFINED RECORD_CHECK)
    set(_args "")
    if(DEFINED RECORD_ARGC AND RECORD_ARGC GREATER 0)
        math(EXPR _top "${RECORD_ARGC} - 1")
        foreach(_i RANGE 0 ${_top})
            list(APPEND _args "${RECORD_ARG${_i}}")
        endforeach()
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} "-DRECORD_DIR=${_folder}" -P "${RECORD_CHECK}"
                -- ${_args}
        RESULT_VARIABLE _check_rc)
    if(NOT "${_check_rc}" STREQUAL "0")
        message(FATAL_ERROR "run_test: the record check ${RECORD_CHECK} failed")
    endif()
endif()
