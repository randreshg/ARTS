# Runtime compute submodule
file(GLOB COMPUTE_SOURCES "*.c")
add_library(arts_runtime_compute OBJECT ${COMPUTE_SOURCES})
target_include_directories(arts_runtime_compute PRIVATE ${ARTS_CORE_INCLUDE_DIR}) 