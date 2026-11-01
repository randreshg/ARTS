# Differential oracle on the route table's probes and pins per unit of a program
# shape: runs route_budget at N and at 2N units, each with its own counter
# folder, and requires rank RANK's NUM_ROUTE_PROBE and NUM_ROUTE_PIN to grow by
# exactly N times the shape's per-unit budget.  What a run does once (the main
# EDT, startup and shutdown) cancels in the difference.
#   cmake -DPROGRAM=<exe> -DSHAPE=<s> -DN=<n> -DRANK=<r> -DPROBES_PER_UNIT=<p>
#         -DPINS_PER_UNIT=<q> -P route_budget_diff.cmake
# The runtime takes ARTS_CONFIG from the environment; the counter files are
# read from the current directory, which is where the tests run.
foreach(_v PROGRAM SHAPE N RANK PROBES_PER_UNIT PINS_PER_UNIT)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "route_budget_diff: -D${_v}= is required")
    endif()
endforeach()

math(EXPR _n2 "2 * ${N}")
set(_counters "")
foreach(_units ${N} ${_n2})
    set(_dir "route_budget_${SHAPE}_${_units}")
    file(REMOVE_RECURSE "${_dir}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "counter_folder=./${_dir}"
                ${PROGRAM} ${SHAPE} ${_units}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out ERROR_VARIABLE _out
        ECHO_OUTPUT_VARIABLE ECHO_ERROR_VARIABLE)
    if(NOT "${_rc}" STREQUAL "0")
        message(FATAL_ERROR "route_budget_diff: ${SHAPE} at ${_units} units "
                "ended with \"${_rc}\", not exit status 0")
    endif()
    if(NOT _out MATCHES "PASS: route_budget ${SHAPE} ${_units}")
        message(FATAL_ERROR "route_budget_diff: ${SHAPE} at ${_units} units "
                "printed no PASS line")
    endif()
    set(_file "${_dir}/n${RANK}.json")
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "route_budget_diff: ${_file} was not written")
    endif()
    file(READ "${_file}" _json)
    foreach(_c NUM_ROUTE_PROBE NUM_ROUTE_PIN)
        string(JSON _val ERROR_VARIABLE _err GET "${_json}" counters ${_c}
               value)
        if(_err)
            message(FATAL_ERROR
                    "route_budget_diff: ${_c} is not in ${_file}: ${_err}")
        endif()
        set(${_c}_${_units} ${_val})
    endforeach()
    # Every judged rank probes in every shape (rank 0 creates the main EDT;
    # a shape judged on another rank probes per unit there), so a run with
    # no probe counted has lost the counter, and a 0 / 0 budget would
    # otherwise pass vacuously.
    if(NUM_ROUTE_PROBE_${_units} LESS 1)
        message(FATAL_ERROR "route_budget_diff: ${SHAPE} at ${_units} units "
                "counted NUM_ROUTE_PROBE ${NUM_ROUTE_PROBE_${_units}} on rank "
                "${RANK}, expected at least 1")
    endif()
endforeach()

set(_bad "")
set(_names NUM_ROUTE_PROBE NUM_ROUTE_PIN)
set(_pers ${PROBES_PER_UNIT} ${PINS_PER_UNIT})
foreach(_name _per IN ZIP_LISTS _names _pers)
    math(EXPR _want "${N} * ${_per}")
    math(EXPR _got "${${_name}_${_n2}} - ${${_name}_${N}}")
    message(STATUS "${_name}: ${${_name}_${N}} at ${N}, ${${_name}_${_n2}} at "
            "${_n2}; difference ${_got}, expected ${_want}")
    if(NOT _got EQUAL _want)
        string(APPEND _bad " ${_name} difference ${_got}, expected ${_want};")
    endif()
endforeach()
if(_bad)
    message(FATAL_ERROR "route_budget_diff: ${SHAPE} (rank ${RANK}):${_bad}")
endif()
message("PASS: route_budget_diff ${SHAPE}")
