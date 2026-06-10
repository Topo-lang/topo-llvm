# topo-llvm LLVM detection.
#
# Logical content of this file used to live inline in the monorepo's root
# CMakeLists.txt; relocated here so the same code drives both monorepo
# configuration (root `include()`s this file when TOPO_ENABLE_LLVM=ON) and
# the future split-mode where topo-llvm is its own top-level repo.
#
# Inputs (in order of precedence):
#   - user-set LLVM_DIR / Clang_DIR cache vars
#   - Homebrew llvm@<major> (preferred — ships shared libLLVM.dylib)
#   - bundled tarball under <topo-llvm-root>/llvm-dev/
#
# Outputs:
#   - LLVM_DIR cache var
#   - TOPO_LLVM_BINDIR cache var
#   - find_package(LLVM REQUIRED CONFIG) brings in LLVM_*, Clang_*

# TOPO_LLVM_SOURCE_DIR — directory containing .llvm-version + llvm-dev/.
# Defaults to ${CMAKE_CURRENT_LIST_DIR}/.. which is the topo-llvm root in
# both monorepo and split-mode.
if(NOT DEFINED TOPO_LLVM_SOURCE_DIR)
    get_filename_component(TOPO_LLVM_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

# ---------- LLVM dev libraries: prefer Homebrew (shared libLLVM.dylib) ----------
# Discovery order when LLVM_DIR is not user-supplied:
#   1. `brew --prefix llvm@22` (matches topo-llvm/.llvm-version major) — ships
#      libLLVM.dylib; relink time drops from minutes to ~1s and per-binary
#      size drops ~80% versus static archives.
#   2. ${TOPO_LLVM_SOURCE_DIR}/llvm-dev/ bundled tarball — static archives only.
# The bundled path remains the fallback for environments without brew
# (Windows scoop downloads need it; Linux without linuxbrew uses it).
# Override either source by passing -DLLVM_DIR=... -DClang_DIR=... .
if(NOT DEFINED LLVM_DIR AND NOT DEFINED CACHE{LLVM_DIR})
    # Read the pinned major version so the brew formula name tracks
    # .llvm-version automatically.
    set(_LLVM_MAJOR "")
    if(EXISTS "${TOPO_LLVM_SOURCE_DIR}/.llvm-version")
        file(STRINGS "${TOPO_LLVM_SOURCE_DIR}/.llvm-version" _PIN_VER)
        string(REGEX MATCH "^[0-9]+" _LLVM_MAJOR "${_PIN_VER}")
    endif()

    find_program(_TOPO_BREW brew)
    if(_TOPO_BREW AND _LLVM_MAJOR)
        execute_process(
            COMMAND ${_TOPO_BREW} --prefix llvm@${_LLVM_MAJOR}
            OUTPUT_VARIABLE _BREW_LLVM_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _BREW_LLVM_RC)
        if(_BREW_LLVM_RC EQUAL 0
                AND EXISTS "${_BREW_LLVM_PREFIX}/lib/cmake/llvm/LLVMConfig.cmake")
            set(LLVM_DIR "${_BREW_LLVM_PREFIX}/lib/cmake/llvm"
                CACHE PATH "Homebrew LLVM (shared libLLVM)")
        endif()
    endif()

    if(NOT DEFINED LLVM_DIR AND NOT DEFINED CACHE{LLVM_DIR})
        set(_BUNDLED "${TOPO_LLVM_SOURCE_DIR}/llvm-dev/lib/cmake/llvm")
        if(EXISTS "${_BUNDLED}/LLVMConfig.cmake")
            set(LLVM_DIR "${_BUNDLED}" CACHE PATH "Bundled LLVM (static archives)")
        else()
            message(FATAL_ERROR
                "No LLVM ${_LLVM_MAJOR} found.\n"
                "  • macOS/Linux: `brew install llvm@${_LLVM_MAJOR}` (recommended — shared libLLVM)\n"
                "  • or run 'bash ${TOPO_LLVM_SOURCE_DIR}/scripts/setup-llvm.sh' to fetch the bundled tarball.")
        endif()
    endif()
endif()

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

# Pinned LLVM major — seeds the runtime version gate in topo-core's
# Platform/ToolResolution.cpp (TOPO_LLVM_MAJOR compile-def) so a relocated
# binary rejects a wrong-major BYO LLVM. Sourced from the LLVM actually found
# (authoritative) rather than re-parsing .llvm-version.
set(TOPO_LLVM_MAJOR "${LLVM_VERSION_MAJOR}" CACHE STRING "Pinned LLVM major" FORCE)

# Workaround: the bundled LLVM tarball on GHA Linux references the
# non-PIC /usr/lib/x86_64-linux-gnu/libzstd.a, which cannot be linked
# into SHARED targets (e.g. libtopo-jit-engine.so → "recompile with
# -fPIC"). The reference comes via CMake imported targets — typical
# names are zstd::libzstd_static or libzstd_static — whose
# IMPORTED_LOCATION points at the .a. CMake resolves those at link
# time, so string-grep on INTERFACE_LINK_LIBRARIES misses it.
#
# Patch every plausible zstd-static imported target's IMPORTED_LOCATION
# to point at the corresponding .so on the runner. Ubuntu's
# libzstd-dev package ships both .a and .so; the .so is PIC.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_topo_zstd_patched 0)
    set(_topo_zstd_so "")

    # Resolve the system shared libzstd once (find_library prefers .so
    # when both are present on the search path; fallback to the
    # well-known Ubuntu path if find_library somehow returns nothing).
    find_library(_topo_zstd_so_found NAMES zstd PATHS /usr/lib/x86_64-linux-gnu)
    if(_topo_zstd_so_found)
        set(_topo_zstd_so "${_topo_zstd_so_found}")
    elseif(EXISTS "/usr/lib/x86_64-linux-gnu/libzstd.so")
        set(_topo_zstd_so "/usr/lib/x86_64-linux-gnu/libzstd.so")
    endif()

    foreach(_tgt zstd::libzstd_static libzstd_static zstd::libzstd_shared libzstd zstd::zstd)
        if(TARGET ${_tgt})
            get_target_property(_loc ${_tgt} IMPORTED_LOCATION)
            if(NOT _loc)
                # Try config-specific variants.
                foreach(_cfg RELEASE DEBUG NOCONFIG RELWITHDEBINFO MINSIZEREL)
                    get_target_property(_loc ${_tgt} IMPORTED_LOCATION_${_cfg})
                    if(_loc)
                        break()
                    endif()
                endforeach()
            endif()
            if(_loc MATCHES "libzstd\\.a$" AND _topo_zstd_so)
                set_target_properties(${_tgt} PROPERTIES
                    IMPORTED_LOCATION "${_topo_zstd_so}")
                # Wipe config-specific overrides so the unconfigured
                # IMPORTED_LOCATION wins.
                foreach(_cfg RELEASE DEBUG NOCONFIG RELWITHDEBINFO MINSIZEREL)
                    set_target_properties(${_tgt} PROPERTIES
                        IMPORTED_LOCATION_${_cfg} "${_topo_zstd_so}")
                endforeach()
                math(EXPR _topo_zstd_patched "${_topo_zstd_patched} + 1")
                message(STATUS "topo-llvm: redirected ${_tgt} IMPORTED_LOCATION ${_loc} → ${_topo_zstd_so}")
            elseif(_loc)
                message(STATUS "topo-llvm: ${_tgt} IMPORTED_LOCATION = ${_loc} (not patched)")
            endif()
        endif()
    endforeach()

    if(_topo_zstd_patched EQUAL 0 AND TOPO_LLVM_BUILD_JIT_ENGINE)
        message(WARNING "topo-llvm: no zstd-static imported target found "
            "to patch. The SHARED jit-engine link will likely fail with "
            "a non-PIC libzstd.a complaint. Found zstd.so: ${_topo_zstd_so}. "
            "Dump of plausible zstd targets follows:")
        foreach(_tgt zstd::libzstd_static libzstd_static zstd::libzstd_shared
                     libzstd zstd::zstd ZSTD::ZSTD)
            if(TARGET ${_tgt})
                get_target_property(_loc ${_tgt} IMPORTED_LOCATION)
                message(STATUS "  target ${_tgt}: IMPORTED_LOCATION=${_loc}")
            else()
                message(STATUS "  target ${_tgt}: does not exist")
            endif()
        endforeach()
    else()
        message(STATUS "topo-llvm: zstd-static → shared redirection patched ${_topo_zstd_patched} target(s)")
    endif()
endif()

# Report link mode — set by the distribution's LLVMConfig.cmake. Homebrew
# builds with LLVM_BUILD_LLVM_DYLIB=ON, bundled tarballs don't.
# See cmake/TopoCompilerFlags.cmake -> topo_llvm_libs() for the switch.
if(LLVM_LINK_LLVM_DYLIB)
    message(STATUS "LLVM link mode: shared libLLVM.dylib")
else()
    message(STATUS "LLVM link mode: static archives (no libLLVM.dylib in this distribution)")
endif()

# Export LLVM bin directory for runtime tool resolution.
# Heal a dangling cache entry first: a versioned Homebrew Cellar path cached
# before a `brew upgrade llvm` patch bump no longer exists — re-derive instead
# of failing later at build/test time on the vanished keg.
if(DEFINED CACHE{TOPO_LLVM_BINDIR} AND NOT EXISTS "${TOPO_LLVM_BINDIR}")
    message(STATUS "topo-llvm: cached TOPO_LLVM_BINDIR dangles (${TOPO_LLVM_BINDIR}) — re-deriving")
    unset(TOPO_LLVM_BINDIR CACHE)
endif()
# Canonicalize a versioned Homebrew Cellar keg to its stable opt symlink
# (…/Cellar/llvm/22.1.7/bin → …/opt/llvm/bin): the value is baked into tools
# as the compile-time dev fallback (TOPO_LLVM_BINDIR define) and cached here,
# so the unversioned form keeps both valid across LLVM patch bumps and keeps
# exact-version Cellar strings out of shipped binaries.
set(_TOPO_LLVM_BINDIR_DETECTED "${LLVM_TOOLS_BINARY_DIR}")
if(_TOPO_LLVM_BINDIR_DETECTED MATCHES "^(.+)/Cellar/(llvm[^/]*)/[^/]+/(.+)$")
    set(_TOPO_LLVM_BINDIR_STABLE "${CMAKE_MATCH_1}/opt/${CMAKE_MATCH_2}/${CMAKE_MATCH_3}")
    if(EXISTS "${_TOPO_LLVM_BINDIR_STABLE}")
        set(_TOPO_LLVM_BINDIR_DETECTED "${_TOPO_LLVM_BINDIR_STABLE}")
    endif()
endif()
set(TOPO_LLVM_BINDIR "${_TOPO_LLVM_BINDIR_DETECTED}" CACHE PATH "LLVM bin dir")

# Version check against .llvm-version
if(EXISTS "${TOPO_LLVM_SOURCE_DIR}/.llvm-version")
    file(STRINGS "${TOPO_LLVM_SOURCE_DIR}/.llvm-version" _EXPECTED_VER)
    if(NOT LLVM_PACKAGE_VERSION VERSION_EQUAL _EXPECTED_VER)
        message(WARNING ".llvm-version=${_EXPECTED_VER} but found LLVM ${LLVM_PACKAGE_VERSION}")
    endif()
endif()
