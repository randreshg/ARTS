# Run by ctest, not compiled.  main_edt is the one entry a program defines.
# The library defines arts_rt and refers to main_edt only as a weak undefined
# symbol, so a binary that links the runtime without starting it needs no
# entry, and arts_rt refuses to start a program that defines none.  Links and
# runs cannot tell a weak reference from a strong one or from a definition in
# the library, so the symbol tables are read as well.
#
# The runs take their cfg from ARTS_CONFIG, which the test sets; two workers
# are enough for them.  Their output is printed, so the sweeps that judge every
# runtime test see it.
#
# INPUTS names the file that sets the compiler and nm, the tree's runtime
# libraries (SHARED_LIB empty when the tree builds none), the public include
# directory, the tree's C flags, directory link options, executable linker
# flags and non-PIE options, the toolchain libraries an installed consumer
# links, and WORK_DIR, where the programs are built.
if(NOT DEFINED INPUTS OR NOT EXISTS "${INPUTS}")
    message(FATAL_ERROR "FAIL main_edt_required: no inputs file '${INPUTS}'")
endif()
include("${INPUTS}")
foreach(_v C_COMPILER NM STATIC_LIB INCLUDE_DIR WORK_DIR)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "FAIL main_edt_required: ${INPUTS} sets no ${_v}")
    endif()
endforeach()
foreach(_lib IN LISTS STATIC_LIB SHARED_LIB)
    if(NOT EXISTS "${_lib}")
        message(FATAL_ERROR "FAIL main_edt_required: ${_lib} is not built")
    endif()
endforeach()

set(LIB_DIR "")
if(SHARED_LIB)
    get_filename_component(LIB_DIR "${SHARED_LIB}" DIRECTORY)
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# The tree compiles every unit with the sanitizer flags it links with.
set(_sanitizers ${LINK_OPTIONS})
list(FILTER _sanitizers INCLUDE REGEX "^-fsanitize=")
set(_cc_cmd "${C_COMPILER}" ${C_FLAGS} ${_sanitizers}
    ${NON_PIE_COMPILE_OPTIONS} "-I${INCLUDE_DIR}")
set(_ld_cmd "${C_COMPILER}" ${C_FLAGS} ${LINK_OPTIONS} ${EXE_LINKER_FLAGS}
    ${NON_PIE_LINK_OPTIONS})

set(_failed "")
macro(_fail what)
    list(APPEND _failed "${what}")
    message("main_edt_required: FAILED ${what}")
endmacro()

function(_compile src obj)
    execute_process(
        COMMAND ${_cc_cmd} ${ARGN} -c "${WORK_DIR}/${src}"
                -o "${WORK_DIR}/${obj}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "FAIL main_edt_required: ${src} does not "
                "compile:\n${_out}")
    endif()
endfunction()

# Links WORK_DIR/<exe> from the given inputs and the toolchain libraries; sets
# <rc_var> and <out_var>.
function(_link exe rc_var out_var)
    execute_process(
        COMMAND ${_ld_cmd} ${ARGN} ${SYSTEM_LIBS} -o "${WORK_DIR}/${exe}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    set(${rc_var} "${_rc}" PARENT_SCOPE)
    set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()

# Sets <out_var> to the "<type> <name>" entries of nm's listing of <file>
# whose name is <symbol> (a dynamic symbol's version suffix dropped).
function(_nm out_var file symbol)
    execute_process(
        COMMAND "${NM}" ${ARGN} "${file}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "FAIL main_edt_required: ${NM} ${ARGN} ${file} "
                "failed:\n${_err}")
    endif()
    string(REPLACE "\n" ";" _lines "${_out}")
    set(_found "")
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "^[0-9a-fA-F ]* ([A-Za-z]) ${symbol}(@.*)?$")
            list(APPEND _found "${CMAKE_MATCH_1} ${symbol}")
        endif()
    endforeach()
    set(${out_var} "${_found}" PARENT_SCOPE)
endfunction()

# Runs WORK_DIR/<exe> on two workers; sets <rc_var> and <out_var> and prints
# the output.
function(_run exe rc_var out_var)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env LD_LIBRARY_PATH=${LIB_DIR}
                worker_threads=2 "${WORK_DIR}/${exe}"
        WORKING_DIRECTORY "${WORK_DIR}" TIMEOUT 60
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    message("${exe}:\n${_out}")
    set(${rc_var} "${_rc}" PARENT_SCOPE)
    set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()

set(_signature "uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]")
file(WRITE "${WORK_DIR}/main_only.c" "#include \"arts.h\"

int main(int argc, char **argv) { return arts_rt(argc, argv); }
")
file(WRITE "${WORK_DIR}/whole.c" "#include \"arts.h\"

void main_edt(${_signature}) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}

int main(int argc, char **argv) { return arts_rt(argc, argv); }
")
_compile(main_only.c main_only.o)
_compile(whole.c whole.o)
_compile(whole.c whole_hidden.o -fvisibility=hidden)

# (1) No main_edt: the program links, and arts_rt refuses it with exit status
# 1 and the refusal line as its only output, so nothing of the runtime ran.
foreach(_lib IN LISTS STATIC_LIB SHARED_LIB)
    get_filename_component(_name "${_lib}" NAME)
    _link(no_entry_${_name} _rc _out "${WORK_DIR}/main_only.o" "${_lib}")
    if(NOT _rc EQUAL 0)
        _fail("(1) a program without main_edt does not link against "
              "${_name}:\n${_out}")
        continue()
    endif()
    _run(no_entry_${_name} _rc _out)
    if(NOT _rc STREQUAL "1")
        _fail("(1) a program without main_edt ended with '${_rc}' against "
              "${_name}, not 1:\n${_out}")
    elseif(NOT _out MATCHES
            "^\\[ARTS\\] arts_rt: no main_edt reached the runtime[^\n]*\n$")
        _fail("(1) the refusal against ${_name} is not the one line alone:"
              "\n${_out}")
    else()
        message("main_edt_required: (1) a program without main_edt links "
                "against ${_name} and is refused with that line alone")
    endif()
endforeach()

# (2) A program defining main_edt in its own unit links and runs.
foreach(_lib IN LISTS STATIC_LIB SHARED_LIB)
    get_filename_component(_name "${_lib}" NAME)
    _link(own_${_name} _rc _out "${WORK_DIR}/whole.o" "${_lib}")
    if(NOT _rc EQUAL 0)
        _fail("(2) a program defining main_edt does not link against "
              "${_name}:\n${_out}")
        continue()
    endif()
    _run(own_${_name} _rc _out)
    if(NOT _rc EQUAL 0)
        _fail("(2) a program defining main_edt does not run against "
              "${_name} (${_rc}):\n${_out}")
    else()
        message("main_edt_required: (2) a program defining main_edt links "
                "and runs against ${_name}")
    endif()
endforeach()

# (3) A program compiled with hidden visibility by default still exports
# main_edt, whose public declaration gives it default visibility, so the
# runtime sees it through the shared library as well.
foreach(_lib IN LISTS STATIC_LIB SHARED_LIB)
    get_filename_component(_name "${_lib}" NAME)
    _link(hidden_${_name} _rc _out "${WORK_DIR}/whole_hidden.o" "${_lib}")
    if(NOT _rc EQUAL 0)
        _fail("(3) a program compiled with hidden visibility does not link "
              "against ${_name}:\n${_out}")
        continue()
    endif()
    _run(hidden_${_name} _rc _out)
    if(NOT _rc EQUAL 0)
        _fail("(3) a program compiled with hidden visibility does not run "
              "against ${_name} (${_rc}):\n${_out}")
    else()
        message("main_edt_required: (3) a program compiled with hidden "
                "visibility links and runs against ${_name}")
    endif()
endforeach()

# (4) The library refers to main_edt only as a weak undefined symbol.
# (5) It defines arts_rt globally.
foreach(_lib IN LISTS STATIC_LIB SHARED_LIB)
    get_filename_component(_name "${_lib}" NAME)
    if(_lib STREQUAL STATIC_LIB)
        set(_nm_args "")
    else()
        set(_nm_args -D)
    endif()
    _nm(_syms "${_lib}" main_edt ${_nm_args})
    if(NOT _syms STREQUAL "w main_edt")
        _fail("(4) ${_name} does not refer to main_edt only as a weak "
              "undefined symbol: '${_syms}'")
    else()
        message("main_edt_required: (4) ${_name} has w main_edt only")
    endif()
    _nm(_entry "${_lib}" arts_rt ${_nm_args})
    if(NOT _entry STREQUAL "T arts_rt")
        _fail("(5) ${_name} does not define the global arts_rt: '${_entry}'")
    else()
        message("main_edt_required: (5) ${_name} defines T arts_rt")
    endif()
endforeach()

if(_failed)
    list(LENGTH _failed _n)
    message(FATAL_ERROR "FAIL main_edt_required: ${_n} check(s) failed")
endif()
message(STATUS "PASS main_edt_required")
