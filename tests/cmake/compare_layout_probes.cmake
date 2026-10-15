# Run a layout probe's C and C++ compilations and require their fingerprint
# lines (TAG c ... / TAG cpp ...) to be identical.
#   -DC_EXECUTABLE=<path> -DCPP_EXECUTABLE=<path> -DTAG=<fingerprint tag>
#   -DNAME=<test name>
foreach(_v C_EXECUTABLE CPP_EXECUTABLE TAG NAME)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "C_EXECUTABLE, CPP_EXECUTABLE, TAG and NAME are required")
    endif()
endforeach()

foreach(_lang c cpp)
    if(_lang STREQUAL "c")
        set(_exe "${C_EXECUTABLE}")
    else()
        set(_exe "${CPP_EXECUTABLE}")
    endif()
    execute_process(
        COMMAND "${_exe}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR
            "${_lang} layout probe failed (${_result})\nstdout:\n${_output}\nstderr:\n${_error}")
    endif()
    string(REGEX MATCH "${TAG} ${_lang} [^\r\n]+" _line "${_output}")
    if(_line STREQUAL "")
        message(FATAL_ERROR "${_lang} layout probe emitted no fingerprint\n${_output}")
    endif()
    set(${_lang}_line "${_line}")
    string(REGEX REPLACE "^${TAG} ${_lang} " "" ${_lang}_fingerprint "${_line}")
endforeach()

if(NOT c_fingerprint STREQUAL cpp_fingerprint)
    message(FATAL_ERROR
        "C/C++ layout mismatch\nC:   ${c_line}\nC++: ${cpp_line}")
endif()

message("PASS ${NAME}: C/C++ fingerprints match")
