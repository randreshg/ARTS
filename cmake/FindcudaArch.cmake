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

# Try to detect CUDA architecture automatically
# Input variables:
#   (none - automatic detection)
# Output variables:
#   CUDAARCH_FOUND     - System has detectable CUDA architecture
#   CUDA_ARCHITECTURES - The CUDA architecture numbers (e.g., "80")

include(FindPackageHandleStandardArgs)

if (NOT DEFINED CUDAARCH_FOUND)

# Only proceed if CUDA is available
if(NOT CMAKE_CUDA_COMPILER)
    set(CUDAARCH_FOUND FALSE)
    return()
endif()

# Try to find nvidia-smi
find_program(NVIDIA_SMI nvidia-smi)

if(NVIDIA_SMI)
    # Use nvidia-smi to query GPU compute capability
    execute_process(
        COMMAND ${NVIDIA_SMI} --query-gpu=compute_cap --format=csv,noheader,nounits
        OUTPUT_VARIABLE NVIDIA_SMI_OUTPUT
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    
    if(NVIDIA_SMI_OUTPUT)
        # Get the first (and typically highest) compute capability
        string(REPLACE "\n" ";" COMPUTE_CAPS_LIST "${NVIDIA_SMI_OUTPUT}")
        list(GET COMPUTE_CAPS_LIST 0 FIRST_COMPUTE_CAP)
        
        # Convert from "X.Y" format to "XY" format
        string(REPLACE "." "" CUDA_ARCHITECTURES "${FIRST_COMPUTE_CAP}")
        
        message(STATUS "nvidia-smi detected compute capability: ${FIRST_COMPUTE_CAP}")
    else()
        set(CUDA_ARCHITECTURES "")
    endif()
else()
    message(STATUS "nvidia-smi not found, using fallback detection")
    set(CUDA_ARCHITECTURES "")
endif()

# Check if detection was successful and set architecture
if(CUDA_ARCHITECTURES AND CUDA_ARCHITECTURES MATCHES "^[0-9]+$")
    # Validate the detected compute capability
    set(SUPPORTED_ARCHITECTURES "60;61;62;70;72;75;80;86;87;89;90")
    
    if(CUDA_ARCHITECTURES IN_LIST SUPPORTED_ARCHITECTURES)
        set(CUDAARCH_FOUND TRUE)
        message(STATUS "Using CUDA architecture: ${CUDA_ARCHITECTURES}")
    else()
        # Use the nearest supported architecture
        if(CUDA_ARCHITECTURES GREATER_EQUAL 90)
            set(CUDA_ARCHITECTURES "90")
        elseif(CUDA_ARCHITECTURES GREATER_EQUAL 86)
            set(CUDA_ARCHITECTURES "86")
        elseif(CUDA_ARCHITECTURES GREATER_EQUAL 80)
            set(CUDA_ARCHITECTURES "80")
        elseif(CUDA_ARCHITECTURES GREATER_EQUAL 75)
            set(CUDA_ARCHITECTURES "75")
        elseif(CUDA_ARCHITECTURES GREATER_EQUAL 70)
            set(CUDA_ARCHITECTURES "70")
        elseif(CUDA_ARCHITECTURES GREATER_EQUAL 60)
            set(CUDA_ARCHITECTURES "60")
        else()
            # Architecture too old, set to false
            set(CUDAARCH_FOUND FALSE)
            message(STATUS "Detected CUDA architecture ${CUDA_ARCHITECTURES} is too old (< 6.0)")
            return()
        endif()
        set(CUDAARCH_FOUND TRUE)
        message(STATUS "Using nearest supported CUDA architecture: ${CUDA_ARCHITECTURES}")
    endif()
else()
    # Detection failed
    set(CUDAARCH_FOUND FALSE)
endif()

# Mark variables as advanced
mark_as_advanced(CUDA_ARCHITECTURES)

find_package_handle_standard_args(cudaArch
    FOUND_VAR CUDAARCH_FOUND
    REQUIRED_VARS CUDA_ARCHITECTURES 
    HANDLE_COMPONENTS)

endif()
