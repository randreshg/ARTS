# Run by ctest, not compiled:
#   cmake -P check_skip_in_fail_lists.cmake <tests dir>/CTestTestfile.cmake
# Reads every test's fail list in a tree's generated test file and requires:
#   - a list that holds the walk's bare marker (a quiescent shutdown, whose
#     every marker is a failure) also holds the bare skip line, since a
#     teardown that skipped the walk would otherwise pass it;
#   - a list that holds the marker's non-quiescent form (a shutdown that leaves
#     work undone, whose count line is legal) holds no bare skip line, since a
#     thread still running is legal there and its list names the skip forms
#     that are defects.
# An alternative of one element counts as an element of its own.
if(NOT CMAKE_ARGC EQUAL 4)
    message(FATAL_ERROR "FAIL quiescence_tests_fail_on_skip: usage: cmake -P "
            "check_skip_in_fail_lists.cmake <CTestTestfile.cmake>")
endif()
set(_file "${CMAKE_ARGV3}")
if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "FAIL quiescence_tests_fail_on_skip: no file ${_file}")
endif()
file(READ "${_file}" _text)

# The file is text to CMake's list syntax here: its ';' and brackets stop being
# separators and quotes, and a statement becomes one list item.  A statement
# may span lines, since a test's regex may hold a newline.
string(REPLACE ";" "<sc>" _text "${_text}")
string(REPLACE "[" "<lb>" _text "${_text}")
string(REPLACE "]" "<rb>" _text "${_text}")
string(REPLACE "\nset_tests_properties(" "<stmt>set_tests_properties("
       _text "${_text}")
string(REPLACE "\nadd_test(" "<stmt>add_test(" _text "${_text}")
string(REPLACE "\n" "<nl>" _text "${_text}")
string(REPLACE "<stmt>" ";" _stmts "${_text}")

set(_head "^set_tests_properties\\(<lb>=<lb>([^<]*)<rb>=<rb> PROPERTIES")
set(_list "FAIL_REGULAR_EXPRESSION \"(([^\"\\]|\\\\.)*)\"")
set(_nonquiescent "QUIESCENCE-DEBUG: (<lb>1-9<rb>|<lb>^0-9 <rb>)")
set(_edge "(^|<sc>|\\|)")
set(_end "(<sc>|\\||$)")
set(_quiescent 0)
set(_other 0)
set(_bad "")
foreach(_stmt IN LISTS _stmts)
    if(NOT _stmt MATCHES "${_head}")
        continue()
    endif()
    set(_name "${CMAKE_MATCH_1}")
    # The directory's finalize gives every test a fail list.
    if(NOT _stmt MATCHES "${_list}")
        message(FATAL_ERROR "FAIL quiescence_tests_fail_on_skip: ${_name} has "
                "no fail list in ${_file}")
    endif()
    set(_fail "${CMAKE_MATCH_1}")
    set(_skip FALSE)
    if(_fail MATCHES "${_edge}teardown skipped:${_end}")
        set(_skip TRUE)
    endif()
    if(_fail MATCHES "${_edge}QUIESCENCE-DEBUG:?${_end}")
        math(EXPR _quiescent "${_quiescent} + 1")
        if(NOT _skip)
            list(APPEND _bad "${_name} (quiescent, no skip line)")
        endif()
    endif()
    string(FIND "${_fail}" "${_nonquiescent}" _at)
    if(NOT _at EQUAL -1)
        math(EXPR _other "${_other} + 1")
        if(_skip)
            list(APPEND _bad "${_name} (non-quiescent, a bare skip line)")
        endif()
    endif()
endforeach()

if(_bad)
    list(JOIN _bad ", " _bad)
    message(FATAL_ERROR "FAIL quiescence_tests_fail_on_skip: ${_bad}")
endif()
if(_quiescent EQUAL 0)
    message(FATAL_ERROR "FAIL quiescence_tests_fail_on_skip: no test holds the "
            "bare marker in ${_file}")
endif()
message("PASS quiescence_tests_fail_on_skip: ${_quiescent} quiescent, "
        "${_other} non-quiescent")
