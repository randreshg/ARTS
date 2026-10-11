# Run by ctest, not compiled.  A block's bytes reach a rank through its store
# on this arm, so every object the arm compiles from this source must name no
# rendezvous and no one-sided-put symbol at all: a surviving reference means a
# payload path was left live behind a run-time condition instead of being
# compiled out.
#
# Objects are resolved by TARGET directory, never by a bare "*purge.c.o" glob:
# one build directory holds this source compiled both ways, and grading
# whichever one CMake listed first is a false verdict in both directions.  The
# target names come from the registration, which asks CMake which of them this
# tree actually has, so a renamed or unbuilt arm is a failure here rather than
# an arm nobody grades.
#
# An absence proves nothing on its own -- a renamed symbol, a stale object or a
# changed nm output shape all read as "clean" -- so the same tree's non-CXL
# build of each graded source is the control: where the payload plane is
# still compiled in, nm must find it.  CONTROLS names the controls this tree
# has; a graded source with none is graded on its absence half alone, and the
# run says so.
#
# The two EXCL release policies compile the arm from different translation
# units (excl/purge.c, excl/retain.c), so every entry of GRADED and CONTROLS
# is <target>:<source stem>, comma-separated.
cmake_policy(SET CMP0009 NEW) # GLOB_RECURSE must not follow symlinks

if(NOT DEFINED BUILD_DIR)
    message(FATAL_ERROR "BUILD_DIR is required")
endif()
if(NOT GRADED)
    message(FATAL_ERROR "GRADED is required")
endif()
string(REPLACE "," ";" _graded "${GRADED}")
string(REPLACE "," ";" _controls "${CONTROLS}")

set(_forbidden
    arts_net_put_payload arts_net_rdzv_expect arts_net_rdzv_local
    arts_net_rdzv_txid_next arts_db_rdzv_discard_landing
    arts_db_buf_ref_release_cb arts_db_await_ack arts_send_db_publish_cts)

# GLOB_RECURSE matches the LAST path component only, so the target directory is
# a filter over the result rather than part of the expression.
file(GLOB_RECURSE _all "${BUILD_DIR}/libs/*.c.o")

function(cxl_object_of _dir _src _out)
    string(REPLACE "." "\\." _dre "${_dir}")
    set(_hit "")
    foreach(_o IN LISTS _all)
        if(_o MATCHES "/${_dre}/.*coherence/excl/${_src}\\.c\\.o$")
            list(APPEND _hit "${_o}")
        endif()
    endforeach()
    list(LENGTH _hit _n)
    if(_n GREATER 1)
        message(FATAL_ERROR "ambiguous objects for ${_dir}: ${_hit}")
    endif()
    set(${_out} "${_hit}" PARENT_SCOPE)
endfunction()

function(cxl_undefined_symbols _obj _out)
    execute_process(COMMAND nm --undefined-only "${_obj}"
                    OUTPUT_VARIABLE _syms ERROR_VARIABLE _err
                    RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "nm failed on ${_obj} (${_rc}): ${_err}")
    endif()
    set(${_out} "${_syms}" PARENT_SCOPE)
endfunction()

set(_hits "")
set(_srcs "")
foreach(_e IN LISTS _graded)
    string(REPLACE ":" ";" _pair "${_e}")
    list(GET _pair 0 _t)
    list(GET _pair 1 _src)
    list(APPEND _srcs "${_src}")
    cxl_object_of("${_t}.dir" "${_src}" _obj)
    if(NOT _obj)
        message(FATAL_ERROR
            "no object for coherence/excl/${_src}.c under ${BUILD_DIR} for "
            "${_t}, which this tree configures -- build the tree before "
            "grading it")
    endif()
    cxl_undefined_symbols("${_obj}" _syms)
    foreach(_s IN LISTS _forbidden)
        if(_syms MATCHES "[ \t]${_s}(\n|$)")
            list(APPEND _hits "${_obj}: ${_s}")
        endif()
    endforeach()
endforeach()
if(_hits)
    message(FATAL_ERROR "the store-backed arm still references the payload "
                        "plane: ${_hits}")
endif()
list(REMOVE_DUPLICATES _srcs)

set(_control_state "")
foreach(_src IN LISTS _srcs)
    set(_control "")
    foreach(_e IN LISTS _controls)
        string(REPLACE ":" ";" _pair "${_e}")
        list(GET _pair 0 _t)
        list(GET _pair 1 _csrc)
        if(_csrc STREQUAL _src)
            cxl_object_of("${_t}.dir" "${_src}" _control)
            if(NOT _control)
                message(FATAL_ERROR
                    "no control object under ${BUILD_DIR} for ${_t}, which "
                    "this tree configures -- without it the absence above "
                    "proves nothing")
            endif()
        endif()
    endforeach()
    if(_control)
        cxl_undefined_symbols("${_control}" _syms)
        set(_seen "")
        foreach(_s IN LISTS _forbidden)
            if(_syms MATCHES "[ \t]${_s}(\n|$)")
                list(APPEND _seen "${_s}")
            endif()
        endforeach()
        if(NOT _seen)
            message(FATAL_ERROR
                "control ${_control} names none of the payload plane either "
                "-- the absence above proves nothing")
        endif()
        list(LENGTH _seen _n_seen)
        list(APPEND _control_state
             "${_src}.c: ${_n_seen} of the plane present in the control")
    else()
        list(APPEND _control_state
             "${_src}.c: no control in this tree, absence half only")
    endif()
endforeach()
string(REPLACE ";" "; " _control_state "${_control_state}")

list(LENGTH _graded _n_objs)
message("PASS cxl_no_payload_plane: ${_n_objs} object(s) clean "
        "(${GRADED}); ${_control_state}")
