# Run by ctest, not compiled.  Every access to an _Atomic object names its
# memory order.  clang-tidy reports one that does not (operator syntax, an
# implicit seq_cst access) in every C unit of this tree's runtime library,
# OCR shim and tests, compiled with the tree's own commands, so each cell's
# conditional code is judged in the tree that compiles it.  A header under the
# same directories is judged wherever a unit includes it.
#
# clang-tidy runs no analysis when its check list names only compiler
# diagnostics, so the list also names one real check, a concurrency check the
# code passes too.
foreach(_v CLANG_TIDY SOURCE_DIR BUILD_DIR)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "${_v} is required")
    endif()
endforeach()

set(_db "${BUILD_DIR}/compile_commands.json")
if(NOT EXISTS "${_db}")
    message(FATAL_ERROR "FAIL atomics_implicit_seq_cst: no ${_db}")
endif()
file(READ "${_db}" _json)
set(_dirs libs benchmarks/ocr_shim tests)
list(JOIN _dirs "|" _alt)
string(REGEX REPLACE "([][.*+?^$(){}|\\])" "\\\\\\1" _root "${SOURCE_DIR}")

# string(JSON) parses the whole document on every call, so the entries'
# "file" members are found in one regex pass and each is decoded alone; the
# one whole-document parse checks that the pass found every entry.
string(JSON _count LENGTH "${_json}")
set(_ws "[ \t\r\n]*")
string(REGEX MATCHALL "\"file\"${_ws}:${_ws}\"([^\"\\\\]|\\\\.)*\""
       _members "${_json}")
list(LENGTH _members _found)
if(NOT _found EQUAL _count)
    message(FATAL_ERROR "FAIL atomics_implicit_seq_cst: ${_db} has ${_count} "
            "entries, but ${_found} file members were found")
endif()
set(_units "")
foreach(_m IN LISTS _members)
    string(JSON _file GET "{${_m}}" file)
    list(APPEND _units "${_file}")
endforeach()
list(FILTER _units INCLUDE REGEX "^${_root}/(${_alt})/.*\\.c$")
list(REMOVE_DUPLICATES _units)
list(LENGTH _units _n)
if(_n EQUAL 0)
    message(FATAL_ERROR "FAIL atomics_implicit_seq_cst: ${_db} names no C "
            "unit under ${SOURCE_DIR}/(${_alt})")
endif()

string(CONCAT _config
       "{Checks: '-*,clang-diagnostic-atomic-implicit-seq-cst,"
       "concurrency-thread-canceltype-asynchronous', WarningsAsErrors: '*'}")
execute_process(
    COMMAND "${CLANG_TIDY}" -p "${BUILD_DIR}" --quiet
            "--header-filter=^${_root}/(${_alt})/" "--config=${_config}"
            --extra-arg=-Watomic-implicit-seq-cst ${_units}
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "FAIL atomics_implicit_seq_cst: clang-tidy exited "
            "${_rc} over ${_n} units\n${_out}\n${_err}")
endif()
message(STATUS "PASS atomics_implicit_seq_cst: ${_n} units")
