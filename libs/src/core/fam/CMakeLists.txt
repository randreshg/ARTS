# Fabric-attached-memory pool.  The target exists wherever the backend does,
# because the benchmark variants read its source list to compile these same
# TUs into their own libraries with their own definitions; only a tree whose
# own library is FAM-enabled compiles and links these objects.
if(NOT ARTS_FAM_BACKEND STREQUAL "OFF")
    # library.c speaks the device library's API for both backends; the
    # backend decides only which library it links and whether strict.c is
    # compiled beside it.  strict.c gives one host's memory the second
    # coherency domain a store that is not coherent across hosts has, so it
    # exists over the vendored fake library and never over a device library,
    # which has that domain already.
    set(ARTS_FAM_SOURCES pool.c library.c)
    set(_arts_fam_tu_floor 2)
    if(ARTS_FAM_BACKEND STREQUAL "SHM")
        list(APPEND ARTS_FAM_SOURCES strict.c)
        set(_arts_fam_tu_floor 3)
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
