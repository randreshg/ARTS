# arts_parse_counter_config(<cfg>)
#
# Reads a counter configuration, one COUNTER=MODE[,LEVEL[,REDUCE]] per line:
# MODE is OFF, ONCE or PERIODIC; LEVEL is THREAD or NODE (NODE when omitted);
# REDUCE is SUM, MAX, MIN or MASTER (SUM when omitted).  A blank line, or one
# whose first non-blank character is '#', is skipped; any other line is a
# configure error naming it.  For every counter named, sets in the caller's
# scope ENABLE_<COUNTER> (0 or 1), COUNTER_MODE_<COUNTER> (0=OFF, 1=ONCE,
# 2=PERIODIC), COUNTER_LEVEL_<COUNTER> (0=THREAD, 1=NODE) and
# REDUCE_METHOD_<COUNTER> (0=SUM, 1=MAX, 2=MIN, 3=MASTER); an OFF counter
# reads as NODE and SUM whatever its line says.  Also sets ENABLED_COUNT and
# DISABLED_COUNT, and COUNTER_CONFIG_NAMES, the counters named, in file order.
function(arts_parse_counter_config cfg)
    if(NOT EXISTS "${cfg}")
        message(FATAL_ERROR "Counter configuration file not found: ${cfg}")
    endif()
    # A CMake list groups elements across '[' ... ']', and '\' and ';' escape
    # or split them, so these four are held as control characters while the
    # text is a list: every line stays one element of its own.
    file(READ "${cfg}" _text)
    string(ASCII 1 _bs)
    string(ASCII 2 _lb)
    string(ASCII 3 _rb)
    string(ASCII 4 _semi)
    string(REPLACE "\\" "${_bs}" _text "${_text}")
    string(REPLACE "[" "${_lb}" _text "${_text}")
    string(REPLACE "]" "${_rb}" _text "${_text}")
    string(REPLACE ";" "${_semi}" _text "${_text}")
    string(REPLACE "\n" ";" _lines "${_text}")
    string(CONCAT _counter_line
           "^([A-Za-z0-9_]+)[ \t]*=[ \t]*(OFF|ONCE|PERIODIC)"
           "(,[ \t]*(THREAD|NODE))?(,[ \t]*(SUM|MAX|MIN|MASTER))?[ \t]*$")
    set(_enabled 0)
    set(_disabled 0)
    set(_names "")
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "^[ \t]*#" OR _line MATCHES "^[ \t]*$")
            continue()
        endif()
        if(NOT _line MATCHES "${_counter_line}")
            string(REPLACE "${_bs}" "\\" _line "${_line}")
            string(REPLACE "${_lb}" "[" _line "${_line}")
            string(REPLACE "${_rb}" "]" _line "${_line}")
            string(REPLACE "${_semi}" ";" _line "${_line}")
            message(FATAL_ERROR "${cfg}: cannot read the line '${_line}'; "
                    "a line is COUNTER=MODE[,LEVEL[,REDUCE]], a comment or "
                    "blank")
        endif()
        set(_key ${CMAKE_MATCH_1})
        set(_mode ${CMAKE_MATCH_2})
        set(_level ${CMAKE_MATCH_4})
        set(_reduce ${CMAKE_MATCH_6})
        list(APPEND _names ${_key})
        if(_mode STREQUAL "OFF")
            set(ENABLE_${_key} 0 PARENT_SCOPE)
            set(COUNTER_MODE_${_key} 0 PARENT_SCOPE)
            set(COUNTER_LEVEL_${_key} 1 PARENT_SCOPE)
            set(REDUCE_METHOD_${_key} 0 PARENT_SCOPE)
            math(EXPR _disabled "${_disabled} + 1")
            continue()
        endif()
        set(ENABLE_${_key} 1 PARENT_SCOPE)
        math(EXPR _enabled "${_enabled} + 1")
        if(_mode STREQUAL "ONCE")
            set(COUNTER_MODE_${_key} 1 PARENT_SCOPE)
        else()
            set(COUNTER_MODE_${_key} 2 PARENT_SCOPE)
        endif()
        if(_level STREQUAL "THREAD")
            set(COUNTER_LEVEL_${_key} 0 PARENT_SCOPE)
        else()
            set(COUNTER_LEVEL_${_key} 1 PARENT_SCOPE)
        endif()
        if(_reduce STREQUAL "MAX")
            set(REDUCE_METHOD_${_key} 1 PARENT_SCOPE)
        elseif(_reduce STREQUAL "MIN")
            set(REDUCE_METHOD_${_key} 2 PARENT_SCOPE)
        elseif(_reduce STREQUAL "MASTER")
            set(REDUCE_METHOD_${_key} 3 PARENT_SCOPE)
        else()
            set(REDUCE_METHOD_${_key} 0 PARENT_SCOPE)
        endif()
    endforeach()
    set(ENABLED_COUNT ${_enabled} PARENT_SCOPE)
    set(DISABLED_COUNT ${_disabled} PARENT_SCOPE)
    set(COUNTER_CONFIG_NAMES "${_names}" PARENT_SCOPE)
endfunction()
