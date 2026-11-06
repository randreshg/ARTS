# Checks that a run lost the named counter files, and removes the temporary
# file each would have been written through (a test may have made it a FIFO
# so that the write never finishes):
#   cmake -DRECORD_DIR=<folder> -P record_lost.cmake -- <file>...
# No <file> may exist in the folder; <file>.tmp is removed.
set(_files "")
set(_seen FALSE)
math(EXPR _last "${CMAKE_ARGC} - 1")
foreach(_i RANGE 0 ${_last})
    if(_seen)
        list(APPEND _files "${CMAKE_ARGV${_i}}")
    elseif(CMAKE_ARGV${_i} STREQUAL "--")
        set(_seen TRUE)
    endif()
endforeach()
if(NOT _files OR NOT DEFINED RECORD_DIR)
    message(FATAL_ERROR
            "record_lost: usage: -DRECORD_DIR=<folder> -- <file>...")
endif()
set(_written "")
foreach(_f IN LISTS _files)
    if(EXISTS "${RECORD_DIR}/${_f}")
        list(APPEND _written "${_f}")
    endif()
    file(REMOVE "${RECORD_DIR}/${_f}.tmp")
endforeach()
if(_written)
    message(FATAL_ERROR "record_lost: written in ${RECORD_DIR}: ${_written}")
endif()
message("record_lost: ${_files} not written in ${RECORD_DIR}")
