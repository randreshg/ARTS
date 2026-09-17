# The translated FFTW is a pinned artifact of the source tree: pinned/ holds
# the assembly build.py generated from the pinned FFTW source with LLVM 14
# (fftw-reloc.s), the translation's schema, and a stamp of the configuration
# it was generated at.  A tree assembles and links it with the compiler it
# was configured with, so the library has no toolchain requirement of its
# own.  LLVM 14 -- the translation is an IR pass over typed pointers, which
# later LLVM releases no longer carry -- is needed only to regenerate the
# artifact, and only a tree that asks for that (FFTW_RELOC_REGENERATE) looks
# for it.
enable_language(ASM)

set(_work "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_pinned "${CMAKE_CURRENT_SOURCE_DIR}/pinned")
set(_simd)
foreach(_flag IN LISTS ARTS_FFTW_SIMD)
    list(APPEND _simd "--simd=${_flag}")
endforeach()

option(FFTW_RELOC_REGENERATE
    "Regenerate the translated FFTW from source with LLVM 14 instead of assembling the pinned artifact" OFF)

if(FFTW_RELOC_REGENERATE)
    set(_tools "${PROJECT_SOURCE_DIR}/install/fftw-port-tools/usr")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
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

    # The generating host's own C compiler assembles the result once, so an
    # assembly dialect the GNU assembler rejects fails here, not on the host
    # that consumes the artifact.  Regenerating IS re-pinning: the files the
    # source tree carries are the ones this tree builds from, so the two
    # cannot drift.
    set(_assembly "${_work}/fftw-reloc.s")
    add_custom_command(OUTPUT "${_assembly}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build.py"
            --archive "${ARTS_FFTW_TARBALL}"
            --work "${_work}" --clang "${FFTW_RELOC_CLANG}"
            --link "${FFTW_RELOC_LINK}" --translator "$<TARGET_FILE:fftw_reloc_translate>"
            --cc "${CMAKE_C_COMPILER}" --pin "${_pinned}" ${_simd}
        DEPENDS fftw_reloc_translate build.py "${ARTS_FFTW_TARBALL}"
        BYPRODUCTS "${_work}/fftw-reloc.bc" "${_work}/fftw-reloc.bc.schema"
            "${_work}/fftw-reloc.stamp" "${_pinned}/fftw-reloc.s"
            "${_pinned}/fftw-reloc.bc.schema" "${_pinned}/fftw-reloc.stamp"
        VERBATIM USES_TERMINAL)
    add_custom_target(fftw_reloc_pin DEPENDS "${_assembly}")
else()
    set(_assembly "${_pinned}/fftw-reloc.s")
    if(NOT EXISTS "${_assembly}" OR NOT EXISTS "${_pinned}/fftw-reloc.stamp")
        message(FATAL_ERROR
            "${_pinned} is incomplete: the translated FFTW is a pinned artifact "
            "of the source tree (fftw-reloc.s and its stamp).  Restore it, or "
            "regenerate it on a host with LLVM 14 (-DFFTW_RELOC_REGENERATE=ON, "
            "target fftw_reloc_pin).")
    endif()
    file(STRINGS "${_pinned}/fftw-reloc.stamp" _stamp)
    foreach(_line IN LISTS _stamp)
        if(_line MATCHES "^([a-z0-9_]+)=(.*)$")
            set(_stamp_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
        endif()
    endforeach()
    # The artifact is one configuration of the library.  A tree whose FFTW
    # configuration differs would compare a mirror built at one codelet set
    # against an HPX program built at another, so the mismatch is an error
    # that names the regeneration, never a silent substitution.
    file(SHA256 "${ARTS_FFTW_TARBALL}" _fftw_sha256)
    string(REPLACE ";" " " _simd_words "${ARTS_FFTW_SIMD}")
    if(NOT _stamp_fftw_sha256 STREQUAL _fftw_sha256
       OR NOT _stamp_simd STREQUAL _simd_words)
        message(FATAL_ERROR
            "The pinned translated FFTW was generated from source ${_stamp_fftw_sha256} "
            "at SIMD '${_stamp_simd}'; this tree has source ${_fftw_sha256} at "
            "ARTS_FFTW_SIMD '${_simd_words}'.  Regenerate the artifact on a host "
            "with LLVM 14 (-DFFTW_RELOC_REGENERATE=ON, target fftw_reloc_pin) "
            "or configure with the pinned SIMD set.")
    endif()
    message(STATUS "fftw_reloc: pinned translation (FFTW ${_stamp_fftw_sha256}, "
        "SIMD '${_stamp_simd}', alignment ${_stamp_alignment}, -march=${_stamp_march}, "
        "${_stamp_clang})")
endif()

# The public header the mirror and the runtime side compile against, from the
# same pinned source the artifact was translated from.
file(ARCHIVE_EXTRACT INPUT "${ARTS_FFTW_TARBALL}" DESTINATION "${_work}"
    PATTERNS "fftw-3.3.10/api/fftw3.h")

add_library(fftw_reloc STATIC reloc.cpp "${_assembly}")
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
