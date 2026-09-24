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
# changed nm output shape all read as "clean" -- so the same tree's non-FAM
# build of this source is the control: where the payload plane is still
# compiled in, nm must find it.  REQUIRE_CONTROL says this tree has that build,
# and without it the run grades the absence half alone and says so.
cmake_policy(SET CMP0009 NEW) # GLOB_RECURSE must not follow symlinks

if(NOT DEFINED BUILD_DIR)
    message(FATAL_ERROR "BUILD_DIR is required")
endif()

set(_control_dir "arts_static_ocr_excl_purge.dir")

set(_graded_dirs "arts_memory.dir")
foreach(_t IN LISTS GRADED_TARGETS)
    list(APPEND _graded_dirs "${_t}.dir")
endforeach()

set(_forbidden
    arts_net_put_payload arts_net_rdzv_expect arts_net_rdzv_local
    arts_net_rdzv_txid_next arts_db_rdzv_discard_landing
    arts_db_buf_ref_release_cb arts_db_await_ack arts_send_db_publish_cts)

# GLOB_RECURSE matches the LAST path component only, so the target directory is
# a filter over the result rather than part of the expression.
file(GLOB_RECURSE _all "${BUILD_DIR}/libs/*purge.c.o")

function(fam_object_of_dir _dir _out)
    string(REPLACE "." "\\." _dre "${_dir}")
    set(_hit "")
    foreach(_o IN LISTS _all)
        if(_o MATCHES "/${_dre}/.*coherence/excl/purge\\.c\\.o$")
            list(APPEND _hit "${_o}")
        endif()
    endforeach()
    list(LENGTH _hit _n)
    if(_n GREATER 1)
        message(FATAL_ERROR "ambiguous objects for ${_dir}: ${_hit}")
    endif()
    set(${_out} "${_hit}" PARENT_SCOPE)
endfunction()

function(fam_undefined_symbols _obj _out)
    execute_process(COMMAND nm --undefined-only "${_obj}"
                    OUTPUT_VARIABLE _syms ERROR_VARIABLE _err
                    RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "nm failed on ${_obj} (${_rc}): ${_err}")
    endif()
    set(${_out} "${_syms}" PARENT_SCOPE)
endfunction()

set(_hits "")
foreach(_d IN LISTS _graded_dirs)
    fam_object_of_dir("${_d}" _obj)
    if(NOT _obj)
        message(FATAL_ERROR
            "no object for coherence/excl/purge.c under ${BUILD_DIR} for ${_d}, "
            "which this tree configures -- build the tree before grading it")
    endif()
    fam_undefined_symbols("${_obj}" _syms)
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

fam_object_of_dir("${_control_dir}" _control)
if(_control)
    fam_undefined_symbols("${_control}" _syms)
    set(_seen "")
    foreach(_s IN LISTS _forbidden)
        if(_syms MATCHES "[ \t]${_s}(\n|$)")
            list(APPEND _seen "${_s}")
        endif()
    endforeach()
    if(NOT _seen)
        message(FATAL_ERROR
            "control ${_control} names none of the payload plane either -- the "
            "absence above proves nothing")
    endif()
    list(LENGTH _seen _n_seen)
    set(_control_state "${_n_seen} of the plane present in the control")
elseif(REQUIRE_CONTROL)
    message(FATAL_ERROR
        "no control object under ${BUILD_DIR} for ${_control_dir}, which this "
        "tree configures -- without it the absence above proves nothing")
else()
    set(_control_state
        "no control in this tree: the absence half is all it can grade")
endif()

list(LENGTH _graded_dirs _n_objs)
message("PASS fam_no_payload_plane: ${_n_objs} object(s) clean "
        "(${_graded_dirs}), ${_control_state}")
