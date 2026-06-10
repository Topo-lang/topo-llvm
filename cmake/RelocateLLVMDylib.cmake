# RelocateLLVMDylib.cmake — install-time step that makes an installed Mach-O
# artifact (a tool in bin/ or a shared library in lib/) relocatable.
#
# Step 1 — load-command rewrite: the artifact's build-host-absolute libLLVM /
# libclang / liblldb load command (e.g. /opt/homebrew/opt/llvm/lib/libLLVM.dylib)
# becomes @rpath/<basename>, so a relocated copy loads the dylib through its
# @loader_path-relative rpath (a bundled copy: ../lib for a bin/ tool, the
# artifact's own directory for a lib/ dylib) or the BYO loader-path injection
# (topo-core ensureLLVMLoaderPathForChildren) instead of a path that only
# exists on the machine that built it.
#
# Step 2 — Cellar rpath canonicalization: CMAKE_INSTALL_RPATH_USE_LINK_PATH
# appends the build host's LLVM lib dir as a dev fallback. With a Homebrew
# LLVM that dir is the VERSIONED Cellar keg (…/Cellar/llvm/22.1.7/lib), which
# bakes an exact-patch string into the shipped artifact and dangles on the
# build host after every `brew upgrade llvm` patch bump. Rewrite it to the
# stable unversioned opt symlink (…/opt/llvm/lib): identical resolution on the
# build host, no exact-version residue in the artifact. @loader_path entries
# are untouched and keep their position (first). Idempotent — an artifact with
# no versioned Cellar LLVM rpath is left unchanged, so non-Homebrew layouts
# are a no-op.
#
# macOS-only: ELF (Linux) already resolves the soname through the rpath, so no
# load-command surgery is needed there.
#
# Invoked via include() from topo_relocate_llvm_rpath()'s install(CODE); the
# caller sets these install-time variables first:
#   TOPO_RELOC_BINARY  — absolute path to the installed Mach-O artifact
#   TOPO_RELOC_PATTERN — dylib basename stem to rewrite (libLLVM | libclang | liblldb)

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
    # deps start with '@' and are skipped; a dylib's own LC_ID_DYLIB line only
    # matches if it itself is a <pattern> dylib, which ours never are).
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

# ── Step 2: versioned Cellar LC_RPATH → stable unversioned opt path ──
execute_process(
    COMMAND otool -l "${TOPO_RELOC_BINARY}"
    OUTPUT_VARIABLE _lc_out
    RESULT_VARIABLE _lc_rc)
if(NOT _lc_rc EQUAL 0)
    message(WARNING "relocate-rpath: otool -l failed on ${TOPO_RELOC_BINARY} — "
        "Cellar rpath canonicalization SKIPPED")
    return()
endif()

# Collect the LC_RPATH entries in load-command order. Each block prints as:
#       cmd LC_RPATH
#   cmdsize NN
#      path <value> (offset NN)
set(_rpaths "")
set(_in_rpath 0)
string(REPLACE "\n" ";" _lc_lines "${_lc_out}")
foreach(_line IN LISTS _lc_lines)
    if(_line MATCHES "cmd LC_RPATH")
        set(_in_rpath 1)
    elseif(_in_rpath AND _line MATCHES "^[ \t]+path (.*) \\(offset ")
        list(APPEND _rpaths "${CMAKE_MATCH_1}")
        set(_in_rpath 0)
    endif()
endforeach()

foreach(_rp IN LISTS _rpaths)
    # <brew-prefix>/Cellar/<formula>/<version>/<sub> → <brew-prefix>/opt/<formula>/<sub>
    # (Homebrew guarantees opt/<formula> symlinks to the active keg.)
    # Restricted to LLVM-family kegs (llvm, llvm@NN) — the only dev fallback
    # this helper owns; other rpaths are not its business.
    if(_rp MATCHES "^(.+)/Cellar/(llvm[^/]*)/[^/]+/(.+)$")
        set(_stable "${CMAKE_MATCH_1}/opt/${CMAKE_MATCH_2}/${CMAKE_MATCH_3}")
        if("${_stable}" IN_LIST _rpaths)
            # The stable form is already an rpath — just drop the versioned one
            # (install_name_tool -rpath would reject the duplicate).
            execute_process(
                COMMAND install_name_tool -delete_rpath "${_rp}" "${TOPO_RELOC_BINARY}"
                RESULT_VARIABLE _int_rc)
        else()
            # In-place replacement preserves LC_RPATH ORDER, so the relocation
            # rpath (@loader_path…) added by topo_relocate_llvm_rpath stays first.
            execute_process(
                COMMAND install_name_tool -rpath "${_rp}" "${_stable}" "${TOPO_RELOC_BINARY}"
                RESULT_VARIABLE _int_rc)
            list(APPEND _rpaths "${_stable}")
        endif()
        if(_int_rc EQUAL 0)
            message(STATUS "relocate-rpath: ${TOPO_RELOC_BINARY}: rpath ${_rp} -> ${_stable}")
        else()
            message(WARNING "relocate-rpath: install_name_tool rpath edit failed for ${_rp}")
        endif()
    endif()
endforeach()
