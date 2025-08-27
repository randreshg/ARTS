# Runtime network submodule
file(GLOB NETWORK_SOURCES "*.c")
add_library(arts_runtime_network OBJECT ${NETWORK_SOURCES})
target_include_directories(arts_runtime_network PRIVATE ${ARTS_CORE_INCLUDE_DIR})