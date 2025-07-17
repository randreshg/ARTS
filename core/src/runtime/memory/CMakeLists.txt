# Runtime memory submodule
file(GLOB MEMORY_SOURCES "*.c")
add_library(arts_runtime_memory OBJECT ${MEMORY_SOURCES})
target_include_directories(arts_runtime_memory PRIVATE ${ARTS_CORE_INCLUDE_DIR}) 