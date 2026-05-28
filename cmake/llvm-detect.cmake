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

# Workaround: the bundled LLVM tarball used by topo-llvm CI bakes an
# absolute path to /usr/lib/.../libzstd.a (non-PIC) into LLVMSupport's
# INTERFACE_LINK_LIBRARIES on Linux. That static archive cannot be linked
# into SHARED targets (e.g. libtopo-jit-engine.so), so the linker rejects
# the SHARED build with "recompile with -fPIC". Substitute the .a path
# with a shared-libzstd reference (`zstd`) when running on Linux against
# a distribution that exhibits this.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET LLVMSupport)
    get_target_property(_llvm_support_iface LLVMSupport INTERFACE_LINK_LIBRARIES)
    if(_llvm_support_iface MATCHES "libzstd\\.a")
        message(STATUS "topo-llvm: substituting non-PIC libzstd.a → -lzstd in LLVMSupport interface")
        string(REGEX REPLACE "[^;]*libzstd\\.a" "zstd" _llvm_support_iface "${_llvm_support_iface}")
        set_target_properties(LLVMSupport PROPERTIES INTERFACE_LINK_LIBRARIES "${_llvm_support_iface}")
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

# Export LLVM bin directory for runtime tool resolution
set(TOPO_LLVM_BINDIR "${LLVM_TOOLS_BINARY_DIR}" CACHE PATH "LLVM bin dir")

# Version check against .llvm-version
if(EXISTS "${TOPO_LLVM_SOURCE_DIR}/.llvm-version")
    file(STRINGS "${TOPO_LLVM_SOURCE_DIR}/.llvm-version" _EXPECTED_VER)
    if(NOT LLVM_PACKAGE_VERSION VERSION_EQUAL _EXPECTED_VER)
        message(WARNING ".llvm-version=${_EXPECTED_VER} but found LLVM ${LLVM_PACKAGE_VERSION}")
    endif()
endif()
