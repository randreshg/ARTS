# Fabric-attached-memory pool.  The target exists wherever the backend does,
# because the benchmark variants read its source list to compile these same
# TUs into their own libraries with their own definitions; only a tree whose
# own library is FAM-enabled compiles and links these objects.
if(NOT ARTS_FAM_BACKEND STREQUAL "OFF")
    # Exactly one backend TU belongs beside pool.c -- the two backends cannot
    # both compile in one tree (device.c includes headers that exist only
    # where the device library does), so the selection happens here, not
    # behind an internal #ifdef.  Choosing at this level is what guarantees
    # that every member of the backend seam is defined by the TU the build
    # compiled.
    set(ARTS_FAM_SOURCES pool.c)
    if(ARTS_FAM_BACKEND STREQUAL "SHM")
        # strict.c belongs to this backend alone: the second coherency domain
        # is what an inherited mapping can be given and a device region cannot.
        list(APPEND ARTS_FAM_SOURCES shm.c strict.c)
        set(_arts_fam_tu_floor 3)
    else()
        list(APPEND ARTS_FAM_SOURCES device.c)
        set(_arts_fam_tu_floor 2)
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
    if(_arts_fam_tu_count LESS _arts_fam_tu_floor)
        message(FATAL_ERROR
                "the fam module's source list holds ${_arts_fam_tu_count} "
                "translation unit(s): a build needs pool.c and everything the "
                "${ARTS_FAM_BACKEND} backend is made of "
                "(${_arts_fam_tu_floor} in all)")
    endif()
    message(STATUS "ARTS FAM: module holds ${_arts_fam_tu_count} translation unit(s)")
endif()
