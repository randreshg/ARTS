# Run by ctest, not compiled.  Two runs of one program write their counter
# files into one folder, which nothing clears between them: the first on
# RANKS ranks, the second on one, so rank 0's file then names the second run
# and every other rank's the first.  The reducer reads the first run's folder
# as one run of RANKS ranks, and refuses the mixed folder with exit status 2
# and "mixed run ids".  The reducer runs isolated (python -I <reducer>), so
# the caller's Python environment cannot change it.
#   cmake -DPROGRAM=<exe> -DRANKS=<n> -DCFG_RANKS=<cfg> -DCFG_ONE=<cfg>
#         -DPYTHON=<python> -DREDUCER=<reduce.py> -P record_foreign_run_id.cmake
# The folder is $ENV{counter_folder}.  Every output is echoed as it arrives.
foreach(_v PROGRAM RANKS CFG_RANKS CFG_ONE PYTHON REDUCER)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "${_v} is required")
    endif()
endforeach()
set(_folder "$ENV{counter_folder}")
if(_folder STREQUAL "")
    message(FATAL_ERROR "counter_folder is not set")
endif()
file(MAKE_DIRECTORY "${_folder}")

function(fail why)
    message(FATAL_ERROR "FAIL record_foreign_run_id: ${why}")
endfunction()

function(run_program cfg)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "ARTS_CONFIG=${cfg}" ${PROGRAM}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out ERROR_VARIABLE _out
        ECHO_OUTPUT_VARIABLE ECHO_ERROR_VARIABLE)
    if(NOT "${_rc}" STREQUAL "0")
        fail("the run on ${cfg} ended with \"${_rc}\", not exit status 0")
    endif()
endfunction()

function(reduce out_rc out_stdout out_stderr)
    execute_process(
        COMMAND ${PYTHON} -I ${REDUCER} "${_folder}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr
        ECHO_OUTPUT_VARIABLE ECHO_ERROR_VARIABLE)
    set(${out_rc} "${_rc}" PARENT_SCOPE)
    set(${out_stdout} "${_stdout}" PARENT_SCOPE)
    set(${out_stderr} "${_stderr}" PARENT_SCOPE)
endfunction()

run_program("${CFG_RANKS}")
reduce(_rc _stdout _stderr)
if(NOT "${_rc}" STREQUAL "0")
    fail("the reducer refused one run's folder (\"${_rc}\")")
endif()
string(JSON _ranks ERROR_VARIABLE _err LENGTH "${_stdout}" ranks)
string(JSON _missing ERROR_VARIABLE _err2 LENGTH "${_stdout}" missing)
if(_err OR _err2 OR NOT _ranks EQUAL RANKS OR NOT _missing EQUAL 0)
    fail("one run's folder did not reduce to ${RANKS} ranks with none missing")
endif()

run_program("${CFG_ONE}")
reduce(_rc _stdout _stderr)
if(NOT "${_rc}" STREQUAL "2" OR NOT _stderr MATCHES "mixed run ids")
    fail("the mixed folder was not refused with exit status 2 and "
         "\"mixed run ids\" (status \"${_rc}\")")
endif()

message("PASS record_foreign_run_id")
