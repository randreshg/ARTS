# Download and build GASNet-EX from its official release tarball into the build
# tree — the same source-build approach as third_party/hwloc. Included from the
# top-level CMakeLists when ARTS_USE_GASNET=ON and no external ARTS_GASNET_PREFIX
# is supplied. On success it sets, in the including scope:
#     ARTS_GASNET_PREFIX     -> the in-build install dir
#     ARTS_GASNET_CONDUIT    -> the resolved conduit
#     ARTS_GASNET_THREADMODE -> par
# which GasnetFlags.cmake then consumes. The release tarball ships a pre-generated
# configure, so no autoconf/automake bootstrap is needed.

set(_gex_ver "${ARTS_GASNET_VERSION}")
# SHA256 pinned for the default release; cleared for an overridden version unless
# the caller supplies ARTS_GASNET_SHA256, so a custom version is not rejected
# against the wrong hash.
set(_gex_default_ver "2025.8.0")
set(_gex_default_sha "bd5919099477d1d2f59c247d006e9d1ac017c9190c974f5e069667418e5bf48d")
if(ARTS_GASNET_SHA256)
  set(_gex_sha "${ARTS_GASNET_SHA256}")
elseif(_gex_ver STREQUAL _gex_default_ver)
  set(_gex_sha "${_gex_default_sha}")
else()
  set(_gex_sha "")
endif()

# ---- resolve the conduit (explicit wins; else probe the actual fabric) -------
# Naive lib-presence detection is wrong on RoCE (ofi silently falls back to TCP,
# ibv does not support RoCE); ucx is the validated RoCE-RDMA path. So probe the
# verbs link layer, not just installed libraries.
set(_conduit "${ARTS_GASNET_CONDUIT}")
if(_conduit)
  message(STATUS "GASNet conduit (explicit): ${_conduit}")
else()
  set(_conduit "udp")
  find_program(_GEX_IBV_DEVINFO ibv_devinfo)
  if(_GEX_IBV_DEVINFO)
    execute_process(COMMAND ${_GEX_IBV_DEVINFO}
      OUTPUT_VARIABLE _ibv_out ERROR_QUIET RESULT_VARIABLE _ibv_rc)
    find_package(PkgConfig QUIET)
    if(_ibv_out MATCHES "link_layer:[ \t]+Ethernet")
      # RoCE fabric: prefer ucx (real RDMA here), else ofi, else udp.
      if(PKG_CONFIG_FOUND)
        pkg_check_modules(_GEX_UCX QUIET ucx)
        pkg_check_modules(_GEX_OFI QUIET libfabric)
      endif()
      if(_GEX_UCX_FOUND)
        set(_conduit "ucx")
      elseif(_GEX_OFI_FOUND)
        set(_conduit "ofi")
      endif()
    elseif(_ibv_out MATCHES "link_layer:[ \t]+InfiniBand")
      set(_conduit "ibv")
    endif()
  endif()
  message(STATUS "GASNet conduit auto-selected: ${_conduit} "
                 "(override with -DARTS_GASNET_CONDUIT=udp|smp|ofi|ibv|ucx)")
endif()

# ---- ensure build prereqs + the conduit's libraries (auto-install, else udp) --
execute_process(
  COMMAND bash ${CMAKE_SOURCE_DIR}/cmake/ensure-gasnet-deps.sh ${_conduit}
  RESULT_VARIABLE _gex_dep_rc)
if(NOT _gex_dep_rc EQUAL 0)
  if(_conduit STREQUAL "udp")
    message(FATAL_ERROR "GASNet udp build prerequisites are missing (see above).")
  endif()
  message(WARNING "GASNet '${_conduit}' dependencies unavailable; "
                  "falling back to the dependency-free udp conduit.")
  set(_conduit "udp")
endif()

# ---- paths + cache guard -----------------------------------------------------
set(_gex_root    "${CMAKE_BINARY_DIR}/third_party")
set(_gex_tarball "${_gex_root}/GASNet-${_gex_ver}.tar.gz")
set(_gex_srcdir  "${_gex_root}/gasnet_src/GASNet-${_gex_ver}")
set(_gex_builddir "${_gex_root}/gasnet_build")
set(_gex_install "${_gex_root}/gasnet_install")
set(_gex_mak     "${_gex_install}/include/${_conduit}-conduit/${_conduit}-par.mak")

if(NOT EXISTS "${_gex_mak}")
  # download (cached)
  if(NOT EXISTS "${_gex_tarball}")
    set(_gex_url "https://gasnet.lbl.gov/EX/GASNet-${_gex_ver}.tar.gz")
    message(STATUS "Downloading GASNet-EX ${_gex_ver} from ${_gex_url}")
    if(_gex_sha)
      file(DOWNLOAD "${_gex_url}" "${_gex_tarball}"
        EXPECTED_HASH SHA256=${_gex_sha} SHOW_PROGRESS STATUS _gex_dl)
    else()
      message(WARNING "No pinned SHA256 for GASNet ${_gex_ver}; downloading without integrity check")
      file(DOWNLOAD "${_gex_url}" "${_gex_tarball}" SHOW_PROGRESS STATUS _gex_dl)
    endif()
    list(GET _gex_dl 0 _gex_dl_rc)
    if(NOT _gex_dl_rc EQUAL 0)
      file(REMOVE "${_gex_tarball}")
      list(GET _gex_dl 1 _gex_dl_msg)
      message(FATAL_ERROR "GASNet download failed: ${_gex_dl_msg}")
    endif()
  endif()

  # extract (cached)
  if(NOT EXISTS "${_gex_srcdir}/configure")
    file(MAKE_DIRECTORY "${_gex_root}/gasnet_src")
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf "${_gex_tarball}"
      WORKING_DIRECTORY "${_gex_root}/gasnet_src" RESULT_VARIABLE _gex_x_rc)
    if(NOT _gex_x_rc EQUAL 0)
      message(FATAL_ERROR "Failed to extract ${_gex_tarball}")
    endif()
  endif()

  # configure: enable the resolved conduit + the portable udp/smp, disable the
  # rest so the build is lean and deterministic. -fPIC so the static GASNet libs
  # embed into the shared libarts.
  set(_gex_enable "")
  foreach(_c ibv ofi ucx mpi udp smp)
    if(_c STREQUAL _conduit OR _c STREQUAL "udp" OR _c STREQUAL "smp")
      list(APPEND _gex_enable "--enable-${_c}")
    else()
      list(APPEND _gex_enable "--disable-${_c}")
    endif()
  endforeach()
  file(MAKE_DIRECTORY "${_gex_builddir}")
  message(STATUS "Configuring GASNet-EX (conduit=${_conduit}, prefix=${_gex_install})")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
      CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER}
      CFLAGS=-fPIC CXXFLAGS=-fPIC
      "${_gex_srcdir}/configure" --prefix=${_gex_install} ${_gex_enable}
    WORKING_DIRECTORY "${_gex_builddir}" RESULT_VARIABLE _gex_cfg_rc)
  if(NOT _gex_cfg_rc EQUAL 0)
    message(FATAL_ERROR "GASNet configure failed (conduit=${_conduit})")
  endif()

  include(ProcessorCount)
  ProcessorCount(_gex_np)
  if(_gex_np EQUAL 0)
    set(_gex_np 4)
  endif()
  message(STATUS "Building + installing GASNet-EX (-j${_gex_np})")
  execute_process(COMMAND make -j${_gex_np}
    WORKING_DIRECTORY "${_gex_builddir}" RESULT_VARIABLE _gex_mk_rc)
  if(NOT _gex_mk_rc EQUAL 0)
    message(FATAL_ERROR "GASNet build failed (conduit=${_conduit})")
  endif()
  execute_process(COMMAND make install
    WORKING_DIRECTORY "${_gex_builddir}" RESULT_VARIABLE _gex_in_rc)
  if(NOT _gex_in_rc EQUAL 0)
    message(FATAL_ERROR "GASNet install failed (conduit=${_conduit})")
  endif()
endif()

if(NOT EXISTS "${_gex_mak}")
  message(FATAL_ERROR "GASNet bootstrap did not produce ${_gex_mak}")
endif()

# Hand the bootstrapped install to GasnetFlags.cmake.
set(ARTS_GASNET_PREFIX "${_gex_install}")
set(ARTS_GASNET_CONDUIT "${_conduit}")
set(ARTS_GASNET_THREADMODE "par")
message(STATUS "GASNet-EX bootstrapped: prefix=${ARTS_GASNET_PREFIX} conduit=${_conduit}")
