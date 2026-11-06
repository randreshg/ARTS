# Runs a program that must end on LeakSanitizer's report:
#   cmake -P expect_lsan_report.cmake -- <program> [args...]
# The program's output is captured and not echoed: every test's fail list
# matches a sanitizer report, which this test expects.  Its exit status must
# be 1 (ASan's exitcode, which LeakSanitizer ends a leak check with) and its
# output must hold LeakSanitizer's header; the script then prints
# "PASS lsan_check_runs".  Like every driver, it makes the counter folder
# ($ENV{counter_folder}, parents included) before the program starts.
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
    message(FATAL_ERROR "expect_lsan_report: no program after --")
endif()
if(NOT "$ENV{counter_folder}" STREQUAL "")
    file(MAKE_DIRECTORY "$ENV{counter_folder}")
endif()
execute_process(COMMAND ${_cmd}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
if(NOT "${_rc}" STREQUAL "1")
    message(FATAL_ERROR "expect_lsan_report: the program ended with \"${_rc}\", "
            "not exit status 1")
endif()
if(NOT _out MATCHES "LeakSanitizer: detected memory leaks")
    message(FATAL_ERROR "expect_lsan_report: no LeakSanitizer report")
endif()
message("PASS lsan_check_runs")
