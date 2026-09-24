# Fabric-attached-memory pool.  The target exists wherever the backend does,
# because the benchmark variants read its source list to compile these same
# TUs into their own libraries with their own definitions; only a tree whose
# own library is FAM-enabled compiles and links these objects.
if(NOT ARTS_FAM_BACKEND STREQUAL "OFF")
    # Exactly one backend TU belongs beside pool.c -- the two backends cannot
    # both compile in one tree, so the selection happens here, not behind an
    # internal #ifdef.  Until a backend TU exists for the selected value the
    # module carries pool.c alone, and the assertion below refuses the tree
    # rather than leaving it to fail at link time for want of the backend
    # symbols pool.c calls.
    set(ARTS_FAM_SOURCES pool.c)
    if(ARTS_FAM_BACKEND STREQUAL "SHM")
        list(APPEND ARTS_FAM_SOURCES shm.c)
    endif()
    if(ARTS_FAM_TREE_RESIDENCY STREQUAL "")
        # Source-list carrier: these objects are never compiled in this tree.
        add_library(arts_fam OBJECT EXCLUDE_FROM_ALL ${ARTS_FAM_SOURCES})
    else()
        add_library(arts_fam OBJECT ${ARTS_FAM_SOURCES})
        target_link_libraries(arts_fam PRIVATE arts_interface)
        arts_apply_protocol(arts_fam ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY}
                            ${ARTS_RELEASE_POLICY} ${ARTS_NO_RO_COMBINING}
                            ${ARTS_FAM_TREE_RESIDENCY})
    endif()
    # The variants depend on this list being complete; an empty or truncated
    # one would give them ARTS_FAM with no allocator behind it.
    get_target_property(_arts_fam_tus arts_fam SOURCES)
    list(LENGTH _arts_fam_tus _arts_fam_tu_count)
    if(_arts_fam_tu_count LESS 2)
        message(FATAL_ERROR
                "the fam module's source list holds ${_arts_fam_tu_count} "
                "translation unit(s): a build needs pool.c and one backend")
    endif()
    message(STATUS "ARTS FAM: module holds ${_arts_fam_tu_count} translation unit(s)")
endif()
