find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_tools "${PROJECT_SOURCE_DIR}/install/fftw-port-tools/usr")
find_program(FFTW_RELOC_CLANG NAMES clang-14 HINTS "${_tools}/bin" REQUIRED)
find_program(FFTW_RELOC_LINK NAMES llvm-link-14 HINTS "${_tools}/bin" REQUIRED)
find_path(FFTW_RELOC_LLVM_INCLUDE llvm/IR/Module.h
    HINTS "${_tools}/lib/llvm-14/include" /usr/lib/llvm-14/include REQUIRED)
find_library(FFTW_RELOC_LLVM_LIBRARY NAMES LLVM-14
    HINTS "${_tools}/lib/x86_64-linux-gnu" REQUIRED)

add_executable(fftw_reloc_translate EXCLUDE_FROM_ALL translate.cpp)
target_compile_features(fftw_reloc_translate PRIVATE cxx_std_17)
target_include_directories(fftw_reloc_translate SYSTEM PRIVATE "${FFTW_RELOC_LLVM_INCLUDE}")
target_link_libraries(fftw_reloc_translate PRIVATE "${FFTW_RELOC_LLVM_LIBRARY}")

set(_work "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_object "${_work}/fftw-reloc.o")
set(_simd)
foreach(_flag IN LISTS ARTS_FFTW_SIMD)
    list(APPEND _simd "--simd=${_flag}")
endforeach()
add_custom_command(OUTPUT "${_object}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build.py"
        --archive "${PROJECT_SOURCE_DIR}/third_party/fftw-3.3.10.tar.gz"
        --work "${_work}" --clang "${FFTW_RELOC_CLANG}"
        --link "${FFTW_RELOC_LINK}" --translator "$<TARGET_FILE:fftw_reloc_translate>"
        ${_simd}
    DEPENDS fftw_reloc_translate build.py "${PROJECT_SOURCE_DIR}/third_party/fftw-3.3.10.tar.gz"
    BYPRODUCTS "${_work}/fftw-reloc.bc" "${_work}/fftw-reloc.bc.schema"
    VERBATIM USES_TERMINAL)
set_source_files_properties("${_object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
add_library(fftw_reloc STATIC reloc.cpp "${_object}")
add_library(arts::fftw_reloc ALIAS fftw_reloc)
option(FFTW_RELOC_AUDIT "Count FFTW relocation operations for diagnostics" OFF)
if(FFTW_RELOC_AUDIT)
    target_compile_definitions(fftw_reloc PRIVATE FFTW_RELOC_AUDIT)
endif()
# A published image is read-only by contract; under this diagnostic the pages
# it occupies are read-only to the hardware as well, which costs the image its
# own page-aligned placement inside the block that carries it.
option(FFTW_RELOC_PROTECT
    "Write-protect the published FFTW image while row transforms execute" OFF)
if(FFTW_RELOC_PROTECT)
    target_compile_definitions(fftw_reloc PRIVATE FFTW_RELOC_PROTECT)
endif()
target_compile_features(fftw_reloc PRIVATE cxx_std_17)
target_include_directories(fftw_reloc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    "${_work}/fftw-3.3.10/api")
target_link_libraries(fftw_reloc PUBLIC m)
