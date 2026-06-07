# Extract GASNet-EX build flags from an installed conduit makefile fragment.
#
# Inputs (cache variables):
#   ARTS_GASNET_PREFIX     - GASNet install prefix (contains include/<conduit>-conduit/)
#   ARTS_GASNET_CONDUIT    - conduit name: udp | smp | ofi | ibv | ucx | mpi
#   ARTS_GASNET_THREADMODE - threading mode: par | seq | parsync (ARTS needs par)
#
# Outputs (set in including scope):
#   GASNET_CC GASNET_CXX GASNET_CPPFLAGS GASNET_CFLAGS
#   GASNET_LD GASNET_LDFLAGS GASNET_LIBS
#
# GASNet ships per-conduit makefile fragments; the supported way to consume them
# is to `include` the fragment in a makefile and read its variables. We do that
# with a tiny generated makefile + `make`.

if(NOT ARTS_GASNET_PREFIX)
  message(FATAL_ERROR "ARTS_USE_GASNET=ON requires -DARTS_GASNET_PREFIX=<gasnet install>")
endif()
if(NOT ARTS_GASNET_CONDUIT)
  set(ARTS_GASNET_CONDUIT "udp")
endif()
if(NOT ARTS_GASNET_THREADMODE)
  set(ARTS_GASNET_THREADMODE "par")
endif()

set(_gex_mak
    "${ARTS_GASNET_PREFIX}/include/${ARTS_GASNET_CONDUIT}-conduit/${ARTS_GASNET_CONDUIT}-${ARTS_GASNET_THREADMODE}.mak")
if(NOT EXISTS "${_gex_mak}")
  message(FATAL_ERROR "GASNet conduit fragment not found: ${_gex_mak}\n"
                      "Check ARTS_GASNET_PREFIX / ARTS_GASNET_CONDUIT / ARTS_GASNET_THREADMODE.")
endif()

set(_gex_tmp "${CMAKE_BINARY_DIR}/arts_gasnet_flags.mak")
file(WRITE "${_gex_tmp}"
     "include ${_gex_mak}\n"
     "print-%:\n\t@echo \$(\$*)\n")

foreach(_var GASNET_CC GASNET_CXX GASNET_CPPFLAGS GASNET_CFLAGS
             GASNET_LD GASNET_LDFLAGS GASNET_LIBS)
  execute_process(
    COMMAND make -s -f "${_gex_tmp}" "print-${_var}"
    OUTPUT_VARIABLE _gex_val
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _gex_rc)
  if(NOT _gex_rc EQUAL 0)
    message(FATAL_ERROR "Failed to read ${_var} from ${_gex_mak}")
  endif()
  set(${_var} "${_gex_val}")
endforeach()

message(STATUS "GASNet conduit: ${ARTS_GASNET_CONDUIT}-${ARTS_GASNET_THREADMODE} (${ARTS_GASNET_PREFIX})")
message(STATUS "GASNet CPPFLAGS: ${GASNET_CPPFLAGS}")
message(STATUS "GASNet LIBS: ${GASNET_LIBS}")
