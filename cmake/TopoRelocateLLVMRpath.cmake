# TopoRelocateLLVMRpath.cmake — topo_relocate_llvm_rpath(), shared by every
# repo that installs LLVM-linked artifacts (topo-llvm, topo-lang-cpp,
# topo-lang-rust). Synced byte-identical from the canonical copy by
# scripts/sync-cmake-helpers.sh — edit the canonical, never a package copy.
# Expects GNUInstallDirs (CMAKE_INSTALL_BINDIR / CMAKE_INSTALL_LIBDIR) in the
# including scope.

# Directory of this module at include time — used to locate the sibling
# RelocateLLVMDylib.cmake (synced next to this file in each repo's cmake/)
# from inside install(CODE) at install time. MUST stay outside the function:
# captured per-repo when each repo's CompilerFlags module includes this file.
set(_TOPO_RELOCATE_HELPER_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Make an installed LLVM-linked Mach-O/ELF artifact relocatable. Usage:
#   topo_relocate_llvm_rpath(<target> <pattern> [DESTINATION <install-subdir>])
# `pattern` is the dylib basename stem to rewrite (libLLVM | libclang | liblldb).
# DESTINATION is the artifact's install subdir as given to install(TARGETS);
# default ${CMAKE_INSTALL_BINDIR} (executables). Pass the LIBDIR for an
# installed shared library (e.g. libtopo-jit-engine.dylib) — a lib/ artifact
# expects bundled dylibs NEXT to itself (rpath @loader_path / $ORIGIN) instead
# of in the sibling lib dir (@loader_path/../lib / $ORIGIN/../lib).
# macOS: prepends the relocation rpath (kept FIRST, ahead of any pre-set dev
# fallback and the build-host LLVM lib dir appended by
# CMAKE_INSTALL_RPATH_USE_LINK_PATH), rewrites the build-host-absolute
# lib<pattern> load command to @rpath/<basename> at install time, and
# canonicalizes a versioned Homebrew Cellar LLVM rpath to its stable /opt
# form (see cmake/RelocateLLVMDylib.cmake).
# Linux (ELF): prepends the $ORIGIN-relative runpath so a bundled libLLVM.so
# resolves relative to the artifact — no load-command rewrite needed, the
# dynamic linker resolves the DT_NEEDED soname through the runpath.
# No-op on Windows (DLL search has no rpath; PATH / app-dir rules apply).
# Call this AFTER the target's install(TARGETS) so the macOS rewrite runs on
# the installed copy.
function(topo_relocate_llvm_rpath target pattern)
    if(WIN32)
        return()
    endif()
    cmake_parse_arguments(_reloc "" "DESTINATION" "" ${ARGN})
    set(_dest "${_reloc_DESTINATION}")
    if(NOT _dest)
        set(_dest "${CMAKE_INSTALL_BINDIR}")
        if(NOT _dest)
            set(_dest "bin")
        endif()
    endif()
    # Artifacts installed into the lib dir find bundled dylibs NEXT to
    # themselves; anything else (bin/ tools) finds them in the sibling ../lib.
    if(_dest STREQUAL "${CMAKE_INSTALL_LIBDIR}" OR _dest STREQUAL "lib")
        set(_rpath_apple "@loader_path")
        set(_rpath_elf "\$ORIGIN")
    else()
        set(_rpath_apple "@loader_path/../lib")
        set(_rpath_elf "\$ORIGIN/../lib")
    endif()
    if(APPLE)
        set(_rpath_reloc "${_rpath_apple}")
    else()
        set(_rpath_reloc "${_rpath_elf}")
    endif()
    # PREPEND the relocation rpath so it stays first even when the target
    # already declares a build-host dev fallback in INSTALL_RPATH.
    get_target_property(_rpath_prev ${target} INSTALL_RPATH)
    if(_rpath_prev)
        set_target_properties(${target} PROPERTIES
            INSTALL_RPATH "${_rpath_reloc};${_rpath_prev}")
    else()
        set_target_properties(${target} PROPERTIES INSTALL_RPATH "${_rpath_reloc}")
    endif()
    if(NOT APPLE)
        return()
    endif()
    # Resolve the installed path the SAME way CMake's own install(TARGETS) does:
    # $ENV{DESTDIR} + (absolute DESTINATION as-is | CMAKE_INSTALL_PREFIX-joined),
    # both deferred to install time. Without the DESTDIR prefix a staged install
    # (DESTDIR=, distro/Homebrew packaging) writes to $DESTDIR$PREFIX/bin but this
    # script would look at $PREFIX/bin, miss the file, and silently skip the
    # rewrite — shipping a non-relocatable binary. $<TARGET_FILE_NAME:…> (not
    # the bare target name) so SHARED libraries resolve to lib<name>.dylib.
    if(IS_ABSOLUTE "${_dest}")
        set(_reloc_bin "\$ENV{DESTDIR}${_dest}/$<TARGET_FILE_NAME:${target}>")
    else()
        set(_reloc_bin "\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${_dest}/$<TARGET_FILE_NAME:${target}>")
    endif()
    install(CODE
"set(TOPO_RELOC_BINARY \"${_reloc_bin}\")
set(TOPO_RELOC_PATTERN \"${pattern}\")
include(\"${_TOPO_RELOCATE_HELPER_DIR}/RelocateLLVMDylib.cmake\")")
endfunction()
