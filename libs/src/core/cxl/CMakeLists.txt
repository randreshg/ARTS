# The CXL store seam.  The target exists wherever a CXL library was resolved,
# because the benchmark variants read its source list to compile the same TU
# into their own libraries with their own definitions; only a tree whose own
# library is CXL compiles and links these objects itself.
if(ARTS_CXL_NEEDED)
    set(ARTS_CXL_SOURCES store.c)
    if(ARTS_CXL_TREE_RESIDENCY STREQUAL "")
        add_library(arts_cxl OBJECT EXCLUDE_FROM_ALL ${ARTS_CXL_SOURCES})
    else()
        add_library(arts_cxl OBJECT ${ARTS_CXL_SOURCES})
        target_link_libraries(arts_cxl PRIVATE arts_interface)
        arts_apply_protocol(arts_cxl ${ARTS_COHERENCE_ARM} ${ARTS_WRITE_POLICY}
                            ${ARTS_RELEASE_POLICY} ${ARTS_NO_RO_COMBINING}
                            ${ARTS_CXL_TREE_RESIDENCY})
    endif()
    # The seam again, recording every flush it performs: linked only by the
    # test that reads the record, through arts_static_cxl_flush_recorder.
    if(ARTS_BUILD_TESTS AND NOT ARTS_CXL_TREE_RESIDENCY STREQUAL "")
        add_library(arts_cxl_flush_recorder OBJECT EXCLUDE_FROM_ALL ${ARTS_CXL_SOURCES})
        target_link_libraries(arts_cxl_flush_recorder PRIVATE arts_interface)
        arts_apply_protocol(arts_cxl_flush_recorder ${ARTS_COHERENCE_ARM}
                            ${ARTS_WRITE_POLICY} ${ARTS_RELEASE_POLICY}
                            ${ARTS_NO_RO_COMBINING} ${ARTS_CXL_TREE_RESIDENCY})
        target_compile_definitions(arts_cxl_flush_recorder PRIVATE ARTS_CXL_FLUSH_RECORDER=1)
    endif()
endif()
