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

cmake_minimum_required(VERSION 3.8)
project(arts LANGUAGES C CXX)

# Set default build type to Debug if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build (Debug or Release)" FORCE)
    message(STATUS "Build type not specified, defaulting to Debug")
endif()

#CMake Configuration
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${PROJECT_SOURCE_DIR}/cmake")

if("${CMAKE_C_COMPILER_ID}" STREQUAL "XL")
    message("Using XL. Not loading new GLIBC")
else()
    include(glibc)
    CHECK_GLIBC_VERSION()
    if(GLIBC_VERSION)
        message("GLIBC Version Available: ${GLIBC_VERSION}")
        if((${GLIBC_VERSION} VERSION_EQUAL 2.19) OR (${GLIBC_VERSION} VERSION_GREATER 2.19))
            add_definitions(-DGLIBC_NEW)
            message("Using new GLIBC, added -DGLIBC_NEW")
        endif()
    endif()
endif()

## Compiler flags
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message("Debug Build")
    # Do not use comma. If you do, this will cause errors because the flags will be passed to nvcc by -Xcompiler.
    set(DEBUG_FLAGS "-O0 -fPIC -Wall -Wextra -Werror")
    set(SANITIZER_FLAGS "-fsanitize=address -fsanitize=undefined -fsanitize=leak")
    set(DEBUG_IGNORE_FLAGS "-Wno-unused -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-unused-value -Wno-sign-compare -Wno-implicit-fallthrough")

    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        set(DEBUG_IGNORE_FLAGS "${DEBUG_IGNORE_FLAGS} -Wno-stringop-truncation")
    elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    endif()

    # Apply compile flags
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${DEBUG_FLAGS} ${SANITIZER_FLAGS} ${DEBUG_IGNORE_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${DEBUG_FLAGS} ${SANITIZER_FLAGS} ${DEBUG_IGNORE_FLAGS}")

    # Apply sanitizer flags to linker as well
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${SANITIZER_FLAGS}")
else()
    message("Release Build")

    # Release flags for maximum performance
    set(RELEASE_FLAGS "-fPIC -march=native -mtune=native")

    # Apply compile flags
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${RELEASE_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${RELEASE_FLAGS}")
endif()

# Set C and C++ standards
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu17")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++17")

include(CheckLanguage)
check_language(CUDA)

if(CMAKE_CUDA_COMPILER)
    set(CMAKE_CUDA_SEPARABLE_COMPILATION ON)
    enable_language(CUDA)
    find_package(CUDA REQUIRED)

    if(CUDA_FOUND)
        include_directories("${CUDA_INCLUDE_DIRS}")
        add_definitions(-DUSE_GPU)
        message("Using CUDA, added -DUSE_GPU")

        # Auto-detect CUDA architecture
        find_package(cudaArch)

        if(CUDAARCH_FOUND)
            set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -arch=sm_${CUDA_ARCHITECTURES}")
            message(STATUS "Setting CUDA architecture to: ${CUDA_ARCHITECTURES}")
        endif()

        find_package(cuBlas)

        if(CUBLAS_FOUND)
            include_directories(${CUBLAS_INCLUDE_DIRS})
            add_definitions(-DUSE_CUBLAS)
            message("Using CUBLAS, added -DUSE_CUBLAS")
        else()
            set(CUBLAS_LIBRARIES "")
        endif()

        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            message("CUDA Debug Build")
            set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -G")
        else()
            message("CUDA Release Build")
        endif()
        set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -std=c++17 -ccbin=${CMAKE_CXX_COMPILER} -Xcompiler=\"${CMAKE_CXX_FLAGS}\"")
    else()
        set(CUDA_LIBRARIES "")
    endif()
else()
    message(STATUS "No CUDA support")
endif()

#Find Required Libs-------------------------->

find_package(Threads)
find_library(PTHREAD_LIB pthread)

if(CONNECTION MATCHES "TCP")
    message("Using TCP, added -DUSE_TCP")
    add_definitions(-DUSE_TCP)
else()
    find_library(RDMACM_LIB rdmacm)
    if(RDMACM_LIB)
        find_path(RSOCKET_INCLUDE rdma/rsocket.h)
        include_directories(${RSOCKET_INCLUDE})
        message("Using RDMA RSockets, added -DUSE_RSOCKETS")
        add_definitions(-DUSE_RSOCKETS)
    else()
        set(RDMACM_LIB "")
        set(CONNECTION "TCP")
        message("Using TCP, added -DUSE_TCP")
        add_definitions(-DUSE_TCP)
    endif()
endif()

find_package(Hwloc)
if(HWLOC_FOUND)
    include_directories(${HWLOC_INC})
    add_definitions(-DHWLOC)
    message("Using HWLOC, added -DHWLOC")
else()
    set(HWLOC_LIB "")
    message("Not using HWLOC")
endif()

#<-------------------------------------------

if(SIMPLE_ARCH)
    message("Using simple CAS2 and BIT_TEST_AND_SET, added -DSIMPLE_ARCH")
    add_definitions(-DSIMPLE_ARCH)
endif()

#DEFAULT SETTINGS
# DEQUE can be set to "chase_lev" or "priority"
if(NOT DEFINED DEQUE)
    set(DEQUE "chase_lev")
endif(NOT DEFINED DEQUE)

if(APPLE)
    set(RT_LIB "")
else()
    find_library(RT_LIB rt)
endif()

if(COUNT)
    message("Counting on, added -DCOUNT")
    add_definitions(-DCOUNT)
endif()

if(MODELCOUNT)
    message("Model Counting only, added -DMODELCOUNT")
    add_definitions(-DMODELCOUNT)
endif()

if(INSPECTOR)
    message("Inspection on, added -DINSPECTOR")
    add_definitions(-DINSPECTOR)
endif()

if(SMART_DB)
    message("Smart DB on, added -DSMART_DB, currently SMART_DB is not supported")
    add_definitions(-DSMART_DB)
else()
endif()

set(CORE_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/core/inc)

include_directories(core/inc)
include_directories(graph/inc)
add_subdirectory(sampleConfigs)
add_subdirectory(core)
add_subdirectory(graph)
add_subdirectory(test)
add_subdirectory(example)
