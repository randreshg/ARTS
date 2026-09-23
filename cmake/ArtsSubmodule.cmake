include_guard(GLOBAL)

# Submodule sources are fetched on demand: a plain `git clone` (without
# --recurse-submodules) leaves every submodule directory empty, so each
# submodule is ensured here right before it is consumed — marker present →
# use as-is; absent → run `git submodule update --init` for that path (which
# honors per-submodule settings such as shallow). The fallback error names
# the exact command for source trees where auto-init is impossible (no git
# available, or not a git checkout, e.g. an exported source archive).
# Bring one submodule to the commit this repository records.
#
# Two things go wrong without this.  A checkout that never initialised the
# submodule has no source at all.  One that initialised it and later took a
# parent revision recording a newer commit keeps its older checkout, and then
# compiles sources from one revision against a parent that expects another --
# a mismatch nothing announces, and which surfaces later as a missing symbol
# or a missing file rather than as a stale submodule.
#
# A submodule carrying local work is never moved.  Uncommitted changes are the
# caller's to lose, and a checkout ahead of the recorded commit is what
# working on a submodule looks like; both are reported and left alone.  Only a
# clean checkout that is behind, or on an unrelated commit, is moved.
function(arts_init_submodule path marker)
    find_package(Git QUIET)
    set(_git_usable FALSE)
    if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
        set(_git_usable TRUE)
    endif()
    set(_sub "${CMAKE_SOURCE_DIR}/${path}")

    if(NOT EXISTS "${_sub}/${marker}")
        if(_git_usable)
            message(STATUS "Initializing submodule ${path}")
            execute_process(
                COMMAND ${GIT_EXECUTABLE} submodule update --init ${path}
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                RESULT_VARIABLE _rc
            )
        endif()
        if(NOT EXISTS "${_sub}/${marker}")
            message(FATAL_ERROR
                "Submodule ${path} is not initialized and could not be fetched "
                "automatically. Run: git submodule update --init ${path}")
        endif()
        # A fresh checkout is at the recorded commit by construction.
        return()
    endif()

    if(NOT _git_usable)
        return()
    endif()

    # The gitlink this revision records, and what is actually checked out.
    execute_process(
        COMMAND ${GIT_EXECUTABLE} ls-tree HEAD -- ${path}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE _entry OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _rc_tree
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C ${_sub} rev-parse HEAD
        OUTPUT_VARIABLE _actual OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _rc_head
    )
    string(REGEX MATCH "commit ([0-9a-f]+)" _matched "${_entry}")
    set(_recorded "${CMAKE_MATCH_1}")
    # Anything unreadable here (a tarball export, a detached parent, an
    # unborn HEAD) leaves the checkout as it stands rather than guessing.
    if(NOT _rc_tree EQUAL 0 OR NOT _rc_head EQUAL 0
       OR _recorded STREQUAL "" OR _actual STREQUAL ""
       OR _recorded STREQUAL _actual)
        return()
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C ${_sub} status --porcelain --untracked-files=no
        OUTPUT_VARIABLE _local_changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
    )
    if(NOT _local_changes STREQUAL "")
        message(WARNING
            "Submodule ${path} is at ${_actual}, this repository records "
            "${_recorded}, and its working tree has uncommitted changes -- "
            "leaving it as it is.  The build uses what is checked out there.  "
            "To take the recorded commit, commit or copy those changes aside "
            "first, then run: git submodule update ${path}")
        return()
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} -C ${_sub} merge-base --is-ancestor ${_recorded} ${_actual}
        RESULT_VARIABLE _recorded_is_ancestor ERROR_QUIET
    )
    if(_recorded_is_ancestor EQUAL 0)
        message(WARNING
            "Submodule ${path} is at ${_actual}, ahead of the ${_recorded} "
            "this repository records -- leaving it as it is, since moving it "
            "back would drop the commits it carries.  The build uses sources "
            "no other checkout has: record them with "
            "'git add ${path}' in this repository.")
        return()
    endif()

    message(STATUS "Submodule ${path}: moving to the recorded ${_recorded}")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} submodule update ${path}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        RESULT_VARIABLE _rc_update
    )
    if(NOT _rc_update EQUAL 0)
        message(WARNING
            "Submodule ${path} could not be moved to ${_recorded} and stays at "
            "${_actual}.  Run: git submodule update ${path}")
    endif()
endfunction()
