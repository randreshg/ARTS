# arts_resolve_atomic_link(<LANG>)
#
# Sets ARTS_ATOMIC_LINK, the single link item every consumer of libatomic
# uses: the library's path when a search finds one; the runtime SONAME's
# file when a host carries libatomic only as that -- a toolchain packaged
# without its development symlink, a distribution's runtime-only package --
# looked for in the toolchain's own library directories first, which
# find_library() does not search; the bare name `atomic` when only the
# linker can see it; empty when nothing resolves.  A name the linker cannot
# resolve fails every link that inherits it, so the bare name is never
# emitted without probing that the linker accepts it.  <LANG> is the enabled
# language whose toolchain performs the link.
function(arts_resolve_atomic_link lang)
    find_library(ATOMIC_LIBRARY NAMES atomic)
    if(NOT ATOMIC_LIBRARY)
        find_file(ATOMIC_LIBRARY_SONAME libatomic.so.1
            PATHS ${CMAKE_${lang}_IMPLICIT_LINK_DIRECTORIES}
                  /usr/lib/x86_64-linux-gnu /usr/lib /usr/local/lib /usr/lib64
            NO_DEFAULT_PATH)
        if(ATOMIC_LIBRARY_SONAME)
            set(ATOMIC_LIBRARY "${ATOMIC_LIBRARY_SONAME}" CACHE FILEPATH
                "Path to libatomic" FORCE)
        endif()
    endif()
    if(ATOMIC_LIBRARY)
        set(_link "${ATOMIC_LIBRARY}")
    else()
        include(CheckLinkerFlag)
        check_linker_flag(${lang} "-latomic" ARTS_HAVE_LATOMIC_FLAG)
        if(ARTS_HAVE_LATOMIC_FLAG)
            set(_link "atomic")
        else()
            set(_link "")
        endif()
    endif()
    set(ARTS_ATOMIC_LINK "${_link}" PARENT_SCOPE)
endfunction()
