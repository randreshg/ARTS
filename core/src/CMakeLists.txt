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
add_subdirectory(common)
add_subdirectory(gas)
add_subdirectory(introspection)
add_subdirectory(network)
add_subdirectory(system)
add_subdirectory(runtime)
add_subdirectory(gpu)

set(main_inc
    ${CORE_INCLUDE_DIR}/artsRT.h
    ${CORE_INCLUDE_DIR}/arts.h
    ${CORE_INCLUDE_DIR}/artsGlobals.h
    ${CORE_INCLUDE_DIR}/artsConfig.h
    ${CORE_INCLUDE_DIR}/shadAdapter.h
)

set(main_src
    main.c
    artsConfig.c
    artsUtil.c
)

if(CUDA_FOUND)
    add_library(arts STATIC ${main_inc} ${main_src} 
                    $<TARGET_OBJECTS:gas> 
                    $<TARGET_OBJECTS:network> 
                    $<TARGET_OBJECTS:introspection> 
                    $<TARGET_OBJECTS:runtime> 
                    $<TARGET_OBJECTS:system> 
                    $<TARGET_OBJECTS:common>
                    $<TARGET_OBJECTS:gpu>
                )
    add_library(carts SHARED ${main_inc} ${main_src} 
                    $<TARGET_OBJECTS:gas> 
                    $<TARGET_OBJECTS:network> 
                    $<TARGET_OBJECTS:introspection> 
                    $<TARGET_OBJECTS:runtime> 
                    $<TARGET_OBJECTS:system> 
                    $<TARGET_OBJECTS:common>
                    $<TARGET_OBJECTS:gpu>
                )
else()
    # add_library(arts STATIC ${main_inc} ${main_src} 
    #                 $<TARGET_OBJECTS:gas> 
    #                 $<TARGET_OBJECTS:network> 
    #                 $<TARGET_OBJECTS:introspection> 
    #                 $<TARGET_OBJECTS:runtime> 
    #                 $<TARGET_OBJECTS:system> 
    #                 $<TARGET_OBJECTS:common>
    #             )
    add_library(arts SHARED ${main_inc} ${main_src} 
        $<TARGET_OBJECTS:gas> 
        $<TARGET_OBJECTS:network> 
        $<TARGET_OBJECTS:introspection> 
        $<TARGET_OBJECTS:runtime> 
        $<TARGET_OBJECTS:system> 
        $<TARGET_OBJECTS:common>
    )
    message("PATHS INSTALLED: " ${M_LIB} ${RT_LIB} ${PTHREAD_LIB} ${RDMACM_LIB} ${HWLOC_LIB})
endif()

install(FILES ${main_inc} DESTINATION include)
install(TARGETS arts ARCHIVE DESTINATION lib)
# install(TARGETS carts ARCHIVE DESTINATION lib)