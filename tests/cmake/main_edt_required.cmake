# Run by ctest, not compiled.  main_edt is the one entry a program defines:
# arts_rt, an inline function of arts.h, hands the runtime main_edt's address
# from the program's own unit, so the program holds the only reference and the
# library names no main_edt at all.  A program without it therefore fails to
# link, and the reference pulls a main_edt out of a static archive and binds
# within a program built with hidden visibility.  Links alone cannot tell this
# from a library that refers to main_edt weakly (such a library links in both
# positive shapes too), so the symbol tables are read as well.
#
# INPUTS names the file that sets the compiler, nm and ar, the tree's runtime
# libraries (SHARED_LIB empty when the tree builds none), the public include
# directory, the tree's C flags, directory link options, executable linker
# flags and non-PIE options, the toolchain libraries an installed consumer
# links, and WORK_DIR, where the programs are built.
if(NOT DEFINED INPUTS OR NOT EXISTS "${INPUTS}")
    message(FATAL_ERROR "FAIL main_edt_required: no inputs file '${INPUTS}'")
endif()
include("${INPUTS}")
foreach(_v C_COMPILER NM AR STATIC_LIB INCLUDE_DIR WORK_DIR)
    if(NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "FAIL main_edt_required: ${INPUTS} sets no ${_v}")
    endif()
endforeach()
foreach(_lib IN LISTS STATIC_LIB SHARED_LIB)
    if(NOT EXISTS "${_lib}")
        message(FATAL_ERROR "FAIL main_edt_required: ${_lib} is not built")
    endif()
endforeach()

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

set(_signature "uint32_t paramc, const uint64_t *paramv, uint32_t depc,
              arts_edt_dep_t depv[]")
file(WRITE "${WORK_DIR}/main_only.c" "#include \"arts.h\"

int main(int argc, char **argv) { return arts_rt(argc, argv); }
")
file(WRITE "${WORK_DIR}/entry_member.c" "#include \"arts.h\"

void main_edt(${_signature}) {
  (void)paramc;
  (void)paramv;
  (void)depc;
  (void)depv;
  arts_shutdown();
}
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
_compile(entry_member.c entry_member.o)

set(_undefined "undefined reference to [`']main_edt'|undefined symbol: main_edt")

# (1) No main_edt: the link fails, naming it.
foreach(_lib IN LISTS STATIC_LIB SHARED_LIB)
    get_filename_component(_name "${_lib}" NAME)
    _link(no_entry_${_name} _rc _out "${WORK_DIR}/main_only.o" "${_lib}")
    if(_rc EQUAL 0)
        _fail("(1) a program without main_edt linked against ${_name}")
    elseif(NOT _out MATCHES "${_undefined}")
        _fail("(1) the failed link against ${_name} names no main_edt:\n${_out}")
    else()
        message("main_edt_required: (1) no main_edt does not link against "
                "${_name}")
    endif()
endforeach()

# (2) main_edt only in a static archive member: the program's own reference
# pulls the member, whichever side of the runtime the archive sits on.
execute_process(
    COMMAND "${AR}" rcs "${WORK_DIR}/libentry.a" "${WORK_DIR}/entry_member.o"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "FAIL main_edt_required: ${AR} failed:\n${_out}")
endif()
set(_orders "archive_first;archive_last")
foreach(_order IN LISTS _orders)
    if(_order STREQUAL "archive_first")
        set(_inputs "${WORK_DIR}/main_only.o" "${WORK_DIR}/libentry.a"
            "${STATIC_LIB}")
    else()
        set(_inputs "${WORK_DIR}/main_only.o" "${STATIC_LIB}"
            "${WORK_DIR}/libentry.a")
    endif()
    _link(member_${_order} _rc _out ${_inputs})
    if(NOT _rc EQUAL 0)
        _fail("(2) main_edt in an archive member (${_order}) does not "
              "link:\n${_out}")
        continue()
    endif()
    _nm(_syms "${WORK_DIR}/member_${_order}" main_edt)
    if(NOT _syms STREQUAL "T main_edt")
        _fail("(2) the program linked with the archive (${_order}) defines "
              "no main_edt of its own: '${_syms}'")
    else()
        message("main_edt_required: (2) the archive member (${_order}) links "
                "and the program defines T main_edt")
    endif()
endforeach()

# (3) A program built with hidden visibility binds main_edt within itself.
if(SHARED_LIB)
    _compile(whole.c whole_hidden.o -fvisibility=hidden)
    _link(hidden_shared _rc _out "${WORK_DIR}/whole_hidden.o" "${SHARED_LIB}")
    if(NOT _rc EQUAL 0)
        _fail("(3) a -fvisibility=hidden program does not link against "
              "the shared library:\n${_out}")
    else()
        message("main_edt_required: (3) a -fvisibility=hidden program links "
                "against the shared library")
    endif()
else()
    message("main_edt_required: (3) not checked: the tree builds no shared "
            "library")
endif()

# (4) The library names no main_edt, in any form: no definition, no undefined
# or weak reference.  (5) It defines the entry arts_rt calls, and arts_rt,
# being inline, is no symbol of it.
foreach(_lib IN LISTS STATIC_LIB SHARED_LIB)
    get_filename_component(_name "${_lib}" NAME)
    if(_lib STREQUAL STATIC_LIB)
        set(_nm_args "")
    else()
        set(_nm_args -D)
    endif()
    _nm(_syms "${_lib}" main_edt ${_nm_args})
    if(_syms)
        _fail("(4) ${_name} names main_edt: '${_syms}'")
    else()
        message("main_edt_required: (4) ${_name} names no main_edt")
    endif()
    _nm(_entry "${_lib}" arts_rt_start ${_nm_args})
    _nm(_inline "${_lib}" arts_rt ${_nm_args})
    if(NOT _entry STREQUAL "T arts_rt_start")
        _fail("(5) ${_name} does not define the global arts_rt_start: "
              "'${_entry}'")
    elseif(_inline)
        _fail("(5) ${_name} has an arts_rt symbol: '${_inline}'")
    else()
        message("main_edt_required: (5) ${_name} defines T arts_rt_start and "
                "no arts_rt")
    endif()
endforeach()

if(_failed)
    list(LENGTH _failed _n)
    message(FATAL_ERROR "FAIL main_edt_required: ${_n} check(s) failed")
endif()
message(STATUS "PASS main_edt_required")
