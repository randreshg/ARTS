# Transport submodule (socket I/O, protocol, message dispatch, launcher)
file(GLOB TRANSPORT_SOURCES "*.c")

# Select the data-plane backend: GASNet-EX (gasnet_transport.c) or rsocket/TCP
# (socket.c). They define the same low-level transport symbols, so exactly one
# must compile.
if(ARTS_USE_GASNET)
    list(FILTER TRANSPORT_SOURCES EXCLUDE REGEX "/socket\\.c$")
else()
    list(FILTER TRANSPORT_SOURCES EXCLUDE REGEX "/gasnet_transport\\.c$")
endif()

add_library(arts_transport OBJECT ${TRANSPORT_SOURCES})
target_include_directories(arts_transport PRIVATE
    ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR} ${ARTS_BUILD_INTERNAL_INCLUDE_DIR}
)

# GASNet headers/defines (gasnetex.h, GASNET_PAR, conduit include dirs) for
# gasnet_transport.c, inherited from the arts_gasnet interface target.
if(ARTS_USE_GASNET)
    target_link_libraries(arts_transport PUBLIC arts_gasnet)
endif()

# Add RDMA provider include directories if available
if(ARTS_RDMA_PROVIDER_INCLUDE_DIRS)
    target_include_directories(arts_transport PRIVATE ${ARTS_RDMA_PROVIDER_INCLUDE_DIRS})
endif()

# Add hwloc include directories if available
if(HWLOC_INCLUDE_DIRS)
    target_include_directories(arts_transport PRIVATE ${HWLOC_INCLUDE_DIRS})
endif()
