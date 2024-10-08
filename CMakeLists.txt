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
project (arts LANGUAGES C CXX)

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
      endif()
   endif()
endif()

## Compiler flags
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message("Debug Build")
    set(CMAKE_C_FLAGS   "-Werror -O0 -g -fsanitize=address -fsanitize=signed-integer-overflow -fPIC -Wno-error=no-stringop-overflow")
    set(CMAKE_CXX_FLAGS "-Werror -O0 -g -fsanitize=address -fsanitize=signed-integer-overflow -fPIC -Wno-error=no-stringop-overflow")
else()
    message("Release Build")
    set(CMAKE_C_FLAGS   "-std=gnu99 -O3 -g -ftree-vectorize -mtune=native -fPIC")
    set(CMAKE_CXX_FLAGS " -O3 -g -ftree-vectorize -mtune=native -fPIC")
endif()


include(CheckLanguage)
# check_language(CUDA)
# if(CMAKE_CUDA_COMPILER)
#     set(CMAKE_CUDA_SEPARABLE_COMPILATION ON)
#     #This is currently set for Volata.  To use other change sm_60
#     set(CMAKE_CUDA_FLAGS "-arch=sm_70") 
#     enable_language(CUDA)
#     find_package(CUDA REQUIRED)
#     # find_package(CUDA 9)
#     if(CUDA_FOUND)
#         include_directories("${CUDA_INCLUDE_DIRS}")
#         add_definitions(-DUSE_GPU)
#         message("Using CUDA")
#         find_package(cuBlas)
#         if(CUBLAS_FOUND)
#             include_directories(${CUBLAS_INCLUDE_DIRS})
#             add_definitions(-DUSE_CUBLAS)
#             message("Using CUBLAS")
#         else()
#             set(CUBLAS_LIBRARIES "")
#         endif()
#         if(CMAKE_BUILD_TYPE STREQUAL "Debug")
#           message("CUDA Debug Build")
#           set(CMAKE_CUDA_FLAGS " -G")
#         endif()
#     else()
#         set(CUDA_LIBRARIES "")
#     endif()
# else()
#     message(STATUS "No CUDA support")
# endif()

#Find Required Libs-------------------------->

find_package(Threads)
find_library(PTHREAD_LIB pthread)

if(CONNECTION MATCHES "TCP")
    message("Using TCP")
    add_definitions(-DUSE_TCP)
else()
    find_library(RDMACM_LIB rdmacm)
    find_path(RSOCKET_INCLUDE rdma/rsocket.h)
    if(RDMACM_LIB)
        include_directories(${RSOCKET_INCLUDE})
        message("Using RDMA RSockets")
        add_definitions(-DUSE_RSOCKETS)
    else()
        set(RDMACM_LIB "")
        set(RSOCKET_INCLUDE "")
        set(CONNECTION "TCP")
        message("Using TCP")
        add_definitions(-DUSE_TCP)
    endif()
endif()

find_package(Hwloc)
if(HWLOC_FOUND)
    include_directories(${HWLOC_INC})
    add_definitions(-DHWLOC)
    message("Using HWLOC")
else()
    set(HWLOC_LIB "")
    message("Not using HWLOC")
endif()

#<-------------------------------------------

if(SIMPLE_ARCH MATCHES "1")
   message("Using simple CAS2 adn BIT_TEST_SET")
   add_definitions(-DSIMPLE_ARCH=1)
endif()

#DEFAULT SETTINGS
if(NOT DEFINED DEQUE)
    set(DEQUE "chase_lev")
endif(NOT DEFINED DEQUE)

if(NOT DEFINED MALLOC)
    set(MALLOC "Standard")
elseif(${MALLOC} MATCHES "FOA")
    add_definitions(-DFOA)
endif()

if(APPLE)
    set(RT_LIB "")
else()
    find_library(RT_LIB rt)
endif()

if(COUNT)
    message("Counting on")
    add_definitions(-DCOUNT)
endif()

if(MODELCOUNT)
    message("Model Counting only")
    add_definitions(-DMODELCOUNT)
endif()

if(INSPECTOR)
    message("Inspection on")
    add_definitions(-DINSPECTOR)

endif()

message("Using ${MALLOC} malloc.")

set(CORE_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/core/inc)

include_directories(core/inc)
include_directories(graph/inc)
add_subdirectory(sampleConfigs)
add_subdirectory(core)
add_subdirectory(graph)
add_subdirectory(test)
add_subdirectory(example)
