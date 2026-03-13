###############################################################################
## This material was prepared as an account of work sponsored by an agency 
## of the United States Government.  Neither the United States Government 
## nor the United States Department of Energy, nor Battelle, nor any of 
## their employees, nor any jurisdiction or organization that has cooperated 
## in the development of these materials, makes any warranty, express or 
## implied, or assumes any legal liability or responsibility for the accuracy, 
## completeness, or usefulness or any information, apparatus, product, 
## software, or process disclosed, or represents that its use would not 
## infringe privately owned rights.
##
## Reference herein to any specific commercial product, process, or service 
## by trade name, trademark, manufacturer, or otherwise does not necessarily 
## constitute or imply its endorsement, recommendation, or favoring by the 
## United States Government or any agency thereof, or Battelle Memorial 
## Institute. The views and opinions of authors expressed herein do not 
## necessarily state or reflect those of the United States Government or 
## any agency thereof.
##
##                      PACIFIC NORTHWEST NATIONAL LABORATORY
##                                  operated by
##                                    BATTELLE
##                                    for the
##                      UNITED STATES DEPARTMENT OF ENERGY
##                         under Contract DE-AC05-76RL01830
##
## Copyright 2019 Battelle Memorial Institute
## Licensed under the Apache License, Version 2.0 (the "License");
## you may not use this file except in compliance with the License.
## You may obtain a copy of the License at
##
##    https://www.apache.org/licenses/LICENSE-2.0 
##
## Unless required by applicable law or agreed to in writing, software
## distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
## WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
## License for the specific language governing permissions and limitations
###############################################################################

# Matrix multiplication GPU examples
set(MM_GPU_EXAMPLES
    mm_tile_naive
    mm_tile
    mm_tile_blas
    mm_thrust
    mm_tile_red
    mm_tile_red_big
)

# Examples that require BLAS
set(MM_BLAS_EXAMPLES mm_tile_blas mm_tile_red mm_tile_red_big mm_thrust)

# Examples that require Thrust
set(MM_THRUST_EXAMPLES mm_thrust)

# Find AMD packages once outside the loop
if(GPU_PLATFORM STREQUAL "AMD")
    find_package(hipblas REQUIRED)
    find_package(rocthrust REQUIRED)
endif()

function(add_mm_gpu_example example_name)
    add_executable(${example_name} ${example_name}.cu mm_util.c)
    target_include_directories(${example_name} PRIVATE
        ${ARTS_PUBLIC_INCLUDE_DIR} ${ARTS_INTERNAL_INCLUDE_DIR}
        ${ARTS_BUILD_INTERNAL_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
    arts_link_gpu_library(${example_name})
    target_link_libraries(${example_name} PRIVATE
        Threads::Threads ${RT_LIBRARY} ${M_LIBRARY} ${ANL_LIBRARY} ${GPU_LINK_LIBS})

    if(${example_name} IN_LIST MM_BLAS_EXAMPLES)
        if(GPU_PLATFORM STREQUAL "AMD")
            target_link_libraries(${example_name} PRIVATE roc::hipblas)
        else()
            target_link_libraries(${example_name} PRIVATE CUDA::cublas)
        endif()
    endif()

    if(${example_name} IN_LIST MM_THRUST_EXAMPLES)
        if(GPU_PLATFORM STREQUAL "AMD")
            target_link_libraries(${example_name} PRIVATE roc::rocthrust)
        endif()
    endif()

    # rocthrust links hip::device which already provides -x hip and --offload-arch
    set(_skip_flags)
    if(${example_name} IN_LIST MM_THRUST_EXAMPLES AND GPU_PLATFORM STREQUAL "AMD")
        set(_skip_flags SKIP_HIPCC_FLAGS)
    endif()
    arts_gpu_set_cu_sources(${example_name} ${_skip_flags} ${example_name}.cu)
endfunction()

# Create GPU matrix multiplication examples
foreach(example ${MM_GPU_EXAMPLES})
    add_mm_gpu_example(${example})
endforeach()