# RelocateLLVMDylib.cmake — install-time step that makes an installed Mach-O
# tool relocatable. It rewrites the tool's build-host-absolute libLLVM /
# libclang load command (e.g. /opt/homebrew/opt/llvm/lib/libLLVM.dylib) to
# @rpath/<basename>, so a relocated binary loads the dylib through its
# @loader_path/../lib rpath (a bundled copy) or the BYO loader-path injection
# (topo-core ensureLLVMLoaderPathForChildren) instead of a path that only
# exists on the machine that built it.
#
# macOS-only: ELF (Linux) already resolves the soname through the rpath, so no
# load-command surgery is needed there.
#
# Invoked via include() from topo_relocate_llvm_rpath()'s install(CODE); the
# caller sets these install-time variables first:
#   TOPO_RELOC_BINARY  — absolute path to the installed binary
#   TOPO_RELOC_PATTERN — dylib basename stem to rewrite (libLLVM | libclang)

if(NOT APPLE)
    return()
endif()
if(NOT EXISTS "${TOPO_RELOC_BINARY}")
    # WARNING (not STATUS): a missing target here means the install path was
    # mis-derived (e.g. a DESTDIR mismatch) and the shipped binary keeps its
    # build-host-absolute load command — make that loud, not silent.
    message(WARNING "relocate-rpath: ${TOPO_RELOC_BINARY} absent — load-command "
        "rewrite SKIPPED; the installed binary may not be relocatable")
    return()
endif()

execute_process(
    COMMAND otool -L "${TOPO_RELOC_BINARY}"
    OUTPUT_VARIABLE _otool_out
    RESULT_VARIABLE _otool_rc)
if(NOT _otool_rc EQUAL 0)
    message(WARNING "relocate-rpath: otool -L failed on ${TOPO_RELOC_BINARY}")
    return()
endif()

string(REPLACE "\n" ";" _otool_lines "${_otool_out}")
set(_relocated 0)
foreach(_line IN LISTS _otool_lines)
    string(STRIP "${_line}" _line)
    # otool -L dependency line, post-strip:
    #   /abs/path/libLLVM.dylib (compatibility version ..., current version ...)
    # Match only an ABSOLUTE path ending in <pattern>*.dylib (already-@rpath
    # deps start with '@' and are skipped).
    if(_line MATCHES "^(/[^ ]+/(${TOPO_RELOC_PATTERN}[^ /]*\\.dylib))[ \t]")
        set(_old "${CMAKE_MATCH_1}")
        set(_base "${CMAKE_MATCH_2}")
        execute_process(
            COMMAND install_name_tool -change "${_old}" "@rpath/${_base}" "${TOPO_RELOC_BINARY}"
            RESULT_VARIABLE _int_rc)
        if(_int_rc EQUAL 0)
            message(STATUS "relocate-rpath: ${TOPO_RELOC_BINARY}: ${_old} -> @rpath/${_base}")
            math(EXPR _relocated "${_relocated} + 1")
        else()
            message(WARNING "relocate-rpath: install_name_tool -change failed for ${_old}")
        endif()
    endif()
endforeach()

if(_relocated EQUAL 0)
    message(STATUS "relocate-rpath: no absolute ${TOPO_RELOC_PATTERN}*.dylib "
        "load command in ${TOPO_RELOC_BINARY} (already @rpath, or statically linked)")
endif()
