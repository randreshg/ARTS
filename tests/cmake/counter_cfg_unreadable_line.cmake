# Run by ctest, not compiled.  The counter configuration parser
# (cmake/ArtsCounterConfig.cmake) reads each line whole and on its own, or
# refuses it: a line it cannot read, such as one naming a level it does not
# know, is a configure error naming that line, never a counter taken from the
# line's readable prefix; a well-formed line between a comment and a blank
# line parses; and a comment holding '[', ']', ';' or a trailing '\' neither
# joins the lines after it nor splits into lines of its own.  Each
# configuration is parsed in a child cmake -P, so a refusal ends only the
# child.
#   cmake -DSOURCE_DIR=<source> -DWORK_DIR=<dir>
#         -P counter_cfg_unreadable_line.cmake
foreach(_v SOURCE_DIR WORK_DIR)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "${_v} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(WRITE "${WORK_DIR}/parse.cmake" [=[
include("${MODULE}")
arts_parse_counter_config("${CFG}")
set(_out "parsed")
foreach(_c IN LISTS COUNTER_CONFIG_NAMES)
    string(APPEND _out " ${_c}=${ENABLE_${_c}},${COUNTER_MODE_${_c}},"
           "${COUNTER_LEVEL_${_c}},${REDUCE_METHOD_${_c}}")
endforeach()
message("${_out} ENABLED=${ENABLED_COUNT} DISABLED=${DISABLED_COUNT}")
]=])

function(fail why)
    message(FATAL_ERROR "FAIL counter_cfg_unreadable_line: ${why}")
endfunction()

function(parse_cfg name content out_rc out_text)
    file(WRITE "${WORK_DIR}/${name}.cfg" "${content}")
    execute_process(
        COMMAND ${CMAKE_COMMAND}
                "-DMODULE=${SOURCE_DIR}/cmake/ArtsCounterConfig.cmake"
                "-DCFG=${WORK_DIR}/${name}.cfg"
                -P "${WORK_DIR}/parse.cmake"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _text ERROR_VARIABLE _text)
    message("${name}: status ${_rc}\n${_text}")
    set(${out_rc} "${_rc}" PARENT_SCOPE)
    set(${out_text} "${_text}" PARENT_SCOPE)
endfunction()

function(expect_refused name line)
    parse_cfg(${name} "${line}\n" _rc _text)
    string(FIND "${_text}" "${line}" _named)
    if(_rc EQUAL 0 OR _named EQUAL -1 OR _text MATCHES "parsed ")
        fail("the line '${line}' was not refused by a configure error "
             "naming it")
    endif()
endfunction()

function(expect_parsed name content want)
    parse_cfg(${name} "${content}" _rc _text)
    string(FIND "${_text}" "${want}\n" _parsed)
    if(NOT _rc EQUAL 0 OR _parsed EQUAL -1)
        fail("the configuration '${name}' did not parse to '${want}'")
    endif()
endfunction()

expect_refused(unreadable "NUM_EDT_CREATE=ONCE,CLUSTER")
expect_refused(unreadable_list_chars "NUM_EDT_CREATE=ONCE,NODE;[x]")

expect_parsed(readable "# a comment\n\nNUM_EDT_CREATE=ONCE,NODE\n"
              "parsed NUM_EDT_CREATE=1,1,1,0 ENABLED=1 DISABLED=0")

string(CONCAT _cfg "# wait [ms\n" "NUM_EDT_CREATE=ONCE,NODE\n" "# x ] y\n"
       "# one; two \\\n" "NUM_EDT_FINISH=PERIODIC,NODE,MAX\n" "# [\n")
string(CONCAT _want "parsed NUM_EDT_CREATE=1,1,1,0 NUM_EDT_FINISH=1,2,1,1"
       " ENABLED=2 DISABLED=0")
expect_parsed(comments "${_cfg}" "${_want}")

message("PASS counter_cfg_unreadable_line")
