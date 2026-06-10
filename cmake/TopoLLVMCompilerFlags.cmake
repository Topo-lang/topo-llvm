# TopoLLVMCompilerFlags.cmake — standalone compiler-flag helpers for
# topo-llvm. Mirrors the monorepo cmake/TopoCompilerFlags.cmake but
# scoped to the LLVM-touching layers.

# Directory of this module — used to locate sibling helper scripts
# (RelocateLLVMDylib.cmake) from inside install(CODE) at install time.
set(_TOPO_LLVM_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# RPATH configuration for Unix shared library builds.
if(NOT WIN32)
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
    if(APPLE)
        set(CMAKE_MACOSX_RPATH ON)
    endif()
endif()

# When using bundled LLVM clang++, use system SDK's libc++ headers to
# avoid ABI mismatch (bundled libc++ ahead of system libc++.dylib).
if(APPLE AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../llvm-dev/lib/libc++.dylib")
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE _TOPO_SDK_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_TOPO_SDK_PATH AND EXISTS "${_TOPO_SDK_PATH}/usr/include/c++/v1")
        add_compile_options(
            "-nostdinc++"
            "-isystem" "${_TOPO_SDK_PATH}/usr/include/c++/v1"
        )
    endif()
endif()

# ── Sanitizer support ────────────────────────────────────
set(TOPO_LLVM_SANITIZER "" CACHE STRING
    "Enable sanitizers (address, undefined, thread, memory)")

function(topo_llvm_apply_sanitizer target)
    if(NOT TOPO_LLVM_SANITIZER)
        return()
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target}
            PRIVATE -fsanitize=${TOPO_LLVM_SANITIZER} -fno-omit-frame-pointer)
        target_link_options(${target}
            PRIVATE -fsanitize=${TOPO_LLVM_SANITIZER})
    endif()
endfunction()

# Compiler flag base — used by zero-LLVM C-ABI runtime libs and helper
# E2E tests.
function(topo_set_compiler_flags target)
    target_compile_features(${target} PUBLIC cxx_std_17)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    endif()
    topo_llvm_apply_sanitizer(${target})
endfunction()

# Compiler flags for LLVM-linked targets: matches LLVM's RTTI flag and
# adds the LLVM headers as SYSTEM includes (suppresses upstream warnings).
function(topo_set_llvm_flags target)
    topo_set_compiler_flags(${target})

    # LLVM is typically built with -fno-rtti; linked code must match.
    if(DEFINED LLVM_ENABLE_RTTI AND NOT LLVM_ENABLE_RTTI)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${target} PRIVATE -fno-rtti)
        elseif(MSVC)
            target_compile_options(${target} PRIVATE /GR-)
        endif()
    endif()

    if(DEFINED LLVM_INCLUDE_DIRS)
        target_include_directories(${target} SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    endif()
    if(DEFINED LLVM_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE ${LLVM_DEFINITIONS})
    endif()
endfunction()

# Make an installed LLVM-linked tool relocatable. macOS: adds an
# @loader_path/../lib rpath (so a dylib bundled next to the tool resolves) and
# rewrites the build-host-absolute libLLVM/libclang load command to
# @rpath/<basename> at install time (see cmake/RelocateLLVMDylib.cmake).
# `pattern` is the dylib basename stem to rewrite (libLLVM | libclang).
# Linux (ELF): appends $ORIGIN/../lib to the installed RUNPATH so a bundled
# libLLVM.so resolves relative to the tool — no load-command rewrite needed,
# the dynamic linker resolves the DT_NEEDED soname through the runpath.
# (CMAKE_INSTALL_RPATH_USE_LINK_PATH still appends the build-host LLVM lib
# dir afterwards as a dev fallback on both platforms.) No-op on Windows (DLL
# search has no rpath; PATH / app-dir rules apply). Call this AFTER the
# target's install(TARGETS) so the macOS rewrite runs on the installed copy.
function(topo_relocate_llvm_rpath target pattern)
    if(WIN32)
        return()
    endif()
    if(NOT APPLE)
        set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "\$ORIGIN/../lib")
        return()
    endif()
    # @loader_path/../lib first (relocation-safe); CMAKE_INSTALL_RPATH_USE_LINK_PATH
    # still appends the build-host LLVM lib dir afterwards as a dev fallback.
    set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "@loader_path/../lib")
    set(_bindir "${CMAKE_INSTALL_BINDIR}")
    if(NOT _bindir)
        set(_bindir "bin")
    endif()
    # Resolve the installed path the SAME way CMake's own install(TARGETS) does:
    # $ENV{DESTDIR} + (absolute DESTINATION as-is | CMAKE_INSTALL_PREFIX-joined),
    # both deferred to install time. Without the DESTDIR prefix a staged install
    # (DESTDIR=, distro/Homebrew packaging) writes to $DESTDIR$PREFIX/bin but this
    # script would look at $PREFIX/bin, miss the file, and silently skip the
    # rewrite — shipping a non-relocatable binary.
    if(IS_ABSOLUTE "${_bindir}")
        set(_reloc_bin "\$ENV{DESTDIR}${_bindir}/${target}")
    else()
        set(_reloc_bin "\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${_bindir}/${target}")
    endif()
    install(CODE
"set(TOPO_RELOC_BINARY \"${_reloc_bin}\")
set(TOPO_RELOC_PATTERN \"${pattern}\")
include(\"${_TOPO_LLVM_CMAKE_DIR}/RelocateLLVMDylib.cmake\")")
endfunction()

# PCH helpers — no-ops in standalone (no project-wide PCH host).
# Guarded so meta-repo embedding (which defines real PCH-applying versions
# before adding topo-llvm as a subdirectory) wins; standalone configure
# falls through and gets the no-op stubs below.
if(NOT COMMAND topo_apply_std_pch)
    function(topo_apply_std_pch target)
    endfunction()
endif()
if(NOT COMMAND topo_apply_std_pch_llvm)
    function(topo_apply_std_pch_llvm target)
    endfunction()
endif()

# Resolve LLVM component names to a link-library list. When LLVM was
# built with LLVM_BUILD_LLVM_DYLIB=ON (Homebrew, shared libLLVM.dylib),
# link the single shared lib instead of dozens of static archives.
function(topo_llvm_libs out_var)
    if(LLVM_LINK_LLVM_DYLIB AND TARGET LLVM)
        set(${out_var} LLVM PARENT_SCOPE)
    else()
        llvm_map_components_to_libnames(_libs ${ARGN})
        set(${out_var} ${_libs} PARENT_SCOPE)
    endif()
endfunction()
