# Reusable helpers for GPU (CUDA / HIP) targets in the ARTS build system.
#
# Expected parent-scope variables (set in the root CMakeLists.txt):
#   GPU_USE_HIPCC, GPU_HIPCC_FLAGS, GPU_PLATFORM, GPU_LINK_LIBS,
#   GPU_SANITIZER_EXCLUDE_FLAGS, ARTS_USE_SANITIZERS

# ---------------------------------------------------------------------------
# arts_link_gpu_library(<target>)
#   Link <target> against the preferred GPU-enabled ARTS library
#   (shared preferred over static).
# ---------------------------------------------------------------------------
function(arts_link_gpu_library target)
    if(TARGET arts_cuda_shared)
        target_link_libraries(${target} PRIVATE arts_cuda_shared)
    elseif(TARGET arts_cuda_static)
        target_link_libraries(${target} PRIVATE arts_cuda_static)
    else()
        message(FATAL_ERROR "No GPU ARTS library target found for ${target}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# arts_gpu_set_cu_sources(<target> <source>...)
#   When GPU_USE_HIPCC is set, mark .cu sources as CXX, apply GPU_HIPCC_FLAGS
#   and sanitizer exclusions.
#
#   Options:
#     SKIP_HIPCC_FLAGS  - do not add GPU_HIPCC_FLAGS (e.g. when rocthrust
#                         already provides -x hip / --offload-arch).
#     RDC               - add -fgpu-rdc compile + link flags.
# ---------------------------------------------------------------------------
function(arts_gpu_set_cu_sources target)
    cmake_parse_arguments(ARG "SKIP_HIPCC_FLAGS;RDC" "" "" ${ARGN})
    if(NOT GPU_USE_HIPCC)
        return()
    endif()
    foreach(src ${ARG_UNPARSED_ARGUMENTS})
        if(src MATCHES "\\.cu$")
            set_source_files_properties(${src} PROPERTIES LANGUAGE CXX)
        endif()
    endforeach()
    if(NOT ARG_SKIP_HIPCC_FLAGS)
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:${GPU_HIPCC_FLAGS}>")
    endif()
    if(ARTS_USE_SANITIZERS)
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:${GPU_SANITIZER_EXCLUDE_FLAGS}>")
    endif()
    if(ARG_RDC)
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:-fgpu-rdc>")
        target_link_options(${target} PRIVATE -fgpu-rdc)
    endif()
endfunction()
