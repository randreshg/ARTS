# Run by ctest, not compiled.  The CXL store orders a device's view of memory
# with AMD64 instructions, so a configure that asks for it on any other target
# must fail, and must fail before anything vendored is configured: a refusal
# that came after the vendored builds would leave a half-configured tree and
# cost the user every configure-time build first.
#
# The nested configure uses the host compilers with only the target processor
# overridden, which is all the refusal reads, so no cross toolchain is needed.
# Every configure-time vendored build lands under <tree>/third_party, so its
# absence after the failure is what shows the refusal came first.
foreach(_v SOURCE_DIR WORK_DIR C_COMPILER CXX_COMPILER MAKE_PROGRAM)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "${_v} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
execute_process(
    COMMAND ${CMAKE_COMMAND} -G Ninja -S "${SOURCE_DIR}" -B "${WORK_DIR}"
            "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
            "-DCMAKE_C_COMPILER=${C_COMPILER}"
            "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
            -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64
            -DARTS_USE_CXL=ON -DARTS_COHERENCE_PROTOCOL=EXCL
            -DARTS_RELEASE_POLICY=PURGE -DARTS_CXL_RESIDENCY=STAGED
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)

# CMake wraps a message's lines, so the text is compared with its whitespace
# folded.
string(REGEX REPLACE "[ \t\r\n]+" " " _said "${_out} ${_err}")
set(_refusal "ARTS_USE_CXL=ON is refused on target processor aarch64: the "
             "CXL store orders the device's view of memory with AMD64 "
             "cache-flush and fence instructions")
string(CONCAT _refusal ${_refusal})

if(_rc EQUAL 0)
    message(FATAL_ERROR
        "FAIL cxl_arm64_refusal: an aarch64 configure accepted "
        "ARTS_USE_CXL=ON\n${_out}\n${_err}")
endif()
string(FIND "${_said}" "${_refusal}" _at)
if(_at EQUAL -1)
    message(FATAL_ERROR
        "FAIL cxl_arm64_refusal: the configure failed, but not with the "
        "refusal\n${_out}\n${_err}")
endif()
if(EXISTS "${WORK_DIR}/third_party")
    message(FATAL_ERROR
        "FAIL cxl_arm64_refusal: the refusal came after a vendored component "
        "was configured (${WORK_DIR}/third_party exists)")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
message(STATUS "PASS cxl_arm64_refusal")
